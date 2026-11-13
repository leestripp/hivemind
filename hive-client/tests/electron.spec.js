import { expect, test, _electron as electron } from '@playwright/test';
import http from 'node:http';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const project = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let app;
let page;
let temporary;
let hubServer;
let chimpServer;
let hubUrl;
let chimpUrl;
let messageCounter = 10;
const requests = { agents: [], skills: [], threads: [], messages: [], cancellations: [], uploads: [], downloads: [], routines: [], speech: [], transcriptions: 0 };
const threadActivity = { busy: false, turns: [] };
const agents = [{ id: 'agent-bob', name: 'Bob', avatar: 'worker-bee', worker: 'local', state: 'ready', busy: false, model: '', role: 'Engineering manager' }];
const threads = [{ id: 'dm-bob', title: 'Bob', kind: 'dm', agent_ids: ['agent-bob'], unread: 0, last_activity: '2026-11-13T00:00:00Z', group_id: null }];
const messages = { 'dm-bob': [
  { id: 'm1', sender_kind: 'user', sender_id: 'user', body: 'Hello Bob', created_at: '2026-11-13T00:00:00Z', attachments: [] },
  { id: 'm2', sender_kind: 'agent', sender_id: 'agent-bob', sender_name: 'Bob', body: 'Hello from Bob.', created_at: '2026-11-13T00:00:01Z', attachments: [
    { name: 'pixel.png', path: '/workspace/output/pixel.png', worker: 'local', size: 68 },
    { name: 'report.txt', path: '/workspace/output/report.txt', worker: 'local', size: 12 },
  ] },
] };

const threadGroups = [];

const sharedSkills = [
  { name: 'code-review', description: 'Review source changes', version: '1.2.0' },
  { name: 'testing', description: 'Run focused tests', version: '1.0.0' },
];
const enabledSkills = { 'agent-bob': ['code-review'] };
const learnedSkills = { 'agent-bob': ['debugging'] };

const routines = [{ id: 'routine-1', agent_id: 'agent-bob', thread_id: 'routine-thread', name: 'Morning brief', prompt: 'Summarize the news', schedule: { type: 'daily', time: '09:00' }, schedule_text: 'Daily at 09:00', next_fire_at: '2026-11-14T09:00:00Z', enabled: true, last_fired_at: '' }];

function json(response, value, status = 200) {
  const body = JSON.stringify(value);
  response.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
  response.end(body);
}

async function body(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  return Buffer.concat(chunks);
}

