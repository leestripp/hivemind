// js/pages/models.js — providers & models management (Phase 13).
// HiveMind-owned LLM config (LLMStore); discover hits the provider's
// live /v1/models so nothing needs hand-typing.
import { API } from '../api.js';
import { $, esc } from '../util.js';

let providers = [];
let models = [];

// ── data ──────────────────────────────────────────────────────────────
async function loadAll() {
  const [p, m] = await Promise.all([
    API.get('/api/providers'),
    API.get('/api/models'),
  ]);
  providers = p.providers || [];
  models = m || [];
  renderProviders();
  renderModels();
}

// ── providers table ───────────────────────────────────────────────────
function renderProviders() {
  const tbody = $('providers-tbody');
  if (!providers.length) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">No providers configured.</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  for (const p of providers) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${esc(p.name)}</td>
      <td class="cell-ellip">${esc(p.host)}</td>
      <td>${p.has_key ? '<span class="badge badge-ok">✓ set</span>' : '<span class="muted">—</span>'}</td>
      <td>${p.timeout_seconds}s</td>
      <td class="row-actions">
        <button class="icon-btn" data-act="discover" data-name="${esc(p.name)}" title="Discover models">⤓</button>
        <button class="icon-btn icon-danger" data-act="del" data-name="${esc(p.name)}" title="Delete provider">🗑</button>
      </td>`;
    tbody.appendChild(tr);
  }
}

// ── models table ──────────────────────────────────────────────────────
function renderModels() {
  const tbody = $('models-tbody');
  if (!models.length) {
    tbody.innerHTML = '<tr><td colspan="7" class="muted">No models configured.</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  for (const m of models) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td><button class="icon-btn star ${m.is_default ? 'star-on' : ''}" data-act="default" data-name="${esc(m.name)}" title="Make default">${m.is_default ? '★' : '☆'}</button></td>
      <td>${esc(m.name)}</td>
      <td>${esc(m.provider)}</td>
      <td class="cell-ellip">${esc(m.model)}</td>
      <td>${m.max_context_size ? (m.max_context_size / 1000) + 'k' : '—'}</td>
      <td>${m.temperature || '—'}</td>
      <td class="row-actions">
        <button class="icon-btn icon-danger" data-act="del" data-name="${esc(m.name)}" title="Delete model">🗑</button>
      </td>`;
    tbody.appendChild(tr);
  }
}

// ── modals ────────────────────────────────────────────────────────────
function openModal(id)  { $(id).classList.remove('hidden'); }
function closeModal(id) { $(id).classList.add('hidden'); }

function wireModal(id, onSubmit) {
  $(id).addEventListener('click', (ev) => { if (ev.target === $(id)) closeModal(id); });
  $(id).querySelectorAll('[data-close]').forEach(b =>
    b.addEventListener('click', () => closeModal(id)));
  const form = $(id).querySelector('form');
  if (!form) return;                      // discover modal: close-only
  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    await onSubmit(new FormData(ev.target));
    closeModal(id);
    ev.target.reset();
    loadAll();
  });
}

// ── discover ──────────────────────────────────────────────────────────
async function discover(providerName) {
  const list = $('discover-list');
  $('discover-title').textContent = `Models at ${providerName}`;
  list.innerHTML = '<p class="muted">querying /v1/models…</p>';
  openModal('discover-modal');
  try {
    const d = await API.get(`/api/providers/${encodeURIComponent(providerName)}/discover`);
    if (!d.models.length) { list.innerHTML = '<p class="muted">Provider returned an empty list.</p>'; return; }
    list.innerHTML = '';
    for (const m of d.models) {
      const row = document.createElement('div');
      row.className = 'discover-row';
      row.innerHTML = `<span class="cell-ellip">${esc(m.id)}</span>
        <button class="btn btn-small" data-id="${esc(m.id)}">+ Add</button>`;
      row.querySelector('button').addEventListener('click', () => {
        closeModal('discover-modal');
        openModelModal(m.id, providerName);
      });
      list.appendChild(row);
    }
  } catch (e) {
    list.innerHTML = `<p class="muted">unavailable — ${esc(e.message)}</p>`;
  }
}

// ── model modal (add; prefilled from discover) ────────────────────────
function openModelModal(modelId = '', provider = '') {
  const sel = $('model-provider');
  sel.innerHTML = providers.map(p =>
    `<option value="${esc(p.name)}" ${p.name === provider ? 'selected' : ''}>${esc(p.name)}</option>`).join('');
  $('model-model-id').value = modelId;
  $('model-name').value = modelId ? modelId.replace(/[^a-z0-9]+/gi, '-').toLowerCase() : '';
  openModal('model-modal');
}

// ── public ────────────────────────────────────────────────────────────
export function enterModels() { loadAll().catch(() => {}); }

export function initModels() {
  $('providers-add').addEventListener('click', () => openModal('provider-modal'));
  $('models-add').addEventListener('click', () => openModelModal());

  wireModal('provider-modal', async (fd) => {
    await API.post('/api/providers', {
      name: fd.get('name').trim(),
      host: fd.get('host').trim(),
      api_key: fd.get('api_key'),
      timeout_seconds: parseInt(fd.get('timeout_seconds')) || 30,
    });
  });

  wireModal('model-modal', async (fd) => {
    await API.post('/api/models', {
      name: fd.get('name').trim(),
      provider: fd.get('provider'),
      model_id: fd.get('model_id').trim(),
      max_context_size: parseInt(fd.get('max_context_size')) || 0,
      temperature: parseFloat(fd.get('temperature')) || 0,
      is_default: fd.get('is_default') === 'on',
    });
  });

  wireModal('discover-modal', async () => {});   // no form submit; close-only

  document.addEventListener('keydown', (ev) => {
    if (ev.key === 'Escape')
      ['provider-modal', 'model-modal', 'discover-modal'].forEach(closeModal);
  });

  // row actions (event delegation)
  $('providers-tbody').addEventListener('click', async (ev) => {
    const b = ev.target.closest('button[data-act]');
    if (!b) return;
    if (b.dataset.act === 'discover') return discover(b.dataset.name);
    if (b.dataset.act === 'del' && confirm(`Delete provider "${b.dataset.name}"?`)) {
      try { await API.del(`/api/providers/${encodeURIComponent(b.dataset.name)}`); }
      catch (e) { alert(e.message); }
      loadAll();
    }
  });

  $('models-tbody').addEventListener('click', async (ev) => {
    const b = ev.target.closest('button[data-act]');
    if (!b) return;
    if (b.dataset.act === 'default') {
      await API.post(`/api/models/${encodeURIComponent(b.dataset.name)}/default`);
      loadAll();
    }
    if (b.dataset.act === 'del' && confirm(`Delete model "${b.dataset.name}"?`)) {
      await API.del(`/api/models/${encodeURIComponent(b.dataset.name)}`);
      loadAll();
    }
  });
}
