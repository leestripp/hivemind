import type { Agent } from '../shared/contracts.js';
import { byId, element, errorMessage } from './dom.js';
import { state } from './state.js';

export class AgentManager {
  private agent?: Agent;
  private deleting: Agent | undefined;

  constructor(
    private readonly onStatus: (message: string) => void,
    private readonly onChanged: () => void,
    private readonly onDeleted: (agent: Agent) => void,
    private readonly onEdit: (agent: Agent) => void,
    private readonly onSchedule: (agent: Agent) => void,
    private readonly onSkills: (agent: Agent) => void,
  ) {
    byId('agent-manage-close').addEventListener('click', () => byId<HTMLDialogElement>('agent-manage-dialog').close());
    byId('agent-cancel-button').addEventListener('click', () => { void this.cancelWork(); });
    byId('agent-inspector-button').addEventListener('click', () => this.openInspector());
    byId('agent-inspector-close').addEventListener('click', () => byId<HTMLDialogElement>('agent-inspector-dialog').close());
    byId('agent-inspector-start').addEventListener('click', () => { void this.setFromInspector(true); });
    byId('agent-inspector-stop').addEventListener('click', () => { void this.setFromInspector(false); });
    byId('agent-edit-button').addEventListener('click', () => { if (this.agent) this.onEdit(this.agent); });
    byId('agent-skills-button').addEventListener('click', () => { if (this.agent) this.onSkills(this.agent); });
    byId('agent-schedule-button').addEventListener('click', () => { if (this.agent) this.onSchedule(this.agent); });
    byId('agent-toggle-running').addEventListener('click', () => { void this.toggleRunning(); });
    byId('agent-screen-button').addEventListener('click', () => { void this.openScreen(false); });
    byId('agent-screen-view-button').addEventListener('click', () => { void this.openScreen(true); });
    byId('agent-exec-button').addEventListener('click', () => { void this.openExecLog(); });
    byId('agent-delete-button').addEventListener('click', () => { void this.openDelete(); });
    byId('exec-log-close').addEventListener('click', () => byId<HTMLDialogElement>('exec-log-dialog').close());
    byId('exec-log-refresh').addEventListener('click', () => { void this.loadExecLog(); });
    byId('agent-delete-cancel').addEventListener('click', () => byId<HTMLDialogElement>('agent-delete-dialog').close());
    byId<HTMLFormElement>('agent-delete-form').addEventListener('submit', (event) => {
      event.preventDefault();
      void this.confirmDelete();
    });
  }

  open(agent: Agent): void {
    this.agent = agent;
    byId('agent-manage-title').textContent = agent.name;
    byId('agent-manage-subtitle').textContent = `${agent.worker || 'hub'} · ${agent.state}${agent.busy ? ' · working' : ''}`;
    byId<HTMLButtonElement>('agent-toggle-running').textContent = agent.state === 'ready' || agent.busy ? 'Stop container' : 'Start container';
    byId('agent-manage-error').textContent = '';
    byId<HTMLDialogElement>('agent-manage-dialog').showModal();
  }

  private async cancelWork(): Promise<void> {
    if (!this.agent) return;
    const button = byId<HTMLButtonElement>('agent-cancel-button');
    button.disabled = true;
    try {
      const cancelled = await window.hive.cancelAgent(this.agent.id);
      this.onStatus(cancelled > 0 ? `Cancel sent to ${this.agent.name}` : `${this.agent.name} wasn’t working`);
    } catch (error) {
      byId('agent-manage-error').textContent = errorMessage(error);
    } finally { button.disabled = false; }
  }

  private openInspector(): void {
    if (!this.agent) return;
    const agent = this.agent;
    byId('agent-inspector-title').textContent = `Inspector — ${agent.name}`;
    const details = byId('agent-inspector-details');
    details.replaceChildren();
    const add = (key: string, value: string) => {
      details.append(element('dt', { text: key }), element('dd', { text: value || '—' }));
    };
    add('Model', agent.model);
    add('Workspace mode', agent.workspaceMode);
    add('Worker', agent.worker);
    add('Thinking level', agent.thinkingLevel || 'Inherit from model');
    add('Memory', agent.memory || 'Worker default');
    add('CPUs', agent.cpus > 0 ? String(agent.cpus) : 'Worker default');
    add('Created', agent.createdAt);
    add('Updated', agent.updatedAt);
    add('Data directory', `~/.local/share/hivemind/agents/${agent.id}/`);
    byId('agent-inspector-host').hidden = !agent.hostAgent;
    const running = agent.state !== 'down' && agent.state !== 'crashed' && Boolean(agent.state);
    byId('agent-inspector-state').textContent = `${running ? '●' : '○'} ${agent.state || 'down'}`;
    byId<HTMLButtonElement>('agent-inspector-start').disabled = running;
    byId<HTMLButtonElement>('agent-inspector-stop').disabled = !running;
    byId<HTMLDialogElement>('agent-inspector-dialog').showModal();
  }

