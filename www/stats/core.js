// Shared helpers for the JustRide web app: device API, CSV parsing, training maths,
// formatting, icons, charts. Everything is served from the SD card: no CDN, no internet.
import uPlot from './vendor/uplot-1.6.32/uPlot.esm.js';

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
export const esc = (v) => String(v ?? '').replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

// ---- Device API -------------------------------------------------------------------
// Every write carries X-JustRide: the device refuses writes without it (CSRF guard).

export const api = {
  async json(url) {
    const r = await fetch(url, { cache: 'no-cache' });
    if (!r.ok) throw new Error(`${url}: ${r.status}`);
    return r.json();
  },
  async text(url) {
    const r = await fetch(url, { cache: 'no-cache' });
    return r.ok ? r.text() : null;
  },
  async send(method, url, body) {
    const r = await fetch(url, { method, headers: { 'X-JustRide': '1' }, body });
    const t = await r.text();
    if (!r.ok) throw new Error(t || `${r.status}`);
    return t;
  },
  users: () => api.json('/api/users'),
  rideFiles: () => api.json('/api/rides'),
  workouts: () => api.json('/api/workouts'),
  workout: (name) => api.text(`/workouts/${encodeURIComponent(name)}`),
  saveWorkout: (name, xml) => api.send('POST', `/workouts/${encodeURIComponent(name)}`, xml),
  deleteWorkout: (name) => api.send('DELETE', `/workouts/${encodeURIComponent(name)}`),
  plan: async (user) => { const t = await api.text(`/plans/${user}.json`); return t ? JSON.parse(t) : null; },
  savePlan: (user, plan) => api.send('POST', `/plans/${user}.json`, JSON.stringify(plan)),
};

// ---- CSV ----------------------------------------------------------------------------
// The device writes simple CSV: quoted strings never contain quotes or commas it
// didn't escape. Empty field = unknown (null).

export function parseCsv(text, types) {
  const lines = (text || '').split(/\r?\n/).filter((l) => l.trim());
  if (!lines.length) return [];
  const head = lines[0].split(',');
  return lines.slice(1).map((line) => {
    const f = [];
    let cur = '', q = false;
    for (let i = 0; i < line.length; i++) {
      const c = line[i];
      if (q) { if (c === '"' && line[i + 1] === '"') { cur += '"'; i++; } else if (c === '"') q = false; else cur += c; }
      else if (c === '"') q = true;
      else if (c === ',') { f.push(cur); cur = ''; }
      else cur += c;
    }
    f.push(cur);
    const o = {};
    head.forEach((h, i) => {
      const v = f[i] ?? '';
      const t = types[h];
      o[h] = v === '' ? null : t === 'n' ? Number(v) : t === 'd' ? new Date(v) : v;
    });
    return o;
  });
}

export const SUMMARY_T = { duration_s: 'n', moving_s: 'n', ftp_w: 'n', avg_w: 'n', np_w: 'n', if: 'n', tss: 'n', kj: 'n',
  avg_hr: 'n', max_hr: 'n', avg_cad: 'n', distance_km: 'n', weight_kg: 'n', user_max_hr: 'n', start_utc: 'd' };
export const BESTS_T = { duration_s: 'n', watts: 'n' };
export const RIDE_T = { t_s: 'n', power_w: 'n', cadence_rpm: 'n', speed_kmh: 'n', hr_bpm: 'n', target_w: 'n',
  grade_pct: 'n', gear: 'n', interval_idx: 'n', lap: 'n', ts_utc: 'd' };

// Rides recorded before the clock was set are stamped 1970: real but undated
export const dated = (r) => r.start_utc && r.start_utc.getUTCFullYear() >= 2020;

// ---- Training maths ---------------------------------------------------------------

// Daily TSS -> fitness (CTL, 42 d), fatigue (ATL, 7 d), form (TSB = yesterday's CTL - ATL)
export function pmc(rides, until = new Date()) {
  const byDay = new Map();
  for (const r of rides.filter(dated)) {
    const k = dayKey(r.start_utc);
    byDay.set(k, (byDay.get(k) || 0) + (r.tss || 0));
  }
  const days = [...byDay.keys()].sort();
  const out = { x: [], ctl: [], atl: [], tsb: [], tss: [] };
  if (!days.length) return out;
  let c = 0, a = 0;
  for (let d = new Date(days[0] + 'T00:00:00Z'); d <= until; d.setUTCDate(d.getUTCDate() + 1)) {
    const tss = byDay.get(dayKey(d)) || 0;
    out.tsb.push(c - a);
    c += (tss - c) / 42;
    a += (tss - a) / 7;
    out.x.push(d.getTime() / 1000); out.ctl.push(c); out.atl.push(a); out.tss.push(tss);
  }
  return out;
}

