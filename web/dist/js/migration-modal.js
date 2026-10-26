// State-preserving agent migration dialog and live job progress.
import { API } from './api.js';
import { $ } from './util.js';

let modal = null;
let generation = 0;
let watching = false;

const TERMINAL_STATES = new Set(['done', 'failed', 'cancelled', 'rolled_back']);
const STATE_STEPS = {
  pending: 0, exporting: 1, transferring: 2, provisioning: 3,
  importing: 3, verifying: 4, cleanup: 5, done: 6,
};

function ensureModal() {
  if (modal) return modal;
  modal = document.createElement('div');
  modal.id = 'migration-modal';
  modal.className = 'modal-overlay hidden';
  modal.innerHTML = `
    <section class="modal-panel migration-panel" role="dialog" aria-modal="true" aria-labelledby="migration-title">
      <header class="modal-head">
        <div><h2 id="migration-title">Migrate agent</h2><p class="muted" id="migration-subtitle"></p></div>
        <button type="button" class="icon-btn" id="migration-close" aria-label="Close">✕</button>
      </header>
      <form id="migration-form">
        <div class="migration-route">
          <div><span class="field-label">From</span><strong id="migration-source"></strong></div>
          <span aria-hidden="true">→</span>
          <div><span class="field-label">To</span><strong id="migration-destination">Select a target</strong></div>
        </div>
        <div class="agent-form-grid">
          <div class="form-field">
            <label for="migration-worker">Target worker</label>
            <select id="migration-worker" required></select>
          </div>
          <div class="form-field">
            <label for="migration-type">Target runtime</label>
            <select id="migration-type">
              <option value="container">Container (sandboxed)</option>
              <option value="host">Bare-metal host ⚠</option>
            </select>
          </div>
        </div>
        <div id="migration-host-warn" class="danger-callout hidden" role="warning">
          <strong>⚠ Bare-metal target</strong>
          <p>The agent will run directly on the target worker with passwordless sudo — no container and no sandbox. Its state and history move with it.</p>
          <label for="migration-ack">Type <strong>I UNDERSTAND</strong> to continue</label>
          <input id="migration-ack" autocomplete="off" placeholder="I UNDERSTAND">
        </div>
        <fieldset class="migration-options">
          <legend>Recovery options</legend>
          <label class="check-row" for="migration-force">
            <input type="checkbox" id="migration-force">
            <span>Force migration even if the agent is mid-turn</span>
          </label>
          <label class="check-row" for="migration-no-rollback">
            <input type="checkbox" id="migration-no-rollback">
            <span>Disable automatic rollback if verification fails (advanced)</span>
          </label>
        </fieldset>
        <p class="form-error hidden" id="migration-error" role="alert"></p>
        <footer class="modal-actions">
          <button type="button" class="btn" id="migration-cancel-close">Close</button>
          <button type="submit" class="btn btn-primary" id="migration-start">Start migration</button>
        </footer>
      </form>
      <div id="migration-progress" class="hidden" aria-live="polite">
        <h3 id="migration-state">Preparing migration…</h3>
        <progress id="migration-progress-bar" max="6" value="0"></progress>
        <p id="migration-detail" class="muted"></p>
        <p id="migration-job" class="migration-job"></p>
        <p id="migration-failure" class="form-error hidden" role="alert"></p>
        <footer class="modal-actions">
          <button type="button" class="btn" id="migration-progress-close">Close</button>
          <button type="button" class="btn danger" id="migration-cancel-job">Cancel migration</button>
        </footer>
      </div>
    </section>`;
  document.body.appendChild(modal);
  for (const id of ['migration-close', 'migration-cancel-close', 'migration-progress-close']) {
    $(id).addEventListener('click', closeMigration);
  }
  return modal;
}

function closeMigration() {
  generation++;
  watching = false;
  modal?.classList.add('hidden');
}

function addOption(select, value, label) {
  const option = document.createElement('option');
  option.value = value;
  option.textContent = label;
  select.appendChild(option);
}

function migrationError(message = '') {
  $('migration-error').textContent = message;
  $('migration-error').classList.toggle('hidden', !message);
}

function isLive(job) {
  return job && !TERMINAL_STATES.has(job.state);
}

function showProgress(job) {
  $('migration-form').classList.add('hidden');
  $('migration-progress').classList.remove('hidden');
  renderProgress(job);
}

function renderProgress(job) {
  const stateLabel = job.state.replaceAll('_', ' ');
  $('migration-state').textContent = job.state === 'done'
    ? 'Migration complete' : stateLabel.charAt(0).toUpperCase() + stateLabel.slice(1);
  $('migration-progress-bar').value = STATE_STEPS[job.state] ?? 0;
  $('migration-detail').textContent = job.error || job.progress ||
    (job.state === 'done' ? `${job.agent_name} is now running on ${job.target_worker}.`
      : `${job.source_worker} → ${job.target_worker} (${job.target_type})`);
  $('migration-job').textContent = `Job ${job.id}`;
  $('migration-failure').textContent = job.error || '';
  $('migration-failure').classList.toggle('hidden', !job.error);
  $('migration-cancel-job').classList.toggle('hidden', !isLive(job));
}

