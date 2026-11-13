import { app, BrowserWindow, ipcMain, nativeTheme, Notification } from 'electron';
import path from 'node:path';
import { appendFile, copyFile, mkdir, unlink, writeFile } from 'node:fs/promises';
import { constants as fsConstants } from 'node:fs';
import { fileURLToPath } from 'node:url';
import type { Attachment, NewAgentInput, RoutineInput, RoutineSchedule, SettingsPatch, SkillInput } from '../shared/contracts.js';
import { arrayValue, objectValue } from '../shared/normalize.js';
import { ChimpFlowService } from './chimpflow.js';
import {
  isRecord, normalizeHttpUrl, SettingsStore, validAttachment,
} from './config.js';
import { HubService } from './hub.js';
import { loadWindowState, saveWindowState } from './window-state.js';
import { ThreadUiStore } from './thread-ui.js';

const directory = path.dirname(fileURLToPath(import.meta.url));
const isolatedUserData = process.env.HIVE_CLIENT_USER_DATA;
if (isolatedUserData) app.setPath('userData', path.resolve(isolatedUserData));
const isolatedDownloads = process.env.HIVE_CLIENT_DOWNLOADS;
if (isolatedDownloads) app.setPath('downloads', path.resolve(isolatedDownloads));
app.setName('HiveMind');

const settings = new SettingsStore(app.getPath('userData'));
const hub = new HubService(settings);
const threadUi = new ThreadUiStore(app.getPath('userData'));
const chimpflow = new ChimpFlowService(settings, (message) => void logStartup(message));
let mainWindow: BrowserWindow | undefined;
let fatalVisible = false;
const startupLog = path.join(app.getPath('userData'), 'logs', 'startup.log');
const windowStateFile = path.join(app.getPath('userData'), 'window-state.json');

function errorText(error: unknown): string {
  return error instanceof Error ? (error.stack ?? error.message) : String(error);
}

async function logStartup(message: string): Promise<void> {
  const line = `${new Date().toISOString()} ${message}\n`;
  try {
    await mkdir(path.dirname(startupLog), { recursive: true, mode: 0o700 });
    await appendFile(startupLog, line, { mode: 0o600 });
  } catch (error) { console.error('Could not write the HiveMind startup log:', error); }
}

function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[character] ?? character);
}

