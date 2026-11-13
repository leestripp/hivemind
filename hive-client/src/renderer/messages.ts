import type { Attachment, ChatMessage } from '../shared/contracts.js';
import { byId, element, errorMessage, formatBytes, formatTime } from './dom.js';
import type { SpeechPlayer } from './speech-player.js';
import { agentAvatar, userAvatar } from './avatars.js';
import { agentById, state } from './state.js';

type MediaKind = 'image' | 'video' | 'audio';

function attachmentKind(attachment: Attachment): MediaKind | undefined {
  const extension = attachment.name.toLowerCase().match(/\.[a-z0-9]+$/)?.[0] ?? '';
  if (['.png', '.jpg', '.jpeg', '.gif', '.webp'].includes(extension)) return 'image';
  if (['.mp4', '.webm', '.mov', '.m4v', '.ogv'].includes(extension)) return 'video';
  if (['.mp3', '.wav', '.ogg', '.oga', '.m4a', '.aac', '.flac', '.opus'].includes(extension)) return 'audio';
  return undefined;
}

function fallbackMime(attachment: Attachment, kind: MediaKind): string {
  const extension = attachment.name.toLowerCase().match(/\.[a-z0-9]+$/)?.[0] ?? '';
  const types: Record<string, string> = {
    '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.png': 'image/png', '.gif': 'image/gif', '.webp': 'image/webp',
    '.mp4': 'video/mp4', '.webm': 'video/webm', '.mov': 'video/mp4', '.m4v': 'video/mp4', '.ogv': 'video/ogg',
    '.mp3': 'audio/mpeg', '.wav': 'audio/wav', '.ogg': 'audio/ogg', '.oga': 'audio/ogg',
    '.m4a': 'audio/mp4', '.aac': 'audio/aac', '.flac': 'audio/flac', '.opus': 'audio/ogg',
  };
  return types[extension] ?? `${kind}/*`;
}

export class MessageRenderer {
  private renderedIds = new Set<string>();
  private objectUrls = new Set<string>();
  private generation = 0;

  constructor(
    private readonly speech: SpeechPlayer,
    private readonly onStatus: (message: string) => void,
  ) {
    byId('media-dialog-close').addEventListener('click', () => this.closeMediaDialog());
    byId<HTMLDialogElement>('media-dialog').addEventListener('close', () => {
      byId('media-dialog-content').replaceChildren();
    });
  }

  reset(): void {
    ++this.generation;
    this.renderedIds.clear();
    this.closeMediaDialog();
    for (const url of this.objectUrls) URL.revokeObjectURL(url);
    this.objectUrls.clear();
    byId('message-list').replaceChildren();
    byId('live-message').hidden = true;
  }

  append(messages: ChatMessage[]): void {
    const list = byId('message-list');
    for (const message of messages) {
      if (!message.id || this.renderedIds.has(message.id)) continue;
      this.renderedIds.add(message.id);
      list.append(this.buildMessage(message));
    }
    this.updateEmptyState();
  }

  updateLive(): void {
    const live = byId('live-message');
    const turn = state.activity.turns.find((item) => !item.done && item.partial && !item.cancelling);
    live.hidden = !turn;
    if (!turn) return;
    live.replaceChildren(element('div', { className: 'message-meta', text: `${turn.agentName} is working…` }),
      element('div', { className: 'message-body', text: turn.partial }));
  }

  resetSpeechButtons(): void {
    document.querySelectorAll<HTMLButtonElement>('.message-speak').forEach((button) => {
      button.textContent = 'Play';
      const id = button.dataset.speechKey ?? '';
      button.setAttribute('aria-label', `Play message${id ? ` from ${id}` : ''}`);
    });
  }

  private buildMessage(message: ChatMessage): HTMLLIElement {
    const agent = message.senderKind === 'agent' ? agentById(message.senderId) : undefined;
    const sender = message.senderKind === 'agent' ? agent?.name ?? message.senderName ?? 'Agent'
      : message.senderKind === 'user' ? state.settings.displayName : 'System';
    const item = element('li', { className: `message ${message.senderKind}-message` });
    item.dataset.messageId = message.id;
    const meta = element('div', { className: 'message-meta' });
    if (message.senderKind === 'agent' && agent) {
      const image = element('img', { className: 'avatar message-avatar' });
      image.src = agentAvatar(agent);
      image.alt = '';
      meta.append(image);
    } else if (message.senderKind === 'user') {
      const image = element('img', { className: 'avatar message-avatar' });
      image.src = userAvatar(state.settings.userAvatar);
      image.alt = '';
      meta.append(image);
    }
    meta.append(element('span', { className: 'message-sender', text: sender }),
      element('span', { text: formatTime(message.createdAt) }));
    if (message.senderKind === 'agent' && message.body.trim()) {
      const key = `message-${message.id}`;
      const play = element('button', { className: 'message-speak', text: 'Play', ariaLabel: `Play message from ${sender}` });
      play.dataset.speechKey = sender;
      play.addEventListener('click', () => {
        if (this.speech.activeKey === key) {
          this.speech.stop();
          return;
        }
        this.resetSpeechButtons();
        play.textContent = 'Stop';
        play.setAttribute('aria-label', `Stop message from ${sender}`);
        this.speech.speak(message.body, message.senderId, key);
      });
      meta.append(play);
    }
    item.append(meta);
    if (message.body) item.append(element('div', { className: 'message-body', text: message.body }));
    if (message.attachments.length) {
      const attachments = element('div', { className: 'message-attachments' });
      for (const attachment of message.attachments) attachments.append(this.buildAttachment(attachment));
      item.append(attachments);
    }
    return item;
  }

