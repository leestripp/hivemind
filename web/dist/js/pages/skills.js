// js/pages/skills.js — global skills library management.
// Server side: SkillRoutes manages ~/.local/share/hivemind/skills/ and
// per-agent enable via symlinks into <agent_dir>/.agents/skills/. Local
// (agent-learned) skills stay between the user and the agent — this page
// only manages the GLOBAL library and who it's enabled for.
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

export function enterSkills() { loadAll().catch(e => log?.(e.message)); }
