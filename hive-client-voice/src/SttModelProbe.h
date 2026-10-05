#pragma once

#include <cstdint>
#include <string>

namespace hv {

// Pre-flight filesystem check for the whisper STT models. Pure logic —
// no whisper, no audio — so the unit tests exercise every failure shape
// (missing dir, missing file, truncated download) without needing the
// engine or real 40 MB+ models.
//
// The app needs two q8_0 models (same pair as the Android client):
//   ggml-tiny.en-q8_0.bin  — live partial decode
//   ggml-base.en-q8_0.bin  — final decode on SEND

inline constexpr const char* kSttModelPartial = "ggml-tiny.en-q8_0.bin";
inline constexpr const char* kSttModelFinal = "ggml-base.en-q8_0.bin";
// Both models are tens of MB; anything smaller is an absent or
// truncated download and must count as NOT usable.
inline constexpr std::uintmax_t kSttModelMinBytes = 10 * 1024 * 1024;
inline constexpr const char* kSttModelBaseUrl =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main";

struct SttModelFile {
    std::string name;        // bare filename
    std::string path;        // full path probed
    bool present = false;    // regular file exists
    std::uintmax_t sizeBytes = 0;
    bool plausible = false;  // present && sizeBytes >= threshold
};

struct SttModelStatus {
    std::string dir;
    bool dirExists = false;
    SttModelFile partial;  // ggml-tiny.en-q8_0.bin
    SttModelFile final;    // ggml-base.en-q8_0.bin

    bool ready() const { return partial.plausible && final.plausible; }

    // One line for the hive-stt: stderr log, e.g.
    //   dir=/x (exists) partial=ggml-tiny.en-q8_0.bin 44MB ok final=... MISSING
    std::string summary() const;

    // Actionable message for the UI: names exactly what is missing or
    // truncated and gives the per-file curl commands that fix it.
    std::string userMessage() const;
};

// Probe `dir` for both models. `minBytes` is injectable so tests can
// use tiny fixture files instead of real 40 MB+ models.
SttModelStatus probeSttModels(const std::string& dir,
                              std::uintmax_t minBytes = kSttModelMinBytes);

// ~/.local/share/hive-client-voice/models ($HOME-aware, "." fallback).
std::string sttModelDir();

}  // namespace hv