async function hub(req, res) {
  const url = new URL(req.url, 'http://localhost');
  const screenMatch = url.pathname.match(/^\/screen\/([^/]+)\/$/);
  if (screenMatch) {
    if (url.searchParams.get('key') !== 'hub-test-key') return json(res, { error: 'Unauthorized' }, 401);
    const agent = agents.find((item) => item.id === screenMatch[1]);
    const html = `<!doctype html><html><head><title>Agent desktop</title></head><body><main><h1>${agent?.name || 'Agent'} desktop</h1><p>${url.searchParams.has('view_only') ? 'View only' : 'Interactive'}</p><button>Desktop control</button></main></body></html>`;
    res.writeHead(200, { 'Content-Type': 'text/html', 'Content-Length': Buffer.byteLength(html) });
    return res.end(html);
  }
  if (req.headers.authorization !== 'Bearer hub-test-key') return json(res, { error: 'Unauthorized' }, 401);
  if (req.method === 'GET' && url.pathname === '/api/agents') return json(res, { agents });
  if (req.method === 'GET' && url.pathname === '/api/workers') return json(res, { workers: [{ name: 'local', online: true, caps: { gpu: true, host_agents: true } }] });
  if (req.method === 'GET' && url.pathname === '/api/models') return json(res, { models: [{ name: 'test-model', provider: 'Kimi' }, { name: 'fast-model', provider: 'Local' }], default_model: 'test-model' });
  if (req.method === 'GET' && url.pathname === '/api/templates') return json(res, { templates: [{ name: 'Grace', role: 'Verification specialist', avatar: 'worker-bee' }] });
  if (req.method === 'GET' && url.pathname === '/api/threads') return json(res, { threads });
  if (req.method === 'POST' && url.pathname === '/api/agents') {
    const input = JSON.parse((await body(req)).toString());
    requests.agents.push({ method: 'create', ...input });
    const agent = { id: `agent-${input.name.toLowerCase()}`, name: input.name, avatar: input.avatar || '', worker: input.worker || 'local', state: 'ready', busy: false, model: input.model || '', role: input.role || '', workspace_mode: input.workspace_mode, type: input.type || '', memory: input.memory || '', cpus: input.cpus || 0, thinking_level: input.thinking_level || '', host_agent: input.host_agent || false, created_at: '2026-11-13T00:00:00Z', updated_at: '2026-11-13T00:00:00Z' };
    agents.push(agent);
    return json(res, { agent });
  }
  if (url.pathname === '/api/skills' && req.method === 'GET') return json(res, { skills: sharedSkills });
  if (url.pathname === '/api/skills' && req.method === 'POST') {
    const input = JSON.parse((await body(req)).toString());
    requests.skills.push({ method: 'create-global', ...input });
    sharedSkills.push({ name: input.name, description: input.description, version: '1.0.0' });
    return json(res, { created: input.name }, 201);
  }
  const globalSkillMatch = url.pathname.match(/^\/api\/skills\/([^/]+)$/);
  if (globalSkillMatch && req.method === 'DELETE') {
    const name = decodeURIComponent(globalSkillMatch[1]);
    requests.skills.push({ method: 'delete-global', name });
    const index = sharedSkills.findIndex((skill) => skill.name === name);
    if (index >= 0) sharedSkills.splice(index, 1);
    return json(res, {});
  }
  const agentGlobalSkills = url.pathname.match(/^\/api\/agents\/([^/]+)\/global-skills$/);
  if (agentGlobalSkills) {
    const agentId = agentGlobalSkills[1];
    enabledSkills[agentId] ??= [];
    if (req.method === 'GET') return json(res, { skills: enabledSkills[agentId] });
    if (req.method === 'POST') {
      const input = JSON.parse((await body(req)).toString());
      requests.skills.push({ method: 'enable', agentId, ...input });
      enabledSkills[agentId] = enabledSkills[agentId].filter((name) => name !== input.name);
      if (input.enabled) enabledSkills[agentId].push(input.name);
      return json(res, {});
    }
  }
  const agentSkills = url.pathname.match(/^\/api\/agents\/([^/]+)\/skills(?:\/([^/]+))?$/);
  if (agentSkills) {
    const agentId = agentSkills[1];
    learnedSkills[agentId] ??= [];
    if (req.method === 'GET' && !agentSkills[2]) return json(res, { skills: learnedSkills[agentId] });
    if (req.method === 'POST' && !agentSkills[2]) {
      const input = JSON.parse((await body(req)).toString());
      requests.skills.push({ method: 'create-local', agentId, ...input });
      learnedSkills[agentId].push(input.name);
      return json(res, { created: input.name }, 201);
    }
    if (req.method === 'DELETE' && agentSkills[2]) {
      const name = decodeURIComponent(agentSkills[2]);
      requests.skills.push({ method: 'delete-local', agentId, name });
      learnedSkills[agentId] = learnedSkills[agentId].filter((item) => item !== name);
      return json(res, {});
    }
  }
  if (url.pathname === '/api/thread-groups') {
    if (req.method === 'GET') return json(res, { groups: threadGroups });
    if (req.method === 'POST') {
      const input = JSON.parse((await body(req)).toString());
      const group = { id: `group-${threadGroups.length + 1}`, name: input.name };
      threadGroups.push(group);
      return json(res, group, 201);
    }
  }
  const groupMatch = url.pathname.match(/^\/api\/thread-groups\/([^/]+)$/);
  if (groupMatch) {
    const group = threadGroups.find((item) => item.id === groupMatch[1]);
    if (!group) return json(res, { error: 'Not found' }, 404);
    if (req.method === 'PATCH') {
      Object.assign(group, JSON.parse((await body(req)).toString()));
      return json(res, group);
    }
    if (req.method === 'DELETE') {
      threadGroups.splice(threadGroups.indexOf(group), 1);
      for (const thread of threads) if (thread.group_id === group.id) thread.group_id = null;
      return json(res, {});
    }
  }
  const threadGroupMatch = url.pathname.match(/^\/api\/threads\/([^/]+)\/group$/);
  if (threadGroupMatch && req.method === 'PUT') {
    const thread = threads.find((item) => item.id === threadGroupMatch[1]);
    if (!thread) return json(res, { error: 'Not found' }, 404);
    thread.group_id = JSON.parse((await body(req)).toString()).group_id;
    return json(res, thread);
  }
  const agentMatch = url.pathname.match(/^\/api\/agents\/([^/]+)(\/start|\/stop|\/cancel|\/exec-log|\/screen|\/screen\/start)?$/);
  if (agentMatch) {
    const agent = agents.find((item) => item.id === agentMatch[1]);
    if (!agent) return json(res, { error: 'Not found' }, 404);
    const action = agentMatch[2] || '';
    if (req.method === 'PATCH' && !action) {
      const input = JSON.parse((await body(req)).toString());
      requests.agents.push({ method: 'update', id: agent.id, ...input });
      Object.assign(agent, input);
      return json(res, { agent });
    }
    if (req.method === 'DELETE' && !action) {
      const reassignTo = url.searchParams.get('reassign_to') || '';
      requests.agents.push({ method: 'delete', id: agent.id, reassignTo });
      agents.splice(agents.indexOf(agent), 1);
      for (const routine of routines) if (routine.agent_id === agent.id) routine.agent_id = reassignTo;
      for (let index = threads.length - 1; index >= 0; --index) {
        const thread = threads[index];
        if (!thread.agent_ids.includes(agent.id)) continue;
        if (thread.agent_ids.length <= 1) threads.splice(index, 1);
        else thread.agent_ids = thread.agent_ids.filter((id) => id !== agent.id);
      }
      return json(res, {});
    }
    if (req.method === 'POST' && action === '/cancel') {
      requests.agents.push({ method: 'cancel', id: agent.id });
      return json(res, { cancelled: 1 });
    }
    if (req.method === 'POST' && (action === '/start' || action === '/stop')) {
      agent.state = action === '/start' ? 'ready' : 'stopped';
      requests.agents.push({ method: action.slice(1), id: agent.id });
      return json(res, {});
    }
    if (req.method === 'GET' && action === '/exec-log') return json(res, { entries: [
      { ts: '2026-11-13T01:00:00Z', command: 'printf hello', exit_code: 0, timed_out: false, duration_ms: 4 },
    ] });
    if (req.method === 'POST' && action === '/screen/start') return json(res, {});
    if (req.method === 'GET' && action === '/screen') return json(res, { running: true });
  }
  if (req.method === 'POST' && url.pathname === '/api/threads') {
    const input = JSON.parse((await body(req)).toString());
    if (input.reuse_dm) return json(res, threads[0]);
    const thread = { id: `thread-${threads.length + 1}`, title: input.title || 'Team', kind: input.kind || 'group', agent_ids: input.agent_ids, unread: 0, last_activity: '2026-11-13T00:00:00Z', group_id: null };
    threads.push(thread);
    messages[thread.id] = [];
    return json(res, thread);
  }
  const threadMatch = url.pathname.match(/^\/api\/threads\/([^/]+)(\/clear)?$/);
  if (threadMatch) {
    const thread = threads.find((item) => item.id === threadMatch[1]);
    if (!thread) return json(res, { error: 'Not found' }, 404);
    if (req.method === 'POST' && threadMatch[2] === '/clear') {
      requests.threads.push({ method: 'clear', id: thread.id });
      messages[thread.id] = [];
      return json(res, {});
    }
    if (req.method === 'DELETE' && !threadMatch[2]) {
      requests.threads.push({ method: 'delete', id: thread.id });
      threads.splice(threads.indexOf(thread), 1);
      delete messages[thread.id];
      return json(res, {});
    }
  }
  const messageMatch = url.pathname.match(/^\/api\/threads\/([^/]+)\/messages$/);
  if (messageMatch) {
    const threadId = decodeURIComponent(messageMatch[1]);
    if (req.method === 'GET') {
      let items = messages[threadId] ?? [];
      const after = url.searchParams.get('after');
      const before = url.searchParams.get('before');
      if (after) items = items.slice(items.findIndex((item) => item.id === after) + 1);
      if (before) items = items.slice(0, items.findIndex((item) => item.id === before));
      return json(res, { messages: items, has_more: false });
    }
    if (req.method === 'POST') {
      const input = JSON.parse((await body(req)).toString());
      requests.messages.push({ threadId, ...input });
      const message = { id: `m${++messageCounter}`, sender_kind: 'user', sender_id: 'user', body: input.body, created_at: '2026-11-13T00:00:02Z', attachments: input.attachments ?? [] };
      messages[threadId] ??= [];
      messages[threadId].push(message);
      return json(res, { message });
    }
  }
  const activityMatch = url.pathname.match(/^\/api\/threads\/([^/]+)\/activity$/);
  if (activityMatch) return json(res, threadActivity);
  if (req.method === 'POST' && /\/read$/.test(url.pathname)) return json(res, {});
  if (req.method === 'POST' && /\/cancel$/.test(url.pathname)) { requests.cancellations.push(url.pathname); return json(res, {}); }
  const uploadMatch = url.pathname.match(/^\/api\/threads\/([^/]+)\/attachments$/);
  if (uploadMatch && req.method === 'POST') {
    const bytes = await body(req);
    const name = url.searchParams.get('name');
    requests.uploads.push({ name, bytes: bytes.toString() });
    return json(res, { name, path: `/uploads/${name}`, size: bytes.length });
  }
  if (req.method === 'GET' && url.pathname === '/api/files/download') {
    const requestedPath = url.searchParams.get('path');
    requests.downloads.push({ path: requestedPath, worker: url.searchParams.get('worker') });
    const bytes = requestedPath.endsWith('.png')
      ? Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=', 'base64')
      : Buffer.from('report body');
    res.writeHead(200, { 'Content-Type': requestedPath.endsWith('.png') ? 'image/png' : 'application/octet-stream', 'Content-Length': bytes.length });
    return res.end(bytes);
  }
  if (req.method === 'GET' && url.pathname === '/api/routines') return json(res, { routines });
  if (req.method === 'POST' && url.pathname === '/api/routines') {
    const input = JSON.parse((await body(req)).toString());
    requests.routines.push({ method: 'create', ...input });
    const routine = { id: `routine-${routines.length + 1}`, agent_id: input.agent_id, thread_id: 'routine-new', name: input.name, prompt: input.prompt, schedule: input.schedule, schedule_text: 'Scheduled', next_fire_at: '', enabled: true, last_fired_at: '' };
    routines.push(routine);
    return json(res, routine, 201);
  }
  const routineMatch = url.pathname.match(/^\/api\/routines\/([^/]+)(\/run|\/enabled)?$/);
  if (routineMatch) {
    const routine = routines.find((item) => item.id === routineMatch[1]);
    if (!routine) return json(res, { error: 'Not found' }, 404);
    if (req.method === 'PATCH' && !routineMatch[2]) {
      const input = JSON.parse((await body(req)).toString());
      requests.routines.push({ method: 'update', id: routine.id, ...input });
      Object.assign(routine, input, { schedule_text: 'Updated schedule' });
      return json(res, routine);
    }
    if (req.method === 'DELETE' && !routineMatch[2]) {
      requests.routines.push({ method: 'delete', id: routine.id });
      routines.splice(routines.indexOf(routine), 1);
      return json(res, {});
    }
    if (req.method === 'POST' && routineMatch[2] === '/run') {
      requests.routines.push({ method: 'run', id: routine.id });
      return json(res, {});
    }
    if (req.method === 'POST' && routineMatch[2] === '/enabled') {
      const input = JSON.parse((await body(req)).toString());
      requests.routines.push({ method: 'enabled', id: routine.id, ...input });
      routine.enabled = input.enabled;
      return json(res, {});
    }
  }
  return json(res, { error: `No mock for ${req.method} ${url.pathname}` }, 404);
}