export function formState(tsb) {
  if (tsb == null) return { label: 'No data', tone: 'muted' };
  if (tsb > 15) return { label: 'Very fresh', tone: 'info' };
  if (tsb > 5) return { label: 'Fresh', tone: 'good' };
  if (tsb > -10) return { label: 'Neutral', tone: 'good' };
  if (tsb > -30) return { label: 'Productive load', tone: 'warn' };
  return { label: 'Overreaching', tone: 'bad' };
}

export const dayKey = (d) => new Date(d).toISOString().slice(0, 10);
export const addDays = (key, n) => { const d = new Date(key + 'T00:00:00Z'); d.setUTCDate(d.getUTCDate() + n); return dayKey(d); };
export const todayKey = () => {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
};

// Power zones by % of FTP (Coggan). Colours are the app's zone ramp (style.css --z1..--z7).
export const ZONES = [
  { name: 'Recovery', max: 0.55 }, { name: 'Endurance', max: 0.75 }, { name: 'Tempo', max: 0.90 },
  { name: 'Threshold', max: 1.05 }, { name: 'VO2max', max: 1.20 }, { name: 'Anaerobic', max: 1.50 },
  { name: 'Neuromuscular', max: Infinity },
];
export const zoneOf = (frac) => ZONES.findIndex((z) => frac < z.max);
export const zoneColor = (i) => `var(--z${i + 1})`;

// ---- Formatting -------------------------------------------------------------------

