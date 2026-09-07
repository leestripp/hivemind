// js/pages/models.js — hub-owned provider/model registry (Phase 43).
// ONE place: providers (endpoint + key) live in the hub's llm.db; models
// belong to a provider; the hub injects the resolved config into every
// task frame. Any model can run on any machine.
//   GET    /api/providers                     list (keys masked)
//   POST   /api/providers                     upsert (blank key = keep)
//   DELETE /api/providers/:name               cascades its models
//   POST   /api/providers/:name/discover      GET {host}/v1/models, auto-add
//   GET    /api/models                        {models[], default_model}
//   POST   /api/models                        upsert {name, provider, ...}
//   DELETE /api/models/:name
//   POST   /api/models/default                {name}
import { API } from '../api.js';
import { $, esc } from '../util.js';

function openModal(id)  { $(id).classList.remove('hidden'); }
function closeModal(id) { $(id).classList.add('hidden'); }

let editingProvider = null;                    // name when editing, '' when adding
let editingModel = null;                       // {provider, name|null}
let lastModels = { models: [], default_model: '' };

async function load() {
  const [prov, mod] = await Promise.all([
    API.get('/api/providers'), API.get('/api/models')]);
  renderProviders(prov.providers || []);
  lastModels = mod;
  renderModels(mod.models || [], mod.default_model || '');
}

// ── Providers table ────────────────────────────────────────────────────

function renderProviders(providers) {
  const tb = $('providers-tbody');
  if (!providers.length) {
    tb.innerHTML = '<tr><td colspan="5" class="muted">No providers yet — add one, then Discover its models.</td></tr>';
    return;
  }
  tb.innerHTML = providers.map(p => `<tr>
    <td><strong>${esc(p.name)}</strong></td>
    <td>${esc(p.host || '—')}</td>
    <td class="muted" style="max-width:260px;overflow:hidden;text-overflow:ellipsis">${esc(p.chat_endpoint || '—')}</td>
    <td>${p.has_key ? '<code>' + esc(p.api_key) + '</code>' : '<span class="muted">none</span>'}</td>
    <td style="white-space:nowrap">
      <button class="btn btn-small" data-act="discover" data-name="${esc(p.name)}" title="Discover models via {host}/v1/models">🔍 Discover</button>
      <button class="btn btn-small" data-act="edit" data-name="${esc(p.name)}" title="Edit">✎</button>
      <button class="btn btn-small" data-act="delete" data-name="${esc(p.name)}" title="Delete (cascades its models)">🗑</button>
    </td>
  </tr>`).join('');
  tb.querySelectorAll('button[data-act]').forEach(b => {
    b.onclick = () => onProviderAction(b.dataset.act, b.dataset.name, b);
  });
}

async function onProviderAction(act, name, btn) {
  try {
    if (act === 'discover') {
      btn.disabled = true; btn.textContent = '…';
      const r = await API.post(`/api/providers/${encodeURIComponent(name)}/discover`, {});
      const n = (r.discovered || []).length;
      btn.textContent = n ? `✓ ${n} models` : '✓ none new';
      setTimeout(load, 600);
    } else if (act === 'edit') {
      const p = (await API.get('/api/providers')).providers.find(x => x.name === name);
      editingProvider = name;
      $('provider-name').value = name;
      $('provider-name').disabled = true;
      $('provider-host').value = p ? p.host : '';
      $('provider-endpoint').value = p ? p.chat_endpoint : '';
      $('provider-key').value = '';
      $('provider-modal-title').textContent = `Edit provider: ${name}`;
      openModal('provider-modal');
    } else if (act === 'delete') {
      if (!confirm(`Delete provider "${name}" and ALL its models?`)) return;
      await API.req('DELETE', `/api/providers/${encodeURIComponent(name)}`);
      setTimeout(load, 300);
    }
  } catch { btn.disabled = false; btn.textContent = '🔍 Discover'; }
}

// ── Models, grouped per provider ───────────────────────────────────────

function renderModels(models, defaultModel) {
  const host = $('models-workers');
  host.innerHTML = '';
  if (!models.length) {
    host.innerHTML = '<p class="muted">No models yet — add a provider above and hit 🔍 Discover, or add models by hand.</p>';
    return;
  }
  const byProv = {};
  for (const m of models) (byProv[m.provider] = byProv[m.provider] || []).push(m);
  for (const prov of Object.keys(byProv).sort())
    host.appendChild(providerCard(prov, byProv[prov], defaultModel));
}

