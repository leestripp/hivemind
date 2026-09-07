// js/pages/models.js — per-worker model registry management.
// Registry of record lives on each worker (models.json + secrets.json,
// 0600); the hub RELAYS management ops over the worker channel and never
// persists credentials (model-registry unification, Phase 3).
//   GET    /api/models                       connected workers + catalog
//   GET    /api/workers/:name/models         full entries (secret-free)
//   POST   /api/workers/:name/models         {name, provider, host, ...}
//   DELETE /api/workers/:name/models/:model  (refuses the default)
//   POST   /api/workers/:name/default-model  {model}
//   POST   /api/workers/:name/secrets        {provider, key} — write-only
import { API } from '../api.js';
import { $, esc } from '../util.js';

function openModal(id)  { $(id).classList.remove('hidden'); }
function closeModal(id) { $(id).classList.add('hidden'); }

let editing = null;   // {worker, name} when editing, {worker, name:null} when adding

async function load() {
  const d = await API.get('/api/models');
  const workers = d.models || [];
  // Detail per worker (full entries) — best-effort; fall back to the
  // hello catalog (name+provider only) when the detail call fails.
  const detailed = await Promise.all(workers.map(async w => {
    try {
      const det = await API.get(`/api/workers/${encodeURIComponent(w.worker)}/models`);
      return { worker: w.worker, default: det.default || '',
               entries: det.models || {} };
    } catch {
      const entries = {};
      for (const m of (w.models || [])) entries[m.name] = { provider: m.provider };
      return { worker: w.worker, default: w.default_model || '', entries };
    }
  }));
  render(detailed);
}

function render(workers) {
  const host = $('models-workers');
  host.innerHTML = '';
  if (!workers.length) {
    host.innerHTML = '<p class="muted">No workers connected.</p>';
    return;
  }
  for (const w of workers) host.appendChild(workerCard(w));
}

function workerCard(w) {
  const card = document.createElement('div');
  card.className = 'card';
  card.style.marginBottom = '16px';

  const names = Object.keys(w.entries);
  const rows = names.length ? names.map(name => {
    const e = w.entries[name] || {};
    return `<tr>
      <td><strong>${esc(name)}</strong>${name === w.default ? ' <span class="badge">default</span>' : ''}</td>
      <td>${esc(e.provider || '')}</td>
      <td>${esc(e.model_id || '—')}</td>
      <td class="muted" style="max-width:220px;overflow:hidden;text-overflow:ellipsis">${esc(e.host || '—')}</td>
      <td>${e.max_context || '—'}</td>
      <td>${e.temperature ?? '—'}</td>
      <td style="white-space:nowrap">
        <button class="btn btn-small" data-act="default" data-name="${esc(name)}" title="Set as worker default">★</button>
        <button class="btn btn-small" data-act="edit" data-name="${esc(name)}" title="Edit">✎</button>
        <button class="btn btn-small" data-act="delete" data-name="${esc(name)}" title="Delete">🗑</button>
      </td>
    </tr>`;
  }).join('') : '<tr><td colspan="7" class="muted">no models in this worker’s models.json</td></tr>';

  card.innerHTML = `
    <div class="page-head" style="margin-bottom:8px">
      <div><h3 style="margin:0">${esc(w.worker)}</h3></div>
      <div>
        <button class="btn" data-act="add">+ Add model</button>
        <button class="btn" data-act="secret" style="margin-left:6px">🔑 Set API key</button>
      </div>
    </div>
    <table class="kv-table">
      <thead><tr><th>Name</th><th>Provider</th><th>Model ID</th><th>Host</th><th>Ctx</th><th>Temp</th><th></th></tr></thead>
      <tbody>${rows}</tbody>
    </table>
    <p class="muted" style="margin:8px 0 0">Agents with model set to
      <em>worker default</em> spawn on <strong>${esc(w.default || '—')}</strong>.
      Changes apply to future spawns; running agents keep their env.</p>`;

  card.querySelectorAll('button[data-act]').forEach(btn => {
    btn.onclick = () => onAction(w, btn.dataset.act, btn.dataset.name || null, btn);
  });
  return card;
}

async function onAction(w, act, name, btn) {
  const wname = encodeURIComponent(w.worker);
  try {
    if (act === 'default') {
      btn.disabled = true;
      await API.post(`/api/workers/${wname}/default-model`, { model: name });
      setTimeout(load, 300);
    } else if (act === 'edit') {
      editing = { worker: w.worker, name };
      fillModal(name, w.entries[name] || {});
      openModal('model-edit-modal');
    } else if (act === 'delete') {
      if (!confirm(`Delete model "${name}" from ${w.worker}'s models.json?`)) return;
      await API.req('DELETE', `/api/workers/${wname}/models/${encodeURIComponent(name)}`);
      setTimeout(load, 300);
    } else if (act === 'add') {
      editing = { worker: w.worker, name: null };
      fillModal('', {});
      openModal('model-edit-modal');
    } else if (act === 'secret') {
      editing = { worker: w.worker };
      $('secret-provider').value = '';
      $('secret-key').value = '';
      openModal('model-secret-modal');
    }
  } catch (e) { /* notifyError already shown by API.req */ }
}

function fillModal(name, e) {
  $('model-name').value = name;
  $('model-name').disabled = !!name;      // name is the key — immutable on edit
  $('model-provider').value = e.provider || '';
  $('model-modelid').value = e.model_id || '';
  $('model-host').value = e.host || '';
  $('model-endpoint').value = e.endpoint || '';
  $('model-context').value = e.max_context || '';
  $('model-temp').value = (e.temperature ?? '') === 0 ? '0' : (e.temperature ?? '');
  $('model-modal-title').textContent = name ? `Edit model: ${name}` : 'Add model';
}

export function enterModels() { load().catch(() => {}); }

export function initModels() {
  $('model-edit-save').onclick = async () => {
    if (!editing) return;
    const body = {
      name: $('model-name').value.trim(),
      provider: $('model-provider').value.trim(),
      host: $('model-host').value.trim(),
    };
    const mid = $('model-modelid').value.trim();
    const ep  = $('model-endpoint').value.trim();
    const ctx = parseInt($('model-context').value, 10);
    const tmp = parseFloat($('model-temp').value);
    if (mid) body.model_id = mid;
    if (ep)  body.endpoint = ep;
    if (!isNaN(ctx)) body.max_context = ctx;
    if (!isNaN(tmp)) body.temperature = tmp;
    if (!body.name || !body.provider || !body.host) {
      alert('name, provider and host are required');
      return;
    }
    const btn = $('model-edit-save');
    btn.disabled = true;
    try {
      await API.post(`/api/workers/${encodeURIComponent(editing.worker)}/models`, body);
      closeModal('model-edit-modal');
      setTimeout(load, 300);
    } catch {} finally { btn.disabled = false; }
  };

  $('model-secret-save').onclick = async () => {
    if (!editing) return;
    const provider = $('secret-provider').value.trim();
    const key = $('secret-key').value.trim();
    if (!provider || !key) { alert('provider and key are required'); return; }
    const btn = $('model-secret-save');
    btn.disabled = true;
    try {
      await API.post(`/api/workers/${encodeURIComponent(editing.worker)}/secrets`,
                     { provider, key });
      closeModal('model-secret-modal');
    } catch {} finally { btn.disabled = false; }
  };

  document.querySelectorAll('#model-edit-modal [data-close], #model-secret-modal [data-close]')
    .forEach(b => b.onclick = () => {
      closeModal('model-edit-modal');
      closeModal('model-secret-modal');
    });
}
