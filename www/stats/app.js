// JustRide web app: start-up, navigation and the statistics views (Today, Rides, Power,
// Device). The CSV files the head unit writes are parsed in JS, so the app is usable
// within a second; DuckDB-WASM (34 MB) is only loaded when the SQL console is used.
import { $, $$, api, esc, fmt, icon, toast, tile, empty, chart, parseCsv, pmc, formState, dated, dayKey, todayKey,
  SUMMARY_T, BESTS_T, RIDE_T, ZONES, zoneOf, zoneColor, HR_ZONES, hrZoneOf } from './core.js';
import { initWorkouts, showWorkouts, GEN_TYPES } from './workouts.js';
import { initPlan, showPlan, autoSync, nextSession, readPlan } from './plan.js';
import { initRiders, showRiders } from './riders.js';
import { initRoutes, showRoutes } from './routes.js';

const S = { users: [], user: null, rides: [], bests: [], pmc: null, view: null, rideOpen: null, range: 90 };

// ---- Data -----------------------------------------------------------------------------

async function loadUser(u) {
  S.user = u;
  const [sum, best] = await Promise.all([api.text(`/rides/${u.id}/summary.csv`), api.text(`/rides/${u.id}/bests.csv`)]);
  // Older firmware could append a recovered ride twice: keep the first row per key
  S.rides = unique(parseCsv(sum, SUMMARY_T), (r) => r.ride_id).sort((a, b) => (b.start_utc || 0) - (a.start_utc || 0));
  S.bests = unique(parseCsv(best, BESTS_T), (b) => `${b.ride_id} ${b.duration_s}`);
  S.pmc = pmc(S.rides);
}

function unique(rows, key) {
  const seen = new Set();
  return rows.filter((r) => !seen.has(key(r)) && seen.add(key(r)));
}

// What the workout generator adapts to
function ctx() {
  const ftp = S.user.ftp_w || S.rides.find((r) => r.ftp_w)?.ftp_w || null;
  const tsb = S.pmc?.tsb.length ? S.pmc.tsb[S.pmc.tsb.length - 1] : null;
  return { userId: S.user.id, rides: S.rides, ftp, ftpKnown: S.user.ftp_known !== false, tsb, best: bestsSince(90),
    weight: S.user.weight_kg || S.rides.find((r) => r.weight_kg)?.weight_kg, maxHr: S.user.max_hr };
}

// Best watts per duration for rides in the last `days` (null: all time)
function bestsSince(days) {
  const cutoff = days == null ? null : Date.now() - days * 86400e3;
  const ok = new Set(S.rides.filter((r) => cutoff == null || (dated(r) && r.start_utc.getTime() > cutoff)).map((r) => r.ride_id));
  const out = {};
  for (const b of S.bests) if (ok.has(b.ride_id) && b.watts > (out[b.duration_s] || 0)) out[b.duration_s] = b.watts;
  return out;
}

const riderLabel = () => (S.users.length > 1 ? S.user.name.replace(/[^A-Za-z0-9 ._-]/g, '') : '');

// ---- Navigation -------------------------------------------------------------------------

const VIEWS = {
  today: { title: 'Today', render: today },
  rides: { title: 'Rides', render: rides },
  power: { title: 'Power', render: power },
  workouts: { title: 'Workouts', render: () => showWorkouts() },
  routes: { title: 'Routes', render: () => showRoutes() },
  plan: { title: 'Plan', render: () => showPlan(S.user, S.rides, riderLabel()) },
  riders: { title: 'Riders', render: () => showRiders(S.users) },
  device: { title: 'Device', render: device },
};

async function go(name) {
  const [view, arg] = name.split('/');
  if (!VIEWS[view]) return go('today');
  S.view = view;
  $$('.nav a').forEach((a) => a.classList.toggle('active', a.dataset.view === view));
  $$('.view').forEach((v) => (v.hidden = v.id !== view));
  document.title = `${VIEWS[view].title} · JustRide`;
  try {
    if (view === 'rides' && arg) await rideDetail(decodeURIComponent(arg));
    else await VIEWS[view].render();
  } catch (err) {
    console.error(err);
    toast(err.message, 'bad');
  }
}
window.addEventListener('hashchange', () => go(location.hash.slice(1) || 'today'));

