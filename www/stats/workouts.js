// Workouts: model, .zwo read/write, load estimate, profile drawing, the generator and
// the editor view. Targets are fractions of FTP, as in .zwo, so a workout scales with
// whoever rides it. Only the elements the device's parser knows are written.
import { $, $$, api, esc, fmt, icon, toast, tile, zoneOf, zoneColor, ZONES } from './core.js';

// ---- Model ------------------------------------------------------------------------
// Block: {type, dur, p, lo, hi, reps, on, off, onP, offP, cad, cadRest}

export const BLOCK_TYPES = {
  warmup: { label: 'Warm-up', fields: ['dur', 'lo', 'hi'] },
  steady: { label: 'Steady', fields: ['dur', 'p', 'cad'] },
  intervals: { label: 'Intervals', fields: ['reps', 'on', 'onP', 'off', 'offP', 'cad'] },
  ramp: { label: 'Ramp', fields: ['dur', 'lo', 'hi'] },
  free: { label: 'Free ride', fields: ['dur'] },
  cooldown: { label: 'Cool-down', fields: ['dur', 'hi', 'lo'] },
};
const NEW_BLOCK = {
  warmup: { dur: 600, lo: 0.45, hi: 0.75 }, steady: { dur: 600, p: 0.75 },
  intervals: { reps: 4, on: 240, onP: 1.1, off: 240, offP: 0.5 }, ramp: { dur: 300, lo: 0.6, hi: 0.9 },
  free: { dur: 600 }, cooldown: { dur: 300, hi: 0.65, lo: 0.4 },
};

export const blockDur = (b) => (b.type === 'intervals' ? b.reps * (b.on + b.off) : b.dur);
export const totalDur = (w) => w.blocks.reduce((a, b) => a + blockDur(b), 0);
const segments = (w) => w.blocks.reduce((a, b) => a + (b.type === 'intervals' ? 2 * b.reps : 1), 0);  // the device keeps 200

// Per-second target as a fraction of FTP (free ride: null)
function expand(w) {
  const out = [];
  for (const b of w.blocks) {
    if (b.type === 'intervals') {
      for (let r = 0; r < b.reps; r++) {
        for (let i = 0; i < b.on; i++) out.push(b.onP);
        for (let i = 0; i < b.off; i++) out.push(b.offP);
      }
    } else if (b.type === 'free') {
      for (let i = 0; i < b.dur; i++) out.push(null);
    } else if (b.type === 'steady') {
      for (let i = 0; i < b.dur; i++) out.push(b.p);
    } else {
      for (let i = 0; i < b.dur; i++) out.push(b.lo + ((b.hi - b.lo) * i) / Math.max(1, b.dur - 1));
    }
  }
  return out;
}

// Estimated load. Free-ride time counts as easy endurance (60 % FTP).
export function load(w, ftp) {
  const s = expand(w).map((v) => (v == null ? 0.6 : v));
  if (!s.length) return { secs: 0, tss: 0, if: 0, avgW: 0, kj: 0 };
  let sum = 0, sum4 = 0, roll = 0;
  const win = [];
  for (const v of s) {
    sum += v;
    win.push(v); roll += v;
    if (win.length > 30) roll -= win.shift();
    sum4 += (roll / win.length) ** 4;
  }
  const IF = (sum4 / s.length) ** 0.25;
  return { secs: s.length, if: IF, tss: (s.length / 3600) * IF * IF * 100, avgW: (sum / s.length) * ftp, kj: (sum * ftp) / 1000 };
}

// ---- .zwo ---------------------------------------------------------------------------

const xmlEsc = (v) => String(v).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const p3 = (v) => (Math.round(v * 1000) / 1000).toFixed(3).replace(/0+$/, '').replace(/\.$/, '.0');

