import { API } from '../api.js';
import { $, esc, log } from '../util.js';
import { S } from '../state.js';


export function enterChat() { loadChatModels(); refreshChat(); }

// ─── Chat page ──────────────────────────────────────────────────────────
let chatPolling = false;
const CHAT_MODEL_KEY = 'hivemind.chatModel';

async function loadChatModels() {
  try {
    const models = await API.get('/api/models');
    const sel = $('chat-model');
    sel.innerHTML = '';
    // Group by provider, same pattern as the Agent node inspector.
    const groups = {};
    for (const m of models) (groups[m.provider || 'other'] ??= []).push(m);
    for (const [prov, ms] of Object.entries(groups)) {
      const og = document.createElement('optgroup');
      og.label = prov;
      for (const m of ms) {
        const o = document.createElement('option');
        o.value = m.name;
        o.textContent = m.name + (m.is_default ? ' (default)' : '');
        og.appendChild(o);
      }
      sel.appendChild(og);
    }
    // Restore saved choice, else the server-side current model wins via refreshChat().
    const saved = localStorage.getItem(CHAT_MODEL_KEY);
    if (saved && [...sel.options].some(o => o.value === saved)) sel.value = saved;
  } catch (e) { console.error('loadChatModels failed:', e); }
}

$('chat-model').addEventListener('change', () => {
  localStorage.setItem(CHAT_MODEL_KEY, $('chat-model').value);
  log('chat model → ' + $('chat-model').value + ' (applies to the next message)');
});

function renderChatMessages(messages, pending) {
  const log = $('chat-log');
  log.innerHTML = '';
  for (const m of messages) {
    const div = document.createElement('div');
    div.className = 'chat-msg ' + m.role;
    div.textContent = m.content;
    if (m.tool_log) {
      const tl = document.createElement('div');
      tl.className = 'tool-log';
      tl.textContent = m.tool_log;
      div.appendChild(tl);
    }
    log.appendChild(div);
  }
  if (pending) {
    const div = document.createElement('div');
    div.className = 'chat-msg assistant pending';
    div.textContent = 'thinking…';
    log.appendChild(div);
  }
  log.scrollTop = log.scrollHeight;
}

async function refreshChat() {
  try {
    const d = await API.get('/api/chat/messages');
    renderChatMessages(d.messages || [], d.pending);
    // If the user hasn't picked a model yet, reflect the server's current one.
    const sel = $('chat-model');
    if (d.model && sel.options.length && !localStorage.getItem(CHAT_MODEL_KEY))
      sel.value = d.model;
    if (d.pending && !chatPolling) {
      chatPolling = true;
      const tick = setInterval(async () => {
        try {
          const d2 = await API.get('/api/chat/messages');
          renderChatMessages(d2.messages || [], d2.pending);
          if (!d2.pending) { clearInterval(tick); chatPolling = false; }
        } catch { clearInterval(tick); chatPolling = false; }
      }, 1500);
    }
  } catch (e) { /* server down — leave */ }
}

async function sendChat() {
  const ta = $('chat-text');
  const msg = ta.value.trim();
  if (!msg) return;
  ta.value = '';
  try {
    const r = await API.post('/api/chat/message',
      { message: msg, model: $('chat-model').value || undefined });
    if (r.error) { log(r.error); }
    refreshChat();
  } catch (e) { log(`chat failed: ${e.message || e}`); }
}

$('chat-send').addEventListener('click', sendChat);
$('chat-text').addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendChat(); }
});
$('chat-clear').addEventListener('click', async () => {
  try { await API.del('/api/chat/messages'); refreshChat(); } catch {}
});

