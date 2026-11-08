// Training plans: a multi-week schedule built in the browser and stored on the SD card
// (/plans/<user>.json). The next 7 days are written to the device as ordinary workouts
// ("Plan 2026-11-02 Sweet spot.zwo", listed first on the device), generated at sync
// time so they follow the rider's current form. Files the rider edited are left alone.
// The plan follows real life: missed sessions, away days and breaks rearrange it (replan()).
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

// 0 = Monday
const dow = (key) => (new Date(key + 'T00:00:00Z').getUTCDay() + 6) % 7;
// Monday of the week containing key
const monday = (key) => addDays(key, -dow(key));

// Sessions for a plan setup: {goal, start, weeks, days: [0..6], longDay, hours}.
// firstTest: the rider's FTP is still a guess, so the first hard day is a ramp test.
export function buildPlan(setup, firstTest = false) {
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
    let hardIdx = w, tested = !((goal === 'ftp' || firstTest) && w === 0) && !(goal === 'ftp' && last);
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
  return { version: 2, ...setup, longDay, start: setup.start, created: new Date().toISOString(), sessions, synced: {} };
}

// ---- Loading --------------------------------------------------------------------------------
// The plan file is on the SD card, which any device on the network can write: keep only what
// this app understands, with the types it expects (it ends up in the page and decides which
// workouts on the device get replaced or deleted).

const DAY_KEY = /^\d{4}-\d{2}-\d{2}$/;
const PLAN_FILE = /^Plan \d{4}-\d{2}-\d{2} [A-Za-z0-9 ._-]*\.zwo$/;   // what planFile() makes
const clampInt = (v, lo, hi, def) => { const n = Math.round(Number(v)); return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : def; };

export function cleanPlan(p) {
  if (!p || typeof p !== 'object' || !Array.isArray(p.sessions)) return null;
  // Version 1 plans moved missed sessions in the file (orig = planned date): back to the plan
  const sessions = p.sessions
    .filter((s) => s && typeof s === 'object' && DAY_KEY.test(s.date) && Object.hasOwn(GEN_TYPES, s.type) &&
      (s.orig === undefined || DAY_KEY.test(s.orig)))
    .map((s) => ({ date: s.orig || s.date, type: s.type, minutes: clampInt(s.minutes, 10, 240, 60),
      difficulty: clampInt(s.difficulty, 1, 5, 3), week: clampInt(s.week, 0, 52, 0) }));
  const avail = {};
  for (const [d, v] of Object.entries(p.avail && typeof p.avail === 'object' ? p.avail : {}).slice(0, 400)) {
    if (DAY_KEY.test(d) && (v === 'off' || v === 'on')) avail[d] = v;
  }
  const synced = {};
  for (const [name, h] of Object.entries(p.synced && typeof p.synced === 'object' ? p.synced : {})) {
    if (PLAN_FILE.test(name) && typeof h === 'string') synced[name] = h;
  }
  return { ...p, sessions, avail, synced, weeks: clampInt(p.weeks, 1, 52, 1),
    start: DAY_KEY.test(p.start) ? p.start : sessions[0]?.date || todayKey(),
    goal: Object.hasOwn(GOALS, p.goal) ? p.goal : 'maintain',
    hours: Math.min(14, Math.max(2, Number(p.hours) || 5)),
    longDay: clampInt(p.longDay, 0, 6, 6), days: Array.isArray(p.days) ? p.days.map((d) => clampInt(d, 0, 6, 0)) : [] };
}

export const readPlan = async (user) => cleanPlan(await api.plan(user));

// ---- Following real life --------------------------------------------------------------------
// plan.sessions is the plan as it was made and never changes. replan() works out where each
// session is now, from the rides and the days the rider marked (plan.avail: date -> 'off' away,
// 'on' extra riding day). It replays the plan day by day, as the app would have shown it then:
// - Each week, a session keeps its day while that day is free. The others (missed, or on a day
//   marked away) take a free day of the same week (an extra day the rider marked), else push out
//   the least important session they can (ramp test > key session > long ride > easy; for "Build
//   endurance" the long ride comes before the key sessions); if possible not right next to another
//   hard day, then the nearest.
// - A ride of a plan workout ("Plan <date> ..." in the ride's workout name) counts as the session
//   of that day, also when ridden early. Any other ride on a day without a session counts as the
//   most important session missed so far this week, or moved off an away day (rode on Tuesday
//   instead of Monday). Rides of RIDE_MIN_S or more count.
// - A key session with no day left moves into the next week once, where it only replaces an easy
//   ride (a ramp test also a key session); others are dropped.
// - After EASE_DAYS or more without riding, the sessions in the 7 days after the break are a step
//   easier (two steps after twice that).

