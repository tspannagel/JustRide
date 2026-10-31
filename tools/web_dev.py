#!/usr/bin/env python3
"""Run the web app (www/stats) on this computer, for development and screenshots.

  tools/web_dev.py                       sample riders and rides (generated into --data)
  tools/web_dev.py --device 192.168.1.5  real data: everything but /stats/ goes to the head unit

Then open http://localhost:8080/. The sample backend implements the device's API
(users, rides, workouts, plans) on a plain folder that looks like the SD card.
"""
import argparse
import http.server
import json
import math
import os
import random
import shutil
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WWW = os.path.join(ROOT, 'www', 'stats')
MIME = {'.html': 'text/html', '.js': 'application/javascript', '.mjs': 'application/javascript', '.css': 'text/css',
        '.wasm': 'application/wasm', '.json': 'application/json', '.csv': 'text/csv', '.zwo': 'application/xml',
        '.route': 'text/plain', '.png': 'image/png', '.svg': 'image/svg+xml',
        '.webmanifest': 'application/manifest+json'}


def sample(data):
    """Two riders, ~10 weeks of rides for the first one."""
    rnd = random.Random(7)
    os.makedirs(os.path.join(data, 'rides', 'u1'), exist_ok=True)
    os.makedirs(os.path.join(data, 'rides', 'u2'), exist_ok=True)
    os.makedirs(os.path.join(data, 'workouts'), exist_ok=True)
    os.makedirs(os.path.join(data, 'plans'), exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, 'tools', 'workouts')):
        shutil.copy(os.path.join(ROOT, 'tools', 'workouts', f), os.path.join(data, 'workouts', f))
    with open(os.path.join(data, 'users.json'), 'w') as f:
        json.dump([{'id': 'u1', 'name': 'Sam', 'active': True}, {'id': 'u2', 'name': 'Alex', 'active': False}], f)
    summary = ['ride_id,start_utc,duration_s,moving_s,workout,ftp_w,avg_w,np_w,if,tss,kj,avg_hr,max_hr,avg_cad,distance_km,fw,weight_kg,user_max_hr']
    bests = ['ride_id,duration_s,watts']
    ftp = 230
    now = datetime.now(timezone.utc).replace(hour=17, minute=30, second=0, microsecond=0)
    day = now - timedelta(days=70)
    while day < now - timedelta(hours=20):
        day += timedelta(days=rnd.choice([1, 1, 2, 2, 3]))
        if day > now:
            break
        kind = rnd.choice(['free', 'Sweet Spot 3x10', 'Endurance 60', 'free', 'VO2max 5x4min'])
        dur = rnd.choice([2700, 3600, 4500, 5400]) if kind != 'VO2max 5x4min' else 3300
        rid = day.strftime('%Y-%m-%d_%H%M')
        pts, ivl = [], 0
        for t in range(dur):
            if kind == 'Sweet Spot 3x10':
                on = 600 <= t < 2100 and (t - 600) % 900 < 600
                target = int(ftp * (0.9 if on else 0.55))
                ivl = 1 + (t - 600) // 450 if t >= 600 else 0
            elif kind == 'VO2max 5x4min':
                on = 600 <= t < 3000 and (t - 600) % 480 < 240
                target = int(ftp * (1.15 if on else 0.5))
                ivl = 1 + (t - 600) // 240 if t >= 600 else 0
            elif kind == 'Endurance 60':
                target = int(ftp * 0.68)
            else:
                target = 0
            base = target or ftp * (0.62 + 0.12 * math.sin(t / 400))
            p = max(0, int(base + rnd.gauss(0, 12)))
            if rnd.random() < 0.002:
                p = int(ftp * rnd.uniform(2.2, 3.2))
            hr = min(188, int(95 + 0.28 * min(p, ftp * 1.3) + 8 * t / dur + rnd.gauss(0, 2)))
            pts.append((t, p, 88 + rnd.gauss(0, 3), hr, target, ivl))
        ps = [p[1] for p in pts]
        avg = sum(ps) / len(ps)
        roll = [sum(ps[max(0, i - 29):i + 1]) / min(30, i + 1) for i in range(len(ps))]
        np_ = (sum(r ** 4 for r in roll) / len(roll)) ** 0.25
        IF = np_ / ftp
        tss = dur / 3600 * IF * IF * 100
        km = dur / 3600 * (28 + avg / 40)
        hrs = [p[3] for p in pts]
        summary.append(f'{rid},{day.strftime("%Y-%m-%dT%H:%M:%SZ")},{dur},{dur - 30},"{"" if kind == "free" else kind}",{ftp},'
                       f'{avg:.0f},{np_:.0f},{IF:.3f},{tss:.1f},{sum(ps) / 1000:.0f},{sum(hrs) / len(hrs):.0f},{max(hrs)},88.0,{km:.3f},dev,75,186')
        for d in (1, 5, 10, 30, 60, 120, 300, 600, 1200, 1800, 3600):
            if d <= dur:
                s = sum(ps[:d])
                best = s
                for i in range(d, len(ps)):
                    s += ps[i] - ps[i - d]
                    best = max(best, s)
                bests.append(f'{rid},{d},{best // d}')
        with open(os.path.join(data, 'rides', 'u1', rid + '.csv'), 'w') as f:
            f.write('t_s,ts_utc,power_w,cadence_rpm,speed_kmh,hr_bpm,target_w,grade_pct,gear,interval_idx,mode,lap\n')
            for t, p, c, h, tg, iv in pts:
                ts = (day + timedelta(seconds=t)).strftime('%Y-%m-%dT%H:%M:%SZ')
                f.write(f'{t},{ts},{p},{c:.1f},{28 + p / 40:.2f},{h},{tg},0.00,12,{iv},{"erg" if tg else "free"},0\n')
        ftp += rnd.choice([0, 0, 0, 1])
    with open(os.path.join(data, 'rides', 'u1', 'summary.csv'), 'w') as f:
        f.write('\n'.join(summary) + '\n')
    with open(os.path.join(data, 'rides', 'u1', 'bests.csv'), 'w') as f:
        f.write('\n'.join(bests) + '\n')


