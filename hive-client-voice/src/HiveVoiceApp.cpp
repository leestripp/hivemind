#include "HiveVoiceApp.h"

#include "ApiClient.h"
#include "RosterWindow.h"
#include "SettingsWindow.h"
#include "VoicePickerWindow.h"
#include "VoiceWindow.h"

#include <thread>

namespace hv {

Glib::RefPtr<HiveVoiceApp> HiveVoiceApp::create() {
    return Glib::RefPtr<HiveVoiceApp>(new HiveVoiceApp());
}

HiveVoiceApp::HiveVoiceApp() : Gtk::Application("org.hivemind.voice") {
    prefs_.load();
}

void HiveVoiceApp::on_activate() {
    // add_window() associates the window with the application — do NOT
    // wrap `this` in a RefPtr (it unrefs on destruction and frees the
    // app mid-activation).
    auto* win = Gtk::make_managed<RosterWindow>(prefs_);
    add_window(*win);
    win->present();
}

void HiveVoiceApp::showSettings() {}

void HiveVoiceApp::openRoster() {}

void HiveVoiceApp::openSavedSession() {}

}  // namespace hv
