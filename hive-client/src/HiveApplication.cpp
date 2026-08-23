#include "HiveApplication.h"
#include <gio/gio.h>
extern "C" { GResource* hivemind_get_resource(void); }
#include "HiveWindow.h"
#include "UiApi.h"

namespace hive {

HiveApplication::HiveApplication()
    : Gtk::Application("com.hivemind.client",
                       Gio::Application::Flags::DEFAULT_FLAGS) {}

Glib::RefPtr<HiveApplication> HiveApplication::create() {
    return Glib::RefPtr<HiveApplication>(new HiveApplication());
}

HiveApplication::~HiveApplication() = default;

void HiveApplication::on_activate() {
    g_resources_register(hivemind_get_resource());
    auto* window = new HiveWindow();
    add_window(*window);
    window->present();
    // Localhost REST test surface (Phase 11.5) — started after the window
    // exists so handlers can marshal onto the GTK main loop.
    ui_api_ = std::make_unique<UiApi>(*window);
    ui_api_->start();
}

void HiveApplication::on_shutdown() {
    if (ui_api_) { ui_api_->stop(); ui_api_.reset(); }
    Gtk::Application::on_shutdown();
}

} // namespace hive
