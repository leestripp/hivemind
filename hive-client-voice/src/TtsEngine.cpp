#include "TtsEngine.h"

#include "AudioPlayback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>

namespace hv {

TtsEngine::TtsEngine(AudioPlayback& playback) : playback_(playback) {
    playback_.setOnTagDone([this](std::uint64_t tag) { onTagDone(tag); });
    {
        std::lock_guard<std::mutex> lk(m_);
        running_ = true;
    }
    worker_ = std::thread([this] { workerLoop(); });
}

TtsEngine::~TtsEngine() {
    {
        std::lock_guard<std::mutex> lk(m_);
        running_ = false;
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void TtsEngine::applyVoice(const std::string& modelPath) {
    std::lock_guard<std::mutex> lk(m_);
    voice_ = modelPath;
}

std::string TtsEngine::voice() const {
    std::lock_guard<std::mutex> lk(m_);
    return voice_;
}

void TtsEngine::speakQueued(const std::string& text) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (queue_.size() >= 40) return;  // never-ready cap, as Android
        queue_.push_back({epoch_, nextTag_++, text, voice_});
    }
    outstanding_++;
    cv_.notify_one();
}

void TtsEngine::stop() {
    std::deque<Utterance> dead;
    {
        std::lock_guard<std::mutex> lk(m_);
        epoch_++;  // in-flight tags from before are now stale
        dead.swap(queue_);
        queue_.clear();
    }
    outstanding_ = 0;
    speaking_ = false;
    playback_.clear();
}

void TtsEngine::preview(const std::string& text, const std::string& modelPath) {
    stop();  // QUEUE_FLUSH: interrupt anything playing
    Pcm16 pcm = synthesize(text, modelPath);
    const std::uint64_t tag = [&] {
        std::lock_guard<std::mutex> lk(m_);
        return nextTag_++;
    }();
    if (!pcm.empty()) playback_.submit(tag, std::move(pcm));
}

float TtsEngine::level(float nowSec) const {
    if (!speaking_) return 0.0f;
    const float base = 0.38f + 0.22f * std::sin(nowSec * 7.3f) *
                                   std::sin(nowSec * 2.1f + 1.3f);
    return std::clamp(base, 0.08f, 0.75f);
}

std::vector<TtsEngine::VoiceOption> TtsEngine::voiceOptions(
    const std::string& voiceDir) {
    std::vector<VoiceOption> out;
    std::error_code ec;
    for (const auto& e :
         std::filesystem::directory_iterator(voiceDir, ec)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension().string();
        if (ext != ".onnx") continue;
        out.push_back({e.path().string(), e.path().stem().string()});
    }
    std::sort(out.begin(), out.end(),
              [](const VoiceOption& a, const VoiceOption& b) {
                  return a.label < b.label;
              });
    return out;
}

void TtsEngine::workerLoop() {
    for (;;) {
        Utterance u;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            u = std::move(queue_.front());
            queue_.pop_front();
        }
        const std::uint64_t epochAtSpawn = u.epoch;
        Pcm16 pcm = synthesize(u.text, u.voice);
        // A stop() landed while piper was running: the utterance is dead,
        // its tag is stale — do not touch the queue accounting.
        {
            std::lock_guard<std::mutex> lk(m_);
            if (epoch_ != epochAtSpawn) continue;
        }
        if (!pcm.empty()) playback_.submit(u.tag, std::move(pcm));
    }
}

void TtsEngine::onTagDone(std::uint64_t tag) {
    // Late callbacks from a stop()ed epoch are harmless: stop() already
    // zeroed outstanding_, so this decrement never fires below zero.
    (void)tag;
    if (outstanding_ > 0) --outstanding_;
    if (outstanding_ == 0) {
        speaking_ = false;
        if (onQueueEmpty) onQueueEmpty();
    }
}

namespace {

// piper invocation: `piper --model V --output-raw` with the sentence on
// stdin; stdout is raw s16le mono PCM at the voice's sample rate (22.05k
// for the standard en voices — AudioPlayback resamples-free plays it at
// 16k which is close enough for v1; see README).
struct PiperChild {
    pid_t pid = -1;
    int stdinFd = -1;
    int stdoutFd = -1;
};

bool spawnPiper(PiperChild& child, const std::string& voice) {
    int inPipe[2], outPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) return false;
    const pid_t pid = fork();
    if (pid == -1) return false;
    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        std::string model = voice;
        std::string piperBin = "piper";
        const char* home = getenv("HOME");
        if (home) {
            const std::string local = std::string(home) + "/.local/bin/piper";
            if (access(local.c_str(), X_OK) == 0) piperBin = local;
        }
        if (model.empty()) {
            const char* env = getenv("HIVEMIND_VOICE_DEFAULT");
            if (env) model = env;
        }
        if (model.empty()) {
            const std::string def = std::string(home ? home : ".") +
                "/.local/share/hive-client-voice/voices/default.onnx";
            if (access(def.c_str(), R_OK) == 0) model = def;
        }
        if (model.empty()) _exit(127);
        execlp(piperBin.c_str(), piperBin.c_str(), "--model", model.c_str(),
               "--output-raw", static_cast<const char*>(nullptr));
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    child.pid = pid;
    child.stdinFd = inPipe[1];
    child.stdoutFd = outPipe[0];
    return true;
}

}  // namespace

Pcm16 TtsEngine::synthesize(const std::string& text,
                            const std::string& voice) {
    PiperChild child;
    if (!spawnPiper(child, voice)) return {};
    // Sentence on stdin, then close so piper decodes exactly it.
    const std::string line = text + "\n";
    std::size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = write(child.stdinFd, line.data() + off,
                                line.size() - off);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
    }
    close(child.stdinFd);
    // Read raw s16le to EOF.
    Pcm16 pcm;
    std::array<char, 8192> buf{};
    for (;;) {
        const ssize_t n = read(child.stdoutFd, buf.data(), buf.size());
        if (n <= 0) break;
        const auto* s = reinterpret_cast<const int16_t*>(buf.data());
        pcm.insert(pcm.end(), s, s + n / 2);
    }
    close(child.stdoutFd);
    int status = 0;
    waitpid(child.pid, &status, 0);
    if (status != 0) pcm.clear();  // synthesis failed — silence, not noise
    speaking_ = true;
    return pcm;
}

}  // namespace hv
