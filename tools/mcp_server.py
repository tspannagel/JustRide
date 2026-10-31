#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp"]
# ///
"""JustRide MCP server: lets Claude (Desktop or Code) read ride stats and add workouts.

Runs on your computer (stdio), not on the head unit, and talks to the device's web API
over Wi-Fi like the web app does. The device must be on and idle (Wi-Fi is off while riding).

  uv run tools/mcp_server.py                      device at trainer.local
  uv run tools/mcp_server.py --device 192.168.1.5
  uv run tools/mcp_server.py --device localhost:8080   tools/web_dev.py sample data

What it can do: read riders, ride summaries, best efforts, one ride's data, training load,
workouts and the training plan; write new workouts (.zwo) to the SD card. It can't change
riders, FTP or Max ERG, delete files, write the plan or upload firmware.

Workouts are checked with the firmware's own parser (tools/zwo_check.c, compiled on first
use with the system C compiler) and against the rider's Max ERG before upload.
"""
import argparse
import csv
import functools
import io
import json
import math
import os
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BEST_DURATIONS = (1, 5, 10, 30, 60, 120, 300, 600, 1200, 1800, 3600)
# Power zones as in the web app (www/stats/core.js ZONES), upper bounds as fractions of FTP
POWER_ZONES = (('Recovery', 0.55), ('Endurance', 0.75), ('Tempo', 0.90), ('Threshold', 1.05),
               ('VO2max', 1.20), ('Anaerobic', 1.50), ('Neuromuscular', math.inf))
# Heart-rate zones as on the device (metrics_hr_zone): fractions of max HR
HR_ZONES = (('Recovery', 0.5, 0.6), ('Endurance', 0.6, 0.7), ('Aerobic', 0.7, 0.8),
            ('Threshold', 0.8, 0.9), ('Maximum', 0.9, 1.0))
MAX_SEGMENTS = 200          # WORKOUT_MAX_SEGMENTS in workout.h
ZWO_MAX_BYTES = 64 * 1024   # ZWO_MAX_BYTES in workout.h
MAX_DUR_S = 36000           # per element, as zwo.c clamps


class JustRideError(Exception):
    """An error the model should see as a plain message."""


# ---- Device API -----------------------------------------------------------------------

class Device:
    def __init__(self, host='trainer.local', timeout=15):
        self.host = host
        self.timeout = timeout

    def request(self, path, body=None, method=None):
        req = urllib.request.Request(f'http://{self.host}{urllib.parse.quote(path)}', data=body,
                                     method=method or ('POST' if body is not None else 'GET'))
        if req.get_method() != 'GET':
            req.add_header('X-JustRide', '1')  # the device's CSRF guard
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise JustRideError(f'device answered {e.code} for {path}: {e.read()[:200].decode(errors="replace")}')
        except (urllib.error.URLError, OSError) as e:
            raise JustRideError(f'head unit not reachable at {self.host} ({getattr(e, "reason", e)}). '
                                'It must be switched on and idle: Wi-Fi is off during a ride.')

    def text(self, path):
        b = self.request(path)
        return None if b is None else b.decode('utf-8', errors='replace')

    def json(self, path):
        b = self.request(path)
        return None if b is None else json.loads(b)


# ---- Parsing helpers ------------------------------------------------------------------

def num(v):
    """CSV field -> int/float, None if empty (unknown)."""
    if v is None or v == '':
        return None
    try:
        f = float(v)
    except ValueError:
        return v
    if not math.isfinite(f):
        return None
    return int(f) if f.is_integer() and '.' not in v else f


def read_csv(text):
    if not text:
        return []
    return [{k: (v if k in ('ride_id', 'start_utc', 'ts_utc', 'workout', 'fw', 'mode') else num(v))
             for k, v in row.items() if k is not None} for row in csv.DictReader(io.StringIO(text))]


