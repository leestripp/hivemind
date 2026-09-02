// js/util.js — shared DOM/format helpers.
export const $ = (id) => document.getElementById(id);

export function esc(s) {
  return String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}


// Phase 19: structured error notices — dismissible toast with a
// copyable request id (matches the server's [req-…] stderr log line).
export function notifyError(message, requestId) {
  let host = $('error-toasts');
  if (!host) {
    host = document.createElement('div');
    host.id = 'error-toasts';
    document.body.appendChild(host);
  }
  const t = document.createElement('div');
  t.className = 'error-toast';
  const rid = requestId || '';
  t.innerHTML = `<span class="error-toast-msg">${esc(message)}</span>` +
    (rid ? `<code class="error-toast-id">${esc(rid)}</code>` : '');
  const copy = document.createElement('button');
  copy.className = 'btn btn-sm';
  copy.textContent = 'Copy ID';
  copy.disabled = !rid;
  copy.onclick = () => {
    navigator.clipboard?.writeText(rid);
    copy.textContent = 'Copied';
    setTimeout(() => { copy.textContent = 'Copy ID'; }, 1200);
  };
  const close = document.createElement('button');
  close.className = 'icon-btn';
  close.textContent = '✕';
  close.onclick = () => t.remove();
  t.appendChild(copy);
  t.appendChild(close);
  host.appendChild(t);
  setTimeout(() => { t.classList.add('fade'); setTimeout(() => t.remove(), 400); }, 9000);
}

// Phase 19: client-side display prefs (localStorage — cosmetic only).
export function getTheme() { return localStorage.getItem('hivemind_theme') || 'dark'; }
export function setTheme(t) {
  localStorage.setItem('hivemind_theme', t);
  document.documentElement.dataset.theme = t;
}
export function getTimezone() { return localStorage.getItem('hivemind_tz') || ''; }
export function setTimezone(tz) { localStorage.setItem('hivemind_tz', tz); }

// Schedule times are stored server-local without an offset. When the
// browser shares the host (the normal deployment), Date parses them as
// local — re-render in the preferred zone. '' = show server-local as-is.
export function fmtTimeTZ(iso) {
  if (!iso) return '—';
  const tz = getTimezone();
  if (!tz) return iso.replace('T', ' ').slice(0, 16);
  const d = new Date(iso);
  if (isNaN(d)) return iso;
  return d.toLocaleString(undefined, {
    timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', hour12: false,
  }).replace(',', '');
}

// ─── Real backend integration ─────────────────────────────────────────
// The canvas state (S.nodes / S.edges) is mirrored to the backend:
//   1. ensure a graph row exists (S.graphId), replacing nodes+edges
//   2. POST /api/graphs/:gid/run  → run_id
//   3. poll /api/runs/:rid/output every 400 ms, render events live
// The server walks the graph; the UI only renders events.

