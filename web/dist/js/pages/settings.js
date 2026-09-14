import { API } from '../api.js';
import { $, esc, getTheme, setTheme, getTimezone, setTimezone } from '../util.js';
import { S } from '../state.js';


export function enterSettings() { loadSettingsPage(); loadConnectors(); }

// ── Phase 23: OAuth connectors ────────────────────────────────────────
async function loadConnectors() {
  const host = $('connectors-list');
  try {
    const d = await API.get('/api/connectors');
    const cs = d.connectors || [];
    if (!cs.length) { host.innerHTML = '<div class="muted">No connectors registered yet.</div>'; return; }
    host.innerHTML = '';
    for (const c of cs) {
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;align-items:center;gap:8px;padding:4px 0';
      row.innerHTML = `<strong>${esc(c.name)}</strong>
        <span class="muted">${esc(c.scopes || '')}</span>
        <span class="badge">${c.connected ? '✓ connected' : 'not connected'}</span>
        <code class="muted" style="font-size:11px">${esc(c.token_secret)}</code>`;
      const btn = document.createElement('button');
      btn.className = 'btn btn-sm';
      btn.textContent = c.connected ? 'Reconnect' : 'Connect';
      btn.onclick = async () => {
        try {
          const r = await API.get('/api/connectors/' + encodeURIComponent(c.name) + '/auth-url');
          window.open(r.url, '_blank');
        } catch (err) { alert(err.message); }
      };
      const del = document.createElement('button');
      del.className = 'icon-btn icon-danger';
      del.textContent = '🗑';
      del.title = 'Remove connector (stored tokens stay in the broker)';
      del.onclick = async () => {
        if (!confirm(`Remove connector "${c.name}"? Stored tokens are kept.`)) return;
        try { await API.del('/api/connectors/' + encodeURIComponent(c.name)); loadConnectors(); }
        catch (err) { alert(err.message); }
      };
      row.appendChild(btn);
      row.appendChild(del);
      host.appendChild(row);
    }
  } catch (e) { host.innerHTML = `<div class="muted">${esc(e.message)}</div>`; }
}

// Workspaces are per-worker (v2) — the old hub "host folder" setting was
// dead config; the Settings page now just documents the worker path.

export function initSettings() {
  $('connector-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    try {
      await API.post('/api/connectors', {
        name: f.name.value.trim(),
        auth_url: f.auth_url.value.trim(),
        token_url: f.token_url.value.trim(),
        client_id: f.client_id.value.trim(),
        client_secret: f.client_secret.value,
        scopes: f.scopes.value.trim(),
      });
      f.reset();
      loadConnectors();
    } catch (err) { alert(err.message); }
  });

  // Phase 19: appearance + timezone (client-side, instant).
  const themeSel = $('set-theme');
  themeSel.value = getTheme();
  themeSel.addEventListener('change', () => setTheme(themeSel.value));
  const tzSel = $('set-tz');
  const cur = getTimezone();
  tzSel.value = (cur === 'browser')
    ? 'browser'
    : (cur === '' ? '' : cur);
  if (tzSel.value !== cur && cur && cur !== 'browser') tzSel.value = cur;
  tzSel.addEventListener('change', () => {
    setTimezone(tzSel.value === 'browser'
      ? Intl.DateTimeFormat().resolvedOptions().timeZone || ''
      : tzSel.value);
  });

  // Phase 55: server-side message window (initial thread load size).
  $('set-msg-window-save').addEventListener('click', async () => {
    const n = parseInt($('set-msg-window').value, 10);
    const status = $('set-msg-window-status');
    if (!Number.isInteger(n) || n < 10 || n > 5000) {
      status.textContent = ' — must be 10–5000'; return;
    }
    try {
      await API.put('/api/config', { message_window: n });
      status.textContent = ' — saved ✓';
      setTimeout(() => { status.textContent = ''; }, 3000);
    } catch (err) { status.textContent = ' — ' + err.message; }
  });

}

// ─── Settings page ──────────────────────────────────────────────────────
async function loadSettingsPage() {
  const rows = (el, pairs) => {
    $(el).innerHTML = pairs.map(([k, v]) =>
      `<div class="k">${esc(k)}</div><div>${v}</div>`).join('');
  };
  try {
    const h = await API.get('/api/health');
    rows('set-server', [['status', esc(h.status)], ['service', esc(h.service)],
      ['address', '<code>' + location.host + '</code>']]);
  } catch { rows('set-server', [['status', 'offline']]); }
  try {
    // Phase 43: hub-owned registry ({name, provider}) + hub default.
    const d = await API.get('/api/models');
    const models = d.models || [];
    rows('set-models', models.length
      ? models.map(m => [m.name + (m.name === d.default_model ? ' (default)' : ''),
          `<code>${esc(m.provider || '')}</code>`])
      : [['models', 'none - add a provider on the Models page']]);
  } catch { rows('set-models', [['models', 'unavailable']]); }
  try {
    // v2: brains are hive-agent engines in containers on workers.
    const w = await API.get('/api/workers');
    const workers = w.workers || [];
    const online = workers.filter(x => x.online);
    rows('set-agent', [
      ['engine', '<code>hive-agent</code> brains in podman containers'],
      ['workers', workers.length
        ? workers.map(x => esc(x.name) + (x.online ? ' (online)' : ' (offline)')).join(', ')
        : 'none enrolled'],
      ['online', String(online.length)]]);
  } catch { rows('set-agent', [['workers', 'unavailable']]); }
  rows('set-storage', [
    ['hub data', '<code>~/.local/share/hivemind/</code> (threads, roster, keys)'],
    ['models/keys', 'worker-side: <code>~/.local/share/hive-worker/</code>'],
    ['theme', 'dark (only theme)']]);
  try {
    const cfg = await API.get('/api/config');
    $('set-msg-window').value = cfg.message_window ?? 200;
  } catch { /* leave default */ }
}