// ---- Today ------------------------------------------------------------------------------

async function today() {
  const P = S.pmc, n = P.x.length;
  const ctl = n ? P.ctl[n - 1] : null, atl = n ? P.atl[n - 1] : null, tsb = n ? P.tsb[n - 1] : null;
  const f = formState(tsb);
  const hour = new Date().getHours();
  $('#hello').textContent = `${hour < 11 ? 'Good morning' : hour < 18 ? 'Hi' : 'Good evening'}, ${S.user.name}`;
  $('#form-hero').innerHTML = `
    <div class="hero-main"><span class="eyebrow">Form</span>
      <div class="hero-num ${f.tone}">${tsb == null ? '–' : (tsb > 0 ? '+' : '') + fmt.num(tsb)}</div>
      <span class="pill ${f.tone}">${esc(f.label)}</span></div>
    <div class="hero-side">
      <div><span class="eyebrow">Fitness</span><b>${fmt.num(ctl)}</b><small>CTL · 42-day load</small></div>
      <div><span class="eyebrow">Fatigue</span><b>${fmt.num(atl)}</b><small>ATL · 7-day load</small></div>
    </div>`;

  const week = S.rides.filter((r) => dated(r) && r.start_utc > Date.now() - 7 * 86400e3);
  const sum = (a, k) => a.reduce((x, r) => x + (r[k] || 0), 0);
  $('#week-tiles').innerHTML = [
    tile('Rides', week.length, '', '', 'activity'),
    tile('Time', fmt.hm(sum(week, 'duration_s')), '', '', 'clock'),
    tile('Load', fmt.num(sum(week, 'tss')), 'TSS', 'load', 'bars'),
    tile('Work', fmt.num(sum(week, 'kj')), 'kJ', 'power', 'energy'),
    tile('Distance', fmt.num(sum(week, 'distance_km'), 1), 'km', 'dist', 'distance'),
  ].join('');

  // Next planned session
  let plan = null;
  try { plan = await readPlan(S.user.id); } catch { /* no plan */ }
  const next = nextSession(plan, S.rides, todayKey());
  // Until the first test the FTP is a guess, and every target depends on it
  const untested = S.user.ftp_known === false && next?.type !== 'ramptest';
  $('#next-card').innerHTML = untested ? `<span class="eyebrow">${icon('zap')} FTP not tested yet</span>
      <b>Start with a ramp test · about 20 min</b>
      <span class="muted small">On the device: Workouts → FTP ramp test. Workouts and plans scale with your FTP; until then it's a guess (${S.user.ftp_w} W).</span>
      <a class="btn ghost" href="#riders">Enter a known FTP instead</a>`
    : next ? `<span class="eyebrow">${icon('calendar')} Next in your plan</span>
      <b>${esc(GEN_TYPES[next.type].label)} · ${next.minutes} min</b>
      <span class="muted small">${fmt.date(new Date(next.date + 'T12:00:00'))} · on the device under Workouts</span>
      <a class="btn ghost" href="#plan">Open plan</a>`
    : `<span class="eyebrow">${icon('sparkles')} No plan yet</span><b>Train with a plan</b>
      <span class="muted small">Pick a goal and your days; the next week's workouts appear on the device, adapted to your form.</span>
      <a class="btn primary" href="#plan">Make a plan</a>`;

  renderPmc();
  const recent = S.rides.slice(0, 4);
  $('#recent').innerHTML = recent.length ? recent.map(rideRow).join('') :
    empty('No rides yet', 'Ride with the head unit and your rides show up here.', 'activity');
}

