// js/pages/secrets.js — Phase 15 secret broker management.
// Values are WRITE-ONLY: the API never returns them. Granted secrets reach
// agents as HIVE_SECRET_<NAME> env vars at container-exec time (and, later,
// via the browser fill_secret tool) — never through the LLM context.
import { API } from '../api.js';
import { $, esc } from '../util.js';

let secrets = [];
let agents = [];

// ── data ──────────────────────────────────────────────────────────────
async function loadAll() {
  const [s, a] = await Promise.all([
    API.get('/api/secrets'),
    API.get('/api/agents'),
  ]);
  secrets = s.secrets || [];
  agents = a.agents || [];
  renderSecrets();
}

// ── table ─────────────────────────────────────────────────────────────
function renderSecrets() {
  const tbody = $('secrets-tbody');
  if (!secrets.length) {
    tbody.innerHTML = '<tr><td colspan="4" class="muted">No secrets yet. Click “+ Add Secret” — values are stored encrypted and are never readable back.</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  for (const s of [...secrets].sort((a, b) => a.name.localeCompare(b.name))) {
    const tr = document.createElement('tr');
    const grantedIds = new Set((s.agents || []).map(a => a.id));
    const chips = agents.map(a => {
      const on = grantedIds.has(a.id);
      return `<button class="skill-chip${on ? ' skill-chip-on' : ''}"
                data-act="toggle" data-secret="${esc(s.name)}" data-agent="${esc(a.id)}"
                title="${on ? 'Revoke from' : 'Grant to'} ${esc(a.name)}">${esc(a.name)}</button>`;
    }).join(' ');
    tr.innerHTML = `
      <td><strong>${esc(s.name)}</strong>${s.source ? ' <span class="badge" title="External vault reference — value fetched at use, never stored">↗ vault</span>' : ''}<br><span class="muted" style="font-size:11px">${s.source ? esc(s.source) : 'HIVE_SECRET_' + esc(s.name.toUpperCase())}</span></td>
      <td class="muted">${esc(s.created_at || '—')}</td>
      <td>${chips || '<span class="muted">no agents</span>'}</td>
      <td class="row-actions">
        <button class="icon-btn icon-danger" data-act="del" data-secret="${esc(s.name)}"
                title="Delete secret (revokes from all agents)">🗑</button>
      </td>`;
    tbody.appendChild(tr);
  }
}

// ── modals ────────────────────────────────────────────────────────────
function openModal(id)  { $(id).classList.remove('hidden'); }
function closeModal(id) { $(id).classList.add('hidden'); }

// ── events ────────────────────────────────────────────────────────────
export function initSecrets() {
  $('secrets-add').addEventListener('click', () => {
    $('secret-form').reset();
    $('secret-kind').dispatchEvent(new Event('change'));
    openModal('secret-modal');
  });

  // Phase 22: stored value XOR external vault reference.
  $('secret-kind').addEventListener('change', () => {
    const ext = $('secret-kind').value === 'external';
    $('secret-value-row').classList.toggle('hidden', ext);
    $('secret-source-row').classList.toggle('hidden', !ext);
    $('secret-form').value.required = !ext;
    $('secret-form').source.required = ext;
  });

  $('secret-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    try {
      const body = { name: f.name.value.trim() };
      if ($('secret-kind').value === 'external') body.source = f.source.value.trim();
      else body.value = f.value.value;
      await API.post('/api/secrets', body);
      closeModal('secret-modal');
      await loadAll();
    } catch (err) { alert(err.message); }
  });

  document.querySelectorAll('#secret-modal [data-close]').forEach(b =>
    b.addEventListener('click', () => closeModal('secret-modal')));

  $('secrets-tbody').addEventListener('click', async (e) => {
    const b = e.target.closest('button[data-act]');
    if (!b) return;
    try {
      if (b.dataset.act === 'del') {
        if (!confirm(`Delete secret "${b.dataset.secret}"?\n\nIt is revoked from every agent and cannot be recovered.`)) return;
        await API.del('/api/secrets/' + encodeURIComponent(b.dataset.secret));
      } else if (b.dataset.act === 'toggle') {
        const on = b.classList.contains('skill-chip-on');
        await API.post('/api/agents/' + b.dataset.agent + '/secrets',
          { name: b.dataset.secret, granted: !on });
      }
      await loadAll();
    } catch (err) { alert(err.message); }
  });
}

export function enterSecrets() { loadAll().catch(e => alert(e.message)); }
