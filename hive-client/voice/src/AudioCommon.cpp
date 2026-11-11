#include "AudioCommon.h"

#include <algorithm>
#include <cmath>
#include <atomic>

namespace hv {

namespace {
constexpr float kTargetRms = 0.106f;    // ≈ -19.5 dBFS
constexpr float kPeakCeiling = 0.95f;   // fraction of full scale
constexpr float kMinGain = 0.25f;  // sanity bounds for degenerate input
// The quietest catalog voices need ~+14 dB to reach target; 6x (+15.6
// dB) covers them, and the peak cap above keeps that safe.
constexpr float kMaxGain = 6.0f;
constexpr float kSilenceRms = 1e-4f;    // below this: leave it alone
std::atomic<float> g_master_volume{1.0f};
std::atomic<bool> g_master_muted{false};
}  // namespace

void setMasterVolume(float volume) noexcept {
    if (!std::isfinite(volume)) return;
    g_master_volume.store(std::clamp(volume, 0.0f, 1.0f),
                          std::memory_order_relaxed);
}

float masterVolume() noexcept {
    return g_master_volume.load(std::memory_order_relaxed);
}

void setMasterMuted(bool muted) noexcept {
    g_master_muted.store(muted, std::memory_order_relaxed);
}

bool masterMuted() noexcept {
    return g_master_muted.load(std::memory_order_relaxed);
}

float masterGain() noexcept {
    return masterMuted() ? 0.0f : masterVolume();
}

void applyOutputGain(std::span<int16_t> pcm, float gain) noexcept {
    if (gain >= 1.0f) return;
    gain = std::clamp(gain, 0.0f, 1.0f);
    for (auto& sample : pcm)
        sample = static_cast<int16_t>(static_cast<float>(sample) * gain);
}

void normalizeLoudness(Pcm16& pcm) {
    if (pcm.empty()) return;
    double sumSq = 0.0;
    int peak = 0;
    for (const int16_t s : pcm) {
        sumSq += static_cast<double>(s) * s;
        peak = std::max(peak, std::abs(static_cast<int>(s)));
    }
    const float rms = static_cast<float>(std::sqrt(sumSq / pcm.size())) /
                      32768.0f;
    if (rms < kSilenceRms || peak == 0) return;  // silence stays silence
    const float peakFs = static_cast<float>(peak) / 32768.0f;
    // One gain for the whole utterance: RMS hits the target, and the
    // peak cap makes clipping impossible by construction.
    float gain = std::min(kTargetRms / rms, kPeakCeiling / peakFs);
    gain = std::clamp(gain, kMinGain, kMaxGain);
    for (auto& s : pcm) {
        const float v = static_cast<float>(s) * gain;
        s = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
}

}  // namespace hv
