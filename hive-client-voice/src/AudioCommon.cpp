#include "AudioCommon.h"

#include <algorithm>
#include <cmath>

namespace hv {

namespace {
constexpr float kTargetRms = 0.106f;    // ≈ -19.5 dBFS
constexpr float kPeakCeiling = 0.95f;   // fraction of full scale
constexpr float kMinGain = 0.25f;  // sanity bounds for degenerate input
// The quietest catalog voices need ~+14 dB to reach target; 6x (+15.6
// dB) covers them, and the peak cap above keeps that safe.
constexpr float kMaxGain = 6.0f;
constexpr float kSilenceRms = 1e-4f;    // below this: leave it alone
}  // namespace

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
