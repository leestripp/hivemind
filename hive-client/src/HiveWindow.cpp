#include "HiveWindow.h"

namespace hive {

HiveWindow::HiveWindow() {
    set_title("HiveMind");
    set_default_size(1024, 720);

    m_placeholder.set_text(
        "🐝 HiveMind 2.0 — hive-client\n\n"
        "GTK4/gtkmm4 shell is up.\n"
        "Roster, chat threads and approvals land in Phase 4.");
    m_placeholder.set_justify(Gtk::Justification::CENTER);
    m_placeholder.set_vexpand(true);

    m_box.append(m_header);
    m_box.append(m_placeholder);
    set_child(m_box);
}

} // namespace hive
