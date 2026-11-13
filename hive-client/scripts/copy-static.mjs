import { copyFile, cp, mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = path.join(root, 'src', 'renderer');
const destination = path.join(root, 'dist', 'renderer');
await mkdir(destination, { recursive: true });
for (const filename of ['index.html', 'styles.css']) {
  await copyFile(path.join(source, filename), path.join(destination, filename));
}
await cp(path.join(source, 'assets'), path.join(destination, 'assets'), { recursive: true });
