import type { Agent, AgentModel, AgentOptions, NewAgentInput } from '../shared/contracts.js';
import { avatarFor, avatarNames } from './avatars.js';
import { byId, element, errorMessage } from './dom.js';
import { state } from './state.js';

export class AgentEditor {
  private existing: Agent | undefined;
  private options: AgentOptions = { models: [], defaultModel: '', templates: [] };
  private selectedAvatar = '';
  private generation = 0;

  constructor(private readonly onSaved: () => Promise<void>) {
    byId('new-agent-cancel').addEventListener('click', () => this.close());
    byId<HTMLFormElement>('new-agent-form').addEventListener('submit', (event) => {
      event.preventDefault();
      void this.save();
    });
    byId<HTMLSelectElement>('agent-provider').addEventListener('change', () => this.refillModels());
    byId<HTMLSelectElement>('agent-worker').addEventListener('change', () => this.rebuildTypes());
    byId<HTMLSelectElement>('agent-type').addEventListener('change', () => this.refreshTypeUi());
    byId<HTMLSelectElement>('agent-template').addEventListener('change', () => this.applyTemplate());
    const picker = byId('agent-avatar-picker');
    const choices = [{ id: '', name: 'Auto avatar' }, ...avatarNames.map((name) => ({ id: name, name }))];
    for (const choice of choices) {
      const button = element('button', { className: 'avatar-choice', ariaLabel: choice.name });
      button.type = 'button';
      button.setAttribute('role', 'radio');
      button.dataset.avatar = choice.id;
      const image = element('img');
      image.alt = '';
      image.src = avatarFor(choice.name, choice.id);
      button.append(image);
      button.addEventListener('click', () => this.chooseAvatar(choice.id));
      picker.append(button);
    }
  }

  open(agent?: Agent): void {
    this.existing = agent;
    ++this.generation;
    byId<HTMLFormElement>('new-agent-form').reset();
    byId('new-agent-title').textContent = agent ? `Edit ${agent.name}` : 'New agent';
    byId<HTMLButtonElement>('agent-submit').textContent = agent ? 'Save' : 'Create agent';
    byId<HTMLInputElement>('agent-name').value = agent?.name ?? '';
    byId<HTMLTextAreaElement>('agent-role').value = agent?.role ?? '';
    byId<HTMLInputElement>('agent-memory').value = agent?.memory ?? '';
    byId<HTMLInputElement>('agent-cpus').value = agent && agent.cpus > 0 ? String(agent.cpus) : '';
    byId<HTMLSelectElement>('agent-thinking').value = agent?.thinkingLevel || '';
    byId<HTMLSelectElement>('agent-template').replaceChildren(new Option('(blank)', ''));
    byId('agent-template-field').hidden = Boolean(agent);
    byId('new-agent-error').textContent = '';
    this.chooseAvatar(agent?.avatar ?? '');
    this.populateWorkers();
    this.rebuildTypes();
    byId<HTMLDialogElement>('new-agent-dialog').showModal();
    void this.loadOptions(++this.generation, agent);
  }

  private close(): void {
    ++this.generation;
    byId<HTMLDialogElement>('new-agent-dialog').close();
  }

  private populateWorkers(): void {
    const worker = byId<HTMLSelectElement>('agent-worker');
    const current = this.existing?.worker ?? '';
    worker.replaceChildren(new Option('Hub default', ''));
    for (const item of state.workers) {
      if (!item.online && item.name !== current) continue;
      worker.append(new Option(`${item.name}${item.online ? '' : ' (offline)'}`, item.name));
    }
    if (current && !Array.from(worker.options).some((option) => option.value === current)) {
      worker.append(new Option(`${current} (offline)`, current));
    }
    worker.value = current || state.workers.find((item) => item.online)?.name || '';
  }

  private async loadOptions(generation: number, agent?: Agent): Promise<void> {
    const submit = byId<HTMLButtonElement>('agent-submit');
    submit.disabled = true;
    try {
      this.options = await window.hive.getAgentOptions();
      if (generation !== this.generation) return;
      const template = byId<HTMLSelectElement>('agent-template');
      template.replaceChildren(new Option('(blank)', ''),
        ...this.options.templates.map((item) => new Option(`${item.name} — template`, item.name)));
      const providers = [...new Set(this.options.models.map((model) => model.provider).filter(Boolean))].sort();
      const currentModel = agent?.model ?? '';
      if (currentModel && !this.options.models.some((model) => model.name === currentModel)) {
        this.options.models.push({ name: currentModel, provider: 'custom' });
        providers.push('custom');
      }
      const provider = byId<HTMLSelectElement>('agent-provider');
      provider.replaceChildren(...providers.map((name) => new Option(name, name)));
      const currentProvider = this.options.models.find((model) => model.name === currentModel)?.provider;
      if (currentProvider) provider.value = currentProvider;
      this.refillModels(currentModel);
      this.rebuildTypes();
      byId('new-agent-error').textContent = '';
    } catch (error) {
      if (generation === this.generation) byId('new-agent-error').textContent = `Could not load model options: ${errorMessage(error)}`;
    } finally {
      if (generation === this.generation) submit.disabled = false;
    }
  }

