#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hv {

// 16 kHz mono s16le everywhere — the Android AudioRecord/AudioTrack
// contract; ChimpFlow's WAV output is resampled into it (WavCodec).
inline constexpr int kSampleRate = 16000;
inline constexpr int kChannels = 1;

using Pcm16 = std::vector<int16_t>;

// Per-utterance loudness normalization. The TTS catalog voices sit
// ±3 dB+ apart (measured -25.3..-19.1 dBFS RMS on one sentence), so
// every utterance is gained to a common RMS target, with the gain
// capped so the utterance's own peak never exceeds full scale —
// quiet voices come up, loud voices come down, nothing clips.
void normalizeLoudness(Pcm16& pcm);

// Process-wide output level shared by every AudioPlayback instance and the
// inline media player. These controls change gain ONLY: playback, completion
// tags, and hub-side agent turns continue while muted.
void setMasterVolume(float volume) noexcept;  // clamped to 0.0 … 1.0
float masterVolume() noexcept;
void setMasterMuted(bool muted) noexcept;
bool masterMuted() noexcept;
float masterGain() noexcept;

// Apply a buffer-local gain without changing the queue or playback state.
void applyOutputGain(std::span<int16_t> pcm, float gain) noexcept;

// One 100 ms capture chunk (SAMPLE_RATE / 10 samples) or any utterance.
using ChunkCallback = std::function<void(const int16_t*, std::size_t)>;
using LevelCallback = std::function<void(float)>;

}  // namespace hv
