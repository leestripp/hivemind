#pragma once

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

class Prefs;

namespace hv {

class AudioPlayback;
class TtsEngine;

// Voice picker: lists the TTS voices the ChimpFlow gateway serves,
// previews one on click, and commits the choice with OK (Cancel
// discards). The pending selection is always visible — a checkmark +
// bold on the row and a header label — and the roster row shows the
// saved voice per agent. Mirrors VoicePickerActivity.kt's flow.
class VoicePickerWindow : public Gtk::Window {
 public:
    VoicePickerWindow(Prefs& prefs, std::string agentId, std::string agentName);
    ~VoicePickerWindow() override;

 private:
    struct Row {
        std::string voice;  // "" = engine default
        Gtk::ListBoxRow* row = nullptr;
        Gtk::Label* mark = nullptr;
        Gtk::Label* name = nullptr;
    };

    void select(const std::string& voice);  // preview + move the indicator
    void updateMarks();

    Prefs& prefs_;
    std::string agentId_;
    std::string pending_;  // selection awaiting OK; "" = default
    std::unique_ptr<AudioPlayback> playback_;
    std::unique_ptr<TtsEngine> tts_;
    Gtk::Label* selected_ = nullptr;
    std::vector<Row> rows_;
};

}  // namespace hv
