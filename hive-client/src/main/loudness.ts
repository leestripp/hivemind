// ==== Per-utterance loudness normalization (ported from the GTK client's
// voice/src/AudioCommon.cpp) ====
//
// The ChimpFlow voice catalog sits ~6 dB RMS voice-to-voice, so every
// synthesized utterance is gained to a common RMS target. The gain is capped
// by the utterance's own peak, so quiet voices come up, loud voices come
// down, and nothing clips. The Electron client must match the GTK client
// exactly — same constants, same math.

const kTargetRms = 0.106;    // ≈ -19.5 dBFS
const kPeakCeiling = 0.95;   // fraction of full scale
const kMinGain = 0.25;       // sanity bounds for degenerate input
const kMaxGain = 6.0;
const kSilenceRms = 1e-4;    // below this: leave it alone

export function normalizeLoudness(samples: Int16Array): void {
  if (!samples.length) return;
  let sumSq = 0;
  let peak = 0;
  for (const s of samples) {
    sumSq += s * s;
    const a = Math.abs(s);
    if (a > peak) peak = a;
  }
  const rms = Math.sqrt(sumSq / samples.length) / 32768;
  if (rms < kSilenceRms || peak === 0) return;  // silence stays silence
  const peakFs = peak / 32768;
  // One gain for the whole utterance: RMS hits the target, and the
  // peak cap makes clipping impossible by construction.
  const gain = Math.min(Math.min(kTargetRms / rms, kPeakCeiling / peakFs), kMaxGain);
  const bounded = Math.max(gain, kMinGain);
  for (let i = 0; i < samples.length; ++i) {
    samples[i] = Math.max(-32768, Math.min(32767, Math.trunc(samples[i]! * bounded)));
  }
}

// Gain a 16-bit PCM WAV in place and return it. Non-PCM or non-16-bit
// payloads (or a chunk layout we do not understand) pass through unchanged —
// normalization must never break playback.
export function normalizeWavLoudness(bytes: ArrayBuffer): ArrayBuffer {
  const view = new DataView(bytes);
  if (bytes.byteLength < 44 || view.getUint32(0, false) !== 0x52494646 || view.getUint32(8, false) !== 0x57415645) {
    return bytes;
  }
  let offset = 12;
  let dataStart = -1;
  let dataLength = 0;
  let pcm16 = false;
  while (offset + 8 <= bytes.byteLength) {
    const id = view.getUint32(offset, false);
    const size = view.getUint32(offset + 4, true);
    const body = offset + 8;
    if (body + size > bytes.byteLength) break;
    if (id === 0x666d7420) {  // 'fmt '
      pcm16 = view.getUint16(body, true) === 1 && view.getUint16(body + 14, true) === 16;
    } else if (id === 0x64617461) {  // 'data'
      dataStart = body;
      dataLength = size;
    }
    offset = body + size + (size % 2);  // chunks are word-aligned
  }
  if (!pcm16 || dataStart < 0 || dataLength < 2) return bytes;
  const count = Math.floor(dataLength / 2);
  const samples = new Int16Array(bytes, dataStart, count);
  normalizeLoudness(samples);
  return bytes;
}
