import type { Agent, SettingsView, Thread } from '../shared/contracts.js';
import { AgentEditor } from './agent-editor.js';
import { AgentManager } from './agent-manager.js';
import { ConfirmationDialog } from './confirmation.js';
import { byId, element, errorMessage, formatBytes, replaceChildren } from './dom.js';
import { RoutinesDialog } from './routines.js';
import { renderAgentOptions, renderAgents, renderThreads, updateSelectedRows } from './lists.js';
import { MessageRenderer } from './messages.js';
import { SkillsDialog } from './skills.js';
import { SettingsDialog } from './settings-dialog.js';
import { SpeechPlayer } from './speech-player.js';
import { ThreadManager } from './thread-manager.js';
import { applyBootstrap, currentThread, state, threadTitle } from './state.js';
import { VoiceController } from './voice.js';

let bootstrapInFlight = false;
let bootstrapRequested = false;
let bootstrapPromise: Promise<void> | undefined;
let chatInFlight = false;
let agentSelectionGeneration = 0;
let rosterSignature = '';
let activitySeeded = false;
const lastActivity = new Map<string, string>();
let threadSignature = '';
let hasMoreHistory = false;

const speech = new SpeechPlayer(
  (message) => setStatus(message),
  () => messages.resetSpeechButtons(),
);
const messages = new MessageRenderer(speech, setStatus);
const voice = new VoiceController(speech, {
  onSent: () => { void refreshChat(); },
  onError: (message) => setStatus(message),
});
const agentEditor = new AgentEditor(refreshBootstrap);
const confirmation = new ConfirmationDialog();
const skills = new SkillsDialog(confirmation, setStatus);
const routines = new RoutinesDialog(setStatus, () => { void refreshBootstrap(); });
const agentManager = new AgentManager(
  setStatus,
  () => { void refreshBootstrap(); },
  (agent) => { void handleAgentDeleted(agent); },
  (agent) => agentEditor.open(agent),
  (agent) => {
    byId<HTMLDialogElement>('agent-manage-dialog').close();
    routines.open(agent.id);
  },
  (agent) => {
    byId<HTMLDialogElement>('agent-manage-dialog').close();
    skills.open(agent);
  },
);
const threadManager = new ThreadManager(
  confirmation,
  setStatus,
  (thread) => { void handleThreadCleared(thread); },
  (thread) => { void handleThreadDeleted(thread); },
  () => { void refreshBootstrap(); },
  (thread) => openGroupEditor(undefined, thread),
);
new SettingsDialog(speech, {
  onSaved: () => speech.setVolume(),
  onClosed: () => speech.stop(),
});

function setStatus(message: string): void { byId('app-status').textContent = message; }

function applySettings(view: SettingsView): void {
  const { chimpflowKey: _credential, ...ordinary } = view;
  state.settings = ordinary;
  byId<HTMLInputElement>('master-volume').value = String(Math.round(ordinary.masterVolume * 100));
  byId('volume-output').textContent = `${Math.round(ordinary.masterVolume * 100)}%`;
  document.documentElement.style.fontSize = `${14 * ordinary.fontScale}px`;
  speech.setVolume();
}

async function refreshBootstrap(): Promise<void> {
  bootstrapRequested = true;
  if (bootstrapPromise) return bootstrapPromise;
  bootstrapPromise = (async () => {
    do {
      bootstrapRequested = false;
      await loadBootstrap();
    } while (bootstrapRequested);
  })();
  try { await bootstrapPromise; }
  finally { bootstrapPromise = undefined; }
}

