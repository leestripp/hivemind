import type { Agent, Thread } from '../shared/contracts.js';
import { agentAvatar } from './avatars.js';
import { byId, element, replaceChildren } from './dom.js';
import { agentById, state, threadTitle } from './state.js';

const collapsedWorkers = new Set<string>();

export function renderAgents(onSelect: (agent: Agent) => void, onManage: (agent: Agent) => void): void {
  const groups = new Map<string, Agent[]>();
  for (const agent of state.agents) {
    const worker = agent.worker || 'hub';
    groups.set(worker, [...groups.get(worker) ?? [], agent]);
  }
  const online = new Map(state.workers.map((worker) => [worker.name, worker.online]));
  const ordered = [...groups.entries()].sort(([left], [right]) => {
    const onlineDifference = Number(online.get(right) ?? false) - Number(online.get(left) ?? false);
    return onlineDifference || left.localeCompare(right);
  });
  const nodes: HTMLElement[] = [];
  for (const [worker, agents] of ordered) {
    const workerOnline = online.get(worker) ?? false;
    if (!collapsedWorkers.has(worker) && !workerOnline && !state.workers.some((item) => item.name === worker && item.online)) {
      // Preserve the legacy offline-collapsed default only on first sight.
      if (!collapsedWorkers.has(`seen:${worker}`)) {
        collapsedWorkers.add(worker);
        collapsedWorkers.add(`seen:${worker}`);
      }
    }
    const expanded = !collapsedWorkers.has(worker);
    const header = element('button', { className: 'worker-header', ariaLabel: `${expanded ? 'Collapse' : 'Expand'} ${worker}` });
    header.setAttribute('aria-expanded', String(expanded));
    header.append(element('span', { text: expanded ? '▾' : '▸' }),
      element('span', { className: 'group-name', text: `${worker} · ${agents.length}` }),
      element('span', { className: `pill ${workerOnline ? 'online' : 'offline'}`, text: workerOnline ? 'online' : 'offline' }));
    header.addEventListener('click', () => {
      if (collapsedWorkers.delete(worker)) collapsedWorkers.add(`seen:${worker}`);
      else collapsedWorkers.add(worker);
      renderAgents(onSelect, onManage);
    });
    nodes.push(header);
    if (!expanded) continue;
    for (const agent of agents) {
      const row = element('button', { className: `agent-row ${agent.id === state.currentAgentId ? 'selected' : ''} ${workerOnline ? '' : 'offline'}` });
      row.dataset.agentId = agent.id;
      const image = element('img', { className: 'avatar' });
      image.src = agentAvatar(agent);
      image.alt = '';
      const copy = element('span', { className: 'row-copy' });
      copy.append(element('span', { className: 'row-title', text: agent.name }),
        element('span', { className: 'row-subtitle', text: agent.busy ? `${agent.state} · working` : agent.state }));
      row.append(image, copy, element('span', { className: `pill ${agent.state === 'ready' ? 'online' : ''}`, text: agent.state }));
      row.addEventListener('click', () => onSelect(agent));
      const manage = element('button', { className: 'icon-button agent-manage', text: '⋯', ariaLabel: `Manage ${agent.name}` });
      manage.addEventListener('click', () => onManage(agent));
      const container = element('div', { className: 'agent-item' });
      container.append(row, manage);
      nodes.push(container);
    }
  }
  if (!nodes.length) nodes.push(element('p', { className: 'empty-list', text: 'No agents yet. Create your first teammate.' }));
  replaceChildren(byId('agent-list'), ...nodes);
}

export interface ThreadGroupActions {
  onToggle: (groupId: string, collapsed: boolean) => void;
  onNew: () => void;
  onRename: (groupId: string, name: string) => void;
  onDelete: (groupId: string, name: string) => void;
}

function smartBucket(thread: Thread): string {
  if (thread.title.startsWith('routine: ')) return 'smart:routines';
  if (thread.kind === 'user' || thread.kind === 'dm') return 'smart:you';
  if (thread.kind === 'group') return 'smart:chatter';
  return 'smart:routines';
}