export function openMigrationModal(agent, { onComplete = () => {} } = {}) {
  ensureModal();
  const ticket = ++generation;
  watching = false;
  modal.classList.remove('hidden');
  $('migration-title').textContent = `Migrate ${agent.name}`;
  $('migration-subtitle').textContent =
    'Move the runtime, memory, skills, and conversation state to another online worker.';
  $('migration-source').textContent =
    `${agent.worker} · ${agent.host_agent ? 'bare-metal host' : agent.type || 'container'}`;
  $('migration-form').classList.remove('hidden');
  $('migration-progress').classList.add('hidden');
  $('migration-start').disabled = true;
  migrationError();
  prepareMigration(agent, { onComplete }, ticket).catch((error) => {
    if (ticket === generation) migrationError(error.message);
  });
}

async function prepareMigration(agent, { onComplete }, ticket) {
  const [workersResult, jobsResult] = await Promise.allSettled([
    API.get('/api/workers'),
    API.get(`/api/migrations?agent=${encodeURIComponent(agent.id)}`),
  ]);
  if (ticket !== generation) return;
  const existingJob = jobsResult.status === 'fulfilled'
    ? (jobsResult.value.migrations || []).find(isLive) : null;
  if (existingJob) {
    showProgress(existingJob);
    await watchMigration(existingJob.id, { onComplete }, ticket);
    return;
  }
  if (workersResult.status === 'rejected') {
    migrationError(`Workers unavailable — ${workersResult.reason.message}`);
    return;
  }
  const workers = (workersResult.value.workers || []).filter((worker) =>
    worker.online && worker.name !== 'local' && worker.name !== agent.worker);
  const workerSelect = $('migration-worker');
  workerSelect.replaceChildren();
  for (const worker of workers) addOption(workerSelect, worker.name, worker.name);
  if (agent.worker === 'local') {
    migrationError('Hub-side local agents are not migratable yet.');
  } else if (!workers.length) {
    migrationError('No other online workers are available as a migration target.');
  }
  const refreshTarget = () => {
    const target = workers.find((worker) => worker.name === workerSelect.value);
    const hostCap = !!target?.caps && typeof target.caps === 'object' &&
      'host_agents' in target.caps;
    const hostOption = $('migration-type').querySelector('option[value="host"]');
    hostOption.disabled = !hostCap;
    if (!hostCap && $('migration-type').value === 'host') {
      $('migration-type').value = 'container';
    }
    const host = $('migration-type').value === 'host';
    $('migration-host-warn').classList.toggle('hidden', !host);
    $('migration-destination').textContent = target
      ? `${target.name} · ${host ? 'bare-metal host' : 'container'}`
      : 'Select a target';
  };
  workerSelect.onchange = refreshTarget;
  $('migration-type').onchange = refreshTarget;
  $('migration-force').checked = false;
  $('migration-no-rollback').checked = false;
  $('migration-ack').value = '';
  refreshTarget();

  $('migration-form').onsubmit = async (event) => {
    event.preventDefault();
    migrationError();
    const targetWorker = workerSelect.value;
    const targetType = $('migration-type').value;
    const ack = $('migration-ack').value.trim();
    if (!targetWorker) return migrationError('Select a target worker.');
    if (targetType === 'host' && ack !== 'I UNDERSTAND') {
      return migrationError('Type I UNDERSTAND in the bare-metal warning to continue.');
    }
    $('migration-start').disabled = true;
    try {
      const response = await API.post(`/api/agents/${agent.id}/migrate`, {
        target_worker: targetWorker,
        target_type: targetType,
        force: $('migration-force').checked,
        no_rollback: $('migration-no-rollback').checked,
        ack,
      });
      await watchMigration(response.job_id, { onComplete }, ticket);
    } catch (error) {
      if (ticket !== generation) return;
      $('migration-form').classList.remove('hidden');
      $('migration-progress').classList.add('hidden');
      migrationError(`Migration failed — ${error.message}`);
      $('migration-start').disabled = false;
    }
  };
  $('migration-start').disabled = !workers.length || agent.worker === 'local';
}

async function watchMigration(jobId, { onComplete }, ticket) {
  if (watching) return;
  watching = true;
  $('migration-cancel-job').onclick = async () => {
    $('migration-cancel-job').disabled = true;
    try {
      await API.post(`/api/migrations/${jobId}/cancel`, {});
      $('migration-detail').textContent = 'Cancellation requested…';
    } catch (error) {
      $('migration-failure').textContent = error.message;
      $('migration-failure').classList.remove('hidden');
    } finally {
      $('migration-cancel-job').disabled = false;
    }
  };
  let completed = false;
  try {
    while (ticket === generation && watching) {
      try {
        const job = await API.get(`/api/migrations/${jobId}`);
        if (ticket !== generation || !watching) return;
        showProgress(job);
        if (TERMINAL_STATES.has(job.state)) {
          completed = job.state === 'done';
          break;
        }
      } catch (error) {
        $('migration-detail').textContent =
          `Status check failed (${error.message}); retrying…`;
      }
      await new Promise((resolve) => setTimeout(resolve, 2000));
    }
  } finally {
    if (completed && ticket === generation) await onComplete();
  }
}
