// js/pages/agents.js — roster overview (Phase 13 first slice).
// Card grid of all agents with live busy status (2s poll while visible).
import { API } from '../api.js';
import { $, esc } from '../util.js';

// ── auto avatar — mirrors the GTK client exactly ──────────────────────
// Client: auto_avatar(name) = AVATARS[std::hash<string>(name) % 20], and
// libstdc++ std::hash<string> is MurmurHash64A(seed 0xc70f6907).
// Reimplemented here (BigInt) so the web picks the same department head
// the client shows in the roster/bubbles. Order must match the client's
// avatar_names() (department pairs: male, then female).
const AVATARS = [
  'executive', 'executive-female',
  'engineering', 'engineering-female',
  'marketing', 'marketing-female',
  'sales', 'sales-female',
  'hr', 'hr-female',
  'finance', 'finance-female',
  'it-support', 'it-support-female',
  'design', 'design-female',
  'legal', 'legal-female',
  'operations', 'operations-female',
];

function murmur64a(str) {
  const m = 0xc6a4a7935bd1e995n, r = 47n, mask = 0xffffffffffffffffn;
  const bytes = new TextEncoder().encode(str);
  let h = (0xc70f6907n ^ (BigInt(bytes.length) * m)) & mask;
  const nBlocks = bytes.length >> 3;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, nBlocks * 8);
  for (let i = 0; i < nBlocks; i++) {
    let k = dv.getBigUint64(i * 8, true);           // little-endian
    k = (k * m) & mask; k ^= k >> r; k = (k * m) & mask;
    h ^= k; h = (h * m) & mask;
  }
  // Tail: XOR each remaining byte at its little-endian position, ONE multiply
  // (mirrors the C++ switch-fallthrough in MurmurHash64A).
  const tail = bytes.slice(nBlocks * 8);
  for (let i = 0; i < tail.length; i++)
    h ^= BigInt(tail[i]) << BigInt(8 * i);
  if (tail.length) h = (h * m) & mask;
  h ^= h >> r; h = (h * m) & mask; h ^= h >> r;
  return h;
}

// "Auto" avatar for agents/templates without an explicit choice: stable
// name hash into the dept-head set (same pick as the client).
const autoAvatar = (name) => AVATARS[Number(murmur64a(name) % BigInt(AVATARS.length))];

