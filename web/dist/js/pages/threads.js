// js/pages/threads.js — thread browser: message ages + lifecycle marks.
// 2026-10-22 (Plan.md): the fleet's claimed→done→verified system was
// API-only; this page makes it visible. List on the left (last-activity
// order, unread badges), messages on the right with relative ages and
// status pills; the signed-in user can mark/clear any message
// (server-side the "user" principal is exempt from claim/verify rules).
import { API } from '../api.js';
import { $, esc, fmtTimeTZ } from '../util.js';

let threads = [];
let agentsById = new Map();
let selectedId = null;
let pollTimer = null;

const agentName = (id) => agentsById.get(id) || id?.slice(0, 8) || '—';

// "just now" / "5m ago" / "3h ago" / "2d ago" — the whole point of the
// page is making message AGE unmissable (delayed-landing confusion).
function relAge(iso) {
  const t = Date.parse(iso || '');
  if (isNaN(t)) return '—';
  const s = Math.max(0, (Date.now() - t) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return Math.floor(s / 60) + 'm ago';
  if (s < 86400) return Math.floor(s / 3600) + 'h ago';
  return Math.floor(s / 86400) + 'd ago';
}

function threadTitle(t) {
  if (t.kind === 'user') return 'User Inbox';
  if (t.title) return t.title;
  const names = (t.agent_ids || []).map(agentName);
  return names.length ? names.join(' ↔ ') : '(empty)';
}

async function loadAgents() {
  const a = await API.get('/api/agents');
  agentsById = new Map((a.agents || []).map(x => [x.id, x.name]));
}

async function loadThreads() {
  const r = await API.get('/api/threads?limit=100');
  threads = (r.threads || []).slice()
    .sort((a, b) => (b.last_activity || '').localeCompare(a.last_activity || ''));
  renderList();
}

function renderList() {
  const host = $('threads-list');
  if (!threads.length) {
    host.innerHTML = '<div class="muted threads-empty">No threads.</div>';
    return;
  }
  host.innerHTML = threads.map(t => `
    <div class="thread-row${t.id === selectedId ? ' active' : ''}" data-tid="${esc(t.id)}">
      <div class="thread-row-top">
        <span class="thread-title">${esc(threadTitle(t))}</span>
        ${t.unread ? `<span class="thread-unread">${t.unread}</span>` : ''}
      </div>
      <div class="thread-row-sub">
        <span class="pill pill-${esc(t.kind)}">${esc(t.kind)}</span>
        <span class="thread-age" title="${esc(fmtTimeTZ(t.last_activity))}">${esc(relAge(t.last_activity))}</span>
      </div>
    </div>`).join('');
  host.querySelectorAll('.thread-row').forEach(row =>
    row.addEventListener('click', () => selectThread(row.dataset.tid)));
}

async function selectThread(id) {
  selectedId = id;
  renderList();   // move the active highlight immediately
  await loadMessages(true);
}

async function loadMessages(scroll) {
  if (!selectedId) return;
  const r = await API.get(`/api/threads/${selectedId}/messages?limit=100`);
  renderMessages(r.messages || [], scroll);
}

function senderLabel(m) {
  if (m.sender_kind === 'user') return 'You';
  if (m.sender_kind === 'system') return 'system';
  return agentName(m.sender_id);
}

function statusPill(m) {
  const st = m.status;
  if (!st || !st.status) return '';
  const cls = { claimed: 'st-claimed', done: 'st-done', verified: 'st-verified' }[st.status] || 'st-claimed';
  const by = st.by === 'user' ? 'You' : agentName(st.by);
  const tip = `${st.status} by ${by} · ${fmtTimeTZ(st.at)}${st.note ? '\n' + st.note : ''}`;
  return `<span class="st-pill ${cls}" title="${esc(tip)}">${esc(st.status)} · ${esc(by)} · ${esc(relAge(st.at))}</span>`;
}

function renderMessages(msgs, scroll) {
  const host = $('threads-msgs');
  $('threads-empty').classList.toggle('hidden', true);
  if (!msgs.length) {
    host.innerHTML = '<div class="muted threads-empty">No messages.</div>';
    return;
  }
  host.innerHTML = msgs.map(m => `
    <div class="msg msg-${esc(m.sender_kind)}">
      <div class="msg-head">
        <span class="msg-sender">${esc(senderLabel(m))}</span>
        <span class="msg-age" title="${esc(fmtTimeTZ(m.created_at))}">${esc(relAge(m.created_at))}</span>
        ${statusPill(m)}
        <span class="msg-actions">
          <button class="btn btn-sm" data-act="claimed" data-mid="${esc(m.id)}">claim</button>
          <button class="btn btn-sm" data-act="done" data-mid="${esc(m.id)}">done</button>
          <button class="btn btn-sm" data-act="verified" data-mid="${esc(m.id)}">verify</button>
          <button class="btn btn-sm danger" data-act="clear" data-mid="${esc(m.id)}">clear</button>
        </span>
      </div>
      <div class="msg-body">${esc(m.body)}</div>
    </div>`).join('');
  host.querySelectorAll('button[data-act]').forEach(b =>
    b.addEventListener('click', () => mark(b.dataset.mid, b.dataset.act)));
  if (scroll) host.scrollTop = host.scrollHeight;
}

async function mark(mid, act) {
  const url = `/api/threads/${selectedId}/messages/${mid}/status`;
  if (act === 'clear') {
    await API.del(url);
  } else {
    const note = window.prompt(`Note for "${act}" (optional, Cancel = no note):`, '');
    await API.post(url, note === null ? { status: act } : { status: act, note });
  }
  await Promise.all([loadMessages(false), loadThreads()]);
}

export function enterThreads() {
  loadAgents().then(loadThreads).catch(() => {});
  pollTimer = setInterval(() => {
    loadThreads().catch(() => {});
    if (selectedId) loadMessages(false).catch(() => {});
  }, 10000);
}

export function leaveThreads() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

export function initThreads() {}