export function toZwo(w) {
  const cad = (b) => (b.cad ? ` Cadence="${Math.round(b.cad)}"` : '');
  const el = (b) => {
    switch (b.type) {
      case 'warmup': return `<Warmup Duration="${b.dur}" PowerLow="${p3(b.lo)}" PowerHigh="${p3(b.hi)}"${cad(b)}/>`;
      case 'cooldown': return `<Cooldown Duration="${b.dur}" PowerLow="${p3(b.hi)}" PowerHigh="${p3(b.lo)}"${cad(b)}/>`;
      case 'ramp': return `<Ramp Duration="${b.dur}" PowerLow="${p3(b.lo)}" PowerHigh="${p3(b.hi)}"${cad(b)}/>`;
      case 'steady': return `<SteadyState Duration="${b.dur}" Power="${p3(b.p)}"${cad(b)}/>`;
      case 'free': return `<FreeRide Duration="${b.dur}"/>`;
      case 'intervals': return `<IntervalsT Repeat="${b.reps}" OnDuration="${b.on}" OffDuration="${b.off}" ` +
        `OnPower="${p3(b.onP)}" OffPower="${p3(b.offP)}"${cad(b)}${b.cadRest ? ` CadenceResting="${Math.round(b.cadRest)}"` : ''}/>`;
      default: return '';
    }
  };
  return `<?xml version="1.0" encoding="UTF-8"?>
<workout_file>
    <author>JustRide</author>
    <name>${xmlEsc(w.name)}</name>
    <description>${xmlEsc(w.description || '')}</description>
    <sportType>bike</sportType>
    <workout>
${w.blocks.map((b) => `        ${el(b)}`).join('\n')}
    </workout>
</workout_file>
`;
}

// Zwift writes a cool-down as PowerLow (start) > PowerHigh (end); our model keeps hi = start
export function fromZwo(xml, fallbackName = 'Workout') {
  const doc = new DOMParser().parseFromString(xml, 'application/xml');
  if (doc.querySelector('parsererror')) throw new Error('not a valid .zwo file');
  const n = (el, a, d = 0) => (el.hasAttribute(a) ? Number(el.getAttribute(a)) : d);
  const blocks = [];
  for (const el of doc.querySelectorAll('workout > *')) {
    const cad = n(el, 'Cadence') || undefined;
    switch (el.tagName) {
      case 'Warmup': blocks.push({ type: 'warmup', dur: n(el, 'Duration'), lo: n(el, 'PowerLow'), hi: n(el, 'PowerHigh'), cad }); break;
      case 'Cooldown': blocks.push({ type: 'cooldown', dur: n(el, 'Duration'), hi: n(el, 'PowerLow'), lo: n(el, 'PowerHigh'), cad }); break;
      case 'Ramp': blocks.push({ type: 'ramp', dur: n(el, 'Duration'), lo: n(el, 'PowerLow'), hi: n(el, 'PowerHigh'), cad }); break;
      case 'SteadyState': blocks.push({ type: 'steady', dur: n(el, 'Duration'), p: n(el, 'Power'), cad }); break;
      case 'FreeRide': blocks.push({ type: 'free', dur: n(el, 'Duration') }); break;
      case 'IntervalsT': blocks.push({ type: 'intervals', reps: n(el, 'Repeat', 1), on: n(el, 'OnDuration'), off: n(el, 'OffDuration'),
        onP: n(el, 'OnPower'), offP: n(el, 'OffPower'), cad, cadRest: n(el, 'CadenceResting') || undefined }); break;
      default: break;  // the device skips unknown elements too
    }
  }
  return {
    name: doc.querySelector('workout_file > name')?.textContent.trim() || fallbackName,
    description: doc.querySelector('workout_file > description')?.textContent.trim() || '',
    blocks,
  };
}

// Device file names: letters, digits, space . _ - only; ≤ 60 characters with ".zwo"
export const fileName = (name) => `${name.replace(/[^A-Za-z0-9 ._-]/g, '-').replace(/\s+/g, ' ').trim().slice(0, 56) || 'Workout'}.zwo`;

// ---- Profile drawing ------------------------------------------------------------------

