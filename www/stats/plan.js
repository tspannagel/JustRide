// Training plans: a multi-week schedule built in the browser and stored on the SD card
// (/plans/<user>.json). The next 7 days are written to the device as ordinary workouts
// ("Plan 2026-11-02 Sweet spot.zwo", listed first on the device), generated at sync
// time so they follow the rider's current form. Files the rider edited are left alone.
// A missed key session moves to the next easy day (see reschedule()).
import { $, api, esc, fmt, icon, toast, todayKey, addDays, dated, dayKey, empty } from './core.js';
import { generate, toZwo, load, profileSvg, GEN_TYPES, fileName } from './workouts.js';

const GOALS = {
  ftp: { label: 'Raise my FTP', about: 'Sweet spot, threshold and VO2max with endurance between; ramp tests at the start and end.',
    hard: ['sweetspot', 'vo2', 'threshold'] },
  base: { label: 'Build endurance', about: 'Mostly long, easy riding with tempo and sweet spot once or twice a week.',
    hard: ['tempo', 'sweetspot'] },
  maintain: { label: 'Stay in shape', about: 'Two quality sessions a week, the rest easy.', hard: ['sweetspot', 'vo2'] },
};
const DAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
const SYNC_DAYS = 7;
const EASY = new Set(['endurance', 'recovery']);
const MOVE_DAYS = 7;   // a missed key session moves at most this far ahead of today

// 0 = Monday
const dow = (key) => (new Date(key + 'T00:00:00Z').getUTCDay() + 6) % 7;
// Monday of the week containing key
const monday = (key) => addDays(key, -dow(key));

// Sessions for a plan setup: {goal, start, weeks, days: [0..6], longDay, hours}
export function buildPlan(setup) {
  const { goal, weeks, hours } = setup;
  const days = [...setup.days].sort((a, b) => a - b);
  const longDay = days.includes(setup.longDay) ? setup.longDay : days[days.length - 1];
  const g = GOALS[goal];
  const nHard = Math.min(goal === 'ftp' ? 3 : 2, days.length <= 2 ? 1 : days.length <= 4 ? 2 : 3);
  // Hard days: spread out, never the long day, avoid back-to-back where possible
  const others = days.filter((d) => d !== longDay);
  const hardDays = [];
  for (const d of others) {
    if (hardDays.length >= nHard) break;
    if (!hardDays.some((h) => Math.abs(h - d) === 1)) hardDays.push(d);
  }
  for (const d of others) if (hardDays.length < nHard && !hardDays.includes(d)) hardDays.push(d);

  const weight = (d) => (d === longDay ? 1.7 : hardDays.includes(d) ? 1.0 : 0.85);
  const wsum = days.reduce((a, d) => a + weight(d), 0);
  const sessions = [];
  const start = monday(setup.start);
  for (let w = 0; w < weeks; w++) {
    const recovery = (w + 1) % 4 === 0 && w !== weeks - 1;
    const last = w === weeks - 1 && weeks >= 4;
    const phase = w % 4;                                   // 0,1,2 build, 3 recovery
    const vol = recovery ? 0.65 : last ? 0.75 : [0.9, 1.0, 1.1][phase];
    const diff = recovery ? 2 : last ? 3 : Math.min(5, 2 + phase + Math.floor(w / 4));
    let hardIdx = w, tested = !(goal === 'ftp' && (w === 0 || last));
    for (const d of days) {
      const date = addDays(start, w * 7 + d);
      if (date < setup.start) continue;
      let minutes = Math.round(((hours * 60 * weight(d)) / wsum) * vol / 5) * 5;
      minutes = Math.min(240, Math.max(30, minutes));
      let type = 'endurance';
      if (hardDays.includes(d)) type = recovery ? (hardDays[0] === d ? 'sweetspot' : 'recovery') : g.hard[hardIdx++ % g.hard.length];
      if (!tested && hardDays.includes(d)) { type = 'ramptest'; minutes = 30; tested = true; }  // first hard day of the first/last week
      if (type === 'recovery') minutes = Math.min(minutes, 45);
      sessions.push({ date, type, minutes, difficulty: type === 'endurance' && d === longDay ? Math.min(diff, 3) : diff, week: w });
    }
  }
  return { version: 1, ...setup, longDay, start: setup.start, created: new Date().toISOString(), sessions, synced: {} };
}

