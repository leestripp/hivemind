// js/main.js — boot + page router. Keys is the default landing page.
// Phase 12: workflow canvas (nodes) and triggers pages removed with the
// graph engine; Phase 13 rebuilds this UI as an agent ops dashboard.
import { $ } from './util.js';
import { enterSettings, initSettings } from './pages/settings.js';
import { enterKeys, initKeys } from './pages/keys.js';

const PAGES = ['keys', 'settings'];   // sidebar order

function showPage(name) {
  if (!PAGES.includes(name)) name = 'keys';
  for (const pg of PAGES)
    $('page-' + pg).classList.toggle('hidden', pg !== name);
  document.querySelectorAll('.ab-item').forEach(b =>
    b.classList.toggle('active', b.dataset.page === name));
  if (location.hash !== '#/' + name) history.replaceState(null, '', '#/' + name);
  if (name === 'settings') enterSettings();
  if (name === 'keys')     enterKeys();
}

document.querySelectorAll('.ab-item').forEach(b =>
  b.addEventListener('click', () => showPage(b.dataset.page)));

initKeys();
initSettings();
showPage((location.hash || '').replace('#/', '') || 'keys');
