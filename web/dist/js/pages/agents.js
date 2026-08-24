// js/pages/agents.js — roster overview (Phase 13 first slice).
// Card grid of all agents with live busy status (2s poll while visible).
import { API } from '../api.js';
import { $, esc } from '../util.js';

// ── palette — mirrors the GTK client exactly ──────────────────────────
// Client: palette_index(name) = std::hash<string>(name) % 6, and libstdc++
// std::hash<string> is MurmurHash64A(seed 0xc70f6907). Reimplemented here
// (BigInt) so web avatar colors match the client's roster/bubbles.
const PALETTE = ['#3584e4', '#33d17a', '#f6d32d', '#ff7800', '#9141ac', '#e01b24'];

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

const paletteIndex = (name) => Number(murmur64a(name) % 6n);

// Hexagon hive-bot avatar, inlined so the color can follow the palette.
function hexBot(color, size) {
  return `<svg width="${size}" height="${size}" viewBox="0 0 64 64" fill="none">
    <path d="M32 4 56 18v28L32 60 8 46V18Z" fill="${color}"/>
    <path d="M14 28h36v14a6 6 0 0 1-6 6H20a6 6 0 0 1-6-6Z" fill="#1a1d23"/>
    <circle cx="24" cy="38" r="3.4" fill="#fff"/>
    <circle cx="40" cy="38" r="3.4" fill="#fff"/>
  </svg>`;
}

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
      grid.innerHTML = '<p class="muted">No agents yet — create one from the hive-client (+ Agent).</p>';
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
          <div class="agent-avatar">${a.avatar
            ? `<img src="/avatars/${esc(a.avatar)}.svg" width="44" height="44" alt="">`
            : hexBot(PALETTE[paletteIndex(a.name)], 44)}</div>
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
        </div>`;
      grid.appendChild(card);
    }
  } catch (e) {
    grid.innerHTML = `<p class="muted">unavailable — ${esc(e.message)}</p>`;
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
