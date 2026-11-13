import { cp, mkdir, readFile, rm, writeFile, chmod, copyFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const execute = promisify(execFile);
const project = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const scripts = path.join(project, 'scripts');
const manifest = JSON.parse(await readFile(path.join(project, 'package.json'), 'utf8'));
const output = path.join(project, 'release');
const bundle = path.join(output, `hivemind-client-${manifest.version}-linux-x64`);
const archive = `${bundle}.tar.gz`;

// The user-facing "hivemind-client" is scripts/launcher.sh: it resolves the
// Chromium sandbox before starting Electron so a misconfigured sandbox fails
// with an actionable message instead of a silent abort (the packaged
// "won't open" freeze on Ubuntu 23.10+ AppArmor-restricted hosts). The raw
// Electron runtime ships as the private hivemind-client.bin next to it.
// scripts/install-template.sh installs the setuid sandbox helper once.

await rm(bundle, { recursive: true, force: true });
await rm(archive, { force: true });
await mkdir(path.join(bundle, 'resources', 'app'), { recursive: true });
await cp(path.join(project, 'node_modules', 'electron', 'dist'), bundle, { recursive: true });
await rm(path.join(bundle, 'resources', 'default_app.asar'), { force: true });
await cp(path.join(project, 'dist'), path.join(bundle, 'resources', 'app', 'dist'), { recursive: true });
await writeFile(path.join(bundle, 'resources', 'app', 'package.json'), JSON.stringify({
  name: manifest.name, version: manifest.version, description: manifest.description,
  type: 'module', main: 'dist/main/main.js',
}, null, 2) + '\n');

const binary = path.join(bundle, 'hivemind-client.bin');
await rm(binary, { force: true });
await cp(path.join(bundle, 'electron'), binary);
await rm(path.join(bundle, 'electron'));

const executable = path.join(bundle, 'hivemind-client');
await copyFile(path.join(scripts, 'launcher.sh'), executable);
await copyFile(path.join(scripts, 'install-template.sh'), path.join(bundle, 'install.sh'));
await chmod(executable, 0o755);
await chmod(path.join(bundle, 'install.sh'), 0o755);

// Exec is rewritten to an absolute path by install.sh; running the launcher
// directly from a terminal does not depend on the desktop entry.
await writeFile(path.join(bundle, 'org.hivemind.Client.desktop'), `[Desktop Entry]\nType=Application\nName=HiveMind\nComment=Talk with your HiveMind agents\nExec=hivemind-client\nIcon=org.hivemind.Client\nTerminal=false\nCategories=Network;Chat;\n`);

await execute('tar', ['-czf', archive, '-C', output, path.basename(bundle)]);
console.log(archive);
