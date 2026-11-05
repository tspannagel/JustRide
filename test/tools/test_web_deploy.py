"""tools/web_deploy.sh against a small fake device. Run: python3 -m unittest test/tools/test_web_deploy.py"""
import gzip
import http.server
import json
import pathlib
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class Device(http.server.ThreadingHTTPServer):
    """The endpoints the script uses. close_after: the upload window runs out after n uploads."""

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.files, self.posts = {}, []
        self.window, self.close_after, self.reopen_on_poll, self.polls = True, None, False, 0


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, body=b''):
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        d = self.server
        if self.path == '/api/info':
            d.polls += 1
            if not d.window and d.reopen_on_poll and d.polls >= 3:   # the rider taps while the script waits
                d.window = True
            return self.reply(200, json.dumps({'project': 'justride', 'upload_s': 100 if d.window else 0}).encode())
        name = self.path.removeprefix('/stats/')
        return self.reply(200, d.files[name]) if name in d.files else self.reply(404)

    def do_POST(self):
        d = self.server
        body = self.rfile.read(int(self.headers['Content-Length']))
        if self.headers.get('X-JustRide') is None or not d.window:
            return self.reply(403, b'tap')
        if not body:
            return self.reply(400, b'file empty')   # as receive_file() in web.c
        name = self.path.removeprefix('/www/stats/')
        d.files[name] = body
        d.posts.append(name)
        if d.close_after is not None:
            d.close_after -= 1
            if d.close_after == 0:
                d.window = False
        self.reply(200)


class WebDeploy(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        (self.tmp / 'tools').mkdir()
        shutil.copy(REPO / 'tools/web_deploy.sh', self.tmp / 'tools')
        self.write('www/stats/index.html', '<html>')
        self.write('www/stats/plan.js', 'plan 1')
        self.write('www/stats/vendor/lib-1/lib.js', 'lib')
        self.write('docs/manual.html', 'manual')
        self.write('docs/img/a.png', 'png')
        self.write('docs/img/JustRide.code-workspace', 'stray')
        self.dev = Device()
        threading.Thread(target=self.dev.serve_forever, daemon=True).start()
        self.addCleanup(self.dev.server_close)
        self.addCleanup(self.dev.shutdown)

    def write(self, rel, text):
        p = self.tmp / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def run_script(self, *args):
        return subprocess.run(['sh', str(self.tmp / 'tools/web_deploy.sh'), f'127.0.0.1:{self.dev.server_port}', *args],
                              capture_output=True, text=True, timeout=60)

    def deployed(self):
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.dev.posts.clear()

    def test_first_deploy_sends_everything_in_order_and_gzipped(self):
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        p = self.dev.posts
        self.assertEqual(p[0], 'deploy-manifest.txt')
        self.assertNotIn(b'# deploy', self.dev.files['deploy-manifest.txt'])
        self.assertEqual(p[-1], 'deploy-manifest.txt')
        self.assertEqual(p[1], 'vendor/lib-1/lib.js.gz')
        self.assertLess(p.index('plan.js.gz'), p.index('version.txt'))
        self.assertLess(p.index('version.txt'), p.index('manual/index.html.gz'))
        self.assertEqual(gzip.decompress(self.dev.files['plan.js.gz']), b'plan 1')
        self.assertIn('manual/img/a.png', p)
        self.assertNotIn('manual/img/JustRide.code-workspace', p)

    def test_second_deploy_sends_only_what_changed(self):
        self.deployed()
        self.write('www/stats/plan.js', 'plan 2')
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(sorted(set(self.dev.posts)), ['deploy-manifest.txt', 'plan.js.gz'])

    def test_nothing_changed_needs_no_tap(self):
        self.deployed()
        self.dev.window = False
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('up to date', r.stdout)
        self.assertEqual(self.dev.posts, [])

    def test_new_version_alone_is_not_sent(self):
        self.deployed()
        self.dev.files['version.txt'] = b'older\n'
        manifest = self.dev.files['deploy-manifest.txt'].decode().splitlines()
        self.dev.files['deploy-manifest.txt'] = '\n'.join(
            ('0' * 64 + '  version.txt') if l.endswith('  version.txt') else l for l in manifest).encode() + b'\n'
        self.assertIn('up to date', self.run_script().stdout)
        self.assertEqual(self.dev.posts, [])

    def test_check_reports_and_writes_nothing(self):
        self.deployed()
        self.assertEqual(self.run_script('--check').returncode, 0)
        self.write('www/stats/plan.js', 'plan 2')
        r = self.run_script('--check')
        self.assertEqual(r.returncode, 1)
        self.assertIn('plan.js', r.stdout)
        self.assertEqual(self.dev.posts, [])

    def test_waits_for_the_tap(self):
        self.dev.window, self.dev.reopen_on_poll = False, True
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('tap', r.stdout)
        self.assertIn('plan.js.gz', self.dev.posts)

    def test_window_running_out_midway_is_survived(self):
        self.dev.close_after, self.dev.reopen_on_poll = 2, True
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('plan.js.gz', self.dev.posts)

    def test_interrupted_deploy_resends_what_was_not_finished(self):
        self.deployed()
        self.write('www/stats/plan.js', 'plan 2')
        self.write('www/stats/index.html', '<html2>')
        self.dev.window, self.dev.close_after = True, 2   # interim list + index.html, then the tap never comes
        self.dev.reopen_on_poll = False
        proc = subprocess.Popen(['sh', str(self.tmp / 'tools/web_deploy.sh'), f'127.0.0.1:{self.dev.server_port}'],
                                stdout=subprocess.DEVNULL)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        while self.dev.window:
            time.sleep(0.01)
        proc.kill()
        self.dev.posts.clear()
        self.dev.window, self.dev.close_after = True, None
        # The file that did get through is left out of the list too: nothing can be skipped wrongly
        self.write('www/stats/index.html', '<html>')   # rider went back to the old file: must be resent
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('index.html.gz', self.dev.posts)
        self.assertIn('plan.js.gz', self.dev.posts)
        self.assertEqual(gzip.decompress(self.dev.files['index.html.gz']), b'<html>')

    def test_unreachable_device_says_so(self):
        self.dev.shutdown()
        self.dev.server_close()
        r = self.run_script()
        self.assertEqual(r.returncode, 1)
        self.assertIn("doesn't answer", r.stdout)


if __name__ == '__main__':
    unittest.main()
