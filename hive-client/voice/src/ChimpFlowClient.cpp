#include "ChimpFlowClient.h"

#include "WavCodec.h"

#include <OmniNet.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>

namespace hv {

ChimpFlowClient::ChimpFlowClient(std::string base, std::string key)
    : base_(std::move(base)), key_(std::move(key)) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

namespace {

using OmniNet::HTTP::ClientConfig;
using OmniNet::HTTP::ClientRequest;

ClientConfig cfg(std::chrono::seconds connect, std::chrono::seconds total) {
    ClientConfig c;
    c.connect_timeout = connect;
    c.request_timeout = total;
    return c;
}

// One request/response round trip. Returns the HTTP status, or -1 on a
// transport failure; `body` receives the raw response bytes either way.
int roundTrip(const std::string& method, const std::string& url,
              const std::string& auth, const std::string& contentType,
              const std::string& payload, std::string& body,
              std::chrono::seconds total = std::chrono::seconds{60}) {
    ClientRequest req;
    req.method(method).url(url);
    if (!auth.empty()) req.header("Authorization", "Bearer " + auth);
    if (!contentType.empty()) req.header("Content-Type", contentType);
    if (!payload.empty() || method == "POST") req.body(payload);
    auto result =
        OmniNet::HTTP::Client::get_instance().send_blocking(
            req, cfg(std::chrono::seconds{3}, total));
    if (!result.has_value()) {
        fprintf(stderr, "hive-voice: chimpflow %s %s -> transport error %s\n",
                method.c_str(), url.c_str(),
                OmniNet::HTTP::clientErrorName(result.error()));
        return -1;
    }
    body = result->bodyAsString();
    return result->statusCode();
}

[[noreturn]] void fail(int status, const std::string& fallback,
                       const std::string& body) {
    std::string msg = fallback;
    try {
        const auto j = nlohmann::json::parse(body);
        // Gateway errors are {"error": "..."}; the engine beneath can
        // nest {"error": {"message": ...}} — accept both.
        if (j.contains("error")) {
            if (j["error"].is_string())
                msg = j["error"].get<std::string>();
            else if (j["error"].is_object() &&
                     j["error"].contains("message") &&
                     j["error"]["message"].is_string())
                msg = j["error"]["message"].get<std::string>();
        }
    } catch (...) {
    }
    fprintf(stderr, "hive-voice: chimpflow error %d: %s\n", status,
            msg.c_str());
    throw ChimpFlowError(status, msg);
}

}  // namespace

bool ChimpFlowClient::healthy() {
    std::string body;
    const int code = roundTrip("GET", base_ + "/healthz", "", "", "", body,
                               std::chrono::seconds{5});
    return code == 200 && body.find("\"ok\"") != std::string::npos;
}

std::vector<std::string> ChimpFlowClient::voices() {
    std::string body;
    const int code =
        roundTrip("GET", base_ + "/v1/voices", key_, "", "", body);
    std::vector<std::string> out;
    if (code != 200) return out;  // picker shows the failure, not a crash
    try {
        const auto j = nlohmann::json::parse(body);
        for (const auto& v : j.value("data", nlohmann::json::array())) {
            if (v.contains("id"))
                out.push_back(v["id"].get<std::string>());
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "hive-voice: chimpflow /v1voices parse: %s\n",
                e.what());
    }
    return out;
}

DecodedWav ChimpFlowClient::synthesize(const std::string& text,
                                       const std::string& voice) {
    // Two distinct ids per the gateway contract: `model` is a model id
    // from GET /v1/models (the engine loads exactly one TTS model),
    // `voice` is a speaker id from GET /v1/voices. Sending a voice id
    // as the model 500s ("unknown model id").
    nlohmann::json req = {{"model", "pocket-tts"},
                          {"input", text},
                          {"response_format", "wav"}};
    if (!voice.empty()) req["voice"] = voice;
    std::string body;
    const int code = roundTrip("POST", base_ + "/v1/audio/speech", key_,
                               "application/json", req.dump(), body);
    if (code != 200)
        fail(code, "TTS request failed (HTTP " + std::to_string(code) + ")",
             body);
    return decodeWavMono(body);  // throws on malformed audio
}

std::string ChimpFlowClient::transcribe(const int16_t* samples,
                                        std::size_t n) {
    // Multipart upload: the WAV as `file`, whisper as the model.
    const std::string wav = encodeWav16kMono(samples, n);
    const std::string boundary = "----hivevoice7d1f9a2c";
    std::string payload;
    payload.reserve(wav.size() + 512);
    payload += "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper\r\n";
    payload += "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; "
        "filename=\"utterance.wav\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    payload.append(wav);
    payload += "\r\n--" + boundary + "--\r\n";

    std::string body;
    const int code = roundTrip("POST", base_ + "/v1/audio/transcriptions",
                               key_,
                               "multipart/form-data; boundary=" + boundary,
                               payload, body);
    if (code != 200)
        fail(code, "STT request failed (HTTP " + std::to_string(code) + ")",
             body);
    try {
        return nlohmann::json::parse(body).value("text", "");
    } catch (const std::exception& e) {
        fail(-1, std::string("STT response unreadable: ") + e.what(), body);
    }
}

}  // namespace hv