USER_DEFAULTS = {'ftp_w': 200, 'max_erg_w': 600, 'weight_kg': 75, 'max_hr': 185}
LIMITS = {'ftp_w': (50, 600), 'max_erg_w': (100, 1500), 'weight_kg': (30, 200), 'max_hr': (100, 230)}


def users_action(users, req):
    """POST /api/users as the device does it; returns an error text or None."""
    valid = lambda n: isinstance(n, str) and 1 <= len(n) <= 15 and n.strip() and all(32 <= ord(c) < 127 for c in n)
    find = [u for u in users if u['id'] == req.get('id')]
    act = req.get('action')
    if act == 'add':
        if len(users) >= 8 or not valid(req.get('name')):
            return 'name invalid, or already 8 riders'
        n = max(int(u['id'][1:]) for u in users) + 1
        for u in users:
            u['active'] = False
        users.append({'id': f'u{n}', 'name': req['name'], 'active': True, **USER_DEFAULTS})
    elif act == 'select' and find:
        for u in users:
            u['active'] = u is find[0]
    elif act == 'delete' and find:
        if len(users) < 2:
            return "the last rider can't be deleted"
        users.remove(find[0])
        if not any(u['active'] for u in users):
            users[0]['active'] = True
    elif act == 'edit' and find:
        vals = {k: req.get(k, find[0][k]) for k in LIMITS}
        if not valid(req.get('name', find[0]['name'])) or any(not isinstance(v, int) or not lo <= v <= hi
                                                              for k, v in vals.items() for lo, hi in [LIMITS[k]]):
            return 'value out of range'
        find[0].update(vals, name=req.get('name', find[0]['name']))
    else:
        return 'bad request'


