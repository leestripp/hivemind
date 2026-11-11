#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pockettts {

struct LoudnessStats {
    float inputRms = 0.0f;
    float gain = 1.0f;
    float outputRms = 0.0f;
    float outputPeak = 0.0f;
};

// Keep every voice at one consistent level. The peak cap is part of the
// gain, not per-sample clipping, so transient consonants retain their
// shape instead of turning harsh. Low-volume headroom is applied live by
// PcmPlayer, which can respond to volume keys without re-synthesizing PCM.
inline LoudnessStats normalizeLoudness(float* samples, std::size_t count) {
    constexpr float kTargetRms = 0.09f;  // ≈ -21 dBFS before device volume
    constexpr float kPeakCeiling = 0.95f;
    constexpr float kMinGain = 0.25f;
    constexpr float kMaxGain = 4.0f;
    constexpr float kSilenceRms = 1e-4f;

    LoudnessStats stats;
    if (samples == nullptr || count == 0) return stats;

    double sumSq = 0.0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        sumSq += static_cast<double>(samples[i]) * samples[i];
        peak = std::max(peak, std::fabs(samples[i]));
    }
    stats.inputRms = static_cast<float>(std::sqrt(sumSq / count));
    stats.outputRms = stats.inputRms;
    stats.outputPeak = peak;
    if (stats.inputRms <= kSilenceRms || peak == 0.0f) return stats;

    stats.gain = std::clamp(kTargetRms / stats.inputRms, kMinGain, kMaxGain);
    stats.gain = std::min(stats.gain, kPeakCeiling / peak);

    double outputSumSq = 0.0;
    float outputPeak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] *= stats.gain;
        outputSumSq += static_cast<double>(samples[i]) * samples[i];
        outputPeak = std::max(outputPeak, std::fabs(samples[i]));
    }
    stats.outputRms = static_cast<float>(std::sqrt(outputSumSq / count));
    stats.outputPeak = outputPeak;
    return stats;
}

}  // namespace pockettts
