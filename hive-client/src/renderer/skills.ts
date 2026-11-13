import type { Agent, AgentSkills } from '../shared/contracts.js';
import type { ConfirmationDialog } from './confirmation.js';
import { byId, element, errorMessage, replaceChildren } from './dom.js';

export class SkillsDialog {
  private agent: Agent | undefined;
  private generation = 0;

  constructor(
    private readonly confirmation: ConfirmationDialog,
    private readonly onStatus: (message: string) => void,
  ) {
    byId('skills-close').addEventListener('click', () => byId<HTMLDialogElement>('skills-dialog').close());
    byId('new-skill-button').addEventListener('click', () => this.openEditor());
    byId('skill-editor-cancel').addEventListener('click', () => byId<HTMLDialogElement>('skill-editor-dialog').close());
    byId<HTMLFormElement>('skill-editor-form').addEventListener('submit', (event) => {
      event.preventDefault();
      void this.createSkill();
    });
  }

  open(agent: Agent): void {
    this.agent = agent;
    byId('skills-title').textContent = `Skills — ${agent.name}`;
    byId('skills-error').textContent = '';
    byId<HTMLDialogElement>('skills-dialog').showModal();
    void this.load();
  }

  private async load(): Promise<void> {
    if (!this.agent) return;
    const agentId = this.agent.id;
    const generation = ++this.generation;
    byId('skills-status').textContent = 'Loading…';
    byId('shared-skill-list').replaceChildren();
    byId('local-skill-list').replaceChildren();
    try {
      const skills = await window.hive.getAgentSkills(agentId);
      if (generation !== this.generation || this.agent?.id !== agentId) return;
      this.render(skills);
    } catch (error) {
      if (generation === this.generation) byId('skills-status').textContent = errorMessage(error);
    }
  }

  private render(skills: AgentSkills): void {
    if (!this.agent) return;
    const agent = this.agent;
    const shared = skills.shared.map((skill) => {
      const row = element('article', { className: 'skill-row' });
      const copy = element('div', { className: 'row-copy' });
      copy.append(element('h4', { text: `${skill.name}${skill.version ? ` · v${skill.version}` : ''}` }),
        element('p', { className: 'help-text', text: skill.description }));
      const enabledLabel = element('label', { className: 'check-row' });
      const enabled = element('input');
      enabled.type = 'checkbox';
      enabled.checked = skill.enabled;
      enabled.setAttribute('aria-label', `Enable ${skill.name} for ${agent.name}`);
      enabled.addEventListener('change', () => {
        void (async () => {
          enabled.disabled = true;
          try {
            await window.hive.setSharedSkillEnabled(agent.id, skill.name, enabled.checked);
            this.onStatus(`${enabled.checked ? 'Enabled' : 'Disabled'} ${skill.name} for ${agent.name}`);
          } catch (error) {
            enabled.checked = !enabled.checked;
            byId('skills-error').textContent = errorMessage(error);
          } finally { enabled.disabled = false; }
        })();
      });
      enabledLabel.append(enabled, document.createTextNode('Enabled'));
      const remove = element('button', { className: 'quiet danger-text', text: 'Delete', ariaLabel: `Delete shared skill ${skill.name}` });
      remove.addEventListener('click', () => { void this.deleteShared(skill.name); });
      const actions = element('div', { className: 'skill-actions' });
      actions.append(enabledLabel, remove);
      row.append(copy, actions);
      return row;
    });
    if (!shared.length) shared.push(element('p', { className: 'help-text', text: 'The shared library is empty.' }));
    const local = skills.local.map((name) => {
      const row = element('article', { className: 'skill-row' });
      row.append(element('h4', { text: name }));
      const remove = element('button', { className: 'quiet danger-text', text: 'Delete', ariaLabel: `Delete learned skill ${name}` });
      remove.addEventListener('click', () => { void this.deleteLocal(name); });
      row.append(remove);
      return row;
    });
    if (!local.length) local.push(element('p', { className: 'help-text', text: `No skills learned by ${agent.name} yet.` }));
    replaceChildren(byId('shared-skill-list'), ...shared);
    replaceChildren(byId('local-skill-list'), ...local);
    byId('skills-status').textContent = `${skills.shared.length} shared · ${skills.local.length} learned`;
  }

  private openEditor(): void {
    byId<HTMLFormElement>('skill-editor-form').reset();
    const scope = byId<HTMLSelectElement>('skill-scope');
    scope.replaceChildren(new Option('Shared library', 'shared'));
    if (this.agent) scope.append(new Option(`Learned by ${this.agent.name}`, 'agent'));
    byId('skill-editor-error').textContent = '';
    byId<HTMLDialogElement>('skill-editor-dialog').showModal();
  }

  private async createSkill(): Promise<void> {
    if (!this.agent) return;
    const button = byId<HTMLButtonElement>('skill-create-confirm');
    button.disabled = true;
    const input = {
      name: byId<HTMLInputElement>('skill-name').value,
      description: byId<HTMLInputElement>('skill-description').value,
      content: byId<HTMLTextAreaElement>('skill-content').value,
    };
    try {
      if (byId<HTMLSelectElement>('skill-scope').value === 'agent') await window.hive.createAgentSkill(this.agent.id, input);
      else await window.hive.createGlobalSkill(input);
      byId<HTMLDialogElement>('skill-editor-dialog').close();
      this.onStatus(`Created skill ${input.name}`);
      await this.load();
    } catch (error) {
      byId('skill-editor-error').textContent = errorMessage(error);
    } finally { button.disabled = false; }
  }

  private async deleteShared(name: string): Promise<void> {
    if (!await this.confirmation.ask('Delete shared skill?', `Delete ${name} from the shared library for every agent? This cannot be undone.`, 'Delete')) return;
    try {
      await window.hive.deleteGlobalSkill(name);
      this.onStatus(`Deleted shared skill ${name}`);
      await this.load();
    } catch (error) { byId('skills-error').textContent = errorMessage(error); }
  }

  private async deleteLocal(name: string): Promise<void> {
    if (!this.agent) return;
    if (!await this.confirmation.ask('Delete learned skill?', `Delete ${name} from ${this.agent.name}? This cannot be undone.`, 'Delete')) return;
    try {
      await window.hive.deleteAgentSkill(this.agent.id, name);
      this.onStatus(`Deleted learned skill ${name}`);
      await this.load();
    } catch (error) { byId('skills-error').textContent = errorMessage(error); }
  }
}
