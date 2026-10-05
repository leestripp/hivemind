#pragma once

#include "AudioCommon.h"

#include <atomic>
#include <thread>

namespace hv {

// Mic capture: delivers 100 ms mono s16le chunks + a smoothed 0..1 level.
// With no PipeWire backend start() fails (there is no honest null mic).
class AudioCapture {
 public:
    AudioCapture() = default;
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool available() const;  // true = real capture (PipeWire)

    // Blocking start; returns false when the backend is missing or the
    // stream fails. Callbacks fire on an internal thread.
    bool start(ChunkCallback chunk, LevelCallback level);
    void stop();  // join the capture thread

 private:
    void captureLoop(const ChunkCallback& chunk, const LevelCallback& level);

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace hv
