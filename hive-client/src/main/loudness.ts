// ==== Per-utterance loudness normalization, EBU R128 / ITU-R BS.1770 style ====
//
// Why not plain RMS-to-target with an absolute-peak cap (the old approach):
// a single outlier sample (TTS click, plosive overshoot) at ~full scale capped
// the gain for the WHOLE utterance, so long clips with one transient played
// several dB quiet while clean short clips hit the target — exactly the
// "short loud, long soft" report. The professional chain is instead:
//
//   1. Measure gated integrated loudness (LUFS) — K-weighted (perceptual),
//      400 ms gated blocks so silence and outliers do not skew the figure.
//   2. Apply one static gain to hit the target LUFS.
//   3. Run a lookahead limiter at the ceiling so the rare clipped transient
//      is shaved in milliseconds instead of capping the entire file.
//
// The LUFS meter is validated against ffmpeg's ebur128 (BS.1770-4).

export const TARGET_LUFS = -19;   // calibrated so existing "good" clips keep their level
const CEILING = 0.95;             // fraction of full scale
const MIN_GAIN = 0.25;
const MAX_GAIN = 6.0;
const ABSOLUTE_GATE_LUFS = -70;
const RELATIVE_GATE_LU = 10;
const BLOCK_SECONDS = 0.4;
const HOP_SECONDS = 0.1;

// ---- WAV parsing (PCM16) ----

interface WavData {
  view: DataView;
  pcm16: boolean;
  rate: number;
  channels: number;
  dataStart: number;
  dataLength: number;
}

function parseWav(bytes: ArrayBuffer): WavData | undefined {
  const view = new DataView(bytes);
  if (bytes.byteLength < 44 || view.getUint32(0, false) !== 0x52494646 || view.getUint32(8, false) !== 0x57415645) {
    return undefined;
  }
  let offset = 12;
  let dataStart = -1;
  let dataLength = 0;
  let pcm16 = false;
  let rate = 0;
  let channels = 0;
  while (offset + 8 <= bytes.byteLength) {
    const id = view.getUint32(offset, false);
    const size = view.getUint32(offset + 4, true);
    const body = offset + 8;
    if (body + size > bytes.byteLength) break;
    if (id === 0x666d7420) {
      pcm16 = view.getUint16(body, true) === 1 && view.getUint16(body + 14, true) === 16;
      channels = view.getUint16(body + 2, true);
      rate = view.getUint32(body + 4, true);
    } else if (id === 0x64617461) {
      dataStart = body;
      dataLength = size;
    }
    offset = body + size + (size % 2);
  }
  if (!pcm16 || !rate || dataStart < 0 || dataLength < 2) return undefined;
  return { view, pcm16, rate, channels, dataStart, dataLength };
}

// ---- K-weighting (BS.1770-4), biquads derived for the actual sample rate ----

interface Biquad { b0: number; b1: number; b2: number; a1: number; a2: number; }

// RBJ cookbook with prewarping; close to the spec's analog-domain design.
function kWeighting(rate: number): [Biquad, Biquad] {
  const stage1 = shelfHigh(rate, 1681.974450955533, 0.7071752369554196, 4.0);
  const stage2 = hp(rate, 38.13547047141274, 0.5003270373253953);
  return [stage1, stage2];
}

function shelfHigh(rate: number, fc: number, q: number, gainDb: number): Biquad {
  const A = 10 ** (gainDb / 40);
  const w0 = 2 * Math.PI * fc / rate;
  const alpha = Math.sin(w0) / (2 * q);
  const cw = Math.cos(w0);
  const b0 = A * ((A + 1) + (A - 1) * cw + 2 * Math.sqrt(A) * alpha);
  const b1 = -2 * A * ((A - 1) + (A + 1) * cw);
  const b2 = A * ((A + 1) + (A - 1) * cw - 2 * Math.sqrt(A) * alpha);
  const a0 = (A + 1) - (A - 1) * cw + 2 * Math.sqrt(A) * alpha;
  const a1 = 2 * ((A - 1) - (A + 1) * cw);
  const a2 = (A + 1) - (A - 1) * cw - 2 * Math.sqrt(A) * alpha;
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}

function hp(rate: number, fc: number, q: number): Biquad {
  const w0 = 2 * Math.PI * fc / rate;
  const alpha = Math.sin(w0) / (2 * q);
  const cw = Math.cos(w0);
  const b0 = (1 + cw) / 2;
  const b1 = -(1 + cw);
  const b2 = (1 + cw) / 2;
  const a0 = 1 + alpha;
  const a1 = -2 * cw;
  const a2 = 1 - alpha;
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}

function filterInPlace(samples: Float64Array, biquads: Biquad[]): void {
  for (const { b0, b1, b2, a1, a2 } of biquads) {
    let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (let i = 0; i < samples.length; ++i) {
      const x = samples[i]!;
      const y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      samples[i] = y;
      x2 = x1; x1 = x; y2 = y1; y1 = y;
    }
  }
}

// ---- Integrated loudness (LUFS) with absolute + relative gating ----

