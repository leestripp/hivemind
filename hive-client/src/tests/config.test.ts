import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { normalizeChimpFlowUrl, normalizeSettings, SettingsStore } from '../main/config.js';

test('ChimpFlow URLs are restricted to plain HTTP loopback', () => {
  assert.equal(normalizeChimpFlowUrl('http://127.0.0.1:8090/'), 'http://127.0.0.1:8090');
  assert.throws(() => normalizeChimpFlowUrl('http://example.com:8090'), /localhost/);
  assert.throws(() => normalizeChimpFlowUrl('https://127.0.0.1:8090'), /localhost/);
});

test('legacy flat voice ids migrate only to Pocket TTS', () => {
  const settings = normalizeSettings({ voices: { agent1: 'alice', 'kokoro-82m': { agent2: 'af_heart' } } });
  assert.deepEqual(settings.voices, { 'pocket-tts': { agent1: 'alice' }, 'kokoro-82m': { agent2: 'af_heart' } });
});

test('settings migrate legacy credentials and persist without the ChimpFlow key', async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), 'hive-settings-test-'));
  const legacy = path.join(root, 'legacy');
  const userData = path.join(root, 'user-data');
  await mkdir(path.join(legacy, 'hivemind'), { recursive: true });
  await mkdir(path.join(legacy, 'hive-client-voice'), { recursive: true });
  await writeFile(path.join(legacy, 'hivemind', 'config.json'), JSON.stringify({
    server: 'http://127.0.0.1:9999', api_key: 'hub-secret', display_name: 'Lee', avatar: 'ant',
  }));
  await writeFile(path.join(legacy, 'hive-client-voice', 'prefs.json'), JSON.stringify({
    chimpflow_url: 'http://127.0.0.1:8090', chimpflow_model: 'pocket-tts', voices: { agent1: 'alice' },
  }));
  await writeFile(path.join(legacy, 'hive-client-voice', 'chimpflow.key'), 'voice-secret\n');
  const previousRoot = process.env.HIVE_CLIENT_LEGACY_CONFIG_HOME;
  const previousKey = process.env.CHIMPFLOW_KEY;
  process.env.HIVE_CLIENT_LEGACY_CONFIG_HOME = legacy;
  delete process.env.CHIMPFLOW_KEY;
  try {
    const store = new SettingsStore(userData);
    await store.load();
    const view = store.view();
    assert.equal(view.hubApiKey, 'hub-secret');
    assert.equal(view.chimpflowKey, 'voice-secret');
    assert.equal(view.displayName, 'Lee');
    await store.update({ chimpflowModel: 'kokoro-82m' });
    const saved = await readFile(path.join(userData, 'settings.json'), 'utf8');
    assert.equal(saved.includes('voice-secret'), false);
    assert.equal(saved.includes('hub-secret'), true);
    assert.equal((await stat(path.join(userData, 'chimpflow.key'))).mode & 0o777, 0o600);
  } finally {
    if (previousRoot === undefined) delete process.env.HIVE_CLIENT_LEGACY_CONFIG_HOME;
    else process.env.HIVE_CLIENT_LEGACY_CONFIG_HOME = previousRoot;
    if (previousKey === undefined) delete process.env.CHIMPFLOW_KEY;
    else process.env.CHIMPFLOW_KEY = previousKey;
  }
});
