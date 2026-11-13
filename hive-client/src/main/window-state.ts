import { mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import path from 'node:path';

export interface WindowState {
  width: number;
  height: number;
}

export const DEFAULT_WINDOW_STATE: WindowState = { width: 1100, height: 760 };
const MIN_WIDTH = 760;
const MIN_HEIGHT = 560;
const MAX_DIMENSION = 16384;

/** Clamp a stored size into something the window can actually open with. */
export function clampWindowState(state: WindowState): WindowState {
  const width = Math.round(Number(state.width));
  const height = Math.round(Number(state.height));
  return {
    width: Number.isFinite(width) ? Math.min(MAX_DIMENSION, Math.max(MIN_WIDTH, width)) : DEFAULT_WINDOW_STATE.width,
    height: Number.isFinite(height) ? Math.min(MAX_DIMENSION, Math.max(MIN_HEIGHT, height)) : DEFAULT_WINDOW_STATE.height,
  };
}

/** Read the persisted size; any problem falls back to the default. */
export function loadWindowState(file: string): WindowState {
  try {
    const parsed: unknown = JSON.parse(readFileSync(file, 'utf8'));
    if (parsed && typeof parsed === 'object' && 'width' in parsed && 'height' in parsed) {
      return clampWindowState(parsed as WindowState);
    }
  } catch { /* missing or unreadable state falls through to the default */ }
  return { ...DEFAULT_WINDOW_STATE };
}

/** Persist the size atomically (tmp file + rename); failures are swallowed. */
export function saveWindowState(file: string, state: WindowState): WindowState {
  const clamped = clampWindowState(state);
  try {
    mkdirSync(path.dirname(file), { recursive: true });
    const temp = `${file}.tmp`;
    writeFileSync(temp, `${JSON.stringify(clamped)}\n`, { mode: 0o600 });
    renameSync(temp, file);
  } catch { /* losing window size is not worth crashing over */ }
  return clamped;
}
