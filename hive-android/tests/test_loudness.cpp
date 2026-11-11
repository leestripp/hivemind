#include "../app/src/main/cpp/loudness.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<float> sine(float amplitude, std::size_t count = 8000) {
    std::vector<float> samples(count);
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = amplitude * std::sin(static_cast<float>(i) * 0.05f);
    return samples;
}

}  // namespace

int main() {
    {
        auto pcm = sine(0.08f);
        const auto s = pockettts::normalizeLoudness(pcm.data(), pcm.size());
        require(std::fabs(s.outputRms - 0.09f) < 0.001f,
                "quiet voice reaches the common target RMS");
        require(s.outputRms > 0.085f && s.outputRms < 0.095f,
                "quiet voice retains the common normalization headroom");
        require(s.outputPeak <= 0.95f, "quiet voice never clips");
    }
    {
        auto pcm = sine(0.22f);
        const auto s = pockettts::normalizeLoudness(pcm.data(), pcm.size());
        require(std::fabs(s.outputRms - 0.09f) < 0.001f,
                "loud voice reaches the same target RMS");
        require(s.gain < 1.0f, "loud voice is attenuated");
    }
    {
        auto pcm = sine(0.05f);
        pcm[100] = 0.99f;
        pcm[4000] = -0.99f;
        const auto s = pockettts::normalizeLoudness(pcm.data(), pcm.size());
        require(s.outputPeak <= 0.95f, "peak cap wins over the RMS target");
        require(s.outputPeak > 0.90f, "peak cap does not hard-clip samples");
    }
    {
        std::vector<float> silence(4000, 0.0f);
        const auto s = pockettts::normalizeLoudness(silence.data(), silence.size());
        require(s.gain == 1.0f && s.outputPeak == 0.0f,
                "silence remains untouched");
        require(pockettts::normalizeLoudness(nullptr, 0).gain == 1.0f,
                "empty input is safe");
    }
    {
        auto first = sine(0.08f);
        auto second = sine(0.12f);
        const auto a = pockettts::normalizeLoudness(first.data(), first.size());
        const auto b = pockettts::normalizeLoudness(second.data(), second.size());
        require(std::fabs(a.outputRms - b.outputRms) < 0.001f,
                "consecutive chunks do not acquire escalating loudness");
    }
    std::cout << "All loudness tests passed\n";
}
