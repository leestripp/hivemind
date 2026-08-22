#pragma once

#include <gtkmm.h>

namespace hive {

// Main window: roster sidebar + chat placeholder (Phase 4 builds this out).
class HiveWindow : public Gtk::Window {
public:
    HiveWindow();
    ~HiveWindow() override = default;

private:
    Gtk::Box m_box{Gtk::Orientation::VERTICAL};
    Gtk::HeaderBar m_header;
    Gtk::Label m_placeholder;
};

} // namespace hive
