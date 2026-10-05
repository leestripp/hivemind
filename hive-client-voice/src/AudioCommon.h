#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hv {

// 16 kHz mono s16le everywhere — the Android AudioRecord/AudioTrack
// contract the whisper models and piper both expect.
inline constexpr int kSampleRate = 16000;
inline constexpr int kChannels = 1;

using Pcm16 = std::vector<int16_t>;

// One 100 ms capture chunk (SAMPLE_RATE / 10 samples) or any utterance.
using ChunkCallback = std::function<void(const int16_t*, std::size_t)>;
using LevelCallback = std::function<void(float)>;

// RMS of a chunk, 0..~32767.
double chunkRms(const int16_t* samples, std::size_t n);

}  // namespace hv
