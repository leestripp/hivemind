import type {
  ActivityTurn, Agent, Attachment, Bootstrap, ChatMessage, MessagePage, Routine, RoutineSchedule,
  Thread, ThreadActivity, WorkerInfo,
} from './contracts.js';

export function objectValue(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown> : {};
}
export function arrayValue(value: unknown): unknown[] { return Array.isArray(value) ? value : []; }
export function stringValue(value: unknown, fallback = ''): string {
  return typeof value === 'string' ? value : fallback;
}
export function numberValue(value: unknown, fallback = 0): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}
export function booleanValue(value: unknown, fallback = false): boolean {
  return typeof value === 'boolean' ? value : fallback;
}

export function normalizeAgent(value: unknown): Agent {
  const item = objectValue(value);
  return {
    id: stringValue(item.id), name: stringValue(item.name, 'Unnamed agent'),
    avatar: stringValue(item.avatar), worker: stringValue(item.worker, 'hub'),
    state: stringValue(item.state, 'down').toLowerCase(), busy: booleanValue(item.busy),
    model: stringValue(item.model), role: stringValue(item.role),
    workspaceMode: stringValue(item.workspace_mode, 'shared'), type: stringValue(item.type),
    memory: stringValue(item.memory), cpus: numberValue(item.cpus),
    thinkingLevel: stringValue(item.thinking_level), hostAgent: booleanValue(item.host_agent),
    createdAt: stringValue(item.created_at), updatedAt: stringValue(item.updated_at),
  };
}

export function normalizeThread(value: unknown): Thread {
  const item = objectValue(value);
  return {
    id: stringValue(item.id), title: stringValue(item.title), kind: stringValue(item.kind),
    agentIds: arrayValue(item.agent_ids).filter((id): id is string => typeof id === 'string'),
    unread: numberValue(item.unread), lastActivity: stringValue(item.last_activity),
    groupId: typeof item.group_id === 'string' ? item.group_id : null,
  };
}

export function normalizeAttachment(value: unknown): Attachment {
  const item = objectValue(value);
  return {
    name: stringValue(item.name, 'attachment'), path: stringValue(item.path),
    hostPath: stringValue(item.host_path), size: numberValue(item.size),
    worker: stringValue(item.worker),
  };
}

export function normalizeMessage(value: unknown): ChatMessage {
  const item = objectValue(value);
  return {
    id: stringValue(item.id), senderKind: stringValue(item.sender_kind),
    senderId: stringValue(item.sender_id), senderName: stringValue(item.sender_name),
    body: stringValue(item.body), createdAt: stringValue(item.created_at),
    attachments: arrayValue(item.attachments).map(normalizeAttachment),
  };
}

export function normalizeMessagePage(value: unknown): MessagePage {
  const root = objectValue(value);
  return {
    messages: arrayValue(root.messages).map(normalizeMessage).filter((message) => message.id.length > 0),
    hasMore: booleanValue(root.has_more),
  };
}

export function normalizeActivity(value: unknown): ThreadActivity {
  const root = objectValue(value);
  const turns: ActivityTurn[] = arrayValue(root.turns).map((value) => {
    const item = objectValue(value);
    return {
      agentId: stringValue(item.agent_id), agentName: stringValue(item.agent_name, 'Agent'),
      partial: stringValue(item.partial), done: booleanValue(item.done, true),
      cancelling: booleanValue(item.cancelling), error: stringValue(item.error),
    };
  });
  return { busy: booleanValue(root.busy), turns };
}

export function normalizeBootstrap(agents: unknown, workers: unknown, threads: unknown): Bootstrap {
  return {
    agents: arrayValue(objectValue(agents).agents).map(normalizeAgent).filter((agent) => agent.id.length > 0),
    workers: arrayValue(objectValue(workers).workers).map((value): WorkerInfo => {
      const item = objectValue(value);
      return { name: stringValue(item.name), online: booleanValue(item.online), caps: objectValue(item.caps) };
    }),
    threads: arrayValue(objectValue(threads).threads).map(normalizeThread).filter((thread) => thread.id.length > 0),
  };
}

export function normalizeRoutine(value: unknown): Routine {
  const item = objectValue(value);
  const raw = objectValue(item.schedule);
  const type = stringValue(raw.type);
  let schedule: RoutineSchedule = { type: 'once', at: stringValue(raw.at) };
  if (type === 'interval') schedule = { type, every_minutes: numberValue(raw.every_minutes, 60) };
  else if (type === 'daily') schedule = { type, time: stringValue(raw.time) };
  else if (type === 'weekly') schedule = { type, time: stringValue(raw.time), days: arrayValue(raw.days).map(numberValue) };
  return {
    id: stringValue(item.id), agentId: stringValue(item.agent_id), threadId: stringValue(item.thread_id),
    name: stringValue(item.name, 'Scheduled task'), prompt: stringValue(item.prompt), schedule,
    scheduleText: stringValue(item.schedule_text), nextFireAt: stringValue(item.next_fire_at),
    enabled: booleanValue(item.enabled, true), lastFiredAt: stringValue(item.last_fired_at),
  };
}