// ---- Missed sessions ------------------------------------------------------------------------
// A session counts as ridden if any ride was recorded that (local) day. A missed key session
// (anything but endurance/recovery) moves once to the next easy day within MOVE_DAYS, not the
// long ride, preferably with no key session the day before or after; the easy session there
// is dropped, so the week's load stays about the same. Missed easy sessions, sessions that
// were already moved and key sessions with no free day are dropped. Sessions are only ever
// moved forward and stay in the plan (dropped: 'missed' | 'replaced', orig: first date).

// Rides by local day: Map(date key -> {tss, secs})
export function rideDays(rides) {
  const m = new Map();
  for (const r of rides.filter(dated)) {
    const k = dayKey(new Date(r.start_utc.getTime() - r.start_utc.getTimezoneOffset() * 60e3));
    const o = m.get(k) || { tss: 0, secs: 0 };
    o.tss += r.tss || 0; o.secs += r.duration_s || 0;
    m.set(k, o);
  }
  return m;
}

// Moves/drops missed sessions in place. Returns the sessions that moved.
export function reschedule(plan, ridden, today) {
  const moved = [];
  const live = () => plan.sessions.filter((x) => !x.dropped);
  // Newest first: when there are fewer free days than missed sessions, the latest (furthest
  // along in the build-up) gets one
  const missed = live().filter((x) => x.date < today && !ridden.has(x.date)).sort((a, b) => b.date.localeCompare(a.date));
  for (const s of missed) {
    if (EASY.has(s.type) || s.orig || s.date < addDays(today, -MOVE_DAYS)) { s.dropped = 'missed'; continue; }
    const keyOn = (d) => live().some((x) => x !== s && x.date === d && !EASY.has(x.type));
    const slots = live().filter((x) => EASY.has(x.type) && x.date >= today && x.date < addDays(today, MOVE_DAYS) &&
      dow(x.date) !== plan.longDay && !ridden.has(x.date)).sort((a, b) => a.date.localeCompare(b.date));
    const slot = slots.find((x) => !keyOn(addDays(x.date, -1)) && !keyOn(addDays(x.date, 1))) || slots[0];
    if (!slot) { s.dropped = 'missed'; continue; }
    slot.dropped = 'replaced';
    s.orig = s.date;
    s.date = slot.date;
    moved.push(s);
  }
  return moved;
}

// The next session still to ride (today included), after moving missed ones
export function nextSession(plan, rides, today) {
  if (!plan?.sessions?.length) return null;
  reschedule(plan, rideDays(rides), today);
  return plan.sessions.filter((s) => !s.dropped && s.date >= today).sort((a, b) => a.date.localeCompare(b.date))[0] || null;
}

// ---- Sync to the device -----------------------------------------------------------------

const planFile = (s, rider) => fileName(`Plan ${s.date} ${rider ? `${rider} - ` : ''}${GEN_TYPES[s.type].label}`);
const hash = (t) => { let h = 2166136261; for (let i = 0; i < t.length; i++) h = Math.imul(h ^ t.charCodeAt(i), 16777619); return (h >>> 0).toString(36); };

