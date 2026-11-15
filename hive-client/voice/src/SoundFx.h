#pragma once

#include <memory>

namespace hv {

class AudioPlayback;

// Short confirmation tones for the push-to-talk flow, so the opening
// and closing of mic recording is audible when TALK is driven from the
// keyboard shortcut (global key) instead of the on-screen button.
// Rising two-tone when recording arms, falling two-tone when it closes
// (SEND or CANCEL).
//
// Owns a DEDICATED AudioPlayback instance: effects never enter the TTS
// queue, so a beep sounds instantly even mid-interrupt without touching
// utterance tag sequencing. Pure PCM synthesis (sine blips with edge
// fades) — no sound files, no new dependencies. On a build without
// PipeWire the queue runs silent by wall-clock, matching the TTS
// contract.
class SoundFx {
 public:
    SoundFx();
    ~SoundFx();
    SoundFx(const SoundFx&) = delete;
    SoundFx& operator=(const SoundFx&) = delete;

    // Safe from any thread.
    void recordingStarted() const;
    void recordingStopped() const;

 private:
    std::unique_ptr<AudioPlayback> playback_;
};

}  // namespace hv
