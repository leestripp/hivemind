// js/pages/workers.js — Phase 30: hive-worker machines.
// Live table (auto-refresh while the page is open): online badge driven
// by the worker channel, caps, protocol, last heartbeat, agents assigned.
// Enrollment is CLI-only by design (the raw hmwk_ key prints ONCE on the
// server host) — the page shows the exact command instead.
import { API } from '../api.js';
import { $, esc } from '../util.js';

let timer = null;

export function enterWorkers() {
  loadWorkers();
  timer = setInterval(loadWorkers, 5000);   // online flag is live
}
export function leaveWorkers() {
  if (timer) { clearInterval(timer); timer = null; }
}

const TRASH_SVG =
  '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" ' +
  'stroke="currentColor" stroke-width="1.8" stroke-linecap="round" ' +
  'stroke-linejoin="round"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg>';

function fmtAgo(iso) {
  const t = Date.parse(iso);
  if (isNaN(t)) return '—';
  const s = Math.max(0, Math.round((Date.now() - t) / 1000));
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.round(s / 60) + 'm ago';
  if (s < 86400) return Math.round(s / 3600) + 'h ago';
  return Math.round(s / 86400) + 'd ago';
}

function fmtCaps(caps) {
  if (!caps || typeof caps !== 'object') return '—';
  const parts = Object.entries(caps).map(([k, v]) =>
    v === true ? k : `${k}=${v}`);
  return parts.length ? esc(parts.join('  ')) : '—';
}

async function loadWorkers() {
  const list = $('workers-list');
  try {
    const [wdata, adata] = await Promise.all([
      API.get('/api/workers'),
      API.get('/api/agents'),
    ]);
    const workers = wdata.workers || [];
    const agents = adata.agents || [];
    const countFor = (name) =>
      agents.filter(a => a.worker === name).length;

    if (!workers.length) {
      list.innerHTML = '<tr><td colspan="7" class="muted">No workers enrolled — every agent runs locally. See the enroll command below.</td></tr>';
      return;
    }
    list.innerHTML = '';
    for (const w of workers) {
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td class="cell-name">${esc(w.name)}</td>
        <td>${w.online
              ? '<span class="badge st-busy">● online</span>'
              : '<span class="badge badge-revoked">○ offline</span>'}</td>
        <td class="muted-cell">${fmtCaps(w.caps)}</td>
        <td class="muted-cell">${esc(String(w.proto ?? ''))}</td>
        <td class="muted-cell">${fmtAgo(w.last_seen)}</td>
        <td class="muted-cell">${countFor(w.name) || '—'}</td>
        <td class="col-actions"></td>`;
      const btn = document.createElement('button');
      btn.className = 'icon-btn icon-danger';
      btn.title = `Revoke worker "${w.name}"`;
      btn.innerHTML = TRASH_SVG;
      btn.onclick = async () => {
        const n = countFor(w.name);
        const warn = n ? `\n${n} agent(s) assigned to it will fail until reassigned.` : '';
        if (!confirm(`Revoke worker "${w.name}"? Its key stops working immediately.${warn}`)) return;
        try {
          await API.del('/api/workers/' + encodeURIComponent(w.name));
          loadWorkers();
        } catch (e) { alert('Revoke failed: ' + e.message); }
      };
      tr.querySelector('.col-actions').appendChild(btn);
      list.appendChild(tr);
    }
  } catch (e) {
    list.innerHTML = `<tr><td colspan="7" class="muted">unavailable — ${esc(e.message)}</td></tr>`;
  }
}

export function initWorkers() {
  $('workers-copy-enroll').addEventListener('click', async () => {
    const cmd = $('workers-enroll-cmd').textContent;
    try {
      await navigator.clipboard.writeText(cmd);
      $('workers-copy-enroll').textContent = 'Copied!';
      setTimeout(() => { $('workers-copy-enroll').textContent = 'Copy'; }, 1500);
    } catch { /* non-secure context — leave as-is */ }
  });
}