async function showFatal(error: unknown): Promise<void> {
  const detail = errorText(error);
  console.error('HiveMind fatal error:', detail);
  await logStartup(`FATAL ${detail}`);
  if (fatalVisible) return;
  fatalVisible = true;
  try {
    if (!app.isReady()) await app.whenReady();
    const window = new BrowserWindow({
      width: 720, height: 480, title: 'HiveMind startup failed',
      backgroundColor: nativeTheme.shouldUseDarkColors ? '#242424' : '#ffffff',
      webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
    });
    const html = `<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'"><title>HiveMind startup failed</title><style>body{font:16px sans-serif;padding:32px;line-height:1.5;background:${nativeTheme.shouldUseDarkColors ? '#242424;color:#eee' : '#fff;color:#222'}}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#8882;padding:16px}code{overflow-wrap:anywhere}</style></head><body><h1>HiveMind could not start</h1><p>The error was caught instead of failing silently. A diagnostic was saved to:</p><p><code>${escapeHtml(startupLog)}</code></p><pre>${escapeHtml(detail)}</pre><p>Close this window to exit.</p></body></html>`;
    await window.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`);
    window.once('closed', () => app.exit(1));
  } catch (displayError) {
    console.error('Could not display the HiveMind fatal-error window:', displayError);
    app.exit(1);
  }
}

process.on('uncaughtException', (error) => { void showFatal(error); });
process.on('unhandledRejection', (reason) => { void showFatal(reason); });

function text(value: unknown, name: string, maximum = 512): string {
  if (typeof value !== 'string') throw new Error(`${name} must be text`);
  if (value.length > maximum) throw new Error(`${name} is too long`);
  return value;
}

function identifier(value: unknown, name: string): string {
  const result = text(value, name, 512);
  if (!result) throw new Error(`${name} is required`);
  return result;
}

function settingsPatch(value: unknown): SettingsPatch {
  if (!isRecord(value)) throw new Error('Settings update must be an object');
  const output: Record<string, unknown> = {};
  const textKeys = ['hubServer', 'hubApiKey', 'chimpflowUrl', 'chimpflowModel', 'displayName',
    'userAvatar', 'selectedAgentId', 'selectedThreadId', 'chimpflowKey'] as const;
  for (const key of textKeys) if (value[key] !== undefined) output[key] = text(value[key], key, 16_384);
  if (value.fontScale !== undefined) {
    if (typeof value.fontScale !== 'number' || !Number.isFinite(value.fontScale)) throw new Error('Invalid font scale');
    output.fontScale = value.fontScale;
  }
  if (value.masterVolume !== undefined) {
    if (typeof value.masterVolume !== 'number' || !Number.isFinite(value.masterVolume)) throw new Error('Invalid volume');
    output.masterVolume = value.masterVolume;
  }
  if (value.voices !== undefined) {
    if (!isRecord(value.voices)) throw new Error('Voice assignments must be an object');
    output.voices = value.voices;
  }
  return output as SettingsPatch;
}

function agentInput(value: unknown): NewAgentInput {
  const root = objectValue(value);
  const name = text(root.name, 'Agent name', 160).trim();
  if (!name) throw new Error('Agent name is required');
  const type = root.type === undefined ? '' : text(root.type, 'Agent type', 32);
  if (!['', 'gpu', 'default'].includes(type)) throw new Error('Invalid agent type');
  const thinkingLevel = root.thinkingLevel === undefined ? '' : text(root.thinkingLevel, 'Thinking level', 32);
  if (!['', 'default', 'none', 'low', 'high', 'max'].includes(thinkingLevel)) throw new Error('Invalid thinking level');
  const cpus = root.cpus === undefined ? -1 : root.cpus;
  if (typeof cpus !== 'number' || !Number.isFinite(cpus) || cpus < -1 || cpus > 1024) throw new Error('Invalid CPU limit');
  if (root.hostAgent !== undefined && typeof root.hostAgent !== 'boolean') throw new Error('Invalid host agent flag');
  return {
    name, role: text(root.role, 'Agent role', 32_768),
    model: text(root.model, 'Model', 512), worker: text(root.worker, 'Worker', 512),
    avatar: root.avatar === undefined ? '' : text(root.avatar, 'Avatar', 512),
    type, memory: root.memory === undefined ? '' : text(root.memory, 'Memory limit', 64),
    cpus, thinkingLevel,
    ...(root.hostAgent === undefined ? {} : { hostAgent: root.hostAgent as boolean }),
  };
}

function skillInput(value: unknown): SkillInput {
  const item = objectValue(value);
  const name = text(item.name, 'Skill name', 160);
  if (!/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(name)) throw new Error('Skill name must be kebab-case (a-z, 0-9, -)');
  const content = text(item.content, 'Skill content', 1_048_576);
  if (!content.trim()) throw new Error('Skill content is required');
  return { name, description: text(item.description, 'Skill description', 4096), content };
}

function attachmentInput(value: unknown): Attachment {
  if (!validAttachment(value)) throw new Error('Invalid attachment metadata');
  const item = objectValue(value);
  return {
    name: text(item.name, 'Attachment name', 1024), path: text(item.path, 'Attachment path', 4096),
    hostPath: typeof item.hostPath === 'string' ? item.hostPath : '',
    size: typeof item.size === 'number' ? item.size : 0,
    worker: typeof item.worker === 'string' ? text(item.worker, 'Worker', 512) : '',
  };
}

function scheduleInput(value: unknown): RoutineSchedule {
  const item = objectValue(value);
  const type = text(item.type, 'Schedule type', 32);
  if (type === 'once') return { type, at: text(item.at, 'Scheduled date/time', 64) };
  if (type === 'interval') {
    const minutes = item.every_minutes;
    if (typeof minutes !== 'number' || !Number.isInteger(minutes) || minutes < 1 || minutes > 10_080) {
      throw new Error('Interval must be a whole number from 1 to 10080 minutes');
    }
    return { type, every_minutes: minutes };
  }
  if (type === 'daily') return { type, time: text(item.time, 'Daily time', 32) };
  if (type === 'weekly') {
    const days = arrayValue(item.days);
    if (!days.every((day) => typeof day === 'number' && Number.isInteger(day) && day >= 0 && day <= 6)) {
      throw new Error('Weekly days must be integers from 0 to 6');
    }
    return { type, time: text(item.time, 'Weekly time', 32), days: [...new Set(days)] as number[] };
  }
  throw new Error('Unknown schedule type');
}

function routineInput(value: unknown): RoutineInput {
  const item = objectValue(value);
  const name = text(item.name, 'Task name', 160).trim();
  const prompt = text(item.prompt, 'Task prompt', 1_000_000).trim();
  if (!name || !prompt) throw new Error('Task name and prompt are required');
  return {
    agentId: identifier(item.agentId, 'Agent id'), name, prompt,
    schedule: scheduleInput(item.schedule),
  };
}

async function saveDownload(filename: string, bytes: ArrayBuffer): Promise<string> {
  const downloads = app.getPath('downloads');
  await mkdir(downloads, { recursive: true });
  const parsed = path.parse(path.basename(filename).replace(/[\0]/g, '_') || 'attachment');
  for (let index = 0; index < 1000; ++index) {
    const suffix = index ? ` (${index})` : '';
    const target = path.join(downloads, `${parsed.name}${suffix}${parsed.ext}`);
    const temporary = `${target}.${process.pid}.${Date.now()}.part`;
    try {
      await writeFile(temporary, Buffer.from(bytes), { flag: 'wx' });
      await copyFile(temporary, target, fsConstants.COPYFILE_EXCL);
      await unlink(temporary);
      return target;
    } catch (error) {
      await unlink(temporary).catch(() => undefined);
      if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
    }
  }
  throw new Error('Could not choose an unused download filename');
}

function registerIpc(): void {
  ipcMain.handle('ui:theme', () => ({ dark: nativeTheme.shouldUseDarkColors, source: nativeTheme.themeSource }));
  ipcMain.on('ui:notify', (_event, title: unknown, body: unknown) => {
    if (!Notification.isSupported()) return;
    const notification = new Notification({
      title: text(title, 'Notification title', 512), body: text(body, 'Notification body', 2048),
    });
    notification.on('click', () => { if (mainWindow) { mainWindow.show(); mainWindow.focus(); } });
    notification.show();
  });
  ipcMain.handle('settings:get', () => settings.view());
  ipcMain.handle('settings:save', (_event, patch) => settings.update(settingsPatch(patch)));
  ipcMain.handle('hub:test', (_event, server, key) => hub.testConnection(
    normalizeHttpUrl(text(server, 'Server URL', 2048), 'HiveMind server'), text(key, 'API key', 16_384)));
  ipcMain.handle('hub:bootstrap', () => hub.getBootstrap());
  ipcMain.handle('hub:thread-groups', () => hub.getThreadGroups());
  ipcMain.handle('hub:create-thread-group', (_event, name) => hub.createThreadGroup(text(name, 'Group name', 160).trim()));
  ipcMain.handle('hub:rename-thread-group', (_event, groupId, name) => hub.renameThreadGroup(
    identifier(groupId, 'Group id'), text(name, 'Group name', 160).trim()));
  ipcMain.handle('hub:delete-thread-group', (_event, groupId) => hub.deleteThreadGroup(identifier(groupId, 'Group id')));
  ipcMain.handle('ui:thread-state', () => threadUi.snapshot());
  ipcMain.handle('ui:thread-collapsed', (_event, groupId, collapsed) => {
    if (typeof collapsed !== 'boolean') throw new Error('Collapsed must be a boolean');
    return threadUi.setCollapsed(identifier(groupId, 'Group id'), collapsed);
  });
  ipcMain.handle('ui:move-thread', async (_event, threadId, groupId) => {
    const thread = identifier(threadId, 'Thread id');
    const group = text(groupId, 'Group id', 512);
    const smart = group.startsWith('smart:');
    await hub.setThreadGroup(thread, smart ? '' : group);
    return threadUi.setOverride(thread, smart ? group : '');
  });
  ipcMain.handle('hub:open-agent', (_event, agentId) => hub.openAgent(identifier(agentId, 'Agent id')));
  ipcMain.handle('hub:create-thread', (_event, title, agentIds) => hub.createThread(
    text(title, 'Thread title', 512), arrayValue(agentIds).map((id) => identifier(id, 'Agent id'))));
  ipcMain.handle('hub:agent-options', () => hub.getAgentOptions());
  ipcMain.handle('hub:cancel-agent', (_event, agentId) => hub.cancelAgent(identifier(agentId, 'Agent id')));
  ipcMain.handle('hub:create-agent', (_event, input) => hub.createAgent(agentInput(input)));
  ipcMain.handle('hub:update-agent', (_event, agentId, input) => hub.updateAgent(
    identifier(agentId, 'Agent id'), agentInput(input)));
  ipcMain.handle('hub:delete-agent', (_event, agentId, reassignTo) => hub.deleteAgent(
    identifier(agentId, 'Agent id'), text(reassignTo, 'Reassignment agent id', 512)));
  ipcMain.handle('hub:set-agent-running', (_event, agentId, running) => {
    if (typeof running !== 'boolean') throw new Error('Running must be a boolean');
    return hub.setAgentRunning(identifier(agentId, 'Agent id'), running);
  });
  ipcMain.handle('hub:exec-log', (_event, agentId) => hub.getExecLog(identifier(agentId, 'Agent id')));
  ipcMain.handle('hub:agent-screen', (_event, agentId, viewOnly) => {
    if (typeof viewOnly !== 'boolean') throw new Error('View-only must be a boolean');
    return openAgentScreen(identifier(agentId, 'Agent id'), viewOnly);
  });
  ipcMain.handle('hub:agent-skills', (_event, agentId) => hub.getAgentSkills(identifier(agentId, 'Agent id')));
  ipcMain.handle('hub:shared-skill-enabled', (_event, agentId, name, enabled) => {
    if (typeof enabled !== 'boolean') throw new Error('Skill enabled must be a boolean');
    return hub.setSharedSkillEnabled(identifier(agentId, 'Agent id'), identifier(name, 'Skill name'), enabled);
  });
  ipcMain.handle('hub:create-global-skill', (_event, input) => hub.createGlobalSkill(skillInput(input)));
  ipcMain.handle('hub:create-agent-skill', (_event, agentId, input) => hub.createAgentSkill(
    identifier(agentId, 'Agent id'), skillInput(input)));
  ipcMain.handle('hub:delete-global-skill', (_event, name) => hub.deleteGlobalSkill(identifier(name, 'Skill name')));
  ipcMain.handle('hub:delete-agent-skill', (_event, agentId, name) => hub.deleteAgentSkill(
    identifier(agentId, 'Agent id'), identifier(name, 'Skill name')));
  ipcMain.handle('hub:clear-thread', (_event, threadId) => hub.clearThread(identifier(threadId, 'Thread id')));
  ipcMain.handle('hub:delete-thread', (_event, threadId) => hub.deleteThread(identifier(threadId, 'Thread id')));
  ipcMain.handle('hub:messages', (_event, threadId, afterId, beforeId) => hub.getMessages(
    identifier(threadId, 'Thread id'), text(afterId, 'After id'), text(beforeId, 'Before id')));
  ipcMain.handle('hub:activity', (_event, threadId) => hub.getActivity(identifier(threadId, 'Thread id')));
  ipcMain.handle('hub:send', (_event, threadId, body, source, attachments) => {
    if (source !== 'user' && source !== 'voice') throw new Error('Invalid message source');
    const items = arrayValue(attachments).map(attachmentInput);
    return hub.sendMessage(identifier(threadId, 'Thread id'), text(body, 'Message', 1_000_000), source, items);
  });
  ipcMain.handle('hub:cancel', (_event, threadId) => hub.cancelThread(identifier(threadId, 'Thread id')));
  ipcMain.handle('hub:read', (_event, threadId) => hub.markThreadRead(identifier(threadId, 'Thread id')));
  ipcMain.handle('hub:upload', async (_event, threadId, filename, bytes) => {
    if (!(bytes instanceof ArrayBuffer)) throw new Error('Upload must contain file bytes');
    const cleanName = path.basename(identifier(filename, 'Filename'));
    return hub.upload(identifier(threadId, 'Thread id'), cleanName, Buffer.from(bytes));
  });
  ipcMain.handle('hub:attachment', (_event, threadId, attachment) => hub.download(
    identifier(threadId, 'Thread id'), attachmentInput(attachment)));
  ipcMain.handle('hub:save-attachment', async (_event, threadId, attachment) => {
    const input = attachmentInput(attachment);
    const payload = await hub.download(identifier(threadId, 'Thread id'), input);
    return saveDownload(input.name, payload.bytes);
  });
  ipcMain.handle('hub:routines', () => hub.getRoutines());
  ipcMain.handle('hub:create-routine', (_event, input) => hub.createRoutine(routineInput(input)));
  ipcMain.handle('hub:update-routine', (_event, routineId, input) => hub.updateRoutine(
    identifier(routineId, 'Task id'), routineInput(input)));
  ipcMain.handle('hub:set-routine-enabled', (_event, routineId, enabled) => {
    if (typeof enabled !== 'boolean') throw new Error('Enabled must be a boolean');
    return hub.setRoutineEnabled(identifier(routineId, 'Task id'), enabled);
  });
  ipcMain.handle('hub:run-routine', (_event, routineId) => hub.runRoutine(identifier(routineId, 'Task id')));
  ipcMain.handle('hub:delete-routine', (_event, routineId) => hub.deleteRoutine(identifier(routineId, 'Task id')));
  ipcMain.handle('chimpflow:catalog', (_event, model) => chimpflow.catalog(text(model, 'Model', 512)));
  ipcMain.handle('chimpflow:synthesize', (_event, body, agentId, model, voice) => chimpflow.synthesize(
    text(body, 'Speech text', 100_000), text(agentId, 'Agent id', 512),
    text(model, 'Model', 512), voice === undefined ? undefined : text(voice, 'Voice', 512)));
  ipcMain.handle('chimpflow:transcribe', (_event, wav) => {
    if (!(wav instanceof ArrayBuffer)) throw new Error('Recording must be a WAV buffer');
    return chimpflow.transcribe(wav);
  });
}

async function openAgentScreen(agentId: string, viewOnly: boolean): Promise<void> {
  await hub.startScreen(agentId);
  let running = false;
  for (let attempt = 0; attempt < 48 && !running; ++attempt) {
    running = await hub.screenRunning(agentId);
    if (!running) await new Promise((resolve) => setTimeout(resolve, 5000));
  }
  if (!running) throw new Error('The agent desktop did not start within four minutes');
  const agentName = (await hub.getBootstrap()).agents.find((agent) => agent.id === agentId)?.name ?? 'Agent';
  const target = new URL(hub.screenUrl(agentId, viewOnly));
  const window = new BrowserWindow({
    title: `Computer — ${agentName}${viewOnly ? ' (view only)' : ''}`,
    width: 1280, height: 820, autoHideMenuBar: true,
    webPreferences: { nodeIntegration: false, contextIsolation: true, sandbox: true, webSecurity: true },
  });
  window.setMenuBarVisibility(false);
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', (event, url) => {
    const next = new URL(url);
    if (next.origin !== target.origin) event.preventDefault();
  });
  await window.loadURL(target.toString());
}

function createWindow(): void {
  const savedState = loadWindowState(windowStateFile);
  void logStartup(`Window size ${savedState.width}x${savedState.height}`);
  mainWindow = new BrowserWindow({
    title: 'HiveMind', width: savedState.width, height: savedState.height, minWidth: 760, minHeight: 560,
    show: false, autoHideMenuBar: true,
    backgroundColor: nativeTheme.shouldUseDarkColors ? '#242424' : '#fafafa',
    webPreferences: {
      preload: path.join(directory, '..', 'preload', 'index.cjs'),
      nodeIntegration: false, contextIsolation: true, sandbox: true,
      webSecurity: true, allowRunningInsecureContent: false,
      devTools: !app.isPackaged || process.env.HIVE_CLIENT_DEVTOOLS === '1',
    },
  });
  const window = mainWindow;
  let resizeTimer: NodeJS.Timeout | undefined;
  const saveSize = (): void => {
    if (window.isDestroyed()) return;
    const [width = 0, height = 0] = window.getSize();
    saveWindowState(windowStateFile, { width, height });
  };
  window.once('ready-to-show', () => window.show());
  window.on('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(saveSize, 500);
  });
  window.on('close', () => { clearTimeout(resizeTimer); saveSize(); });
  window.on('closed', () => { clearTimeout(resizeTimer); if (mainWindow === window) mainWindow = undefined; });
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', (event, url) => {
    if (url !== window.webContents.getURL()) event.preventDefault();
  });
  window.webContents.session.setPermissionRequestHandler((_contents, permission, callback, details) => {
    callback(permission === 'media' && details.requestingUrl.startsWith('file://'));
  });
  window.webContents.session.setPermissionCheckHandler((_contents, permission, requestingOrigin) => {
    return permission === 'media' && requestingOrigin.startsWith('file://');
  });
  void window.loadFile(path.join(directory, '..', 'renderer', 'index.html')).catch((error: unknown) => {
    console.error('Could not load HiveMind renderer:', error);
    app.quit();
  });
}

app.on('before-quit', () => { if (mainWindow && !mainWindow.isDestroyed()) mainWindow.emit('close'); });

const gotLock = app.requestSingleInstanceLock();
if (!gotLock) app.quit();
else {
  app.on('second-instance', () => {
    if (!mainWindow) return;
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  });
  nativeTheme.themeSource = 'system';

  void logStartup('Main process starting');
  void app.whenReady().then(async () => {
    await settings.load();
    await threadUi.load();
    registerIpc();
    nativeTheme.on('updated', () => {
      const theme = { dark: nativeTheme.shouldUseDarkColors, source: nativeTheme.themeSource };
      for (const window of BrowserWindow.getAllWindows()) window.webContents.send('ui:theme-changed', theme);
    });
    createWindow();
    await logStartup('Main window created');
    app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
  }).catch((error: unknown) => { void showFatal(error); });
  app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
}
