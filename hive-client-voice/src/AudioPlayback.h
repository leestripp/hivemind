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

    // `rate` is the PCM's native sample rate (e.g. 24000 from the TTS
    // WAV); the sink opens at that rate and the device resamples.
    void submit(std::uint64_t tag, Pcm16 samples, int rate);

    // Called by the PipeWire sink (process callback consumed the last
    // sample of this tag). Not for general use.
    void itemDone(std::uint64_t tag);

    void setOnTagDone(std::function<void(std::uint64_t)> cb);

    // Push-to-talk hold (2026-10-14): while the probe reports the user
    // is recording, playback FREEZES — the sink pads silence WITHOUT
    // consuming the ring (no samples move, no tags complete, nothing
    // is lost) and resumes mid-word when recording ends. A derived-
    // state probe, not set/forget flags: a missed setHeld(false)
    // would mute the app forever. Set once at startup, before audio
    // starts; the probe must be lock-free (atomic load).
    void setHoldProbe(std::function<bool()> probe);
    bool held() const { return hold_probe_ ? hold_probe_() : false; }

 private:
    struct Item {
        std::uint64_t tag;
        Pcm16 samples;
        int rate;
    };

    void workerLoop();

    std::function<void(std::uint64_t)> on_tag_done_;
    std::function<bool()> hold_probe_;  // set once at startup
    std::mutex m_;
    std::deque<Item> queue_;
    std::deque<std::uint64_t> done_;  // tags consumed by the sink
    bool clearSink_ = false;          // worker must drop the sink's ring
    std::condition_variable cv_;
    bool running_ = false;
    std::uint64_t generation_ = 0;  // bumped on clear/stop
    std::thread worker_;
};

}  // namespace hv