export function profileSvg(w, { height = 180, compact = false } = {}) {
  const total = totalDur(w);
  if (!total) return `<svg class="profile" viewBox="0 0 100 ${height}" preserveAspectRatio="none"></svg>`;
  const s = expand(w);
  const top = Math.max(1.3, ...s.filter((v) => v != null).map((v) => v + 0.1));
  const W = 1000, y = (f) => height - (f / top) * (height - 6);
  const shapes = [], edges = [], glow = [], gaps = [];
  // Shapes of equal zone; a step change (interval edge) starts a new one, ramps split per zone
  let i = 0, prevTop = null;
  while (i < s.length) {
    const free = s[i] == null;
    const z = free ? -1 : zoneOf(s[i]);
    let j = i;
    while (j < s.length && (s[j] == null) === free && (free || zoneOf(s[j]) === z) && (j === i || !isEdge(s, j))) j++;
    const x0 = (i / total) * W, x1 = (j / total) * W;
    const a = free ? y(0.6) : y(s[i]), b = free ? a : y(s[j - 1]);
    // A thin gap between blocks, up to the lower of the two tops
    if (prevTop != null && (isEdge(s, i) || (s[i] == null) !== (s[i - 1] == null))) gaps.push(`<line x1="${x0}" x2="${x0}" y1="${Math.max(prevTop, a)}" y2="${height}" class="gap"/>`);
    prevTop = b;
    if (free) {
      shapes.push(`<rect x="${x0}" y="${a}" width="${x1 - x0}" height="${height - a}" class="free"/>`);
      edges.push(`<path d="M${x0} ${a + 1}H${x1}" class="free-edge"/>`);
    } else {
      const d = `M${x0} ${height}L${x0} ${a}L${x1} ${b}L${x1} ${height}Z`;
      shapes.push(`<path d="${d}" fill="url(#pg-z${z + 1})"/>`);
      if (!compact) {
        // Light edge inside the top of the block
        edges.push(`<path d="M${x0} ${a + 1.5}L${x1} ${b + 1.5}" class="edge" style="stroke:color-mix(in srgb, var(--z${z + 1}) 50%, #fff)"/>`);
        if (z >= 3) glow.push(`<path d="${d}" style="fill:var(--z${z + 1})"/>`);
      }
    }
    i = j;
  }
  const ftp = y(1);
  return `<svg class="profile${compact ? ' compact' : ''}" viewBox="0 0 ${W} ${height}" preserveAspectRatio="none" role="img"
    aria-label="Workout profile, ${fmt.hm(total)}">${glow.length ? `<g class="glow" filter="url(#pg-glow)">${glow.join('')}</g>` : ''}
    ${shapes.join('')}${edges.join('')}${gaps.join('')}
    ${compact ? '' : `<line x1="0" x2="${W}" y1="${ftp}" y2="${ftp}" class="ftp-line"/>`}</svg>`;
}
// A step change (interval edge) starts a new shape even within one zone
const isEdge = (s, j) => s[j] != null && s[j - 1] != null && Math.abs(s[j] - s[j - 1]) > 0.02;

// ---- Generator ------------------------------------------------------------------------
// ctx: {ftp, tsb, best: {60, 300, 1200}} from the rider's history (all optional).

export const GEN_TYPES = {
  recovery: { label: 'Recovery', zone: 0, about: 'Very easy spinning to absorb training.' },
  endurance: { label: 'Endurance', zone: 1, about: 'Steady aerobic base, the bulk of any plan.' },
  tempo: { label: 'Tempo', zone: 2, about: 'Long, moderately hard blocks.' },
  sweetspot: { label: 'Sweet spot', zone: 3, about: 'Just under threshold: lots of training per hour of fatigue.' },
  threshold: { label: 'Threshold', zone: 3, about: 'At FTP, to push it up.' },
  vo2: { label: 'VO2max', zone: 4, about: 'Hard 2–5 minute efforts for top-end aerobic power.' },
  anaerobic: { label: 'Sprints', zone: 5, about: 'Short all-out efforts with long recovery.' },
  ramptest: { label: 'FTP ramp test', zone: 6, about: '1-minute steps until you can\'t hold the power. FTP ≈ 75 % of your best minute.' },
};

export function adaptation(ctx) {
  const notes = [];
  let factor = 1;
  const tsb = ctx?.tsb;
  if (tsb != null) {
    if (tsb < -30) { factor = 0.94; notes.push(`Form ${Math.round(tsb)} (very tired): targets 6 % lower`); }
    else if (tsb < -15) { factor = 0.97; notes.push(`Form ${Math.round(tsb)} (tired): targets 3 % lower`); }
    else if (tsb > 15) { factor = 1.02; notes.push(`Form +${Math.round(tsb)} (fresh): targets 2 % higher`); }
    else notes.push(`Form ${Math.round(tsb)}: normal targets`);
  }
  const ftp = ctx?.ftp, b = ctx?.best || {};
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  let vo2 = 1.12, an = 1.5;
  if (ftp && b[300]) { vo2 = clamp((0.95 * b[300]) / ftp, 1.06, 1.25); notes.push(`VO2max at ${Math.round(vo2 * 100)} % from your 5-min best (${b[300]} W)`); }
  if (ftp && b[60]) { an = clamp((0.85 * b[60]) / ftp, 1.25, 1.8); }
  if (ftp && b[1200] && b[1200] * 0.95 > ftp * 1.03) notes.push(`Your 20-min best suggests an FTP of ${Math.round(b[1200] * 0.95)} W (now ${ftp} W)`);
  return { factor, vo2, an, notes };
}

