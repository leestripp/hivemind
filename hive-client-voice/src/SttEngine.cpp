#include "SttEngine.h"

#include "AudioCapture.h"
#include "SttModelProbe.h"

#include <whisper.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace hv {

namespace {
constexpr int kThreads = 4;
constexpr int kPartialMs = 2000;
constexpr int kWindowSamples = kSampleRate * 5;  // 5 s partial window
constexpr int kMaxSeconds = 120;
constexpr size_t kMaxSamples = kSampleRate * kMaxSeconds;
constexpr int kVadPeakMin = 500;  // PCM16 peak; speech ~3-20k
// Route whisper.cpp's own stderr noise through a prefixed logger so a
// model failure is attributable to THIS app (whisper_log_set is global;
// install once).
void whisperLogToStderr(enum ggml_log_level level, const char* text,
                        void* /*user_data*/) {
    const char* tag = level == GGML_LOG_LEVEL_ERROR ? "ERROR"
                    : level == GGML_LOG_LEVEL_WARN  ? "warn"
                                                    : "info";
    fprintf(stderr, "hive-stt[whisper:%s] %s", tag, text ? text : "");
    if (text && text[0] && text[strlen(text) - 1] != '\n') fputc('\n', stderr);
}
}  // namespace

std::string SttEngine::modelDir() {
    return sttModelDir();
}

SttEngine::SttEngine(AudioCapture& capture,
                     std::function<void(float)> onLevel,
                     std::function<void(const std::string&)> onTranscript,
                     std::function<void(const std::string&)> onStopped)
    : capture_(capture),
      on_level_(std::move(onLevel)),
      on_transcript_(std::move(onTranscript)),
      on_stopped_(std::move(onStopped)) {
    running_ = true;
    worker_ = std::thread([this] { workerInit(); });
    ticker_ = std::thread([this] { partialLoop(); });
}

SttEngine::~SttEngine() {
    running_ = false;
    capture_.stop();
    if (worker_.joinable()) worker_.join();
    if (ticker_.joinable()) ticker_.join();
    if (ctx_partial_) whisper_free(ctx_partial_);
    if (ctx_final_) whisper_free(ctx_final_);
}

std::string SttEngine::transcript() const {
    std::lock_guard<std::mutex> lk(m_);
    return transcript_;
}

void SttEngine::workerInit() {
    // Engine init happens here; start() requests arriving before the
    // models load set want_listen_ and are re-armed below.
    initEngine();
}

void SttEngine::initEngine() {
    static const bool logInstalled = [] {
        whisper_log_set(whisperLogToStderr, nullptr);
        return true;
    }();
    (void)logInstalled;

    // Pre-flight the files ourselves: whisper's "failed to open" alone
    // can't say WHICH model is missing, and a truncated download opens
    // fine and dies deep inside ggml. The probe names both.
    const SttModelStatus st = probeSttModels(modelDir());
    fprintf(stderr, "hive-stt: model probe — %s\n", st.summary().c_str());
    if (!st.ready()) {
        if (on_stopped_) on_stopped_(st.userMessage());
        return;
    }

    whisper_context_params cparams = whisper_context_default_params();
    ctx_partial_ =
        whisper_init_from_file_with_params(st.partial.path.c_str(), cparams);
    ctx_final_ =
        whisper_init_from_file_with_params(st.final.path.c_str(), cparams);
    if (!ctx_partial_ || !ctx_final_) {
        // Files probed clean but ggml rejected one — log exactly which.
        fprintf(stderr,
                "hive-stt: whisper_init FAILED despite clean probe "
                "(partial=%s final=%s) — file(s) corrupt or not ggml "
                "whisper models\n",
                ctx_partial_ ? "ok" : "FAILED",
                ctx_final_ ? "ok" : "FAILED");
        if (ctx_partial_) { whisper_free(ctx_partial_); ctx_partial_ = nullptr; }
        if (ctx_final_) { whisper_free(ctx_final_); ctx_final_ = nullptr; }
        if (on_stopped_)
            on_stopped_("Speech models exist but whisper could not parse "
                        "them (see hive-stt log) — re-download from " +
                        std::string(kSttModelBaseUrl));
        return;
    }
    fprintf(stderr, "hive-stt: models loaded — partial=%s final=%s\n",
            st.partial.path.c_str(), st.final.path.c_str());
    ready_ = true;
    if (want_listen_) startCapture();
}

void SttEngine::start() {
    want_listen_ = true;
    const int s = ++session_;
    // Cheap checks inline; anything heavy runs on the worker via a
    // detached task so start() never blocks the UI.
    if (listening_) {
        teardownCapture();
    }
    if (!ready_) {
        std::thread([this, s] {
            if (!want_listen_ || s != session_) return;
            initEngine();
        }).detach();
        return;
    }
    startCapture();
}

void SttEngine::startCapture() {
    if (!ready_ || listening_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        transcript_.clear();
        pcm_.assign(kSampleRate * 30, 0);
        pcm_len_ = 0;
    }
    const int s = session_.load();
    listening_ = capture_.start(
        [this, s](const int16_t* samples, std::size_t n) {
            if (s != session_) return;  // stale capture
            if (on_level_) on_level_(0.0f);  // level comes via LevelCallback
            std::lock_guard<std::mutex> lk(m_);
            if (pcm_len_ + n > kMaxSamples) {
                const size_t keep = kMaxSamples / 2;
                std::copy_n(pcm_.data() + (pcm_len_ - keep), keep,
                            pcm_.data());
                pcm_len_ = keep;
            }
            if (pcm_len_ + n > pcm_.size())
                pcm_.resize(std::min(pcm_.size() * 2, kMaxSamples));
            std::copy_n(samples, n, pcm_.data() + pcm_len_);
            pcm_len_ += n;
        },
        [this, s](float level) {
            if (s == session_ && on_level_) on_level_(level);
        });
    if (!listening_ && on_stopped_)
        on_stopped_("Microphone unavailable — no audio capture backend");
}

