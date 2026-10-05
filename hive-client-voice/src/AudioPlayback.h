#pragma once

#include "AudioCommon.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace hv {

// Serial PCM playback queue with per-utterance completion. Utterances
// are submitted tagged; onTagDone fires (on an internal worker thread)
// when that tag's final sample has been handed to the audio device.
// With no PipeWire backend the queue still runs — tags complete after
// their nominal duration — so the rest of the app (TTS flow, sphere)
// works silently on machines without the dev package.
class AudioPlayback {
 public:
    AudioPlayback();
    ~AudioPlayback();
    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    bool available() const;  // true = real audio out (PipeWire)

    void start();
    void stop();    // halt + drain; pending tags never fire
    void clear();   // drop everything queued; pending tags never fire

    void submit(std::uint64_t tag, Pcm16 samples);

    void setOnTagDone(std::function<void(std::uint64_t)> cb);

 private:
    void workerLoop();

    std::function<void(std::uint64_t)> on_tag_done_;
    std::mutex m_;
    std::deque<std::pair<std::uint64_t, Pcm16>> queue_;
    std::condition_variable cv_;
    bool running_ = false;
    std::uint64_t generation_ = 0;  // bumped on clear/stop
    std::thread worker_;
};

}  // namespace hv