function renderPmc() {
  const P = S.pmc;
  const el = $('#pmc');
  $$('#pmc-range button').forEach((b) => b.classList.toggle('active', Number(b.dataset.days) === S.range));
  if (!P.x.length) { el.innerHTML = empty('Nothing to chart yet', 'Fitness, fatigue and form appear after your first dated ride.', 'trending'); return; }
  const from = S.range ? Math.max(0, P.x.length - S.range) : 0;
  const cut = (a) => a.slice(from);
  chart(el, {
    data: [cut(P.x), cut(P.ctl), cut(P.atl), cut(P.tsb)],
    series: [
      { label: 'Fitness', stroke: '--s1', fill: true, value: (u, v) => fmt.num(v) },
      { label: 'Fatigue', stroke: '--s2', value: (u, v) => fmt.num(v) },
      { label: 'Form', stroke: '--s3', dash: [5, 4], value: (u, v) => fmt.num(v) },
    ],
    height: 240,
  });
}

function rideRow(r) {
  const free = !r.workout;
  return `<a class="ride-row" href="#rides/${encodeURIComponent(r.ride_id)}">
    <span class="ride-ic ${free ? 'dist' : 'load'}">${icon(free ? 'distance' : 'target')}</span>
    <span class="ride-name"><b>${esc(r.workout || 'Free ride')}</b><small>${fmt.date(r.start_utc)} ${fmt.time(r.start_utc)}</small></span>
    <span class="ride-stat"><b>${fmt.dur(r.duration_s)}</b><small>time</small></span>
    <span class="ride-stat"><b>${fmt.num(r.np_w)}</b><small>NP W</small></span>
    <span class="ride-stat"><b>${fmt.num(r.tss)}</b><small>TSS</small></span>
    <span class="ride-stat hide-s"><b>${r.avg_hr || '–'}</b><small>HR</small></span></a>`;
}

// ---- Rides ------------------------------------------------------------------------------

async function rides() {
  $('#ride-detail').hidden = true;
  $('#ride-list-wrap').hidden = false;
  const months = new Map();
  for (const r of S.rides) {
    const k = dated(r) ? r.start_utc.toLocaleDateString(undefined, { month: 'long', year: 'numeric' }) : 'Before the clock was set';
    if (!months.has(k)) months.set(k, []);
    months.get(k).push(r);
  }
  $('#ride-list').innerHTML = S.rides.length ? [...months].map(([m, rs]) => `<h3 class="group">${esc(m)}
      <small>${rs.length} ride${rs.length === 1 ? '' : 's'} · ${fmt.hm(rs.reduce((a, r) => a + (r.duration_s || 0), 0))} · ${fmt.num(rs.reduce((a, r) => a + (r.tss || 0), 0))} TSS</small></h3>
      ${rs.map(rideRow).join('')}`).join('') :
    empty('No rides yet', 'Your rides appear here after you ride with the head unit.', 'activity');
}

