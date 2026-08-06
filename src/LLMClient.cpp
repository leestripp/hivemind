#include "hivemind/LLMClient.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <algorithm>

namespace {

// RAII curl handle
struct CurlRAII {
    CURL* h = nullptr;
    CurlRAII() { h = curl_easy_init(); }
    ~CurlRAII() { if (h) curl_easy_cleanup(h); }
    CurlRAII(const CurlRAII&) = delete;
    CurlRAII& operator=(const CurlRAII&) = delete;
    operator CURL*() const { return h; }
};

// Stream state passed through curl userdata
struct StreamCtx {
    std::string* output = nullptr;
    std::function<void(std::string_view)>* on_chunk = nullptr;
};

// curl write callback for streaming SSE
size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    size_t total = size * nmemb;
    std::string_view sv{ptr, total};

    // Process complete SSE lines
    size_t pos = 0;
    while (pos < sv.size()) {
        size_t nl = sv.find('\n', pos);
        if (nl == std::string_view::npos) break;
        size_t end = nl;
        if (end > 0 && sv[end - 1] == '\r') --end;
        std::string line{sv.data() + pos, end - pos};
        pos = nl + 1;

        if (line.rfind("data: ", 0) == 0) {
            std::string_view jsv{line.c_str() + 6, line.size() - 6};
            if (jsv == "[DONE]") return total;
            try {
                auto j = nlohmann::json::parse(jsv);
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& ch = j["choices"][0];
                    if (ch.contains("delta") && ch["delta"].contains("content")) {
                        std::string txt = ch["delta"]["content"].get<std::string>();
                        if (!txt.empty() && ctx->on_chunk && *ctx->on_chunk) {
                            (*ctx->on_chunk)(txt);
                        }
                    }
                }
            } catch (...) {
                // Malformed JSON in SSE — skip
            }
        }
    }
    return total;
}

// curl write callback for non-streaming response
size_t body_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

LLMClient::LLMClient(std::string base_url,
                       std::string api_key,
                       std::string model,
                       std::string endpoint)
    : base_url_(std::move(base_url))
    , api_key_(std::move(api_key))
    , model_(std::move(model))
    , endpoint_(std::move(endpoint))
{}

std::string LLMClient::stream(const std::vector<LLMMessage>& messages,
                              ChunkCallback on_chunk,
                              ToolCallback /*on_tool*/,
                              std::stop_token /*stop*/) {
    // Build messages array
    nlohmann::json msg_array = nlohmann::json::array();
    for (const auto& m : messages) {
        msg_array.push_back({
            {"role",    m.role},
            {"content", m.content}
        });
    }

    nlohmann::json payload = {
        {"model",       model_},
        {"messages",    msg_array},
        {"stream",      true},
        {"temperature", 0.0}
    };

    std::string url = base_url_ + endpoint_;
    std::string json_body = payload.dump();

    CurlRAII curl;
    if (!curl) { error_ = "curl init failed"; return {}; }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key_).c_str());

    StreamCtx ctx;
    ctx.on_chunk = &on_chunk;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 900L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    error_.clear();
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        error_ = curl_easy_strerror(rc);
        return {};
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
        error_ = "HTTP " + std::to_string(http_code);
        return {};
    }

    return {}; // Accumulated via on_chunk callback
}

LLMClient::Result LLMClient::complete(const std::vector<LLMMessage>& messages,
                                     std::stop_token /*stop*/) {
    Result result;

    nlohmann::json msg_array = nlohmann::json::array();
    for (const auto& m : messages) {
        msg_array.push_back({
            {"role",    m.role},
            {"content", m.content}
        });
    }

    nlohmann::json payload = {
        {"model",       model_},
        {"messages",    msg_array},
        {"stream",       false},
        {"temperature",  0.0}
    };

    std::string url = base_url_ + endpoint_;
    std::string json_body = payload.dump();

    CurlRAII curl;
    if (!curl) { result.error = "curl init failed"; result.ok = false; return result; }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key_).c_str());

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 900L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        result.error = curl_easy_strerror(rc);
        result.ok = false;
        return result;
    }

    if (http_code >= 400) {
        result.error = "HTTP " + std::to_string(http_code);
        result.ok = false;
        return result;
    }

    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("choices") && !j["choices"].empty()) {
            auto& ch = j["choices"][0];
            if (ch.contains("message") && ch["message"].contains("content")) {
                result.content = ch["message"]["content"].get<std::string>();
            }
        }
        if (j.contains("usage")) {
            result.prompt_tokens     = j["usage"].value("prompt_tokens", 0);
            result.completion_tokens = j["usage"].value("completion_tokens", 0);
            result.total_tokens     = j["usage"].value("total_tokens", 0);
        }
    } catch (const std::exception& e) {
        result.error = e.what();
        result.ok = false;
    }

    return result;
}