export function measureLufs(samples: Float64Array, rate: number): number | undefined {
  if (!samples.length) return undefined;
  const filtered = new Float64Array(samples);
  filterInPlace(filtered, kWeighting(rate));
  const block = Math.max(1, Math.round(BLOCK_SECONDS * rate));
  const hop = Math.max(1, Math.round(HOP_SECONDS * rate));
  const energies: number[] = [];
  for (let start = 0; start + block <= filtered.length; start += hop) {
    let sum = 0;
    for (let i = start; i < start + block; ++i) sum += filtered[i]! * filtered[i]!;
    energies.push(sum / block);
  }
  // Pad the tail: BS.1770 pads the final partial block with zeros so short
  // utterances are not measured over fewer samples than the block.
  if (filtered.length >= hop && filtered.length % hop !== 0 || filtered.length < block) {
    let sum = 0;
    const fullBlocks = Math.floor(filtered.length / hop);
    const start = fullBlocks * hop;
    const count = filtered.length - start;
    for (let i = start; i < filtered.length; ++i) sum += filtered[i]! * filtered[i]!;
    if (count > 0) energies.push(sum / block);  // zero-padded tail block
  }
  const absolute = energies.filter((e) => loudnessOf(e) > ABSOLUTE_GATE_LUFS);
  if (!absolute.length) return undefined;
  const preliminary = loudnessOf(meanEnergy(absolute));
  const relativeThreshold = preliminary - RELATIVE_GATE_LU;
  const gated = absolute.filter((e) => loudnessOf(e) > relativeThreshold);
  if (!gated.length) return undefined;
  return loudnessOf(meanEnergy(gated));
}

function meanEnergy(blocks: number[]): number {
  let sum = 0;
  for (const e of blocks) sum += e;
  return sum / blocks.length;
}

function loudnessOf(energy: number): number {
  return -0.691 + 10 * Math.log10(energy);
}

// ---- Lookahead peak limiter: instant attack, ~80 ms release ----

export function limitPeaks(samples: Float64Array, rate: number, ceiling: number = CEILING): void {
  const lookahead = Math.max(1, Math.round(0.005 * rate));
  const release = Math.max(1, Math.round(0.08 * rate));
  // Backward pass: peak envelope with per-sample decay, approximating a
  // `lookahead`-wide sliding max in O(n).
  const decay = 0.5 ** (1 / lookahead);
  const required = new Float64Array(samples.length);
  let peak = 0;
  for (let i = samples.length - 1; i >= 0; --i) {
    const level = Math.abs(samples[i]!);
    peak = level > peak ? level : peak * decay;
    required[i] = Math.max(1, peak / ceiling);
  }
  // Forward pass: instant attack, exponential release, gain applied.
  let gain = 1;
  for (let i = 0; i < samples.length; ++i) {
    const want = 1 / required[i]!;
    if (want < gain) gain = want;
    else gain = Math.min(want, gain + (1 - gain) / release);
    samples[i] = samples[i]! * gain;
  }
}

// ---- Public API ----

// Normalize a 16-bit PCM WAV in place and return it. Non-PCM or non-16-bit
// payloads (or a chunk layout we do not understand) pass through unchanged —
// normalization must never break playback.
export function normalizeWavLoudness(bytes: ArrayBuffer): ArrayBuffer {
  const wav = parseWav(bytes);
  if (!wav || wav.channels !== 1) return bytes;   // TTS contract is mono
  const count = Math.floor(wav.dataLength / 2);
  const int16 = new Int16Array(bytes, wav.dataStart, count);
  const samples = new Float64Array(count);
  for (let i = 0; i < count; ++i) samples[i] = int16[i]! / 32768;

  const lufs = measureLufs(samples, wav.rate);
  if (lufs === undefined) return bytes;   // silence stays silence
  const gain = Math.min(Math.max(10 ** ((TARGET_LUFS - lufs) / 20), MIN_GAIN), MAX_GAIN);
  for (let i = 0; i < count; ++i) samples[i] = samples[i]! * gain;
  limitPeaks(samples, wav.rate);

  for (let i = 0; i < count; ++i) {
    const clamped = Math.max(-1, Math.min(1, samples[i]!));
    int16[i] = Math.round(clamped < 0 ? clamped * 0x8000 : clamped * 0x7fff);
  }
  return bytes;
}

// Diagnostic helpers (used by the synthesize log line).
export function wavLufs(bytes: ArrayBuffer): number | undefined {
  const wav = parseWav(bytes);
  if (!wav || wav.channels !== 1) return undefined;
  const count = Math.floor(wav.dataLength / 2);
  const int16 = new Int16Array(bytes, wav.dataStart, count);
  const samples = new Float64Array(count);
  for (let i = 0; i < count; ++i) samples[i] = int16[i]! / 32768;
  return measureLufs(samples, wav.rate);
}

export function wavRmsDb(bytes: ArrayBuffer): number | undefined {
  const wav = parseWav(bytes);
  if (!wav) return undefined;
  const samples = new Int16Array(bytes, wav.dataStart, Math.floor(wav.dataLength / 2));
  let sumSq = 0;
  for (const s of samples) sumSq += s * s;
  return 20 * Math.log10(Math.sqrt(sumSq / samples.length) / 32768);
}