function fmtDate(iso) {
  const d = new Date(iso);
  return isNaN(d) ? esc(iso)
    : d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

let pollTimer = null;

async function refresh() {
  const grid = $('agents-grid');
  try {
    const [roster, act] = await Promise.all([
      API.get('/api/agents'),
      API.get('/api/agents/activity'),
    ]);
    const agents = roster.agents || [];
    const live = act.agents || {};
    if (!agents.length) {
      // Phase 19 first-run: one-click teammate templates.
      const tpl = await API.get('/api/templates');
      const templates = tpl.templates || [];
      grid.innerHTML = '<p class="muted" style="grid-column:1/-1">No agents yet — start your team with a template (or use + Agent in the client for a blank one):</p>';
      for (const t of templates) {
        const card = document.createElement('div');
        card.className = 'agent-card';
        card.innerHTML = `
          <div class="agent-top">
            <div class="agent-avatar"><img src="/avatars/${esc(t.avatar || autoAvatar(t.name))}.svg" width="132" height="132" alt=""></div>
            <div class="agent-id-block">
              <div class="agent-name">${esc(t.name)}</div>
              <div class="agent-role">${esc(t.role)}</div>
            </div>
          </div>
          <div class="agent-actions">
            <button class="btn btn-sm suggested-action" data-tpl="${esc(t.id)}">+ Add ${esc(t.name)}</button>
          </div>`;
        grid.appendChild(card);
      }
      grid.onclick = async (e) => {
        const b = e.target.closest('[data-tpl]');
        if (!b) return;
        const t = templates.find(x => x.id === b.dataset.tpl);
        if (!t) return;
        b.disabled = true;
        try {
          await API.post('/api/agents', {
            name: t.name, role: t.role, avatar: t.avatar,
            require_approval: !!t.require_approval,
          });
          refresh();
        } catch (err) { alert(err.message); b.disabled = false; }
      };
      return;
    }
    grid.innerHTML = '';
    for (const a of agents) {
      const st = live[a.id];
      const status = st ? (st.cancelling ? 'cancelling' : 'busy') : 'idle';
      const card = document.createElement('div');
      card.className = 'agent-card status-' + status;
      card.innerHTML = `
        <div class="agent-top">
          <div class="agent-avatar"><img src="/avatars/${esc(a.avatar || autoAvatar(a.name))}.svg" width="132" height="132" alt=""></div>
          <div class="agent-id-block">
            <div class="agent-name">${a.busy ? '<span class="busy-dot" title="Working…">●</span> ' : ''}${esc(a.name)}</div>
            <div class="agent-role">${esc(a.role || '—')}</div>
          </div>
          <span class="badge st-${status}">${status === 'busy' ? '● busy' : status === 'cancelling' ? '◌ cancelling' : 'idle'}</span>
        </div>
        <div class="agent-meta">
          <div><span>Model</span>${esc(a.model || 'default')}</div>
          <div><span>Workspace</span>${esc(a.workspace_mode || 'shared')}</div>
          <div><span>Approvals</span>${a.require_approval ? 'required' : 'auto'}</div>
          <div><span>Created</span>${fmtDate(a.created_at)}</div>
        </div>
        <div class="agent-actions">
          <button class="btn btn-sm" data-screen="${esc(a.id)}" title="Live container desktop (Phase 6)">🖥 Computer</button>
          <button class="btn btn-sm" data-rules="${esc(a.id)}" title="Auto-review rules for approvals (Phase 16)">⚖ Rules</button>
        </div>`;
      card.querySelector('[data-screen]').addEventListener('click',
        () => openScreen(a.id, a.name));
      card.querySelector('[data-rules]').addEventListener('click',
        () => openRules(a.id, a.name));
      grid.appendChild(card);
    }
  } catch (e) {
    grid.innerHTML = `<p class="muted">unavailable — ${esc(e.message)}</p>`;
  }
}

// ── Phase 6: Agent Computer (noVNC through hive-server) ─────────────────
let screenModal = null;

function ensureScreenModal() {
  if (screenModal) return screenModal;
  screenModal = document.createElement('div');
  screenModal.className = 'modal-overlay hidden';
  screenModal.id = 'screen-modal';
  screenModal.innerHTML = `
    <div class="modal-panel screen-panel">
      <div class="modal-head">
        <strong id="screen-title">Agent Computer</strong>
        <span class="muted" id="screen-state"></span>
        <span style="flex:1"></span>
        <button class="btn btn-sm" id="screen-stop">Stop screen</button>
        <button class="btn btn-sm" id="screen-close">Close</button>
      </div>
      <div id="screen-body" class="screen-body">
        <p class="muted" id="screen-msg">Starting the display stack…</p>
      </div>
    </div>`;
  document.body.appendChild(screenModal);
  screenModal.querySelector('#screen-close').addEventListener('click', hideScreen);
  return screenModal;
}

let screenAgent = null;

async function openScreen(id, name) {
  ensureScreenModal();
  screenAgent = id;
  screenModal.classList.remove('hidden');
  document.getElementById('screen-title').textContent = `🖥 ${name} — Computer`;
  const msg = document.getElementById('screen-msg');
  const body = document.getElementById('screen-body');
  msg.textContent = 'Starting the display stack…';
  body.querySelector('iframe')?.remove();
  msg.style.display = '';
  try {
    const r = await API.post(`/api/agents/${id}/screen/start`, {});
    // Cookie-based auth: first hit carries ?key=, server sets an HttpOnly
    // cookie for all subsequent asset + WS requests, then redirects into
    // noVNC with autoconnect.
    const ifr = document.createElement('iframe');
    ifr.className = 'screen-frame';
    ifr.src = `${r.url}?key=${encodeURIComponent(API.getKey())}`;
    body.appendChild(ifr);
    msg.style.display = 'none';
  } catch (e) {
    msg.textContent = 'Screen unavailable — ' + e.message;
  }
  document.getElementById('screen-stop').onclick = async () => {
    try { await API.post(`/api/agents/${screenAgent}/screen/stop`, {}); }
    catch (_) {}
    hideScreen();
  };
}

function hideScreen() {
  if (!screenModal) return;
  screenModal.classList.add('hidden');
  screenModal.querySelector('iframe')?.remove();   // drops the VNC session
  screenAgent = null;
}

// ── Phase 16: auto-review rules editor ──────────────────────────────────
let rulesModal = null, rulesAgent = null;

function ensureRulesModal() {
  if (rulesModal) return rulesModal;
  rulesModal = document.createElement('div');
  rulesModal.className = 'modal-overlay hidden';
  rulesModal.innerHTML = `
    <div class="modal-panel" style="width:min(720px,94vw)">
      <div class="modal-head">
        <strong id="rules-title">Auto-review rules</strong>
        <span style="flex:1"></span>
        <button class="btn btn-sm" id="rules-add">+ Add rule</button>
        <button class="btn btn-sm btn-primary" id="rules-save">Save</button>
        <button class="btn btn-sm" id="rules-close">Close</button>
      </div>
      <p class="muted" style="margin:4px 0 10px">First match wins. Globs:
        <code>*</code> = any run, <code>?</code> = one char. Tool is
        <code>command</code> or <code>browser</code>. No match → the human
        approval queue (when the agent requires approvals).</p>
      <div id="rules-rows"></div>
    </div>`;
  document.body.appendChild(rulesModal);
  rulesModal.querySelector('#rules-close').addEventListener('click', () => {
    rulesModal.classList.add('hidden'); rulesAgent = null;
  });
  rulesModal.querySelector('#rules-add').addEventListener('click', () =>
    addRuleRow({tool: 'command', command: '', action: 'allow'}));
  rulesModal.querySelector('#rules-save').addEventListener('click', saveRules);
  return rulesModal;
}

function addRuleRow(rule) {
  const rows = rulesModal.querySelector('#rules-rows');
  const row = document.createElement('div');
  row.className = 'rule-row';
  row.innerHTML = `
    <select class="rule-action">
      <option value="allow"${rule.action === 'allow' ? ' selected' : ''}>allow</option>
      <option value="deny"${rule.action === 'deny' ? ' selected' : ''}>deny</option>
    </select>
    <input class="rule-tool" placeholder="tool glob" value="${esc(rule.tool)}" size="9">
    <input class="rule-cmd" placeholder="command glob  e.g. git *" value="${esc(rule.command)}">
    <button class="icon-btn" data-up title="Move up">↑</button>
    <button class="icon-btn" data-down title="Move down">↓</button>
    <button class="icon-btn" data-del title="Delete">✕</button>`;
  row.querySelector('[data-del]').addEventListener('click', () => row.remove());
  row.querySelector('[data-up]').addEventListener('click', () =>
    row.previousElementSibling && rows.insertBefore(row, row.previousElementSibling));
  row.querySelector('[data-down]').addEventListener('click', () =>
    row.nextElementSibling && rows.insertBefore(row.nextElementSibling, row));
  rows.appendChild(row);
}

async function openRules(id, name) {
  ensureRulesModal();
  rulesAgent = id;
  rulesModal.classList.remove('hidden');
  document.getElementById('rules-title').textContent = `⚖ ${name} — auto-review rules`;
  const rows = rulesModal.querySelector('#rules-rows');
  rows.innerHTML = '';
  const data = await API.get(`/api/agents/${id}/approval-rules`);
  for (const r of data.rules || []) addRuleRow(r);
}

async function saveRules() {
  const rules = [...rulesModal.querySelectorAll('.rule-row')].map(row => ({
    action: row.querySelector('.rule-action').value,
    tool: row.querySelector('.rule-tool').value.trim(),
    command: row.querySelector('.rule-cmd').value.trim(),
  }));
  if (rules.some(r => !r.tool || !r.command)) {
    alert('Every rule needs a tool and a command pattern.');
    return;
  }
  try {
    await API.req('PUT', `/api/agents/${rulesAgent}/approval-rules`, {rules});
    rulesModal.classList.add('hidden');
    rulesAgent = null;
  } catch (e) {
    alert('Save failed — ' + e.message);
  }
}

export function enterAgents() {
  refresh();
  if (!pollTimer) pollTimer = setInterval(refresh, 2000);
}

export function leaveAgents() {
  clearInterval(pollTimer);
  pollTimer = null;
}

export function initAgents() {}
