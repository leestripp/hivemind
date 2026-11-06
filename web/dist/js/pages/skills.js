// js/pages/skills.js — curated starter-skill installation.
// Installed starters are copied into every worker's writable workspace-global
// skill directory and are available to every agent there. The hub deliberately
// does not inventory agents' private or self-created workspace skills.
import { API } from '../api.js';
import { $, esc } from '../util.js';

export function initSkills() {}

async function loadRegistry() {
  const host = $('skills-registry');
  host.innerHTML = '<p class="muted">loading…</p>';
  try {
    const reg = await API.get('/api/skills/registry');
    const entries = reg.entries || [];
    if (!entries.length) {
      host.innerHTML = '<p class="muted">No starter skills are available.</p>';
      return;
    }
    host.innerHTML = '';
    for (const entry of entries) {
      const card = document.createElement('div');
      card.className = 'agent-card';
      const tags = (entry.tags || [])
        .map(tag => `<span class="badge">${esc(tag)}</span>`).join(' ');
      card.innerHTML = `
        <div class="agent-top">
          <div class="agent-id-block">
            <div class="agent-name">${esc(entry.name)}</div>
            <div class="agent-role">${esc(entry.description || '')}</div>
          </div>
        </div>
        <div class="agent-meta"><div><span>Source</span>${esc(entry.source || entry.git_url)}</div>
        <div><span>Tags</span>${tags || '—'}</div></div>
        <div class="agent-actions">
          <button class="btn btn-sm ${entry.installed ? '' : 'suggested-action'}"
                  data-install="${esc(entry.name)}" ${entry.installed ? 'disabled' : ''}>
            ${entry.installed ? '✓ Installed for all agents' : '⬇ Install for all agents'}
          </button>
          ${entry.installed ? `<button class="btn btn-sm" data-uninstall="${esc(entry.name)}">Uninstall</button>` : ''}
        </div>`;
      host.appendChild(card);
    }
  } catch (error) {
    host.innerHTML = `<p class="muted">Starter skills failed to load: ${esc(error.message)}</p>`;
  }
}

export function enterSkills() {
  const host = $('skills-registry');
  host.onclick = async event => {
    const install = event.target.closest('[data-install]');
    const uninstall = event.target.closest('[data-uninstall]');
    const button = install || uninstall;
    if (!button || button.disabled) return;
    const name = install ? install.dataset.install : uninstall.dataset.uninstall;
    if (uninstall && !confirm(
        `Uninstall starter skill "${name}" for every agent?\n\n` +
        'This removes its shared workspace copies. Private skills are not affected. ' +
        'All agent workers must be online.')) return;
    button.disabled = true;
    button.textContent = install ? 'Installing…' : 'Uninstalling…';
    try {
      if (install) {
        const result = await API.post('/api/skills/install', { name });
        alert(`Installed "${result.installed}" for every agent. ` +
              'Workers that were offline will receive it automatically on reconnect.');
      } else {
        await API.del('/api/skills/' + encodeURIComponent(name));
        alert(`Uninstalled "${name}" for every agent.`);
      }
    } catch (error) {
      // A connection hiccup can hide a completed install; reload the actual
      // installed state instead of assuming the operation failed.
      alert(error.message);
    }
    await loadRegistry();
  };
  loadRegistry();
}