async function rideDetail(id) {
  const s = S.rides.find((r) => r.ride_id === id);
  if (!s) return go('rides');
  $('#ride-list-wrap').hidden = true;
  $('#ride-detail').hidden = false;
  $('#ride-title').textContent = s.workout || 'Free ride';
  $('#ride-when').textContent = `${fmt.date(s.start_utc)} ${fmt.time(s.start_utc)}`;
  $('#ride-dl').href = `/rides/${S.user.id}/${encodeURIComponent(id)}.fit`;
  $('#ride-charts').innerHTML = '<div class="skeleton" style="height:300px"></div>';
  const pts = parseCsv(await api.text(`/rides/${S.user.id}/${encodeURIComponent(id)}.csv`), RIDE_T);

  // Aerobic decoupling: power per heartbeat, first half vs second half
  const half = pts.length / 2;
  const ef = (a) => { const v = a.filter((p) => p.hr_bpm > 0 && p.power_w > 0); return v.length > 60 ? v.reduce((x, p) => x + p.power_w, 0) / v.reduce((x, p) => x + p.hr_bpm, 0) : null; };
  const e1 = ef(pts.slice(0, half)), e2 = ef(pts.slice(half));
  const drift = e1 && e2 ? (e1 / e2 - 1) * 100 : null;
  $('#ride-tiles').innerHTML = [
    tile('Time', fmt.dur(s.duration_s), '', '', 'clock'),
    tile('Distance', fmt.num(s.distance_km, 1), 'km', 'dist', 'distance'),
    tile('Avg power', fmt.num(s.avg_w), 'W', 'power', 'zap'),
    tile('Normalized', fmt.num(s.np_w), 'W', 'power', 'zap'),
    ...(s.weight_kg ? [tile('Avg W/kg', fmt.num(s.avg_w / s.weight_kg, 2), '', 'power', 'trending')] : []),
    tile('Load', fmt.num(s.tss), 'TSS', 'load', 'bars'),
    tile('Intensity', fmt.num(s.if, 2), 'IF', 'load', 'target'),
    tile('Work', fmt.num(s.kj), 'kJ', 'power', 'energy'),
    tile('Avg HR', s.avg_hr || '–', s.avg_hr ? 'bpm' : '', 'hr', 'heart'),
    tile('Cadence', fmt.num(s.avg_cad), 'rpm', 'cad', 'cadence'),
    ...(drift != null ? [tile('HR drift', fmt.num(drift, 1), '%', 'hr', 'activity')] : []),
  ].join('');

  if (!pts.length) { $('#ride-charts').innerHTML = empty('No per-second data', 'The ride file is missing on the SD card.', 'file'); return; }
  const x = pts.map((p) => p.t_s);
  const hasTarget = pts.some((p) => p.target_w > 0), hasHr = pts.some((p) => p.hr_bpm > 0);
  $('#ride-charts').innerHTML = `<div class="chart-card"><h3>${icon('zap')} Power</h3><div id="c-power"></div></div>
    ${hasHr ? `<div class="chart-card"><h3>${icon('heart')} Heart rate</h3><div id="c-hr"></div></div>` : ''}
    <div class="chart-card"><h3>${icon('cadence')} Cadence</h3><div id="c-cad"></div></div>`;
  const common = { xTime: false, xFmt: fmt.dur, sync: 'ride' };
  chart($('#c-power'), { ...common, height: 240, data: [x, pts.map((p) => p.power_w), ...(hasTarget ? [pts.map((p) => p.target_w || null)] : [])],
    series: [{ label: 'Power', stroke: '--c-power', fill: true, value: (u, v) => (v == null ? '–' : `${v} W`) },
      ...(hasTarget ? [{ label: 'Target', stroke: '--text-3', dash: [6, 4], width: 1.5, value: (u, v) => (v == null ? '–' : `${v} W`) }] : [])] });
  if (hasHr) chart($('#c-hr'), { ...common, height: 150, data: [x, pts.map((p) => p.hr_bpm || null)],
    series: [{ label: 'Heart rate', stroke: '--c-hr', value: (u, v) => (v == null ? '–' : `${v} bpm`) }] });
  chart($('#c-cad'), { ...common, height: 130, data: [x, pts.map((p) => p.cadence_rpm)],
    series: [{ label: 'Cadence', stroke: '--c-cad', value: (u, v) => (v == null ? '–' : `${Math.round(v)} rpm`) }] });

  // Time in zones
  const ftp = s.ftp_w || 200, pz = new Array(7).fill(0);
  for (const p of pts) if (p.power_w != null) pz[zoneOf(p.power_w / ftp)]++;
  $('#ride-zones').innerHTML = zoneBars(pz, ZONES.map((z, i) => `Z${i + 1} ${z.name}`), (i) => zoneColor(i));
  const hrmax = s.user_max_hr, hz = new Array(5).fill(0);
  if (hrmax) for (const p of pts) if (p.hr_bpm > 0) hz[hrZoneOf(p.hr_bpm, hrmax)]++;
  const hzLabel = (z, i) => `Z${i + 1} ${i === 0 ? '< 60' : i === 4 ? '> 90' : `${z.lo * 100}–${z.hi * 100}`} %`;
  $('#ride-hr-zones').innerHTML = hz.some(Boolean) ? zoneBars(hz, HR_ZONES.map(hzLabel),
    (i) => `var(--h${i + 1})`) : '<p class="muted small">No heart-rate data.</p>';
  $('#ride-zones-sub').textContent = `FTP ${ftp} W at the time of the ride`;
  $('#ride-hr-sub').textContent = hrmax ? `% of max HR ${hrmax}` : '';

  // Intervals (ERG)
  const iv = new Map();
  for (const p of pts) {
    if (p.mode !== 'erg') continue;
    const o = iv.get(p.interval_idx) || { n: p.interval_idx + 1, secs: 0, t: 0, p: 0, pc: 0, c: 0, cc: 0, h: 0, hc: 0 };
    o.secs++; o.t += p.target_w || 0;
    if (p.power_w != null) { o.p += p.power_w; o.pc++; }
    if (p.cadence_rpm != null) { o.c += p.cadence_rpm; o.cc++; }
    if (p.hr_bpm > 0) { o.h += p.hr_bpm; o.hc++; }
    iv.set(p.interval_idx, o);
  }
  const rows = [...iv.values()];
  $('#ride-intervals-wrap').hidden = !rows.length;
  $('#ride-intervals').innerHTML = rows.length ? `<table><thead><tr><th>#</th><th>Time</th><th>Target</th><th>Actual</th><th>Compliance</th><th>Cadence</th><th>HR</th></tr></thead>
    <tbody>${rows.map((r) => { const t = r.t / r.secs, a = r.pc ? r.p / r.pc : null, c = a && t ? (a / t) * 100 : null;
      return `<tr><td>${r.n}</td><td>${fmt.dur(r.secs)}</td><td>${fmt.num(t)} W</td><td>${fmt.num(a)} W</td>
      <td><span class="pill ${c == null ? 'muted' : Math.abs(c - 100) <= 5 ? 'good' : Math.abs(c - 100) <= 10 ? 'warn' : 'bad'}">${c == null ? '–' : fmt.num(c) + ' %'}</span></td>
      <td>${r.cc ? fmt.num(r.c / r.cc) : '–'}</td><td>${r.hc ? fmt.num(r.h / r.hc) : '–'}</td></tr>`; }).join('')}</tbody></table>` : '';
}

