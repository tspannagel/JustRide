// Routes: a GPX file becomes a .route file on the device (elevation every 10 m,
// smoothed), which it rides in SIM mode: the trainer follows the gradient by distance.
// The device smooths and clamps again; this only makes the file small and clean.
import { $, api, esc, fmt, icon, toast, tile, empty } from './core.js';

export const STEP_M = 10;
const MAX_POINTS = 20000;      // device limit (route.h): 200 km at 10 m
const SMOOTH = 4;              // moving average over ±40 m, twice
const GRADE_WINDOW_M = 100;    // as on the device

// ---- GPX -> profile ---------------------------------------------------------------------

// [{lat, lon, ele|null}] from a GPX text (track points, else route points)
export function gpxPoints(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.querySelector('parsererror')) throw new Error('not a valid GPX file');
  let pts = [...doc.getElementsByTagNameNS('*', 'trkpt')];
  if (!pts.length) pts = [...doc.getElementsByTagNameNS('*', 'rtept')];
  const name = doc.getElementsByTagNameNS('*', 'name')[0]?.textContent?.trim() || '';
  return {
    name,
    points: pts.map((p) => {
      const e = p.getElementsByTagNameNS('*', 'ele')[0];
      const ele = e ? Number(e.textContent) : NaN;
      return { lat: Number(p.getAttribute('lat')), lon: Number(p.getAttribute('lon')), ele: Number.isFinite(ele) ? ele : null };
    }).filter((p) => Number.isFinite(p.lat) && Number.isFinite(p.lon)),
  };
}

const haversine = (a, b) => {
  const r = 6371008.8, rad = Math.PI / 180;
  const dLat = (b.lat - a.lat) * rad, dLon = (b.lon - a.lon) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * rad) * Math.cos(b.lat * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * r * Math.asin(Math.min(1, Math.sqrt(h)));
};

const movingAvg = (v, r) => v.map((_, i) => {
  let s = 0, n = 0;
  for (let k = Math.max(0, i - r); k <= Math.min(v.length - 1, i + r); k++) { s += v[k]; n++; }
  return s / n;
});

// Elevations (m) every STEP_M from points with elevation; throws with a reason the rider can act on
export function profileFromPoints(points) {
  if (points.length < 2) throw new Error('the GPX has no track');
  const withEle = points.filter((p) => p.ele != null).length;
  if (withEle < points.length * 0.9) throw new Error('the GPX has no elevation data. Export it from a route planner (Komoot, RideWithGPS, Strava) with elevation');
  // Cumulative distance; points without elevation take it from their neighbours
  const d = [0], e = [];
  for (let i = 1; i < points.length; i++) d.push(d[i - 1] + haversine(points[i - 1], points[i]));
  for (let i = 0; i < points.length; i++) {
    if (points[i].ele != null) { e.push(points[i].ele); continue; }
    let a = i - 1, b = i + 1;
    while (b < points.length && points[b].ele == null) b++;
    const ea = a >= 0 ? e[a] : null, eb = b < points.length ? points[b].ele : null;
    e.push(ea == null ? eb : eb == null ? ea : ea + (eb - ea) * (d[i] - d[a]) / ((d[b] - d[a]) || 1));
  }
  const total = d[d.length - 1];
  if (total < 200) throw new Error('the route is shorter than 200 m');
  const n = Math.floor(total / STEP_M) + 1;
  if (n > MAX_POINTS) throw new Error(`the route is ${fmt.num(total / 1000, 1)} km; the device takes up to ${(MAX_POINTS - 1) * STEP_M / 1000} km`);
  const out = [];
  for (let k = 0, j = 0; k < n; k++) {
    const x = k * STEP_M;
    while (j < d.length - 2 && d[j + 1] < x) j++;
    const span = d[j + 1] - d[j];
    out.push(span > 0 ? e[j] + (e[j + 1] - e[j]) * Math.min(1, (x - d[j]) / span) : e[j]);
  }
  return movingAvg(movingAvg(out, SMOOTH), SMOOTH);
}

// Length, ascent, highest and lowest point, steepest 100 m (%), as the device computes them
export function profileInfo(ele) {
  const w = GRADE_WINDOW_M / STEP_M;
  let climb = 0, maxGrade = 0;
  for (let i = 1; i < ele.length; i++) climb += Math.max(0, ele[i] - ele[i - 1]);
  for (let i = 0; i + w < ele.length; i++) maxGrade = Math.max(maxGrade, (ele[i + w] - ele[i]) / GRADE_WINDOW_M * 100);
  return { length: (ele.length - 1) * STEP_M, climb, min: Math.min(...ele), max: Math.max(...ele), maxGrade };
}

export const routeText = (name, ele) =>
  `JustRide route 1\nname=${name.replace(/[\x00-\x1f]/g, ' ').slice(0, 47)}\nstep_m=${STEP_M}\n${ele.map((m) => Math.round(m * 100)).join('\n')}\n`;

// The device parses this back (route_parse_line): elevations in m
export function parseRouteText(text) {
  const lines = (text || '').split(/\r?\n/);
  if (lines[0] !== 'JustRide route 1') return null;
  let name = '', step = 0;
  const ele = [];
  for (const l of lines.slice(1)) {
    if (l.startsWith('name=')) name = l.slice(5);
    else if (l.startsWith('step_m=')) step = Number(l.slice(7));
    else if (/^-?\d+$/.test(l)) ele.push(Number(l) / 100);
  }
  return step === STEP_M && ele.length >= 2 ? { name, ele } : null;
}

const fileName = (name) => `${name.replace(/[^A-Za-z0-9 ._-]/g, '-').replace(/\s+/g, ' ').trim().slice(0, 54) || 'Route'}.route`;

