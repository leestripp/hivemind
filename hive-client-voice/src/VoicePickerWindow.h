#pragma once

#include <gtkmm.h>

#include <memory>
#include <string>

class Prefs;

namespace hv {

class AudioPlayback;
class TtsEngine;

// Voice picker: lists .onnx piper voices from the voices dir, previews
// one on selection, saves the choice per agent. Mirrors
// VoicePickerActivity.kt.
class VoicePickerWindow : public Gtk::Window {
 public:
    VoicePickerWindow(Prefs& prefs, std::string agentId, std::string agentName);
    ~VoicePickerWindow() override;

 private:
    void pick(const std::string& path);

    Prefs& prefs_;
    std::string agentId_;
    std::unique_ptr<AudioPlayback> playback_;
    std::unique_ptr<TtsEngine> tts_;
    Gtk::Box* list_ = nullptr;
};

}  // namespace hv