function zoneBars(secs, labels, color) {
  const total = secs.reduce((a, b) => a + b, 0) || 1, max = Math.max(...secs, 1);
  return `<div class="zones">${secs.map((v, i) => `<div class="zone-row" title="${esc(labels[i])}: ${fmt.dur(v)}">
    <span class="zone-label">${esc(labels[i])}</span>
    <span class="zone-track"><span class="zone-fill" style="--w:${(v / max) * 100}%;--zc:${color(i)}"></span></span>
    <span class="zone-val">${fmt.dur(v)}<small>${fmt.num((v / total) * 100)} %</small></span></div>`).join('')}</div>`;
}

// ---- Power -------------------------------------------------------------------------------

const KEY_BESTS = [[5, '5 s', 'Sprint'], [60, '1 min', 'Anaerobic'], [300, '5 min', 'VO2max'], [1200, '20 min', 'Threshold']];

async function power() {
  const all = bestsSince(null), recent = bestsSince(90);
  const durs = Object.keys(all).map(Number).sort((a, b) => a - b);
  const kg = ctx().weight;
  const ftpNow = ctx().ftp;
  const est = all[1200] ? Math.round(all[1200] * 0.95) : null;
  $('#ftp-hero').innerHTML = est ? `<div><span class="eyebrow">Estimated FTP</span><div class="hero-num power">${est}<small>W</small></div>
      <span class="muted small">95 % of your best 20 minutes (${all[1200]} W)${kg ? ` · ${fmt.num(est / kg, 2)} W/kg at ${kg} kg` : ''}</span></div>
      ${ftpNow ? `<div class="ftp-cmp"><span class="eyebrow">Set on the device</span><b>${ftpNow} W</b>${est > ftpNow ? '<span class="pill good">You may be stronger</span>' : ''}</div>` : ''}`
    : `<div><span class="eyebrow">Estimated FTP</span><div class="hero-num">–</div><span class="muted small">Ride 20 minutes or longer for an estimate, or do the FTP ramp test from Workouts.</span></div>`;
  $('#bests-tiles').innerHTML = KEY_BESTS.map(([d, l, z]) => tile(`${l} · ${z}`, all[d] ? fmt.num(all[d]) : '–', all[d] ? 'W' : '', 'power', 'zap')).join('');
  const el = $('#curve');
  if (!durs.length) { el.innerHTML = empty('No power data yet', 'Your best efforts appear after your first ride.', 'zap'); return; }
  chart(el, {
    xTime: false, xLog: true, height: 300, xSplits: [1, 5, 10, 30, 60, 120, 300, 600, 1200, 3600].filter((d) => d >= durs[0] && d <= durs[durs.length - 1]),
    data: [durs, durs.map((d) => all[d] ?? null), durs.map((d) => recent[d] ?? null)],
    xFmt: (s) => (s < 60 ? `${s}s` : s < 3600 ? `${s / 60}m` : `${s / 3600}h`),
    series: [
      { label: 'All time', stroke: '--s1', fill: true, points: { show: true, size: 6 }, value: (u, v) => (v == null ? '–' : `${v} W`) },
      { label: 'Last 90 days', stroke: '--s2', dash: [6, 4], points: { show: true, size: 6 }, value: (u, v) => (v == null ? '–' : `${v} W`) },
    ],
  });
}

