import { mkdir, readFile, rename, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import type { ThreadUiState } from '../shared/contracts.js';
import { arrayValue, objectValue } from '../shared/normalize.js';

export class ThreadUiStore {
  private value: ThreadUiState = { collapsed: [], overrides: {} };

  constructor(private readonly directory: string) {}

  private get filename(): string { return path.join(this.directory, 'thread-ui.json'); }

  async load(): Promise<void> {
    const legacyHome = process.env.HIVE_CLIENT_LEGACY_DATA_HOME ?? path.join(os.homedir(), '.local', 'share');
    let saved: unknown = {};
    let legacy: unknown = {};
    try { saved = JSON.parse(await readFile(this.filename, 'utf8')); } catch { /* New profile. */ }
    try { legacy = JSON.parse(await readFile(path.join(legacyHome, 'hivemind', 'client-ui.json'), 'utf8')); } catch { /* No legacy state. */ }
    const savedRoot = objectValue(saved);
    const legacyRoot = objectValue(legacy);
    const collapsed = savedRoot.collapsed ?? legacyRoot.collapsed_groups;
    const overrides = savedRoot.overrides ?? legacyRoot.bucket_overrides;
    this.value = {
      collapsed: [...new Set(arrayValue(collapsed).filter((item): item is string => typeof item === 'string' && item.length <= 512))],
      overrides: Object.fromEntries(Object.entries(objectValue(overrides)).flatMap(([thread, bucket]) =>
        typeof bucket === 'string' && bucket.length <= 512 ? [[thread, bucket]] : [])),
    };
  }

  snapshot(): ThreadUiState { return structuredClone(this.value); }

  async setCollapsed(groupId: string, collapsed: boolean): Promise<ThreadUiState> {
    const groups = new Set(this.value.collapsed);
    if (collapsed) groups.add(groupId); else groups.delete(groupId);
    this.value.collapsed = [...groups];
    await this.save();
    return this.snapshot();
  }

  async setOverride(threadId: string, bucket: string): Promise<ThreadUiState> {
    if (bucket) this.value.overrides[threadId] = bucket;
    else delete this.value.overrides[threadId];
    await this.save();
    return this.snapshot();
  }

  private async save(): Promise<void> {
    await mkdir(this.directory, { recursive: true, mode: 0o700 });
    const temporary = `${this.filename}.${process.pid}.tmp`;
    await writeFile(temporary, `${JSON.stringify(this.value, null, 2)}\n`, { mode: 0o600 });
    await rename(temporary, this.filename);
  }
}
