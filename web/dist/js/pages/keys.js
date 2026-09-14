// js/pages/keys.js — API key management (Phase 9, redesigned Phase 12).
// Table of keys with per-row revoke (bin icon); create form lives in a modal.
import { API } from '../api.js';
import { $, esc } from '../util.js';

export function enterKeys() { loadKeys(); }

const TRASH_SVG =
  '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" ' +
  'stroke="currentColor" stroke-width="1.8" stroke-linecap="round" ' +
  'stroke-linejoin="round"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg>';

function fmtDate(iso) {
  const d = new Date(iso);
  return isNaN(d) ? esc(iso)
    : d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

async function loadKeys() {
  const list = $('keys-list');
  try {
    const data = await API.get('/api/keys');
    const keys = data.keys || [];
    if (!keys.length) {
      list.innerHTML = '<tr><td colspan="6" class="muted">No keys — this should not happen; the master key is created at first server start.</td></tr>';
      return;
    }
    list.innerHTML = '';
    for (const k of keys) {
      const tr = document.createElement('tr');
      if (k.revoked) tr.className = 'row-revoked';
      tr.innerHTML = `
        <td class="cell-name">${esc(k.name)}</td>
        <td><code class="cell-id" title="${esc(k.id)}">${esc(k.id)}</code></td>
        <td><span class="badge badge-${esc(k.scopes)}">${esc(k.scopes)}</span></td>
        <td class="muted-cell">${fmtDate(k.created_at)}</td>
        <td>${k.revoked
              ? '<span class="badge badge-revoked">revoked</span>'
              : '<span class="badge badge-active">active</span>'}</td>
        <td class="col-actions"></td>`;
      const actions = tr.querySelector('.col-actions');
      const mkbtn = (label, title, danger, fn) => {
        const b = document.createElement('button');
        b.className = 'icon-btn' + (danger ? ' icon-danger' : '');
        b.title = title;
        b.textContent = label;
        b.onclick = fn;
        actions.appendChild(b);
      };
      if (k.name === 'master') {
        // no actions — the master key is untouchable
      } else if (!k.revoked) {
        mkbtn('Revoke', `Revoke "${k.name}" — clients stop working immediately (reversible)`, true,
          async () => {
            if (!confirm(`Revoke key "${k.name}" (${k.id})?\nClients using it stop working immediately.\nYou can reactivate it later.`)) return;
            try {
              await API.post('/api/keys/' + encodeURIComponent(k.id) + '/revoke');
              loadKeys();
            } catch (e) { alert('Revoke failed: ' + e.message); }
          });
      } else {
        // Revoked keys keep their actions: undo, or remove for good.
        mkbtn('Reactivate', `Reactivate "${k.name}" — it works again immediately`, false,
          async () => {
            try {
              await API.post('/api/keys/' + encodeURIComponent(k.id) + '/reactivate');
              loadKeys();
            } catch (e) { alert('Reactivate failed: ' + e.message); }
          });
        mkbtn('Delete', `Delete "${k.name}" permanently — this cannot be undone`, true,
          async () => {
            if (!confirm(`DELETE key "${k.name}" (${k.id}) permanently?\nThis cannot be undone.`)) return;
            try {
              await API.del('/api/keys/' + encodeURIComponent(k.id));
              loadKeys();
            } catch (e) { alert('Delete failed: ' + e.message); }
          });
      }
      list.appendChild(tr);
    }
  } catch (e) {
    list.innerHTML = `<tr><td colspan="6" class="muted">unavailable — ${esc(e.message)}</td></tr>`;
  }
}

// ── modal ─────────────────────────────────────────────────────────────
function openModal() {
  $('keys-modal-title').textContent = 'New API key';
  $('keys-form').classList.remove('hidden');
  $('keys-reveal').classList.add('hidden');
  $('keys-err').textContent = '';
  $('keys-name').value = '';
  $('keys-modal').classList.remove('hidden');
  $('keys-name').focus();
}

function closeModal() {
  $('keys-modal').classList.add('hidden');
  // If the reveal state was up, closing means the user is done — refresh.
  if (!$('keys-reveal').classList.contains('hidden')) loadKeys();
}

export function initKeys() {
  $('keys-open-add').addEventListener('click', openModal);
  $('keys-cancel').addEventListener('click', closeModal);
  $('keys-modal-close').addEventListener('click', closeModal);
  $('keys-done').addEventListener('click', closeModal);
  // Click on the dimmed overlay (not the panel) closes.
  $('keys-modal').addEventListener('mousedown', (ev) => {
    if (ev.target === $('keys-modal')) closeModal();
  });
  document.addEventListener('keydown', (ev) => {
    if (ev.key === 'Escape' && !$('keys-modal').classList.contains('hidden'))
      closeModal();
  });

  $('keys-add').addEventListener('click', async () => {
    const name = $('keys-name').value.trim();
    const scopes = $('keys-scopes').value;
    const err = $('keys-err');
    err.textContent = '';
    if (!name) { err.textContent = 'Name is required.'; return; }
    try {
      const data = await API.post('/api/keys', { name, scopes });
      // Switch the modal to the one-time reveal state.
      $('keys-form').classList.add('hidden');
      $('keys-reveal').classList.remove('hidden');
      $('keys-modal-title').textContent = `Key created — "${name}"`;
      $('keys-new-value').textContent = data.key;
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
}