  private async setFromInspector(running: boolean): Promise<void> {
    if (!this.agent) return;
    const agent = this.agent;
    const buttons = [byId<HTMLButtonElement>('agent-inspector-start'), byId<HTMLButtonElement>('agent-inspector-stop')];
    buttons.forEach((button) => { button.disabled = true; });
    try {
      await window.hive.setAgentRunning(agent.id, running);
      this.onStatus(`${running ? 'Started' : 'Stopped'} ${agent.name}`);
      byId<HTMLDialogElement>('agent-inspector-dialog').close();
      byId<HTMLDialogElement>('agent-manage-dialog').close();
      this.onChanged();
    } catch (error) {
      byId('agent-inspector-state').textContent = errorMessage(error);
      buttons.forEach((button) => { button.disabled = false; });
    }
  }

  private async toggleRunning(): Promise<void> {
    if (!this.agent) return;
    const button = byId<HTMLButtonElement>('agent-toggle-running');
    button.disabled = true;
    try {
      const running = this.agent.state !== 'ready' && !this.agent.busy;
      await window.hive.setAgentRunning(this.agent.id, running);
      this.onStatus(`${running ? 'Started' : 'Stopped'} ${this.agent.name}`);
      byId<HTMLDialogElement>('agent-manage-dialog').close();
      this.onChanged();
    } catch (error) {
      byId('agent-manage-error').textContent = errorMessage(error);
    } finally { button.disabled = false; }
  }

  private async openScreen(viewOnly: boolean): Promise<void> {
    if (!this.agent) return;
    const buttons = [byId<HTMLButtonElement>('agent-screen-button'), byId<HTMLButtonElement>('agent-screen-view-button')];
    buttons.forEach((button) => { button.disabled = true; });
    byId('agent-manage-error').textContent = 'Starting the agent desktop…';
    try {
      await window.hive.openAgentScreen(this.agent.id, viewOnly);
      byId('agent-manage-error').textContent = '';
      this.onStatus(`Opened ${this.agent.name}'s desktop${viewOnly ? ' (view only)' : ''}`);
    } catch (error) {
      byId('agent-manage-error').textContent = errorMessage(error);
    } finally { buttons.forEach((button) => { button.disabled = false; }); }
  }

  private async openExecLog(): Promise<void> {
    byId<HTMLDialogElement>('exec-log-dialog').showModal();
    await this.loadExecLog();
  }

  private async loadExecLog(): Promise<void> {
    if (!this.agent) return;
    byId('exec-log-title').textContent = `Exec log — ${this.agent.name}`;
    byId('exec-log-status').textContent = 'Loading…';
    byId('exec-log-content').textContent = '';
    try {
      const entries = await window.hive.getExecLog(this.agent.id);
      const lines = entries.map((entry) => {
        const outcome = entry.timedOut ? 'timeout' : `exit ${entry.exitCode}`;
        return `${entry.timestamp}  ${outcome}  ${entry.durationMs}ms\n  $ ${entry.command}`;
      });
      byId('exec-log-content').textContent = lines.join('\n\n');
      byId('exec-log-status').textContent = `${entries.length} ${entries.length === 1 ? 'entry' : 'entries'} — newest first`;
    } catch (error) {
      byId('exec-log-status').textContent = errorMessage(error);
    }
  }

  private async openDelete(): Promise<void> {
    if (!this.agent) return;
    this.deleting = this.agent;
    const agent = this.agent;
    const owned = state.threads.filter((thread) => thread.agentIds.includes(agent.id));
    const solo = owned.filter((thread) => thread.agentIds.length <= 1).length;
    const groups = owned.length - solo;
    byId('agent-delete-name').textContent = agent.name;
    byId('agent-delete-detail').textContent = 'The roster entry is removed. Conversation history and the workspace stay on disk.'
      + (owned.length ? ` ${solo} private thread(s) will be deleted; ${groups} group thread(s) continue without ${agent.name}.` : '');
    byId('agent-delete-error').textContent = '';
    const reassignment = byId<HTMLSelectElement>('agent-reassign');
    reassignment.replaceChildren(new Option('Delete scheduled tasks', ''),
      ...state.agents.filter((item) => item.id !== agent.id).map((item) => new Option(`Reassign to ${item.name}`, item.id)));
    byId('agent-reassign-field').hidden = true;
    byId<HTMLDialogElement>('agent-delete-dialog').showModal();
    try {
      const routines = (await window.hive.getRoutines()).filter((routine) => routine.agentId === agent.id);
      if (this.deleting?.id !== agent.id) return;
      byId('agent-reassign-label').textContent = `${routines.length} scheduled task(s)`;
      byId('agent-reassign-field').hidden = routines.length === 0 || state.agents.length < 2;
    } catch (error) {
      byId('agent-delete-error').textContent = `Could not check scheduled tasks: ${errorMessage(error)}`;
    }
  }

  private async confirmDelete(): Promise<void> {
    if (!this.deleting) return;
    const agent = this.deleting;
    const button = byId<HTMLButtonElement>('agent-delete-confirm');
    button.disabled = true;
    try {
      await window.hive.deleteAgent(agent.id, byId<HTMLSelectElement>('agent-reassign').value);
      byId<HTMLDialogElement>('agent-delete-dialog').close();
      byId<HTMLDialogElement>('agent-manage-dialog').close();
      this.deleting = undefined;
      this.onStatus(`Deleted ${agent.name}`);
      this.onDeleted(agent);
    } catch (error) {
      byId('agent-delete-error').textContent = errorMessage(error);
    } finally { button.disabled = false; }
  }
}
