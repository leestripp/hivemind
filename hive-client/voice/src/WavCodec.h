#pragma once

#include "AudioCommon.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hv {

// WAV <-> PCM helpers for the ChimpFlow gateway. Pure functions — no
// I/O, no audio devices — so everything here is unit-tested.
//
// ChimpFlow's TTS returns WAV (PCM 16-bit; pocket-tts is mono 24 kHz)
// and its STT accepts any WAV whisper.cpp does. The app's playback and
// capture contract is 16 kHz mono s16le (AudioCommon), so decode
// resamples anything non-16k and encode always produces 16k mono.

// Linear-interpolation resample (adequate for voice 22.05/24k -> 16k).
inline Pcm16 resampleLinear(const int16_t* in, std::size_t n, int fromRate,
                            int toRate) {
    if (n == 0 || fromRate <= 0 || toRate <= 0) return {};
    if (fromRate == toRate) return Pcm16(in, in + n);
    const std::size_t outN =
        static_cast<std::size_t>(
            (static_cast<long double>(n) * toRate) / fromRate + 0.5L);
    Pcm16 out(outN);
    for (std::size_t i = 0; i < outN; ++i) {
        const long double src =
            (static_cast<long double>(i) * fromRate) / toRate;
        const std::size_t i0 = static_cast<std::size_t>(src);
        const std::size_t i1 = (i0 + 1 < n) ? i0 + 1 : n - 1;
        const long double frac = src - i0;
        const long double v =
            static_cast<long double>(in[i0]) * (1.0L - frac) +
            static_cast<long double>(in[i1]) * frac;
        long c = static_cast<long>(v + (v >= 0 ? 0.5L : -0.5L));
        c = c < -32768 ? -32768 : (c > 32767 ? 32767 : c);
        out[i] = static_cast<int16_t>(c);
    }
    return out;
}

struct DecodedWav {
    Pcm16 pcm;   // mono s16le, native rate
    int rate = 0;
};

// Parse a RIFF/WAVE PCM buffer to mono s16le at its NATIVE rate — no
// resampling. Playback feeds this straight to the audio device, which
// resamples to hardware rate with a proper filter; resampling here
// with bare linear interpolation aliases sibilant energy back into
// the speech band (audible as crackle). Throws std::runtime_error on
// anything malformed — a TTS failure must be loud in the log, not
// noise on output.
inline DecodedWav decodeWavMono(std::string_view bytes) {
    auto need = [&](std::size_t off, std::size_t len) {
        if (off + len > bytes.size())
            throw std::runtime_error("WAV truncated (header overruns "
                                     "buffer)");
    };
    need(0, 12);
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAVE buffer");
    std::uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    const char* data = nullptr;
    std::size_t dataLen = 0;
    std::size_t off = 12;
    while (off + 8 <= bytes.size()) {
        const std::uint32_t chunkLen =
            static_cast<std::uint8_t>(bytes[off + 4]) |
            (static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(bytes[off + 5]))
             << 8) |
            (static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(bytes[off + 6]))
             << 16) |
            (static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(bytes[off + 7]))
             << 24);
        const std::size_t body = off + 8;
        if (body + chunkLen > bytes.size())
            throw std::runtime_error("WAV truncated (chunk overruns "
                                     "buffer)");
        if (std::memcmp(bytes.data() + off, "fmt ", 4) == 0) {
            if (chunkLen < 16)
                throw std::runtime_error("WAV fmt chunk too small");
            auto u16 = [&](std::size_t o) {
                return static_cast<std::uint16_t>(
                    static_cast<std::uint8_t>(bytes[o]) |
                    (static_cast<std::uint16_t>(
                         static_cast<std::uint8_t>(bytes[o + 1]))
                     << 8));
            };
            auto u32 = [&](std::size_t o) {
                return static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(bytes[o]) |
                    (static_cast<std::uint32_t>(
                         static_cast<std::uint8_t>(bytes[o + 1]))
                     << 8) |
                    (static_cast<std::uint32_t>(
                         static_cast<std::uint8_t>(bytes[o + 2]))
                     << 16) |
                    (static_cast<std::uint32_t>(
                         static_cast<std::uint8_t>(bytes[o + 3]))
                     << 24));
            };
            audioFormat = u16(body);
            channels = u16(body + 2);
            sampleRate = u32(body + 4);
            bitsPerSample = u16(body + 14);
        } else if (std::memcmp(bytes.data() + off, "data", 4) == 0) {
            data = bytes.data() + body;
            dataLen = chunkLen;
        }
        off = body + chunkLen + (chunkLen & 1);  // chunks are word-aligned
    }
    if (!data) throw std::runtime_error("WAV has no data chunk");
    if (audioFormat != 1)
        throw std::runtime_error("WAV is not PCM (codec " +
                                 std::to_string(audioFormat) + ")");
    if (bitsPerSample != 16)
        throw std::runtime_error("WAV is not 16-bit (" +
                                 std::to_string(bitsPerSample) + ")");
    if (channels == 0 || sampleRate == 0)
        throw std::runtime_error("WAV fmt chunk incomplete");
    DecodedWav out;
    Pcm16& pcm = out.pcm;
    if (channels == 1) {
        const std::size_t n = dataLen / 2;
        const auto* s = reinterpret_cast<const int16_t*>(data);
        pcm.assign(s, s + n);
    } else {
        // Downmix interleaved stereo (or more) by averaging.
        const std::size_t frames = dataLen / (2 * channels);
        pcm.resize(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            long acc = 0;
            for (std::uint16_t c = 0; c < channels; ++c)
                acc += reinterpret_cast<const int16_t*>(
                    data)[f * channels + c];
            pcm[f] = static_cast<int16_t>(acc / channels);
        }
    }
    out.rate = static_cast<int>(sampleRate);
    return out;
}

// 16 kHz variant — the STT upload contract. Playback should prefer
// decodeWavMono (see above).
inline Pcm16 decodeWavTo16kMono(std::string_view bytes) {
    DecodedWav d = decodeWavMono(bytes);
    return resampleLinear(d.pcm.data(), d.pcm.size(), d.rate, kSampleRate);
}

// Encode 16 kHz mono s16le PCM as a RIFF/WAVE buffer (for the STT
// multipart upload).
inline std::string encodeWav16kMono(const int16_t* samples, std::size_t n) {
    const std::uint32_t dataLen =
        static_cast<std::uint32_t>(n * sizeof(int16_t));
    std::string out;
    out.reserve(44 + dataLen);
    auto put = [&](const char* s, std::size_t len) {
        out.append(s, len);
    };
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
    u32(16);        // fmt chunk size
    u16(1);         // PCM
    u16(kChannels);
    u32(kSampleRate);
    u32(kSampleRate * kChannels * 2);  // byte rate
    u16(kChannels * 2);                // block align
    u16(16);                           // bits per sample
    put("data", 4);
    u32(dataLen);
    out.append(reinterpret_cast<const char*>(samples), dataLen);
    return out;
}

}  // namespace hv
