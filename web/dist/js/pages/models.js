// js/pages/models.js — per-worker model catalog + default-model control.
// Models live in each worker's ~/.local/share/hive-worker/models.json
// (credentials in secrets.json, never leave the worker). GET /api/models
// returns one entry per connected worker: {worker, default_model, models}.
// Agents whose model is "" resolve to the worker's default at spawn.
import { API } from '../api.js';
import { $, esc } from '../util.js';

async function load() {
  const d = await API.get('/api/models');
  render(d.models || []);
}

function render(workers) {
  const host = $('models-workers');
  host.innerHTML = '';
  if (!workers.length) {
    host.innerHTML = '<p class="muted">No worker catalogs — connect a ' +
      'worker (models live in its ' +
      '<code>~/.local/share/hive-worker/models.json</code>).</p>';
    return;
  }
  for (const w of workers) host.appendChild(workerCard(w));
}

function workerCard(w) {
  const card = document.createElement('div');
  card.className = 'card';
  card.style.marginBottom = '16px';

  const models = w.models || [];
  const opts = models.map(m =>
    `<option value="${esc(m.name)}"${m.name === w.default_model ? ' selected' : ''}>` +
    `${esc(m.name)}</option>`).join('');

  card.innerHTML = `
    <div class="page-head" style="margin-bottom:8px">
      <div><h3 style="margin:0">${esc(w.worker)}</h3></div>
      <div>
        <label class="muted" style="margin-right:6px">Default model</label>
        <select class="model-default-sel">${opts || '<option value="">— none —</option>'}</select>
        <button class="btn model-default-save" style="margin-left:6px">Set default</button>
        <span class="muted model-default-msg" style="margin-left:8px"></span>
      </div>
    </div>
    <table class="kv-table">
      <thead><tr><th>Name</th><th>Provider</th><th></th></tr></thead>
      <tbody>${
        models.length
          ? models.map(m => `<tr>
              <td>${esc(m.name)}</td>
              <td>${esc(m.provider)}</td>
              <td>${m.name === w.default_model
                    ? '<span class="badge">default</span>' : ''}</td>
            </tr>`).join('')
          : '<tr><td colspan="3" class="muted">no models in this worker\u2019s models.json</td></tr>'
      }</tbody>
    </table>
    <p class="muted" style="margin:8px 0 0">Agents with model set to
      <em>worker default</em> spawn on <strong>${esc(w.default_model || '—')}</strong>
      (resolved on ${esc(w.worker)}).</p>`;

  const btn = card.querySelector('.model-default-save');
  btn.onclick = async () => {
    const sel = card.querySelector('.model-default-sel');
    const msg = card.querySelector('.model-default-msg');
    const model = sel.value;
    if (!model) return;
    btn.disabled = true; msg.textContent = 'saving…';
    try {
      await API.post(`/api/workers/${encodeURIComponent(w.worker)}/default-model`,
                     { model });
      msg.textContent = 'saved';
      setTimeout(load, 400);
    } catch (e) {
      msg.textContent = 'failed: ' + (e.message || e);
    } finally {
      btn.disabled = false;
    }
  };
  return card;
}

export function enterModels() { load().catch(() => {}); }

export function initModels() {
  // Per-worker default pickers are wired per render (workerCard).
}
