#pragma once

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

#include "AgentStateAnnouncer.h"
#include "ApiClient.h"

class Prefs;

namespace hv {

class AudioPlayback;
class TtsEngine;

// Agent roster page — port of hive-android MainActivity using
// hive-client's roster row structure: avatar + name/role + live state
// pill (+spinner), card-styled rows, ⚙ settings, per-card voice button,
// 2.5s state poll with dwell-filtered spoken announcements.
//
// A page (Gtk::Box), not a window: MainWindow stacks it with the voice
// session page. Tapping an agent emits signal_open_voice().
class RosterView : public Gtk::Box {
 public:
    explicit RosterView(Prefs& prefs);
    ~RosterView() override;

    void refresh();

    // Emitted when the user taps an online agent card.
    sigc::signal<void(const AgentInfo&)>& signal_open_voice() {
        return signal_open_voice_;
    }

    // DebugShell hooks.
    const std::vector<AgentInfo>& debugAgents() const { return lastAgents_; }
    bool debugOpenVoice(const std::string& nameSub);

 private:
    void rebuildList(const std::vector<AgentInfo>& agents);
    void openVoice(const AgentInfo& agent);
    void openVoicePicker(const AgentInfo& agent);
    void openSettings();
    void announce(const std::vector<AgentInfo>& agents);

    sigc::signal<void(const AgentInfo&)> signal_open_voice_;

    Prefs& prefs_;
    std::unique_ptr<ApiClient> api_;
    std::vector<AgentInfo> lastAgents_;
    Gtk::Label* status_ = nullptr;
    Gtk::Label* error_ = nullptr;
    Gtk::Spinner* spinner_ = nullptr;
    Gtk::Box* list_ = nullptr;

    // Announcement TTS — created lazily on the first edge so a mute /
    // voiceless setup never pays for the pipeline.
    std::unique_ptr<AudioPlayback> playback_;
    std::unique_ptr<TtsEngine> tts_;
    AgentStateAnnouncer announcer_;

    sigc::connection poll_;
};

}  // namespace hv
