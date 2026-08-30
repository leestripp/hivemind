// js/pages/models.js …â€”… v2: READ-ONLY model catalog.
// The hub owns no LLM config: models live in each worker's
// ~/.local/share/hive-worker/models.json (credentials in secrets.json,
// never leave the worker). GET /api/models returns the union of all
// connected workers' catalogs, worker-attributed.
import { API } from '../api.js';
import { $, esc } from '../util.js';

async function load() {
  const d = await API.get('/api/models');
  render(d.models || []);
}

function render(models) {
  const tbody = $('models-tbody');
  if (!models.length) {
    tbody.innerHTML = '<tr><td colspan="3" class="muted">No models in the ' +
      'catalog …â€”… connect a worker (models live in its ' +
      '<code>~/.local/share/hive-worker/models.json</code>).</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  for (const m of models) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${esc(m.name)}</td>
      <td>${esc(m.provider)}</td>
      <td><span class="badge">${esc(m.worker || '?')}</span></td>`;
    tbody.appendChild(tr);
  }
}

export function enterModels() { load().catch(() => {}); }

export function initModels() {
  // No add/edit/delete: the catalog is derived from worker hello frames.
}
