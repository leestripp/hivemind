#include "AudioCapture.h"

#include <cmath>

#ifdef HIVEMIND_VOICE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <vector>
#endif

namespace hv {

AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::available() const {
#ifdef HIVEMIND_VOICE_PIPEWIRE
    return true;
#else
    return false;
#endif
}

void AudioCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

#ifdef HIVEMIND_VOICE_PIPEWIRE
namespace {

// PipeWire capture source, 16 kHz mono s16le. The process callback
// converts each mapped buffer to s16 chunks and hands them to the
// consumer running under the thread loop; the capture thread just
// pumps pw_thread_loop_start/stop.
struct PwSource {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    ChunkCallback chunk;
    LevelCallback level;
    bool ok = false;

    static void onProcess(void* data) {
        auto* self = static_cast<PwSource*>(data);
        pw_buffer* b = nullptr;
        while ((b = pw_stream_dequeue_buffer(self->stream)) != nullptr) {
            const auto& d = b->buffer->datas[0];
            if (d.data && d.chunk->size > 0) {
                // The format adapter gives us f32; convert to s16 here.
                const auto* f =
                    static_cast<const float*>(static_cast<const void*>(
                        static_cast<const char*>(d.data) + d.chunk->offset));
                const std::size_t frames =
                    d.chunk->size / (sizeof(float) * kChannels);
                std::vector<int16_t> out(frames);
                for (std::size_t i = 0; i < frames; ++i) {
                    const float v = std::clamp(f[i] * 32767.0f, -32768.0f,
                                               32767.0f);
                    out[i] = static_cast<int16_t>(v);
                }
                if (self->chunk && !out.empty())
                    self->chunk(out.data(), out.size());
            }
            pw_stream_queue_buffer(self->stream, b);
        }
    }

    static void onStateChanged(void* data, pw_stream_state /*old*/,
                               pw_stream_state state, const char* error) {
        auto* self = static_cast<PwSource*>(data);
        if (state == PW_STREAM_STATE_ERROR)
            fprintf(stderr, "hive-voice: capture stream error: %s\n", error);
        pw_thread_loop_signal(self->loop, false);
    }

    static const pw_stream_events events;

    bool open() {
        pw_init(0, nullptr);
        loop = pw_thread_loop_new("hive-voice-rec", nullptr);
        if (!loop) return false;
        if (pw_thread_loop_start(loop) != 0) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            return false;
        }
        pw_thread_loop_lock(loop);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Speech", nullptr);
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                      "hive-voice-rec", props, &events,
                                      this);
        if (!stream) {
            pw_thread_loop_unlock(loop);
            pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            return false;
        }
        spa_audio_info_raw fmt = {};
        fmt.format = SPA_AUDIO_FORMAT_F32_LE;
        fmt.channels = kChannels;
        fmt.rate = kSampleRate;
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &fmt);
        int res = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
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
};

const pw_stream_events PwSource::events = {
    PW_VERSION_STREAM_EVENTS, nullptr, &PwSource::onStateChanged,
    nullptr, &PwSource::onProcess, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

}  // namespace
#endif  // HIVEMIND_VOICE_PIPEWIRE

bool AudioCapture::start(ChunkCallback chunk, LevelCallback level) {
    if (running_) return true;
#ifdef HIVEMIND_VOICE_PIPEWIRE
    running_ = true;
    thread_ = std::thread([this, chunk = std::move(chunk),
                           level = std::move(level)]() mutable {
        PwSource src;
        src.chunk = std::move(chunk);
        src.level = std::move(level);
        if (!src.open()) {
            running_ = false;
            return;
        }
        // Adaptive level reference (fast attack, slow release) exactly as
        // the Android capture loop: mic gain varies wildly, so a fixed
        // RMS divisor leaves the sphere dead on quiet mics.
        double rmsRef = 400.0;
        // Synthesize 100 ms level ticks from the chunk stream: the chunk
        // callback fires per PipeWire buffer (~5-20 ms), so accumulate
        // here over 1600 samples before reporting.
        auto wrapped = src.chunk;
        std::size_t acc = 0;
        double accSum = 0.0;
        src.chunk = [&, wrapped](const int16_t* s, std::size_t n) {
            if (wrapped) wrapped(s, n);
            for (std::size_t i = 0; i < n; ++i) {
                const double v = s[i];
                accSum += v * v;
            }
            acc += n;
            if (acc >= static_cast<std::size_t>(kSampleRate / 10)) {
                const double rms = std::sqrt(accSum / acc);
                rmsRef = (rms > rmsRef) ? rms : rmsRef * 0.999 + rms * 0.001;
                if (src.level)
                    src.level(std::clamp(rms / (rmsRef * 1.3), 0.0, 1.0));
                acc = 0;
                accSum = 0.0;
            }
        };
        while (running_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        src.close();
    });
    return true;
#else
    (void)chunk;
    (void)level;
    return false;  // no backend — the caller must say so
#endif
}

void AudioCapture::captureLoop(const ChunkCallback&, const LevelCallback&) {}

}  // namespace hv
