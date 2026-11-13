import { contextBridge, ipcRenderer } from 'electron';
import type { HiveClientApi } from '../shared/contracts.js';

const invoke = (channel: string, ...args: unknown[]) => ipcRenderer.invoke(channel, ...args);

const api: HiveClientApi = {
  getSettings: () => invoke('settings:get'),
  saveSettings: (patch) => invoke('settings:save', patch),
  notify: (title, body) => { ipcRenderer.send('ui:notify', title, body); },
  getTheme: () => invoke('ui:theme'),
  onThemeChanged: (callback) => {
    const listener = (_event: unknown, theme: { dark: boolean; source: string }) => callback(theme);
    ipcRenderer.on('ui:theme-changed', listener);
    return () => { ipcRenderer.removeListener('ui:theme-changed', listener); };
  },
  testHubConnection: (server, apiKey) => invoke('hub:test', server, apiKey),
  getBootstrap: () => invoke('hub:bootstrap'),
  getThreadGroups: () => invoke('hub:thread-groups'),
  createThreadGroup: (name) => invoke('hub:create-thread-group', name),
  renameThreadGroup: (groupId, name) => invoke('hub:rename-thread-group', groupId, name),
  deleteThreadGroup: (groupId) => invoke('hub:delete-thread-group', groupId),
  getThreadUiState: () => invoke('ui:thread-state'),
  setThreadGroupCollapsed: (groupId, collapsed) => invoke('ui:thread-collapsed', groupId, collapsed),
  moveThreadToGroup: (threadId, groupId) => invoke('ui:move-thread', threadId, groupId),
  openAgent: (agentId) => invoke('hub:open-agent', agentId),
  createThread: (title, agentIds) => invoke('hub:create-thread', title, agentIds),
  getAgentOptions: () => invoke('hub:agent-options'),
  cancelAgent: (agentId) => invoke('hub:cancel-agent', agentId),
  createAgent: (input) => invoke('hub:create-agent', input),
  updateAgent: (agentId, input) => invoke('hub:update-agent', agentId, input),
  deleteAgent: (agentId, reassignTo = '') => invoke('hub:delete-agent', agentId, reassignTo),
  setAgentRunning: (agentId, running) => invoke('hub:set-agent-running', agentId, running),
  getExecLog: (agentId) => invoke('hub:exec-log', agentId),
  openAgentScreen: (agentId, viewOnly) => invoke('hub:agent-screen', agentId, viewOnly),
  getAgentSkills: (agentId) => invoke('hub:agent-skills', agentId),
  setSharedSkillEnabled: (agentId, name, enabled) => invoke('hub:shared-skill-enabled', agentId, name, enabled),
  createGlobalSkill: (input) => invoke('hub:create-global-skill', input),
  createAgentSkill: (agentId, input) => invoke('hub:create-agent-skill', agentId, input),
  deleteGlobalSkill: (name) => invoke('hub:delete-global-skill', name),
  deleteAgentSkill: (agentId, name) => invoke('hub:delete-agent-skill', agentId, name),
  getMessages: (threadId, afterId = '', beforeId = '') => invoke('hub:messages', threadId, afterId, beforeId),
  getActivity: (threadId) => invoke('hub:activity', threadId),
  sendMessage: (threadId, body, source, attachments = []) => invoke('hub:send', threadId, body, source, attachments),
  cancelThread: (threadId) => invoke('hub:cancel', threadId),
  clearThread: (threadId) => invoke('hub:clear-thread', threadId),
  deleteThread: (threadId) => invoke('hub:delete-thread', threadId),
  markThreadRead: (threadId) => invoke('hub:read', threadId),
  getChimpFlowCatalog: (model = '') => invoke('chimpflow:catalog', model),
  synthesize: (text, agentId, model = '', voice) => invoke('chimpflow:synthesize', text, agentId, model, voice),
  transcribe: (wav) => invoke('chimpflow:transcribe', wav),
  uploadAttachment: (threadId, filename, bytes) => invoke('hub:upload', threadId, filename, bytes),
  getAttachment: (threadId, attachment) => invoke('hub:attachment', threadId, attachment),
  saveAttachment: (threadId, attachment) => invoke('hub:save-attachment', threadId, attachment),
  getRoutines: () => invoke('hub:routines'),
  createRoutine: (input) => invoke('hub:create-routine', input),
  updateRoutine: (routineId, input) => invoke('hub:update-routine', routineId, input),
  setRoutineEnabled: (routineId, enabled) => invoke('hub:set-routine-enabled', routineId, enabled),
  runRoutine: (routineId) => invoke('hub:run-routine', routineId),
  deleteRoutine: (routineId) => invoke('hub:delete-routine', routineId),
};

contextBridge.exposeInMainWorld('hive', Object.freeze(api));
