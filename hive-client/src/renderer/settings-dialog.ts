import type { AppSettings, SettingsView, VoiceAssignments } from '../shared/contracts.js';
import { avatarFor, avatarNames, userAvatar } from './avatars.js';
import { byId, element, errorMessage, replaceChildren } from './dom.js';
import type { SpeechPlayer } from './speech-player.js';
import { state } from './state.js';

interface SettingsCallbacks {
  onSaved: (settings: AppSettings) => void;
  onClosed: () => void;
}

export class SettingsDialog {
  private readonly dialog = byId<HTMLDialogElement>('settings-dialog');
  private readonly modelSelect = byId<HTMLSelectElement>('speech-model');
  private selectedModel = 'kokoro-82m';
  private catalogGeneration = 0;
  private selectedAvatar = '';

  constructor(private readonly speech: SpeechPlayer, private readonly callbacks: SettingsCallbacks) {
    byId('settings-button').addEventListener('click', () => this.open());
    byId('settings-close').addEventListener('click', () => this.close());
    byId('settings-cancel').addEventListener('click', () => this.close());
    byId('interface-tab').addEventListener('click', () => this.selectTab('interface'));
    byId('user-tab').addEventListener('click', () => this.selectTab('user'));
    byId('chimpflow-tab').addEventListener('click', () => this.selectTab('chimpflow'));
    this.modelSelect.addEventListener('change', () => {
      this.selectedModel = this.modelSelect.value;
      void this.loadCatalog(this.selectedModel);
    });
    byId<HTMLFormElement>('settings-form').addEventListener('submit', (event) => {
      event.preventDefault();
      void this.save();
    });
    this.dialog.addEventListener('cancel', () => {
      ++this.catalogGeneration;
      this.callbacks.onClosed();
    });
    const avatarPicker = byId('user-avatar');
    const choices = [{ id: '', name: 'Default user avatar' }, ...avatarNames.map((name) => ({ id: name, name }))];
    for (const choice of choices) {
      const image = element('img');
      image.alt = '';
      image.src = choice.id ? avatarFor(choice.name, choice.id) : userAvatar('');
      const button = element('button', { className: 'avatar-choice', ariaLabel: choice.name });
      button.type = 'button';
      button.setAttribute('role', 'radio');
      button.setAttribute('aria-checked', 'false');
      button.append(image);
      button.addEventListener('click', () => this.chooseAvatar(choice.id));
      button.dataset.avatar = choice.id;
      avatarPicker.append(button);
    }
    byId<HTMLInputElement>('font-scale').addEventListener('input', () => this.previewFontScale());
    byId<HTMLInputElement>('font-scale').addEventListener('change', () => { void this.saveFontScale(); });
  }

  open(): void {
    const settings = state.settings;
    byId<HTMLInputElement>('display-name').value = settings.displayName;
    this.chooseAvatar(settings.userAvatar);
    byId<HTMLInputElement>('font-scale').value = String(settings.fontScale);
    byId('settings-server').textContent = settings.hubServer;
    this.previewFontScale();
    const endpoint = new URL(settings.chimpflowUrl);
    byId<HTMLInputElement>('chimpflow-address').value = endpoint.hostname.replace(/^\[|\]$/g, '');
    byId<HTMLInputElement>('chimpflow-port').value = endpoint.port || '8090';
    this.selectedModel = settings.chimpflowModel;
    byId('settings-error').textContent = '';
    this.selectTab('user');
    this.dialog.showModal();
    void this.loadKeyAndCatalog();
  }

  close(): void {
    ++this.catalogGeneration;
    this.dialog.close();
    this.callbacks.onClosed();
  }

  applySettings(view: SettingsView): void {
    const { chimpflowKey: _chimpflowKey, ...settings } = view;
    state.settings = settings;
    this.callbacks.onSaved(settings);
  }

  private async loadKeyAndCatalog(): Promise<void> {
    try {
      const view = await window.hive.getSettings();
      if (!this.dialog.open) return;
      byId<HTMLInputElement>('chimpflow-key').value = view.chimpflowKey;
      await this.loadCatalog(this.selectedModel);
    } catch (error) {
      byId('catalog-status').textContent = errorMessage(error);
    }
  }

  private selectTab(tab: 'interface' | 'user' | 'chimpflow'): void {
    for (const name of ['interface', 'user', 'chimpflow'] as const) {
      byId(`${name}-settings`).hidden = tab !== name;
      byId(`${name}-tab`).setAttribute('aria-selected', String(tab === name));
    }
  }

  private chooseAvatar(avatar: string): void {
    this.selectedAvatar = avatar;
    for (const button of byId('user-avatar').querySelectorAll<HTMLButtonElement>('button')) {
      button.setAttribute('aria-checked', String((button.dataset.avatar ?? '') === avatar));
    }
  }