// minutes: total length; difficulty 1–5; adapt: use ctx
export function generate(type, minutes, difficulty, ctx, adapt = true) {
  const a = adapt ? adaptation(ctx) : { factor: 1, vo2: 1.12, an: 1.5, notes: [] };
  const d = Math.min(5, Math.max(1, difficulty));
  const k = (d - 3) * 0.02;            // intensity shift per difficulty step
  const P = (v) => Math.round(v * a.factor * 1000) / 1000;
  const total = minutes * 60;
  const blocks = [];
  let name;
  const wu = type === 'recovery' || type === 'ramptest' ? 300 : 600, cd = type === 'ramptest' ? 300 : type === 'recovery' ? 0 : 300;
  const main = Math.max(300, total - wu - cd);
  blocks.push({ type: 'warmup', dur: wu, lo: 0.45, hi: type === 'recovery' ? 0.5 : 0.72 });

  const sets = (on, off, onP, offP, label, cad) => {
    const reps = Math.max(1, Math.floor(main / (on + off)));
    blocks.push({ type: 'intervals', reps, on, off, onP: P(onP), offP: P(offP), cad });
    const used = reps * (on + off);
    if (main - used >= 60) blocks.push({ type: 'steady', dur: main - used, p: P(0.6) });
    name = `${label} ${reps}x${on >= 60 ? `${Math.round(on / 60)}min` : `${on}s`}`;
  };

  switch (type) {
    case 'recovery':
      blocks.push({ type: 'steady', dur: main, p: P(0.5), cad: 90 }); name = `Recovery ${minutes}min`; break;
    case 'endurance':
      if (d >= 4) {  // endurance with tempo bursts
        const burst = 300, reps = Math.max(1, Math.floor(main / 1200));
        const easy = Math.floor((main - reps * burst) / (reps + 1));
        for (let r = 0; r < reps; r++) {
          blocks.push({ type: 'steady', dur: easy, p: P(0.66 + k) });
          blocks.push({ type: 'steady', dur: burst, p: P(0.82) });
        }
        blocks.push({ type: 'steady', dur: main - reps * (easy + burst), p: P(0.66 + k) });
      } else blocks.push({ type: 'steady', dur: main, p: P(0.65 + k) });
      name = `Endurance ${minutes}min`; break;
    case 'tempo': sets([600, 900, 900, 1200, 1200][d - 1], 300, 0.8 + k, 0.6, 'Tempo'); break;
    case 'sweetspot': sets([480, 600, 720, 900, 1200][d - 1], 300, 0.9 + k / 2, 0.55, 'Sweet spot'); break;
    case 'threshold': { const on = [360, 480, 600, 720, 900][d - 1]; sets(on, Math.round(on / 2), 0.98 + k / 2, 0.55, 'Threshold'); break; }
    case 'vo2': { const on = [120, 180, 180, 240, 300][d - 1]; sets(on, on, a.vo2 + k / 2, 0.5, 'VO2max', 100); break; }
    case 'anaerobic': sets(30, [210, 180, 150, 120, 90][d - 1], a.an + k, 0.5, 'Sprints', 110); break;
    case 'ramptest': {
      const steps = 25;  // 50 % up to 194 %: nobody finishes, stop when you can't hold it
      for (let i = 0; i < steps; i++) blocks.push({ type: 'steady', dur: 60, p: Math.round((0.5 + i * 0.06) * 1000) / 1000 });
      name = 'FTP ramp test'; break;
    }
    default: throw new Error(`unknown type ${type}`);
  }
  if (cd) blocks.push({ type: 'cooldown', dur: cd, hi: 0.6, lo: 0.4 });
  const about = GEN_TYPES[type].about + (a.notes.length ? ` Adapted: ${a.notes.join('; ')}.` : '');
  return { name, description: about, blocks, generated: { type, minutes, difficulty: d } };
}

// ---- Editor view ------------------------------------------------------------------------

const state = { w: null, file: null, dirty: false, list: [], ctx: null, ftp: 200 };
const pct = (v) => Math.round((v || 0) * 100);
const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
const parseMmss = (t) => {
  const m = String(t).trim().match(/^(\d+)(?::(\d{1,2}))?$/);
  return m ? Number(m[1]) * 60 + Number(m[2] || 0) : null;
};

