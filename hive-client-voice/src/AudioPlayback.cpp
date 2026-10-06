#include "AudioPlayback.h"

#include <chrono>
#include <cstdio>
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
    done_.clear();  // pending tags must never fire
    clearSink_ = true;
    cv_.notify_all();
}

void AudioPlayback::itemDone(std::uint64_t tag) {
    std::lock_guard<std::mutex> lk(m_);
    done_.push_back(tag);
    cv_.notify_all();
}

void AudioPlayback::submit(std::uint64_t tag, Pcm16 samples, int rate) {
    {
        std::lock_guard<std::mutex> lk(m_);
        queue_.push_back({tag, std::move(samples), rate});
    }
    cv_.notify_one();
}

void AudioPlayback::setOnTagDone(std::function<void(std::uint64_t)> cb) {
    std::lock_guard<std::mutex> lk(m_);
    on_tag_done_ = std::move(cb);
}

#ifdef HIVEMIND_VOICE_PIPEWIRE
namespace {

// PipeWire playback sink fed from an internal ring buffer.
//
// Canonical pattern per docs.pipewire.org/page_streams.html ("It is
// recommended to call pw_stream_dequeue_buffer() and
// pw_stream_queue_buffer() from the process() callback") — the
// process callback drains the ring and fills EVERY dequeued buffer
// in one go; buffers queued outside process() are delayed a cycle,
// and each delayed cycle is one more chance to underrun (pop).
//
// Buffers are padded with silence when the ring is momentarily dry
// (next utterance still synthesizing). The device NEVER underruns:
// underruns, not the silence itself, are what click. PW 1.6 headers
// ship PW_STREAM_FLAG_EARLY_PROCESS (0.3.81+): "call process as soon
// as there is a buffer to dequeue ... keep the maximum number of
// buffers queued" — exactly what a push feeder wants, so we set it.
struct PwSink {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    bool ok = false;

    std::mutex ringM_;
    Pcm16 ring_;                    // pending samples, fed by worker
    std::size_t ringPos_ = 0;       // read cursor into ring_
    // (tag, unplayed sample count) in play order. The ring queues ANY
    // number of utterances; a tag completes the moment its count
    // reaches zero inside fill().
    std::deque<std::pair<std::uint64_t, std::size_t>> ringTags_;
    bool starvedLogged_ = false;

    AudioPlayback* owner = nullptr;

    static void onProcess(void* data) {
        auto* self = static_cast<PwSink*>(data);
        pw_buffer* b = nullptr;
        while ((b = pw_stream_dequeue_buffer(self->stream)) != nullptr)
            self->fill(b);
    }

    static void onStateChanged(void* data, pw_stream_state /*old*/,
                               pw_stream_state state, const char* error) {
        auto* self = static_cast<PwSink*>(data);
        if (state == PW_STREAM_STATE_ERROR)
            fprintf(stderr, "hive-voice: playback stream error: %s\n", error);
        if (state == PW_STREAM_STATE_STREAMING) self->starvedLogged_ = false;
    }

    static const pw_stream_events events;

    bool open(int rate) {
        pw_init(0, nullptr);
        loop = pw_thread_loop_new("hive-voice-play", nullptr);
        if (!loop) return false;
        if (pw_thread_loop_start(loop) != 0) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            return false;
        }
        pw_thread_loop_lock(loop);
        char latencyFrac[32];
        std::snprintf(latencyFrac, sizeof(latencyFrac), "%d/%d", rate / 10,
                      rate);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Notification", PW_KEY_NODE_LATENCY,
            latencyFrac, nullptr);
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
        fmt.rate = rate;
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &fmt);
        const int res = pw_stream_connect(
            stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_EARLY_PROCESS),
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
        // Destroy the stream WHILE HOLDING the thread-loop lock (the
        // earlier unlocked teardown raced the process callback and
        // segfaulted — see the 2026-10-05 picker-preview crash).
        if (loop) pw_thread_loop_lock(loop);
        if (stream) {
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (loop) {
            pw_thread_loop_unlock(loop);
            pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
            loop = nullptr;
        }
        pw_deinit();
        std::lock_guard<std::mutex> lk(ringM_);
        ring_.clear();
        ringPos_ = 0;
        ringTags_.clear();
    }

    // Worker thread: append one utterance to the ring. Utterances
    // QUEUE behind whatever is playing — synthesis is often faster
    // than playback, so the previous utterance's unplayed tail must
    // survive. Dropping it (the old single-slot behaviour) both cut
    // the audio mid-sentence and lost the tag completion the TTS
    // engine's outstanding-utterance accounting waits on — the app
    // then sat in Streaming with the Stop chrome showing forever.
    void feed(Pcm16 samples, std::uint64_t tag) {
        std::lock_guard<std::mutex> lk(ringM_);
        if (ringPos_ > 0) {  // compact the consumed prefix
            ring_.erase(ring_.begin(), ring_.begin() + ringPos_);
            ringPos_ = 0;
        }
        ring_.insert(ring_.end(), samples.begin(), samples.end());
        ringTags_.push_back({tag, samples.size()});
    }

    // Stop/clear: drop everything queued + playing. The dropped tags
    // never fire — that is clear()'s contract ("pending tags never
    // fire"); the caller has already zeroed its own accounting.
    void dropRing() {
        std::lock_guard<std::mutex> lk(ringM_);
        ring_.clear();
        ringPos_ = 0;
        ringTags_.clear();
    }

    // Drain the ring into every dequeued buffer; silence-pad the tail.
    // Runs on the thread-loop thread inside process() — no RT
    // restrictions, plain mutex is fine here.
    void fill(pw_buffer* b) {
        spa_data& d = b->buffer->datas[0];
        if (!d.data) {  // unmapped buffer: return as explicit silence
            d.chunk->offset = 0;
            d.chunk->stride = sizeof(int16_t);
            d.chunk->size = 0;
            pw_stream_queue_buffer(stream, b);
            return;
        }
        const std::size_t capacity = d.maxsize / sizeof(int16_t);
        auto* dst = static_cast<int16_t*>(d.data);
        std::size_t n = 0;
        std::vector<std::uint64_t> finishedTags;
        {
            std::lock_guard<std::mutex> lk(ringM_);
            const std::size_t avail = ring_.size() - ringPos_;
            n = std::min(capacity, avail);
            std::copy_n(ring_.data() + ringPos_, n, dst);
            ringPos_ += n;
            // Walk the play order: every tag whose last sample this
            // buffer just consumed is complete.
            std::size_t consumed = n;
            while (consumed > 0 && !ringTags_.empty()) {
                auto& [tag, left] = ringTags_.front();
                const std::size_t take = std::min(left, consumed);
                left -= take;
                consumed -= take;
                if (left == 0) {
                    finishedTags.push_back(tag);
                    ringTags_.pop_front();
                }
            }
            if (ringPos_ == ring_.size()) {
                ring_.clear();
                ringPos_ = 0;
            }
        }
        if (n < capacity) {
            // Starved: pad with silence so the device clock never
            // gaps. One log line per stream, not per buffer.
            std::fill_n(dst + n, capacity - n, 0);
            if (!starvedLogged_ && n == 0) {
                starvedLogged_ = true;
                fprintf(stderr, "hive-voice: playback starved (TTS lag?) — "
                                "padding silence\n");
            }
        }
        d.chunk->offset = 0;
        d.chunk->stride = sizeof(int16_t);
        d.chunk->size = capacity * sizeof(int16_t);
        pw_stream_queue_buffer(stream, b);
        // itemDone takes the owner's m_; fire AFTER releasing ringM_ so
        // the two locks are never held together in this direction.
        for (const auto t : finishedTags)
            if (owner) owner->itemDone(t);
    }
};