const RIDE_MIN_S = 600;
const EASE_DAYS = 7;
const isHard = (s) => !EASY.has(s.type);
const daysBetween = (a, b) => Math.round((new Date(b) - new Date(a)) / 86400e3);
const isAvail = (plan, d) => plan.avail?.[d] === 'on' || (plan.avail?.[d] !== 'off' && plan.days.includes(dow(d)));

// wk: the week being placed; a key session left over from the week before ranks below a long ride
function rank(plan, s, wk) {
  if (s.type === 'ramptest') return 4;
  if (monday(s.date) < wk) return 1.5;
  const long = s.type === 'endurance' && dow(s.date) === plan.longDay;
  if (plan.goal === 'base') return long ? 3 : isHard(s) ? 2 : 1;
  return isHard(s) ? 3 : long ? 2 : 1;
}

// Rides by local day: Map(date key -> {tss, secs, plans: days of the plan workouts ridden})
export function rideDays(rides) {
  const m = new Map();
  for (const r of rides.filter(dated)) {
    const k = dayKey(new Date(r.start_utc.getTime() - r.start_utc.getTimezoneOffset() * 60e3));
    const o = m.get(k) || { tss: 0, secs: 0, plans: [] };
    o.tss += r.tss || 0; o.secs += r.duration_s || 0;
    const p = String(r.workout || '').match(/^Plan (\d{4}-\d{2}-\d{2}) /);
    if (p && (r.duration_s || 0) >= RIDE_MIN_S) o.plans.push(p[1]);
    m.set(k, o);
  }
  return m;
}
const rode = (ridden, d) => (ridden.get(d)?.secs || 0) >= RIDE_MIN_S;

// Where the open sessions of the week starting wk go, seen on day `now`: Map(date -> session).
// open: sessions not ridden yet; this week's and, if important enough, last week's.
function placeWeek(plan, wk, now, open, doneOn) {
  const slots = [];
  for (let i = 0; i < 7; i++) {
    const d = addDays(wk, i);
    if (d >= now && d >= plan.start && !doneOn.has(d) && isAvail(plan, d)) slots.push(d);
  }
  const prev = addDays(wk, -7);
  const rk = (s) => rank(plan, s, wk);
  const cands = open.filter((s) => monday(s.date) === wk || (monday(s.date) === prev && rank(plan, s, prev) >= 3));
  const at = new Map();
  for (const s of cands) if (monday(s.date) === wk && slots.includes(s.date)) at.set(s.date, s);
  const hardNear = (d, s) => [-1, 1].some((n) => {
    const x = at.get(addDays(d, n)) || doneOn.get(addDays(d, n));
    return x && x !== s && isHard(x);
  });
  const queue = cands.filter((s) => at.get(s.date) !== s);
  while (queue.length) {
    queue.sort((a, b) => rk(b) - rk(a) || a.date.localeCompare(b.date));
    const s = queue.shift(), r = rk(s);
    // A free day, else the least important session (a ramp test: the first day, so the sessions
    // after it use the new FTP); then not next to a hard day; then nearest (later wins a tie)
    const score = (d) => [at.has(d) && s.type !== 'ramptest' ? rk(at.get(d)) : 0, isHard(s) && hardNear(d, s) ? 1 : 0,
      Math.abs(daysBetween(s.date, d)) * 2 - (d > s.date ? 1 : 0)];
    const cmp = (a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2];
    let best = null, bs = null;
    for (const d of slots) {
      if (at.has(d) && rk(at.get(d)) >= r) continue;
      const sc = score(d);
      if (!bs || cmp(sc, bs) < 0) { best = d; bs = sc; }
    }
    if (!best) continue;
    if (at.has(best)) queue.push(at.get(best));
    at.set(best, s);
  }
  return at;
}

