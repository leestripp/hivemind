#include "HiveApplication.h"
#include "HiveWindow.h"

namespace hive {

HiveApplication::HiveApplication()
    : Gtk::Application("com.hivemind.client",
                       Gio::Application::Flags::DEFAULT_FLAGS) {}

Glib::RefPtr<HiveApplication> HiveApplication::create() {
    return Glib::RefPtr<HiveApplication>(new HiveApplication());
}

void HiveApplication::on_activate() {
    auto* window = new HiveWindow();
    add_window(*window);
    window->present();
}

} // namespace hive