// ---- Device -------------------------------------------------------------------------------

async function device() {
  const files = (await api.rideFiles()).sort((a, b) => b.mtime - a.mtime);
  const names = Object.fromEntries(S.users.map((u) => [u.id, u.name]));
  $('#files').innerHTML = files.length ? `<table><thead><tr><th>File</th><th>Rider</th><th>Size</th></tr></thead><tbody>${
    files.map((f) => `<tr><td><a href="/rides/${esc(f.name)}" download>${icon('download')} ${esc(f.name.split('/')[1])}</a></td>
      <td>${esc(names[f.name.split('/')[0]] || '?')}</td><td>${fmt.num(f.size / 1024)} kB</td></tr>`).join('')}</tbody></table>`
    : '<p class="muted small">No ride files yet.</p>';
  showVersions();
}

// ---- Firmware: installed versions, update file check, restart watch ----------------------
// /api/info: {project, version, idf, upload_s}; null on firmware without it or while away.
const deviceInfo = () => api.json('/api/info').catch(() => null);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function showVersions() {
  const [info, www] = await Promise.all([deviceInfo(), api.text('version.txt').catch(() => null)]);
  $('#versions').innerHTML = `Installed: firmware <b>${esc(info?.version || 'unknown')}</b> · web app <b>${
    esc((www || '').trim() || 'development build')}</b>`;
  return info;
}

// The app image starts with esp_image_header_t (magic 0xE9) + one segment header, then
// esp_app_desc_t at 0x20: magic 0xABCD5432, version at 0x30, project name at 0x50.
// A -full.bin starts with the boot loader instead, so it has no app description there.
async function readFirmware(f) {
  const v = new DataView(await f.slice(0, 0x70).arrayBuffer());
  if (v.byteLength < 0x70 || v.getUint8(0) !== 0xe9 || v.getUint32(0x20, true) !== 0xabcd5432) {
    return { err: /-full\.bin$/i.test(f.name) || (v.byteLength >= 1 && v.getUint8(0) === 0xe9)
      ? `${f.name} is the full image for a first install over USB. For an update choose justride-<version>.bin (without -full).`
      : `${f.name} is not a firmware file.` };
  }
  const str = (o) => new TextDecoder().decode(new Uint8Array(v.buffer, o, 32)).replace(/\0[\s\S]*$/, '');
  return { version: str(0x30), project: str(0x50) };
}

function otaStatus(text, tone) {
  const el = $('#ota-status');
  el.textContent = text;
  el.className = `small${tone ? ` status-${tone}` : ''}`;
}

