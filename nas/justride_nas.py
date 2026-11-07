#!/usr/bin/env python3
"""JustRide on a NAS: the web app with the head unit's data, also while the head unit is off.

  nas/justride_nas.py --device 192.168.1.50 --www ./www/stats --data ./data

Serves the web app (/stats/, the same files as on the SD card) and the device's HTTP API
from a mirror of the device data. A sync job (every --interval seconds, and before a read
when the last try is older than 15 s) pulls new rides, riders, workouts and plans while the
head unit answers, and pushes the writes made while it didn't. Python standard library only.

Who owns what:
  rides, summary.csv, bests.csv   the device; the NAS copies them (and drops what the device dropped)
  riders (FTP, weight, Max ERG)   the device; edits go straight to it and are refused while it's off
  workouts and plans              written through to the device when it answers, else queued here;
                                  a queued write wins over a copy changed on the device meanwhile,
                                  which is kept under <data>/conflicts/
  firmware and web-app uploads    only on the device itself (they need the tap there)

The device's own rules stay in force: the NAS sends Host = the device address and
X-JustRide on every write, and the browser only ever talks to the NAS (same origin).
This service applies the same rules to its own clients: Host must be an IPv4 address or
one of --host, and every POST/DELETE needs X-JustRide.
"""
import argparse
import hashlib
import http.server
import ipaddress
import json
import os
import re
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request

MIME = {'.html': 'text/html', '.js': 'application/javascript', '.mjs': 'application/javascript', '.css': 'text/css',
        '.wasm': 'application/wasm', '.json': 'application/json', '.csv': 'text/csv', '.zwo': 'application/xml',
        '.route': 'text/plain', '.txt': 'text/plain', '.png': 'image/png', '.svg': 'image/svg+xml',
        '.webmanifest': 'application/manifest+json', '.fit': 'application/octet-stream'}
# The device's limits (components/web/web.c)
ZWO_MAX, ROUTE_MAX, PLAN_MAX = 64 * 1024, 256 * 1024, 32 * 1024
FRESH_S = 15  # a read syncs first when the last try is older than this
ERRORS_KEPT = 10


