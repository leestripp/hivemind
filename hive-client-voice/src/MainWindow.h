#pragma once

#include <gtkmm.h>

#include "ApiClient.h"  // AgentInfo

class Prefs;

namespace hv {

class RosterView;
class VoiceView;

// THE window — the app has exactly one. Inside, a GtkStack switches
// between the roster page and an on-demand voice-session page:
// tap an agent -> voice page slides in; < Back -> roster, session ends.
// Settings / voice picker open as modal dialogs on this window.
class MainWindow : public Gtk::Window {
 public:
    explicit MainWindow(Prefs& prefs);

    void showVoice(const AgentInfo& agent);
    void showRoster();

 private:
    Prefs& prefs_;
    Gtk::Stack* stack_ = nullptr;
    RosterView* roster_ = nullptr;  // owned by the stack

    // Voice page is builder-derived: the builder owns the widget, so the
    // builder must outlive it. Dropping both ends the session.
    Glib::RefPtr<Gtk::Builder> voiceBuilder_;
    VoiceView* voice_ = nullptr;
};

}  // namespace hv