export function initWorkouts(ctxFn) {
  state.ctxFn = ctxFn;
  const root = $('#workouts');
  root.addEventListener('input', onInput);
  root.addEventListener('click', onClick);
  $('#wo-import').addEventListener('change', onImport);
  window.addEventListener('beforeunload', (e) => { if (state.dirty) e.preventDefault(); });
}

export async function showWorkouts() {
  state.ctx = state.ctxFn();
  state.ftp = state.ctx.ftp || 200;
  $('#wo-ftp').value = state.ftp;
  await refreshList();
  if (!state.w) newWorkout(generate('sweetspot', 60, 3, state.ctx), null);
  render();
}

async function refreshList() {
  try {
    state.list = (await api.workouts()).sort((a, b) => a.name.localeCompare(b.name));
  } catch {
    state.list = [];
  }
  const el = $('#wo-list');
  el.innerHTML = state.list.length ? state.list.map((f) => `<button class="lib-item${f.name === state.file ? ' active' : ''}"
      data-open="${esc(f.name)}">${icon(f.name.startsWith('Plan ') ? 'calendar' : 'file')}<span>${esc(f.name.replace(/\.zwo$/i, ''))}</span></button>`).join('')
    : `<p class="muted small">No workouts on the device yet.</p>`;
}

function newWorkout(w, file) {
  state.w = structuredClone(w);
  state.file = file;
  state.dirty = false;
}

function render() {
  const w = state.w;
  $('#wo-name').value = w.name;
  $('#wo-desc').value = w.description;
  renderStats();
  $('#wo-blocks').innerHTML = w.blocks.map(blockRow).join('') ||
    `<p class="muted small">Add blocks with the buttons below, or let the generator build one.</p>`;
  $('#wo-delete').hidden = !state.file;
  $$('#wo-list .lib-item').forEach((b) => b.classList.toggle('active', b.dataset.open === state.file));
}

function renderStats() {
  const w = state.w, L = load(w, state.ftp);
  $('#wo-profile').innerHTML = profileSvg(w, { height: 200 });
  $('#wo-stats').innerHTML = [
    tile('Duration', fmt.dur(L.secs), '', '', 'clock'),
    tile('Load', fmt.num(L.tss), 'TSS', 'load', 'bars'),
    tile('Intensity', fmt.num(L.if, 2), 'IF', 'load', 'target'),
    tile('Average', fmt.num(L.avgW), 'W', 'power', 'zap'),
    tile('Work', fmt.num(L.kj), 'kJ', 'power', 'energy'),
  ].join('') + (segments(w) > 200 ? '<p class="error wide">Too long for the device: it runs the first 200 steps only (an interval repeat counts as two).</p>' : '');
  $('#wo-save').classList.toggle('pulse', state.dirty);
}

function blockRow(b, i) {
  const T = BLOCK_TYPES[b.type];
  const f = (key) => {
    const watts = (v) => `<span class="watts">${Math.round((v || 0) * state.ftp)} W</span>`;
    switch (key) {
      case 'dur': return field('Time', `<input data-k="dur" value="${mmss(b.dur)}" inputmode="numeric" class="t">`);
      case 'on': return field('On', `<input data-k="on" value="${mmss(b.on)}" inputmode="numeric" class="t">`);
      case 'off': return field('Off', `<input data-k="off" value="${mmss(b.off)}" inputmode="numeric" class="t">`);
      case 'reps': return field('Repeats', `<input data-k="reps" type="number" min="1" max="50" value="${b.reps}" class="n">`);
      case 'cad': return field('Cadence', `<input data-k="cad" type="number" min="0" max="150" placeholder="any" value="${b.cad || ''}" class="n">`);
      default: {
        const label = { p: 'Power', lo: b.type === 'cooldown' ? 'End' : 'From', hi: b.type === 'cooldown' ? 'Start' : 'To', onP: 'On power', offP: 'Off power' }[key];
        return field(label, `<input data-k="${key}" type="number" min="0" max="300" step="1" value="${pct(b[key])}" class="n"><i>%</i>${watts(b[key])}`);
      }
    }
  };
  const z = b.type === 'free' ? -1 : zoneOf(b.type === 'intervals' ? b.onP : b.type === 'steady' ? b.p : Math.max(b.lo, b.hi));
  return `<div class="block" data-i="${i}" style="--zc:${z < 0 ? 'var(--text-3)' : zoneColor(z)}">
    <div class="block-head"><span class="chip">${esc(T.label)}</span><span class="block-dur">${fmt.dur(blockDur(b))}${z >= 0 ? ` · ${ZONES[z].name}` : ''}</span>
      <span class="block-tools">
        <button class="icon-btn" data-act="up" title="Move up" aria-label="Move up">${icon('up')}</button>
        <button class="icon-btn" data-act="down" title="Move down" aria-label="Move down">${icon('down')}</button>
        <button class="icon-btn" data-act="dup" title="Duplicate" aria-label="Duplicate">${icon('copy')}</button>
        <button class="icon-btn danger" data-act="del" title="Remove" aria-label="Remove">${icon('trash')}</button>
      </span></div>
    <div class="block-fields">${T.fields.map(f).join('')}</div></div>`;
}
const field = (label, input) => `<label class="field"><span>${label}</span><div>${input}</div></label>`;