class Handler(http.server.BaseHTTPRequestHandler):
    data = device = None

    def log_message(self, fmt, *args):
        if os.environ.get('WEB_DEV_LOG'):
            super().log_message(fmt, *args)

    def send(self, code, body=b'', ctype='text/plain'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def file(self, path):
        if not os.path.isfile(path):
            return self.send(404, b'not found')
        with open(path, 'rb') as f:
            self.send(200, f.read(), MIME.get(os.path.splitext(path)[1], 'application/octet-stream'))

    def proxy(self, body=None):
        req = urllib.request.Request(f'http://{self.device}{self.path}', data=body, method=self.command,
                                     headers={'Host': self.device, 'X-JustRide': '1'})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                self.send(r.status, r.read(), r.headers.get('Content-Type', 'text/plain'))
        except urllib.error.HTTPError as e:
            self.send(e.code, e.read())

    def route(self):
        url = urllib.parse.urlparse(self.path)
        path = urllib.parse.unquote(url.path)
        if path == '/':
            self.send_response(302)
            self.send_header('Location', '/stats/')
            self.end_headers()
            return
        if path.startswith('/stats/'):
            rel = path[len('/stats/'):] or 'index.html'
            return self.file(os.path.join(WWW, os.path.normpath(rel).lstrip('./')))
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0)) if self.command in ('POST',) else None
        if self.device:
            return self.proxy(body)
        if self.command != 'GET' and self.headers.get('X-JustRide') is None:
            return self.send(403, b'missing X-JustRide header')
        if '..' in path:
            return self.send(400, b'bad name')
        d = self.data
        if path == '/api/users':
            with open(os.path.join(d, 'users.json')) as f:
                users = [{**USER_DEFAULTS, **u} for u in json.load(f)]
            if self.command == 'POST':
                err = users_action(users, json.loads(body or b'{}'))
                if err:
                    return self.send(400, err.encode())
                with open(os.path.join(d, 'users.json'), 'w') as f:
                    json.dump(users, f)
            return self.send(200, json.dumps(users).encode(), 'application/json')
        if path == '/api/time':
            return self.send(200, b'kept')
        if path == '/api/rides':
            out = []
            for u in sorted(os.listdir(os.path.join(d, 'rides'))):
                for f in sorted(os.listdir(os.path.join(d, 'rides', u))):
                    st = os.stat(os.path.join(d, 'rides', u, f))
                    out.append({'name': f'{u}/{f}', 'size': st.st_size, 'mtime': int(st.st_mtime)})
            return self.send(200, json.dumps(out).encode(), 'application/json')
        if path == '/api/workouts':
            out = [{'name': f, 'size': os.path.getsize(os.path.join(d, 'workouts', f)), 'mtime': 0}
                   for f in os.listdir(os.path.join(d, 'workouts')) if f.lower().endswith(('.zwo', '.route'))]
            return self.send(200, json.dumps(out).encode(), 'application/json')
        for prefix in ('/rides/', '/workouts/', '/plans/'):
            if path.startswith(prefix):
                target = os.path.join(d, prefix.strip('/'), path[len(prefix):])
                if self.command == 'GET':
                    return self.file(target)
                if self.command == 'POST' and prefix != '/rides/':
                    with open(target, 'wb') as f:
                        f.write(body)
                    return self.send(200, b'saved')
                if self.command == 'DELETE' and prefix == '/workouts/':
                    if not os.path.isfile(target):
                        return self.send(404, b'not found')
                    os.remove(target)
                    return self.send(200, b'deleted')
        self.send(404, b'not found')

    do_GET = do_POST = do_DELETE = route


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--device', help='head unit address: use its data instead of samples')
    ap.add_argument('--data', default=os.path.join(ROOT, 'build', 'web_dev'), help='sample SD folder (created if missing)')
    a = ap.parse_args()
    if not a.device and not os.path.isdir(a.data):
        sample(a.data)
    Handler.data, Handler.device = a.data, a.device
    print(f'http://localhost:{a.port}/  ({"device " + a.device if a.device else "samples in " + a.data})')
    http.server.ThreadingHTTPServer(('127.0.0.1', a.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
