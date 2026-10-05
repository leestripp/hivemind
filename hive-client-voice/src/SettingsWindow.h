#pragma once

#include <gtkmm.h>

class Prefs;

namespace hv {

// Settings: hub base URL, API key, audio thresholds, state announce.
// Mirrors SettingsActivity.kt — same fields, persisted via Prefs.
class SettingsWindow : public Gtk::Window {
 public:
    explicit SettingsWindow(Prefs& prefs);

 private:
    void saveAndClose();

    Prefs& prefs_;
    Gtk::Entry* base_ = nullptr;
    Gtk::Entry* key_ = nullptr;
    Gtk::Switch* announce_ = nullptr;
};

}  // namespace hv