async function flashFirmware(f) {
  const [fw, info] = await Promise.all([readFirmware(f), deviceInfo()]);
  if (fw.err) return otaStatus(fw.err, 'bad');
  if (info && fw.project !== info.project) return otaStatus(`${f.name} is firmware for "${fw.project}", not for this device.`, 'bad');
  if (info && info.upload_s <= 0) {
    return otaStatus('The device refuses updates right now: tap Wi-Fi → Allow firmware update on it, then choose the file again.', 'bad');
  }
  const old = info?.version;
  const same = old === fw.version ? ' (the same version that is installed)' : '';
  if (!confirm(`Install firmware ${fw.version}${same}?${old ? ` Installed: ${old}.` : ''}\nKeep the device powered until it restarts.`)) return;
  otaStatus(`Uploading ${fw.version}… do not unplug the device`);
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), 180000);
  try {
    const r = await fetch('/ota', { method: 'POST', headers: { 'X-JustRide': '1' }, body: f, signal: abort.signal });
    const t = await r.text();
    if (!r.ok) return otaStatus(t || `Upload failed (${r.status})`, 'bad');
  } catch (err) {
    // The device answers a refused upload at once and closes the connection; browsers
    // then often report a network error instead of its message.
    return otaStatus(abort.signal.aborted ? 'No answer from the device within 3 minutes. Check that it is on Wi-Fi and try again.'
      : 'The device closed the connection. Tap Allow firmware update on it again and retry; if it keeps failing, check the file.', 'bad');
  } finally { clearTimeout(timer); }
  // Restart: the old firmware answers for about a second, then the device is away
  // for a few seconds. If the new firmware doesn't start, the boot loader goes back.
  otaStatus(`Uploaded. Restarting with ${fw.version}…`);
  await sleep(6000);
  for (const until = Date.now() + 90000; Date.now() < until; await sleep(3000)) {
    const now = await deviceInfo();
    if (!now) continue;
    showVersions();
    if (now.version === fw.version) return otaStatus(`Done: firmware ${fw.version} is running.`, 'good');
    return otaStatus(`The device restarted with ${now.version}, not ${fw.version}: the new firmware didn't start and the device went back. A USB log helps to find out why.`, 'bad');
  }
  otaStatus('The device hasn\'t come back within 90 seconds. Check its screen and Wi-Fi, then reload this page.', 'bad');
}

function initDevice() {
  $('#up-zwo').onchange = async (e) => {
    for (const f of e.target.files) {
      try { await api.saveWorkout(f.name, f); toast(`Uploaded ${f.name}`); } catch (err) { toast(`${f.name}: ${err.message}`, 'bad'); }
    }
    e.target.value = '';
  };
  $('#up-ota').onchange = (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) flashFirmware(f);
  };
  $('#sql-run').onclick = runSql;
}

// SQL console: DuckDB-WASM, loaded on first use. Column types are given (times are UTC
// TIMESTAMPs: DuckDB-WASM has no ICU, so "...Z" must not become TIMESTAMPTZ).
const SQL_SUMMARY = 'ride_id VARCHAR, start_utc TIMESTAMP, duration_s INTEGER, moving_s INTEGER, workout VARCHAR, ftp_w INTEGER, ' +
  'avg_w INTEGER, np_w INTEGER, if DOUBLE, tss DOUBLE, kj INTEGER, avg_hr INTEGER, max_hr INTEGER, avg_cad DOUBLE, ' +
  'distance_km DOUBLE, fw VARCHAR, weight_kg INTEGER, user_max_hr INTEGER';
