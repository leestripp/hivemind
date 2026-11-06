#include "../src/WavCodec.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>

using hv::Pcm16;
using hv::decodeWavTo16kMono;
using hv::encodeWav16kMono;
using hv::resampleLinear;

namespace {

// Wrap raw PCM16 interleaved samples in a minimal RIFF/WAVE buffer.
std::string makeWav(const std::vector<int16_t>& pcm, int rate,
                    int channels) {
    const auto dataLen = static_cast<std::uint32_t>(pcm.size() * 2);
    std::string out;
    auto put = [&](const char* s, std::size_t n) { out.append(s, n); };
    auto u32 = [&](std::uint32_t v) {
        char b[4] = {static_cast<char>(v & 0xff),
                     static_cast<char>((v >> 8) & 0xff),
                     static_cast<char>((v >> 16) & 0xff),
                     static_cast<char>((v >> 24) & 0xff)};
        put(b, 4);
    };
    auto u16 = [&](std::uint16_t v) {
        char b[2] = {static_cast<char>(v & 0xff),
                     static_cast<char>((v >> 8) & 0xff)};
        put(b, 2);
    };
    put("RIFF", 4);
    u32(36 + dataLen);
    put("WAVE", 4);
    put("fmt ", 4);
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(channels));
    u32(static_cast<std::uint32_t>(rate));
    u32(rate * channels * 2);
    u16(channels * 2);
    u16(16);
    put("data", 4);
    u32(dataLen);
    out.append(reinterpret_cast<const char*>(pcm.data()), dataLen);
    return out;
}

}  // namespace

TEST_CASE("encode produces a decodable 16k mono wav", "[wav]") {
    const Pcm16 pcm = {0, 1000, -1000, 32767, -32768, 42};
    const std::string wav = encodeWav16kMono(pcm.data(), pcm.size());
    REQUIRE(wav.size() == 44 + pcm.size() * 2);
    const Pcm16 back = decodeWavTo16kMono(wav);
    REQUIRE(back == pcm);  // 16k in, 16k out — lossless round trip
}

TEST_CASE("24k wav resamples to 16k", "[wav]") {
    // 2400 samples @24k = 100ms -> 1600 samples @16k.
    Pcm16 pcm(2400, 0);
    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(20000 * std::sin(i * 0.05));
    const Pcm16 out = decodeWavTo16kMono(makeWav(pcm, 24000, 1));
    REQUIRE(out.size() == 1600);
    // Same signal, slightly fewer samples: RMS stays in the same band.
    double sum = 0;
    for (const int16_t v : out) sum += v * v;
    const double rms = std::sqrt(sum / out.size());
    REQUIRE(rms > 10000);
    REQUIRE(rms < 16000);
}

TEST_CASE("stereo wav downmixes to mono", "[wav]") {
    // L = 1000 constant, R = -1000 constant -> mono ~ 0.
    std::vector<int16_t> stereo;
    for (int i = 0; i < 1600; ++i) {
        stereo.push_back(1000);
        stereo.push_back(-1000);
    }
    const Pcm16 out = decodeWavTo16kMono(makeWav(stereo, 16000, 2));
    REQUIRE(out.size() == 1600);
    for (const int16_t v : out) REQUIRE(v == 0);
}

TEST_CASE("malformed buffers throw, not crash", "[wav]") {
    REQUIRE_THROWS(decodeWavTo16kMono(""));                 // empty
    REQUIRE_THROWS(decodeWavTo16kMono("not a wav at all")); // bad magic
    const std::string riffOnly = std::string("RIFF\0\0\0\0WAVE", 12);
    REQUIRE_THROWS(decodeWavTo16kMono(riffOnly));           // no chunks
    // Truncated mid-data.
    const Pcm16 pcm(1600, 7);
    std::string wav = encodeWav16kMono(pcm.data(), pcm.size());
    REQUIRE_THROWS(decodeWavTo16kMono(wav.substr(0, wav.size() - 10)));
}

TEST_CASE("resampleLinear identity and length math", "[wav]") {
    const Pcm16 pcm = {1, 2, 3, 4};
    REQUIRE(resampleLinear(pcm.data(), pcm.size(), 16000, 16000) == pcm);
    // 16000 -> 8000 halves the sample count.
    const Pcm16 half = resampleLinear(pcm.data(), pcm.size(), 16000, 8000);
    REQUIRE(half.size() == 2);
    // Clamps to int16 range on upsampling overshoot-free paths.
    const Pcm16 big = {32767, -32768};
    const Pcm16 up = resampleLinear(big.data(), big.size(), 16000, 24000);
    REQUIRE(up.size() == 3);
    REQUIRE(up.front() == 32767);
    REQUIRE(up.back() == -32768);
}
