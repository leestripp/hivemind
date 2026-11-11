#include "../src/AudioCommon.h"
#include "../src/PlaybackRateLogic.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using hv::Pcm16;
using hv::normalizeLoudness;

// REQUIRE_THAT is avoided on purpose: Catch2's INTERNAL_CHECK_THAT
// token-pastes a _catch_sr literal suffix that cppcheck cannot parse
// (unknownMacro), so the RMS tolerance is asserted with REQUIRE + CAPTURE.

namespace {

float rmsOf(const Pcm16& pcm) {
    double sumSq = 0.0;
    for (const int16_t s : pcm)
        sumSq += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(sumSq / pcm.size())) / 32768.0f;
}

float peakOf(const Pcm16& pcm) {
    int peak = 0;
    for (const int16_t s : pcm)
        peak = std::max(peak, std::abs(static_cast<int>(s)));
    return static_cast<float>(peak) / 32768.0f;
}

Pcm16 sine(float amplitude, std::size_t n) {
    Pcm16 pcm(n);
    for (std::size_t i = 0; i < n; ++i)
        pcm[i] = static_cast<int16_t>(
            amplitude * 32767.0f *
            std::sin(static_cast<float>(i) * 0.05f));
    return pcm;
}

}  // namespace

TEST_CASE("quiet voice is gained up to the target RMS", "[audio]") {
    Pcm16 pcm = sine(0.08f, 8000);  // RMS ≈ 0.057 — the caro_davy case
    normalizeLoudness(pcm);
    const float rms = rmsOf(pcm);
    CAPTURE(rms);
    REQUIRE(std::abs(rms - 0.106f) <= 0.005f);
    REQUIRE(peakOf(pcm) <= 0.95f);
}

TEST_CASE("loud voice is attenuated to the same target", "[audio]") {
    Pcm16 pcm = sine(0.22f, 8000);  // RMS ≈ 0.156
    normalizeLoudness(pcm);
    const float rms = rmsOf(pcm);
    CAPTURE(rms);
    REQUIRE(std::abs(rms - 0.106f) <= 0.005f);
    REQUIRE(peakOf(pcm) <= 0.95f);
}

TEST_CASE("hot peaks never clip — the peak cap wins over the RMS target",
          "[audio]") {
    Pcm16 pcm = sine(0.05f, 8000);
    pcm[100] = 32000;   // one near-full-scale transient
    pcm[4000] = -32000;
    normalizeLoudness(pcm);
    REQUIRE(peakOf(pcm) <= 0.95f);
}

TEST_CASE("silence and empty input pass through untouched", "[audio]") {
    Pcm16 silence(4000, 0);
    normalizeLoudness(silence);
    REQUIRE(peakOf(silence) == 0.0f);
    Pcm16 empty;
    normalizeLoudness(empty);  // must not crash
    REQUIRE(empty.empty());
}

TEST_CASE("very quiet voices get the full boost without clipping", "[audio]") {
    Pcm16 pcm = sine(0.02f, 8000);  // RMS ≈ 0.014 — the stuart_bell case
    normalizeLoudness(pcm);         // needs ~7.5x; gain cap 6.0 applies
    REQUIRE(rmsOf(pcm) > 0.08f);
    REQUIRE(peakOf(pcm) <= 0.95f);
}

TEST_CASE("degenerate extreme input is bounded by the gain clamp", "[audio]") {
    Pcm16 pcm = sine(0.9f, 8000);  // RMS ≈ 0.64 — far above target
    normalizeLoudness(pcm);
    REQUIRE(rmsOf(pcm) >= 0.15f);  // min-gain 0.25 → RMS ≈ 0.16, not 0.106
    REQUIRE(peakOf(pcm) <= 0.95f);
}


TEST_CASE("native-rate playback drains old audio before reopening") {
    using hv::playback::rateSwitchBlocked;
    REQUIRE(rateSwitchBlocked(24000, 16000, true));
    REQUIRE_FALSE(rateSwitchBlocked(24000, 16000, false));
    REQUIRE_FALSE(rateSwitchBlocked(24000, 24000, true));
}

TEST_CASE("master volume scales samples and mute only changes gain", "[audio]") {
    Pcm16 pcm{10000, -10000, 0, 2000, -32768, 32767};
    hv::applyOutputGain(std::span{pcm}, 0.5f);
    REQUIRE(pcm == Pcm16{5000, -5000, 0, 1000, -16384, 16383});

    hv::applyOutputGain(std::span{pcm}, 0.0f);
    REQUIRE(pcm == Pcm16(6, 0));

    hv::setMasterVolume(0.35f);
    REQUIRE(std::abs(hv::masterVolume() - 0.35f) < 0.0001f);
    hv::setMasterMuted(true);
    REQUIRE(hv::masterGain() == 0.0f);
    REQUIRE(std::abs(hv::masterVolume() - 0.35f) < 0.0001f);
    hv::setMasterMuted(false);
    REQUIRE(std::abs(hv::masterGain() - 0.35f) < 0.0001f);

    hv::setMasterVolume(-1.0f);
    REQUIRE(hv::masterGain() == 0.0f);
    hv::setMasterVolume(2.0f);
    REQUIRE(hv::masterGain() == 1.0f);
}