// The latest break of EASE_DAYS or more without riding that ended within the last week (or
// still goes on): {end: first day back, days}
function lastBreak(ridden, today) {
  const days = [...ridden.keys()].filter((d) => d <= today && rode(ridden, d)).sort();
  if (!days.length) return null;                     // no history: nothing to come back from
  const tail = daysBetween(days[days.length - 1], today) - 1;
  if (tail >= EASE_DAYS) return { end: today, days: tail };
  for (let i = days.length - 1; i > 0 && days[i] > addDays(today, -7); i--) {
    const gap = daysBetween(days[i - 1], days[i]) - 1;
    if (gap >= EASE_DAYS) return { end: days[i], days: gap };
  }
  return null;
}

// Every session with where it is now: {...session, base: planned date, date, state, eased?}.
// state: done (ridden on date), planned, missed (its day passed without a ride and no day was
// left for it), dropped (pushed out or no free day, never missed).
export function replan(plan, ridden, today) {
  const ss = plan.sessions;
  const last = addDays(monday(plan.start), plan.weeks * 7 - 1);
  const done = new Map(), doneOn = new Map(), missed = new Set();
  const open = (also) => ss.filter((s) => !done.has(s) && !also?.has(s));
  for (let t = plan.start; t <= today && t <= last; t = addDays(t, 1)) {
    const wk = monday(t), at = placeWeek(plan, wk, t, open(), doneOn);
    // The plan workout of another day, ridden early
    let s = rode(ridden, t) && ridden.get(t).plans.map((d) => d !== t && at.get(d)).find(Boolean) || at.get(t);
    if (!s && rode(ridden, t)) {   // a ride on another day: the most important session it stands in for
      const prev = addDays(wk, -7);
      s = open().filter((x) => (monday(x.date) === wk && (x.date < t || !isAvail(plan, x.date))) ||
        (monday(x.date) === prev && rank(plan, x, prev) >= 3))
        .sort((a, b) => rank(plan, b, wk) - rank(plan, a, wk) || a.date.localeCompare(b.date))[0];
    }
    if (!s) continue;
    if (rode(ridden, t)) { done.set(s, t); doneOn.set(t, s); }
    else if (t < today) missed.add(s);
  }
  const place = new Map();
  for (let wk = monday(today > plan.start ? today : plan.start); wk <= last; wk = addDays(wk, 7)) {
    for (const [d, s] of placeWeek(plan, wk, today, open(place), doneOn)) place.set(s, d);
  }
  const br = lastBreak(ridden, today);
  return ss.map((s) => {
    const e = { ...s, base: s.date, date: done.get(s) || place.get(s) || s.date,
      state: done.has(s) ? 'done' : place.has(s) ? 'planned' : missed.has(s) ? 'missed' : 'dropped' };
    if (br && e.state === 'planned' && e.date < addDays(br.end, 7)) {
      e.difficulty = Math.max(1, e.difficulty - (br.days >= 2 * EASE_DAYS ? 2 : 1));
      e.eased = br.days;
    }
    return e;
  });
}

const upcoming = (entries, today) => entries.filter((e) => e.state === 'planned' && e.date >= today)
  .sort((a, b) => a.date.localeCompare(b.date));

// The next session still to ride (today included)
export function nextSession(plan, rides, today) {
  if (!plan?.sessions?.length) return null;
  return upcoming(replan(plan, rideDays(rides), today), today)[0] || null;
}

// ---- Sync to the device -----------------------------------------------------------------

const planFile = (s, rider) => fileName(`Plan ${s.date} ${rider ? `${rider} - ` : ''}${GEN_TYPES[s.type].label}`);
const hash = (t) => { let h = 2166136261; for (let i = 0; i < t.length; i++) h = Math.imul(h ^ t.charCodeAt(i), 16777619); return (h >>> 0).toString(36); };