def parse_utc(s):
    try:
        return datetime.strptime(s, '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=timezone.utc)
    except (TypeError, ValueError):
        return None


def dated(r):
    """Rides recorded before the clock was set are stamped 1970."""
    t = parse_utc(r.get('start_utc'))
    return t is not None and t.year >= 2020


def parse_day(s, name):
    try:
        return datetime.strptime(s, '%Y-%m-%d').replace(tzinfo=timezone.utc)
    except ValueError:
        raise JustRideError(f'{name} must be a date like 2026-10-31')


class JustRide:
    """The tools' logic, independent of the MCP library (tested in test/tools)."""

    def __init__(self, device, zwo_check=None):
        self.dev = device
        self.zwo_check_path = zwo_check

    # -- riders --
    def riders(self):
        users = self.dev.json('/api/users')
        if not users:
            raise JustRideError('the device returned no riders')
        return users

    def rider(self, rider=None):
        users = self.riders()
        if not rider:
            return next((u for u in users if u.get('active')), users[0])
        key = str(rider).strip().lower()
        for u in users:
            if u['id'].lower() == key or u.get('name', '').lower() == key:
                return u
        raise JustRideError(f'no rider "{rider}"; riders: ' + ', '.join(f'{u["name"]} ({u["id"]})' for u in users))

    def summaries(self, uid):
        return read_csv(self.dev.text(f'/rides/{uid}/summary.csv'))

    # -- tools --
    def list_riders(self):
        return {'riders': self.riders(),
                'note': 'ftp_known false = FTP is a default guess (no ramp test yet). max_erg_w caps any ERG target.'}

    def ride_history(self, rider=None, since=None, until=None, limit=50):
        u = self.rider(rider)
        rows = self.summaries(u['id'])
        lo = parse_day(since, 'since') if since else None
        hi = parse_day(until, 'until') + timedelta(days=1) if until else None
        out = []
        for r in rows:
            t = parse_utc(r.get('start_utc'))
            if (lo or hi) and (t is None or t.year < 2020 or (lo and t < lo) or (hi and t >= hi)):
                continue
            out.append(r)
        out.sort(key=lambda r: r.get('start_utc') or '', reverse=True)
        limit = max(1, min(int(limit), 500))
        return {'rider': u['name'], 'rider_id': u['id'], 'total_matching': len(out), 'rides': out[:limit],
                'columns': 'duration_s/moving_s seconds; ftp_w = FTP at the time; if = intensity factor; '
                           'tss = training stress; kj = work; distance_km virtual; start_utc ISO UTC'}

    def power_bests(self, rider=None, days=90):
        u = self.rider(rider)
        days = max(1, min(int(days), 3650))
        start = {r['ride_id']: parse_utc(r.get('start_utc')) for r in self.summaries(u['id'])}
        cutoff = datetime.now(timezone.utc) - timedelta(days=days)
        rows = read_csv(self.dev.text(f'/rides/{u["id"]}/bests.csv'))
        recent, all_time = {}, {}
        for r in rows:
            d, w = r.get('duration_s'), r.get('watts')
            if not isinstance(d, int) or not isinstance(w, (int, float)) or w <= 0:
                continue
            entry = {'watts': w, 'ride_id': r['ride_id']}
            if d not in all_time or w > all_time[d]['watts']:
                all_time[d] = entry
            t = start.get(r['ride_id'])
            if t and t >= cutoff and (d not in recent or w > recent[d]['watts']):
                recent[d] = entry
        fmt = lambda m: [{'duration_s': d, **m[d]} for d in sorted(m)]
        out = {'rider': u['name'], 'ftp_w': u.get('ftp_w'), 'weight_kg': u.get('weight_kg'),
               f'last_{days}_days': fmt(recent), 'all_time': fmt(all_time)}
        if 1200 in recent:
            out['ftp_estimate_w'] = round(recent[1200]['watts'] * 0.95)
            out['ftp_estimate_note'] = '95 % of the best 20 min in the window (the device offers this too)'
        return out

    def ride_detail(self, ride_id, rider=None, step_s=10):
        if not re.fullmatch(r'[0-9A-Za-z_-]{1,40}', ride_id or ''):
            raise JustRideError('ride_id looks like 2026-10-20_1730 (from ride_history)')
        u = self.rider(rider)
        text = self.dev.text(f'/rides/{u["id"]}/{ride_id}.csv')
        if text is None:
            raise JustRideError(f'no ride {ride_id} for {u["name"]}')
        rows = read_csv(text)
        summary = next((r for r in self.summaries(u['id']) if r['ride_id'] == ride_id), None)
        ftp = (summary or {}).get('ftp_w') or u.get('ftp_w')
        max_hr = (summary or {}).get('user_max_hr') or u.get('max_hr')
        step_s = max(1, min(int(step_s), 300))

        def mean(xs):
            xs = [x for x in xs if isinstance(x, (int, float))]
            return round(sum(xs) / len(xs), 1) if xs else None

        series = []
        for i in range(0, len(rows), step_s):
            chunk = rows[i:i + step_s]
            series.append({'t_s': chunk[0].get('t_s'), 'power_w': mean(r.get('power_w') for r in chunk),
                           'cadence_rpm': mean(r.get('cadence_rpm') for r in chunk),
                           'hr_bpm': mean(r.get('hr_bpm') for r in chunk),
                           'target_w': mean(r.get('target_w') for r in chunk),
                           'grade_pct': mean(r.get('grade_pct') for r in chunk)})
        pz = [0] * len(POWER_ZONES)
        hz = [0] * len(HR_ZONES)
        for r in rows:
            p, h = r.get('power_w'), r.get('hr_bpm')
            if ftp and isinstance(p, (int, float)):
                pz[next(i for i, (_, hi) in enumerate(POWER_ZONES) if p / ftp < hi)] += 1
            if max_hr and isinstance(h, (int, float)) and h > 0:
                pct10 = int(h * 1000 // max_hr)  # as metrics_hr_zone
                hz[min(len(HR_ZONES) - 1, 0 if pct10 < 600 else (pct10 - 500) // 100)] += 1
        intervals = {}
        for r in rows:
            k = r.get('interval_idx')
            if isinstance(k, int) and k > 0:
                intervals.setdefault(k, []).append(r)
        ivl = [{'interval': k, 'duration_s': len(v), 'avg_power_w': mean(r.get('power_w') for r in v),
                'avg_target_w': mean(r.get('target_w') for r in v), 'avg_hr_bpm': mean(r.get('hr_bpm') for r in v),
                'avg_cadence_rpm': mean(r.get('cadence_rpm') for r in v)} for k, v in sorted(intervals.items())]
        return {'rider': u['name'], 'ride_id': ride_id, 'summary': summary, 'ftp_w': ftp, 'max_hr': max_hr,
                'seconds': len(rows), 'step_s': step_s,
                'power_zones_s': {name: s for (name, _), s in zip(POWER_ZONES, pz)},
                'hr_zones_s': {name: s for (name, _, _), s in zip(HR_ZONES, hz)},
                'intervals': ivl, 'series': series,
                'note': 'series values are means over step_s seconds; null = sensor data unknown'}

    def training_load(self, rider=None, days=28):
        """Same model as the web app (core.js pmc): CTL 42 d, ATL 7 d, TSB = yesterday's CTL - ATL."""
        u = self.rider(rider)
        days = max(1, min(int(days), 365))
        by_day = {}
        for r in self.summaries(u['id']):
            if dated(r) and isinstance(r.get('tss'), (int, float)):
                k = r['start_utc'][:10]
                by_day[k] = by_day.get(k, 0) + r['tss']
        if not by_day:
            return {'rider': u['name'], 'note': 'no dated rides yet'}
        d = datetime.strptime(min(by_day), '%Y-%m-%d').date()
        today = datetime.now(timezone.utc).date()
        ctl = atl = 0.0
        out = []
        while d <= today:
            k = d.isoformat()
            tss = by_day.get(k, 0)
            tsb = ctl - atl
            ctl += (tss - ctl) / 42
            atl += (tss - atl) / 7
            out.append({'day': k, 'tss': round(tss, 1), 'fitness_ctl': round(ctl, 1), 'fatigue_atl': round(atl, 1),
                        'form_tsb': round(tsb, 1)})
            d += timedelta(days=1)
        tsb = out[-1]['form_tsb']
        label = ('Very fresh' if tsb > 15 else 'Fresh' if tsb > 5 else 'Neutral' if tsb > -10
                 else 'Productive load' if tsb > -30 else 'Overreaching')
        return {'rider': u['name'], 'today': out[-1], 'form': label,
                'tss_last_7_days': round(sum(x['tss'] for x in out[-7:]), 1),
                'daily': out[-days:], 'note': 'days are UTC dates; the web app uses the same model'}

    def list_workouts(self):
        files = self.dev.json('/api/workouts') or []
        zwo = sorted((f for f in files if f['name'].lower().endswith('.zwo')), key=lambda f: f['name'])
        routes = sorted((f for f in files if f['name'].lower().endswith('.route')), key=lambda f: f['name'])
        return {'workouts': [f['name'] for f in zwo], 'routes': [f['name'] for f in routes],
                'note': '"Plan <date> ..." workouts belong to the training plan (the web app rewrites them).'}

    def get_workout(self, name):
        name = self._file_name(name, check_new=False)
        xml = self.dev.text(f'/workouts/{name}')
        if xml is None:
            raise JustRideError(f'no workout {name}; see list_workouts')
        return {'file': name, 'device_view': self.zwo_check(xml.encode()), 'zwo': xml}

    def get_plan(self, rider=None):
        u = self.rider(rider)
        plan = self.dev.json(f'/plans/{u["id"]}.json')
        if plan is None:
            return {'rider': u['name'], 'plan': None, 'note': 'no plan; the web app (Plan view) creates one'}
        return {'rider': u['name'], 'plan': plan,
                'note': 'read-only here: the web app owns the plan and rewrites its "Plan ..." workouts'}

    # -- workouts --
    @staticmethod
    def _file_name(name, check_new=True):
        base = re.sub(r'\.zwo$', '', str(name or '').strip(), flags=re.I)
        if not re.fullmatch(r'[A-Za-z0-9 ._-]{1,56}', base) or base.startswith('.') or '..' in base:
            raise JustRideError('workout name: 1-56 characters, letters, digits, space . _ - only')
        if check_new and base.lower().startswith('plan '):
            raise JustRideError('names starting with "Plan " belong to the training plan; pick another name')
        return base + '.zwo'

    def zwo_check(self, data):
        exe = self._zwo_check_exe()
        r = subprocess.run([exe, '-'], input=data, capture_output=True, timeout=10)
        if r.returncode not in (0, 1):
            raise JustRideError(f'zwo_check failed: {r.stderr.decode(errors="replace")[:200]}')
        return json.loads(r.stdout)

    def _zwo_check_exe(self):
        if self.zwo_check_path:
            return self.zwo_check_path
        srcs = [os.path.join(ROOT, p) for p in ('tools/zwo_check.c', 'components/workout/zwo.c',
                                                 'components/workout/workout.c', 'components/workout/include/workout.h')]
        exe = os.path.join(ROOT, 'build', 'zwo_check')
        if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(s) for s in srcs):
            os.makedirs(os.path.dirname(exe), exist_ok=True)
            cc = os.environ.get('CC', 'cc')
            r = subprocess.run([cc, '-O2', '-std=c11', '-I' + os.path.join(ROOT, 'components/workout/include'),
                                *srcs[:3], '-lm', '-o', exe], capture_output=True)
            if r.returncode:
                raise JustRideError('could not compile tools/zwo_check.c (needs a C compiler, e.g. Xcode command line '
                                    'tools): ' + r.stderr.decode(errors='replace')[:300])
        self.zwo_check_path = exe
        return exe

    def create_workout(self, name, blocks, description='', rider=None, replace=False):
        file = self._file_name(name)
        u = self.rider(rider)
        xml, n_seg, peak, title = build_zwo(name, blocks, description)
        data = xml.encode()
        if len(data) > ZWO_MAX_BYTES:
            raise JustRideError('workout too large')
        chk = self.zwo_check(data)
        if not chk.get('ok'):
            raise JustRideError(f'the device parser rejects this workout: {chk.get("error")}')
        if chk['segments'] != n_seg:
            raise JustRideError(f'the device would read {chk["segments"]} segments, expected {n_seg} '
                                f'(at most {MAX_SEGMENTS}; an IntervalsT repeat counts twice)')
        ftp, max_erg = u.get('ftp_w') or 0, u.get('max_erg_w') or 0
        if ftp and max_erg and round(peak * ftp) > max_erg:
            raise JustRideError(f'highest target {round(peak * 100)} % of FTP = {round(peak * ftp)} W for {u["name"]} '
                                f'(FTP {ftp} W) is above their Max ERG of {max_erg} W; lower it')
        exists = file in (f['name'] for f in (self.dev.json('/api/workouts') or []))
        if exists and not replace:
            raise JustRideError(f'{file} already exists; pass replace=true to overwrite it, or pick another name')
        self.dev.request(f'/workouts/{file}', data)
        return {'saved': file, 'replaced': exists, 'rider_checked': u['name'],
                'device_view': {**chk, 'duration_min': round(chk['total_s'] / 60, 1), 'if': chk['if_pm'] / 1000,
                                'tss': chk['tss10'] / 10, 'avg_w': round(chk['avg_pm'] * ftp / 1000) if ftp else None,
                                'max_w': round(peak * ftp) if ftp else None},
                'on_device': f'Workouts -> {file[:-4]}'}


# ---- .zwo writer (mirrors toZwo in www/stats/workouts.js) --------------------------------

def _f(b, key, lo, hi, default=None, integer=False):
    v = b.get(key, default)
    if v is None:
        raise JustRideError(f'{b.get("type")} block needs "{key}"')
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not lo <= v <= hi:
        raise JustRideError(f'{b.get("type")} block: "{key}" must be a number from {lo} to {hi}, got {v!r}')
    if integer and v != int(v):
        raise JustRideError(f'{b.get("type")} block: "{key}" must be a whole number')
    return int(v) if integer else v


def _p(v):
    s = f'{round(v * 1000) / 1000:.3f}'.rstrip('0')
    return s + '0' if s.endswith('.') else s


def _cad(b, key='cadence', attr='Cadence'):
    return f' {attr}="{_f(b, key, 30, 150, integer=True)}"' if b.get(key) else ''


def build_zwo(name, blocks, description=''):
    """blocks -> (xml, expected segment count, highest power fraction, name)."""
    title = str(name or '').strip()[:47]
    if not isinstance(blocks, list) or not blocks:
        raise JustRideError('blocks must be a non-empty list')
    els, n_seg, peak = [], 0, 0.0
    for i, b in enumerate(blocks):
        if not isinstance(b, dict):
            raise JustRideError(f'block {i + 1} must be an object')
        t = b.get('type')
        dur = lambda k='duration_s': _f(b, k, 1, MAX_DUR_S, integer=True)
        pw = lambda k, d=None: _f(b, k, 0, 3.0, d)
        if t in ('warmup', 'ramp', 'cooldown'):
            tag = {'warmup': 'Warmup', 'ramp': 'Ramp', 'cooldown': 'Cooldown'}[t]
            a, z = pw('from'), pw('to')
            # PowerLow = start, PowerHigh = end for all three (zwo.c; Zwift's cool-downs too)
            els.append(f'<{tag} Duration="{dur()}" PowerLow="{_p(a)}" PowerHigh="{_p(z)}"{_cad(b)}/>')
            n_seg, peak = n_seg + 1, max(peak, a, z)
        elif t == 'steady':
            p = pw('power')
            els.append(f'<SteadyState Duration="{dur()}" Power="{_p(p)}"{_cad(b)}/>')
            n_seg, peak = n_seg + 1, max(peak, p)
        elif t == 'intervals':
            reps = _f(b, 'repeat', 1, 100, integer=True)
            on, off = pw('on_power'), pw('off_power')
            els.append(f'<IntervalsT Repeat="{reps}" OnDuration="{dur("on_s")}" OffDuration="{dur("off_s")}" '
                       f'OnPower="{_p(on)}" OffPower="{_p(off)}"{_cad(b)}{_cad(b, "cadence_rest", "CadenceResting")}/>')
            n_seg, peak = n_seg + 2 * reps, max(peak, on, off)
        elif t == 'free':
            els.append(f'<FreeRide Duration="{dur()}"/>')
            n_seg += 1
        elif t == 'adaptive':
            p = pw('power')
            attrs = f'Duration="{dur()}" Power="{_p(p)}"'
            if b.get('hold_average'):
                attrs += ' JrAvg="1"'
            hz = b.get('hr_zone')
            if hz is not None:
                z = _f(b, 'hr_zone', 1, 5, integer=True)
                attrs += f' JrHrLow="{_p(HR_ZONES[z - 1][1])}" JrHrHigh="{_p(HR_ZONES[z - 1][2])}"'
            if b.get('cadence_low') or b.get('cadence_high'):
                cl, ch = _f(b, 'cadence_low', 30, 150, integer=True), _f(b, 'cadence_high', 30, 150, integer=True)
                if cl >= ch:
                    raise JustRideError('adaptive block: cadence_low must be below cadence_high')
                attrs += f' JrCadLow="{cl}" JrCadHigh="{ch}"'
            mn = pw('min', 0) if b.get('min') else None
            mx = pw('max', 0) if b.get('max') else None
            if mn is not None:
                attrs += f' JrMin="{_p(mn)}"'
            if mx is not None:
                attrs += f' JrMax="{_p(mx)}"'
            if hz is None and not b.get('hold_average') and not b.get('cadence_low'):
                raise JustRideError('adaptive block needs hr_zone, hold_average or cadence_low/high; else use steady')
            # the device's default upper bound is power + 15 % (workout_add_goal)
            peak = max(peak, p, mx if mx is not None else p * 1.15)
            els.append(f'<SteadyState {attrs}/>')
            n_seg += 1
        else:
            raise JustRideError(f'block {i + 1}: unknown type {t!r} (warmup, ramp, steady, intervals, '
                                'adaptive, free, cooldown)')
    if n_seg > MAX_SEGMENTS:
        raise JustRideError(f'{n_seg} segments; the device keeps at most {MAX_SEGMENTS} '
                            '(an intervals repeat counts twice)')
    esc = lambda s: str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')
    body = '\n'.join(f'        {e}' for e in els)
    xml = f'''<?xml version="1.0" encoding="UTF-8"?>
<workout_file>
    <author>JustRide</author>
    <name>{esc(title)}</name>
    <description>{esc(str(description or '')[:2000])}</description>
    <sportType>bike</sportType>
    <workout>
{body}
    </workout>
</workout_file>
'''
    return xml, n_seg, peak, title


# ---- MCP wiring -----------------------------------------------------------------------

INSTRUCTIONS = """JustRide is a standalone indoor-cycling head unit (Wahoo KICKR Core, ERG and SIM modes).
Use these tools to look at a rider's training and to put new structured workouts on the device.
Power targets are fractions of the rider's FTP (0.75 = 75 %). Read list_riders first: an FTP with
ftp_known=false is only a guess, suggest the built-in ramp test (on the device's home screen) before
hard sessions. Never plan targets above the rider's Max ERG. The device must be on and idle."""

BLOCKS_DOC = """blocks: list of objects, in order. Power values are fractions of FTP (0.0-3.0).
- {"type":"warmup","duration_s":600,"from":0.45,"to":0.75}   (ramp/cooldown: same fields; cooldown from > to)
- {"type":"steady","duration_s":1200,"power":0.88,"cadence":90}   (cadence optional, rpm cue)
- {"type":"intervals","repeat":5,"on_s":240,"off_s":240,"on_power":1.15,"off_power":0.5,"cadence":95,"cadence_rest":85}
- {"type":"free","duration_s":300}   (no ERG target, rider chooses)
- {"type":"adaptive","duration_s":2400,"power":0.65,"hr_zone":2,"min":0.5,"max":0.75}
    The device steers the ERG target within [min,max] (default power -30 %/+15 %) to meet goals:
    hr_zone 1-5 (% of max HR: 1=50-60 Recovery, 2=60-70 Endurance, 3=70-80 Aerobic, 4=80-90 Threshold,
    5=90-100), hold_average:true (power is an average to hold), cadence_low/cadence_high (rpm).
At most 200 segments (an intervals repeat counts twice). Durations are whole seconds."""


def build_server(app):
    try:
        from mcp.server.mcpserver import MCPServer as Server  # mcp >= 2
        from mcp.server.mcpserver.exceptions import ToolError
    except ImportError:
        from mcp.server.fastmcp import FastMCP as Server       # mcp 1.x
        from mcp.server.fastmcp.exceptions import ToolError
    mcp = Server('justride', instructions=INSTRUCTIONS)

    # A JustRideError becomes a ToolError: the model sees an error result with the message
    def tool(fn=None, extra_doc=''):
        def wrap(fn):
            @functools.wraps(fn)
            def run(*a, **kw):
                try:
                    return fn(*a, **kw)
                except JustRideError as e:
                    raise ToolError(str(e)) from None
            return mcp.tool(description=(fn.__doc__ or '').strip() + extra_doc)(run)
        return wrap(fn) if fn else wrap

    @tool
    def list_riders() -> dict:
        """Riders on the head unit: id, name, active, ftp_w, ftp_known, max_erg_w, weight_kg, max_hr."""
        return app.list_riders()

    @tool
    def ride_history(rider: str = '', since: str = '', until: str = '', limit: int = 50) -> dict:
        """Ride summaries, newest first: duration, avg/NP power, IF, TSS, kJ, HR, cadence, distance, workout.
        rider: id or name (default: active rider). since/until: YYYY-MM-DD (UTC), inclusive."""
        return app.ride_history(rider or None, since or None, until or None, limit)

    @tool
    def power_bests(rider: str = '', days: int = 90) -> dict:
        """Best average power for 1 s ... 60 min over the last `days` days and all time, plus an FTP estimate."""
        return app.power_bests(rider or None, days)

    @tool
    def ride_detail(ride_id: str, rider: str = '', step_s: int = 10) -> dict:
        """One ride: summary, time in power and HR zones, per-interval averages, and a series of
        power/cadence/HR/target/grade averaged over step_s seconds (10 s default; use 30-60 for long rides)."""
        return app.ride_detail(ride_id, rider or None, step_s)

    @tool
    def training_load(rider: str = '', days: int = 28) -> dict:
        """Fitness (CTL), fatigue (ATL) and form (TSB) per day from ride TSS, with today's form label."""
        return app.training_load(rider or None, days)

    @tool
    def list_workouts() -> dict:
        """Workout (.zwo) and route files on the device's SD card."""
        return app.list_workouts()

    @tool
    def get_workout(name: str) -> dict:
        """One workout's .zwo XML and how the device reads it (segments, duration, IF, TSS)."""
        return app.get_workout(name)

    @tool
    def get_plan(rider: str = '') -> dict:
        """The rider's training plan as written by the web app (read-only)."""
        return app.get_plan(rider or None)

    @tool(extra_doc='\n' + BLOCKS_DOC)
    def create_workout(name: str, blocks: list[dict], description: str = '', rider: str = '',
                       replace: bool = False) -> dict:
        """Write a new structured workout to the device (Workouts list). Checked with the firmware's parser and
        against the rider's Max ERG (rider: whose FTP to check, default active). replace=true overwrites a file
        of the same name. name: letters, digits, space . _ - (not starting with "Plan ").
        """
        return app.create_workout(name, blocks, description, rider or None, replace)
    return mcp


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', default=os.environ.get('JUSTRIDE_DEVICE', 'trainer.local'),
                    help='head unit address (default trainer.local, or $JUSTRIDE_DEVICE)')
    a = ap.parse_args()
    build_server(JustRide(Device(a.device))).run()


if __name__ == '__main__':
    main()