  private buildAttachment(attachment: Attachment): HTMLElement {
    const kind = attachmentKind(attachment);
    const container = element('div', { className: 'attachment' });
    const actions = element('div', { className: 'attachment-actions' });
    const size = formatBytes(attachment.size);
    const suffix = size ? ` · ${size}` : '';
    if (kind) {
      const preview = element('button', { className: 'attachment-chip', text: `▶ ${attachment.name}${suffix}`, ariaLabel: `Preview ${attachment.name}` });
      const holder = element('div', { className: 'attachment-preview' });
      holder.hidden = true;
      preview.addEventListener('click', () => {
        if (!holder.hidden) {
          holder.hidden = true;
          holder.querySelectorAll<HTMLMediaElement>('audio,video').forEach((media) => media.pause());
          preview.textContent = `▶ ${attachment.name}${suffix}`;
          return;
        }
        preview.disabled = true;
        preview.textContent = 'Loading…';
        void this.loadPreview(attachment, kind, holder).then(() => {
          holder.hidden = false;
          preview.textContent = `▼ ${attachment.name}${suffix}`;
        }).catch((error: unknown) => {
          preview.textContent = `▶ ${attachment.name}${suffix}`;
          this.onStatus(errorMessage(error));
        }).finally(() => { preview.disabled = false; });
      });
      actions.append(preview);
      container.append(actions, holder);
    } else {
      const download = element('button', { className: 'attachment-chip', text: `📎 ${attachment.name}${suffix}`, ariaLabel: `Download ${attachment.name}` });
      download.addEventListener('click', () => { void this.download(attachment, download); });
      actions.append(download);
      container.append(actions);
    }
    if (kind) {
      const download = element('button', { className: 'quiet attachment-download', text: 'Download', ariaLabel: `Download ${attachment.name}` });
      download.addEventListener('click', () => { void this.download(attachment, download); });
      actions.append(download);
    }
    return container;
  }

  private async loadPreview(attachment: Attachment, kind: MediaKind, holder: HTMLElement): Promise<void> {
    if (holder.childElementCount) return;
    const threadId = state.currentThreadId;
    if (!threadId) throw new Error('Select a thread before downloading an attachment');
    const generation = this.generation;
    const payload = await window.hive.getAttachment(threadId, attachment);
    if (generation !== this.generation || threadId !== state.currentThreadId) throw new Error('Attachment preview was cancelled');
    const mimeType = payload.mimeType === 'application/octet-stream' || !payload.mimeType
      ? fallbackMime(attachment, kind) : payload.mimeType;
    const url = URL.createObjectURL(new Blob([payload.bytes], { type: mimeType }));
    this.objectUrls.add(url);
    let media: HTMLElement;
    if (kind === 'image') {
      const image = element('img', { className: 'attachment-image', ariaLabel: attachment.name });
      image.alt = attachment.name;
      image.src = url;
      image.addEventListener('click', () => this.openImageDialog(attachment.name, url));
      media = image;
    } else {
      const player = document.createElement(kind);
      player.className = 'attachment-media';
      player.controls = true;
      player.preload = 'metadata';
      player.src = url;
      player.setAttribute('aria-label', attachment.name);
      media = player;
    }
    holder.replaceChildren(media);
  }

  private openImageDialog(name: string, url: string): void {
    byId('media-dialog-title').textContent = name;
    const image = element('img', { className: 'full-attachment-image', ariaLabel: name });
    image.alt = name;
    image.src = url;
    byId('media-dialog-content').replaceChildren(image);
    byId<HTMLDialogElement>('media-dialog').showModal();
  }

  private closeMediaDialog(): void {
    const dialog = byId<HTMLDialogElement>('media-dialog');
    if (dialog.open) dialog.close();
  }

  private async download(attachment: Attachment, button: HTMLButtonElement): Promise<void> {
    const threadId = state.currentThreadId;
    if (!threadId || button.disabled) return;
    const previous = button.textContent;
    button.disabled = true;
    button.textContent = 'Downloading…';
    try {
      const saved = await window.hive.saveAttachment(threadId, attachment);
      this.onStatus(`Downloaded ${attachment.name} to ${saved}`);
    } catch (error) {
      this.onStatus(errorMessage(error));
    } finally {
      button.disabled = false;
      button.textContent = previous;
    }
  }

  private updateEmptyState(): void {
    byId('empty-chat').hidden = this.renderedIds.size > 0 || Boolean(state.currentThreadId);
  }
}
