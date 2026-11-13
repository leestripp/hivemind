import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {
  clampWindowState, DEFAULT_WINDOW_STATE, loadWindowState, saveWindowState,
} from '../main/window-state.js';

test('clampWindowState enforces minimum and maximum dimensions', () => {
  assert.deepEqual(clampWindowState({ width: 100, height: 50 }), { width: 760, height: 560 });
  assert.deepEqual(clampWindowState({ width: 99999, height: 99999 }), { width: 16384, height: 16384 });
  assert.deepEqual(clampWindowState({ width: 1234.6, height: 777.2 }), { width: 1235, height: 777 });
});

test('clampWindowState rejects non-finite dimensions', () => {
  assert.deepEqual(clampWindowState({ width: NaN, height: Infinity }), DEFAULT_WINDOW_STATE);
});

test('loadWindowState falls back to defaults for missing or garbage files', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'hive-window-state-'));
  const file = path.join(dir, 'state.json');
  assert.deepEqual(loadWindowState(file), DEFAULT_WINDOW_STATE);
  writeFileSync(file, 'not json');
  assert.deepEqual(loadWindowState(file), DEFAULT_WINDOW_STATE);
  writeFileSync(file, '{"width": 800}');
  assert.deepEqual(loadWindowState(file), DEFAULT_WINDOW_STATE);
  rmSync(dir, { recursive: true, force: true });
});

test('saveWindowState round-trips through loadWindowState', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'hive-window-state-'));
  const file = path.join(dir, 'state.json');
  const saved = saveWindowState(file, { width: 1280, height: 900 });
  assert.deepEqual(saved, { width: 1280, height: 900 });
  assert.deepEqual(loadWindowState(file), { width: 1280, height: 900 });
  rmSync(dir, { recursive: true, force: true });
});