// Writes the next days, removes this plan's older files. Returns {written, removed, kept}.
export async function syncPlan(user, plan, ctx, riderLabel) {
  const today = todayKey(), until = addDays(today, SYNC_DAYS - 1);
  const want = new Map();
  for (const s of upcoming(replan(plan, rideDays(ctx.rides || []), today), today)) {
    if (s.date <= until) want.set(planFile(s, riderLabel), s);
  }
  const res = { written: 0, removed: 0, kept: 0, same: 0, moved: [...want.values()].filter((s) => s.date !== s.base) };
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

const ui = { user: null, plan: null, setup: null, ctxFn: null, rides: [], riderLabel: '', busy: false };

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
  try { ui.plan = await readPlan(user.id); } catch { ui.plan = null; }
  if (!ui.plan?.sessions?.length) ui.plan = null;   // ended (or never made)
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
  const preview = s.days.length ? buildPlan(s, !ui.ctxFn().ftpKnown) : null;
  $('#plan-preview').innerHTML = preview ? calendar(preview, preview.sessions.map((x) => ({ ...x, base: x.date, state: 'planned' })), true) : empty('Pick at least one day', 'Choose the days you can ride.', 'calendar');
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
const dayName = (d) => `${DAYS[dow(d)]} ${Number(d.slice(8))}`;

// entries: replan() output; preview: the plan as made, before any ride
function calendar(plan, entries, preview = false) {
  const ctx = ui.ctxFn();
  const today = todayKey(), done = actualByDay();
  const end = addDays(monday(plan.start), plan.weeks * 7 - 1);
  const at = new Map(), left = new Map();   // date -> session there; planned date -> sessions gone from it
  for (const e of entries) {
    if (e.state === 'planned' || e.state === 'done') at.set(e.date, e);
    if (e.date !== e.base || e.state === 'missed' || e.state === 'dropped') left.set(e.base, [...(left.get(e.base) || []), e]);
  }
  const tss = (e) => load(generate(e.type, e.minutes, e.difficulty, ctx, false), ctx.ftp || 200).tss;
  const start = monday(plan.start);
  let html = `<div class="cal"><div class="cal-head"><span></span>${DAYS.map((d) => `<span>${d}</span>`).join('')}<span>Load</span></div>`;
  for (let w = 0; w < plan.weeks; w++) {
    let planned = 0, actual = 0;
    const cells = DAYS.map((_, d) => {
      const date = addDays(start, w * 7 + d), s = at.get(date), a = !preview && done.get(date);
      const gone = preview ? [] : left.get(date) || [];
      const mark = !preview && plan.avail?.[date];
      if (a) actual += a.tss;
      const tap = !preview && date >= today && date >= plan.start && date <= end;
      const cls = ['cal-day', date === today ? 'today' : '', date < today ? 'past' : '', mark ? `avail-${mark}` : '', tap ? 'tap' : ''].join(' ');
      const attrs = tap ? ` data-date="${date}"` : '';
      const tag = mark === 'off' ? '<span class="cal-tag">Away</span>' : mark === 'on' ? '<span class="cal-tag">Extra day</span>' : '';
      const ride = a ? `<span class="cal-extra">${icon('check')} ${fmt.num(a.tss)} TSS</span>` : '';
      for (const g of gone) if (g.state === 'missed') planned += tss(g);
      if (s) {
        const wk = generate(s.type, s.minutes, s.difficulty, ctx, false);
        const L = load(wk, ctx.ftp || 200);
        planned += L.tss;
        const state = preview ? '' : s.state === 'done' ? 'done' : date < today ? 'missed' : '';
        const notes = [s.date !== s.base ? `from ${dayName(s.base)}` : '', s.eased ? 'easier' : '',
          ...gone.filter((g) => g.state === 'dropped').map((g) => `instead of ${GEN_TYPES[g.type].label}`)].filter(Boolean);
        return `<div class="${cls} has ${state}"${attrs} style="--zc:var(--z${GEN_TYPES[s.type].zone + 1})" title="${esc(wk.name)}">
          <span class="cal-date">${Number(date.slice(8))}${state === 'done' ? icon('check') : ''}${tag}</span>
          <b>${esc(GEN_TYPES[s.type].label)}</b><span class="cal-meta">${s.minutes} min · ${fmt.num(L.tss)}</span>
          ${notes.length ? `<span class="cal-meta">${esc(notes.join(' · '))}</span>` : ''}
          ${profileSvg(wk, { height: 28, width: 140, compact: true })}</div>`;
      }
      const g = gone[0];
      if (g) {
        const why = g.state === 'done' ? `ridden ${dayName(g.date)}` : g.state === 'planned' ? `moved to ${dayName(g.date)}`
          : g.state === 'missed' ? 'missed' : 'no day left';
        return `<div class="${cls} has ${g.state === 'missed' || g.state === 'dropped' ? 'missed' : 'moved'}"${attrs} style="--zc:var(--z${GEN_TYPES[g.type].zone + 1})">
          <span class="cal-date">${Number(date.slice(8))}${tag}</span><b>${esc(GEN_TYPES[g.type].label)}</b>
          <span class="cal-meta">${why}</span>${ride}</div>`;
      }
      return `<div class="${cls}"${attrs}><span class="cal-date">${Number(date.slice(8))}${tag}</span>${ride}</div>`;
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
  const entries = replan(p, actualByDay(), today);
  const next = upcoming(entries, today)[0];
  const hit = entries.filter((e) => e.state === 'done').length, due = hit + entries.filter((e) => e.state === 'missed').length;
  $('#plan-title').textContent = GOALS[p.goal]?.label || 'Training plan';
  $('#plan-sub').textContent = today > end ? `Finished on ${fmt.date(new Date(end))} · ${hit} of ${due} sessions ridden` :
    `Week ${week} of ${p.weeks} · ${due ? `${hit} of ${due} sessions ridden` : 'starts ' + fmt.date(new Date(p.start))}`;
  $('#plan-progress').style.setProperty('--p', `${Math.min(100, (week / p.weeks) * 100)}%`);
  const why = !next ? '' : [next.date !== next.base ? `Moved from ${fmt.date(new Date(next.base))}` : '',
    next.eased ? `a step easier after ${next.eased} days off` : ''].filter(Boolean).join(', ');
  $('#plan-next').innerHTML = next ? `<span class="eyebrow">${next.date === today ? 'Today' : fmt.date(new Date(next.date))}</span>
    <b>${esc(GEN_TYPES[next.type].label)} · ${next.minutes} min</b>${why ? `<span class="muted small">${esc(why)}</span>` : ''}<span class="muted small">On the device under Workouts as "${esc(planFile(next, ui.riderLabel).replace(/\.zwo$/, ''))}"</span>`
    : '<b>No sessions left</b>';
  $('#plan-sync-info').textContent = p.lastSync ? `Last sent to the device ${fmt.rel(new Date(p.lastSync))} ${new Date(p.lastSync).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}` : 'Not sent to the device yet';
  $('#plan-cal').innerHTML = calendar(p, entries);
}

// Tap a coming day: a riding day becomes "away", any other day an extra riding day; tap again
// to undo. The plan rearranges and the device gets the new week. "Away" asks first: a stray tap
// on today's session would otherwise move or drop it and remove its file from the device.
async function toggleDay(date) {
  if (ui.busy) return;
  const p = ui.plan, avail = { ...p.avail };
  if (!avail[date] && p.days.includes(dow(date))) {
    const s = replan(p, actualByDay(), todayKey()).find((e) => e.state === 'planned' && e.date === date);
    const what = s ? ` ${GEN_TYPES[s.type].label} moves to a free day this week, or is dropped if there is none.` : '';
    if (!confirm(`Mark ${date === todayKey() ? 'today' : dayName(date)} as away?${what}`)) return;
  }
  if (avail[date]) delete avail[date];
  else avail[date] = p.days.includes(dow(date)) ? 'off' : 'on';
  p.avail = avail;
  render();
  ui.busy = true;
  try { await sync(false); } finally { ui.busy = false; }
  render();
}

async function onClick(e) {
  const day = !ui.setup && e.target.closest('[data-date]');
  if (day) return toggleDay(day.dataset.date);
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
      const plan = buildPlan(s, !ui.ctxFn().ftpKnown);
      if (ui.plan?.synced) plan.synced = ui.plan.synced;   // so the old plan's files get replaced
      // Days already marked from today on stay marked
      plan.avail = Object.fromEntries(Object.entries(ui.plan?.avail || {}).filter(([d]) => d >= todayKey()));
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
    if (report) toast(`Device updated: ${r.written} workout${r.written === 1 ? '' : 's'} written${r.removed ? `, ${r.removed} old removed` : ''}${r.kept ? `, ${r.kept} edited kept` : ''}${r.moved.length ? `, ${r.moved.length} moved` : ''}`);
  } catch (err) { toast(`Could not update the device: ${err.message}`, 'bad'); }
}

// Called on start-up for the active rider: keeps the next week on the device
export async function autoSync(user, ctx, riderLabel) {
  let plan;
  try { plan = await readPlan(user.id); } catch { return null; }
  if (!plan?.sessions?.length) return null;
  try { return await syncPlan(user, plan, ctx, riderLabel); } catch { return null; }
}

