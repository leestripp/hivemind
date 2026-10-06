#pragma once

#include "Prefs.h"

#include <gtkmm.h>

#include <memory>

namespace hv {

// Gtk::Application shell: loads Prefs, opens the roster (or jumps
// straight into the saved agent's voice session, as MainActivity does).
class HiveVoiceApp : public Gtk::Application {
 public:
    static Glib::RefPtr<HiveVoiceApp> create();

 protected:
    HiveVoiceApp();
    void on_activate() override;

 private:
    void showSettings();
    void openRoster();
    void openSavedSession();

    Prefs prefs_;
    std::unique_ptr<class DebugShell> shell_;    // unix-socket test harness
    std::unique_ptr<class GlobalShortcuts> keys_;  // portal global key
};

}  // namespace hv
