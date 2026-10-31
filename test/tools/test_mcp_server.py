"""Tests for tools/mcp_server.py against tools/web_dev.py's sample backend (no device, no mcp library).

  python3 -m unittest discover -s test/tools
"""
import http.server
import os
import shutil
import sys
import tempfile
import threading
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mcp_server as m  # noqa: E402
import web_dev  # noqa: E402


class McpServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        data = os.path.join(cls.tmp, 'sd')
        web_dev.sample(data)
        web_dev.Handler.data, web_dev.Handler.device = data, None
        cls.data = data
        cls.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), web_dev.Handler)
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.app = m.JustRide(m.Device(f'127.0.0.1:{cls.httpd.server_address[1]}'))
        # Alex: FTP 200 W, Max ERG 300 W
        cls.app.dev.request('/api/users', b'{"action":"edit","id":"u2","max_erg_w":300}')

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        shutil.rmtree(cls.tmp)

    def test_riders(self):
        r = self.app.list_riders()['riders']
        self.assertEqual([u['name'] for u in r], ['Sam', 'Alex'])
        self.assertEqual(self.app.rider('alex')['id'], 'u2')
        self.assertEqual(self.app.rider()['id'], 'u1')
        with self.assertRaises(m.JustRideError):
            self.app.rider('nobody')

    def test_history_and_detail(self):
        h = self.app.ride_history(limit=3)
        self.assertEqual(len(h['rides']), 3)
        self.assertGreater(h['total_matching'], 10)
        newest = h['rides'][0]
        self.assertGreaterEqual(newest['start_utc'], h['rides'][1]['start_utc'])
        self.assertIsInstance(newest['tss'], float)
        day = newest['start_utc'][:10]
        only = self.app.ride_history(since=day, until=day)['rides']
        self.assertTrue(only and all(r['start_utc'].startswith(day) for r in only))
        d = self.app.ride_detail(newest['ride_id'], step_s=60)
        self.assertEqual(d['seconds'], newest['duration_s'])
        self.assertEqual(len(d['series']), -(-d['seconds'] // 60))
        self.assertEqual(sum(d['power_zones_s'].values()), d['seconds'])
        self.assertEqual(sum(d['hr_zones_s'].values()), d['seconds'])
        with self.assertRaises(m.JustRideError):
            self.app.ride_detail('../users')
        with self.assertRaises(m.JustRideError):
            self.app.ride_detail('2000-01-01_0000')

    def test_bests_and_load(self):
        b = self.app.power_bests(days=3650)
        durs = [x['duration_s'] for x in b['all_time']]
        self.assertEqual(durs, sorted(durs))
        self.assertIn(1200, durs)
        self.assertIn('ftp_estimate_w', b)
        watts = [x['watts'] for x in b['all_time']]
        self.assertEqual(watts, sorted(watts, reverse=True))  # longer efforts never beat shorter ones
        t = self.app.training_load(days=7)
        self.assertEqual(len(t['daily']), 7)
        self.assertGreater(t['today']['fitness_ctl'], 0)
        self.assertEqual(self.app.training_load('Alex')['note'], 'no dated rides yet')

    def test_workouts_and_plan(self):
        names = self.app.list_workouts()['workouts']
        self.assertIn('sweet_spot_3x10.zwo', names)
        w = self.app.get_workout('sweet_spot_3x10')
        self.assertTrue(w['device_view']['ok'])
        self.assertEqual(w['device_view']['total_s'], 3600)
        self.assertIsNone(self.app.get_plan()['plan'])

    BLOCKS = [{'type': 'warmup', 'duration_s': 600, 'from': 0.45, 'to': 0.7},
              {'type': 'intervals', 'repeat': 5, 'on_s': 240, 'off_s': 240, 'on_power': 1.15, 'off_power': 0.5,
               'cadence': 95, 'cadence_rest': 85},
              {'type': 'steady', 'duration_s': 300, 'power': 0.6},
              {'type': 'adaptive', 'duration_s': 1200, 'power': 0.65, 'hr_zone': 2, 'max': 0.75},
              {'type': 'free', 'duration_s': 120},
              {'type': 'cooldown', 'duration_s': 300, 'from': 0.6, 'to': 0.4}]

    def test_create_workout(self):
        r = self.app.create_workout('MCP VO2 5x4', self.BLOCKS, 'five by four & easy')
        self.assertEqual(r['saved'], 'MCP VO2 5x4.zwo')
        v = r['device_view']
        self.assertEqual(v['segments'], 1 + 10 + 1 + 1 + 1 + 1)
        self.assertEqual(v['goals'], 1)
        self.assertEqual(v['total_s'], 600 + 2400 + 300 + 1200 + 120 + 300)
        self.assertEqual(v['max_pm'], 1150)
        self.assertEqual(v['name'], 'MCP VO2 5x4')
        with open(os.path.join(self.data, 'workouts', 'MCP VO2 5x4.zwo')) as f:
            xml = f.read()
        self.assertIn('JrHrLow="0.6" JrHrHigh="0.7"', xml)
        self.assertIn('<Cooldown Duration="300" PowerLow="0.6" PowerHigh="0.4"/>', xml)
        self.assertIn('five by four &amp; easy', xml)
        # no silent overwrite; replace works
        with self.assertRaises(m.JustRideError):
            self.app.create_workout('MCP VO2 5x4', self.BLOCKS)
        self.assertTrue(self.app.create_workout('MCP VO2 5x4', self.BLOCKS, replace=True)['replaced'])

    def test_create_workout_refusals(self):
        bad = [
            ('Plan 2026-11-01 Endurance', self.BLOCKS),                       # plan-owned name
            ('../evil', self.BLOCKS),
            ('x', []),
            ('x', [{'type': 'steady', 'duration_s': 60, 'power': 3.5}]),      # parser range
            ('x', [{'type': 'steady', 'duration_s': 0, 'power': 0.5}]),
            ('x', [{'type': 'steady', 'duration_s': 60, 'power': True}]),
            ('x', [{'type': 'sprint', 'duration_s': 60}]),
            ('x', [{'type': 'adaptive', 'duration_s': 60, 'power': 0.6}]),    # no goal
            ('x', [{'type': 'steady', 'duration_s': 60, 'power': 0.5},                # 201 segments
                   {'type': 'intervals', 'repeat': 100, 'on_s': 10, 'off_s': 10, 'on_power': 1, 'off_power': 0.5}]),
        ]
        for name, blocks in bad:
            with self.subTest(name=name, blocks=blocks), self.assertRaises(m.JustRideError):
                self.app.create_workout(name, blocks)
        # Max ERG: 1.6 x 200 W = 320 W > 300 W; adaptive 1.4 may rise +15 % to 322 W
        for blocks in ([{'type': 'steady', 'duration_s': 60, 'power': 1.6}],
                       [{'type': 'adaptive', 'duration_s': 60, 'power': 1.4, 'hold_average': True}]):
            with self.subTest(blocks=blocks), self.assertRaisesRegex(m.JustRideError, 'Max ERG'):
                self.app.create_workout('x', blocks, rider='Alex')
        self.app.create_workout('y', [{'type': 'steady', 'duration_s': 60, 'power': 1.5}], rider='Alex')
        self.assertNotIn('x.zwo', self.app.list_workouts()['workouts'])

    def test_unreachable(self):
        app = m.JustRide(m.Device('127.0.0.1:1', timeout=2))
        with self.assertRaisesRegex(m.JustRideError, 'Wi-Fi is off during a ride'):
            app.list_riders()


if __name__ == '__main__':
    unittest.main()
