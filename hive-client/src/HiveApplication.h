#pragma once

#include <gtkmm.h>

namespace hive {

// HiveMind 2.0 native client application shell (GTK4/gtkmm4).
class HiveApplication : public Gtk::Application {
public:
    static Glib::RefPtr<HiveApplication> create();

protected:
    HiveApplication();
    void on_activate() override;
};

} // namespace hive
