import { expect, _electron as electron } from '@playwright/test';
import { mkdtemp, mkdir, readFile, rm, stat } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const execute = promisify(execFile);

const project = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const manifest = JSON.parse(await readFile(path.join(project, 'package.json'), 'utf8'));
const bundle = path.join(project, 'release', `hivemind-client-${manifest.version}-linux-x64`);
const temporary = await mkdtemp(path.join(os.tmpdir(), 'hive-package-smoke-'));
let app;
// The packaged app launches through the sandbox-aware wrapper, which refuses
// to fall back to --no-sandbox. On hosts where the setuid helper is not
// installed yet (a fresh unpack), install it non-interactively when sudo
// allows; otherwise there is no sandboxed way to launch Chromium here and
// the smoke test cannot run.
const helper = path.join(bundle, 'chrome-sandbox');
async function helperReady() {
  try {
    const info = await stat(helper);
    return info.uid === 0 && Boolean(info.mode & 0o4000);
  } catch { return false; }
}
const { stdout: mountOptions } = await execute('findmnt', ['-rn', '-o', 'OPTIONS', '--target', bundle]).catch(() => ({ stdout: '' }));
if (/\bnosuid\b/.test(mountOptions)) {
  console.warn(`SKIP: ${path.basename(bundle)} lives on a nosuid mount (${mountOptions.trim()}); move the bundle to a setuid-capable filesystem (e.g. your home directory) to run the packaged smoke test.`);
  await rm(temporary, { recursive: true, force: true });
  process.exit(0);
}
if (!(await helperReady())) {
  try {
    await execute('sudo', ['-n', 'chown', 'root:root', helper]);
    await execute('sudo', ['-n', 'chmod', '4755', helper]);
  } catch {
    console.warn(`SKIP: ${path.basename(bundle)} needs its sandbox helper installed once:
  sudo chown root:root ${helper}
  sudo chmod 4755 ${helper}
(or run ./install.sh in the bundle directory)`);
    await rm(temporary, { recursive: true, force: true });
    process.exit(0);
  }
}

try {
  const userData = path.join(temporary, 'user-data');
  const legacy = path.join(temporary, 'legacy');
  const legacyData = path.join(temporary, 'legacy-data');
  await mkdir(userData, { recursive: true });
  await mkdir(legacy, { recursive: true });
  await mkdir(legacyData, { recursive: true });
  app = await electron.launch({
    chromiumSandbox: true,
    executablePath: path.join(bundle, 'hivemind-client'),
    args: ['--disable-setuid-sandbox', '--disable-dev-shm-usage'],
    env: {
      ...process.env,
      HIVE_CLIENT_USER_DATA: userData,
      HIVE_CLIENT_LEGACY_CONFIG_HOME: legacy,
      HIVE_CLIENT_LEGACY_DATA_HOME: legacyData,
      HIVE_CLIENT_DOWNLOADS: path.join(temporary, 'downloads'),
    },
  });
  const argv = await app.evaluate(() => process.argv);
  expect(argv).not.toContain('--no-sandbox');
  expect(argv).toContain('--disable-setuid-sandbox');
  const page = await app.firstWindow();
  await expect(page).toHaveTitle('HiveMind');
  await expect(page.getByRole('button', { name: '＋ New agent' })).toBeVisible();
  await expect(page.getByRole('dialog', { name: 'Connect to hive-server' })).toBeVisible();
  const isolated = await page.evaluate(() => ({
    require: typeof window.require,
    process: typeof window.process,
    hive: typeof window.hive?.getSettings,
  }));
  expect(isolated).toEqual({ require: 'undefined', process: 'undefined', hive: 'function' });
  console.log('Packaged Linux client launched with isolated settings and Chromium UI.');
} finally {
  await app?.close();
  await rm(temporary, { recursive: true, force: true });
}