// Writes the next days, removes this plan's older files. Returns {written, removed, kept}.
export async function syncPlan(user, plan, ctx, riderLabel) {
  const today = todayKey(), until = addDays(today, SYNC_DAYS - 1);
  const res = { written: 0, removed: 0, kept: 0, same: 0, moved: reschedule(plan, rideDays(ctx.rides || []), today) };
  const want = new Map();
  for (const s of plan.sessions) if (!s.dropped && s.date >= today && s.date <= until) want.set(planFile(s, riderLabel), s);
  const synced = plan.synced || {};
  // Remove our files that are no longer wanted (past days, moved or changed plan); a coming
  // day's file the rider edited stays (it's theirs now)
  for (const name of Object.keys(synced)) {
    if (want.has(name)) continue;
    const day = name.match(/^Plan (\d{4}-\d{2}-\d{2})/)?.[1];
    const cur = day >= today ? await api.workout(name).catch(() => null) : null;
    if (!cur || hash(cur) === synced[name]) {
      try { await api.deleteWorkout(name); res.removed++; } catch { /* already gone */ }
    }
    delete synced[name];
  }
  const onDevice = new Set((await api.workouts()).map((f) => f.name));
  for (const [name, s] of want) {
    if (onDevice.has(name) && synced[name]) {
      const cur = await api.workout(name);
      if (cur && hash(cur) !== synced[name]) { res.kept++; continue; }   // the rider edited it
    } else if (onDevice.has(name)) { res.kept++; continue; }            // not ours
    const w = generate(s.type, s.minutes, s.difficulty, ctx, true);
    w.name = name.replace(/\.zwo$/, '');
    const xml = toZwo(w);
    if (onDevice.has(name) && synced[name] === hash(xml)) { res.same++; continue; }  // no SD write needed
    await api.saveWorkout(name, xml);
    synced[name] = hash(xml);
    res.written++;
  }
  plan.synced = synced;
  plan.lastSync = new Date().toISOString();
  await api.savePlan(user.id, plan);
  return res;
}

// ---- View -------------------------------------------------------------------------------

const ui = { user: null, plan: null, setup: null, ctxFn: null, rides: [], riderLabel: '' };

export function initPlan(ctxFn) {
  ui.ctxFn = ctxFn;
  const root = $('#plan');
  root.addEventListener('click', onClick);
  root.addEventListener('input', onSetupInput);
}

export async function showPlan(user, rides, riderLabel) {
  ui.user = user;
  ui.rides = rides;
  ui.riderLabel = riderLabel;
  try { ui.plan = await api.plan(user.id); } catch { ui.plan = null; }
  if (!ui.plan?.sessions?.length) ui.plan = null;   // ended (or never made)
  else reschedule(ui.plan, rideDays(rides), todayKey());   // saved with the next sync
  render();
}

function defaults() {
  return { goal: 'ftp', start: todayKey(), weeks: 8, days: [1, 3, 5, 6], longDay: 6, hours: 5 };
}

function render() {
  const p = ui.plan;
  $('#plan-setup').hidden = !!p && !ui.setup;
  $('#plan-view').hidden = !p || !!ui.setup;
  if (!p || ui.setup) {
    ui.setup = ui.setup || defaults();
    renderSetup();
  } else renderPlan();
}

function renderSetup() {
  const s = ui.setup;
  $('#plan-goals').innerHTML = Object.entries(GOALS).map(([k, g]) => `<button class="gen-type${k === s.goal ? ' active' : ''}"
    data-goal="${k}"><b>${esc(g.label)}</b><span>${esc(g.about)}</span></button>`).join('');
  $('#plan-days').innerHTML = DAYS.map((d, i) => `<button class="day-pill${s.days.includes(i) ? ' on' : ''}${i === s.longDay ? ' long' : ''}"
    data-day="${i}" title="Tap: ride this day. Tap again on a riding day: make it the long ride.">${d}</button>`).join('');
  $('#plan-weeks').value = s.weeks;
  $('#plan-hours').value = s.hours;
  $('#plan-start').value = s.start;
  $('#plan-weeks-v').textContent = `${s.weeks} weeks`;
  $('#plan-hours-v').textContent = `${s.hours} h / week`;
  $('#plan-cancel').hidden = !ui.plan;
  const preview = s.days.length ? buildPlan(s) : null;
  $('#plan-preview').innerHTML = preview ? calendar(preview, true) : empty('Pick at least one day', 'Choose the days you can ride.', 'calendar');
}

function onSetupInput(e) {
  const s = ui.setup;
  if (!s) return;
  if (e.target.id === 'plan-weeks') s.weeks = Number(e.target.value);
  else if (e.target.id === 'plan-hours') s.hours = Number(e.target.value);
  else if (e.target.id === 'plan-start' && e.target.value) s.start = e.target.value;
  else return;
  renderSetup();
}

