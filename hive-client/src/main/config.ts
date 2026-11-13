import { mkdir, readFile, rename, writeFile } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import type { AppSettings, SettingsPatch, SettingsView, VoiceAssignments } from '../shared/contracts.js';
import { booleanValue, numberValue, objectValue, stringValue } from '../shared/normalize.js';

export const DEFAULT_CHIMPFLOW_MODEL = 'kokoro-82m';
const DEFAULT_CHIMPFLOW_URL = 'http://127.0.0.1:8090';

export function defaultSettings(): AppSettings {
  return {
    hubServer: 'http://127.0.0.1:8421', hubApiKey: '',
    chimpflowUrl: DEFAULT_CHIMPFLOW_URL, chimpflowModel: DEFAULT_CHIMPFLOW_MODEL,
    voices: {}, displayName: 'You', userAvatar: '', fontScale: 1, masterVolume: 1,
    selectedAgentId: '', selectedThreadId: '',
  };
}

export function normalizeHttpUrl(value: string, name: string): string {
  const trimmed = value.trim().replace(/\/+$/, '');
  const url = new URL(trimmed);
  if (url.protocol !== 'http:' && url.protocol !== 'https:') {
    throw new Error(`${name} must use HTTP or HTTPS`);
  }
  if (url.username || url.password || url.search || url.hash) {
    throw new Error(`${name} must not contain credentials, a query, or a fragment`);
  }
  return trimmed;
}

