// js/pages/secrets.js — Phase 15 secret broker management, typed secrets.
// Types: api_key (HIVE_SECRET_<NAME>), login (_USER/_PASS/_URL env vars),
// note (multiline text, newlines escaped in env). Values are WRITE-ONLY:
// the API never returns them. Granted secrets reach agents as env vars at
// container-exec time — never through the LLM context.
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

// ── presentation helpers ──────────────────────────────────────────────
const TYPE_BADGE = {
  api_key: '🔑 api key',
  login:   '👤 login',
  note:    '📝 note',
};

function deliveryHint(s) {
  const env = 'HIVE_SECRET_' + s.name.toUpperCase();
  if (s.source) return esc(s.source);
  if (s.type === 'login') {
    const bits = [env + '_USER', env + '_PASS'];
    if (s.url) bits.push(env + '_URL');
    return esc(bits.join(' · '));
  }
  return esc(env);
}

function metaLine(s) {
  if (s.type === 'login') {
    const parts = [];
    if (s.username) parts.push('user: ' + s.username);
    if (s.url) parts.push(s.url);
    return parts.length ? `<br><span class="muted" style="font-size:11px">${esc(parts.join(' — '))}</span>` : '';
  }
  if (s.type === 'note')
    return '<br><span class="muted" style="font-size:11px">multiline note (newlines escaped in env)</span>';
  return '';
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
    const badge = s.source
      ? ' <span class="badge" title="External vault reference — value fetched at use, never stored">↗ vault</span>'
      : ` <span class="badge">${TYPE_BADGE[s.type] || '🔑 api key'}</span>`;
    tr.innerHTML = `
      <td><strong>${esc(s.name)}</strong>${badge}${metaLine(s)}<br><span class="muted" style="font-size:11px">${deliveryHint(s)}</span></td>
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

function syncSecretFormRows() {
  const ext = $('secret-kind').value === 'external';
  const type = $('secret-type').value;
  $('secret-type-row').classList.toggle('hidden', ext);
  $('secret-value-row').classList.toggle('hidden', ext || type !== 'api_key');
  $('secret-username-row').classList.toggle('hidden', ext || type !== 'login');
  $('secret-password-row').classList.toggle('hidden', ext || type !== 'login');
  $('secret-url-row').classList.toggle('hidden', ext || type !== 'login');
  $('secret-text-row').classList.toggle('hidden', ext || type !== 'note');
  $('secret-source-row').classList.toggle('hidden', !ext);
  const f = $('secret-form');
  f.value.required = !ext && type === 'api_key';
  f.password.required = !ext && type === 'login';
  f.text.required = !ext && type === 'note';
  f.source.required = ext;
}

// ── events ────────────────────────────────────────────────────────────
export function initSecrets() {
  $('secrets-add').addEventListener('click', () => {
    $('secret-form').reset();
    syncSecretFormRows();
    openModal('secret-modal');
  });

  $('secret-kind').addEventListener('change', syncSecretFormRows);
  $('secret-type').addEventListener('change', syncSecretFormRows);

  $('secret-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    try {
      const body = { name: f.name.value.trim() };
      if ($('secret-kind').value === 'external') {
        body.source = f.source.value.trim();
      } else {
        body.type = $('secret-type').value;
        if (body.type === 'login') {
          body.username = f.username.value.trim();
          body.password = f.password.value;
          body.url = f.url.value.trim();
        } else if (body.type === 'note') {
          body.text = f.text.value;
        } else {
          body.value = f.value.value;
        }
      }
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