def sha(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def workout_name_ok(name):
    """The device's rule: letters, digits, '._- ', no '..', .zwo or .route, <= 60 characters."""
    return (bool(re.fullmatch(r'[A-Za-z0-9._ -]{5,60}', name)) and '..' not in name and not name.startswith('.')
            and name.lower().endswith(('.zwo', '.route')))


def plan_name_ok(name):
    return bool(re.fullmatch(r'[A-Za-z0-9]{1,8}\.json', name))


def ride_name_ok(name):
    """<user id>/<file> as /api/rides lists them."""
    return bool(re.fullmatch(r'[A-Za-z0-9]{1,8}/[A-Za-z0-9._-]{1,64}', name)) and '..' not in name


class DeviceError(Exception):
    """The head unit didn't answer (network), as opposed to answering with an error status."""


class Device:
    def __init__(self, addr, timeout=30):
        self.addr, self.timeout = addr, timeout

    def request(self, method, path, body=None, timeout=None):
        """(status, body). Raises DeviceError if the device can't be reached."""
        p, q = (path.split('?', 1) + [''])[:2]
        url = f'http://{self.addr}{urllib.parse.quote(p)}' + (f'?{q}' if q else '')
        req = urllib.request.Request(url, data=body, method=method,
                                     headers={'Host': self.addr, 'X-JustRide': '1'})
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.read()
        except (OSError, ValueError) as e:  # URLError, timeouts, refused, reset
            raise DeviceError(str(e)) from e


class Store:
    """<data>/mirror: the device's files by API path (users.json, info.json, rides/, workouts/, plans/);
    <data>/outbox: writes waiting for the device; <data>/state.json: sync bookkeeping;
    <data>/conflicts: device copies a queued write replaced."""

    def __init__(self, data):
        self.data = data
        self.mirror = os.path.join(data, 'mirror')
        self.outbox_dir = os.path.join(data, 'outbox')
        for d in (self.mirror, self.outbox_dir, os.path.join(data, 'conflicts')):
            os.makedirs(d, exist_ok=True)
        self.state = self._load('state.json', {})
        self.state.setdefault('base', {})    # rel -> hash of the device's copy at the last sync
        self.state.setdefault('meta', {})    # rel -> [size, mtime] as the device listed it
        self.state.setdefault('errors', [])  # writes the device refused
        self.outbox = self._load(os.path.join('outbox', 'index.json'), {})  # rel -> {op, base, body, time}

    def _load(self, rel, default):
        try:
            with open(os.path.join(self.data, rel)) as f:
                return json.load(f)
        except (OSError, ValueError):
            return default

    def _atomic(self, path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix='.tmp')
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        os.replace(tmp, path)

    def save(self):
        self._atomic(os.path.join(self.data, 'state.json'), json.dumps(self.state, indent=1).encode())
        self._atomic(os.path.join(self.outbox_dir, 'index.json'), json.dumps(self.outbox, indent=1).encode())

    def path(self, rel):
        p = os.path.normpath(os.path.join(self.mirror, rel))
        if not p.startswith(self.mirror + os.sep):
            raise ValueError(rel)
        return p

    def read(self, rel):
        try:
            with open(self.path(rel), 'rb') as f:
                return f.read()
        except OSError:
            return None

    def write(self, rel, data):
        self._atomic(self.path(rel), data)

    def delete(self, rel):
        try:
            os.remove(self.path(rel))
        except FileNotFoundError:
            pass

    def files(self, sub):
        """Files under mirror/<sub>, as rel paths."""
        root = self.path(sub)
        out = []
        for dirpath, _, names in os.walk(root):
            out += [os.path.relpath(os.path.join(dirpath, n), self.mirror) for n in names if not n.startswith('.')]
        return sorted(out)

    def queue(self, op, rel, body):
        """Remember a write for the device. A later write to the same file replaces an earlier one
        but keeps its base: the conflict check compares with what the device had before both."""
        old = self.outbox.get(rel)
        body_file = None
        if body is not None:
            body_file = sha(rel.encode())[:16] + '.body'
            self._atomic(os.path.join(self.outbox_dir, body_file), body)
        self.state['seq'] = self.state.get('seq', 0) + 1
        self.outbox[rel] = {'op': op, 'base': old['base'] if old else self.state['base'].get(rel), 'body': body_file,
                            'time': int(time.time()), 'seq': self.state['seq']}

    def queued_body(self, rel):
        f = self.outbox[rel]['body']
        with open(os.path.join(self.outbox_dir, f), 'rb') as fh:
            return fh.read()

    def unqueue(self, rel):
        op = self.outbox.pop(rel, None)
        if op and op['body']:
            try:
                os.remove(os.path.join(self.outbox_dir, op['body']))
            except FileNotFoundError:
                pass

    def conflict(self, rel, data):
        name = time.strftime('%Y-%m-%d_%H%M%S ') + rel.replace('/', '_')
        self._atomic(os.path.join(self.data, 'conflicts', name), data)

    def error(self, rel, status, msg):
        self.state['errors'] = (self.state['errors'] + [{'time': int(time.time()), 'path': rel, 'status': status,
                                                         'message': msg[:200]}])[-ERRORS_KEPT:]


class Sync:
    def __init__(self, store, device):
        self.store, self.dev = store, device
        self.lock = threading.RLock()  # mirror, outbox and state changes (short; never held for a sync)
        self.sync_lock = threading.Lock()  # one sync at a time
        self.wake = threading.Event()
        st = store.state
        st.setdefault('online', False)
        st.setdefault('last_sync', 0)
        st['last_try'] = 0

    @property
    def online(self):
        return self.store.state['online']

    def offline(self, why):
        st = self.store.state
        st['online'] = False
        st['last_error'] = why

    def status(self):
        st = self.store.state
        return {'online': st['online'], 'device': self.dev.addr, 'last_sync': st['last_sync'],
                'pending': len(self.store.outbox), 'errors': st['errors'], 'last_error': st.get('last_error')}

    # ---- one round: push the outbox, then pull --------------------------------------------

    def run_once(self, wait=True):
        """One round. wait=False: skip it if a sync is already running (reads use the mirror)."""
        if not self.sync_lock.acquire(blocking=wait):
            return
        try:
            st = self.store.state
            st['last_try'] = time.time()
            try:
                status, body = self.dev.request('GET', '/api/info', timeout=3)
                if status != 200:
                    raise DeviceError(f'/api/info: {status}')
                with self.lock:
                    self.store.write('info.json', body)
                    st['online'] = True
                self._push()
                self._pull()
                with self.lock:
                    st['last_sync'] = int(time.time())
                    st.pop('last_error', None)
            except DeviceError as e:
                with self.lock:
                    self.offline(str(e))
            except Exception as e:  # a bug or a full/read-only /data: keep the loop alive, retry next round
                traceback.print_exc()
                with self.lock:
                    st['last_error'] = f'{type(e).__name__}: {e}'
            finally:
                with self.lock:
                    self.store.save()
        finally:
            self.sync_lock.release()

    def fresh(self, wait=False):
        """Before a read: sync if the last try is old (offline: at most one try per FRESH_S)."""
        if time.time() - self.store.state['last_try'] > FRESH_S:
            self.run_once(wait)

    def loop(self, interval):
        while True:
            self.run_once()
            self.wake.wait(interval)
            self.wake.clear()

    def _push(self):
        s = self.store
        with self.lock:
            todo = sorted(s.outbox)
        for rel in todo:
            with self.lock:
                op = dict(s.outbox.get(rel) or {})
                body = s.queued_body(rel) if op.get('op') == 'put' else None
            if not op:
                continue
            url = '/' + rel
            status, cur = self.dev.request('GET', url)
            cur = cur if status == 200 else None
            if op['op'] == 'put':
                status, msg = self.dev.request('POST', url, body)
            else:
                status, msg = self.dev.request('DELETE', url)
                status = 200 if status == 404 else status
            with self.lock:
                self._pushed(rel, op, body, cur, status, msg)

    def _pushed(self, rel, op, body, cur, status, msg):
        s = self.store
        if s.outbox.get(rel, {}).get('seq') != op['seq']:
            return  # written again meanwhile: the newer write goes next round
        if 500 <= status:
            raise DeviceError(f'/{rel}: {status}')  # try again next round
        if cur is not None and sha(cur) not in (op['base'], sha(body)):
            s.conflict(rel, cur)  # changed on the device too: ours wins, theirs is kept
        if 200 <= status < 300:
            if op['op'] == 'put':
                s.state['base'][rel] = sha(body)
            else:
                s.state['base'].pop(rel, None)
            s.state['meta'].pop(rel, None)  # the next listing has the device's size and time
        else:  # refused (bad workout, too large): the pull restores the device's copy
            s.error(rel, status, msg.decode('utf-8', 'replace'))
            s.state['meta'].pop(rel, None)
            s.state['base'].pop(rel, None)
            if op['op'] == 'put':
                s.delete(rel)
        s.unqueue(rel)

    def _pull(self):
        s = self.store
        status, body = self.dev.request('GET', '/api/users')
        if status != 200:
            raise DeviceError(f'/api/users: {status}')
        users = json.loads(body)
        with self.lock:
            s.write('users.json', body)

        # Workouts and plans first: small, and what the builder and the plan need; rides can take a while
        status, body = self.dev.request('GET', '/api/workouts')
        if status != 200:
            raise DeviceError(f'/api/workouts: {status}')
        listed = {'workouts/' + f['name']: [f['size'], f.get('mtime', 0)] for f in json.loads(body)
                  if workout_name_ok(f['name'])}
        self._mirror_listing('workouts', listed, track_base=True)

        # Plans: one per rider, no listing
        for u in users:
            rel = f"plans/{u['id']}.json"
            if rel in s.outbox or not plan_name_ok(rel[6:]):
                continue
            status, body = self.dev.request('GET', '/' + rel)
            with self.lock:
                if rel in s.outbox:
                    continue  # written meanwhile
                if status == 200:
                    if sha(body) != sha(s.read(rel)):
                        s.write(rel, body)
                    s.state['base'][rel] = sha(body)
                elif status == 404:
                    s.delete(rel)
                    s.state['base'].pop(rel, None)

        # Rides: the device's listing is the truth (a re-dated ride changes its file names)
        status, body = self.dev.request('GET', '/api/rides')
        if status != 200:
            raise DeviceError(f'/api/rides: {status}')
        listed = {}
        for f in json.loads(body):
            if ride_name_ok(f['name']):
                listed['rides/' + f['name']] = [f['size'], f.get('mtime', 0)]
        self._mirror_listing('rides', listed)

    def _mirror_listing(self, sub, listed, track_base=False):
        s = self.store
        for rel, meta in listed.items():
            with self.lock:
                if rel in s.outbox or (s.state['meta'].get(rel) == meta and s.read(rel) is not None):
                    continue
            status, body = self.dev.request('GET', '/' + rel)
            with self.lock:
                if status == 200 and rel not in s.outbox:
                    s.write(rel, body)
                    s.state['meta'][rel] = meta
                    if track_base:
                        s.state['base'][rel] = sha(body)
        with self.lock:
            for rel in s.files(sub):
                if rel not in listed and rel not in s.outbox:
                    s.delete(rel)
                    s.state['meta'].pop(rel, None)
                    s.state['base'].pop(rel, None)


# ---- HTTP ------------------------------------------------------------------------------------

class Handler(http.server.BaseHTTPRequestHandler):
    sync = www = None
    hosts = ('localhost',)
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        if os.environ.get('JR_LOG'):
            super().log_message(fmt, *args)

    def send(self, code, body=b'', ctype='text/plain', headers=()):
        if self.unread:
            headers = tuple(headers) + (('Connection', 'close'),)  # else the unread body becomes the next request
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        if not any(k == 'Cache-Control' for k, _ in headers):
            self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def send_json(self, obj, code=200):
        self.send(code, json.dumps(obj).encode(), 'application/json')

    def host_ok(self):
        host = (self.headers.get('Host') or '').rsplit(':', 1)[0] if ']' not in (self.headers.get('Host') or '') else ''
        try:
            ipaddress.IPv4Address(host)
            return True
        except ValueError:
            return host.lower() in self.hosts

    # Static web app: plain or pre-gzipped (as the release zip and the SD card have it)
    def static(self, rel):
        rel = os.path.normpath(rel or 'index.html').lstrip('./')
        if rel.startswith('..'):
            return self.send(400, b'bad name')
        base = os.path.join(self.www, rel)
        if os.path.isdir(base):
            base = os.path.join(base, 'index.html')
        ctype = MIME.get(os.path.splitext(base)[1], 'application/octet-stream')
        cache = ('Cache-Control', 'public, max-age=31536000, immutable' if rel.startswith('vendor/') else 'no-cache')
        for path, extra in ((base, ()), (base + '.gz', (('Content-Encoding', 'gzip'),))):
            if os.path.isfile(path):
                with open(path, 'rb') as f:
                    return self.send(200, f.read(), ctype, (cache,) + extra)
        self.send(404, b'not found')

    def mirror_file(self, rel):
        data = self.sync.store.read(rel)
        if data is None:
            return self.send(404, b'not found')
        self.send(200, data, MIME.get(os.path.splitext(rel)[1], 'application/octet-stream'))

    def proxy(self, path, body=None):
        """Straight to the device (online only); False if it can't be reached."""
        try:
            status, out = self.sync.dev.request(self.command, path, body)
        except DeviceError as e:
            with self.sync.lock:
                self.sync.offline(str(e))
            return False
        self.send(status, out, 'application/json' if out[:1] in (b'[', b'{') else 'text/plain')
        return True

    def write_through(self, rel, body):
        """A workout or plan write: to the device if it answers (its checks and errors apply),
        else into the mirror and the outbox."""
        s = self.sync
        with s.lock:
            if s.online and not s.store.outbox:
                try:
                    status, msg = s.dev.request(self.command, '/' + rel, body, timeout=10)
                except DeviceError as e:
                    s.offline(str(e))
                else:
                    if 200 <= status < 300:
                        if body is None:
                            s.store.delete(rel)
                            s.store.state['base'].pop(rel, None)
                        else:
                            s.store.write(rel, body)
                            s.store.state['base'][rel] = sha(body)
                        s.store.state['meta'].pop(rel, None)
                        s.store.save()
                    elif status == 404 and body is None:
                        s.store.delete(rel)  # deleted on the device already
                    return self.send(status, msg)
            if body is None and s.store.read(rel) is None:
                return self.send(404, b'not found')
            if body is None:
                s.store.delete(rel)
            else:
                s.store.write(rel, body)
            s.store.queue('delete' if body is None else 'put', rel, body)
            s.store.save()
        s.wake.set()
        self.send(200, b'queued: the head unit gets it when it is on')

    def route(self):
        self.unread = int(self.headers.get('Content-Length') or 0) if self.command in ('POST', 'DELETE') else 0
        if not self.host_ok():
            return self.send(403, b'bad host')
        url = urllib.parse.urlparse(self.path)
        path = urllib.parse.unquote(url.path)
        body = None
        if self.command in ('POST', 'DELETE'):
            if self.headers.get('X-JustRide') is None:
                return self.send(403, b'missing X-JustRide header')
            if self.unread > ROUTE_MAX + 1024 or path == '/ota' or path.startswith('/www/'):
                if path == '/ota' or path.startswith('/www/'):
                    return self.send(409, f'Firmware and web-app updates go to the head unit itself (they need the '
                                          f'tap there): open http://{self.sync.dev.addr}/stats/#device'.encode())
                return self.send(413, b'too large')
            body = self.rfile.read(self.unread)
            self.unread = 0
            body = body if self.command == 'POST' else None
        s = self.sync
        get = self.command in ('GET', 'HEAD')

        if path == '/' and get:
            self.send_response(302)
            self.send_header('Location', '/stats/')
            self.send_header('Content-Length', '0')
            return self.end_headers()
        if path.startswith('/stats/') and get:
            return self.static(path[len('/stats/'):])
        if path == '/api/info' and get:
            s.fresh()
            with s.lock:
                info = json.loads(s.store.read('info.json') or b'{}')
                info['upload_s'] = 0  # uploads only on the device itself
                info['nas'] = s.status()
            return self.send_json(info)
        if path == '/api/time' and self.command == 'POST':
            return None if s.online and self.proxy(path, body) else self.send(200, b'kept')
        if path == '/api/users':
            if get:
                s.fresh(wait=s.store.read('users.json') is None)  # first start: wait for the first sync
                data = s.store.read('users.json')
                return self.send(200, data, 'application/json') if data else self.send(
                    503, b'No data yet: the NAS has not reached the head unit')
            if self.command == 'POST':
                with s.lock:
                    if s.online:
                        try:
                            status, out = s.dev.request('POST', path, body)
                        except DeviceError as e:
                            s.offline(str(e))
                        else:
                            if status == 200:
                                s.store.write('users.json', out)
                                s.store.save()
                            return self.send(status, out, 'application/json' if status == 200 else 'text/plain')
                return self.send(503, b'The head unit is off: rider changes need it (FTP, weight and limits live '
                                      b'there). Try again when it is on.')
        if path == '/api/rides' and get:
            s.fresh()
            with s.lock:
                meta = s.store.state['meta']
                out = [{'name': rel[len('rides/'):], 'size': meta.get(rel, [0, 0])[0], 'mtime': meta.get(rel, [0, 0])[1]}
                       for rel in s.store.files('rides')]
            return self.send_json(out)
        if path == '/api/workouts' and get:
            s.fresh()
            with s.lock:
                meta = s.store.state['meta']
                out = []
                for rel in s.store.files('workouts'):
                    m = meta.get(rel) or [os.path.getsize(s.store.path(rel)), int(os.path.getmtime(s.store.path(rel)))]
                    out.append({'name': rel[len('workouts/'):], 'size': m[0], 'mtime': m[1]})
            return self.send_json(out)
        if path == '/api/workout.ics' and get:
            if s.online and self.proxy(path + ('?' + url.query if url.query else '')):
                return None
            return self.send(503, b'"Add to calendar" needs the head unit: try again when it is on')
        if path.startswith('/rides/') and get:
            rel = path[1:]
            if not ride_name_ok(rel[len('rides/'):]):
                return self.send(400, b'bad name')
            if s.store.read(rel) is None and s.online and self.proxy(path):
                return None
            return self.mirror_file(rel)
        if path.startswith('/workouts/'):
            name = path[len('/workouts/'):]
            if not workout_name_ok(name):
                return self.send(400, b'name must be <name>.zwo or <name>.route')
            rel = 'workouts/' + name
            if get:
                return self.mirror_file(rel)
            if self.command == 'POST':
                limit = ROUTE_MAX if name.lower().endswith('.route') else ZWO_MAX
                if not body or len(body) > limit:
                    return self.send(400 if not body else 413, b'empty or too large')
                return self.write_through(rel, body)
            if self.command == 'DELETE':
                return self.write_through(rel, None)
        if path.startswith('/plans/'):
            name = path[len('/plans/'):]
            if not plan_name_ok(name):
                return self.send(400, b'name must be <user id>.json')
            rel = 'plans/' + name
            if get:
                return self.mirror_file(rel)
            if self.command == 'POST':
                if not body or len(body) > PLAN_MAX:
                    return self.send(400 if not body else 413, b'empty or too large')
                return self.write_through(rel, body)
        self.send(404, b'not found')

    do_GET = do_HEAD = do_POST = do_DELETE = route


def serve(device, www, data, port=8080, interval=60, hosts=(), bind='0.0.0.0'):
    """Starts the sync thread and returns the (not yet serving) HTTP server."""
    sync = Sync(Store(data), Device(device))
    Handler.sync, Handler.www, Handler.hosts = sync, www, tuple(['localhost'] + [h.lower() for h in hosts])
    threading.Thread(target=sync.loop, args=(interval,), daemon=True).start()
    return http.server.ThreadingHTTPServer((bind, port), Handler)


def drop_root(data, uid, gid):
    """Started as root (Docker): give the data folder to uid:gid, then run as that user.
    A bind mount Docker created itself belongs to root, which the service user can't write."""
    if os.name != 'posix' or os.getuid() != 0:
        return
    os.makedirs(data, exist_ok=True)
    for root, dirs, files in os.walk(data):
        for p in [root] + [os.path.join(root, n) for n in dirs + files]:
            st = os.lstat(p)
            if (st.st_uid, st.st_gid) != (uid, gid):
                os.lchown(p, uid, gid)
    os.setgroups([])
    os.setgid(gid)
    os.setuid(uid)


def main():
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', default=env('JR_DEVICE'), help='head unit IPv4 address (fixed via a DHCP reservation)')
    ap.add_argument('--www', default=env('JR_WWW'),
                    help='the web app folder (www/stats); default: the ./www mount if it has one, else the image\'s')
    ap.add_argument('--data', default=env('JR_DATA', '/data'), help='mirror, outbox and state')
    ap.add_argument('--port', type=int, default=int(env('JR_PORT', '8080')))
    ap.add_argument('--interval', type=int, default=int(env('JR_INTERVAL', '60')), help='seconds between syncs')
    ap.add_argument('--host', action='append', default=[h for h in env('JR_HOSTS', '').split(',') if h],
                    help='extra host name the browser may use (e.g. nas.local); IPv4 addresses always work')
    a = ap.parse_args()
    if not a.device:
        ap.error('--device (or JR_DEVICE) is required')
    def has_app(d):
        return any(os.path.isfile(os.path.join(d, n)) for n in ('index.html', 'index.html.gz'))
    if not a.www:
        a.www = next((d for d in ('/app/www/stats', '/app/builtin/www/stats') if has_app(d)), '/app/www/stats')
    if not has_app(a.www):
        ap.error(f'no web app in {a.www}: unzip the release\'s justride-www-*.zip in the folder with '
                 'compose.yaml (the zip has www/stats/, which compose mounts as /app/www)')
    if env('JR_UID'):
        drop_root(a.data, int(env('JR_UID')), int(env('JR_GID', env('JR_UID'))))
    httpd = serve(a.device, a.www, a.data, a.port, a.interval, a.host)
    print(f'JustRide NAS on port {a.port}: device {a.device}, web app {a.www}, data {a.data}', flush=True)
    httpd.serve_forever()


if __name__ == '__main__':
    main()
