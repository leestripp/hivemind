// js/pages/keys.js — API key management (Phase 9).
// List keys, mint scoped keys (raw key shown once), revoke by public id.
import { API } from '../api.js';
import { $, esc, log } from '../util.js';

export function enterKeys() { loadKeys(); }

async function loadKeys() {
  const list = $('keys-list');
  list.innerHTML = '<p class="muted">loading…</p>';
  try {
    const data = await API.get('/api/keys');
    const keys = data.keys || [];
    if (!keys.length) {
      list.innerHTML = '<p class="muted">No keys (this should not happen — the master key is created at first server start).</p>';
      return;
    }
    list.innerHTML = '';
    for (const k of keys) {
      const row = document.createElement('div');
      row.className = 'trig-row';
      row.innerHTML = `
        <div class="trig-main">
          <div class="trig-name">${esc(k.name)} ${k.revoked ? '<span class="muted">(revoked)</span>' : ''}</div>
          <div class="trig-sub"><code>${esc(k.id)}</code> · scopes: ${esc(k.scopes)} · created ${esc(k.created_at)}</div>
        </div>
        <div class="trig-actions"></div>`;
      if (!k.revoked && k.name !== 'master') {
        const btn = document.createElement('button');
        btn.className = 'btn ctx-danger';
        btn.textContent = 'Revoke';
        btn.onclick = async () => {
          if (!confirm(`Revoke key "${k.name}" (${k.id})? Clients using it stop working immediately.`)) return;
          try {
            await API.del('/api/keys/' + encodeURIComponent(k.id));
            log(`revoked key ${k.id}`);
            loadKeys();
          } catch (e) { log('revoke failed: ' + e.message); }
        };
        row.querySelector('.trig-actions').appendChild(btn);
      }
      list.appendChild(row);
    }
  } catch (e) {
    list.innerHTML = `<p class="muted">unavailable — ${esc(e.message)}</p>`;
  }
}

export function initKeys() {
  $('keys-add').addEventListener('click', async () => {
    const name = $('keys-name').value.trim();
    const scopes = $('keys-scopes').value;
    const err = $('keys-err');
    err.textContent = '';
    if (!name) { err.textContent = 'Name is required.'; return; }
    try {
      const data = await API.post('/api/keys', { name, scopes });
      $('keys-name').value = '';
      // The raw key is shown exactly once — make it impossible to miss.
      const box = $('keys-new');
      box.classList.remove('hidden');
      $('keys-new-value').textContent = data.key;
      log(`created key "${name}" (${scopes})`);
      loadKeys();
    } catch (e) {
      err.textContent = e.message;
    }
  });

  $('keys-copy').addEventListener('click', async () => {
    const v = $('keys-new-value').textContent;
    try {
      await navigator.clipboard.writeText(v);
      $('keys-copy').textContent = 'Copied!';
      setTimeout(() => { $('keys-copy').textContent = 'Copy'; }, 1500);
    } catch {
      // Clipboard API needs a secure context on some browsers — select instead.
      const r = document.createRange();
      r.selectNodeContents($('keys-new-value'));
      const s = getSelection();
      s.removeAllRanges(); s.addRange(r);
    }
  });

  $('keys-dismiss').addEventListener('click', () =>
    $('keys-new').classList.add('hidden'));
}