function wav() {
  const sampleCount = 800;
  const bytes = Buffer.alloc(44 + sampleCount * 2);
  bytes.write('RIFF', 0); bytes.writeUInt32LE(bytes.length - 8, 4); bytes.write('WAVE', 8);
  bytes.write('fmt ', 12); bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20);
  bytes.writeUInt16LE(1, 22); bytes.writeUInt32LE(16000, 24); bytes.writeUInt32LE(32000, 28);
  bytes.writeUInt16LE(2, 32); bytes.writeUInt16LE(16, 34); bytes.write('data', 36);
  bytes.writeUInt32LE(sampleCount * 2, 40);
  return bytes;
}

async function chimpflow(req, res) {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/healthz') return json(res, { ok: true });
  if (req.headers.authorization !== 'Bearer chimp-test-key') return json(res, { error: 'Unauthorized' }, 401);
  if (url.pathname === '/v1/models') return json(res, { data: [{ id: 'kokoro-82m' }, { id: 'pocket-tts' }] });
  if (url.pathname === '/v1/voices') {
    const model = url.searchParams.get('model');
    return json(res, { data: (model === 'pocket-tts' ? ['alice', 'bob'] : ['af_heart', 'am_adam']).map((id) => ({ id })) });
  }
  if (url.pathname === '/v1/audio/speech' && req.method === 'POST') {
    requests.speech.push(JSON.parse((await body(req)).toString()));
    const audio = wav();
    res.writeHead(200, { 'Content-Type': 'audio/wav', 'Content-Length': audio.length });
    return res.end(audio);
  }
  if (url.pathname === '/v1/audio/transcriptions' && req.method === 'POST') {
    await body(req);
    ++requests.transcriptions;
    return json(res, { text: 'This is a voice test' });
  }
  return json(res, { error: 'Unknown ChimpFlow endpoint' }, 404);
}

