// js/main.js — boot + page router. Agents is the default landing page.
import { $ } from './util.js';
import { enterAgents, leaveAgents, initAgents } from './pages/agents.js?v=20261027a';
import { enterSettings, initSettings } from './pages/settings.js';
import { enterKeys, initKeys } from './pages/keys.js';
import { enterModels, initModels } from './pages/models.js';
import { enterTasks, initTasks } from './pages/tasks.js';
import { enterSkills, initSkills } from './pages/skills.js';
import { enterSecrets, initSecrets } from './pages/secrets.js';
import { enterFiles, initFiles } from './pages/files.js';
import { enterWorkers, leaveWorkers, initWorkers } from './pages/workers.js';
import { enterSystem, leaveSystem, initSystem } from './pages/system.js';
import { getTheme } from './util.js';

document.documentElement.dataset.theme = getTheme();   // Phase 19

const PAGES = ['agents', 'tasks', 'files', 'skills', 'secrets', 'models', 'workers', 'system', 'keys', 'settings'];   // sidebar order

function showPage(name) {
  if (!PAGES.includes(name)) name = 'agents';
  for (const pg of PAGES)
    $('page-' + pg).classList.toggle('hidden', pg !== name);
  document.querySelectorAll('.ab-item').forEach(b =>
    b.classList.toggle('active', b.dataset.page === name));
  if (location.hash !== '#/' + name) history.replaceState(null, '', '#/' + name);
  if (name === 'agents')   enterAgents(); else leaveAgents();
  if (name === 'models')   enterModels();
  if (name === 'tasks')    enterTasks();
  if (name === 'skills')   enterSkills();
  if (name === 'files')    enterFiles();
  if (name === 'secrets')  enterSecrets();
  if (name === 'settings') enterSettings();
  if (name === 'keys')     enterKeys();
  if (name === 'workers')  enterWorkers(); else leaveWorkers();
  if (name === 'system')   enterSystem(); else leaveSystem();
}

document.querySelectorAll('.ab-item').forEach(b =>
  b.addEventListener('click', () => showPage(b.dataset.page)));

initAgents();
initKeys();
initWorkers();
initSystem();
initModels();
initTasks();
initSkills();
initFiles();
initSecrets();
initSettings();
showPage((location.hash || '').replace('#/', '') || 'agents');

// ── Responsive tables: copy each column's <th> text onto its <td> as
// data-label, so the ≤760px card layout can show "Label: value" rows.
// A MutationObserver covers every table forever — no per-renderer edits.
function labelTable(t) {
  const heads = [...t.querySelectorAll('thead th')].map(th => th.textContent.trim());
  if (!heads.length) return;
  t.querySelectorAll('tbody tr').forEach(tr =>
    [...tr.children].forEach((td, i) => {
      if (heads[i]) td.setAttribute('data-label', heads[i]);
    }));
}
const labelAll = () =>
  document.querySelectorAll('table.kv-table').forEach(labelTable);
new MutationObserver(labelAll)
  .observe(document.body, { childList: true, subtree: true });
labelAll();
