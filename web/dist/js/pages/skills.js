// js/pages/skills.js — skills management (two tiers, §Skills-Global).
// Tier 1 "Synced Library": SkillRoutes manages ~/.local/share/hivemind/skills/
// (synced to workers, mounted read-only at /skills) + per-agent enable.
// Tier 2 "Workspace Global Skills": per-worker /workspace/.agents/skills/,
// agent-writable and lazy-loaded; fetched live via GET /api/skills/workspace
// (hub relays to each worker, persists nothing) — read-only view here.
import { API } from '../api.js';
import { $, esc } from '../util.js';

let skills = [];
let agents = [];

// ── data ──────────────────────────────────────────────────────────────
async function loadAll() {
  const [s, a] = await Promise.all([
    API.get('/api/skills'),
    API.get('/api/agents'),
  ]);
  skills = s.skills || [];
  agents = a.agents || [];
  renderSkills();
}

// ── workspace global skills (per worker, live relay) ──────────────────
async function loadWorkspaceSkills() {
  const host = $('skills-workspace');
  host.innerHTML = '<p class="muted">loading…</p>';
  try {
    const d = await API.get('/api/skills/workspace');
    const workers = d.workers || [];
    if (!workers.length) {
      host.innerHTML = '<p class="muted">No workers with agents yet.</p>';
      return;
    }
    host.innerHTML = '';
    for (const w of workers) {
      const card = document.createElement('div');
      card.className = 'agent-card';
      const list = (w.skills || []);
      const rows = list.length
        ? list.map(sk =>
            `<div style="margin:4px 0"><strong>${esc(sk.name)}</strong>` +
            (sk.version ? ` <span class="badge">v${esc(sk.version)}</span>` : '') +
            `<div class="muted">${esc(sk.description || '—')}</div></div>`).join('')
        : '<p class="muted">none yet — agents create them under /workspace/.agents/skills/</p>';
      const status = w.online === false
        ? '<span class="badge">offline</span>'
        : '<span class="badge">live</span>';
      card.innerHTML = `
        <div class="agent-top">
          <div class="agent-id-block">
            <div class="agent-name">🖥 ${esc(w.worker)}</div>
            <div class="agent-role">${list.length} skill(s) ${status}</div>
          </div>
        </div>
        ${w.error ? `<p class="muted">relay error: ${esc(w.error)}</p>` : ''}
        ${w.online === false ? '<p class="muted">worker offline</p>' : rows}`;
      host.appendChild(card);
    }
  } catch (e) {
    host.innerHTML = `<p class="muted">workspace skills failed: ${esc(e.message)}</p>`;
  }
}

// ── table ─────────────────────────────────────────────────────────────
function renderSkills() {
  const tbody = $('skills-tbody');
  if (!skills.length) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">No global skills yet. Click “+ Add Skill” to create one.</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  for (const s of [...skills].sort((a, b) => a.name.localeCompare(b.name))) {
    const tr = document.createElement('tr');
    const enabledIds = new Set((s.enabled_agents || []).map(a => a.id));
    const chips = agents.map(a => {
      const on = enabledIds.has(a.id);
      return `<button class="skill-chip${on ? ' skill-chip-on' : ''}"
                data-act="toggle" data-skill="${esc(s.name)}" data-agent="${esc(a.id)}"
                title="${on ? 'Disable' : 'Enable'} for ${esc(a.name)}">${esc(a.name)}</button>`;
    }).join(' ');
    tr.innerHTML = `
      <td><strong>${esc(s.name)}</strong></td>
      <td class="muted cell-ellip" style="max-width:340px">${esc(s.description || '—')}</td>
      <td>${esc(s.version || '—')}</td>
      <td>${chips || '<span class="muted">no agents</span>'}</td>
      <td class="row-actions">
        <button class="icon-btn icon-danger" data-act="del" data-skill="${esc(s.name)}"
                title="Delete skill (removes from all agents)">🗑</button>
      </td>`;
    tbody.appendChild(tr);
  }
}

// ── modals ────────────────────────────────────────────────────────────
function openModal(id)  { $(id).classList.remove('hidden'); }
function closeModal(id) { $(id).classList.add('hidden'); }