export const fmt = {
  dur(s) {
    s = Math.round(s || 0);
    const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = s % 60;
    return h ? `${h}:${String(m).padStart(2, '0')}:${String(x).padStart(2, '0')}` : `${m}:${String(x).padStart(2, '0')}`;
  },
  hm(s) { const m = Math.round((s || 0) / 60); return m >= 60 ? `${Math.floor(m / 60)} h ${m % 60 ? `${m % 60} min` : ''}`.trim() : `${m} min`; },
  num: (v, d = 0) => (v == null || Number.isNaN(v) ? '–' : Number(v).toLocaleString(undefined, { minimumFractionDigits: d, maximumFractionDigits: d })),
  date: (d) => (dated({ start_utc: d }) ? d.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' }) : 'Date unknown'),
  time: (d) => (dated({ start_utc: d }) ? d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' }) : ''),
  rel(d) {
    if (!dated({ start_utc: d })) return 'before the clock was set';
    const days = Math.round((new Date(todayKey()) - new Date(dayKey(d))) / 86400e3);
    return days <= 0 ? 'today' : days === 1 ? 'yesterday' : days < 7 ? `${days} days ago` : fmt.date(d);
  },
  secs: (s) => (s < 60 ? `${s} s` : s < 3600 ? `${s / 60} min` : `${s / 3600} h`),
};

// ---- Icons ------------------------------------------------------------------------
// Feather icons (https://feathericons.com, MIT licence), inlined.

const ICONS = {
  home: '<path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/>',
  activity: '<polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>',
  trending: '<polyline points="23 6 13.5 15.5 8.5 10.5 1 18"/><polyline points="17 6 23 6 23 12"/>',
  zap: '<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/>',
  heart: '<path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z"/>',
  clock: '<circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/>',
  calendar: '<rect x="3" y="4" width="18" height="18" rx="2"/><line x1="16" y1="2" x2="16" y2="6"/><line x1="8" y1="2" x2="8" y2="6"/><line x1="3" y1="10" x2="21" y2="10"/>',
  bars: '<line x1="18" y1="20" x2="18" y2="10"/><line x1="12" y1="20" x2="12" y2="4"/><line x1="6" y1="20" x2="6" y2="14"/>',
  energy: '<polyline points="11 6 7 12 13 12 9 18"/><rect x="2" y="6" width="17" height="12" rx="2"/><line x1="22" y1="11" x2="22" y2="13"/>',
  cadence: '<polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/>',
  distance: '<polygon points="3 11 22 2 13 21 11 13 3 11"/>',
  award: '<circle cx="12" cy="8" r="7"/><polyline points="8.21 13.89 7 23 12 20 17 23 15.79 13.88"/>',
  back: '<line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>',
  layers: '<polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/>',
  play: '<polygon points="5 3 19 12 5 21 5 3"/>',
  plus: '<line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>',
  trash: '<polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>',
  save: '<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/>',
  download: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>',
  upload: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/>',
  sparkles: '<path d="M12 3l1.9 5.1L19 10l-5.1 1.9L12 17l-1.9-5.1L5 10l5.1-1.9z"/><path d="M19 15l.9 2.1L22 18l-2.1.9L19 21l-.9-2.1L16 18l2.1-.9z"/>',
  up: '<polyline points="18 15 12 9 6 15"/>',
  down: '<polyline points="6 9 12 15 18 9"/>',
  copy: '<rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>',
  cpu: '<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/>',
  database: '<ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M21 12c0 1.66-4 3-9 3s-9-1.34-9-3"/><path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"/>',
  user: '<path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/>',
  target: '<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="2"/>',
  file: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/>',
  check: '<polyline points="20 6 9 17 4 12"/>',
  alert: '<circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/>',
  x: '<line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>',
};
export const icon = (name, cls = '') => `<svg class="i ${cls}" viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ''}</svg>`;

// ---- UI bits ------------------------------------------------------------------------

export function toast(msg, tone = 'good') {
  const el = document.createElement('div');
  el.className = `toast ${tone}`;
  el.innerHTML = `${icon(tone === 'bad' ? 'alert' : 'check')}<span>${esc(msg)}</span>`;
  $('#toasts').append(el);
  setTimeout(() => el.classList.add('out'), 3600);
  setTimeout(() => el.remove(), 4200);
}

// Metric tile: value + unit + label, optional tone (power, hr, cad, dist, load)
export const tile = (label, value, unit = '', tone = '', ic = '') => `<div class="tile ${tone}">
  <div class="tile-label">${ic ? icon(ic) : ''}${esc(label)}</div>
  <div class="tile-value">${esc(value)}<small>${esc(unit)}</small></div></div>`;

export const empty = (title, text, ic = 'activity') =>
  `<div class="empty">${icon(ic)}<h3>${esc(title)}</h3><p>${text}</p></div>`;

// ---- Charts (uPlot) -----------------------------------------------------------------
// One y axis per chart (different units get their own chart, cursors synced).

export const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const charts = new Map();

export function chart(el, { series, data, height = 260, xTime = true, xLog = false, xSplits, xFmt, yFmt, sync, bands }) {
  charts.get(el)?.destroy();
  el.innerHTML = '';
  const muted = css('--text-3'), grid = css('--line');
  const font = '12px ' + css('--font');
  const axis = (fmtFn, splits) => ({
    stroke: muted, font, grid: { stroke: grid, width: 1 }, ticks: { show: false }, gap: 6,
    ...(fmtFn ? { values: (u, v) => v.map((x) => (x == null ? '' : fmtFn(x))) } : {}),
    ...(splits ? { splits: () => splits } : {}),
  });
  const opts = {
    width: el.clientWidth || 600, height,
    padding: [12, 8, 0, 0],
    cursor: { sync: sync ? { key: sync } : undefined, points: { size: 8, width: 2, fill: css('--surface') }, drag: { x: true, y: false } },
    legend: { live: true },
    scales: { x: { time: xTime, ...(xLog ? { distr: 3 } : {}) } },
    axes: [axis(xFmt, xSplits), { ...axis(yFmt), size: 48 }],
    bands,
    series: [{ label: xTime ? 'Date' : 'Time', value: xFmt ? (u, v) => (v == null ? '–' : xFmt(v)) : undefined }, ...series.map((s) => ({
      width: 2, points: { show: false }, spanGaps: false, ...s,
      stroke: s.stroke && s.stroke.startsWith('--') ? css(s.stroke) : s.stroke,
      fill: s.fill === true ? (u) => gradient(u, css(s.stroke)) : s.fill,
    }))],
  };
  const u = new uPlot(opts, data, el);
  charts.set(el, u);
  return u;
}

function gradient(u, color) {
  if (!u.bbox || !Number.isFinite(u.bbox.top) || !u.bbox.height) return color + '22';  // before the first layout
  const g = u.ctx.createLinearGradient(0, u.bbox.top, 0, u.bbox.top + u.bbox.height);
  g.addColorStop(0, color + '55');
  g.addColorStop(1, color + '00');
  return g;
}

// Keep charts the width of their container
new ResizeObserver(() => {
  for (const [el, u] of charts) {
    if (!el.isConnected) { u.destroy(); charts.delete(el); continue; }
    if (el.clientWidth && Math.abs(el.clientWidth - u.width) > 4) u.setSize({ width: el.clientWidth, height: u.height });
  }
}).observe(document.documentElement);
