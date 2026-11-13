import type { AgentOptions, Attachment, AttachmentPayload, Bootstrap, AgentSkills, ExecEntry, NewAgentInput, Routine, RoutineInput, SkillInput, Thread, ThreadGroup } from '../shared/contracts.js';
import {
  normalizeActivity, normalizeAgent, normalizeBootstrap, normalizeMessagePage, normalizeRoutine,
  normalizeThread, objectValue, arrayValue, stringValue, numberValue,
} from '../shared/normalize.js';
import type { SettingsStore } from './config.js';
import { requestBytes, requestJson, responseError } from './http.js';

const id = (value: string) => encodeURIComponent(value);

export class HubService {
  constructor(private readonly settings: SettingsStore) {}

  private connection(): { server: string; key: string } {
    const snapshot = this.settings.snapshot();
    return { server: snapshot.hubServer, key: snapshot.hubApiKey };
  }

  private json(path: string, method = 'GET', body?: unknown, timeout?: number): Promise<unknown> {
    const { server, key } = this.connection();
    return requestJson(server, path, key, method, body, timeout);
  }

  async getBootstrap(): Promise<Bootstrap> {
    const agentsPromise = this.json('/api/agents');
    const threadsPromise = this.json('/api/threads');
    const workersPromise = this.json('/api/workers').catch((error: unknown) => {
      console.warn('Worker roster unavailable:', error);
      return {};
    });
    return normalizeBootstrap(await agentsPromise, await workersPromise, await threadsPromise);
  }

  async openAgent(agentId: string): Promise<Thread> {
    const result = await this.json('/api/threads', 'POST', {
      title: '', agent_ids: [agentId], kind: 'dm', reuse_dm: true,
    });
    return normalizeThread(result);
  }

  async getThreadGroups(): Promise<ThreadGroup[]> {
    const result = await this.json('/api/thread-groups');
    return arrayValue(objectValue(result).groups ?? result).map((value) => {
      const item = objectValue(value);
      return { id: stringValue(item.id), name: stringValue(item.name) };
    }).filter((group) => group.id && group.name);
  }

  async createThreadGroup(name: string): Promise<ThreadGroup> {
    const result = objectValue(await this.json('/api/thread-groups', 'POST', { name }));
    return { id: stringValue(result.id), name: stringValue(result.name, name) };
  }

  async renameThreadGroup(groupId: string, name: string): Promise<void> {
    await this.json(`/api/thread-groups/${id(groupId)}`, 'PATCH', { name });
  }

  async deleteThreadGroup(groupId: string): Promise<void> {
    await this.json(`/api/thread-groups/${id(groupId)}`, 'DELETE');
  }

  async setThreadGroup(threadId: string, groupId: string): Promise<void> {
    await this.json(`/api/threads/${id(threadId)}/group`, 'PUT', { group_id: groupId || null });
  }

  async createThread(title: string, agentIds: string[]): Promise<Thread> {
    const result = await this.json('/api/threads', 'POST', { title, agent_ids: agentIds });
    return normalizeThread(result);
  }

  async getAgentOptions(): Promise<AgentOptions> {
    const [modelResult, templateResult] = await Promise.all([this.json('/api/models'), this.json('/api/templates')]);
    const models = objectValue(modelResult);
    const templates = objectValue(templateResult);
    return {
      models: arrayValue(models.models).map((value) => {
        const item = objectValue(value);
        return { name: stringValue(item.name), provider: stringValue(item.provider) };
      }).filter((model) => model.name),
      defaultModel: stringValue(models.default_model),
      templates: arrayValue(templates.templates ?? templateResult).map((value) => {
        const item = objectValue(value);
        return { name: stringValue(item.name), role: stringValue(item.role), avatar: stringValue(item.avatar) };
      }).filter((template) => template.name),
    };
  }

  async cancelAgent(agentId: string): Promise<number> {
    const result = objectValue(await this.json(`/api/agents/${id(agentId)}/cancel`, 'POST'));
    return numberValue(result.cancelled);
  }

  async createAgent(input: NewAgentInput) {
    const result = await this.json('/api/agents', 'POST', {
      name: input.name, role: input.role, model: input.model,
      worker: input.worker, workspace_mode: 'shared', avatar: input.avatar ?? '',
      ...(input.type ? { type: input.type } : {}),
      ...(input.memory ? { memory: input.memory } : {}),
      ...(input.cpus >= 0 ? { cpus: input.cpus } : {}),
      ...(input.thinkingLevel ? { thinking_level: input.thinkingLevel } : {}),
      ...(input.hostAgent === undefined ? {} : { host_agent: input.hostAgent }),
    });
    return normalizeAgent(objectValue(result).agent ?? result);
  }