function onInput(e) {
  const t = e.target;
  if (t.id === 'wo-name') { state.w.name = t.value; state.dirty = true; return renderStats(); }
  if (t.id === 'wo-desc') { state.w.description = t.value; state.dirty = true; return; }
  if (t.id === 'wo-ftp') { state.ftp = Math.max(50, Number(t.value) || 200); return refreshRows(); }
  const row = t.closest('.block');
  if (!row || !t.dataset.k) return;
  const b = state.w.blocks[row.dataset.i], k = t.dataset.k;
  if (['dur', 'on', 'off'].includes(k)) {
    const s = parseMmss(t.value);
    t.classList.toggle('bad', s == null || s < 1);
    if (s == null || s < 1) return;
    b[k] = Math.min(s, 4 * 3600);
  } else if (k === 'reps') b.reps = Math.min(50, Math.max(1, Math.round(Number(t.value) || 1)));
  else if (k === 'cad') b.cad = Number(t.value) || undefined;
  else b[k] = Math.min(3, Math.max(0, (Number(t.value) || 0) / 100));
  state.dirty = true;
  renderStats();
  const w = row.querySelector(`[data-k="${k}"] ~ .watts`);
  if (w) w.textContent = `${Math.round(b[k] * state.ftp)} W`;
  row.querySelector('.block-dur').textContent = fmt.dur(blockDur(b));
}

function refreshRows() { renderStats(); $('#wo-blocks').innerHTML = state.w.blocks.map(blockRow).join(''); }

async function onClick(e) {
  const b = e.target.closest('button');
  if (!b) return;
  const blocks = state.w?.blocks;
  if (b.dataset.open) return openFile(b.dataset.open);
  if (b.dataset.add) { blocks.push({ type: b.dataset.add, ...NEW_BLOCK[b.dataset.add] }); state.dirty = true; return refreshRows(); }
  if (b.dataset.act) {
    const i = Number(b.closest('.block').dataset.i);
    if (b.dataset.act === 'del') blocks.splice(i, 1);
    if (b.dataset.act === 'dup') blocks.splice(i + 1, 0, structuredClone(blocks[i]));
    if (b.dataset.act === 'up' && i > 0) [blocks[i - 1], blocks[i]] = [blocks[i], blocks[i - 1]];
    if (b.dataset.act === 'down' && i < blocks.length - 1) [blocks[i + 1], blocks[i]] = [blocks[i], blocks[i + 1]];
    state.dirty = true;
    return refreshRows();
  }
  switch (b.id) {
    case 'wo-new': if (confirmDiscard()) { newWorkout({ name: 'My workout', description: '', blocks: [{ type: 'warmup', ...NEW_BLOCK.warmup }, { type: 'cooldown', ...NEW_BLOCK.cooldown }] }, null); render(); } break;
    case 'wo-gen-open': openGenerator(); break;
    case 'wo-gen-close': $('#wo-gen').close(); break;
    case 'wo-gen-apply': applyGenerator(); break;
    case 'wo-save': await save(); break;
    case 'wo-download': download(); break;
    case 'wo-delete': await remove(); break;
    case 'wo-import-btn': $('#wo-import').click(); break;
    default: break;
  }
}

const confirmDiscard = () => !state.dirty || confirm('Discard the changes to this workout?');

