import { API } from '../api.js';
import { $, esc, log, getTheme, setTheme, getTimezone, setTimezone } from '../util.js';
import { S } from '../state.js';


export function enterSettings() { loadSettingsPage(); loadWorkspace(); loadConnectors(); }

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

// ─── Company workspace (PUT /api/config — admin scope) ─────────────────
async function loadWorkspace() {
  try {
    const c = await API.get('/api/config');
    $('set-workspace').value = c.workspace_root || '';
  } catch { /* leave placeholder */ }
}

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

  $('set-workspace-save').addEventListener('click', async () => {
    const msg = $('set-workspace-msg');
    msg.textContent = '';
    try {
      const r = await API.req('PUT', '/api/config',
                              { workspace_root: $('set-workspace').value.trim() });
      msg.textContent = '✓ workspace set to ' + r.workspace_root;
    } catch (e) {
      msg.textContent = '✗ ' + e.message;
    }
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
    // v2: read-only catalog from connected workers ({name, provider, worker}).
    const d = await API.get('/api/models');
    const models = d.models || [];
    rows('set-models', models.length
      ? models.map(m => [m.name,
          `<code>${esc(m.provider || '')}</code> via ${esc(m.worker || '?')}`])
      : [['models', 'none - connect a worker (models.json lives worker-side)']]);
  } catch { rows('set-models', [['models', 'unavailable']]); }
  try {
    // v2: brains are daedalus agents in containers on workers.
    const w = await API.get('/api/workers');
    const workers = w.workers || [];
    const online = workers.filter(x => x.online);
    rows('set-agent', [
      ['engine', '<code>daedalus</code> brains in podman containers'],
      ['workers', workers.length
        ? workers.map(x => esc(x.name) + (x.online ? ' (online)' : ' (offline)')).join(', ')
        : 'none enrolled'],
      ['online', String(online.length)]]);
  } catch { rows('set-agent', [['workers', 'unavailable']]); }
  rows('set-storage', [
    ['hub data', '<code>~/.local/share/hivemind/</code> (threads, roster, keys)'],
    ['models/keys', 'worker-side: <code>~/.local/share/hive-worker/</code>'],
    ['theme', 'dark (only theme)']]);
}

