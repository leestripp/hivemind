import { API } from '../api.js';
import { $, esc, log } from '../util.js';
import { S } from '../state.js';


export function enterSettings() { loadSettingsPage(); loadWorkspace(); }

// ─── Company workspace (PUT /api/config — admin scope) ─────────────────
async function loadWorkspace() {
  try {
    const c = await API.get('/api/config');
    $('set-workspace').value = c.workspace_root || '';
  } catch { /* leave placeholder */ }
}

export function initSettings() {
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
    const models = await API.get('/api/models');
    rows('set-models', models.length
      ? models.map(m => [m.name + (m.is_default ? ' ★' : ''),
          `<code>${esc(m.provider || '')}</code> ${esc(m.model || '')}`])
      : [['models', 'none configured — add via icarus CLI']]);
  } catch { rows('set-models', [['models', 'unavailable']]); }
  try {
    const a = await API.get('/api/agent/status');
    rows('set-agent', [['engine', '<code>daedalus</code> (libdaedalus.a)'],
      ['tools', String(a.tool_count ?? a.tools?.length ?? '?')],
      ['stats', `<code>${JSON.stringify(a.stats || {})}</code>`]]);
  } catch { rows('set-agent', [['engine', 'unavailable']]); }
  rows('set-storage', [
    ['workflows', '<code>~/.local/share/hivemind/hivemind.db</code>'],
    ['models/providers', '<code>~/.local/share/daedalus/daedalus.db</code>'],
    ['theme', 'dark (only theme)']]);
}