async function openFile(name) {
  if (!confirmDiscard()) return;
  try {
    const xml = await api.workout(name);
    if (!xml) throw new Error('not found');
    newWorkout(fromZwo(xml, name.replace(/\.zwo$/i, '')), name);
    render();
  } catch (err) { toast(`Could not open ${name}: ${err.message}`, 'bad'); }
}

async function onImport(e) {
  const f = e.target.files[0];
  e.target.value = '';
  if (!f || !confirmDiscard()) return;
  try {
    newWorkout(fromZwo(await f.text(), f.name.replace(/\.zwo$/i, '')), null);
    state.dirty = true;
    render();
    toast(`Imported ${f.name}: save it to put it on the device`);
  } catch (err) { toast(err.message, 'bad'); }
}

async function save() {
  const w = state.w;
  if (!w.name.trim()) return toast('Give the workout a name first', 'bad');
  if (!w.blocks.length) return toast('The workout has no blocks', 'bad');
  const name = fileName(w.name);
  try {
    if (name !== state.file && state.list.some((f) => f.name === name) && !confirm(`Replace "${name}" on the device?`)) return;
    await api.saveWorkout(name, toZwo(w));
    if (state.file && state.file !== name && confirm(`Saved as "${name}". Delete the old "${state.file}"?`)) await api.deleteWorkout(state.file);
    state.file = name;
    state.dirty = false;
    await refreshList();
    render();
    toast(`Saved to the device: ${name}`);
  } catch (err) { toast(`Save failed: ${err.message}`, 'bad'); }
}

function download() {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([toZwo(state.w)], { type: 'application/xml' }));
  a.download = fileName(state.w.name);
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function remove() {
  if (!state.file || !confirm(`Delete "${state.file}" from the device?`)) return;
  try {
    await api.deleteWorkout(state.file);
    toast(`Deleted ${state.file}`);
    state.file = null;
    state.dirty = true;
    await refreshList();
    render();
  } catch (err) { toast(`Delete failed: ${err.message}`, 'bad'); }
}

// ---- Generator dialog ---------------------------------------------------------------------

const gen = { type: 'sweetspot', minutes: 60, difficulty: 3, adapt: true };

function openGenerator() {
  $('#gen-types').innerHTML = Object.entries(GEN_TYPES).map(([k, t]) => `<button class="gen-type${k === gen.type ? ' active' : ''}"
    data-type="${k}" style="--zc:${zoneColor(t.zone)}"><b>${esc(t.label)}</b><span>${esc(t.about)}</span></button>`).join('');
  $('#gen-min').value = gen.minutes;
  $('#gen-diff').value = gen.difficulty;
  $('#gen-adapt').checked = gen.adapt;
  const ctx = state.ctx;
  $('#gen-adapt-note').textContent = ctx.ftp ? `Uses your FTP (${ctx.ftp} W), form and power bests from your rides.`
    : 'Ride a few times first: then targets follow your form and power bests.';
  previewGen();
  const dlg = $('#wo-gen');
  dlg.onclick = (e) => { const t = e.target.closest('[data-type]'); if (t) { gen.type = t.dataset.type; $$('.gen-type', dlg).forEach((x) => x.classList.toggle('active', x === t)); previewGen(); } };
  dlg.oninput = () => { gen.minutes = Number($('#gen-min').value); gen.difficulty = Number($('#gen-diff').value); gen.adapt = $('#gen-adapt').checked; previewGen(); };
  dlg.showModal();
}

function previewGen() {
  $('#gen-min-v').textContent = `${gen.minutes} min`;
  $('#gen-diff-v').textContent = ['Easy', 'Moderate', 'Standard', 'Hard', 'Very hard'][gen.difficulty - 1];
  $('#gen-min').disabled = gen.type === 'ramptest';
  const w = generate(gen.type, gen.minutes, gen.difficulty, state.ctx, gen.adapt);
  const L = load(w, state.ftp);
  $('#gen-preview').innerHTML = `${profileSvg(w, { height: 120 })}<div class="gen-sum"><b>${esc(w.name)}</b>
    <span>${fmt.dur(L.secs)} · TSS ${fmt.num(L.tss)} · IF ${fmt.num(L.if, 2)}</span><p>${esc(w.description)}</p></div>`;
}

function applyGenerator() {
  if (!confirmDiscard()) return;
  newWorkout(generate(gen.type, gen.minutes, gen.difficulty, state.ctx, gen.adapt), null);
  state.dirty = true;
  $('#wo-gen').close();
  render();
}