// Actual rides by day, for ticks and the weekly bars
const actualByDay = () => rideDays(ui.rides);

function calendar(plan, preview = false) {
  const ctx = ui.ctxFn();
  const today = todayKey(), done = actualByDay();
  const byDate = new Map(), movedFrom = new Map();
  for (const s of plan.sessions) {
    if (!s.dropped || !byDate.has(s.date)) byDate.set(s.date, s);   // the live session wins
    if (s.orig) movedFrom.set(s.orig, s);
  }
  const start = monday(plan.start);
  let html = `<div class="cal"><div class="cal-head"><span></span>${DAYS.map((d) => `<span>${d}</span>`).join('')}<span>Load</span></div>`;
  for (let w = 0; w < plan.weeks; w++) {
    let planned = 0, actual = 0;
    const cells = DAYS.map((_, d) => {
      const date = addDays(start, w * 7 + d), s = byDate.get(date), a = done.get(date);
      if (a && !preview) actual += a.tss;
      const cls = ['cal-day', date === today ? 'today' : '', date < today ? 'past' : ''].join(' ');
      const m = !s && !preview && movedFrom.get(date);
      if (m) {
        return `<div class="${cls} has moved" style="--zc:var(--z${GEN_TYPES[m.type].zone + 1})" title="Missed, moved to ${DAYS[dow(m.date)]} ${Number(m.date.slice(8))}">
          <span class="cal-date">${Number(date.slice(8))}</span><b>${esc(GEN_TYPES[m.type].label)}</b>
          <span class="cal-meta">moved to ${DAYS[dow(m.date)]} ${Number(m.date.slice(8))}</span>${a ? `<span class="cal-extra">${icon('check')} ${fmt.num(a.tss)} TSS</span>` : ''}</div>`;
      }
      if (!s) return `<div class="${cls}"><span class="cal-date">${Number(date.slice(8))}</span>${a && !preview ? `<span class="cal-extra">${icon('check')} ${fmt.num(a.tss)} TSS</span>` : ''}</div>`;
      const wk = generate(s.type, s.minutes, s.difficulty, ctx, false);
      const L = load(wk, ctx.ftp || 200);
      if (!s.dropped) planned += L.tss;
      const state = preview ? '' : a ? 'done' : s.dropped || date < today ? 'missed' : '';
      return `<div class="${cls} has ${state}" style="--zc:var(--z${GEN_TYPES[s.type].zone + 1})" title="${esc(wk.name)}">
        <span class="cal-date">${Number(date.slice(8))}${state === 'done' ? icon('check') : ''}</span>
        <b>${esc(GEN_TYPES[s.type].label)}</b><span class="cal-meta">${s.orig && !preview ? `from ${DAYS[dow(s.orig)]} · ` : ''}${s.minutes} min · ${fmt.num(L.tss)}</span>
        ${profileSvg(wk, { height: 28, width: 140, compact: true })}</div>`;
    }).join('');
    const max = Math.max(planned, actual, 1);
    const recovery = (w + 1) % 4 === 0 && w !== plan.weeks - 1;
    html += `<div class="cal-week"><span class="cal-wk">W${w + 1}${recovery ? '<i>easy</i>' : ''}</span>${cells}
      <div class="cal-load"><div class="bar plan" style="--w:${(planned / max) * 100}%"><span>${fmt.num(planned)}</span></div>
      ${preview ? '' : `<div class="bar done" style="--w:${(actual / max) * 100}%"><span>${fmt.num(actual)}</span></div>`}</div></div>`;
  }
  return html + '</div>';
}

