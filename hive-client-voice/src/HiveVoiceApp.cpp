#include "HiveVoiceApp.h"

#include "ApiClient.h"
#include "DebugShell.h"
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
    // App-wide theme: the hive-android palette (data/style.css), priority
    // above the theme so the dark shell wins everywhere.
    auto css = Gtk::CssProvider::create();
    css->load_from_resource("/org/hivemind/voice/style.css");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), css,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void HiveVoiceApp::on_activate() {
    if (!shell_) shell_ = std::make_unique<DebugShell>(*this);
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