async function loadBootstrap(): Promise<void> {
  if (bootstrapInFlight) return;
  bootstrapInFlight = true;
  try {
    const [bootstrap, groups] = await Promise.all([window.hive.getBootstrap(), window.hive.getThreadGroups()]);
    state.threadGroups = groups;
    applyBootstrap(bootstrap);
    processThreadActivity();
    const nextRosterSignature = JSON.stringify(bootstrap.agents) + JSON.stringify(bootstrap.workers);
    if (nextRosterSignature !== rosterSignature) {
      rosterSignature = nextRosterSignature;
      renderAgents((agent) => { void selectAgent(agent); }, (agent) => agentManager.open(agent));
    }
    const nextThreadSignature = JSON.stringify([bootstrap.threads, groups, state.threadUi]);
    if (nextThreadSignature !== threadSignature) {
      threadSignature = nextThreadSignature;
      renderThreads((thread) => { void selectThread(thread); }, (thread) => threadManager.open(thread), {
        onToggle: (groupId, collapsed) => { void toggleThreadGroup(groupId, collapsed); },
        onNew: () => openGroupEditor(),
        onRename: (groupId, name) => openGroupEditor({ id: groupId, name }),
        onDelete: (groupId, name) => { void deleteThreadGroup(groupId, name); },
      });
    }
    updateSelectedRows();
    byId('hub-dot').className = 'status-dot online';
    byId('hub-label').textContent = 'Connected';
    setStatus(state.activity.busy ? 'Agent is working…' : 'Connected');
    renderThreadHeader();
    voice.update();
  } catch (error) {
    byId('hub-dot').className = 'status-dot offline';
    byId('hub-label').textContent = 'Offline';
    setStatus(errorMessage(error));
  } finally {
    bootstrapInFlight = false;
  }
}

let editingGroupId = '';
let moveAfterGroup: Thread | undefined;

function openGroupEditor(group?: { id: string; name: string }, thread?: Thread): void {
  editingGroupId = group?.id ?? '';
  moveAfterGroup = thread;
  byId('thread-group-title').textContent = group ? 'Rename thread group' : 'New thread group';
  byId<HTMLInputElement>('thread-group-name').value = group?.name ?? '';
  byId('thread-group-error').textContent = '';
  byId<HTMLDialogElement>('thread-group-dialog').showModal();
}

async function toggleThreadGroup(groupId: string, collapsed: boolean): Promise<void> {
  try {
    state.threadUi = await window.hive.setThreadGroupCollapsed(groupId, collapsed);
    threadSignature = '';
    await refreshBootstrap();
  } catch (error) { setStatus(errorMessage(error)); }
}

async function deleteThreadGroup(groupId: string, name: string): Promise<void> {
  if (!await confirmation.ask('Delete thread group?', `Delete “${name}”? Its conversations return to their smart groups; no messages are deleted.`, 'Delete group')) return;
  try {
    await window.hive.deleteThreadGroup(groupId);
    threadSignature = '';
    await refreshBootstrap();
  } catch (error) { setStatus(errorMessage(error)); }
}

async function selectAgent(agent: Agent): Promise<void> {
  const generation = ++agentSelectionGeneration;
  setStatus(`Opening ${agent.name}…`);
  try {
    const thread = await window.hive.openAgent(agent.id);
    if (generation !== agentSelectionGeneration) return;
    const existing = state.threads.findIndex((item) => item.id === thread.id);
    if (existing >= 0) state.threads[existing] = thread;
    else state.threads.push(thread);
    await selectThread(thread, agent.id);
    if (generation !== agentSelectionGeneration) return;
    const catalog = await window.hive.getChimpFlowCatalog();
    if (generation !== agentSelectionGeneration || state.currentAgentId !== agent.id) return;
    if (catalog.online) voice.open(agent);
    else {
      voice.close();
      setStatus(catalog.error || 'ChimpFlow is offline');
    }
    await refreshBootstrap();
  } catch (error) {
    setStatus(errorMessage(error));
  }
}

async function selectThread(thread: Thread, agentId?: string): Promise<void> {
  voice.close();
  state.currentThreadId = thread.id;
  state.currentAgentId = agentId ?? (thread.kind === 'dm' ? thread.agentIds[0] ?? '' : '');
  state.messages = [];
  state.activity = { busy: false, turns: [] };
  state.pendingAttachments = [];
  messages.reset();
  renderPendingAttachments();
  updateSelectedRows();
  renderThreadHeader();
  renderComposer();
  byId('chat-scroll').scrollTop = byId('chat-scroll').scrollHeight;
  persistSelection();
  await refreshChat();
  void window.hive.markThreadRead(thread.id).catch((error: unknown) => {
    console.warn('Could not mark thread read:', error);
  });
}

function renderThreadHeader(): void {
  const thread = currentThread();
  byId('thread-title').textContent = thread ? threadTitle(thread) : 'Select a conversation';
  const participants = thread?.agentIds.map((id) => state.agents.find((agent) => agent.id === id)?.name ?? 'Unknown agent') ?? [];
  byId('thread-subtitle').textContent = thread ? participants.join(' · ') : '';
  byId('stop-button').hidden = !thread || !state.activity.busy;
}

