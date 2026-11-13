import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { limitPeaks, measureLufs, normalizeWavLoudness, TARGET_LUFS, wavLufs } from '../main/loudness.js';

function makeWav(samples: Float64Array, rate = 24000): ArrayBuffer {
  const bytes = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(bytes);
  const writeAscii = (offset: number, value: string) => {
    for (let i = 0; i < value.length; ++i) view.setUint8(offset + i, value.charCodeAt(i));
  };
  writeAscii(0, 'RIFF'); view.setUint32(4, 36 + samples.length * 2, true);
  writeAscii(8, 'WAVE'); writeAscii(12, 'fmt ');
  view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, rate, true); view.setUint32(28, rate * 2, true);
  view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  writeAscii(36, 'data'); view.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; ++i) {
    const c = Math.max(-1, Math.min(1, samples[i]!));
    view.setInt16(44 + i * 2, Math.round(c < 0 ? c * 0x8000 : c * 0x7fff), true);
  }
  return bytes;
}

function sine(rate: number, seconds: number, frequency: number, amplitude: number): Float64Array {
  const out = new Float64Array(Math.round(rate * seconds));
  for (let i = 0; i < out.length; ++i) out[i] = amplitude * Math.sin(2 * Math.PI * frequency * i / rate);
  return out;
}

describe('measureLufs', () => {
  it('measures a 1 kHz tone near its level plus K-weighting', () => {
    // Amplitude 0.1 sine: RMS is -23.01 dBFS; K-weighting at the 1 kHz
    // reference frequency is ~0 dB, so it must read within half a dB of that.
    const measured = measureLufs(sine(24000, 3, 1000, 0.1), 24000);
    assert.ok(measured !== undefined);
    assert.ok(Math.abs(measured! - -23.0) < 0.5, `expected ≈ -23.0, got ${measured}`);
  });

  it('returns undefined for silence', () => {
    assert.equal(measureLufs(new Float64Array(24000), 24000), undefined);
  });

  it('ignores a silent tail when gating', () => {
    const tone = sine(24000, 2, 1000, 0.1);
    const withTail = new Float64Array(tone.length + 48000 * 8);
    withTail.set(tone);
    const measured = measureLufs(withTail, 24000);
    assert.ok(measured !== undefined && Math.abs(measured! - -23.0) < 0.8);  // ffmpeg ebur128 reads -23.3 on the same file
  });
});

describe('limitPeaks', () => {
  it('leaves content under the ceiling untouched', () => {
    const s = sine(24000, 1, 1000, 0.5);
    limitPeaks(s, 24000);
    let peak = 0;
    for (const x of s) peak = Math.max(peak, Math.abs(x));
    assert.ok(Math.abs(peak - 0.5) < 0.01);
  });

  it('shaves an over-ceiling burst down to the ceiling', () => {
    const s = new Float64Array(24000);
    for (let i = 1200; i < 1800; ++i) s[i] = 0.99;   // 25 ms burst at 0.99
    limitPeaks(s, 24000);
    let peak = 0;
    for (const x of s) peak = Math.max(peak, Math.abs(x));
    assert.ok(peak <= 0.96, `peak ${peak} above ceiling`);
  });
});

describe('normalizeWavLoudness (EBU R128)', () => {
  it('passes non-WAV bytes through unchanged', () => {
    const bytes = new Uint8Array([1, 2, 3, 4]).buffer as ArrayBuffer;
    assert.equal(normalizeWavLoudness(bytes), bytes);
  });

  it('passes silence through unchanged', () => {
    const wav = makeWav(new Float64Array(24000));
    const out = normalizeWavLoudness(wav);
    assert.equal(wavLufs(out), undefined);
  });

  it('gains a quiet clean tone up to the target', () => {
    const wav = makeWav(sine(24000, 3, 1000, 0.03));
    const out = normalizeWavLoudness(wav);
    const lufs = wavLufs(out);
    assert.ok(lufs !== undefined && Math.abs(lufs! - TARGET_LUFS) < 0.5, `got ${lufs}`);
  });

  it('does not let one full-scale outlier cap the whole utterance (regression)', () => {
    // Quiet speech-like content with a single click at full scale — the old
    // absolute-peak cap applied ~unity gain and the clip stayed quiet.
    const base = sine(24000, 4, 440, 0.045);
    const click = new Float64Array(base.length);
    click.set(base);
    click[48000] = 1.0;
    const wav = makeWav(click);
    const out = normalizeWavLoudness(wav);
    const lufs = wavLufs(out);
    assert.ok(lufs !== undefined && lufs! > TARGET_LUFS - 1.5, `still quiet: ${lufs}`);
    const view = new DataView(out);
    let peak = 0;
    const int16 = new Int16Array(out, 44, (out.byteLength - 44) / 2);
    for (const x of int16) peak = Math.max(peak, Math.abs(x));
    assert.ok(peak / 32768 <= 0.96, `peak ${peak / 32768} exceeds ceiling`);
    void view;
  });
});