// ---- Drawing ----------------------------------------------------------------------------

// Elevation profile as SVG, coloured by gradient (easy -> steep, the zone ramp)
export function profileSvg(ele, h = 120) {
  const W = 1000, n = ele.length, lo = Math.min(...ele), hi = Math.max(...ele);
  const range = Math.max(hi - lo, 50);  // flat routes don't fill the box with noise
  const y = (m) => h - 4 - (m - lo) / range * (h - 12);
  const cols = Math.min(200, n - 1), w = GRADE_WINDOW_M / STEP_M;
  let bars = '';
  for (let c = 0; c < cols; c++) {
    const i = Math.floor(c * (n - 1) / cols), j = Math.floor((c + 1) * (n - 1) / cols);
    const a = Math.max(0, i - w / 2), b = Math.min(n - 1, a + w);
    const g = (ele[b] - ele[a]) / ((b - a) * STEP_M) * 100;
    const z = g < 2 ? 2 : g < 4 ? 3 : g < 6 ? 4 : g < 8 ? 5 : g < 10 ? 6 : 7;
    const x1 = i / (n - 1) * W, x2 = j / (n - 1) * W + 1;  // overlap: no seams between columns
    bars += `<path d="M${x1} ${h}L${x1} ${y(ele[i])}L${x2} ${y(ele[j])}L${x2} ${h}Z" fill="var(--z${z})"/>`;
  }
  return `<svg class="route-profile" viewBox="0 0 ${W} ${h}" preserveAspectRatio="none" role="img" aria-label="Elevation profile">${bars}</svg>`;
}

const stats = (i) => [
  tile('Distance', fmt.num(i.length / 1000, 1), 'km', '', 'distance'),
  tile('Climbing', fmt.num(i.climb), 'm', '', 'trending'),
  tile('Highest', fmt.num(i.max), 'm', '', 'up'),
  tile('Steepest', fmt.num(i.maxGrade, 1), '%', '', 'bars'),
].join('');

// ---- View ---------------------------------------------------------------------------------

let pending = null;  // imported, not saved yet: { name, ele }

export function initRoutes() {
  $('#rt-import-btn').onclick = () => $('#rt-import').click();
  $('#rt-import').onchange = async (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (!f) return;
    try {
      const { name, points } = gpxPoints(await f.text());
      pending = { name: (name || f.name.replace(/\.gpx$/i, '')).slice(0, 47), ele: profileFromPoints(points) };
      renderPending();
    } catch (err) {
      toast(`Can't use this file: ${err.message}`, 'bad');
    }
  };
}

function renderPending() {
  const el = $('#rt-new');
  if (!pending) { el.innerHTML = ''; return; }
  el.innerHTML = `<div class="card">
    <input id="rt-name" class="title-input" maxlength="47" aria-label="Route name" value="${esc(pending.name)}">
    ${profileSvg(pending.ele, 160)}
    <div class="tiles compact">${stats(profileInfo(pending.ele))}</div>
    <p class="caption">Steepest = the steepest 100 m. Smoothed over about 100 m, so GPS noise doesn't turn into sudden walls. On the device the trainer
      follows the gradient by distance (−10 to +20 %); your virtual gears still work.</p>
    <div class="actions"><button class="btn ghost" id="rt-cancel">Cancel</button>
      <button class="btn primary" id="rt-save">${icon('save')}Save to device</button></div></div>`;
  $('#rt-cancel').onclick = () => { pending = null; renderPending(); };
  $('#rt-save').onclick = async () => {
    const name = $('#rt-name').value.trim() || 'Route';
    const file = fileName(name);
    try {
      const list = await api.workouts();
      if (list.some((f) => f.name === file) && !confirm(`Replace "${file}" on the device?`)) return;
      await api.saveWorkout(file, routeText(name, pending.ele));
      toast(`Saved: on the device under Workouts → ${file.replace(/\.route$/, '')}`);
      pending = null;
      showRoutes();
    } catch (err) {
      toast(`Not saved: ${err.message}`, 'bad');
    }
  };
}

export async function showRoutes() {
  renderPending();
  const el = $('#rt-list');
  let files = [];
  try {
    files = (await api.workouts()).filter((f) => /\.route$/i.test(f.name)).sort((a, b) => a.name.localeCompare(b.name));
  } catch (err) {
    el.innerHTML = empty('Can\'t reach the device', esc(err.message), 'alert');
    return;
  }
  if (!files.length) {
    el.innerHTML = empty('No routes yet', 'Import a GPX file with elevation (from Komoot, RideWithGPS, Strava routes or a bike computer).', 'map');
    return;
  }
  el.innerHTML = files.map((f, i) => `<div class="card" id="rt-${i}"><div class="card-head"><h2>${esc(f.name.replace(/\.route$/i, ''))}</h2>
    <button class="btn ghost icon-btn" data-del="${esc(f.name)}" aria-label="Delete">${icon('trash')}</button></div>
    <div class="rt-body muted small">Loading…</div></div>`).join('');
  el.onclick = async (e) => {
    const b = e.target.closest('[data-del]');
    if (!b || !confirm(`Delete "${b.dataset.del}" from the device?`)) return;
    try { await api.deleteWorkout(b.dataset.del); toast('Deleted'); showRoutes(); } catch (err) { toast(err.message, 'bad'); }
  };
  for (const [i, f] of files.entries()) {
    const r = parseRouteText(await api.text(`/workouts/${encodeURIComponent(f.name)}`));
    $(`#rt-${i} .rt-body`).outerHTML = r ? `${profileSvg(r.ele)}<div class="tiles compact">${stats(profileInfo(r.ele))}</div>`
      : '<p class="muted small">Not a route file this app can read.</p>';
  }
}
