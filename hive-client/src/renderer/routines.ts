import type { Routine, RoutineInput, RoutineSchedule } from '../shared/contracts.js';
import { byId, element, errorMessage, formatTime } from './dom.js';
import { agentById, state } from './state.js';

function scheduleDescription(routine: Routine): string {
  const schedule = routine.scheduleText || routine.schedule.type;
  const next = routine.nextFireAt ? formatTime(routine.nextFireAt) || routine.nextFireAt : 'not scheduled';
  return `${schedule} · next: ${next}`;
}

export class RoutinesDialog {
  private routines: Routine[] = [];
  private editingId = '';
  private loadGeneration = 0;
  private preferredAgentId = '';

  constructor(
    private readonly onStatus: (message: string) => void,
    private readonly onChanged: () => void,
  ) {
    byId('tasks-close').addEventListener('click', () => byId<HTMLDialogElement>('tasks-dialog').close());
    byId('task-add-button').addEventListener('click', () => this.openEditor());
    byId<HTMLDialogElement>('tasks-dialog').addEventListener('close', () => { ++this.loadGeneration; });
    byId('task-editor-cancel').addEventListener('click', () => byId<HTMLDialogElement>('task-editor-dialog').close());
    byId<HTMLSelectElement>('task-schedule-type').addEventListener('change', () => this.showScheduleFields());
    byId<HTMLFormElement>('task-editor-form').addEventListener('submit', (event) => {
      event.preventDefault();
      void this.saveEditor();
    });
    byId('task-delete-cancel').addEventListener('click', () => byId<HTMLDialogElement>('task-delete-dialog').close());
    byId<HTMLFormElement>('task-delete-form').addEventListener('submit', (event) => {
      event.preventDefault();
      void this.confirmDelete();
    });
  }

  open(agentId = ''): void {
    this.preferredAgentId = agentId;
    byId<HTMLDialogElement>('tasks-dialog').showModal();
    void this.load();
  }

  private async load(): Promise<void> {
    const generation = ++this.loadGeneration;
    byId('tasks-status').textContent = 'Loading scheduled tasks…';
    try {
      const routines = await window.hive.getRoutines();
      if (generation !== this.loadGeneration) return;
      this.routines = routines;
      this.render();
      byId('tasks-status').textContent = `${routines.length} ${routines.length === 1 ? 'task' : 'tasks'}`;
    } catch (error) {
      if (generation === this.loadGeneration) byId('tasks-status').textContent = errorMessage(error);
    }
  }

  private render(): void {
    const list = byId('tasks-list');
    if (!this.routines.length) {
      list.replaceChildren(element('p', { className: 'empty-list', text: 'No scheduled tasks yet.' }));
      return;
    }
    const groups = new Map<string, Routine[]>();
    for (const routine of this.routines) {
      const items = groups.get(routine.agentId) ?? [];
      items.push(routine);
      groups.set(routine.agentId, items);
    }
    const children: HTMLElement[] = [];
    for (const [agentId, routines] of groups) {
      const section = element('section', { className: 'task-agent-group' });
      section.append(element('h3', { text: agentById(agentId)?.name ?? 'Unknown agent' }));
      for (const routine of routines) section.append(this.renderRoutine(routine));
      children.push(section);
    }
    list.replaceChildren(...children);
  }

  private renderRoutine(routine: Routine): HTMLElement {
    const row = element('article', { className: 'task-row' });
    row.dataset.routineId = routine.id;
    const copy = element('div', { className: 'task-copy' });
    copy.append(element('h4', { text: routine.name }),
      element('p', { text: scheduleDescription(routine) }),
      element('p', { className: 'task-prompt', text: routine.prompt }));
    const actions = element('div', { className: 'task-actions' });
    const enabledLabel = element('label', { className: 'task-enabled' });
    const enabled = document.createElement('input');
    enabled.type = 'checkbox';
    enabled.checked = routine.enabled;
    enabled.setAttribute('aria-label', `Enable ${routine.name}`);
    enabled.addEventListener('change', () => {
      enabled.disabled = true;
      void window.hive.setRoutineEnabled(routine.id, enabled.checked).then(() => this.load())
        .catch((error: unknown) => {
          enabled.checked = !enabled.checked;
          enabled.disabled = false;
          this.onStatus(errorMessage(error));
        });
    });
    enabledLabel.append(enabled, document.createTextNode('Enabled'));
    const run = element('button', { text: 'Run now', ariaLabel: `Run ${routine.name} now` });
    run.addEventListener('click', () => {
      run.disabled = true;
      void window.hive.runRoutine(routine.id).then(() => {
        this.onStatus(`Started ${routine.name}`);
        return this.load();
      }).catch((error: unknown) => this.onStatus(errorMessage(error)))
        .finally(() => { run.disabled = false; });
    });
    const edit = element('button', { text: 'Edit', ariaLabel: `Edit ${routine.name}` });
    edit.addEventListener('click', () => this.openEditor(routine));
    const remove = element('button', { className: 'danger', text: 'Delete', ariaLabel: `Delete ${routine.name}` });
    remove.addEventListener('click', () => this.openDelete(routine));
    actions.append(enabledLabel, run, edit, remove);
    row.append(copy, actions);
    return row;
  }