async function listen(handler) {
  const server = http.createServer((req, res) => { void handler(req, res).catch((error) => json(res, { error: String(error) }, 500)); });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  return { server, url: `http://127.0.0.1:${address.port}` };
}

test.beforeAll(async () => {
  ({ server: hubServer, url: hubUrl } = await listen(hub));
  ({ server: chimpServer, url: chimpUrl } = await listen(chimpflow));
  temporary = await mkdtemp(path.join(os.tmpdir(), 'hive-electron-e2e-'));
  const userData = path.join(temporary, 'user-data');
  const legacy = path.join(temporary, 'empty-legacy');
  const legacyData = path.join(temporary, 'empty-legacy-data');
  await mkdir(userData, { recursive: true });
  await mkdir(legacy, { recursive: true });
  await mkdir(legacyData, { recursive: true });
  await writeFile(path.join(userData, 'settings.json'), JSON.stringify({
    hubServer: hubUrl, hubApiKey: 'hub-test-key', chimpflowUrl: chimpUrl,
    chimpflowModel: 'kokoro-82m', voices: {}, displayName: 'Lee', userAvatar: '', masterVolume: 0,
    selectedAgentId: '', selectedThreadId: '',
  }));
  await writeFile(path.join(userData, 'chimpflow.key'), 'chimp-test-key\n', { mode: 0o600 });
  app = await electron.launch({
    chromiumSandbox: true,
    args: ['.', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--disable-dev-shm-usage'],
    cwd: project,
    env: { ...process.env, HIVE_CLIENT_USER_DATA: userData, HIVE_CLIENT_LEGACY_CONFIG_HOME: legacy, HIVE_CLIENT_LEGACY_DATA_HOME: legacyData, HIVE_CLIENT_DOWNLOADS: path.join(temporary, 'downloads') },
  });
  page = await app.firstWindow();
  page.on('pageerror', (error) => { throw error; });
  await expect(page.locator('#hub-label')).toBeVisible();
});

test.afterAll(async () => {
  await app?.close();
  await new Promise((resolve) => hubServer?.close(resolve));
  await new Promise((resolve) => chimpServer?.close(resolve));
  if (temporary) await rm(temporary, { recursive: true, force: true });
});

test('loads the roster, opens a DM, and enters the voice panel', async () => {
  await page.locator('[data-agent-id="agent-bob"]').click();
  await expect(page.locator('#voice-panel')).toBeVisible();
  await expect(page.locator('#voice-agent-name')).toHaveText('Bob');
  await expect(page.locator('#message-list')).toContainText('Hello from Bob.');
  await expect(page.locator('#talk-button')).toHaveText('TALK');
});

test('operates Settings and model/voice dropdowns entirely through Chromium DOM', async () => {
  await page.getByRole('button', { name: 'Settings', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Settings' });
  await expect(dialog).toBeVisible();
  expect(await dialog.evaluate((node) => node instanceof HTMLDialogElement)).toBe(true);
  await page.getByRole('tab', { name: 'ChimpFlow' }).click();
  await expect(page.getByLabel('Speech model')).toBeEnabled();
  await page.getByLabel('Speech model').selectOption('pocket-tts');
  const voice = page.getByLabel('Voice for Bob');
  await expect(voice).toBeEnabled();
  await voice.selectOption('alice');
  await expect.poll(() => requests.speech.some((item) => item.model === 'pocket-tts' && item.voice === 'alice')).toBe(true);
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(dialog).not.toBeVisible();
  await expect.poll(async () => {
    const saved = JSON.parse(await readFile(path.join(temporary, 'user-data', 'settings.json'), 'utf8'));
    return saved.chimpflowModel === 'pocket-tts' && saved.voices['pocket-tts']['agent-bob'] === 'alice';
  }).toBe(true);
});

test('sends text and uploads through the HTML file input without an OS dialog', async () => {
  await page.locator('#message-input').fill('A keyboard message');
  await page.locator('#send-button').click();
  await expect.poll(() => requests.messages.some((item) => item.body === 'A keyboard message' && item.source === 'user')).toBe(true);
  await page.locator('#attachment-input').setInputFiles({ name: 'note.txt', mimeType: 'text/plain', buffer: Buffer.from('attachment body') });
  await expect(page.locator('#pending-attachments')).toContainText('note.txt');
  await page.locator('#message-input').fill('Message with attachment');
  await page.locator('#send-button').click();
  await expect.poll(() => requests.uploads.some((item) => item.name === 'note.txt' && item.bytes === 'attachment body')).toBe(true);
  await expect.poll(() => requests.messages.some((item) => item.body === 'Message with attachment' && item.attachments?.[0]?.name === 'note.txt')).toBe(true);
});

test('previews media and downloads files without native dialogs', async () => {
  await page.getByRole('button', { name: 'Preview pixel.png', exact: true }).click();
  const image = page.locator('.attachment-image');
  await expect(image).toBeVisible();
  expect(await image.evaluate((node) => node.src.startsWith('blob:'))).toBe(true);
  await image.click();
  const preview = page.getByRole('dialog', { name: 'pixel.png' });
  await expect(preview).toBeVisible();
  expect(await preview.evaluate((node) => node instanceof HTMLDialogElement)).toBe(true);
  await page.getByRole('button', { name: 'Close attachment preview' }).click();
  await page.getByRole('button', { name: 'Download report.txt', exact: true }).click();
  await expect.poll(() => requests.downloads.some((item) => item.path === 'output/report.txt' && item.worker === 'local')).toBe(true);
  expect(await readFile(path.join(temporary, 'downloads', 'report.txt'), 'utf8')).toBe('report body');
});

test('creates, edits, runs, toggles, and deletes scheduled tasks in HTML dialogs', async () => {
  await page.getByRole('button', { name: 'Scheduled tasks', exact: true }).click();
  const manager = page.getByRole('dialog', { name: 'Scheduled tasks', exact: true });
  await expect(manager).toBeVisible();
  await expect(manager).toContainText('Morning brief');
  await page.getByLabel('Enable Morning brief').uncheck();
  await expect.poll(() => requests.routines.some((item) => item.method === 'enabled' && item.enabled === false)).toBe(true);
  await page.getByRole('button', { name: 'Run Morning brief now' }).click();
  await expect.poll(() => requests.routines.some((item) => item.method === 'run' && item.id === 'routine-1')).toBe(true);
  await page.getByRole('button', { name: 'Add scheduled task' }).click();
  await page.locator('#task-name').fill('Evening review');
  await page.locator('#task-prompt').fill('Review the day');
  await page.locator('#task-schedule-type').selectOption('weekly');
  await page.locator('#task-weekly-time').fill('18:30');
  await page.locator('#task-weekly-days input[value="6"]').check();
  await page.getByRole('button', { name: 'Create', exact: true }).click();
  await expect.poll(() => requests.routines.some((item) => item.method === 'create' && item.schedule?.days?.includes(6))).toBe(true);
  await expect(manager).toContainText('Evening review');
  await page.getByRole('button', { name: 'Edit Evening review' }).click();
  await page.locator('#task-name').fill('Evening summary');
  await page.locator('#task-schedule-type').selectOption('interval');
  await page.locator('#task-interval').fill('15');
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect.poll(() => requests.routines.some((item) => item.method === 'update' && item.schedule?.every_minutes === 15)).toBe(true);
  await page.getByRole('button', { name: 'Delete Evening summary' }).click();
  const confirmation = page.getByRole('dialog', { name: 'Delete scheduled task?' });
  await expect(confirmation).toBeVisible();
  expect(await confirmation.evaluate((node) => node instanceof HTMLDialogElement)).toBe(true);
  await confirmation.getByRole('button', { name: 'Delete', exact: true }).click();
  await expect.poll(() => requests.routines.some((item) => item.method === 'delete')).toBe(true);
  await expect(manager).not.toContainText('Evening summary');
  await page.getByRole('button', { name: 'Close scheduled tasks' }).click();
});

test('creates agents and group threads in HTML dialogs', async () => {
  await page.locator('#voice-back-button').click();
  await page.getByRole('button', { name: '＋ New agent' }).click();
  const editor = page.getByRole('dialog', { name: 'New agent' });
  await editor.getByLabel('Template', { exact: true }).selectOption('Grace');
  await expect(editor.getByLabel('Name', { exact: true })).toHaveValue('Grace');
  await expect(editor.getByLabel('Role / instructions')).toHaveValue('Verification specialist');
  await editor.getByLabel('Model', { exact: true }).selectOption('test-model');
  await editor.getByLabel('Thinking level', { exact: true }).selectOption('high');
  await editor.getByLabel('Agent type', { exact: true }).selectOption('gpu');
  await editor.getByLabel('Memory limit', { exact: true }).fill('8g');
  await editor.getByLabel('CPU limit', { exact: true }).fill('3');
  await page.getByRole('button', { name: 'Create agent' }).click();
  await expect(page.locator('[data-agent-id="agent-grace"]')).toBeVisible();
  await expect.poll(() => requests.agents.some((item) => item.method === 'create' && item.model === 'test-model' && item.type === 'gpu' && item.memory === '8g' && item.cpus === 3 && item.thinking_level === 'high' && item.avatar === 'worker-bee')).toBe(true);
  await page.getByRole('button', { name: '＋ New thread' }).click();
  await page.getByLabel('Title', { exact: true }).fill('Release crew');
  await page.getByLabel('Bob', { exact: true }).check();
  await page.getByLabel('Grace', { exact: true }).check();
  await page.getByRole('button', { name: 'Create thread' }).click();
  await expect(page.locator('#thread-title')).toHaveText('Release crew');
});

test('edits, inspects, starts, stops, schedules, and deletes an agent through Chromium UI', async () => {
  await page.getByRole('button', { name: 'Manage Grace' }).click();
  await page.getByRole('button', { name: 'Edit agent', exact: true }).click();
  const editor = page.getByRole('dialog', { name: 'Edit Grace' });
  await expect(editor.getByLabel('Role / instructions')).toHaveValue('Verification specialist');
  await editor.getByLabel('Role / instructions').fill('Release verification specialist');
  await editor.getByLabel('Model', { exact: true }).selectOption('test-model');
  await editor.getByLabel('Thinking level', { exact: true }).selectOption('low');
  await editor.getByLabel('Memory limit', { exact: true }).fill('16g');
  await editor.getByLabel('CPU limit', { exact: true }).fill('4');
  await editor.getByRole('button', { name: 'Save', exact: true }).click();
  await expect.poll(() => requests.agents.some((item) => item.method === 'update' && item.id === 'agent-grace' && item.model === 'test-model' && item.memory === '16g' && item.cpus === 4 && item.thinking_level === 'low')).toBe(true);

  await page.getByRole('button', { name: 'Manage Grace' }).click();
  await page.getByRole('button', { name: 'Exec log', exact: true }).click();
  await expect(page.locator('#exec-log-content')).toContainText('printf hello');
  await expect(page.locator('#exec-log-status')).toHaveText('1 entry — newest first');
  await page.getByRole('button', { name: 'Close exec log' }).click();

  const desktopPromise = app.waitForEvent('window');
  await page.getByRole('button', { name: 'Computer (view only)' }).click();
  const desktop = await desktopPromise;
  await desktop.waitForLoadState();
  await expect(desktop.getByRole('heading', { name: 'Grace desktop' })).toBeVisible();
  await expect(desktop.getByText('View only', { exact: true })).toBeVisible();
  expect(desktop.url()).toContain('view_only=1');
  expect(await desktop.evaluate(() => typeof window.hive)).toBe('undefined');
  await desktop.close();
  await page.getByRole('button', { name: 'Close agent management' }).click();

  await page.getByRole('button', { name: 'Manage Grace' }).click();
  await page.getByRole('button', { name: 'Inspector', exact: true }).click();
  await expect(page.getByRole('dialog', { name: 'Inspector — Grace' })).toContainText('16g');
  await page.getByRole('button', { name: 'Close inspector', exact: true }).click();
  await page.getByRole('button', { name: 'Cancel current work', exact: true }).click();
  await expect.poll(() => requests.agents.some((item) => item.method === 'cancel' && item.id === 'agent-grace')).toBe(true);
  await page.getByRole('button', { name: 'Stop container', exact: true }).click();
  await expect.poll(() => requests.agents.some((item) => item.method === 'stop' && item.id === 'agent-grace')).toBe(true);
  await page.getByRole('button', { name: 'Manage Grace' }).click();
  await page.getByRole('button', { name: 'Start container', exact: true }).click();
  await expect.poll(() => requests.agents.some((item) => item.method === 'start' && item.id === 'agent-grace')).toBe(true);

  await page.getByRole('button', { name: 'Manage Grace' }).click();
  await page.getByRole('button', { name: 'Schedule task', exact: true }).click();
  await page.locator('#task-add-button').click();
  await expect(page.locator('#task-agent')).toHaveValue('agent-grace');
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  await page.getByRole('button', { name: 'Close scheduled tasks' }).click();

  await page.getByRole('button', { name: 'Manage Grace' }).click();
  await page.getByRole('button', { name: 'Delete agent', exact: true }).click();
  const confirmation = page.getByRole('dialog', { name: 'Delete agent' });
  await expect(confirmation).toBeVisible();
  await confirmation.getByRole('button', { name: 'Delete', exact: true }).click();
  await expect.poll(() => requests.agents.some((item) => item.method === 'delete' && item.id === 'agent-grace')).toBe(true);
  await expect(page.locator('[data-agent-id="agent-grace"]')).toHaveCount(0);
  await expect(page.locator('#thread-title')).toHaveText('Release crew');
});

test('completes Talk, Send, transcription, and voice-source posting', async () => {
  await page.locator('[data-agent-id="agent-bob"]').click();
  await expect(page.locator('#voice-panel')).toBeVisible();
  const historySpeech = requests.speech.length;
  await page.waitForTimeout(1700);
  expect(requests.speech.length).toBe(historySpeech);
  await page.locator('#talk-button').click();
  await expect(page.locator('#talk-button')).toHaveText('SEND');
  // Let the fake microphone produce a real non-empty WebM clip.
  await page.waitForTimeout(600);
  await page.locator('#talk-button').click();
  await expect.poll(() => requests.transcriptions, { timeout: 20_000 }).toBeGreaterThan(0);
  await expect.poll(() => requests.messages.some((item) => item.body === 'This is a voice test' && item.source === 'voice')).toBe(true);
  await expect(page.locator('#message-list')).toContainText('This is a voice test');
  expect(requests.speech.length).toBe(historySpeech);
  messages['dm-bob'].push({ id: 'fresh-reply', sender_kind: 'agent', sender_id: 'agent-bob', sender_name: 'Bob', body: 'Fresh reply.', created_at: '2026-11-13T00:00:03Z', attachments: [] });
  await expect.poll(() => requests.speech.some((item) => item.input === 'Fresh reply.')).toBe(true);
  expect(requests.speech.filter((item) => item.input === 'Hello from Bob.').length).toBe(0);
  threadActivity.busy = true;
  agents.find((item) => item.id === 'agent-bob').busy = true;
  await page.waitForTimeout(1800);
  await page.locator('#voice-stop-button').click();
  await expect.poll(() => requests.cancellations.some((item) => item === '/api/threads/dm-bob/cancel')).toBe(true);
  await expect(page.locator('#voice-status')).toContainText('Stopping');
  threadActivity.busy = false;
  agents.find((item) => item.id === 'agent-bob').busy = false;
  await expect(page.locator('#voice-status')).toHaveText('Cancelled');
  await page.locator('#voice-back-button').click();
});

test('manages shared and learned skills in accessible HTML dialogs', async () => {
  await page.getByRole('button', { name: 'Manage Bob' }).click();
  await page.getByRole('button', { name: 'Skills', exact: true }).click();
  const manager = page.getByRole('dialog', { name: 'Skills — Bob' });
  await expect(manager).toBeVisible();
  await expect(manager).toContainText('code-review');
  await page.getByLabel('Enable testing for Bob').check();
  await expect.poll(() => requests.skills.some((item) => item.method === 'enable' && item.name === 'testing' && item.enabled)).toBe(true);

  await page.getByRole('button', { name: 'New skill', exact: true }).click();
  await page.locator('#skill-name').fill('shared-helper');
  await page.locator('#skill-description').fill('Shared test helper');
  await page.locator('#skill-content').fill('Use the shared test helper.');
  await page.getByRole('button', { name: 'Create skill', exact: true }).click();
  await expect.poll(() => requests.skills.some((item) => item.method === 'create-global' && item.name === 'shared-helper')).toBe(true);
  await page.getByRole('button', { name: 'Delete shared skill shared-helper' }).click();
  const sharedConfirmation = page.getByRole('dialog', { name: 'Delete shared skill?' });
  expect(await sharedConfirmation.evaluate((node) => node instanceof HTMLDialogElement)).toBe(true);
  await sharedConfirmation.getByRole('button', { name: 'Delete', exact: true }).click();
  await expect.poll(() => requests.skills.some((item) => item.method === 'delete-global' && item.name === 'shared-helper')).toBe(true);

  await page.getByRole('button', { name: 'New skill', exact: true }).click();
  await page.locator('#skill-scope').selectOption('agent');
  await page.locator('#skill-name').fill('release-notes');
  await page.locator('#skill-description').fill('Write release notes');
  await page.locator('#skill-content').fill('Write concise release notes.');
  await page.getByRole('button', { name: 'Create skill', exact: true }).click();
  await expect.poll(() => requests.skills.some((item) => item.method === 'create-local' && item.name === 'release-notes')).toBe(true);
  await page.getByRole('button', { name: 'Delete learned skill release-notes' }).click();
  await page.getByRole('dialog', { name: 'Delete learned skill?' }).getByRole('button', { name: 'Delete', exact: true }).click();
  await expect.poll(() => requests.skills.some((item) => item.method === 'delete-local' && item.name === 'release-notes')).toBe(true);
  await page.getByRole('button', { name: 'Close skills' }).click();
});

test('clears and deletes threads with in-app confirmations', async () => {
  await page.getByRole('button', { name: 'Manage thread Release crew' }).click();
  await page.getByRole('button', { name: 'Clear history', exact: true }).click();
  await page.getByRole('dialog', { name: 'Clear thread history?' }).getByRole('button', { name: 'Clear', exact: true }).click();
  await expect.poll(() => requests.threads.some((item) => item.method === 'clear' && item.id === 'thread-2')).toBe(true);
  await page.getByRole('button', { name: 'Manage thread Release crew' }).click();
  await page.getByRole('button', { name: 'Delete thread', exact: true }).click();
  const confirmation = page.getByRole('dialog', { name: 'Delete thread?' });
  expect(await confirmation.evaluate((node) => node instanceof HTMLDialogElement)).toBe(true);
  await confirmation.getByRole('button', { name: 'Delete', exact: true }).click();
  await expect.poll(() => requests.threads.some((item) => item.method === 'delete' && item.id === 'thread-2')).toBe(true);
  await expect(page.locator('[data-thread-id="thread-2"]')).toHaveCount(0);
});

test('organizes threads into persistent custom and smart groups', async () => {
  await page.getByRole('button', { name: 'New thread group', exact: true }).click();
  await page.getByRole('textbox', { name: 'Name', exact: true }).fill('Projects');
  await page.getByRole('button', { name: 'Save group' }).click();
  await expect(page.getByRole('button', { name: 'Collapse Projects' })).toBeVisible();
  await page.getByRole('button', { name: 'Manage thread Bob', exact: true }).click();
  await page.getByLabel('Group', { exact: true }).selectOption('group-1');
  await expect.poll(() => threads.find((thread) => thread.id === 'dm-bob').group_id).toBe('group-1');
  await page.getByRole('button', { name: 'Close thread management' }).click();
  await expect(page.locator('.thread-group-heading').filter({ hasText: 'Projects' }).locator('[data-thread-id="dm-bob"]')).toHaveCount(0);
  await expect(page.locator('.thread-group-heading').filter({ hasText: 'Projects' })).toContainText('1');
  await page.getByRole('button', { name: 'Collapse Projects' }).click();
  await expect(page.getByRole('button', { name: 'Expand Projects' })).toBeVisible();
  await expect.poll(async () => JSON.parse(await readFile(path.join(temporary, 'user-data', 'thread-ui.json'), 'utf8')).collapsed).toContain('group-1');
  await page.getByRole('button', { name: 'Expand Projects' }).click();
  await page.locator('summary[aria-label="Manage group Projects"]').click();
  await page.getByRole('button', { name: 'Rename group', exact: true }).click();
  await page.getByRole('textbox', { name: 'Name', exact: true }).fill('Active projects');
  await page.getByRole('button', { name: 'Save group' }).click();
  await expect(page.getByRole('button', { name: 'Collapse Active projects' })).toBeVisible();
  await page.locator('summary[aria-label="Manage group Active projects"]').click();
  await page.getByRole('button', { name: 'Delete group', exact: true }).click();
  await page.getByRole('button', { name: 'Delete group', exact: true }).last().click();
  await expect.poll(() => threadGroups.length).toBe(0);
  await expect.poll(() => threads.find((thread) => thread.id === 'dm-bob').group_id).toBeNull();
});

test('renderer is isolated from Node and Electron primitives', async () => {
  const privileges = await page.evaluate(() => ({
    require: typeof window.require, process: typeof window.process,
    hive: typeof window.hive?.getSettings,
  }));
  expect(privileges).toEqual({ require: 'undefined', process: 'undefined', hive: 'function' });
});