export function renderThreads(
  onSelect: (thread: Thread) => void,
  onManage: (thread: Thread) => void,
  actions: ThreadGroupActions,
): void {
  const buckets = [
    { id: 'smart:you', title: 'You & agents', custom: false, threads: [] as Thread[] },
    ...state.threadGroups.map((group) => ({ id: group.id, title: group.name, custom: true, threads: [] as Thread[] })),
    { id: 'smart:chatter', title: 'Agent chatter', custom: false, threads: [] as Thread[] },
    { id: 'smart:routines', title: 'Routines', custom: false, threads: [] as Thread[] },
  ];
  for (const thread of state.threads) {
    const desired = thread.groupId || state.threadUi.overrides[thread.id] || smartBucket(thread);
    const bucket = buckets.find((item) => item.id === desired) ?? buckets.find((item) => item.id === smartBucket(thread));
    bucket?.threads.push(thread);
  }
  const nodes: HTMLElement[] = [];
  for (const bucket of buckets) {
    if (!bucket.threads.length && !bucket.custom) continue;
    const expanded = !state.threadUi.collapsed.includes(bucket.id);
    const unread = bucket.threads.reduce((sum, thread) => sum + thread.unread, 0);
    const header = element('div', { className: 'thread-group-heading' });
    const toggle = element('button', { className: 'thread-group-header', ariaLabel: `${expanded ? 'Collapse' : 'Expand'} ${bucket.title}` });
    toggle.setAttribute('aria-expanded', String(expanded));
    toggle.append(element('span', { text: expanded ? '▾' : '▸' }),
      element('span', { className: 'group-name', text: bucket.title }),
      element('span', { text: `${bucket.threads.length}${unread ? ` · ${unread} new` : ''}` }));
    toggle.addEventListener('click', () => actions.onToggle(bucket.id, expanded));
    header.append(toggle);
    if (bucket.custom) {
      const menu = element('details', { className: 'row-menu group-menu' });
      menu.append(element('summary', { text: '⋯', ariaLabel: `Manage group ${bucket.title}` }));
      const rename = element('button', { text: 'Rename group' });
      rename.addEventListener('click', () => { menu.open = false; actions.onRename(bucket.id, bucket.title); });
      const remove = element('button', { text: 'Delete group', className: 'danger' });
      remove.addEventListener('click', () => { menu.open = false; actions.onDelete(bucket.id, bucket.title); });
      menu.append(rename, remove);
      header.append(menu);
    }
    nodes.push(header);
    if (!expanded) continue;
    const ordered = [...bucket.threads].sort((left, right) => right.unread - left.unread);
    for (const thread of ordered) {
      const row = element('button', { className: `thread-row ${thread.id === state.currentThreadId ? 'selected' : ''}` });
      row.dataset.threadId = thread.id;
      const copy = element('span', { className: 'row-copy' });
      copy.append(element('span', { className: 'row-title', text: threadTitle(thread) }));
      row.append(copy);
      if (thread.unread > 0 && thread.id !== state.currentThreadId) {
        row.append(element('span', { className: 'unread-dot', ariaLabel: `${thread.unread} unread messages` }));
      }
      row.addEventListener('click', () => onSelect(thread));
      const manage = element('button', { className: 'icon-button thread-manage', text: '⋯', ariaLabel: `Manage thread ${threadTitle(thread)}` });
      manage.addEventListener('click', () => onManage(thread));
      const container = element('div', { className: 'thread-item' });
      container.append(row, manage);
      nodes.push(container);
    }
  }
  if (!nodes.length) nodes.push(element('p', { className: 'empty-list', text: 'No threads yet.' }));
  replaceChildren(byId('thread-list'), ...nodes);
}

export function renderAgentOptions(): void {
  const rows = state.agents.map((agent) => {
    const label = element('label', { className: 'check-row' });
    const checkbox = element('input');
    checkbox.type = 'checkbox';
    checkbox.value = agent.id;
    checkbox.name = 'participant';
    label.append(checkbox, document.createTextNode(agent.name));
    return label;
  });
  replaceChildren(byId('thread-agent-options'), ...rows);
}

export function updateSelectedRows(): void {
  document.querySelectorAll<HTMLElement>('[data-agent-id]').forEach((row) => {
    row.classList.toggle('selected', row.dataset.agentId === state.currentAgentId);
  });
  document.querySelectorAll<HTMLElement>('[data-thread-id]').forEach((row) => {
    row.classList.toggle('selected', row.dataset.threadId === state.currentThreadId);
  });
}

export function selectedAgentName(id: string): string { return agentById(id)?.name ?? 'Agent'; }