  async updateAgent(agentId: string, input: NewAgentInput) {
    const result = await this.json(`/api/agents/${id(agentId)}`, 'PATCH', {
      name: input.name, role: input.role, model: input.model,
      worker: input.worker, workspace_mode: 'shared', avatar: input.avatar ?? '',
      ...(input.type ? { type: input.type } : {}),
      ...(input.memory ? { memory: input.memory } : {}),
      ...(input.cpus >= 0 ? { cpus: input.cpus } : {}),
      thinking_level: input.thinkingLevel,
      ...(input.hostAgent === undefined ? {} : { host_agent: input.hostAgent }),
    });
    return normalizeAgent(objectValue(result).agent ?? result);
  }

  async deleteAgent(agentId: string, reassignTo = ''): Promise<void> {
    const query = reassignTo ? `?reassign_to=${id(reassignTo)}` : '';
    await this.json(`/api/agents/${id(agentId)}${query}`, 'DELETE');
  }

  async setAgentRunning(agentId: string, running: boolean): Promise<void> {
    await this.json(`/api/agents/${id(agentId)}/${running ? 'start' : 'stop'}`, 'POST');
  }

  async getExecLog(agentId: string): Promise<ExecEntry[]> {
    const result = await this.json(`/api/agents/${id(agentId)}/exec-log?limit=200`);
    return arrayValue(objectValue(result).entries).map((value) => {
      const item = objectValue(value);
      return {
        timestamp: typeof item.ts === 'string' ? item.ts : '',
        command: typeof item.command === 'string' ? item.command : '',
        exitCode: typeof item.exit_code === 'number' ? item.exit_code : -1,
        timedOut: item.timed_out === true,
        durationMs: typeof item.duration_ms === 'number' ? item.duration_ms : 0,
      };
    });
  }

  async startScreen(agentId: string): Promise<void> {
    await this.json(`/api/agents/${id(agentId)}/screen/start`, 'POST', undefined, 120_000);
  }

  async screenRunning(agentId: string): Promise<boolean> {
    const result = await this.json(`/api/agents/${id(agentId)}/screen`);
    return objectValue(result).running === true;
  }

  screenUrl(agentId: string, viewOnly: boolean): string {
    const { server, key } = this.connection();
    return `${server.replace(/\/+$/, '')}/screen/${id(agentId)}/?key=${id(key)}${viewOnly ? '&view_only=1' : ''}`;
  }

  async getAgentSkills(agentId: string): Promise<AgentSkills> {
    const agentPath = `/api/agents/${id(agentId)}`;
    const [global, enabled, local] = await Promise.all([
      this.json('/api/skills'), this.json(`${agentPath}/global-skills`), this.json(`${agentPath}/skills`),
    ]);
    const enabledNames = new Set(arrayValue(objectValue(enabled).skills)
      .filter((value): value is string => typeof value === 'string'));
    const shared = arrayValue(objectValue(global).skills).map((value) => {
      const item = objectValue(value);
      const name = typeof item.name === 'string' ? item.name : '';
      return {
        name, description: typeof item.description === 'string' ? item.description : '',
        content: '', version: typeof item.version === 'string' ? item.version : '',
        enabled: enabledNames.has(name),
      };
    }).filter((skill) => skill.name);
    return {
      shared,
      local: arrayValue(objectValue(local).skills).filter((value): value is string => typeof value === 'string'),
    };
  }

  async setSharedSkillEnabled(agentId: string, name: string, enabled: boolean): Promise<void> {
    await this.json(`/api/agents/${id(agentId)}/global-skills`, 'POST', { name, enabled });
  }

  async createGlobalSkill(input: SkillInput): Promise<void> {
    await this.json('/api/skills', 'POST', input);
  }

  async createAgentSkill(agentId: string, input: SkillInput): Promise<void> {
    await this.json(`/api/agents/${id(agentId)}/skills`, 'POST', input);
  }

  async deleteGlobalSkill(name: string): Promise<void> {
    await this.json(`/api/skills/${id(name)}`, 'DELETE');
  }

  async deleteAgentSkill(agentId: string, name: string): Promise<void> {
    await this.json(`/api/agents/${id(agentId)}/skills/${id(name)}`, 'DELETE');
  }

  async clearThread(threadId: string): Promise<void> {
    await this.json(`/api/threads/${id(threadId)}/clear`, 'POST');
  }

