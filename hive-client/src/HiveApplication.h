#pragma once

#include <gtkmm.h>

#include <memory>

namespace hive {

class UiApi;

// HiveMind 2.0 native client application shell (GTK4/gtkmm4).
class HiveApplication : public Gtk::Application {
public:
    static Glib::RefPtr<HiveApplication> create();
    ~HiveApplication() override;

protected:
    HiveApplication();
    void on_activate() override;
    void on_shutdown() override;

private:
    std::unique_ptr<UiApi> ui_api_;   // localhost test API (Phase 11.5)
};

} // namespace hive
