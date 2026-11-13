import { byId } from './dom.js';

export class ConfirmationDialog {
  private resolve: ((confirmed: boolean) => void) | undefined;
  private confirmed = false;

  constructor() {
    byId<HTMLFormElement>('confirmation-form').addEventListener('submit', (event) => {
      event.preventDefault();
      this.confirmed = true;
      byId<HTMLDialogElement>('confirmation-dialog').close();
    });
    byId('confirmation-cancel').addEventListener('click', () => byId<HTMLDialogElement>('confirmation-dialog').close());
    byId<HTMLDialogElement>('confirmation-dialog').addEventListener('close', () => {
      this.resolve?.(this.confirmed);
      this.resolve = undefined;
    });
  }

  ask(title: string, detail: string, confirmLabel: string): Promise<boolean> {
    this.resolve?.(false);
    this.confirmed = false;
    byId('confirmation-title').textContent = title;
    byId('confirmation-detail').textContent = detail;
    byId('confirmation-confirm').textContent = confirmLabel;
    const dialog = byId<HTMLDialogElement>('confirmation-dialog');
    if (!dialog.open) dialog.showModal();
    return new Promise<boolean>((resolve) => { this.resolve = resolve; });
  }
}