void SttEngine::teardownCapture() {
    listening_ = false;
    capture_.stop();
}

void SttEngine::stop(std::function<void()> onFinalized) {
    want_listen_ = false;
    ++session_;
    std::thread([this, onFinalized = std::move(onFinalized)]() mutable {
        if (listening_) teardownCapture();
        if (ready_) {
            std::vector<int16_t> snap;
            {
                std::lock_guard<std::mutex> lk(m_);
                snap.assign(pcm_.data(), pcm_.data() + pcm_len_);
            }
            if (!snap.empty() && ctx_final_) {
                const auto t0 = std::chrono::steady_clock::now();
                whisper_full_params wparams =
                    whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                wparams.print_progress = false;
                wparams.print_special = false;
                wparams.print_realtime = false;
                wparams.print_timestamps = false;
                wparams.translate = false;
                wparams.single_segment = false;
                wparams.no_timestamps = true;
                wparams.language = "en";
                wparams.n_threads = kThreads;
                // whisper_full takes float PCM; convert.
                std::vector<float> f(snap.size());
                for (size_t i = 0; i < snap.size(); ++i)
                    f[i] = snap[i] / 32768.0f;
                if (whisper_full(ctx_final_, wparams, f.data(),
                                 static_cast<int>(f.size())) == 0) {
                    const int n = whisper_full_n_segments(ctx_final_);
                    std::string text;
                    for (int i = 0; i < n; ++i) {
                        const char* seg =
                            whisper_full_get_segment_text(ctx_final_, i);
                        if (seg) {
                            if (!text.empty()) text += ' ';
                            text += seg;
                        }
                    }
                    // Trim + set under the lock (last write wins, like the
                    // Kotlin transcript var).
                    const auto b = text.find_first_not_of(" \t\n\r");
                    const auto e = text.find_last_not_of(" \t\n\r");
                    if (b != std::string::npos) {
                        std::lock_guard<std::mutex> lk(m_);
                        transcript_ = text.substr(b, e - b + 1);
                    }
                }
                const auto dt = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                fprintf(stderr, "hive-stt: final decode %lldms for %zus audio\n",
                        static_cast<long long>(dt), snap.size() / kSampleRate);
            }
        }
        if (onFinalized) onFinalized();
    }).detach();
}

void SttEngine::discard() {
    want_listen_ = false;
    ++session_;
    {
        std::lock_guard<std::mutex> lk(m_);
        transcript_.clear();
        pcm_len_ = 0;
    }
    if (listening_) teardownCapture();
}

void SttEngine::partialLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPartialMs));
        if (listening_) decodePartial();
    }
}

void SttEngine::decodePartial() {
    if (!ready_ || decode_in_flight_) return;
    std::vector<int16_t> snap;
    {
        std::lock_guard<std::mutex> lk(m_);
        const size_t n = std::min(pcm_len_, size_t{kWindowSamples});
        if (n < kSampleRate / 2) return;  // ignore sub-500ms blips
        snap.assign(pcm_.data() + (pcm_len_ - n), pcm_.data() + pcm_len_);
    }
    int peak = 0;
    for (const int16_t v : snap)
        peak = std::max(peak, static_cast<int>(std::abs(static_cast<int>(v))));
    if (peak < kVadPeakMin) return;  // silent window — skip the CPU cost
    decode_in_flight_ = true;
    std::thread([this, snap = std::move(snap)]() mutable {
        std::string text;
        if (ctx_partial_) {
            std::vector<float> f(snap.size());
            for (size_t i = 0; i < snap.size(); ++i)
                f[i] = snap[i] / 32768.0f;
            whisper_full_params wparams =
                whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            wparams.print_progress = false;
            wparams.print_special = false;
            wparams.print_realtime = false;
            wparams.print_timestamps = false;
            wparams.translate = false;
            wparams.single_segment = true;
            wparams.no_timestamps = true;
            wparams.language = "en";
            wparams.n_threads = kThreads;
            if (whisper_full(ctx_partial_, wparams, f.data(),
                             static_cast<int>(f.size())) == 0) {
                const int n = whisper_full_n_segments(ctx_partial_);
                for (int i = 0; i < n; ++i) {
                    const char* seg =
                        whisper_full_get_segment_text(ctx_partial_, i);
                    if (seg) {
                        if (!text.empty()) text += ' ';
                        text += seg;
                    }
                }
                const auto b = text.find_first_not_of(" \t\n\r");
                const auto e = text.find_last_not_of(" \t\n\r");
                text = (b == std::string::npos)
                           ? ""
                           : text.substr(b, e - b + 1);
            }
        }
        if (!text.empty() && listening_) {
            {
                std::lock_guard<std::mutex> lk(m_);
                transcript_ = text;
            }
            if (on_transcript_) on_transcript_(text);
        }
        decode_in_flight_ = false;
    }).detach();
}

}  // namespace hv
