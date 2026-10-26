// JustRide statistics: DuckDB-WASM over the CSV files the head unit writes.
// summary.csv and bests.csv are loaded on every visit (small); a ride's
// per-second CSV only when it is opened. Plain HTTP on the LAN is not a secure
// context, so OPFS is unavailable; the browser's HTTP cache + ETags do the caching.
import * as duckdb from './vendor/duckdb-1.32.0/duckdb.mjs';
import uPlot from './vendor/uplot-1.6.32/uPlot.esm.js';

const $ = (id) => document.getElementById(id);
const status = (t) => { $('status').textContent = t; $('status').hidden = !t; };
let conn;

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
  'avg_cad DOUBLE, distance_km DOUBLE, fw VARCHAR';
const BESTS_COLS = 'ride_id VARCHAR, duration_s INTEGER, watts INTEGER';
const RIDE_COLS = 't_s INTEGER, ts_utc TIMESTAMP, power_w INTEGER, cadence_rpm DOUBLE, speed_kmh DOUBLE, ' +
  'hr_bpm INTEGER, target_w INTEGER, grade_pct DOUBLE, gear INTEGER, interval_idx INTEGER, mode VARCHAR, lap INTEGER';

// "now - days" as a UTC TIMESTAMP literal. DuckDB-WASM ships without ICU, so
// now() is TIMESTAMPTZ and can't do interval math; the CSVs are UTC anyway.
const ago = (days) => `TIMESTAMP '${new Date(Date.now() - days * 86400e3).toISOString().slice(0, 19).replace('T', ' ')}'`;

// ---- Rendering helpers ----------------------------------------------------------

const fmt = {
  dur: (s) => `${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`,
  date: (d) => new Date(d).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }),
  num: (v, d = 0) => (v == null || Number.isNaN(v) ? '-' : Number(v).toFixed(d)),
};

function cards(el, items) {
  el.innerHTML = items.map(([label, value]) => `<div class="card"><b>${value}</b><span>${label}</span></div>`).join('');
}

function table(el, rows, cols, onClick) {
  if (!rows.length) { el.innerHTML = '<p>No data yet.</p>'; return; }
  cols = cols || Object.keys(rows[0]).map((k) => [k, k]);
  const esc = (v) => String(v ?? '').replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
  el.innerHTML = `<div class="scroll"><table class="${onClick ? 'clickable' : ''}"><thead><tr>${
    cols.map(([, h]) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>${
    rows.map((r, i) => `<tr data-i="${i}">${cols.map(([k, , f]) => `<td>${esc(f ? f(r[k], r) : r[k])}</td>`).join('')}</tr>`).join('')
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
    ['rides, last 7 days', t.rides7], ['TSS, last 7 days', fmt.num(t.tss7)],
    ['hours, last 30 days', fmt.num(t.secs30 / 3600, 1)], ['kJ, last 30 days', fmt.num(t.kj30)],
    ['rides total', t.rides], ['hours total', fmt.num(t.secs / 3600, 1)],
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
  if (!rows.length) { $('curve-chart').innerHTML = '<p>No rides yet.</p>'; $('ftp').textContent = ''; return; }
  const m20 = rows.find((r) => r.d === 1200);
  $('ftp').textContent = m20 ? `Estimated FTP: ${Math.round(m20.best * 0.95)} W (95 % of best 20 min, ${m20.best} W)` :
    'Ride 20 minutes or longer for an FTP estimate.';
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
    ['start_utc', 'Date', fmt.date], ['workout', 'Workout', (v) => v || 'Free ride'], ['duration_s', 'Time', fmt.dur],
    ['avg_w', 'Avg W'], ['np_w', 'NP'], ['tss', 'TSS', (v) => fmt.num(v)], ['avg_hr', 'HR', (v) => v || '-'],
    ['distance_km', 'km', (v) => fmt.num(v, 1)],
  ], (r) => rideDetail(db, r));
}

async function rideDetail(db, s) {
  status('Loading ride...');
  await loadCsv(db, 'ride', `/rides/${encodeURIComponent(s.ride_id)}.csv`, RIDE_COLS);
  status('');
  $('ride-list').hidden = true;
  $('ride-detail').hidden = false;
  $('ride-title').textContent = `${fmt.date(s.start_utc)}  ${s.workout || 'Free ride'}`;
  cards($('ride-cards'), [
    ['time', fmt.dur(s.duration_s)], ['avg power', `${s.avg_w} W`], ['normalized power', `${s.np_w} W`],
    ['TSS', fmt.num(s.tss)], ['IF', fmt.num(s.if, 2)], ['kJ', s.kj], ['avg HR', s.avg_hr || '-'],
    ['avg cadence', fmt.num(s.avg_cad)],
  ]);

  // Aerobic decoupling: power per heartbeat, first half vs second half
  const drift = (await q(`WITH h AS (SELECT t_s < (SELECT max(t_s) / 2 FROM ride) AS first, power_w, hr_bpm
      FROM ride WHERE hr_bpm > 0 AND power_w > 0)
    SELECT (avg(power_w) FILTER (WHERE first) / avg(hr_bpm) FILTER (WHERE first)) /
           (avg(power_w) FILTER (WHERE NOT first) / avg(hr_bpm) FILTER (WHERE NOT first)) - 1 AS d FROM h`))[0]?.d;
  if (drift != null) cards($('ride-cards'), [...[...$('ride-cards').children].map((c) => [c.children[1].textContent, c.children[0].textContent]),
    ['HR drift (Pa:HR)', `${fmt.num(drift * 100, 1)} %`]]);

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

  const intervals = await q(`SELECT interval_idx + 1 AS n, count(*) AS secs, round(avg(target_w)) AS target,
      round(avg(power_w)) AS actual, round(avg(cadence_rpm)) AS cad, round(avg(hr_bpm)) AS hr
    FROM ride WHERE mode = 'erg' GROUP BY interval_idx ORDER BY interval_idx`);
  table($('ride-intervals'), intervals, [['n', '#'], ['secs', 'Time', fmt.dur], ['target', 'Target W'],
    ['actual', 'Actual W'], ['actual', 'Compliance', (v, r) => (r.target ? `${fmt.num((v / r.target) * 100)} %` : '-')],
    ['cad', 'Cadence'], ['hr', 'HR', (v) => v || '-']]);
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

async function main() {
  const db = await openDb();
  status('Loading rides...');
  const n = await loadCsv(db, 'summary', '/rides/summary.csv', SUMMARY_COLS);
  await loadCsv(db, 'bests', '/rides/bests.csv', BESTS_COLS);
  await loadCsv(db, 'ride', null, RIDE_COLS); // empty until a ride is opened
  status(n ? '' : 'No rides recorded yet.');

  const views = { overview, curve, rides: () => rideList(db), sql: async () => {} };
  const show = async (name) => {
    document.querySelectorAll('nav button').forEach((b) => b.classList.toggle('active', b.dataset.view === name));
    document.querySelectorAll('.view').forEach((v) => (v.hidden = v.id !== name));
    await views[name]();
  };
  document.querySelectorAll('nav button').forEach((b) => (b.onclick = () => show(b.dataset.view)));
  $('ride-back').onclick = () => rideList(db);
  $('sql-run').onclick = runSql;
  await show('overview');
}

main().catch((e) => status(`Error: ${e.message}`));
