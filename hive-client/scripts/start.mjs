import { spawn } from 'node:child_process';
import { readFile, stat } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
const executable = require('electron');
const args = process.argv.slice(2);

async function readSysctl(name) {
  try { return (await readFile(`/proc/sys/kernel/${name}`, 'utf8')).trim(); } catch { return ''; }
}

async function namespaceSandboxAvailable() {
  // Ubuntu 23.10+ restricts unprivileged user namespaces via AppArmor; Chromium
  // then reports "No usable sandbox" and aborts even though the userns sysctls
  // look enabled, so a namespace-sandbox fallback must rule that out first.
  if ((await readSysctl('apparmor_restrict_unprivileged_userns')) === '1') return false;
  try {
    const enabled = (await readFile('/proc/sys/kernel/unprivileged_userns_clone', 'utf8')).trim() === '1';
    const maximum = Number((await readFile('/proc/sys/user/max_user_namespaces', 'utf8')).trim());
    return enabled && maximum > 0;
  } catch { return false; }
}

if (process.platform === 'linux') {
  const helper = path.join(path.dirname(executable), 'chrome-sandbox');
  let setuid = false;
  try {
    const info = await stat(helper);
    setuid = info.uid === 0 && Boolean(info.mode & 0o4000);
  } catch { /* Report the actionable failure below. */ }
  if (!setuid) {
    if (await namespaceSandboxAvailable()) {
      args.unshift('--disable-setuid-sandbox');
      console.warn('[hivemind] setuid helper is not installed; using the secure Linux namespace sandbox.');
    } else {
      console.error(`[hivemind] Electron cannot start its sandbox.\nRepair the setuid helper:\n  sudo chown root:root "${helper}"\n  sudo chmod 4755 "${helper}"\nAlternatively enable unprivileged Linux user namespaces. Refusing to use --no-sandbox.`);
      process.exit(1);
    }
  }
}

const child = spawn(executable, ['.', ...args], { stdio: 'inherit', env: process.env });
child.once('error', (error) => {
  console.error(`[hivemind] Could not execute Electron at ${executable}: ${error.stack || error}`);
  process.exitCode = 1;
});
child.once('exit', (code, signal) => {
  if (code !== 0) {
    console.error(`[hivemind] Electron exited ${signal ? `on ${signal}` : `with status ${code}`}. See the startup log in the HiveMind user-data/logs directory for application errors.`);
    process.exitCode = code ?? 1;
  }
});
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
