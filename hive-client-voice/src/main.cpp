#include "HiveVoiceApp.h"

#include <glib-unix.h>

namespace {
// SIGTERM/SIGINT: close windows through the normal close-request path
// so MainWindow::on_close_request() can persist its geometry — a raw
// default signal kill skips the handler and the size is lost.
gboolean onTermSignal(void* data) {
    auto* app = static_cast<Gtk::Application*>(data);
    for (auto* w : app->get_windows()) w->close();
    return G_SOURCE_REMOVE;
}
}  // namespace

int main(int argc, char** argv) {
    auto app = hv::HiveVoiceApp::create();
    g_unix_signal_add(SIGTERM, onTermSignal, app->gobj());
    g_unix_signal_add(SIGINT, onTermSignal, app->gobj());
    return app->run(argc, argv);
}
