// js/main.js — boot + page router. Agents is the default landing page.
import { $ } from './util.js';
import { enterAgents, leaveAgents, initAgents } from './pages/agents.js';
import { enterSettings, initSettings } from './pages/settings.js';
import { enterKeys, initKeys } from './pages/keys.js';

const PAGES = ['agents', 'keys', 'settings'];   // sidebar order

function showPage(name) {
  if (!PAGES.includes(name)) name = 'agents';
  for (const pg of PAGES)
    $('page-' + pg).classList.toggle('hidden', pg !== name);
  document.querySelectorAll('.ab-item').forEach(b =>
    b.classList.toggle('active', b.dataset.page === name));
  if (location.hash !== '#/' + name) history.replaceState(null, '', '#/' + name);
  if (name === 'agents')   enterAgents(); else leaveAgents();
  if (name === 'settings') enterSettings();
  if (name === 'keys')     enterKeys();
}

document.querySelectorAll('.ab-item').forEach(b =>
  b.addEventListener('click', () => showPage(b.dataset.page)));

initAgents();
initKeys();
initSettings();
showPage((location.hash || '').replace('#/', '') || 'agents');
