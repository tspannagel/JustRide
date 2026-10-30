// JustRide statistics: DuckDB-WASM over the CSV files the head unit writes.
// Each rider has a folder /rides/<id>/; /api/users lists the riders.
// summary.csv and bests.csv are loaded on every visit (small); a ride's
// per-second CSV only when it is opened. Plain HTTP on the LAN is not a secure
// context, so OPFS is unavailable; the browser's HTTP cache + ETags do the caching.
import * as duckdb from './vendor/duckdb-1.32.0/duckdb.mjs';
import uPlot from './vendor/uplot-1.6.32/uPlot.esm.js';

const $ = (id) => document.getElementById(id);
const status = (t) => { $('status').textContent = t; $('status').hidden = !t; };
let conn, user;  // user: {id, name}

// ---- DuckDB ---------------------------------------------------------------------

async function openDb() {
  const base = new URL('./vendor/duckdb-1.32.0/', import.meta.url);
  const worker = new Worker(new URL('duckdb-browser-eh.worker.js', base));
  const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), worker);
  await db.instantiate(new URL('duckdb-eh.wasm', base).href);
  conn = await db.connect();
  return db;
}

// Rows as plain objects; BigInt -> Number, DATE/TIMESTAMP -> Date
async function q(sql) {
  const res = await conn.query(sql);
  const times = res.schema.fields.filter((f) => /^(Date|Timestamp)/.test(String(f.type))).map((f) => f.name);
  return res.toArray().map((r) => {
    const o = r.toJSON();
    for (const k in o) if (typeof o[k] === 'bigint') o[k] = Number(o[k]);
    for (const k of times) if (o[k] != null) o[k] = new Date(o[k]);
    return o;
  });
}

// columns: "name TYPE, name TYPE, ..." (names unquoted; quoted here where SQL needs it).
// url null or a missing file -> empty table.
async function loadCsv(db, table, url, columns) {
  const cols = columns.split(',').map((c) => c.trim().split(/\s+/));
  const res = url ? await fetch(url, { cache: 'no-cache' }) : null;
  const text = res?.ok ? await res.text() : '';
  await conn.query(`DROP TABLE IF EXISTS ${table}`);
  if (!text.trim()) {
    await conn.query(`CREATE TABLE ${table} (${cols.map(([n, t]) => `"${n}" ${t}`).join(', ')})`);
    return 0;
  }
  await db.registerFileText(`${table}.csv`, text);
  const spec = cols.map(([n, t]) => `'${n}': '${t}'`).join(', ');
  await conn.query(`CREATE TABLE ${table} AS SELECT * FROM read_csv('${table}.csv', header = true, columns = {${spec}})`);
  return (await q(`SELECT count(*) AS n FROM ${table}`))[0].n;
}

const SUMMARY_COLS = 'ride_id VARCHAR, start_utc TIMESTAMP, duration_s INTEGER, moving_s INTEGER, workout VARCHAR, ' +
  'ftp_w INTEGER, avg_w INTEGER, np_w INTEGER, if DOUBLE, tss DOUBLE, kj INTEGER, avg_hr INTEGER, max_hr INTEGER, ' +
  'avg_cad DOUBLE, distance_km DOUBLE, fw VARCHAR, weight_kg INTEGER, user_max_hr INTEGER';
const BESTS_COLS = 'ride_id VARCHAR, duration_s INTEGER, watts INTEGER';
const RIDE_COLS = 't_s INTEGER, ts_utc TIMESTAMP, power_w INTEGER, cadence_rpm DOUBLE, speed_kmh DOUBLE, ' +
  'hr_bpm INTEGER, target_w INTEGER, grade_pct DOUBLE, gear INTEGER, interval_idx INTEGER, mode VARCHAR, lap INTEGER';

// "now - days" as a UTC TIMESTAMP literal. DuckDB-WASM ships without ICU, so
// now() is TIMESTAMPTZ and can't do interval math; the CSVs are UTC anyway.
const ago = (days) => `TIMESTAMP '${new Date(Date.now() - days * 86400e3).toISOString().slice(0, 19).replace('T', ' ')}'`;

// ---- Icons ------------------------------------------------------------------------
// Feather icons (https://feathericons.com, MIT licence), inlined: the app must work offline.