  private refillModels(selected = ''): void {
    const provider = byId<HTMLSelectElement>('agent-provider').value;
    const model = byId<HTMLSelectElement>('agent-model');
    const defaultLabel = this.options.defaultModel ? `(hub default: ${this.options.defaultModel})` : '(hub default)';
    const models = this.options.models.filter((item) => item.provider === provider);
    model.replaceChildren(new Option(defaultLabel, ''), ...models.map((item: AgentModel) => new Option(`${item.name} (${item.provider})`, item.name)));
    model.value = selected;
    if (model.selectedIndex < 0) model.value = '';
  }

  private rebuildTypes(): void {
    const workerName = byId<HTMLSelectElement>('agent-worker').value;
    const worker = state.workers.find((item) => item.name === workerName);
    const type = byId<HTMLSelectElement>('agent-type');
    const previous = this.existing?.hostAgent ? 'host' : this.existing?.type || '';
    type.replaceChildren(new Option('Standard container', ''));
    if (worker && Object.hasOwn(worker.caps, 'gpu')) type.append(new Option('GPU container (render/compute)', 'gpu'));
    if ((worker && Object.hasOwn(worker.caps, 'host_agents')) || this.existing?.hostAgent) {
      type.append(new Option('Bare-metal ⚠ (host — sudo, NO sandbox)', 'host'));
    }
    type.value = previous;
    if (type.selectedIndex < 0) type.value = '';
    this.refreshTypeUi();
  }

  private refreshTypeUi(): void {
    const host = byId<HTMLSelectElement>('agent-type').value === 'host';
    byId('agent-host-warning').hidden = !host;
    byId('agent-memory-field').hidden = host;
    byId('agent-cpus-field').hidden = host;
  }

  private applyTemplate(): void {
    const name = byId<HTMLSelectElement>('agent-template').value;
    const template = this.options.templates.find((item) => item.name === name);
    if (!template) return;
    byId<HTMLInputElement>('agent-name').value = template.name;
    byId<HTMLTextAreaElement>('agent-role').value = template.role;
    this.chooseAvatar(template.avatar);
  }

  private chooseAvatar(avatar: string): void {
    this.selectedAvatar = avatar;
    for (const button of document.querySelectorAll<HTMLButtonElement>('#agent-avatar-picker [data-avatar]')) {
      button.setAttribute('aria-checked', String(button.dataset.avatar === avatar));
    }
  }

  private input(): NewAgentInput {
    const existing = this.existing;
    const selectedType = byId<HTMLSelectElement>('agent-type').value;
    const host = selectedType === 'host';
    let type = selectedType === 'host' ? '' : selectedType;
    if (!host && !selectedType && existing?.type) type = 'default';
    let memory = byId<HTMLInputElement>('agent-memory').value.trim();
    let cpus = -1;
    const cpuText = byId<HTMLInputElement>('agent-cpus').value.trim();
    if (cpuText) cpus = Number(cpuText);
    else if (existing && existing.cpus > 0) cpus = 0;
    if (host) {
      if (existing?.memory) memory = 'default';
      if (existing && existing.cpus > 0) cpus = 0;
    } else if (!memory && existing?.memory) memory = 'default';
    let thinkingLevel = byId<HTMLSelectElement>('agent-thinking').value;
    if (existing && !thinkingLevel && existing.thinkingLevel) thinkingLevel = 'default';
    return {
      name: byId<HTMLInputElement>('agent-name').value.trim(),
      role: byId<HTMLTextAreaElement>('agent-role').value,
      model: byId<HTMLSelectElement>('agent-model').value,
      worker: byId<HTMLSelectElement>('agent-worker').value,
      avatar: this.selectedAvatar, type, memory, cpus, thinkingLevel,
      ...(host ? { hostAgent: true } : existing?.hostAgent ? { hostAgent: false } : {}),
    };
  }

  private async save(): Promise<void> {
    const submit = byId<HTMLButtonElement>('agent-submit');
    submit.disabled = true;
    try {
      const input = this.input();
      if (this.existing) await window.hive.updateAgent(this.existing.id, input);
      else await window.hive.createAgent(input);
      this.close();
      byId<HTMLDialogElement>('agent-manage-dialog').close();
      await this.onSaved();
    } catch (error) {
      byId('new-agent-error').textContent = errorMessage(error);
    } finally { submit.disabled = false; }
  }
}
