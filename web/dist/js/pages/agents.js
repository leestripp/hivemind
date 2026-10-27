// js/pages/agents.js — roster overview (Phase 13 first slice).
// Card grid of all agents with live busy status (2s poll while visible).
import { API } from '../api.js';
import { $, esc } from '../util.js';
import { avatarSrc } from '../avatars.js';
import { openAgentModal } from '../agent-modal.js?v=20261027a';
import { openMigrationModal } from '../migration-modal.js?v=20261027a';

function fmtDate(iso) {
  const d = new Date(iso);
  return isNaN(d) ? esc(iso)
    : d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

let pollTimer = null;

const showAgentModal = (existing = null, prefill = null) =>
  openAgentModal(existing, prefill, {
    onSaved: refresh,
    onMigrate: showMigrationModal,
  });
const showMigrationModal = (agent) =>
  openMigrationModal(agent, { onComplete: refresh });

async function refresh() {
  const grid = $('agents-grid');
  try {
    const [roster, act] = await Promise.all([
      API.get('/api/agents'),
      API.get('/api/agents/activity'),
    ]);
    let workers = [];
    try { workers = (await API.get('/api/workers')).workers || []; }
    catch { /* group headers just lose the live online/offline badge */ }
    const agents = roster.agents || [];
    const live = act.agents || {};
    if (!agents.length) {
      // Phase 19 first-run: one-click teammate templates.
      const tpl = await API.get('/api/templates');
      const templates = tpl.templates || [];
      grid.innerHTML = '<p class="muted" style="grid-column:1/-1">No agents yet — start your team with a template below, or the <b>+ Agent</b> button above for a blank one:</p>';
      for (const t of templates) {
        const card = document.createElement('div');
        card.className = 'agent-card';
        card.innerHTML = `
          <div class="agent-top">
            <div class="agent-avatar"><img src="${avatarSrc(t)}" width="44" height="44" alt=""></div>
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
      grid.onclick = (e) => {
        const b = e.target.closest('[data-tpl]');
        if (!b) return;
        const t = templates.find(x => x.id === b.dataset.tpl);
        if (!t) return;
        showAgentModal(null, t);   // worker + type selection happens there
      };
      return;
    }
    grid.innerHTML = '';
    // 2026-10-04: band the roster by hive-worker (machine). The co-located
    // worker ("" / "local") shares one "This server" band, always first.
    const LOCAL = 'local';
    const workerOf = (a) => (a.worker && a.worker !== LOCAL) ? a.worker : LOCAL;
    const groups = new Map();
    for (const a of agents) {
      const w = workerOf(a);
      if (!groups.has(w)) groups.set(w, []);
      groups.get(w).push(a);
    }
    const sortedWorkers = [...groups.keys()].sort((x, y) =>
      x === LOCAL ? -1 : y === LOCAL ? 1 : x.localeCompare(y));
    for (const w of sortedWorkers) {
      const members = groups.get(w)
        .sort((a, b) => a.name.localeCompare(b.name));
      const isLocal = w === LOCAL;
      const wk = isLocal ? null : workers.find(x => x.name === w);
      const online = isLocal || !!wk?.online;
      const band = document.createElement('div');
      band.className = 'agent-group';
      band.innerHTML = `
        <div class="agent-group-head">
          <span class="agent-group-name">${isLocal ? '🖥 This server'
                                                    : '🖥 ' + esc(w)}</span>
          <span class="badge ${online ? 'st-busy' : 'badge-revoked'}">${
            online ? '● online' : '○ offline'}</span>
          <span class="muted">${members.length} agent${
            members.length === 1 ? '' : 's'}</span>
        </div>
        <div class="agents-grid"></div>`;
      const sub = band.querySelector('.agents-grid');
      grid.appendChild(band);
      for (const a of members) {
      const st = live[a.id];
      // Real brain/fleet state from /api/agents (hive-agent AgentState via
      // worker heartbeats); the live-map only adds the cancelling nuance.
      const brain = (a.state || '').toLowerCase();
      const ACTIVE = ['understanding','planning','acting','verifying',
                      'documenting','compressing','starting','restarting'];
      const status = st && st.cancelling ? 'cancelling'
                   : ACTIVE.includes(brain) ? 'busy'
                   : (brain === 'ready' || brain === 'idle' || !brain) ? 'idle'
                   : 'down';
      const label = st && st.cancelling ? '\u25cc cancelling'
                  : status === 'busy' ? '\u25cf ' + (ACTIVE.includes(brain) ? brain : 'busy')
                  : brain === 'down' || brain === 'error' || brain === 'cancelled' ? brain
                  : 'idle';
      const card = document.createElement('div');
      card.className = 'agent-card status-' + status;
      card.innerHTML = `
        <div class="agent-top">
          <div class="agent-avatar"><img src="${avatarSrc(a)}" width="44" height="44" alt=""></div>
          <div class="agent-id-block">
            <div class="agent-name">${a.busy ? '<span class="busy-dot" title="Working…">●</span> ' : ''}${esc(a.name)}</div>
            <div class="agent-role">${esc(a.role || '—')}</div>
          </div>
          ${a.host_agent ? '<span class="badge" style="background:#b91c1c;color:#fff" title="HOST AGENT — runs bare-metal with sudo, NO sandbox">HOST ⚠</span>' : ''}
          <span class="badge st-${status}" title="state: ${esc(brain || 'idle')}">${label}</span>
        </div>
        <div class="agent-meta">
          <div><span>Model</span>${esc(a.model || 'default')}</div>
          <div><span>Workspace</span>${esc(a.workspace_mode || 'shared')}</div>
          ${a.type ? `<div><span>Type</span>⚡ ${esc(a.type)}</div>` : ''}
          <div><span>Created</span>${fmtDate(a.created_at)}</div>
        </div>
        <div class="agent-actions">
          <button class="btn btn-sm" data-edit="${esc(a.id)}" title="Edit agent (worker, type, model…)">✎ Edit</button>
          <button class="btn btn-sm" data-migrate="${esc(a.id)}" title="Move this agent and its state to another worker">⇄ Migrate</button>
          <button class="btn btn-sm" data-screen="${esc(a.id)}" title="Live container desktop (Phase 6)">🖥 Computer</button>
        </div>`;
      card.querySelector('[data-edit]').addEventListener('click',
        () => showAgentModal(a));
      card.querySelector('[data-migrate]').addEventListener('click',
        () => showMigrationModal(a));
      card.querySelector('[data-screen]').addEventListener('click',
        () => openScreen(a.id, a.name));
      sub.appendChild(card);
      }
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

export function enterAgents() {
  refresh();
  if (!pollTimer) pollTimer = setInterval(refresh, 2000);
}

export function leaveAgents() {
  clearInterval(pollTimer);
  pollTimer = null;
}

export function initAgents() {
  $('agent-add').addEventListener('click', () => showAgentModal());
}