const ICONS = {
  activity: '<polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>',
  trending: '<polyline points="23 6 13.5 15.5 8.5 10.5 1 18"/><polyline points="17 6 23 6 23 12"/>',
  list: '<line x1="8" y1="6" x2="21" y2="6"/><line x1="8" y1="12" x2="21" y2="12"/><line x1="8" y1="18" x2="21" y2="18"/>' +
    '<line x1="3" y1="6" x2="3.01" y2="6"/><line x1="3" y1="12" x2="3.01" y2="12"/><line x1="3" y1="18" x2="3.01" y2="18"/>',
  database: '<ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M21 12c0 1.66-4 3-9 3s-9-1.34-9-3"/>' +
    '<path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"/>',
  user: '<path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/>',
  zap: '<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/>',
  heart: '<path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23' +
    'l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z"/>',
  clock: '<circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/>',
  calendar: '<rect x="3" y="4" width="18" height="18" rx="2" ry="2"/><line x1="16" y1="2" x2="16" y2="6"/>' +
    '<line x1="8" y1="2" x2="8" y2="6"/><line x1="3" y1="10" x2="21" y2="10"/>',
  bars: '<line x1="18" y1="20" x2="18" y2="10"/><line x1="12" y1="20" x2="12" y2="4"/><line x1="6" y1="20" x2="6" y2="14"/>',
  energy: '<path d="M5 18H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h3.19M15 6h2a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-3.19"/>' +
    '<line x1="23" y1="13" x2="23" y2="11"/><polyline points="11 6 7 12 13 12 9 18"/>',
  percent: '<line x1="19" y1="5" x2="5" y2="19"/><circle cx="6.5" cy="6.5" r="2.5"/><circle cx="17.5" cy="17.5" r="2.5"/>',
  cadence: '<polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/>' +
    '<path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/>',
  distance: '<polygon points="3 11 22 2 13 21 11 13 3 11"/>',
  award: '<circle cx="12" cy="8" r="7"/><polyline points="8.21 13.89 7 23 12 20 17 23 15.79 13.88"/>',
  back: '<line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>',
  target: '<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="2"/>',
  layers: '<polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/>',
  play: '<polygon points="5 3 19 12 5 21 5 3"/>',
};
// Metric colours, the same as on the device
const C = { power: '#f59e0b', hr: '#ef4444', cad: '#14b8a6', dist: '#8b5cf6', load: '#2563eb', time: '#64748b' };

const icon = (name, color) =>
  `<svg class="i" viewBox="0 0 24 24" aria-hidden="true"${color ? ` style="color:${color}"` : ''}>${ICONS[name]}</svg>`;

// ---- Rendering helpers ----------------------------------------------------------

const fmt = {
  dur: (s) => `${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`,
  date: (d) => new Date(d).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }),
  num: (v, d = 0) => (v == null || Number.isNaN(v) ? '-' : Number(v).toFixed(d)),
};

