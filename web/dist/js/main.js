// js/main.js — boot + page router. Chat is the default landing page.
import { $ } from './util.js';
import { initNodes } from './pages/nodes.js';
import { enterChat } from './pages/chat.js';
import { enterTriggers } from './pages/triggers.js';
import { enterSettings } from './pages/settings.js';

const PAGES = ['chat', 'nodes', 'triggers', 'settings'];   // sidebar order

function showPage(name) {
  if (!PAGES.includes(name)) name = 'chat';
  for (const pg of PAGES)
    $('page-' + pg).classList.toggle('hidden', pg !== name);
  document.querySelectorAll('.ab-item').forEach(b =>
    b.classList.toggle('active', b.dataset.page === name));
  if (location.hash !== '#/' + name) history.replaceState(null, '', '#/' + name);
  if (name === 'chat')     enterChat();
  if (name === 'nodes')    { /* canvas already live; nothing to refetch */ }
  if (name === 'triggers') enterTriggers();
  if (name === 'settings') enterSettings();
}

document.querySelectorAll('.ab-item').forEach(b =>
  b.addEventListener('click', () => showPage(b.dataset.page)));

// Boot: node editor state first (canvas must render even when hidden),
// then land on the page named in the hash — default: Agent Chat.
initNodes();
showPage((location.hash || '').replace('#/', '') || 'chat');