// ── events ────────────────────────────────────────────────────────────
export function initSkills() {
  $('skills-ws-refresh').addEventListener('click', () => loadWorkspaceSkills());

  $('skills-install').addEventListener('click', () => {
    $('skill-install-form').reset();
    openModal('skill-install-modal');
  });

  $('skill-install-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    const body = { git_url: f.git_url.value.trim() };
    if (f.path.value.trim()) body.path = f.path.value.trim();
    try {
      const r = await API.post('/api/skills/install', body);
      closeModal('skill-install-modal');
      await loadAll();
      alert(`Installed "${r.installed}"${r.version ? ' v' + r.version : ''}`);
    } catch (err) { alert(err.message); }
  });

  document.querySelectorAll('#skill-install-modal [data-close]').forEach(b =>
    b.addEventListener('click', () => closeModal('skill-install-modal')));

  $('skills-add').addEventListener('click', () => {
    $('skill-form').reset();
    openModal('skill-modal');
  });

  $('skills-tbody').addEventListener('click', async (e) => {
    const b = e.target.closest('button[data-act]');
    if (!b) return;
    try {
      if (b.dataset.act === 'del') {
        if (!confirm(`Delete global skill "${b.dataset.skill}"?\n\nIt is removed from the library and unlinked from every agent.`)) return;
        await API.del('/api/skills/' + encodeURIComponent(b.dataset.skill));
      } else if (b.dataset.act === 'toggle') {
        const on = b.classList.contains('skill-chip-on');
        await API.post('/api/agents/' + b.dataset.agent + '/global-skills',
          { name: b.dataset.skill, enabled: !on });
      }
      await loadAll();
    } catch (err) { alert(err.message); }
  });

  $('skill-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    try {
      await API.post('/api/skills', {
        name: f.name.value.trim(),
        description: f.description.value.trim(),
        content: f.content.value,
      });
      closeModal('skill-modal');
      await loadAll();
    } catch (err) { alert(err.message); }
  });

  document.querySelectorAll('#skill-modal [data-close]').forEach(b =>
    b.addEventListener('click', () => closeModal('skill-modal')));
}

// ── Phase 21: curated registry (marketplace) ─────────────────────────
async function loadRegistry() {
  const host = $('skills-registry');
  try {
    const reg = await API.get('/api/skills/registry');
    const entries = reg.entries || [];
    if (!entries.length) {
      host.innerHTML = '<p class="muted">registry unavailable</p>';
      return;
    }
    host.innerHTML = '';
    for (const e of entries) {
      const card = document.createElement('div');
      card.className = 'agent-card';
      const tags = (e.tags || []).map(t => `<span class="badge">${esc(t)}</span>`).join(' ');
      card.innerHTML = `
        <div class="agent-top">
          <div class="agent-id-block">
            <div class="agent-name">${esc(e.name)}</div>
            <div class="agent-role">${esc(e.description || '')}</div>
          </div>
        </div>
        <div class="agent-meta"><div><span>Source</span>${esc(e.source || e.git_url)}</div>
        <div><span>Tags</span>${tags || '—'}</div></div>
        <div class="agent-actions">
          <button class="btn btn-sm ${e.installed ? '' : 'suggested-action'}"
                  data-reg="${esc(e.name)}" ${e.installed ? 'disabled' : ''}>
            ${e.installed ? '✓ Installed' : '⬇ Install'}</button>
        </div>`;
      host.appendChild(card);
    }
    host.onclick = async (ev) => {
      const b = ev.target.closest('[data-reg]');
      if (!b || b.disabled) return;
      const entry = entries.find(x => x.name === b.dataset.reg);
      if (!entry) return;
      b.disabled = true; b.textContent = '…';
      try {
        const body = { git_url: entry.git_url };
        if (entry.path) body.path = entry.path;
        const r = await API.post('/api/skills/install', body);
        b.textContent = '✓ Installed';
        await loadAll();   // refresh library table + enabled chips
        alert(`Installed "${r.installed}"${r.version ? ' v' + r.version : ''} — toggle it on per agent above.`);
      } catch (err) {
        // The install may have succeeded server-side even when the
        // response was lost (hub restart mid-request, network blip) —
        // re-check before crying wolf. (2026-09-03: pdf installed fine
        // while the user saw a fetch error + the button reset.)
        try {
          const reg = await API.get('/api/skills/registry');
          const now = (reg.entries || []).find(x => x.name === entry.name);
          if (now && now.installed) {
            b.textContent = '✓ Installed';
            await loadAll();
            alert(`Installed "${entry.name}" (confirmed after a connection hiccup).`);
            return;
          }
        } catch {}
        b.disabled = false; b.textContent = '⬇ Install';
        alert(err.message);
      }
    };
  } catch (e2) {
    host.innerHTML = `<p class="muted">registry failed: ${esc(e2.message)}</p>`;
  }
}

export function enterSkills() {
  loadAll().catch(e => alert(e.message));
  loadRegistry();
  loadWorkspaceSkills();
}
