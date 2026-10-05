#pragma once

#include "Prefs.h"

#include <gtkmm.h>

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
};

}  // namespace hv