function renderComposer(): void {
  const inbox = state.currentThreadId === 'user';
  const enabled = Boolean(state.currentThreadId) && !inbox;
  byId<HTMLTextAreaElement>('message-input').disabled = !enabled;
  byId<HTMLButtonElement>('send-button').disabled = !enabled;
  byId<HTMLButtonElement>('attach-button').disabled = !enabled;
  byId<HTMLTextAreaElement>('message-input').placeholder = inbox
    ? 'Agents message you here — reply in an agent’s own thread'
    : 'Message the team…  (Enter to send)';
}

function processThreadActivity(): void {
  for (const thread of state.threads) {
    if (!activitySeeded) { lastActivity.set(thread.id, thread.lastActivity); continue; }
    if (thread.id === state.currentThreadId) { lastActivity.set(thread.id, thread.lastActivity); continue; }
    const previous = lastActivity.get(thread.id);
    if (previous !== undefined && thread.lastActivity && thread.lastActivity > previous &&
        !document.hasFocus()) {
      window.hive.notify(thread.title || 'HiveMind conversation', 'New messages in this thread');
    }
    // lastActivity remains the notification baseline, not the unread marker.
    if (thread.lastActivity > (lastActivity.get(thread.id) ?? '')) lastActivity.set(thread.id, thread.lastActivity);
  }
  activitySeeded = true;
}

async function refreshChat(): Promise<void> {
  if (chatInFlight || !state.currentThreadId) return;
  chatInFlight = true;
  const threadId = state.currentThreadId;
  const afterId = state.messages.at(-1)?.id ?? '';
  try {
    const [page, activity] = await Promise.all([
      window.hive.getMessages(threadId, afterId),
      window.hive.getActivity(threadId),
    ]);
    if (threadId !== state.currentThreadId) return;
    state.messages.push(...page.messages.filter((message) => !state.messages.some((existing) => existing.id === message.id)));
    state.activity = activity;
    if (!afterId) hasMoreHistory = page.hasMore;
    byId('load-earlier-button').hidden = !hasMoreHistory;
    messages.append(page.messages);
    messages.updateLive();
    renderThreadHeader();
    if (page.messages.length) {
      byId('chat-scroll').scrollTop = byId('chat-scroll').scrollHeight;
      void window.hive.markThreadRead(threadId).catch((error: unknown) => console.warn('Read mark failed:', error));
    }
    voice.update();
  } catch (error) {
    setStatus(errorMessage(error));
  } finally {
    chatInFlight = false;
  }
}

async function loadEarlier(): Promise<void> {
  const threadId = state.currentThreadId;
  const oldest = state.messages[0]?.id;
  if (!threadId || !oldest) return;
  try {
    const page = await window.hive.getMessages(threadId, '', oldest);
    if (threadId !== state.currentThreadId) return;
    const fresh = page.messages.filter((message) => !state.messages.some((existing) => existing.id === message.id));
    state.messages.unshift(...fresh);
    hasMoreHistory = page.hasMore;
    messages.reset();
    messages.append(state.messages);
    messages.updateLive();
    byId('load-earlier-button').hidden = !hasMoreHistory;
  } catch (error) { setStatus(errorMessage(error)); }
}

function persistSelection(): void {
  void window.hive.saveSettings({
    selectedAgentId: state.currentAgentId, selectedThreadId: state.currentThreadId,
  }).catch((error: unknown) => console.error('Could not save selection:', error));
}

async function sendCurrentMessage(): Promise<void> {
  const input = byId<HTMLTextAreaElement>('message-input');
  const body = input.value;
  if (!state.currentThreadId || state.currentThreadId === 'user' || (!body.trim() && !state.pendingAttachments.length)) return;
  const send = byId<HTMLButtonElement>('send-button');
  send.disabled = true;
  try {
    await window.hive.sendMessage(state.currentThreadId, body, 'user', state.pendingAttachments);
    input.value = '';
    state.pendingAttachments = [];
    renderPendingAttachments();
    await refreshChat();
  } catch (error) { setStatus(errorMessage(error)); }
  finally { send.disabled = false; }
}