  private openEditor(routine?: Routine): void {
    this.editingId = routine?.id ?? '';
    byId('task-editor-title').textContent = routine ? 'Edit scheduled task' : 'Add scheduled task';
    byId<HTMLButtonElement>('task-save-button').textContent = routine ? 'Save' : 'Create';
    byId('task-editor-error').textContent = '';
    byId<HTMLInputElement>('task-name').value = routine?.name ?? '';
    byId<HTMLTextAreaElement>('task-prompt').value = routine?.prompt ?? '';
    const agent = byId<HTMLSelectElement>('task-agent');
    agent.replaceChildren(...state.agents.map((item) => new Option(item.name, item.id)));
    agent.value = routine?.agentId || this.preferredAgentId || state.currentAgentId || state.agents[0]?.id || '';
    agent.disabled = Boolean(routine);
    const defaultAt = new Date(Date.now() + 3_600_000).toISOString();
    const schedule = routine?.schedule ?? { type: 'once', at: defaultAt } as RoutineSchedule;
    byId<HTMLSelectElement>('task-schedule-type').value = schedule.type;
    const at = schedule.type === 'once' ? schedule.at : '';
    byId<HTMLInputElement>('task-once-date').value = at.includes('T') ? (at.split('T')[0] ?? '') : defaultAt.slice(0, 10);
    byId<HTMLInputElement>('task-once-time').value = at.includes('T') ? (at.split('T')[1] ?? '').slice(0, 5) : defaultAt.slice(11, 16);
    byId<HTMLInputElement>('task-interval').value = String(schedule.type === 'interval' ? schedule.every_minutes : 60);
    byId<HTMLInputElement>('task-daily-time').value = schedule.type === 'daily' ? schedule.time : '09:00';
    byId<HTMLInputElement>('task-weekly-time').value = schedule.type === 'weekly' ? schedule.time : '09:00';
    const days = schedule.type === 'weekly' ? schedule.days : [1, 2, 3, 4, 5];
    document.querySelectorAll<HTMLInputElement>('#task-weekly-days input').forEach((input) => {
      input.checked = days.includes(Number(input.value));
    });
    this.showScheduleFields();
    byId<HTMLDialogElement>('task-editor-dialog').showModal();
  }

  private showScheduleFields(): void {
    const type = byId<HTMLSelectElement>('task-schedule-type').value;
    for (const name of ['once', 'interval', 'daily', 'weekly']) {
      byId(`task-schedule-${name}`).hidden = name !== type;
    }
  }

  private editorInput(): RoutineInput {
    const type = byId<HTMLSelectElement>('task-schedule-type').value;
    let schedule: RoutineSchedule;
    if (type === 'once') {
      schedule = { type, at: `${byId<HTMLInputElement>('task-once-date').value}T${byId<HTMLInputElement>('task-once-time').value}` };
    } else if (type === 'interval') {
      schedule = { type, every_minutes: byId<HTMLInputElement>('task-interval').valueAsNumber };
    } else if (type === 'daily') {
      schedule = { type, time: byId<HTMLInputElement>('task-daily-time').value };
    } else if (type === 'weekly') {
      const days = [...document.querySelectorAll<HTMLInputElement>('#task-weekly-days input:checked')].map((input) => Number(input.value));
      if (!days.length) throw new Error('Choose at least one weekday');
      schedule = { type, time: byId<HTMLInputElement>('task-weekly-time').value, days };
    } else {
      throw new Error('Choose a schedule type');
    }
    return {
      agentId: byId<HTMLSelectElement>('task-agent').value,
      name: byId<HTMLInputElement>('task-name').value.trim(),
      prompt: byId<HTMLTextAreaElement>('task-prompt').value.trim(), schedule,
    };
  }

  private async saveEditor(): Promise<void> {
    const save = byId<HTMLButtonElement>('task-save-button');
    save.disabled = true;
    byId('task-editor-error').textContent = '';
    try {
      const input = this.editorInput();
      if (this.editingId) await window.hive.updateRoutine(this.editingId, input);
      else await window.hive.createRoutine(input);
      byId<HTMLDialogElement>('task-editor-dialog').close();
      this.onChanged();
      await this.load();
    } catch (error) {
      byId('task-editor-error').textContent = errorMessage(error);
    } finally {
      save.disabled = false;
    }
  }

  private openDelete(routine: Routine): void {
    byId('task-delete-dialog').dataset.routineId = routine.id;
    byId('task-delete-name').textContent = routine.name;
    byId('task-delete-error').textContent = '';
    byId<HTMLDialogElement>('task-delete-dialog').showModal();
  }

  private async confirmDelete(): Promise<void> {
    const dialog = byId<HTMLDialogElement>('task-delete-dialog');
    const id = dialog.dataset.routineId ?? '';
    const button = byId<HTMLButtonElement>('task-delete-confirm');
    button.disabled = true;
    try {
      await window.hive.deleteRoutine(id);
      dialog.close();
      this.onChanged();
      await this.load();
    } catch (error) {
      byId('task-delete-error').textContent = errorMessage(error);
    } finally {
      button.disabled = false;
    }
  }
}