export function normalizeChimpFlowUrl(value: string): string {
  const normalized = normalizeHttpUrl(value, 'ChimpFlow address');
  const url = new URL(normalized);
  if (url.protocol !== 'http:' || !['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname)) {
    throw new Error('ChimpFlow must use HTTP on localhost, 127.0.0.1, or ::1');
  }
  return normalized;
}

function validVoiceAssignments(value: unknown): VoiceAssignments {
  const result: VoiceAssignments = {};
  for (const [model, agents] of Object.entries(objectValue(value))) {
    if (typeof agents === 'string') {
      // Legacy unscoped Pocket TTS ids belong only to pocket-tts.
      result['pocket-tts'] ??= {};
      result['pocket-tts'][model] = agents;
      continue;
    }
    const target: Record<string, string> = {};
    for (const [agent, voice] of Object.entries(objectValue(agents))) {
      if (typeof voice === 'string') target[agent] = voice;
    }
    result[model] = target;
  }
  return result;
}

export function normalizeSettings(value: unknown): AppSettings {
  const root = objectValue(value);
  const defaults = defaultSettings();
  return {
    hubServer: stringValue(root.hubServer, defaults.hubServer),
    hubApiKey: stringValue(root.hubApiKey),
    chimpflowUrl: stringValue(root.chimpflowUrl, defaults.chimpflowUrl),
    chimpflowModel: stringValue(root.chimpflowModel, defaults.chimpflowModel) || DEFAULT_CHIMPFLOW_MODEL,
    voices: validVoiceAssignments(root.voices),
    displayName: stringValue(root.displayName, defaults.displayName) || defaults.displayName,
    userAvatar: stringValue(root.userAvatar),
    fontScale: Math.min(1.75, Math.max(0.75, numberValue(root.fontScale, 1))),
    masterVolume: Math.min(1, Math.max(0, numberValue(root.masterVolume, 1))),
    selectedAgentId: stringValue(root.selectedAgentId),
    selectedThreadId: stringValue(root.selectedThreadId),
  };
}

async function readJson(filename: string): Promise<unknown> {
  try { return JSON.parse(await readFile(filename, 'utf8')) as unknown; }
  catch { return {}; }
}

async function readKey(filename: string): Promise<string> {
  try { return (await readFile(filename, 'utf8')).trim(); }
  catch { return ''; }
}

function legacyConfigRoot(): string {
  return process.env.HIVE_CLIENT_LEGACY_CONFIG_HOME ??
    (process.platform === 'win32' ? path.join(os.homedir(), 'AppData', 'Roaming') : path.join(os.homedir(), '.config'));
}

export class SettingsStore {
  private settings = defaultSettings();
  private chimpflowKey = '';
  private saveQueue: Promise<void> = Promise.resolve();

  constructor(private readonly directory: string) {}

  private get filename(): string { return path.join(this.directory, 'settings.json'); }
  private get keyFilename(): string { return path.join(this.directory, 'chimpflow.key'); }

  async load(): Promise<void> {
    const legacyRoot = legacyConfigRoot();
    const [saved, hub, voice, savedKey, legacyKey] = await Promise.all([
      readJson(this.filename),
      readJson(path.join(legacyRoot, 'hivemind', 'config.json')),
      readJson(path.join(legacyRoot, 'hive-client-voice', 'prefs.json')),
      readKey(this.keyFilename),
      readKey(path.join(legacyRoot, 'hive-client-voice', 'chimpflow.key')),
    ]);
    const hubValues = objectValue(hub);
    const voiceValues = objectValue(voice);
    const legacy: AppSettings = {
      ...defaultSettings(),
      hubServer: stringValue(hubValues.server, stringValue(voiceValues.base, defaultSettings().hubServer)),
      hubApiKey: stringValue(hubValues.api_key, stringValue(voiceValues.api_key)),
      displayName: stringValue(hubValues.display_name, 'You'),
      userAvatar: stringValue(hubValues.avatar),
      fontScale: Math.min(1.75, Math.max(0.75, numberValue(hubValues.font_scale, 1))),
      chimpflowUrl: stringValue(voiceValues.chimpflow_url,
        process.env.CHIMPFLOW_URL || DEFAULT_CHIMPFLOW_URL),
      chimpflowModel: stringValue(voiceValues.chimpflow_model, DEFAULT_CHIMPFLOW_MODEL),
      voices: validVoiceAssignments(voiceValues.voices),
      selectedAgentId: stringValue(voiceValues.agent_id),
    };
    this.settings = normalizeSettings({ ...legacy, ...objectValue(saved) });
    this.chimpflowKey = savedKey || process.env.CHIMPFLOW_KEY || legacyKey;
    if (!savedKey && this.chimpflowKey) {
      await mkdir(this.directory, { recursive: true, mode: 0o700 });
      await writeFile(this.keyFilename, `${this.chimpflowKey}\n`, { mode: 0o600 });
    }
  }

  view(): SettingsView { return { ...this.settings, voices: structuredClone(this.settings.voices), chimpflowKey: this.chimpflowKey }; }
  snapshot(): AppSettings { return { ...this.settings, voices: structuredClone(this.settings.voices) }; }
  key(): string { return this.chimpflowKey; }

  async update(patch: SettingsPatch): Promise<SettingsView> {
    const { chimpflowKey, ...ordinary } = patch;
    const merged = { ...this.settings, ...ordinary };
    if (ordinary.chimpflowUrl !== undefined) merged.chimpflowUrl = normalizeChimpFlowUrl(ordinary.chimpflowUrl);
    if (ordinary.hubServer !== undefined) merged.hubServer = normalizeHttpUrl(ordinary.hubServer, 'HiveMind server');
    this.settings = normalizeSettings(merged);
    if (chimpflowKey !== undefined) this.chimpflowKey = chimpflowKey.trim();
    await mkdir(this.directory, { recursive: true, mode: 0o700 });
    if (chimpflowKey !== undefined) await writeFile(this.keyFilename, `${this.chimpflowKey}\n`, { mode: 0o600 });
    const snapshot = this.snapshot();
    this.saveQueue = this.saveQueue.catch((error: unknown) => {
      console.error('Previous settings save failed:', error);
    }).then(async () => {
      const temporary = `${this.filename}.${process.pid}.tmp`;
      await writeFile(temporary, `${JSON.stringify(snapshot, null, 2)}\n`, { mode: 0o600 });
      await rename(temporary, this.filename);
    });
    await this.saveQueue;
    return this.view();
  }
}

/** Validate a downloaded/uploaded attachment at the IPC boundary. */
export function validAttachment(value: unknown): boolean {
  const item = objectValue(value);
  return typeof item.name === 'string' && typeof item.path === 'string' &&
    (item.size === undefined || (typeof item.size === 'number' && item.size >= 0)) &&
    (item.hostPath === undefined || typeof item.hostPath === 'string') &&
    (item.worker === undefined || typeof item.worker === 'string');
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

export function optionalBoolean(value: unknown): boolean | undefined {
  return typeof value === 'boolean' ? value : booleanValue(value, false);
}
