#include "AudioPlayback.h"

#include <chrono>
#include <utility>

#ifdef HIVEMIND_VOICE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace hv {

AudioPlayback::AudioPlayback() = default;

AudioPlayback::~AudioPlayback() { stop(); }

bool AudioPlayback::available() const {
#ifdef HIVEMIND_VOICE_PIPEWIRE
    return true;
#else
    return false;
#endif
}

void AudioPlayback::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (running_) return;
    running_ = true;
    worker_ = std::thread([this] { workerLoop(); });
}

void AudioPlayback::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return;
        running_ = false;
        ++generation_;  // pending tags must never fire
        queue_.clear();
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void AudioPlayback::clear() {
    std::lock_guard<std::mutex> lk(m_);
    ++generation_;
    queue_.clear();
}

void AudioPlayback::submit(std::uint64_t tag, Pcm16 samples) {
    {
        std::lock_guard<std::mutex> lk(m_);
        queue_.emplace_back(tag, std::move(samples));
    }
    cv_.notify_one();
}

void AudioPlayback::setOnTagDone(std::function<void(std::uint64_t)> cb) {
    std::lock_guard<std::mutex> lk(m_);
    on_tag_done_ = std::move(cb);
}

#ifdef HIVEMIND_VOICE_PIPEWIRE
namespace {

// PipeWire playback sink, 16 kHz mono s16le, fed from the worker thread.
// Canonical pw_thread_loop + process-callback pattern (per
// pipewire examples/audio-play.c); the process callback dequeues
// buffers and signals the feeder, which copies from the current
// utterance into each dequeued buffer.
struct PwSink {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    bool ok = false;

    static void onProcess(void* data) {
        auto* self = static_cast<PwSink*>(data);
        pw_thread_loop_signal(self->loop, false);
    }

    static void onStateChanged(void* data, pw_stream_state /*old*/,
                               pw_stream_state state, const char* error) {
        auto* self = static_cast<PwSink*>(data);
        if (state == PW_STREAM_STATE_ERROR)
            fprintf(stderr, "hive-voice: playback stream error: %s\n", error);
        pw_thread_loop_signal(self->loop, false);
    }

    static const pw_stream_events events;

    bool open() {
        pw_init(0, nullptr);
        loop = pw_thread_loop_new("hive-voice-play", nullptr);
        if (!loop) return false;
        if (pw_thread_loop_start(loop) != 0) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            return false;
        }
        pw_thread_loop_lock(loop);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Notification", nullptr);
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                      "hive-voice-play", props, &events,
                                      this);
        if (!stream) {
            pw_thread_loop_unlock(loop);
            pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            return false;
        }
        spa_audio_info_raw fmt = {};
        fmt.format = SPA_AUDIO_FORMAT_S16_LE;
        fmt.channels = kChannels;
        fmt.rate = kSampleRate;
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &fmt);
        int res = pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            &params, 1);
        if (res < 0) {
            pw_stream_destroy(stream);
            stream = nullptr;
            pw_thread_loop_unlock(loop);
            pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            return false;
        }
        pw_thread_loop_unlock(loop);
        ok = true;
        return true;
    }

    void close() {
        if (stream) pw_stream_destroy(stream);
        if (loop) {
            pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
        }
        pw_deinit();
    }

    // Copy up to `avail` samples from `src` into the next stream buffer;
    // returns samples written. Blocking while the device is full.
    std::size_t feed(const int16_t* src, std::size_t avail) {
        pw_thread_loop_lock(loop);
        for (;;) {
            pw_buffer* b = pw_stream_dequeue_buffer(stream);
            if (b) {
                std::size_t n = 0;
                if (b->buffer->datas[0].data) {
                    const std::size_t capacity =
                        b->buffer->datas[0].maxsize / sizeof(int16_t);
                    n = std::min(capacity, avail);
                    std::copy_n(src, n,
                                static_cast<int16_t*>(b->buffer->datas[0].data));
                    b->buffer->datas[0].chunk->offset = 0;
                    b->buffer->datas[0].chunk->stride = sizeof(int16_t);
                    b->buffer->datas[0].chunk->size = n * sizeof(int16_t);
                }
                pw_stream_queue_buffer(stream, b);
                pw_thread_loop_unlock(loop);
                return n;
            }
            pw_thread_loop_wait(loop);
        }
    }
};

const pw_stream_events PwSink::events = {
    PW_VERSION_STREAM_EVENTS, nullptr, &PwSink::onStateChanged,
    nullptr, &PwSink::onProcess, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

}  // namespace
#endif  // HIVEMIND_VOICE_PIPEWIRE

void AudioPlayback::workerLoop() {
#ifdef HIVEMIND_VOICE_PIPEWIRE
    PwSink sink;
    if (!sink.open()) {
        // Fall through to the timed path below with pw_stream == nullptr.
    }
#endif
    for (;;) {
        std::pair<std::uint64_t, Pcm16> item;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
            if (!running_) break;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        std::size_t written = 0;
        while (running_ && written < item.second.size()) {
#ifdef HIVEMIND_VOICE_PIPEWIRE
            const std::size_t n = sink.ok
                ? sink.feed(item.second.data() + written,
                            item.second.size() - written)
                : 0;
            if (n == 0) {
                // No usable stream: advance by wall-clock instead.
                const std::size_t step =
                    std::min<std::size_t>(kSampleRate / 10,
                                          item.second.size() - written);
                written += step;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                written += n;
            }
#else
            const std::size_t step =
                std::min<std::size_t>(kSampleRate / 10,
                                      item.second.size() - written);
            written += step;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
        }
        std::function<void(std::uint64_t)> cb;
        {
            std::lock_guard<std::mutex> lk(m_);
            cb = on_tag_done_;
        }
        if (cb && running_) cb(item.first);
    }
#ifdef HIVEMIND_VOICE_PIPEWIRE
    if (sink.ok) sink.close();
#endif
}

}  // namespace hv