  private previewFontScale(): void {
    const scale = Number(byId<HTMLInputElement>('font-scale').value);
    byId<HTMLOutputElement>('font-scale-output').value = `${Math.round(scale * 100)}%`;
    document.documentElement.style.fontSize = `${14 * scale}px`;
  }

  private async saveFontScale(): Promise<void> {
    try {
      this.applySettings(await window.hive.saveSettings({ fontScale: Number(byId<HTMLInputElement>('font-scale').value) }));
    } catch (error) { byId('settings-error').textContent = errorMessage(error); }
  }

  private async loadCatalog(model: string): Promise<void> {
    const generation = ++this.catalogGeneration;
    this.modelSelect.disabled = true;
    byId('catalog-status').textContent = `Loading voices for ${model}…`;
    try {
      const catalog = await window.hive.getChimpFlowCatalog(model);
      if (generation !== this.catalogGeneration || !this.dialog.open) return;
      this.selectedModel = catalog.model;
      this.modelSelect.replaceChildren();
      if (!catalog.models.length) {
        this.modelSelect.append(new Option('Models unavailable', ''));
        this.modelSelect.disabled = true;
      } else {
        for (const modelId of catalog.models) this.modelSelect.append(new Option(modelId, modelId));
        this.modelSelect.value = catalog.model;
        this.modelSelect.disabled = false;
      }
      byId('catalog-status').textContent = catalog.error || `${catalog.voices.length} voices for ${catalog.model}`;
      this.renderVoiceAssignments(catalog.model, catalog.voices);
    } catch (error) {
      if (generation !== this.catalogGeneration) return;
      byId('catalog-status').textContent = errorMessage(error);
      this.renderVoiceAssignments(model, []);
    }
  }

  private renderVoiceAssignments(model: string, voices: string[]): void {
    const container = byId('voice-assignments');
    if (!state.agents.length) {
      replaceChildren(container, element('p', { className: 'help-text', text: 'No agents available' }));
      return;
    }
    const rows = state.agents.map((agent) => {
      const row = element('div', { className: 'voice-assignment' });
      const label = element('label', { text: agent.name });
      const selectId = `voice-${agent.id.replace(/[^a-zA-Z0-9_-]/g, '-')}`;
      label.htmlFor = selectId;
      const select = element('select', { ariaLabel: `Voice for ${agent.name}` });
      select.id = selectId;
      select.disabled = !voices.length;
      select.append(new Option('Default voice', ''));
      for (const voice of voices) select.append(new Option(voice, voice));
      select.value = state.settings.voices[model]?.[agent.id] ?? '';
      const preview = element('button', { text: 'Preview', ariaLabel: `Preview ${agent.name}` });
      preview.type = 'button';
      preview.disabled = !voices.length;
      select.addEventListener('change', () => {
        void this.assignVoice(agent.id, agent.name, model, select.value);
      });
      preview.addEventListener('click', () => {
        this.speech.speak(`Hi, I'm ${agent.name}.`, agent.id, `preview-${agent.id}`, model, select.value);
      });
      row.append(label, select, preview);
      return row;
    });
    replaceChildren(container, ...rows);
  }

  private async assignVoice(agentId: string, agentName: string, model: string, voice: string): Promise<void> {
    try {
      const voices: VoiceAssignments = structuredClone(state.settings.voices);
      voices[model] ??= {};
      voices[model][agentId] = voice;
      const updated = await window.hive.saveSettings({ voices });
      this.applySettings(updated);
      this.speech.speak(`Hi, I'm ${agentName}.`, agentId, `preview-${agentId}`, model, voice);
    } catch (error) {
      byId('settings-error').textContent = errorMessage(error);
    }
  }

  private async save(): Promise<void> {
    const address = byId<HTMLInputElement>('chimpflow-address').value.trim();
    const port = byId<HTMLInputElement>('chimpflow-port').value.trim();
    const host = address.includes(':') && !address.startsWith('[') ? `[${address}]` : address;
    const submit = byId<HTMLButtonElement>('settings-form').querySelector<HTMLButtonElement>('button[type=submit]')!;
    submit.disabled = true;
    byId('settings-error').textContent = '';
    try {
      const updated = await window.hive.saveSettings({
        displayName: byId<HTMLInputElement>('display-name').value.trim() || 'You',
        userAvatar: this.selectedAvatar,
        fontScale: Number(byId<HTMLInputElement>('font-scale').value),
        chimpflowUrl: `http://${host}:${port}`,
        chimpflowKey: byId<HTMLInputElement>('chimpflow-key').value,
        chimpflowModel: this.selectedModel,
      });
      this.applySettings(updated);
      this.close();
    } catch (error) {
      byId('settings-error').textContent = errorMessage(error);
    } finally {
      submit.disabled = false;
    }
  }
}