// Zero-init first, then assign — the pipewire struct layout is
// versioned (PW_VERSION_STREAM_EVENTS=2 has 12 members here), and
// positional init both overflows on newer/older versions and silently
// lands callbacks in the wrong slot (index 4 is io_changed, NOT
// process). A factory also avoids -Wmissing-field-initializers.
pw_stream_events makeEvents() {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = &PwSink::onStateChanged;
    e.process = &PwSink::onProcess;
    return e;
}

const pw_stream_events PwSink::events = makeEvents();

}  // namespace
#endif  // HIVEMIND_VOICE_PIPEWIRE

namespace {

// 5 ms linear fades on both edges of an utterance. Each TTS sentence
// arrives as its own submission and starts/ends at arbitrary waveform
// amplitude; a hard cut to the next sentence (or to silence while the
// next one synthesizes) is exactly the "pop" heard between words.
// 5 ms is below audibility in speech but enough to ramp the waveform
// to/from zero without a discontinuity.
void applyEdgeFades(Pcm16& s, int rate) {
    const std::size_t fade = std::min<std::size_t>(rate / 200, s.size() / 4);
    if (fade == 0) return;
    for (std::size_t i = 0; i < fade; ++i) {
        const float k = static_cast<float>(i + 1) / (fade + 1);
        s[i] = static_cast<int16_t>(s[i] * k);
        s[s.size() - 1 - i] = static_cast<int16_t>(s[s.size() - 1 - i] * k);
    }
}

}  // namespace

void AudioPlayback::workerLoop() {
#ifdef HIVEMIND_VOICE_PIPEWIRE
    PwSink sink;
    sink.owner = this;
    int openedRate = 0;
#endif
    for (;;) {
        Item item;
        std::vector<std::uint64_t> doneTags;
        std::function<void(std::uint64_t)> cb;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] {
                return !running_ || !queue_.empty() || !done_.empty() ||
                       clearSink_;
            });
            if (!running_) break;
            if (clearSink_) {
                clearSink_ = false;
#ifdef HIVEMIND_VOICE_PIPEWIRE
                // Stop means STOP: silence whatever the sink is still
                // playing, not just the not-yet-fed queue. (m_ is held
                // here, ringM_ inside dropRing — fill() never holds
                // ringM_ while taking m_, so no lock-order inversion.)
                if (sink.ok) sink.dropRing();
#endif
            }
            doneTags.assign(done_.begin(), done_.end());
            done_.clear();
            cb = on_tag_done_;
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        for (const auto t : doneTags)
            if (cb) cb(t);
        if (item.samples.empty()) continue;
        applyEdgeFades(item.samples, item.rate);
#ifdef HIVEMIND_VOICE_PIPEWIRE
        // The TTS WAV's native rate can change (engine swap): reopen
        // the sink so we never resample in software.
        if (sink.ok && openedRate != item.rate) {
            sink.close();
            sink.ok = false;
            openedRate = 0;
        }
        if (!sink.ok && sink.open(item.rate)) openedRate = item.rate;
        if (sink.ok) {
            sink.feed(std::move(item.samples), item.tag);
            continue;
        }
#endif
        // No usable stream: advance by wall-clock instead.
        std::size_t written = 0;
        while (running_ && written < item.samples.size()) {
            const std::size_t step = std::min<std::size_t>(
                item.rate / 10, item.samples.size() - written);
            written += step;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (cb) cb(item.tag);
    }
#ifdef HIVEMIND_VOICE_PIPEWIRE
    if (sink.ok) sink.close();
#endif
}
}  // namespace hv