function renderPlan() {
  const p = ui.plan, today = todayKey();
  const end = addDays(monday(p.start), p.weeks * 7 - 1);
  const week = Math.min(p.weeks, Math.max(1, Math.floor((new Date(today) - new Date(monday(p.start))) / (7 * 86400e3)) + 1));
  const next = nextSession(p, ui.rides, today);
  const done = actualByDay();
  const past = p.sessions.filter((s) => s.dropped !== 'replaced' && s.date < today);
  const hit = past.filter((s) => !s.dropped && done.has(s.date)).length;
  $('#plan-title').textContent = GOALS[p.goal]?.label || 'Training plan';
  $('#plan-sub').textContent = today > end ? `Finished on ${fmt.date(new Date(end))}` :
    `Week ${week} of ${p.weeks} · ${past.length ? `${hit} of ${past.length} sessions ridden` : 'starts ' + fmt.date(new Date(p.start))}`;
  $('#plan-progress').style.setProperty('--p', `${Math.min(100, (week / p.weeks) * 100)}%`);
  $('#plan-next').innerHTML = next ? `<span class="eyebrow">${next.date === today ? 'Today' : fmt.date(new Date(next.date))}</span>
    <b>${esc(GEN_TYPES[next.type].label)} · ${next.minutes} min</b>${next.orig ? `<span class="muted small">Moved from ${fmt.date(new Date(next.orig))}, when it was missed</span>` : ''}<span class="muted small">On the device under Workouts as "${esc(planFile(next, ui.riderLabel).replace(/\.zwo$/, ''))}"</span>`
    : '<b>No sessions left</b>';
  $('#plan-sync-info').textContent = p.lastSync ? `Last sent to the device ${fmt.rel(new Date(p.lastSync))} ${new Date(p.lastSync).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}` : 'Not sent to the device yet';
  $('#plan-cal').innerHTML = calendar(p);
}

async function onClick(e) {
  const b = e.target.closest('button');
  if (!b) return;
  const s = ui.setup;
  if (b.dataset.goal) { s.goal = b.dataset.goal; return renderSetup(); }
  if (b.dataset.day) {
    const d = Number(b.dataset.day);
    if (!s.days.includes(d)) s.days.push(d);
    else if (s.longDay !== d) s.longDay = d;
    else { s.days = s.days.filter((x) => x !== d); s.longDay = s.days[s.days.length - 1]; }
    return renderSetup();
  }
  switch (b.id) {
    case 'plan-create': {
      if (!s.days.length) return toast('Pick at least one day', 'bad');
      const plan = buildPlan(s);
      if (ui.plan?.synced) plan.synced = ui.plan.synced;   // so the old plan's files get replaced
      ui.plan = plan;
      ui.setup = null;
      await sync(true);
      return render();
    }
    case 'plan-edit': {
      const p = ui.plan;
      ui.setup = { goal: p.goal, start: todayKey(), weeks: Math.max(1, p.weeks - Math.max(0, Math.floor((new Date(todayKey()) - new Date(monday(p.start))) / (7 * 86400e3)))),
        days: [...p.days], longDay: p.longDay, hours: p.hours };
      return render();
    }
    case 'plan-cancel': ui.setup = null; return render();
    case 'plan-sync': await sync(true); return render();
    case 'plan-end': {
      if (!confirm('End this plan? Its workouts are removed from the device.')) return;
      ui.plan.sessions = [];
      await sync(false);
      try { await api.savePlan(ui.user.id, { version: 1, ended: new Date().toISOString(), sessions: [], synced: {} }); } catch { /* ignore */ }
      ui.plan = null;
      toast('Plan ended');
      return render();
    }
    default: break;
  }
}

async function sync(report) {
  try {
    const r = await syncPlan(ui.user, ui.plan, ui.ctxFn(), ui.riderLabel);
    if (report) toast(`Device updated: ${r.written} workout${r.written === 1 ? '' : 's'} written${r.removed ? `, ${r.removed} old removed` : ''}${r.kept ? `, ${r.kept} edited kept` : ''}${r.moved.length ? `, ${r.moved.length} missed moved` : ''}`);
  } catch (err) { toast(`Could not update the device: ${err.message}`, 'bad'); }
}

// Called on start-up for the active rider: keeps the next week on the device
export async function autoSync(user, ctx, riderLabel) {
  let plan;
  try { plan = await api.plan(user.id); } catch { return null; }
  if (!plan?.sessions?.length) return null;
  try { return await syncPlan(user, plan, ctx, riderLabel); } catch { return null; }
}

