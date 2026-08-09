import { API } from '../api.js';
import { $, esc, log } from '../util.js';
import { S } from '../state.js';


export function enterTriggers() { loadTrigWorkflows(); renderTriggerList(); }

// ─── Triggers page ──────────────────────────────────────────────────────
let trigEditing = null;   // trigger object when editing, null when adding

async function loadTrigWorkflows() {
  try {
    const graphs = await API.get('/api/graphs');
    const sel = $('trig-wf');
    sel.innerHTML = '';
    for (const g of graphs) {
      const o = document.createElement('option');
      o.value = g.id; o.textContent = g.name;
      if (g.id === S.graphId) o.selected = true;
      sel.appendChild(o);
    }
  } catch (e) {
    console.error('loadTrigWorkflows failed:', e);
    $('trig-form-err').textContent = 'could not load workflows: ' + (e.message || e);
  }
}

function syncTrigKindRows() {
  const kind = $('trig-kind').value;
  document.querySelectorAll('.trig-form-row[data-kinds]').forEach(row => {
    row.classList.toggle('hidden', !row.dataset.kinds.split(' ').includes(kind));
  });
}
$('trig-kind').addEventListener('change', syncTrigKindRows);

async function openTrigModal(existing) {
  trigEditing = existing || null;
  await loadTrigWorkflows();   // always refresh so dropdown is populated
  $('trig-modal-title').textContent = existing ? 'Edit trigger' : 'Add trigger';
  $('trig-form-err').textContent = '';
  const sch = existing?.schedule;
  const type = existing ? (existing.kind === 'webhook' ? 'webhook' : sch?.type || 'daily') : 'daily';
  $('trig-kind').value = type;
  $('trig-wf').disabled = !!existing;
  $('trig-kind').disabled = !!existing;
  if (existing) {
    $('trig-wf').value = existing.graph_id;
    if (sch?.at) $('trig-once-at').value = sch.at;
    if (sch?.time) $('trig-time').value = sch.time;
    if (sch?.every_minutes) $('trig-mins').value = sch.every_minutes;
    const days = sch?.days || [1];
    document.querySelectorAll('#trig-days input').forEach(cb => {
      cb.checked = days.includes(Number(cb.value));
    });
  }
  syncTrigKindRows();
  $('trig-modal').classList.remove('hidden');
}
const closeTrigModal = () => { $('trig-modal').classList.add('hidden'); trigEditing = null; };

$('trig-add').addEventListener('click', () => openTrigModal(null));
$('trig-cancel').addEventListener('click', closeTrigModal);
$('trig-modal').addEventListener('click', (e) => { if (e.target === $('trig-modal')) closeTrigModal(); });
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !$('trig-modal').classList.contains('hidden')) closeTrigModal();
});

function readTrigForm() {
  const errEl = $('trig-form-err');
  errEl.textContent = '';
  const kind = $('trig-kind').value;
  if (kind === 'webhook') return { body: { kind: 'webhook' } };
  let schedule;
  if (kind === 'once') {
    const at = $('trig-once-at').value;
    if (!at) { errEl.textContent = 'pick a date + time'; return null; }
    schedule = { type: 'once', at };
  } else if (kind === 'daily') {
    schedule = { type: 'daily', time: $('trig-time').value || '09:00' };
  } else if (kind === 'weekly') {
    const days = [...document.querySelectorAll('#trig-days input:checked')].map(cb => Number(cb.value));
    if (!days.length) { errEl.textContent = 'pick at least one day'; return null; }
    schedule = { type: 'weekly', days, time: $('trig-time').value || '09:00' };
  } else {
    const mins = Number($('trig-mins').value);
    if (!mins || mins < 1) { errEl.textContent = 'minutes must be ≥ 1'; return null; }
    schedule = { type: 'interval', every_minutes: mins };
  }
  return { schedule };
}

$('trig-save').addEventListener('click', async () => {
  const errEl = $('trig-form-err');
  const form = readTrigForm();
  if (!form) return;
  try {
    if (trigEditing) {
      await API.put(`/api/triggers/${trigEditing.id}`, { schedule: form.schedule });
    } else {
      const gid = $('trig-wf').value;
      if (!gid) { errEl.textContent = 'pick a workflow first'; return; }
      const body = form.body || { kind: 'schedule', schedule: form.schedule };
      await API.post(`/api/graphs/${gid}/triggers`, body);
    }
    log(trigEditing ? 'trigger updated' : 'trigger added');
    closeTrigModal();
    renderTriggerList();
  } catch (e) { errEl.textContent = String(e.message || e); }
});

function trigCard(t) {
  const card = document.createElement('div');
  card.className = 'trig-card' + (t.enabled ? '' : ' disabled');
  const icon = t.kind === 'webhook' ? '🔗' : '🕐';
  const desc = t.kind === 'webhook' ? 'Webhook (external HTTP POST)'
                                    : esc(t.description || 'Scheduled');
  card.innerHTML = `
    <div class="trig-card-head">
      <span>${icon}</span>
      <span class="trig-wf-name">${esc(t.graph_name || '')}</span>
      <span class="grow trig-desc">${desc}</span>
      <label title="Enabled"><input type="checkbox" data-en ${t.enabled ? 'checked' : ''}></label>
      ${t.kind === 'schedule' ? '<button class="btn" data-edit>Edit</button>' : ''}
      <button class="btn danger" data-del>Delete</button>
    </div>
    <div class="trig-meta">
      <span>next: ${t.enabled && t.next_fire_at ? esc(t.next_fire_at.replace('T', ' ').slice(0, 16)) : '—'}</span>
      <span>last fired: ${t.last_fired_at ? esc(t.last_fired_at.replace('T', ' ').slice(0, 16)) : 'never'}</span>
    </div>
    ${t.kind === 'webhook' && t.url ? `
    <div class="trig-url">
      <code>${esc(location.origin + t.url)}</code><br>
      <code>curl -X POST ${esc(location.origin + t.url)} -d '{"topic":"..."}'</code>
    </div>` : ''}`;
  card.querySelector('[data-del]').addEventListener('click', async () => {
    if (!confirm('Delete this trigger?')) return;
    try { await API.del(`/api/triggers/${t.id}`); renderTriggerList(); }
    catch (e) { log(`delete failed: ${e.message || e}`); }
  });
  card.querySelector('[data-en]').addEventListener('change', async (e) => {
    try { await API.put(`/api/triggers/${t.id}`, { enabled: e.target.checked }); renderTriggerList(); }
    catch (err) { log(`toggle failed: ${err.message || err}`); }
  });
  const editBtn = card.querySelector('[data-edit]');
  if (editBtn) editBtn.addEventListener('click', () => openTrigModal(t));
  return card;
}

async function renderTriggerList() {
  const list = $('trig-list');
  let triggers = [];
  try { triggers = await API.get('/api/triggers'); }
  catch (e) { list.innerHTML = `<div class="insp-help">failed to load: ${esc(e.message || e)}</div>`; return; }
  list.innerHTML = '';
  if (!triggers.length) {
    list.innerHTML = '<div class="insp-help">No triggers yet — click + Add Trigger.</div>';
    return;
  }
  for (const t of triggers) list.appendChild(trigCard(t));
}

