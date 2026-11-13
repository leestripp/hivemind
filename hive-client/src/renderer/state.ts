import type {
  Agent, AppSettings, Attachment, Bootstrap, ChatMessage, Thread, ThreadActivity, ThreadGroup, ThreadUiState, WorkerInfo,
} from '../shared/contracts.js';

export interface AppState {
  settings: AppSettings;
  agents: Agent[];
  workers: WorkerInfo[];
  threadGroups: ThreadGroup[];
  threadUi: ThreadUiState;
  threads: Thread[];
  messages: ChatMessage[];
  activity: ThreadActivity;
  currentThreadId: string;
  currentAgentId: string;
  pendingAttachments: Attachment[];
  voiceOpen: boolean;
  muted: boolean;
}

export const state: AppState = {
  settings: {
    hubServer: '', hubApiKey: '', chimpflowUrl: '', chimpflowModel: 'kokoro-82m', voices: {},
    displayName: 'You', userAvatar: '', fontScale: 1, masterVolume: 1, selectedAgentId: '', selectedThreadId: '',
  },
  agents: [], workers: [], threadGroups: [], threadUi: { collapsed: [], overrides: {} }, threads: [], messages: [],
  activity: { busy: false, turns: [] }, currentThreadId: '', currentAgentId: '',
  pendingAttachments: [], voiceOpen: false, muted: false,
};

export function applyBootstrap(bootstrap: Bootstrap): void {
  state.agents = bootstrap.agents;
  state.workers = bootstrap.workers;
  state.threads = bootstrap.threads;
}

export function currentThread(): Thread | undefined {
  return state.threads.find((thread) => thread.id === state.currentThreadId);
}

export function currentAgent(): Agent | undefined {
  return state.agents.find((agent) => agent.id === state.currentAgentId);
}

export function agentById(id: string): Agent | undefined {
  return state.agents.find((agent) => agent.id === id);
}

export function threadTitle(thread: Thread): string {
  if (thread.title) return thread.kind === 'user' ? `📥 ${thread.title}` : thread.title;
  const names = thread.agentIds.map((id) => agentById(id)?.name ?? 'Unknown agent');
  return names.join(', ') || (thread.kind === 'user' ? '📥 User Inbox' : 'Conversation');
}