const esc = (v) => String(v ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// items: [label, value, icon, colour]
function cards(el, items) {
  el.innerHTML = items.map(([label, value, ic, color]) => `<div class="card">${ic ? `<span class="badge" style="--c:${color ||
    C.time}">${icon(ic)}</span>` : ''}<div><b>${esc(value)}</b><span>${esc(label)}</span></div></div>`).join('');
}

// cols: [key, heading, format(value, row), html]; html: format returns markup (escape it yourself)
function table(el, rows, cols, onClick) {
  if (!rows.length) { el.innerHTML = '<p>No data yet.</p>'; return; }
  cols = cols || Object.keys(rows[0]).map((k) => [k, k]);
  el.innerHTML = `<div class="scroll"><table class="${onClick ? 'clickable' : ''}"><thead><tr>${
    cols.map(([, h]) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>${
    rows.map((r, i) => `<tr data-i="${i}">${cols.map(([k, , f, html]) => `<td>${html ? f(r[k], r) : esc(f ? f(r[k], r) : r[k])}</td>`).join('')}</tr>`).join('')
  }</tbody></table></div>`;
  if (onClick) el.querySelectorAll('tbody tr').forEach((tr) => tr.onclick = () => onClick(rows[tr.dataset.i]));
}

const charts = {};
function chart(id, opts, data) {
  charts[id]?.destroy();
  const el = $(id);
  el.innerHTML = '';
  const width = Math.min(el.clientWidth || 800, 1100);
  charts[id] = new uPlot({ width, height: 300, ...opts }, data, el);
}

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const axes = () => {
  const stroke = css('--muted'), grid = { stroke: css('--line') };
  return [{ stroke, grid }, { stroke, grid }];
};

// ---- Overview (PMC) -------------------------------------------------------------

async function overview() {
  const t = (await q(`SELECT
      count(*) FILTER (WHERE start_utc > ${ago(7)}) AS rides7,
      coalesce(sum(tss) FILTER (WHERE start_utc > ${ago(7)}), 0) AS tss7,
      coalesce(sum(duration_s) FILTER (WHERE start_utc > ${ago(30)}), 0) AS secs30,
      coalesce(sum(kj) FILTER (WHERE start_utc > ${ago(30)}), 0) AS kj30,
      count(*) AS rides, coalesce(sum(duration_s), 0) AS secs FROM summary`))[0];
  cards($('totals'), [
    ['rides, last 7 days', t.rides7, 'activity', C.load], ['TSS, last 7 days', fmt.num(t.tss7), 'bars', C.load],
    ['hours, last 30 days', fmt.num(t.secs30 / 3600, 1), 'clock'], ['kJ, last 30 days', fmt.num(t.kj30), 'energy', C.power],
    ['rides total', t.rides, 'calendar'], ['hours total', fmt.num(t.secs / 3600, 1), 'clock'],
  ]);

  const daily = await q(`SELECT CAST(start_utc AS DATE) AS day, sum(tss) AS tss FROM summary GROUP BY 1 ORDER BY 1`);
  if (!daily.length) { $('pmc').innerHTML = '<p>No rides yet.</p>'; return; }
  // Daily series from the first ride until today, EWMA with 42 / 7 day constants
  const byDay = new Map(daily.map((d) => [new Date(d.day).toISOString().slice(0, 10), d.tss]));
  const xs = [], ctl = [], atl = [], tsb = [];
  let c = 0, a = 0;
  for (let d = new Date(daily[0].day); d <= new Date(); d.setUTCDate(d.getUTCDate() + 1)) {
    const tss = byDay.get(d.toISOString().slice(0, 10)) || 0;
    tsb.push(c - a); // form = yesterday's fitness - fatigue
    c += (tss - c) / 42;
    a += (tss - a) / 7;
    xs.push(d.getTime() / 1000); ctl.push(c); atl.push(a);
  }
  chart('pmc', {
    axes: axes(),
    series: [{}, { label: 'CTL', stroke: '#2563eb', width: 2 }, { label: 'ATL', stroke: '#db2777' },
      { label: 'TSB', stroke: '#16a34a', fill: 'rgba(22,163,74,0.1)' }],
  }, [xs, ctl, atl, tsb]);
}

// ---- Power curve ----------------------------------------------------------------

async function curve() {
  const rows = await q(`SELECT b.duration_s AS d, max(b.watts) AS best,
      max(b.watts) FILTER (WHERE s.start_utc > ${ago(90)}) AS recent
    FROM bests b LEFT JOIN summary s USING (ride_id) GROUP BY 1 ORDER BY 1`);
  if (!rows.length) { $('curve-chart').innerHTML = '<p>No rides yet.</p>'; $('ftp').innerHTML = ''; return; }
  const m20 = rows.find((r) => r.d === 1200);
  const kg = (await q(`SELECT weight_kg FROM summary WHERE weight_kg > 0 ORDER BY start_utc DESC LIMIT 1`))[0]?.weight_kg;
  const ftp = m20 && Math.round(m20.best * 0.95);
  $('ftp').innerHTML = icon('award', C.power) + ' ' + esc(m20 ?
    `Estimated FTP: ${ftp} W${kg ? ` (${fmt.num(ftp / kg, 2)} W/kg at ${kg} kg)` : ''} = 95 % of best 20 min, ${m20.best} W` :
    'Ride 20 minutes or longer for an FTP estimate.');
  chart('curve-chart', {
    scales: { x: { time: false, distr: 3 } },
    axes: [{ ...axes()[0], values: (u, v) => v.map((s) => (s < 60 ? `${s}s` : s < 3600 ? `${s / 60}m` : `${s / 3600}h`)) }, axes()[1]],
    series: [{ label: 'duration', value: (u, v) => (v == null ? '-' : `${v} s`) },
      { label: 'All time', stroke: '#2563eb', width: 2, points: { show: true } },
      { label: 'Last 90 days', stroke: '#f59e0b', width: 2, points: { show: true } }],
  }, [rows.map((r) => r.d), rows.map((r) => r.best), rows.map((r) => r.recent)]);
}

// ---- Rides ----------------------------------------------------------------------

async function rideList(db) {
  $('ride-detail').hidden = true;
  $('ride-list').hidden = false;
  const rows = await q(`SELECT * FROM summary ORDER BY start_utc DESC`);
  table($('ride-list'), rows, [
    ['start_utc', 'Date', fmt.date],
    ['workout', 'Workout', (v) => (v ? `${icon('target', C.load)} ${esc(v)}` : `${icon('distance', C.dist)} Free ride`), true], ['duration_s', 'Time', fmt.dur],
    ['avg_w', 'Avg W'], ['np_w', 'NP'], ['tss', 'TSS', (v) => fmt.num(v)], ['avg_hr', 'HR', (v) => v || '-'],
    ['distance_km', 'km', (v) => fmt.num(v, 1)],
  ], (r) => rideDetail(db, r));
}

async function rideDetail(db, s) {
  status('Loading ride...');
  await loadCsv(db, 'ride', `/rides/${user.id}/${encodeURIComponent(s.ride_id)}.csv`, RIDE_COLS);
  status('');
  $('ride-list').hidden = true;
  $('ride-detail').hidden = false;
  $('ride-title').textContent = `${fmt.date(s.start_utc)}  ${s.workout || 'Free ride'}`;
  // Aerobic decoupling: power per heartbeat, first half vs second half
  const drift = (await q(`WITH h AS (SELECT t_s < (SELECT max(t_s) / 2 FROM ride) AS first, power_w, hr_bpm
      FROM ride WHERE hr_bpm > 0 AND power_w > 0)
    SELECT (avg(power_w) FILTER (WHERE first) / avg(hr_bpm) FILTER (WHERE first)) /
           (avg(power_w) FILTER (WHERE NOT first) / avg(hr_bpm) FILTER (WHERE NOT first)) - 1 AS d FROM h`))[0]?.d;
  cards($('ride-cards'), [
    ['time', fmt.dur(s.duration_s), 'clock'], ['distance', `${fmt.num(s.distance_km, 1)} km`, 'distance', C.dist],
    ['avg power', `${s.avg_w} W`, 'zap', C.power], ['normalized power', `${s.np_w} W`, 'zap', C.power],
    ...(s.weight_kg ? [['avg W/kg', fmt.num(s.avg_w / s.weight_kg, 2), 'trending', C.power]] : []),
    ['TSS', fmt.num(s.tss), 'bars', C.load], ['IF', fmt.num(s.if, 2), 'percent', C.load],
    ['kJ', s.kj, 'energy', C.power], ['avg HR', s.avg_hr || '-', 'heart', C.hr],
    ['avg cadence', fmt.num(s.avg_cad), 'cadence', C.cad],
    ...(drift != null ? [['HR drift (Pa:HR)', `${fmt.num(drift * 100, 1)} %`, 'activity', C.hr]] : []),
  ]);

  const pts = await q(`SELECT t_s, power_w, hr_bpm, cadence_rpm, nullif(target_w, 0) AS target_w FROM ride ORDER BY t_s`);
  chart('ride-chart', {
    scales: { x: { time: false } },
    axes: [{ ...axes()[0], values: (u, v) => v.map((x) => fmt.dur(x)) }, axes()[1]],
    series: [{ label: 'time', value: (u, v) => (v == null ? '-' : fmt.dur(v)) },
      { label: 'Power', stroke: '#2563eb' }, { label: 'Target', stroke: '#94a3b8', dash: [6, 4] },
      { label: 'HR', stroke: '#dc2626' }, { label: 'Cadence', stroke: '#16a34a', show: false }],
  }, [pts.map((p) => p.t_s), pts.map((p) => p.power_w), pts.map((p) => p.target_w), pts.map((p) => p.hr_bpm),
    pts.map((p) => p.cadence_rpm)]);

  const ftp = s.ftp_w || 200;
  const zones = await q(`SELECT CASE
        WHEN power_w < ${ftp} * 0.55 THEN 'Z1 Recovery' WHEN power_w < ${ftp} * 0.75 THEN 'Z2 Endurance'
        WHEN power_w < ${ftp} * 0.90 THEN 'Z3 Tempo' WHEN power_w < ${ftp} * 1.05 THEN 'Z4 Threshold'
        WHEN power_w < ${ftp} * 1.20 THEN 'Z5 VO2max' WHEN power_w < ${ftp} * 1.50 THEN 'Z6 Anaerobic'
        ELSE 'Z7 Neuromuscular' END AS zone, count(*) AS secs
      FROM ride WHERE power_w IS NOT NULL GROUP BY 1 ORDER BY 1`);
  const total = zones.reduce((a, z) => a + z.secs, 0) || 1;
  table($('ride-zones'), zones, [['zone', `Zone (FTP ${ftp} W)`], ['secs', 'Time', fmt.dur],
    ['secs', 'Share', (v) => `${fmt.num((v / total) * 100)} %`]]);

  // Heart-rate zones as % of the rider's max HR at the time of the ride
  const hrmax = s.user_max_hr;
  const hrz = hrmax ? await q(`SELECT CASE
        WHEN hr_bpm < ${hrmax} * 0.60 THEN 'Z1 < 60 %' WHEN hr_bpm < ${hrmax} * 0.70 THEN 'Z2 60-70 %'
        WHEN hr_bpm < ${hrmax} * 0.80 THEN 'Z3 70-80 %' WHEN hr_bpm < ${hrmax} * 0.90 THEN 'Z4 80-90 %'
        ELSE 'Z5 > 90 %' END AS zone, count(*) AS secs
      FROM ride WHERE hr_bpm > 0 GROUP BY 1 ORDER BY 1`) : [];
  const hrTotal = hrz.reduce((a, z) => a + z.secs, 0) || 1;
  table($('ride-hr-zones'), hrz, [['zone', `Zone (max HR ${hrmax || '-'})`], ['secs', 'Time', fmt.dur],
    ['secs', 'Share', (v) => `${fmt.num((v / hrTotal) * 100)} %`]]);

  const intervals = await q(`SELECT interval_idx + 1 AS n, count(*) AS secs, round(avg(target_w)) AS target,
      round(avg(power_w)) AS actual, round(avg(cadence_rpm)) AS cad, round(avg(hr_bpm)) AS hr
    FROM ride WHERE mode = 'erg' GROUP BY interval_idx ORDER BY interval_idx`);
  table($('ride-intervals'), intervals, [['n', '#'], ['secs', 'Time', fmt.dur], ['target', 'Target W'],
    ['actual', 'Actual W'], ['actual', 'Compliance', (v, r) => (r.target ? `${fmt.num((v / r.target) * 100)} %` : '-')],
    ['cad', 'Cadence'], ['hr', 'HR', (v) => v || '-']]);
}

// ---- Workouts: add to calendar -----------------------------------------------------
// The device builds the .ics (GET /api/workout.ics): served as text/calendar,
// phones and computers open it straight in the calendar app.

const TIME_KEY = 'justride.trainingTime';

function savedTime() {
  try { return localStorage.getItem(TIME_KEY) || '18:00'; } catch { return '18:00'; }
}

async function workoutList() {
  const res = await fetch('/api/workouts', { cache: 'no-cache' });
  const list = res.ok ? await res.json() : [];
  list.forEach((w) => (w.label = w.name || w.title));
  list.sort((a, b) => a.label.localeCompare(b.label));
  if (!list.length) { $('workout-list').innerHTML = '<p>No workouts on the SD card (folder <code>/workouts</code>).</p>'; return; }
  table($('workout-list'), list, [
    ['label', 'Workout', (v) => `${icon('target', C.load)} ${esc(v)}`, true], ['duration_s', 'Time', fmt.dur],
    ['tss', 'TSS', (v) => fmt.num(v)], ['if', 'IF', (v) => fmt.num(v, 2)],
    ['file', '', () => `<button>${icon('calendar')} Add to calendar</button>`, true],
  ], addToCalendar);
}

function addToCalendar(w) {
  const d = new Date();
  if (d.getHours() * 60 + d.getMinutes() >= 20 * 60) d.setDate(d.getDate() + 1);  // late: suggest tomorrow
  $('cal-title').textContent = w.label;
  $('cal-info').textContent = `${fmt.dur(w.duration_s)} · TSS ${fmt.num(w.tss)} · IF ${fmt.num(w.if, 2)} · ` +
    `on the device: Workouts → ${w.title}`;
  $('cal-date').value = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  $('cal-time').value = savedTime();
  $('cal').onclose = () => {
    if ($('cal').returnValue !== 'add') return;
    const time = $('cal-time').value;
    try { localStorage.setItem(TIME_KEY, time); } catch { /* private mode: just don't remember */ }
    const start = Math.round(new Date(`${$('cal-date').value}T${time}`).getTime() / 1000);  // browser's local time
    // encodeURIComponent, not URLSearchParams: the device decodes %20, not "+"
    location.href = `/api/workout.ics?file=${encodeURIComponent(w.file)}&start=${start}&user=${encodeURIComponent(user.id)}`;
  };
  $('cal').returnValue = '';
  $('cal').showModal();
}

// ---- SQL console ----------------------------------------------------------------

async function runSql() {
  try {
    const rows = await q($('sql-text').value);
    const show = (v) => (v instanceof Date ? v.toISOString().slice(0, 19).replace('T', ' ') :
      v != null && typeof v === 'object' ? JSON.stringify(v) : v);
    table($('sql-out'), rows.slice(0, 500).map((r) => Object.fromEntries(Object.entries(r).map(([k, v]) => [k, show(v)]))));
  } catch (e) {
    $('sql-out').textContent = e.message;
  }
}

// ---- Main -----------------------------------------------------------------------

async function loadUser(db, u) {
  user = u;
  status('Loading rides...');
  const n = await loadCsv(db, 'summary', `/rides/${u.id}/summary.csv`, SUMMARY_COLS);
  await loadCsv(db, 'bests', `/rides/${u.id}/bests.csv`, BESTS_COLS);
  await loadCsv(db, 'ride', null, RIDE_COLS); // empty until a ride is opened
  status(n ? '' : `No rides recorded for ${u.name} yet.`);
}

async function main() {
  const db = await openDb();
  // Static icons in index.html: <el data-icon="name" data-color="power">
  document.querySelectorAll('[data-icon]').forEach((el) =>
    el.insertAdjacentHTML('afterbegin', icon(el.dataset.icon, C[el.dataset.color]) + ' '));
  const users = await (await fetch('/api/users', { cache: 'no-cache' })).json();
  if (!users.length) { status('No riders found (is the SD card in?)'); return; }
  const pick = $('user');
  pick.innerHTML = users.map((u, i) => `<option value="${i}">${u.name.replace(/[&<>"]/g, '')}</option>`).join('');
  pick.value = String(Math.max(0, users.findIndex((u) => u.active)));
  await loadUser(db, users[pick.value]);

  let current = 'overview';
  const views = { overview, curve, rides: () => rideList(db), workouts: workoutList, sql: async () => {} };
  const show = async (name) => {
    current = name;
    document.querySelectorAll('nav button').forEach((b) => b.classList.toggle('active', b.dataset.view === name));
    document.querySelectorAll('.view').forEach((v) => (v.hidden = v.id !== name));
    await views[name]();
  };
  document.querySelectorAll('nav button').forEach((b) => (b.onclick = () => show(b.dataset.view)));
  pick.onchange = async () => { await loadUser(db, users[pick.value]); await show(current); };
  $('ride-back').onclick = () => rideList(db);
  $('sql-run').onclick = runSql;
  await show('overview');
}

main().catch((e) => status(`Error: ${e.message}`));
