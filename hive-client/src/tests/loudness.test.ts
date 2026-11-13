import assert from 'node:assert/strict';
import test from 'node:test';
import { normalizeLoudness, normalizeWavLoudness } from '../main/loudness.js';

function rmsOf(samples: Int16Array): number {
  let sumSq = 0;
  for (const s of samples) sumSq += s * s;
  return Math.sqrt(sumSq / samples.length) / 32768;
}

function peakOf(samples: Int16Array): number {
  let peak = 0;
  for (const s of samples) peak = Math.max(peak, Math.abs(s));
  return peak / 32768;
}

function sine(amplitude: number, n: number): Int16Array {
  const pcm = new Int16Array(n);
  for (let i = 0; i < n; ++i) pcm[i] = Math.round(amplitude * 32767 * Math.sin(i * 0.05));
  return pcm;
}

test('quiet voice is gained up to the target RMS', () => {
  const pcm = sine(0.08, 8000);  // RMS ≈ 0.057 — the caro_davy case
  normalizeLoudness(pcm);
  assert.ok(Math.abs(rmsOf(pcm) - 0.106) <= 0.005, `rms ${rmsOf(pcm)}`);
  assert.ok(peakOf(pcm) <= 0.95);
});

test('loud voice is attenuated to the same target', () => {
  const pcm = sine(0.22, 8000);  // RMS ≈ 0.156
  normalizeLoudness(pcm);
  assert.ok(Math.abs(rmsOf(pcm) - 0.106) <= 0.005, `rms ${rmsOf(pcm)}`);
  assert.ok(peakOf(pcm) <= 0.95);
});

test('hot peaks never clip — the peak cap wins over the RMS target', () => {
  const pcm = sine(0.05, 8000);
  pcm[100] = 32000;   // one near-full-scale transient
  pcm[4000] = -32000;
  normalizeLoudness(pcm);
  assert.ok(peakOf(pcm) <= 0.95);
});

test('silence and empty input pass through untouched', () => {
  const silence = new Int16Array(4000);
  normalizeLoudness(silence);
  assert.equal(peakOf(silence), 0);
  const empty = new Int16Array(0);
  normalizeLoudness(empty);  // must not crash
  assert.equal(empty.length, 0);
});

test('very quiet voices get the full boost without clipping', () => {
  const pcm = sine(0.02, 8000);  // RMS ≈ 0.014 — the stuart_bell case
  normalizeLoudness(pcm);        // needs ~7.5x; gain cap 6.0 applies
  assert.ok(rmsOf(pcm) > 0.08);
  assert.ok(peakOf(pcm) <= 0.95);
});

test('degenerate extreme input is bounded by the gain clamp', () => {
  const pcm = sine(0.9, 8000);  // RMS ≈ 0.64 — far above target
  normalizeLoudness(pcm);
  assert.ok(rmsOf(pcm) >= 0.15);  // min-gain 0.25 → RMS ≈ 0.16, not 0.106
  assert.ok(peakOf(pcm) <= 0.95);
});

function wavWith(samples: Int16Array): ArrayBuffer {
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(buffer);
  const writeAscii = (offset: number, text: string) => {
    for (let i = 0; i < text.length; ++i) view.setUint8(offset + i, text.charCodeAt(i));
  };
  writeAscii(0, 'RIFF');
  view.setUint32(4, 36 + samples.length * 2, true);
  writeAscii(8, 'WAVE');
  writeAscii(12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);       // PCM
  view.setUint16(22, 1, true);       // mono
  view.setUint32(24, 16000, true);
  view.setUint32(28, 32000, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeAscii(36, 'data');
  view.setUint32(40, samples.length * 2, true);
  new Int16Array(buffer, 44, samples.length).set(samples);
  return buffer;
}

test('WAV wrapper normalizes the data chunk and keeps the header intact', () => {
  const pcm = sine(0.02, 8000);
  const bytes = normalizeWavLoudness(wavWith(pcm));
  const view = new DataView(bytes);
  assert.equal(view.getUint32(0, false), 0x52494646);  // RIFF
  const normalized = new Int16Array(bytes, 44, 8000);
  assert.ok(rmsOf(normalized) > 0.08);
});

test('non-PCM WAV passes through unchanged', () => {
  const buffer = wavWith(sine(0.02, 100));
  new DataView(buffer).setUint16(20, 3, true);  // IEEE float, not PCM
  const bytes = normalizeWavLoudness(buffer);
  const original = new Int16Array(buffer, 44, 100);
  const result = new Int16Array(bytes, 44, 100);
  assert.deepEqual(Array.from(result), Array.from(original));
});
