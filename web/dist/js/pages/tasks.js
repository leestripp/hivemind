// js/pages/tasks.js — scheduled tasks (routines) management.
// Server engine: RoutineStore + 30s scheduler tick (Phase 5). This page is
// the dashboard on top: list, enable/disable, run-now, delete, cleanup.
import { API } from '../api.js';
import { $, esc, fmtTimeTZ } from '../util.js';

let tasks = [];
let agents = [];

const DAY_NAMES = ['Su', 'Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa'];

// ── data ──────────────────────────────────────────────────────────────
async function loadAll() {
  const [t, a] = await Promise.all([
    API.get('/api/routines'),
    API.get('/api/agents'),
  ]);
  tasks = t.routines || [];
  agents = a.agents || [];
  renderTasks();
}

const agentName = (id) => (agents.find(a => a.id === id) || {}).name || '—';

// Table display only: collapse the prompt to one short line — the full
// text stays available as the cell's hover tooltip (title attribute).
function truncate(prompt, max = 140) {
  const oneLine = String(prompt || '').replace(/\s+/g, ' ').trim();
  return oneLine.length <= max ? oneLine : oneLine.slice(0, max - 1) + '…';
}

function isCompleted(t) {
  // A "completed" task is a one-shot that already fired.
  return t.schedule && t.schedule.type === 'once' && !!t.last_fired_at;
}

function fmtTime(iso) { return fmtTimeTZ(iso); }   // Phase 19: tz pref