function renderPendingAttachments(): void {
  const children = state.pendingAttachments.map((attachment, index) => {
    const chip = element('span', { className: 'attachment-chip', text: `📎 ${attachment.name} · ${formatBytes(attachment.size)} ` });
    const remove = element('button', { className: 'quiet', text: '✕', ariaLabel: `Remove ${attachment.name}` });
    remove.addEventListener('click', () => {
      state.pendingAttachments.splice(index, 1);
      renderPendingAttachments();
    });
    chip.append(remove);
    return chip;
  });
  replaceChildren(byId('pending-attachments'), ...children);
}

function openConnect(): void {
  byId<HTMLInputElement>('hub-server').value = state.settings.hubServer;
  byId<HTMLInputElement>('hub-api-key').value = state.settings.hubApiKey;
  byId('connect-error').textContent = '';
  byId<HTMLDialogElement>('connect-dialog').showModal();
}

async function handleThreadCleared(thread: Thread): Promise<void> {
  if (state.currentThreadId === thread.id) {
    state.messages = [];
    state.activity = { busy: false, turns: [] };
    hasMoreHistory = false;
    messages.reset();
    await refreshChat();
  }
  await refreshBootstrap();
}

async function handleThreadDeleted(thread: Thread): Promise<void> {
  if (state.currentThreadId === thread.id) {
    voice.close();
    state.currentThreadId = '';
    state.currentAgentId = '';
    state.messages = [];
    state.activity = { busy: false, turns: [] };
    hasMoreHistory = false;
    messages.reset();
    persistSelection();
  }
  await refreshBootstrap();
  renderThreadHeader();
  renderComposer();
}

async function handleAgentDeleted(agent: Agent): Promise<void> {
  if (state.currentAgentId === agent.id) {
    voice.close();
    state.currentAgentId = '';
    state.currentThreadId = '';
    state.messages = [];
    state.activity = { busy: false, turns: [] };
    messages.reset();
    persistSelection();
  }
  await refreshBootstrap();
  if (state.currentThreadId && !state.threads.some((thread) => thread.id === state.currentThreadId)) {
    state.currentThreadId = '';
    state.currentAgentId = '';
    state.messages = [];
    messages.reset();
    persistSelection();
  }
  renderThreadHeader();
  renderComposer();
}

function openNewThread(): void {
  if (!state.agents.length) { setStatus('Create an agent first'); return; }
  byId<HTMLFormElement>('new-thread-form').reset();
  renderAgentOptions();
  byId<HTMLButtonElement>('new-thread-submit').disabled = true;
  byId('new-thread-error').textContent = '';
  byId<HTMLDialogElement>('new-thread-dialog').showModal();
}

