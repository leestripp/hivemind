// js/pages/files.js — Phase 18 artifacts browser: browse the shared
// company workspace (agent outputs, chat attachments) over /api/files.
import { API } from '../api.js';
import { $, esc, getTimezone } from '../util.js';

let cwd = '';            // workspace-relative path, '' = root
let entries = [];

function fmtSize(n) {
  if (n === undefined || n === null) return '';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}
function fmtTime(epoch) {
  if (!epoch) return '';
  const tz = getTimezone();   // Phase 19
  // new Date(epoch*1000) overflows in some embedded webviews (large ms
  // values render as 1969); setUTCSeconds takes seconds and is safe.
  const d = new Date(0);
  d.setUTCSeconds(epoch);
  return d.toLocaleString(undefined, tz ? { timeZone: tz } : undefined);
}

async function load() {
  const data = await API.get('/api/files?path=' + encodeURIComponent(cwd));
  entries = data.entries || [];
  renderCrumbs();
  renderRows();
}

function renderCrumbs() {
  const parts = cwd ? cwd.split('/') : [];
  let html = `<a href="#" data-crumb="">📁 workspace</a>`;
  let acc = '';
  for (const p of parts) {
    acc = acc ? acc + '/' + p : p;
    html += ` <span class="muted">/</span> <a href="#" data-crumb="${esc(acc)}">${esc(p)}</a>`;
  }
  $('files-crumbs').innerHTML = html;
}

function renderRows() {
  const tbody = $('files-tbody');
  if (!entries.length) {
    tbody.innerHTML = '<tr><td colspan="4" class="muted">empty directory</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  for (const e of entries) {
    const tr = document.createElement('tr');
    const icon = e.kind === 'dir' ? '📁' : '📄';
    const nameHtml = e.kind === 'dir'
      ? `<a href="#" data-open="${esc(e.path)}">${icon} ${esc(e.name)}</a>`
      : `<a href="#" data-dl="${esc(e.path)}" title="Download">${icon} ${esc(e.name)}</a>`;
    tr.innerHTML = `<td>${nameHtml}</td>
      <td class="muted">${esc(e.kind)}</td>
      <td class="muted">${fmtSize(e.size)}</td>
      <td class="muted">${fmtTime(e.mtime)}</td>`;
    tbody.appendChild(tr);
  }
}

async function download(rel, name) {
  // Authenticated fetch → blob → object-URL download (window.open would
  // lose the bearer header).
  const r = await fetch('/api/files/download?path=' + encodeURIComponent(rel), {
    headers: { 'Authorization': 'Bearer ' + API.getKey() },
  });
  if (!r.ok) throw new Error('download failed: HTTP ' + r.status);
  const blob = await r.blob();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  URL.revokeObjectURL(a.href);
}

// ── page lifecycle ────────────────────────────────────────────────────
export function enterFiles() { load().catch(e => {
  $('files-tbody').innerHTML = `<tr><td colspan="4" class="muted">${esc(e.message)}</td></tr>`;
}); }

export function initFiles() {
  $('files-refresh').addEventListener('click', () => load());
  $('files-crumbs').addEventListener('click', (e) => {
    const c = e.target.closest('[data-crumb]');
    if (!c) return;
    e.preventDefault();
    cwd = c.dataset.crumb;
    load();
  });
  $('files-tbody').addEventListener('click', (e) => {
    const open = e.target.closest('[data-open]');
    if (open) { e.preventDefault(); cwd = open.dataset.open; load(); return; }
    const dl = e.target.closest('[data-dl]');
    if (dl) {
      e.preventDefault();
      const rel = dl.dataset.dl;
      download(rel, rel.split('/').pop())
        .catch(err => alert(err.message));
    }
  });
}
