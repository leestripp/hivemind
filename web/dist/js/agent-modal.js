// Full Add/Edit Agent dialog. Worker moves deliberately use migration-modal.js.
import { API } from './api.js';
import { $, esc } from './util.js';
import { AVATARS, avatarSrc } from './avatars.js';

let editor = null;
let generation = 0;

function closeEditor() {
  generation++;
  editor?.classList.add('hidden');
}

function ensureEditor() {
  if (editor) return editor;
  editor = document.createElement('div');
  editor.id = 'agent-editor-modal';
  editor.className = 'modal-overlay hidden';
  const avatarChoices = AVATARS.map((name) => {
    const label = name.replaceAll('-', ' ');
    return `<label class="avatar-option" title="${esc(label)}">
      <input type="radio" name="agent-avatar" value="${esc(name)}" aria-label="${esc(label)}">
      <img src="${avatarSrc({ name: '', avatar: name })}" width="42" height="42" alt="">
    </label>`;
  }).join('');
  editor.innerHTML = `
    <section class="modal-panel agent-editor-panel" role="dialog" aria-modal="true" aria-labelledby="am-title">
      <header class="modal-head">
        <div><h2 id="am-title">New agent</h2><p class="muted" id="am-subtitle"></p></div>
        <button type="button" class="icon-btn" id="am-close" aria-label="Close">✕</button>
      </header>
      <form id="am-form">
        <div class="agent-editor-sections">
          <fieldset class="agent-form-section">
            <legend>Identity</legend>
            <div class="agent-form-grid">
              <div class="form-field agent-wide" id="am-template-field">
                <label for="am-template">Template</label>
                <select id="am-template"><option value="">Blank agent</option></select>
              </div>
              <div class="form-field">
                <label for="am-name">Name</label>
                <input id="am-name" required autocomplete="off" placeholder="e.g. Nova">
              </div>
              <div class="form-field">
                <label for="am-role">Role</label>
                <input id="am-role" autocomplete="off" placeholder="e.g. Research assistant">
              </div>
            </div>
            <fieldset class="avatar-fieldset">
              <legend>Avatar</legend>
              <div class="avatar-grid">
                <label class="avatar-auto">
                  <input type="radio" name="agent-avatar" value="" checked>
                  <span>Auto — choose from the agent’s name</span>
                </label>
                ${avatarChoices}
              </div>
            </fieldset>
          </fieldset>

          <fieldset class="agent-form-section">
            <legend>Runtime &amp; resources</legend>
            <div class="agent-form-grid">
              <div class="form-field">
                <label for="am-worker">Worker</label>
                <select id="am-worker" required></select>
                <small class="field-hint" id="am-worker-note">Where this agent runs.</small>
              </div>
              <div class="form-field">
                <label for="am-type">Runtime type</label>
                <select id="am-type"></select>
              </div>
              <div class="form-field container-cap">
                <label for="am-memory">Memory cap</label>
                <input id="am-memory" autocomplete="off" placeholder="Worker default — e.g. 32g, 1536m">
              </div>
              <div class="form-field container-cap">
                <label for="am-cpus">CPU cap</label>
                <input id="am-cpus" type="number" min="0" step="0.5" placeholder="Worker default">
              </div>
            </div>
            <button type="button" class="btn agent-wide hidden" id="am-migrate">⇄ Migrate to another worker…</button>
            <div id="am-host-warn" class="danger-callout hidden" role="warning">
              <strong>⚠ Bare-metal host agent</strong>
              <p>This runs directly on the worker as a real user with passwordless sudo — no container and no sandbox. It can modify or destroy the machine.</p>
            </div>
          </fieldset>

          <fieldset class="agent-form-section agent-wide">
            <legend>Intelligence</legend>
            <div class="agent-form-grid agent-three-columns">
              <div class="form-field">
                <label for="am-provider">Provider</label>
                <select id="am-provider"></select>
              </div>
              <div class="form-field">
                <label for="am-model">Model</label>
                <select id="am-model"></select>
              </div>
              <div class="form-field">
                <label for="am-thinking">Thinking level</label>
                <select id="am-thinking">
                  <option value="">Inherit from model</option>
                  <option value="none">None</option>
                  <option value="low">Low</option>
                  <option value="high">High</option>
                  <option value="max">Max</option>
                </select>
              </div>
            </div>
          </fieldset>
        </div>
        <p class="form-error hidden" id="am-error" role="alert"></p>
        <footer class="modal-actions">
          <button type="button" class="btn" id="am-cancel">Cancel</button>
          <button type="submit" class="btn btn-primary" id="am-save">Create agent</button>
        </footer>
      </form>
    </section>`;
  document.body.appendChild(editor);
  $('am-close').addEventListener('click', closeEditor);
  $('am-cancel').addEventListener('click', closeEditor);
  return editor;
}

