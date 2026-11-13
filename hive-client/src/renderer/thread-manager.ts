import type { Thread } from '../shared/contracts.js';
import type { ConfirmationDialog } from './confirmation.js';
import { byId, errorMessage } from './dom.js';
import { state, threadTitle } from './state.js';

export class ThreadManager {
  private thread: Thread | undefined;

  constructor(
    private readonly confirmation: ConfirmationDialog,
    private readonly onStatus: (message: string) => void,
    private readonly onCleared: (thread: Thread) => void,
    private readonly onDeleted: (thread: Thread) => void,
    private readonly onChanged: () => void,
    private readonly onNewGroup: (thread: Thread) => void,
  ) {
    byId('thread-manage-close').addEventListener('click', () => byId<HTMLDialogElement>('thread-manage-dialog').close());
    byId('thread-clear-button').addEventListener('click', () => { void this.clear(); });
    byId('thread-group-select').addEventListener('change', () => { void this.move(); });
    byId('thread-new-group-button').addEventListener('click', () => {
      if (!this.thread) return;
      byId<HTMLDialogElement>('thread-manage-dialog').close();
      this.onNewGroup(this.thread);
    });
    byId('thread-delete-button').addEventListener('click', () => { void this.delete(); });
  }

  open(thread: Thread): void {
    this.thread = thread;
    byId('thread-manage-title').textContent = threadTitle(thread);
    byId('thread-delete-button').hidden = thread.kind === 'user';
    byId('thread-manage-error').textContent = '';
    const select = byId<HTMLSelectElement>('thread-group-select');
    select.replaceChildren(
      new Option('Automatic smart group', ''),
      new Option('You & agents', 'smart:you'),
      new Option('Agent chatter', 'smart:chatter'),
      new Option('Routines', 'smart:routines'),
      ...state.threadGroups.map((group) => new Option(group.name, group.id)),
    );
    select.value = thread.groupId ?? state.threadUi.overrides[thread.id] ?? '';
    byId<HTMLDialogElement>('thread-manage-dialog').showModal();
  }

  private async move(): Promise<void> {
    if (!this.thread) return;
    try {
      state.threadUi = await window.hive.moveThreadToGroup(this.thread.id, byId<HTMLSelectElement>('thread-group-select').value);
      this.onStatus(`Moved ${threadTitle(this.thread)}`);
      this.onChanged();
    } catch (error) { byId('thread-manage-error').textContent = errorMessage(error); }
  }

  private async clear(): Promise<void> {
    if (!this.thread) return;
    const thread = this.thread;
    if (!await this.confirmation.ask('Clear thread history?', `Clear every message in “${threadTitle(thread)}”? The thread stays; the history is gone for good.`, 'Clear')) return;
    try {
      await window.hive.clearThread(thread.id);
      byId<HTMLDialogElement>('thread-manage-dialog').close();
      this.onStatus(`Cleared ${threadTitle(thread)}`);
      this.onCleared(thread);
    } catch (error) { byId('thread-manage-error').textContent = errorMessage(error); }
  }

  private async delete(): Promise<void> {
    if (!this.thread) return;
    const thread = this.thread;
    if (!await this.confirmation.ask('Delete thread?', `Delete “${threadTitle(thread)}” and all its messages? This cannot be undone.`, 'Delete')) return;
    try {
      await window.hive.deleteThread(thread.id);
      byId<HTMLDialogElement>('thread-manage-dialog').close();
      this.onStatus(`Deleted ${threadTitle(thread)}`);
      this.onDeleted(thread);
    } catch (error) { byId('thread-manage-error').textContent = errorMessage(error); }
  }
}
