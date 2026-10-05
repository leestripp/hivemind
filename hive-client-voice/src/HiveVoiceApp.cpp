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
    auto* win = Gtk::make_managed<RosterWindow>(prefs_);
    win->set_application(Glib::RefPtr<HiveVoiceApp>(this));
    add_window(*win);
    win->present();
}

void HiveVoiceApp::showSettings() {}

void HiveVoiceApp::openRoster() {}

void HiveVoiceApp::openSavedSession() {}

}  // namespace hv