function selectedAvatar() {
  return editor.querySelector('input[name="agent-avatar"]:checked')?.value ?? '';
}

function selectAvatar(value) {
  const choices = [...editor.querySelectorAll('input[name="agent-avatar"]')];
  const match = choices.find((input) => input.value === value) ?? choices[0];
  match.checked = true;
}

function setError(message = '') {
  $('am-error').textContent = message;
  $('am-error').classList.toggle('hidden', !message);
}

function workerHasCap(worker, cap) {
  return !!worker?.caps && typeof worker.caps === 'object' && cap in worker.caps;
}

function addOption(select, value, label) {
  const option = document.createElement('option');
  option.value = value;
  option.textContent = label;
  select.appendChild(option);
}

export function openAgentModal(existing = null, prefill = null,
                               { onSaved = () => {}, onMigrate = null } = {}) {
  ensureEditor();
  const ticket = ++generation;
  editor.classList.remove('hidden');
  editor.setAttribute('aria-busy', 'true');
  $('am-save').disabled = true;
  setError();
  prepareEditor(existing, prefill, { onSaved, onMigrate }, ticket).catch((error) => {
    if (ticket !== generation) return;
    setError(`Could not load agent settings — ${error.message}`);
    $('am-save').disabled = true;
  }).finally(() => {
    if (ticket === generation) editor.setAttribute('aria-busy', 'false');
  });
}

