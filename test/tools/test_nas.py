"""Tests for nas/justride_nas.py against tools/web_dev.py's sample backend as the head unit,
which is switched off and on during a test.

  python3 -m unittest discover -s test/tools
"""
import gzip
import http.client
import http.server
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'nas'))
import justride_nas as nas  # noqa: E402
import web_dev  # noqa: E402

PLAN = b'{"v":2,"setup":{},"sessions":[]}'
ZWO = b'<workout_file><name>Queued</name><workout><SteadyState Duration="600" Power="0.6"/></workout></workout_file>'


class FakeDevice:
    """web_dev's sample backend on a fixed port that can be stopped and started again."""

    def __init__(self, sd):
        self.sd = sd
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), web_dev.Handler)
        self.port = self.httpd.server_address[1]
        self._run()

    def _run(self):
        web_dev.Handler.data, web_dev.Handler.device = self.sd, None
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def off(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def on(self):
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', self.port), web_dev.Handler)
        self._run()

    def path(self, *p):
        return os.path.join(self.sd, *p)


class NasTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.template = tempfile.mkdtemp()
        web_dev.sample(os.path.join(cls.template, 'sd'))
        nas.FRESH_S = 1e9  # the tests sync explicitly

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.template)

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        shutil.copytree(os.path.join(self.template, 'sd'), os.path.join(self.tmp, 'sd'))
        os.makedirs(os.path.join(self.tmp, 'www'))
        with gzip.open(os.path.join(self.tmp, 'www', 'index.html.gz'), 'wb') as f:
            f.write(b'<!doctype html><title>JustRide</title>')
        self.dev = FakeDevice(os.path.join(self.tmp, 'sd'))
        self.sync = nas.Sync(nas.Store(os.path.join(self.tmp, 'data')), nas.Device(f'127.0.0.1:{self.dev.port}', timeout=3))
        nas.Handler.sync, nas.Handler.www, nas.Handler.hosts = self.sync, os.path.join(self.tmp, 'www'), ('localhost',)
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), nas.Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.sync.run_once()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        try:
            self.dev.off()
        except OSError:
            pass
        shutil.rmtree(self.tmp)

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection('127.0.0.1', self.httpd.server_address[1], timeout=10)
        h = {'Host': f'127.0.0.1:{self.httpd.server_address[1]}'}
        if method in ('POST', 'DELETE'):
            h['X-JustRide'] = '1'
        h.update(headers or {})
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        out = r.status, r.read(), dict(r.getheaders())
        c.close()
        return out

    def info(self):
        return json.loads(self.req('GET', '/api/info')[1])['nas']

    # ---- online -----------------------------------------------------------------------------

    def test_mirror_matches_the_device(self):
        st, body, _ = self.req('GET', '/api/users')
        self.assertEqual(st, 200)
        self.assertEqual([u['name'] for u in json.loads(body)], ['Sam', 'Alex'])
        rides = json.loads(self.req('GET', '/api/rides')[1])
        on_device = sorted(f'u1/{f}' for f in os.listdir(self.dev.path('rides', 'u1')))
        self.assertEqual(sorted(r['name'] for r in rides), on_device)
        st, body, _ = self.req('GET', '/rides/u1/summary.csv')
        with open(self.dev.path('rides', 'u1', 'summary.csv'), 'rb') as f:
            self.assertEqual(body, f.read())
        names = {w['name'] for w in json.loads(self.req('GET', '/api/workouts')[1])}
        self.assertEqual(names, {f for f in os.listdir(self.dev.path('workouts'))})
        info = json.loads(self.req('GET', '/api/info')[1])
        self.assertEqual(info['project'], 'justride')
        self.assertEqual(info['upload_s'], 0)  # uploads only on the device itself
        self.assertEqual((info['nas']['online'], info['nas']['pending']), (True, 0))

    def test_online_writes_go_straight_to_the_device(self):
        st, body, _ = self.req('POST', '/plans/u1.json', PLAN)
        self.assertEqual((st, body), (200, b'saved'))  # the device's answer
        with open(self.dev.path('plans', 'u1.json'), 'rb') as f:
            self.assertEqual(f.read(), PLAN)
        self.assertEqual(self.req('GET', '/plans/u1.json')[1], PLAN)
        st, body, _ = self.req('POST', '/api/users', json.dumps({'action': 'edit', 'id': 'u1', 'ftp_w': 260}).encode())
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(self.req('GET', '/api/users')[1])[0]['ftp_w'], 260)

    def test_changes_on_the_device_are_pulled(self):
        with open(self.dev.path('workouts', 'New one.zwo'), 'wb') as f:
            f.write(ZWO)
        os.remove(self.dev.path('workouts', 'endurance_60.zwo'))
        os.rename(self.dev.path('rides', 'u1', sorted(os.listdir(self.dev.path('rides', 'u1')))[0]),
                  self.dev.path('rides', 'u1', '2099-01-01_0000.csv'))  # a re-dated ride
        self.sync.run_once()
        names = {w['name'] for w in json.loads(self.req('GET', '/api/workouts')[1])}
        self.assertIn('New one.zwo', names)
        self.assertNotIn('endurance_60.zwo', names)
        self.assertEqual(self.req('GET', '/workouts/New%20one.zwo')[1], ZWO)
        rides = sorted(r['name'] for r in json.loads(self.req('GET', '/api/rides')[1]))
        self.assertEqual(rides, sorted(f'u1/{f}' for f in os.listdir(self.dev.path('rides', 'u1'))))

    def test_workouts_mirrored_before_rides(self):
        """A failing ride download must not keep the workouts out of the mirror."""
        shutil.rmtree(os.path.join(self.tmp, 'data', 'mirror', 'workouts'))
        self.sync.store.state['meta'].clear()
        orig = self.sync.dev.request

        def flaky(method, path, *a, **kw):
            if path.startswith('/rides/'):
                raise nas.DeviceError('timed out')
            return orig(method, path, *a, **kw)
        self.sync.dev.request = flaky
        self.sync.run_once()
        names = {w['name'] for w in json.loads(self.req('GET', '/api/workouts')[1])}
        self.assertEqual(names, set(os.listdir(self.dev.path('workouts'))))

    def test_one_lost_answer_is_not_off(self):
        """A head unit busy with BLE can lose one /api/info answer: that's not "off"."""
        orig, lost = self.sync.dev.request, []

        def once(method, path, *a, **kw):
            if path == '/api/info' and not lost:
                lost.append(path)
                raise nas.DeviceError('timed out')
            return orig(method, path, *a, **kw)
        self.sync.dev.request = once
        self.sync.run_once()
        self.assertEqual(lost, ['/api/info'])
        self.assertTrue(self.info()['online'])

    def test_unexpected_error_keeps_syncing(self):
        orig = self.sync.store.write
        self.sync.store.write = lambda rel, data: (_ for _ in ()).throw(PermissionError(13, 'Permission denied'))
        self.sync.run_once()  # must not raise: it would end the sync thread for good
        n = self.info()
        self.assertTrue(n['online'])
        self.assertIn('PermissionError', n['last_error'])
        self.sync.store.write = orig
        with open(self.dev.path('workouts', 'New one.zwo'), 'wb') as f:
            f.write(ZWO)
        self.sync.run_once()
        self.assertIsNone(self.info()['last_error'])
        self.assertIn('New one.zwo', {w['name'] for w in json.loads(self.req('GET', '/api/workouts')[1])})

    # ---- offline ----------------------------------------------------------------------------

    def test_offline_reads_and_queued_writes(self):
        self.dev.off()
        self.sync.run_once()
        self.assertFalse(self.info()['online'])
        self.assertEqual(self.req('GET', '/api/users')[0], 200)  # from the mirror
        self.assertEqual(self.req('GET', '/rides/u1/summary.csv')[0], 200)

        self.assertEqual(self.req('POST', '/plans/u1.json', PLAN)[:2], (200, b'queued: the head unit gets it when it is on'))
        self.assertEqual(self.req('POST', '/workouts/Queued.zwo', ZWO)[0], 200)
        self.assertEqual(self.req('DELETE', '/workouts/endurance_60.zwo')[0], 200)
        self.assertEqual(self.req('GET', '/plans/u1.json')[1], PLAN)  # the app sees its own writes
        names = {w['name'] for w in json.loads(self.req('GET', '/api/workouts')[1])}
        self.assertIn('Queued.zwo', names)
        self.assertNotIn('endurance_60.zwo', names)
        self.assertEqual(self.info()['pending'], 3)

        # Riders stay with the device; updates need it too
        st, body, _ = self.req('POST', '/api/users', b'{"action":"edit","id":"u1","ftp_w":250}')
        self.assertEqual(st, 503)
        self.assertIn(b'head unit is off', body)
        self.assertEqual(self.req('POST', '/ota', b'x' * 10)[0], 409)

        self.dev.on()
        self.sync.run_once()
        self.assertEqual(self.info(), {**self.info(), 'online': True, 'pending': 0})
        with open(self.dev.path('plans', 'u1.json'), 'rb') as f:
            self.assertEqual(f.read(), PLAN)
        with open(self.dev.path('workouts', 'Queued.zwo'), 'rb') as f:
            self.assertEqual(f.read(), ZWO)
        self.assertFalse(os.path.exists(self.dev.path('workouts', 'endurance_60.zwo')))
        # and nothing came back from the pull that undoes it
        names = {w['name'] for w in json.loads(self.req('GET', '/api/workouts')[1])}
        self.assertIn('Queued.zwo', names)
        self.assertNotIn('endurance_60.zwo', names)

    def test_queue_survives_a_restart(self):
        self.dev.off()
        self.sync.run_once()
        self.req('POST', '/plans/u1.json', PLAN)
        # A new service on the same data folder (container restart)
        self.sync = nas.Sync(nas.Store(os.path.join(self.tmp, 'data')), nas.Device(f'127.0.0.1:{self.dev.port}', timeout=3))
        nas.Handler.sync = self.sync
        self.assertEqual(self.info()['pending'], 1)
        self.dev.on()
        self.sync.run_once()
        with open(self.dev.path('plans', 'u1.json'), 'rb') as f:
            self.assertEqual(f.read(), PLAN)

    def test_conflict_nas_wins_device_copy_kept(self):
        with open(self.dev.path('plans', 'u1.json'), 'wb') as f:
            f.write(b'{"v":2,"from":"before"}')
        self.sync.run_once()
        self.dev.off()
        self.sync.run_once()
        self.req('POST', '/plans/u1.json', PLAN)
        with open(self.dev.path('plans', 'u1.json'), 'wb') as f:  # edited on the device directly meanwhile
            f.write(b'{"v":2,"from":"device"}')
        self.dev.on()
        self.sync.run_once()
        with open(self.dev.path('plans', 'u1.json'), 'rb') as f:
            self.assertEqual(f.read(), PLAN)
        kept = os.listdir(os.path.join(self.tmp, 'data', 'conflicts'))
        self.assertEqual(len(kept), 1)
        with open(os.path.join(self.tmp, 'data', 'conflicts', kept[0]), 'rb') as f:
            self.assertEqual(f.read(), b'{"v":2,"from":"device"}')

    def test_plan_edited_only_on_the_device_is_taken(self):
        self.req('POST', '/plans/u1.json', PLAN)
        with open(self.dev.path('plans', 'u1.json'), 'wb') as f:
            f.write(b'{"v":2,"from":"device"}')
        self.sync.run_once()
        self.assertEqual(self.req('GET', '/plans/u1.json')[1], b'{"v":2,"from":"device"}')
        self.assertEqual(os.listdir(os.path.join(self.tmp, 'data', 'conflicts')), [])

    # ---- the service's own rules --------------------------------------------------------------

    def test_host_and_csrf_checks(self):
        self.assertEqual(self.req('GET', '/api/users', headers={'Host': 'evil.example'})[0], 403)
        self.assertEqual(self.req('GET', '/api/users', headers={'Host': 'localhost:8080'})[0], 200)
        c = http.client.HTTPConnection('127.0.0.1', self.httpd.server_address[1], timeout=5)
        c.request('POST', '/plans/u1.json', body=PLAN)  # no X-JustRide
        self.assertEqual(c.getresponse().status, 403)
        c.close()
        self.assertEqual(self.req('POST', '/workouts/..%2Fx.zwo', ZWO)[0], 400)
        self.assertEqual(self.req('POST', '/plans/../x.json', PLAN)[0], 400)
        self.assertEqual(self.req('GET', '/rides/u1/..%2F..%2Fstate.json')[0], 400)

    def test_web_app_files_pre_gzipped(self):
        st, body, h = self.req('GET', '/stats/')
        self.assertEqual((st, h.get('Content-Encoding')), (200, 'gzip'))
        self.assertIn(b'JustRide', gzip.decompress(body))
        self.assertEqual(self.req('GET', '/')[0], 302)


if __name__ == '__main__':
    unittest.main()
