// Riders: add, rename, FTP / max ERG / weight / max HR, the active rider, delete.
// Every change goes to the device (POST /api/users): it checks the limits again, saves
// settings.json and answers with the new rider list. Not possible during a ride.
import { $, $$, api, esc, icon, toast } from './core.js';

// Same limits as the device (app_edit_user, ride.h)
const FIELDS = [
  { key: 'ftp_w', label: 'FTP', unit: 'W', min: 50, max: 600 },
  { key: 'max_erg_w', label: 'Max ERG', unit: 'W', min: 100, max: 1500 },
  { key: 'weight_kg', label: 'Weight', unit: 'kg', min: 30, max: 200 },
  { key: 'max_hr', label: 'Max HR', unit: 'bpm', min: 100, max: 230 },
];
const MAX_RIDERS = 8;
// Printable ASCII, 1..15 characters, not only spaces (settings_valid_name)
const validName = (n) => /^[\x20-\x7e]{1,15}$/.test(n) && n.trim() !== '';

let S = { users: [], onChange: () => {} };

async function post(body) {
  const list = JSON.parse(await api.send('POST', '/api/users', JSON.stringify(body)));
  S.users = list;
  S.onChange(list);
  render();
  return list;
}

function riderCard(u) {
  const wkg = u.weight_kg ? (u.ftp_w / u.weight_kg).toFixed(1) : '–';
  return `<div class="card rider-card${u.active ? ' active' : ''}" data-id="${esc(u.id)}">
    <div class="rider-head">
      <span class="avatar" aria-hidden="true">${esc(u.name.trim()[0] || '?')}</span>
      <div class="rider-title"><input class="rider-name" value="${esc(u.name)}" maxlength="15" aria-label="Name" spellcheck="false">
        <span class="wkg"><b>${wkg}</b> W/kg</span></div>
      ${u.active ? `<span class="pill good">${icon('check')} Active on the device</span>`
        : `<button class="btn ghost" data-act="select">${icon('user')} Make active</button>`}
    </div>
    <div class="rider-fields">
      ${FIELDS.map((f) => `<label class="field"><span>${f.label} <i>${f.unit}</i></span>
        <input type="number" data-key="${f.key}" min="${f.min}" max="${f.max}" value="${u[f.key]}"></label>`).join('')}
    </div>
    <div class="rider-actions">
      <button class="btn primary" data-act="save" disabled>${icon('save')} Save</button>
      <button class="btn ghost danger" data-act="delete" ${S.users.length < 2 ? 'disabled title="The last rider can\'t be deleted"' : ''}>${icon('trash')} Delete</button>
    </div>
  </div>`;
}

function render() {
  $('#riders-list').innerHTML = S.users.map(riderCard).join('');
  $('#rider-add-name').disabled = $('#rider-add button').disabled = S.users.length >= MAX_RIDERS;
  $('#rider-add-name').placeholder = S.users.length >= MAX_RIDERS ? `At most ${MAX_RIDERS} riders` : 'New rider name';
}

// The card's values, or null (with the bad inputs marked) if one is out of range
function readCard(card) {
  const name = $('.rider-name', card);
  let ok = validName(name.value);
  name.classList.toggle('bad', !ok);
  const out = { action: 'edit', id: card.dataset.id, name: name.value };
  for (const inp of $$('input[data-key]', card)) {
    const f = FIELDS.find((x) => x.key === inp.dataset.key);
    const v = Number(inp.value);
    const good = Number.isInteger(v) && v >= f.min && v <= f.max;
    inp.classList.toggle('bad', !good);
    ok = ok && good;
    out[f.key] = v;
  }
  return ok ? out : null;
}

export function initRiders(onChange) {
  S.onChange = onChange;
  $('#rider-add').onsubmit = async (e) => {
    e.preventDefault();
    const inp = $('#rider-add-name');
    if (!validName(inp.value)) { toast('Use 1–15 letters, digits or spaces', 'bad'); return; }
    try {
      await post({ action: 'add', name: inp.value });
      toast(`Added ${inp.value}. New riders start with FTP 200 W: set yours below.`);
      inp.value = '';
    } catch (err) { toast(err.message, 'bad'); }
  };
  const list = $('#riders-list');
  list.oninput = (e) => {
    const card = e.target.closest('.rider-card');
    if (!card) return;
    const u = S.users.find((x) => x.id === card.dataset.id);
    const vals = readCard(card);
    const changed = !vals || vals.name !== u.name || FIELDS.some((f) => vals[f.key] !== u[f.key]);
    $('[data-act="save"]', card).disabled = !vals || !changed;
    if (vals) $('.wkg b', card).textContent = (vals.ftp_w / vals.weight_kg).toFixed(1);
  };
  list.onclick = async (e) => {
    const b = e.target.closest('button[data-act]');
    if (!b) return;
    const card = b.closest('.rider-card');
    const u = S.users.find((x) => x.id === card.dataset.id);
    try {
      if (b.dataset.act === 'save') {
        const vals = readCard(card);
        if (!vals) return;
        await post(vals);
        toast(`Saved ${vals.name}`);
      } else if (b.dataset.act === 'select') {
        await post({ action: 'select', id: u.id });
        toast(`${u.name} is riding next`);
      } else if (b.dataset.act === 'delete') {
        if (!confirm(`Delete ${u.name}? Their rides stay on the SD card.`)) return;
        await post({ action: 'delete', id: u.id });
        toast(`Deleted ${u.name}`);
      }
    } catch (err) { toast(err.message, 'bad'); }
  };
}

export function showRiders(users) {
  S.users = users;
  render();
}