async function prepareEditor(existing, prefill, callbacks, ticket) {
  const editing = !!existing;
  const identity = existing || prefill;
  // Never show or submit the previously edited agent while async options load.
  // This function runs synchronously up to the first await, so identity is
  // correct from the user's first visible frame of the popup.
  $('am-form').onsubmit = (event) => event.preventDefault();
  $('am-migrate').onclick = null;
  $('am-name').value = identity?.name || '';
  $('am-role').value = identity?.role || '';
  $('am-memory').value = existing?.memory || '';
  $('am-cpus').value = existing?.cpus > 0 ? existing.cpus : '';
  $('am-thinking').value = existing?.thinking_level || '';
  selectAvatar(identity?.avatar || '');
  for (const id of ['am-template', 'am-worker', 'am-type', 'am-provider', 'am-model']) {
    const select = $(id);
    select.replaceChildren();
    select.onchange = null;
  }
  $('am-host-warn').classList.add('hidden');
  editor.querySelectorAll('.container-cap').forEach((field) =>
    field.classList.remove('hidden'));
  $('am-title').textContent = editing ? `Edit ${existing.name}` : 'New agent';
  $('am-subtitle').textContent = editing
    ? 'Configuration changes apply to this agent. Use Migrate for a state-preserving worker move.'
    : 'Create a teammate with its runtime, resources, model, and identity.';
  $('am-save').textContent = editing ? 'Save changes' : 'Create agent';
  $('am-template-field').classList.toggle('hidden', editing);
  $('am-migrate').classList.toggle('hidden', !editing || !callbacks.onMigrate);
  $('am-migrate').onclick = () => {
    closeEditor();
    callbacks.onMigrate?.(existing);
  };

  const [workersResult, modelsResult, templatesResult] = await Promise.allSettled([
    API.get('/api/workers'),
    API.get('/api/models'),
    editing ? Promise.resolve({ templates: [] }) : API.get('/api/templates'),
  ]);
  if (ticket !== generation) return;
  const workers = workersResult.status === 'fulfilled'
    ? workersResult.value.workers || [] : [];
  const modelData = modelsResult.status === 'fulfilled'
    ? modelsResult.value : { models: [], default_model: '' };
  const models = modelData.models || [];
  const templates = templatesResult.status === 'fulfilled'
    ? templatesResult.value.templates || [] : [];
  if (workersResult.status === 'rejected') {
    setError(`Workers unavailable — ${workersResult.reason.message}`);
  }

  const workerSelect = $('am-worker');
  workerSelect.replaceChildren();
  const currentWorker = existing?.worker || '';
  const eligible = workers.filter((worker) =>
    editing ? worker.name === currentWorker : worker.online);
  for (const worker of eligible) {
    addOption(workerSelect, worker.name,
      worker.online ? worker.name : `${worker.name} (offline)`);
  }
  workerSelect.disabled = editing || !eligible.length;
  $('am-worker-note').textContent = editing
    ? 'Placement is changed with Migrate so memory and state travel too.'
    : eligible.length ? 'Only online workers can start a new agent.'
      : 'No online worker — enroll or connect one before creating an agent.';

  const typeSelect = $('am-type');
  const currentType = existing?.host_agent ? 'host' : existing?.type || '';
  const refreshType = () => {
    const worker = workers.find((item) => item.name === workerSelect.value);
    const previous = typeSelect.value || currentType;
    typeSelect.replaceChildren();
    addOption(typeSelect, '', 'Standard container');
    if (workerHasCap(worker, 'gpu')) addOption(typeSelect, 'gpu', 'GPU container');
    if (workerHasCap(worker, 'host_agents') || currentType === 'host') {
      addOption(typeSelect, 'host', 'Bare-metal host ⚠ (sudo, no sandbox)');
    }
    typeSelect.value = [...typeSelect.options].some((option) => option.value === previous)
      ? previous : '';
    const host = typeSelect.value === 'host';
    $('am-host-warn').classList.toggle('hidden', !host);
    editor.querySelectorAll('.container-cap').forEach((field) =>
      field.classList.toggle('hidden', host));
  };
  workerSelect.onchange = refreshType;
  typeSelect.onchange = refreshType;
  refreshType();

  const providerSelect = $('am-provider');
  const modelSelect = $('am-model');
  const providers = [...new Set(models.map((model) => model.provider))].sort();
  providerSelect.replaceChildren();
  const currentModel = existing?.model || prefill?.model || '';
  if (currentModel && !models.some((model) => model.name === currentModel)) {
    models.push({ name: currentModel, provider: existing?.provider || 'Saved model' });
    if (!providers.includes(existing?.provider || 'Saved model')) {
      providers.push(existing?.provider || 'Saved model');
    }
  }
  for (const provider of providers) addOption(providerSelect, provider, provider);
  providerSelect.disabled = !providers.length;
  const refillModels = () => {
    modelSelect.replaceChildren();
    addOption(modelSelect, '', modelData.default_model
      ? `Hub default (${modelData.default_model})` : 'Hub default');
    for (const model of models.filter((item) => item.provider === providerSelect.value)) {
      addOption(modelSelect, model.name, model.name);
    }
  };
  providerSelect.onchange = refillModels;
  const currentEntry = models.find((model) => model.name === currentModel);
  if (currentEntry) providerSelect.value = currentEntry.provider;
  refillModels();
  if (currentModel) modelSelect.value = currentModel;

  const templateSelect = $('am-template');
  templateSelect.replaceChildren();
  addOption(templateSelect, '', 'Blank agent');
  for (const template of templates) {
    addOption(templateSelect, template.id, `${template.name} — template`);
  }
  const applyTemplate = (template) => {
    $('am-name').value = template?.name || '';
    $('am-role').value = template?.role || '';
    selectAvatar(template?.avatar || '');
  };
  applyTemplate(existing || prefill);
  $('am-memory').value = existing?.memory || '';
  $('am-cpus').value = existing?.cpus > 0 ? existing.cpus : '';
  $('am-thinking').value = existing?.thinking_level || '';
  templateSelect.onchange = () => {
    const template = templates.find((item) => item.id === templateSelect.value);
    if (template) applyTemplate(template);
  };

  $('am-form').onsubmit = async (event) => {
    event.preventDefault();
    setError();
    const name = $('am-name').value.trim();
    if (!name) return setError('Name is required.');
    if (!workerSelect.value) return setError('Select an online worker.');
    const cpusText = $('am-cpus').value.trim();
    const cpus = cpusText === '' ? null : Number(cpusText);
    if (cpus !== null && (!Number.isFinite(cpus) || cpus < 0)) {
      return setError('CPU cap must be a number of 0 or greater.');
    }
    const host = typeSelect.value === 'host';
    const wasHost = !!existing?.host_agent;

    const body = {
      name,
      role: $('am-role').value.trim(),
      model: modelSelect.value,
      workspace_mode: 'shared',
      avatar: selectedAvatar(),
    };
    if (!editing) body.worker = workerSelect.value;
    if (typeSelect.value === 'gpu') body.type = 'gpu';
    else if (existing?.type) body.type = 'default';
    const memory = $('am-memory').value.trim();
    if (host) {
      if (existing?.memory) body.memory = 'default';
      if (existing?.cpus > 0) body.cpus = 0;
    } else {
      if (memory) body.memory = memory;
      else if (existing?.memory) body.memory = 'default';
      if (cpus !== null) body.cpus = cpus;
      else if (existing?.cpus > 0) body.cpus = 0;
    }
    const thinking = $('am-thinking').value;
    if (thinking) body.thinking_level = thinking;
    else if (existing?.thinking_level) body.thinking_level = 'default';
    if (host) {
      body.host_agent = true;
    } else if (wasHost) {
      body.host_agent = false;
    }

    $('am-save').disabled = true;
    try {
      if (editing) await API.req('PATCH', `/api/agents/${existing.id}`, body);
      else await API.post('/api/agents', body);
      closeEditor();
      await callbacks.onSaved();
    } catch (error) {
      setError(`${editing ? 'Save' : 'Create'} failed — ${error.message}`);
      $('am-save').disabled = false;
    }
  };
  $('am-save').disabled = !eligible.length;
  $('am-name').focus();
}
