export interface Agent {
  id: string;
  name: string;
  avatar: string;
  worker: string;
  state: string;
  busy: boolean;
  model: string;
  role: string;
  workspaceMode: string;
  type: string;
  memory: string;
  cpus: number;
  thinkingLevel: string;
  hostAgent: boolean;
  createdAt: string;
  updatedAt: string;
}

export interface WorkerInfo { name: string; online: boolean; caps: Record<string, unknown>; }

export interface Thread {
  id: string;
  title: string;
  kind: string;
  agentIds: string[];
  unread: number;
  lastActivity: string;
  groupId: string | null;
}

export interface ThreadGroup { id: string; name: string; }
export interface ThreadUiState { collapsed: string[]; overrides: Record<string, string>; }

export interface Attachment {
  name: string;
  path: string;
  hostPath: string;
  size: number;
  worker?: string;
}

export interface ChatMessage {
  id: string;
  senderKind: string;
  senderId: string;
  senderName: string;
  body: string;
  createdAt: string;
  attachments: Attachment[];
}

export interface ActivityTurn {
  agentId: string;
  agentName: string;
  partial: string;
  done: boolean;
  cancelling: boolean;
  error: string;
}

export interface ThreadActivity { busy: boolean; turns: ActivityTurn[]; }
export interface MessagePage { messages: ChatMessage[]; hasMore: boolean; }
export interface Bootstrap { agents: Agent[]; workers: WorkerInfo[]; threads: Thread[]; }
export type VoiceAssignments = Record<string, Record<string, string>>;

export interface AppSettings {
  hubServer: string;
  hubApiKey: string;
  chimpflowUrl: string;
  chimpflowModel: string;
  voices: VoiceAssignments;
  displayName: string;
  userAvatar: string;
  fontScale: number;
  masterVolume: number;
  selectedAgentId: string;
  selectedThreadId: string;
}

export interface SettingsPatch extends Partial<Omit<AppSettings, 'voices'>> {
  voices?: VoiceAssignments;
  chimpflowKey?: string;
}
export interface SettingsView extends AppSettings { chimpflowKey: string; }
export interface ThemeState { dark: boolean; source: string; }

export interface ChimpFlowCatalog {
  online: boolean;
  models: string[];
  model: string;
  voices: string[];
  error: string;
}
export interface SpeechAudio { bytes: ArrayBuffer; mimeType: string; }
export interface AttachmentPayload { bytes: ArrayBuffer; mimeType: string; }
export type RoutineSchedule =
  | { type: 'once'; at: string }
  | { type: 'interval'; every_minutes: number }
  | { type: 'daily'; time: string }
  | { type: 'weekly'; time: string; days: number[] };

export interface Routine {
  id: string;
  agentId: string;
  threadId: string;
  name: string;
  prompt: string;
  schedule: RoutineSchedule;
  scheduleText: string;
  nextFireAt: string;
  enabled: boolean;
  lastFiredAt: string;
}
export interface RoutineInput {
  agentId: string;
  name: string;
  prompt: string;
  schedule: RoutineSchedule;
}
export interface NewAgentInput {
  name: string;
  role: string;
  model: string;
  worker: string;
  avatar?: string;
  type: string;
  memory: string;
  cpus: number;
  thinkingLevel: string;
  hostAgent?: boolean;
}
export interface AgentModel { name: string; provider: string; }
export interface AgentTemplate { name: string; role: string; avatar: string; }
export interface AgentOptions { models: AgentModel[]; defaultModel: string; templates: AgentTemplate[]; }

export interface SkillInput { name: string; description: string; content: string; }
export interface SharedSkill extends SkillInput { version: string; enabled: boolean; }
export interface AgentSkills { shared: SharedSkill[]; local: string[]; }
export interface ExecEntry {
  timestamp: string;
  command: string;
  exitCode: number;
  timedOut: boolean;
  durationMs: number;
}

export interface HiveClientApi {
  getSettings(): Promise<SettingsView>;
  saveSettings(patch: SettingsPatch): Promise<SettingsView>;
  notify(title: string, body: string): void;
  getTheme(): Promise<ThemeState>;
  onThemeChanged(callback: (theme: ThemeState) => void): () => void;
  testHubConnection(server: string, apiKey: string): Promise<void>;
  getBootstrap(): Promise<Bootstrap>;
  getThreadGroups(): Promise<ThreadGroup[]>;
  createThreadGroup(name: string): Promise<ThreadGroup>;
  renameThreadGroup(groupId: string, name: string): Promise<void>;
  deleteThreadGroup(groupId: string): Promise<void>;
  getThreadUiState(): Promise<ThreadUiState>;
  setThreadGroupCollapsed(groupId: string, collapsed: boolean): Promise<ThreadUiState>;
  moveThreadToGroup(threadId: string, groupId: string): Promise<ThreadUiState>;
  openAgent(agentId: string): Promise<Thread>;
  createThread(title: string, agentIds: string[]): Promise<Thread>;
  getAgentOptions(): Promise<AgentOptions>;
  cancelAgent(agentId: string): Promise<number>;
  createAgent(input: NewAgentInput): Promise<Agent>;
  updateAgent(agentId: string, input: NewAgentInput): Promise<Agent>;
  deleteAgent(agentId: string, reassignTo?: string): Promise<void>;
  setAgentRunning(agentId: string, running: boolean): Promise<void>;
  getExecLog(agentId: string): Promise<ExecEntry[]>;
  openAgentScreen(agentId: string, viewOnly: boolean): Promise<void>;
  getAgentSkills(agentId: string): Promise<AgentSkills>;
  setSharedSkillEnabled(agentId: string, name: string, enabled: boolean): Promise<void>;
  createGlobalSkill(input: SkillInput): Promise<void>;
  createAgentSkill(agentId: string, input: SkillInput): Promise<void>;
  deleteGlobalSkill(name: string): Promise<void>;
  deleteAgentSkill(agentId: string, name: string): Promise<void>;
  getMessages(threadId: string, afterId?: string, beforeId?: string): Promise<MessagePage>;
  getActivity(threadId: string): Promise<ThreadActivity>;
  sendMessage(threadId: string, body: string, source: 'user' | 'voice', attachments?: Attachment[]): Promise<void>;
  cancelThread(threadId: string): Promise<void>;
  clearThread(threadId: string): Promise<void>;
  deleteThread(threadId: string): Promise<void>;
  markThreadRead(threadId: string): Promise<void>;
  getChimpFlowCatalog(model?: string): Promise<ChimpFlowCatalog>;
  synthesize(text: string, agentId: string, model?: string, voice?: string): Promise<SpeechAudio>;
  transcribe(wav: ArrayBuffer): Promise<string>;
  uploadAttachment(threadId: string, filename: string, bytes: ArrayBuffer): Promise<Attachment>;
  getAttachment(threadId: string, attachment: Attachment): Promise<AttachmentPayload>;
  saveAttachment(threadId: string, attachment: Attachment): Promise<string>;
  getRoutines(): Promise<Routine[]>;
  createRoutine(input: RoutineInput): Promise<Routine>;
  updateRoutine(routineId: string, input: RoutineInput): Promise<Routine>;
  setRoutineEnabled(routineId: string, enabled: boolean): Promise<void>;
  runRoutine(routineId: string): Promise<void>;
  deleteRoutine(routineId: string): Promise<void>;
}

declare global { interface Window { hive: HiveClientApi; } }
