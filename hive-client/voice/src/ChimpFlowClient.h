#pragma once

#include "AudioCommon.h"
#include "WavCodec.h"

#include <string>
#include <vector>

namespace hv {

// Client for the ChimpFlow gateway (http://127.0.0.1:8090) — the
// localhost voice service that proxies TTS to audio.cpp and STT to
// whisper.cpp. OpenAI-audio-API shaped; this class owns the transport
// (OmniNet HTTP), the bearer key, and the WAV <-> PCM16 (16 kHz mono)
// conversion the rest of the app speaks.
//
// Error contract (surfaced as ChimpFlowError with .status):
//   401  bad/missing key — stop, tell the operator; retry cannot help
//   403  key lacks scope — needs a differently-scoped key
//   502  engine down (audio.cpp :8080 / whisper :8081) — surface, don't
//        retry-loop
//   -1   transport failure (gateway not running, etc.)
class ChimpFlowError : public std::exception {
 public:
    ChimpFlowError(int status, std::string message)
        : status_(status), message_(std::move(message)) {}
    int status() const { return status_; }
    const char* what() const noexcept override { return message_.c_str(); }

 private:
    int status_;
    std::string message_;
};

class ChimpFlowClient {
 public:
    ChimpFlowClient(std::string base, std::string key);

    const std::string& base() const { return base_; }

    // GET /healthz — no auth. True = the gateway process responds.
    bool healthy();

    // GET /v1/voices — legacy unscoped catalog (WAV sample basenames the
    // engine serves). Empty on failure (logged); the picker then
    // shows the failure instead of crashing.
    std::vector<std::string> voices();

    // GET /v1/models — TTS model ids the gateway currently serves
    // (kokoro-82m, chatterbox, ...). Empty on failure.
    std::vector<std::string> models();

    // GET /v1/voices?model=<model-id> — speaker ids for ONE model
    // (e.g. kokoro "af_heart"). Empty on failure.
    std::vector<std::string> voicesForModel(const std::string& model);

    // POST /v1/audio/speech -> WAV -> mono s16le at the WAV's native
    // rate (decodeWavMono — no software resample; the audio device
    // resamples with a proper filter). Throws ChimpFlowError on any
    // failure. `voice` "" = model default voice, `model` "" = gateway
    // default model (kokoro-82m — never hardcode a legacy model here).
    DecodedWav synthesize(const std::string& text, const std::string& voice,
                          const std::string& model = "");

    // POST /v1/audio/transcriptions (multipart WAV) -> {"text": ...}.
    // Throws ChimpFlowError on any failure.
    std::string transcribe(const int16_t* samples, std::size_t n);

 private:
    std::string base_;
    std::string key_;
};

}  // namespace hv