function providerCard(prov, models, defaultModel) {
  const card = document.createElement('div');
  card.className = 'card';
  card.style.marginBottom = '16px';
  const rows = models.map(m => `<tr>
    <td><strong>${esc(m.name)}</strong>${m.name === defaultModel ? ' <span class="badge">default</span>' : ''}</td>
    <td>${esc(m.model_id || m.name)}</td>
    <td>${m.max_context || '—'}</td>
    <td>${m.temperature ?? '—'}</td>
    <td style="white-space:nowrap">
      <button class="btn btn-small" data-act="default" data-name="${esc(m.name)}" title="Set as hub default">★</button>
      <button class="btn btn-small" data-act="edit" data-name="${esc(m.name)}" title="Edit">✎</button>
      <button class="btn btn-small" data-act="delete" data-name="${esc(m.name)}" title="Delete">🗑</button>
    </td>
  </tr>`).join('');
  card.innerHTML = `
    <div class="page-head" style="margin-bottom:8px">
      <div><h3 style="margin:0">${esc(prov)}</h3></div>
      <button class="btn" data-act="add">+ Add model</button>
    </div>
    <table class="kv-table">
      <thead><tr><th>Name</th><th>Model ID</th><th>Ctx</th><th>Temp</th><th></th></tr></thead>
      <tbody>${rows}</tbody>
    </table>`;
  card.querySelectorAll('button[data-act]').forEach(b => {
    b.onclick = () => onModelAction(prov, b.dataset.act, b.dataset.name || null, b);
  });
  return card;
}

async function onModelAction(prov, act, name, btn) {
  try {
    if (act === 'default') {
      btn.disabled = true;
      await API.post('/api/models/default', { name });
      setTimeout(load, 300);
    } else if (act === 'edit') {
      const m = lastModels.models.find(x => x.name === name);
      editingModel = { provider: prov, name };
      fillModal(name, m || {});
      openModal('model-edit-modal');
    } else if (act === 'delete') {
      if (!confirm(`Delete model "${name}"?`)) return;
      await API.req('DELETE', `/api/models/${encodeURIComponent(name)}`);
      setTimeout(load, 300);
    } else if (act === 'add') {
      editingModel = { provider: prov, name: null };
      fillModal('', {});
      openModal('model-edit-modal');
    }
  } catch {}
}

function fillModal(name, e) {
  $('model-name').value = name;
  $('model-name').disabled = !!name;      // name is the key — immutable on edit
  $('model-provider').value = editingModel ? editingModel.provider : '';
  $('model-modelid').value = e.model_id || '';
  $('model-context').value = e.max_context || '';
  $('model-temp').value = (e.temperature ?? '') === 0 ? '0' : (e.temperature ?? '');
  $('model-modal-title').textContent = name ? `Edit model: ${name}` : 'Add model';
}

export function enterModels() { load().catch(() => {}); }

export function initModels() {
  $('provider-add').onclick = () => {
    editingProvider = '';
    $('provider-name').value = '';
    $('provider-name').disabled = false;
    $('provider-host').value = '';
    $('provider-endpoint').value = '';
    $('provider-key').value = '';
    $('provider-modal-title').textContent = 'Add provider';
    openModal('provider-modal');
  };

  $('provider-save').onclick = async () => {
    const body = {
      name: (editingProvider || $('provider-name').value).trim(),
      host: $('provider-host').value.trim(),
      chat_endpoint: $('provider-endpoint').value.trim(),
    };
    const key = $('provider-key').value.trim();
    if (key) body.api_key = key;
    if (!body.name || !body.host) { alert('name and host are required'); return; }
    const btn = $('provider-save');
    btn.disabled = true;
    try {
      await API.post('/api/providers', body);
      closeModal('provider-modal');
      setTimeout(load, 300);
    } catch {} finally { btn.disabled = false; }
  };

  $('model-edit-save').onclick = async () => {
    if (!editingModel) return;
    const body = {
      name: $('model-name').value.trim(),
      provider: editingModel.provider,
    };
    const mid = $('model-modelid').value.trim();
    const ctx = parseInt($('model-context').value, 10);
    const tmp = parseFloat($('model-temp').value);
    if (mid) body.model_id = mid;
    if (!isNaN(ctx)) body.max_context = ctx;
    if (!isNaN(tmp)) body.temperature = tmp;
    if (!body.name) { alert('name is required'); return; }
    const btn = $('model-edit-save');
    btn.disabled = true;
    try {
      await API.post('/api/models', body);
      closeModal('model-edit-modal');
      setTimeout(load, 300);
    } catch {} finally { btn.disabled = false; }
  };

  document.querySelectorAll('#model-edit-modal [data-close], #provider-modal [data-close]')
    .forEach(b => b.onclick = () => {
      closeModal('model-edit-modal');
      closeModal('provider-modal');
    });
}