  async deleteThread(threadId: string): Promise<void> {
    await this.json(`/api/threads/${id(threadId)}`, 'DELETE');
  }

  async getMessages(threadId: string, afterId = '', beforeId = '') {
    const query = afterId ? `?after=${id(afterId)}` : beforeId ? `?before=${id(beforeId)}` : '';
    return normalizeMessagePage(await this.json(`/api/threads/${id(threadId)}/messages${query}`));
  }

  async getActivity(threadId: string) {
    return normalizeActivity(await this.json(`/api/threads/${id(threadId)}/activity`));
  }

  async sendMessage(threadId: string, body: string, source: 'user' | 'voice', attachments: Attachment[] = []): Promise<void> {
    const payload: Record<string, unknown> = { body, source };
    if (attachments.length) payload.attachments = attachments.map((attachment) => ({
      name: attachment.name, path: attachment.path, host_path: attachment.hostPath, size: attachment.size, worker: attachment.worker ?? '',
    }));
    await this.json(`/api/threads/${id(threadId)}/messages`, 'POST', payload);
  }

  async cancelThread(threadId: string): Promise<void> {
    await this.json(`/api/threads/${id(threadId)}/cancel`, 'POST');
  }

  async markThreadRead(threadId: string): Promise<void> {
    await this.json(`/api/threads/${id(threadId)}/read`, 'POST');
  }

  async download(threadId: string, attachment: Attachment): Promise<AttachmentPayload> {
    let relative = attachment.path;
    if (relative.startsWith('/workspace/')) relative = relative.slice('/workspace/'.length);
    if (!relative) relative = `attachments/${threadId}/${attachment.name}`;
    const worker = attachment.worker ? `&worker=${id(attachment.worker)}` : '';
    const { server, key } = this.connection();
    const response = await requestBytes(server,
      `/api/files/download?path=${id(relative)}${worker}`, key, {}, 120_000);
    if (response.status < 200 || response.status >= 300) {
      throw responseError(response.status, response.bytes, 'Download failed');
    }
    if (response.bytes.byteLength > 50 * 1024 * 1024) throw new Error('Attachment exceeds the 50 MB preview limit');
    return {
      bytes: response.bytes,
      mimeType: response.contentType || 'application/octet-stream',
    };
  }

  async getRoutines(): Promise<Routine[]> {
    const result = await this.json('/api/routines');
    return arrayValue(objectValue(result).routines).map(normalizeRoutine)
      .filter((routine) => routine.id.length > 0);
  }

  async createRoutine(input: RoutineInput): Promise<Routine> {
    const result = await this.json('/api/routines', 'POST', {
      agent_id: input.agentId, name: input.name, prompt: input.prompt, schedule: input.schedule,
    });
    return normalizeRoutine(result);
  }

  async updateRoutine(routineId: string, input: RoutineInput): Promise<Routine> {
    const result = await this.json(`/api/routines/${id(routineId)}`, 'PATCH', {
      name: input.name, prompt: input.prompt, schedule: input.schedule,
    });
    return normalizeRoutine(result);
  }

  async setRoutineEnabled(routineId: string, enabled: boolean): Promise<void> {
    await this.json(`/api/routines/${id(routineId)}/enabled`, 'POST', { enabled });
  }

  async runRoutine(routineId: string): Promise<void> {
    await this.json(`/api/routines/${id(routineId)}/run`, 'POST');
  }

  async deleteRoutine(routineId: string): Promise<void> {
    await this.json(`/api/routines/${id(routineId)}`, 'DELETE');
  }

  async testConnection(server: string, key: string): Promise<void> {
    await requestJson(server, '/api/agents', key);
  }

  async upload(threadId: string, filename: string, bytes: Buffer): Promise<Attachment> {
    if (bytes.length > 10 * 1024 * 1024) throw new Error('File too large (10 MB maximum)');
    const { server, key } = this.connection();
    const response = await requestBytes(server,
      `/api/threads/${id(threadId)}/attachments?name=${id(filename)}`, key,
      { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' },
        body: Uint8Array.from(bytes).buffer }, 60_000);
    if (response.status < 200 || response.status >= 300) {
      throw responseError(response.status, response.bytes, 'Upload failed');
    }
    const parsed = JSON.parse(new TextDecoder().decode(response.bytes)) as unknown;
    const item = objectValue(parsed);
    return {
      name: typeof item.name === 'string' ? item.name : filename,
      path: typeof item.path === 'string' ? item.path : '',
      hostPath: typeof item.host_path === 'string' ? item.host_path : '',
      size: typeof item.size === 'number' ? item.size : bytes.length,
    };
  }
}