const SQL_BESTS = 'ride_id VARCHAR, duration_s INTEGER, watts INTEGER';
let duck = null;
async function runSql() {
  const out = $('#sql-out');
  try {
    if (!duck) {
      out.innerHTML = '<p class="muted small">Loading DuckDB (34 MB, once per visit)…</p>';
      const duckdb = await import('./vendor/duckdb-1.32.0/duckdb.mjs');
      const base = new URL('./vendor/duckdb-1.32.0/', import.meta.url);
      const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), new Worker(new URL('duckdb-browser-eh.worker.js', base)));
      await db.instantiate(new URL('duckdb-eh.wasm', base).href);
      duck = { db, conn: await db.connect(), user: null };
    }
    if (duck.user !== S.user.id) {
      for (const [t, cols] of [['summary', SQL_SUMMARY], ['bests', SQL_BESTS]]) {
        const text = (await api.text(`/rides/${S.user.id}/${t}.csv`)) || '';
        const spec = cols.split(', ').map((c) => c.split(' ')).map(([n, ty]) => `'${n}': '${ty}'`).join(', ');
        await duck.db.registerFileText(`${t}.csv`, text);
        await duck.conn.query(text.trim() ? `CREATE OR REPLACE TABLE ${t} AS SELECT * FROM read_csv('${t}.csv', header = true, columns = {${spec}})`
          : `CREATE OR REPLACE TABLE ${t} (${cols.replace(/(\w+) /g, '"$1" ')})`);
      }
      duck.user = S.user.id;
    }
    const res = await duck.conn.query($('#sql-text').value);
    const rows = res.toArray().slice(0, 500).map((r) => r.toJSON());
    const cols = res.schema.fields.map((f) => f.name);
    const times = new Set(res.schema.fields.filter((f) => /^(Date|Timestamp)/.test(String(f.type))).map((f) => f.name));
    const show = (v, c) => (v == null ? '' : times.has(c) ? new Date(Number(v)).toISOString().slice(0, 19).replace('T00:00:00', '').replace('T', ' ')
      : typeof v === 'bigint' ? Number(v) : typeof v === 'number' && !Number.isInteger(v) ? Math.round(v * 1000) / 1000
      : typeof v === 'object' ? JSON.stringify(v) : v);
    out.innerHTML = rows.length ? `<div class="scroll"><table><thead><tr>${cols.map((c) => `<th>${esc(c)}</th>`).join('')}</tr></thead><tbody>${
      rows.map((r) => `<tr>${cols.map((c) => `<td>${esc(show(r[c], c))}</td>`).join('')}</tr>`).join('')}</tbody></table></div>` : '<p class="muted small">No rows.</p>';
  } catch (err) {
    out.innerHTML = `<p class="error">${esc(err.message)}</p>`;
  }
}

// ---- Start --------------------------------------------------------------------------------

function fillPicker(id) {
  const pick = $('#user');
  pick.innerHTML = S.users.map((u, i) => `<option value="${i}">${esc(u.name)}</option>`).join('');
  const i = S.users.findIndex((u) => u.id === id);
  pick.value = String(i >= 0 ? i : Math.max(0, S.users.findIndex((u) => u.active)));
}

async function main() {
  $$('[data-icon]').forEach((el) => el.insertAdjacentHTML('afterbegin', icon(el.dataset.icon)));
  try {
    S.users = await api.users();
  } catch (err) {
    $('#boot').innerHTML = empty('Can\'t reach the device', `${esc(err.message)}. Is the head unit on and on the same Wi-Fi?`, 'alert');
    return;
  }
  if (!S.users.length) { $('#boot').innerHTML = empty('No riders found', 'Is the SD card in the head unit?', 'alert'); return; }
  // No internet on the device is fine: the browser's clock sets it if no time server answered
  api.send('POST', '/api/time', String(Math.floor(Date.now() / 1000))).catch(() => {});
  const pick = $('#user');
  fillPicker(S.users.find((u) => u.active)?.id);
  await loadUser(S.users[pick.value]);
  pick.onchange = async () => { await loadUser(S.users[pick.value]); go(location.hash.slice(1) || 'today'); };
  initRiders(async (list) => {
    // Keep the shown rider unless it was deleted; reload it (FTP, name may have changed)
    S.users = list;
    fillPicker(S.user.id);
    await loadUser(S.users[pick.value]);
  });
  $('#pmc-range').onclick = (e) => { const b = e.target.closest('button'); if (b) { S.range = Number(b.dataset.days); renderPmc(); } };
  initWorkouts(ctx);
  initPlan(ctx);
  initRoutes();
  initDevice();
  $('#boot').remove();
  document.body.classList.add('ready');
  await go(location.hash.slice(1) || 'today');
  // Keep the active rider's next week of plan workouts on the device
  const active = S.users.find((u) => u.active) || S.users[0];
  if (active.id === S.user.id) {
    const r = await autoSync(S.user, ctx(), riderLabel());
    if (r?.moved.length) toast(`Plan: missed ${r.moved.map((s) => GEN_TYPES[s.type].label).join(', ')} moved to ${r.moved.map((s) => fmt.date(new Date(s.date))).join(', ')}`);
    else if (r?.written) toast(`Plan: ${r.written} upcoming workout${r.written === 1 ? '' : 's'} sent to the device`);
  }
}

main().catch((e) => { console.error(e); toast(e.message, 'bad'); });