// ── table ─────────────────────────────────────────────────────────────
// 2026-10-01: grouped by agent — the flat list got hard to scan as it
// grew. One band per agent; groups with the soonest active fire first,
// fully-inactive groups last (name order, unknown agent very last).
function renderTasks() {
  const tbody = $('tasks-tbody');
  if (!tasks.length) {
    tbody.innerHTML = '<tr><td colspan="6" class="muted">No scheduled tasks. Click “+ Add Task” to create one.</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  // Within a group: enabled+scheduled first (by next fire), then the rest.
  const sortTasks = (list) => [...list].sort((a, b) => {
    const ka = a.enabled && a.next_fire_at ? 0 : 1;
    const kb = b.enabled && b.next_fire_at ? 0 : 1;
    if (ka !== kb) return ka - kb;
    return (a.next_fire_at || a.created_at || '').localeCompare(b.next_fire_at || b.created_at || '');
  });
  const groups = new Map();
  for (const t of tasks) {
    const key = t.agent_id || '';
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(t);
  }
  const nextActiveFire = (list) => {
    const fires = list.filter(t => t.enabled && t.next_fire_at && !isCompleted(t))
                      .map(t => t.next_fire_at).sort();
    return fires.length ? fires[0] : null;
  };
  const sortedGroups = [...groups.entries()].sort((a, b) => {
    const fa = nextActiveFire(a[1]), fb = nextActiveFire(b[1]);
    if (fa && fb) return fa.localeCompare(fb);
    if (fa) return -1;
    if (fb) return 1;
    const na = agentName(a[0]), nb = agentName(b[0]);
    if (na === '—' && nb !== '—') return 1;   // unknown agent last
    if (nb === '—' && na !== '—') return -1;
    return na.localeCompare(nb);
  });
  for (const [agentId, list] of sortedGroups) {
    const name = agentName(agentId);
    const nActive = list.filter(t => t.enabled && !isCompleted(t)).length;
    const gtr = document.createElement('tr');
    gtr.className = 'group-row';
    gtr.innerHTML = `<td colspan="6" class="group-cell"><span class="group-name">${esc(name)}</span> <span class="muted">${list.length} task${list.length === 1 ? '' : 's'}${nActive ? ` · ${nActive} active` : ''}</span></td>`;
    tbody.appendChild(gtr);
    renderTaskRows(tbody, sortTasks(list));
  }
}

function renderTaskRows(tbody, sorted) {
  for (const t of sorted) {
    const tr = document.createElement('tr');
    const completed = isCompleted(t);
    const status = !t.enabled
      ? (completed ? '<span class="badge">✓ done</span>' : '<span class="badge">paused</span>')
      : '<span class="badge badge-ok">active</span>';
    tr.innerHTML = `
      <td><strong>${esc(t.name)}</strong><div class="muted cell-ellip" style="max-width:280px" title="${esc(t.prompt)}">${esc(truncate(t.prompt))}</div></td>
      <td>${esc(t.schedule_text || '')}</td>
      <td>${fmtTime(t.next_fire_at)}</td>
      <td>${fmtTime(t.last_fired_at)}</td>
      <td>${status}</td>
      <td class="row-actions">
        <input type="checkbox" data-act="toggle" data-id="${esc(t.id)}" ${t.enabled ? 'checked' : ''} title="Enable / pause">
        <button class="icon-btn" data-act="run" data-id="${esc(t.id)}" title="Run now">▶</button>
        <button class="icon-btn icon-danger" data-act="del" data-id="${esc(t.id)}" data-name="${esc(t.name)}" title="Delete task">🗑</button>
      </td>`;
    tbody.appendChild(tr);
  }
}

// ── schedule builder (modal) ──────────────────────────────────────────
function scheduleFromForm(f) {
  const type = f.sched_type.value;
  if (type === 'once') {
    const at = f.once_at.value;   // datetime-local: "YYYY-MM-DDTHH:MM"
    if (!at) throw new Error('Pick a date & time for the one-shot task');
    return { type, at };
  }
  if (type === 'interval') {
    const m = parseInt(f.every_minutes.value, 10);
    if (!m || m < 1) throw new Error('Interval must be at least 1 minute');
    return { type, every_minutes: m };
  }
  if (type === 'daily') {
    if (!f.daily_time.value) throw new Error('Pick a time of day');
    return { type, time: f.daily_time.value };
  }
  if (type === 'weekly') {
    if (!f.weekly_time.value) throw new Error('Pick a time of day');
    const days = [...f.querySelectorAll('input[name="wday"]:checked')]
      .map(c => parseInt(c.value, 10));
    if (!days.length) throw new Error('Pick at least one weekday');
    return { type, time: f.weekly_time.value, days };
  }
  throw new Error('unknown schedule type');
}

function syncSchedFields() {
  const type = $('task-sched-type').value;
  for (const t of ['once', 'interval', 'daily', 'weekly'])
    $('sched-fields-' + t).classList.toggle('hidden', t !== type);
}

// ── modals ────────────────────────────────────────────────────────────
function openModal(id)  { $(id).classList.remove('hidden'); }
function closeModal(id) { $(id).classList.add('hidden'); }

async function openAddModal() {
  const sel = $('task-agent');
  sel.innerHTML = agents.map(a =>
    `<option value="${esc(a.id)}">${esc(a.name)}</option>`).join('');
  $('task-form').reset();
  syncSchedFields();
  openModal('task-modal');
}

// ── cleanup ───────────────────────────────────────────────────────────
async function cleanupCompleted() {
  const done = tasks.filter(isCompleted);
  if (!done.length) { alert('No completed one-shot tasks to clean up.'); return; }
  if (!confirm(`Delete ${done.length} completed one-shot task(s)?\n\n` +
               done.map(t => '• ' + t.name).join('\n'))) return;
  for (const t of done) await API.del('/api/routines/' + t.id);
  await loadAll();
}

// ── events ────────────────────────────────────────────────────────────
export function initTasks() {
  $('tasks-add').addEventListener('click', openAddModal);
  $('tasks-cleanup').addEventListener('click', () =>
    cleanupCompleted().catch(e => alert(e.message)));
  $('tasks-tbody').addEventListener('click', async (e) => {
    const b = e.target.closest('button[data-act]');
    if (!b) return;
    const id = b.dataset.id;
    try {
      if (b.dataset.act === 'run') {
        await API.post('/api/routines/' + id + '/run', {});
        b.textContent = '✓';
        setTimeout(() => { b.textContent = '▶'; }, 1200);
      } else if (b.dataset.act === 'del') {
        if (confirm(`Delete task "${b.dataset.name}"? Its thread is kept.`)) {
          await API.del('/api/routines/' + id);
          await loadAll();
        }
      }
    } catch (err) { alert(err.message); }
  });
  $('tasks-tbody').addEventListener('change', async (e) => {
    const c = e.target.closest('input[data-act="toggle"]');
    if (!c) return;
    try {
      await API.post('/api/routines/' + c.dataset.id + '/enabled', { enabled: c.checked });
      await loadAll();
    } catch (err) { alert(err.message); c.checked = !c.checked; }
  });
  $('task-sched-type').addEventListener('change', syncSchedFields);
  $('task-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    try {
      await API.post('/api/routines', {
        agent_id: f.agent_id.value,
        name: f.name.value.trim(),
        prompt: f.prompt.value.trim(),
        schedule: scheduleFromForm(f),
      });
      closeModal('task-modal');
      await loadAll();
    } catch (err) { alert(err.message); }
  });
  document.querySelectorAll('#task-modal [data-close]').forEach(b =>
    b.addEventListener('click', () => closeModal('task-modal')));
}

export function enterTasks() { loadAll().catch(e => console.error('tasks:', e)); }