function bindStaticControls(): void {
  byId('connect-button').addEventListener('click', openConnect);
  byId('tasks-button').addEventListener('click', () => routines.open());
  byId('connect-cancel').addEventListener('click', () => byId<HTMLDialogElement>('connect-dialog').close());
  byId<HTMLFormElement>('connect-form').addEventListener('submit', (event) => {
    event.preventDefault();
    void (async () => {
      const submit = byId<HTMLButtonElement>('connect-submit');
      submit.disabled = true;
      try {
        const server = byId<HTMLInputElement>('hub-server').value;
        const apiKey = byId<HTMLInputElement>('hub-api-key').value;
        await window.hive.testHubConnection(server, apiKey);
        applySettings(await window.hive.saveSettings({ hubServer: server, hubApiKey: apiKey }));
        byId<HTMLDialogElement>('connect-dialog').close();
        await refreshBootstrap();
      } catch (error) { byId('connect-error').textContent = errorMessage(error); }
      finally { submit.disabled = false; }
    })();
  });
  byId('new-agent-button').addEventListener('click', () => agentEditor.open());
  byId('new-thread-button').addEventListener('click', openNewThread);
  byId('thread-agent-options').addEventListener('change', () => {
    byId<HTMLButtonElement>('new-thread-submit').disabled = !document.querySelector('input[name=participant]:checked');
  });
  byId('new-thread-cancel').addEventListener('click', () => byId<HTMLDialogElement>('new-thread-dialog').close());
  byId<HTMLFormElement>('new-thread-form').addEventListener('submit', (event) => {
    event.preventDefault();
    void (async () => {
      const agentIds = Array.from(document.querySelectorAll<HTMLInputElement>('input[name=participant]:checked')).map((input) => input.value);
      if (!agentIds.length) {
        byId('new-thread-error').textContent = 'Select at least one agent';
        return;
      }
      try {
        const thread = await window.hive.createThread(byId<HTMLInputElement>('new-thread-name').value, agentIds);
        state.threads.push(thread);
        byId<HTMLDialogElement>('new-thread-dialog').close();
        await refreshBootstrap();
        await selectThread(thread);
      } catch (error) { byId('new-thread-error').textContent = errorMessage(error); }
    })();
  });
  byId<HTMLFormElement>('composer').addEventListener('submit', (event) => {
    event.preventDefault();
    void sendCurrentMessage();
  });
  byId<HTMLTextAreaElement>('message-input').addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      void sendCurrentMessage();
    }
  });
  byId('stop-button').addEventListener('click', () => {
    void (async () => {
      if (!state.currentThreadId) return;
      try {
        await window.hive.cancelThread(state.currentThreadId);
        speech.stop();
        await refreshChat();
      } catch (error) { setStatus(errorMessage(error)); }
    })();
  });
  byId('load-earlier-button').addEventListener('click', () => { void loadEarlier(); });
  byId('attach-button').addEventListener('click', () => byId<HTMLInputElement>('attachment-input').click());
  byId<HTMLInputElement>('attachment-input').addEventListener('change', (event) => {
    void (async () => {
      const input = event.currentTarget as HTMLInputElement;
      const file = input.files?.[0];
      if (!file || !state.currentThreadId) return;
      try {
        setStatus(`Uploading ${file.name}…`);
        const attachment = await window.hive.uploadAttachment(state.currentThreadId, file.name, await file.arrayBuffer());
        state.pendingAttachments.push(attachment);
        renderPendingAttachments();
        setStatus(`${file.name} attached`);
      } catch (error) { setStatus(errorMessage(error)); }
      finally { input.value = ''; }
    })();
  });
  byId('new-thread-group-button').addEventListener('click', () => openGroupEditor());
  byId('thread-group-cancel').addEventListener('click', () => byId<HTMLDialogElement>('thread-group-dialog').close());
  byId<HTMLFormElement>('thread-group-form').addEventListener('submit', (event) => {
    event.preventDefault();
    void (async () => {
      try {
        const name = byId<HTMLInputElement>('thread-group-name').value.trim();
        if (editingGroupId) await window.hive.renameThreadGroup(editingGroupId, name);
        else {
          const group = await window.hive.createThreadGroup(name);
          if (moveAfterGroup) state.threadUi = await window.hive.moveThreadToGroup(moveAfterGroup.id, group.id);
        }
        byId<HTMLDialogElement>('thread-group-dialog').close();
        threadSignature = '';
        await refreshBootstrap();
      } catch (error) { byId('thread-group-error').textContent = errorMessage(error); }
    })();
  });
  byId<HTMLInputElement>('master-volume').addEventListener('input', (event) => {
    const value = Number((event.currentTarget as HTMLInputElement).value);
    state.settings.masterVolume = value / 100;
    byId('volume-output').textContent = `${value}%`;
    speech.setVolume();
  });
  byId<HTMLInputElement>('master-volume').addEventListener('change', () => {
    void window.hive.saveSettings({ masterVolume: state.settings.masterVolume })
      .catch((error: unknown) => setStatus(errorMessage(error)));
  });
  byId<HTMLInputElement>('mute-audio').addEventListener('change', (event) => {
    state.muted = (event.currentTarget as HTMLInputElement).checked;
    speech.setVolume();
  });
}

function applyTheme(theme: { dark: boolean; source: string }): void {
  document.documentElement.dataset.theme = theme.dark ? 'dark' : 'light';
  document.documentElement.style.colorScheme = theme.dark ? 'dark' : 'light';
}

async function initialize(): Promise<void> {
  bindStaticControls();
  window.hive.onThemeChanged(applyTheme);
  applyTheme(await window.hive.getTheme());
  applySettings(await window.hive.getSettings());
  state.threadUi = await window.hive.getThreadUiState();
  await refreshBootstrap();
  const restore = state.threads.find((thread) => thread.id === state.settings.selectedThreadId);
  if (restore) await selectThread(restore, state.settings.selectedAgentId || undefined);
  if (!state.settings.hubApiKey) openConnect();
  window.setInterval(() => { void refreshBootstrap(); }, 1500);
  window.setInterval(() => { void refreshChat(); }, 1500);
}

void initialize().catch((error: unknown) => {
  console.error('Renderer initialization failed:', error);
  setStatus(`Could not start HiveMind: ${errorMessage(error)}`);
});
