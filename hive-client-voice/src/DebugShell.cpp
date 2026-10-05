#include "DebugShell.h"

#include "RosterView.h"
#include "SphereArea.h"
#include "VoiceView.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hv {

namespace {
RosterView* g_roster = nullptr;
VoiceView* g_voice = nullptr;

std::string socketPath() {
    if (const char* p = std::getenv("HIVE_VOICE_CTL")) return p;
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"))
        return std::string(x) + "/hive-voice-ctl.sock";
    return "/tmp/hive-voice-ctl-" + std::to_string(::getuid()) + ".sock";
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

const char* kHelp =
    "commands: help | status | windows | shot <path.png> [title] | agents "
    "| open <agent> | say <text> | phase <idle|listening|thinking|speaking|"
    "processing> | level <0..1> | glinfo\n";
}  // namespace

void DebugShell::registerRoster(RosterView* w) { g_roster = w; }
void DebugShell::registerVoice(VoiceView* w) { g_voice = w; }

DebugShell::DebugShell(Gtk::Application& app) : app_(app), path_(socketPath()) {
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) return;
    ::unlink(path_.c_str());  // stale socket from a previous run
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
            0 ||
        ::listen(listenFd_, 4) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }
    running_ = true;
    thread_ = std::thread([this] { serveLoop(); });
    std::fprintf(stderr, "hive-voice: debug shell on %s\n", path_.c_str());
}

DebugShell::~DebugShell() {
    running_ = false;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
    }
    if (thread_.joinable()) thread_.join();
    ::unlink(path_.c_str());
}

void DebugShell::serveLoop() {
    while (running_) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) break;  // shutdown() woke us
        std::string cmd;
        char buf[512];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
            cmd.append(buf, static_cast<std::size_t>(n));
            if (cmd.find('\n') != std::string::npos) break;
        }
        if (const auto nl = cmd.find('\n'); nl != std::string::npos)
            cmd.resize(nl);
        while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == ' '))
            cmd.pop_back();

        // GTK work must happen on the main thread; the worker blocks on a
        // condvar until the idle callback delivers the response.
        std::string result;
        bool done = false;
        std::mutex m;
        std::condition_variable cv;
        Glib::signal_idle().connect_once(
            [this, cmd, &result, &done, &m, &cv] {
                result = handle(cmd);
                {
                    std::lock_guard<std::mutex> lk(m);
                    done = true;
                }
                cv.notify_one();
            });
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&done] { return done; });
        }
        if (!result.empty())
            (void)::write(fd, result.data(), result.size());
        ::close(fd);
    }
}

std::string DebugShell::shot(const std::string& path,
                             const std::string& titleSub) {
    if (path.empty()) return "error: shot needs a path\n";
    Gtk::Window* target = nullptr;
    for (auto* w : app_.get_windows()) {
        if (!w->get_visible()) continue;
        if (titleSub.empty()) {
            target = w;  // last visible window = frontmost stacking
        } else if (w->get_title().lowercase().find(
                       Glib::ustring(titleSub).lowercase()) !=
                   Glib::ustring::npos) {
            target = w;
            break;
        }
    }
    if (!target) return "error: no matching window\n";
    // Raw C API — the gtkmm4 wrappers for snapshot/renderer are partial.
    GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(target->gobj()));
    if (!renderer) return "error: no gsk renderer\n";
    const int w = target->get_width(), h = target->get_height();
    // GtkWidgetPaintable is the public way to snapshot a widget subtree.
    GdkPaintable* paintable =
        gtk_widget_paintable_new(GTK_WIDGET(target->gobj()));
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, w, h);
    GskRenderNode* node = gtk_snapshot_to_node(snapshot);
    g_object_unref(snapshot);
    g_object_unref(paintable);
    if (!node) return "error: empty render node\n";
    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0, 0, static_cast<float>(w),
                       static_cast<float>(h));
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
    gsk_render_node_unref(node);
    if (!texture) return "error: render_texture failed\n";
    const gboolean saved = gdk_texture_save_to_png(texture, path.c_str());
    g_object_unref(texture);
    if (!saved) return "error: save_to_png failed\n";
    return "ok " + path + " (" + std::to_string(w) + "x" + std::to_string(h) +
           ", " + target->get_title() + ")\n";
}

std::string DebugShell::handle(const std::string& line) {
    if (line.empty() || line == "help") return kHelp;

    if (line == "windows") {
        std::string out;
        for (auto* w : app_.get_windows()) {
            out += "\"" + w->get_title() + "\" " +
                   std::to_string(w->get_width()) + "x" +
                   std::to_string(w->get_height()) +
                   (w->get_visible() ? " visible" : " hidden") + "\n";
        }
        return out.empty() ? "no windows\n" : out;
    }

    if (line == "status") {
        std::string out = "{\"windows\":[";
        bool first = true;
        for (auto* w : app_.get_windows()) {
            if (!first) out += ",";
            first = false;
            out += "{\"title\":\"" + jsonEscape(w->get_title()) +
                   "\",\"visible\":" + (w->get_visible() ? "true" : "false") +
                   "}";
        }
        out += "],\"voice\":";
        if (g_voice) {
            out += "{\"agent\":\"" + jsonEscape(g_voice->debugAgentName()) +
                   "\",\"phase\":\"" + g_voice->debugPhaseName() +
                   "\",\"status\":\"" + jsonEscape(g_voice->debugStatusText()) +
                   "\"}";
        } else {
            out += "null";
        }
        out += ",\"gl\":\"" + jsonEscape(SphereArea::glInfo()) + "\"}\n";
        return out;
    }

    if (line.rfind("glshot ", 0) == 0) {
        if (!g_voice) return "error: no voice session open\n";
        g_voice->debugSphereShot(line.substr(7));
        return "ok queued — file appears after the next frame\n";
    }

    if (line == "sphere") {
        if (!g_voice) return "error: no voice session open\n";
        return g_voice->debugSphereStats() + "\n";
    }

    if (line == "glinfo")
        return SphereArea::glInfo().empty() ? "GL not realized yet\n"
                                            : SphereArea::glInfo() + "\n";

    if (line.rfind("picker ", 0) == 0) {
        if (!g_roster) return "error: no roster\n";
        const std::string name = line.substr(7);
        if (g_roster->debugOpenVoicePicker(name)) return "ok\n";
        return "error: no such agent (see `agents`)\n";
    }

    if (line == "agents") {
        if (!g_roster) return "error: no roster\n";
        std::string out;
        for (const auto& a : g_roster->debugAgents())
            out += a.name + " [" + a.role + "] online=" +
                   (a.online ? "1" : "0") + " busy=" + (a.busy ? "1" : "0") +
                   " state=" + a.state + "\n";
        return out.empty() ? "no agents polled yet\n" : out;
    }

    if (line.rfind("shot ", 0) == 0) {
        std::string rest = line.substr(5);
        std::string path, title;
        if (const auto sp = rest.find(' '); sp != std::string::npos) {
            path = rest.substr(0, sp);
            title = rest.substr(sp + 1);
        } else {
            path = rest;
        }
        return shot(path, title);
    }

    if (line.rfind("open ", 0) == 0) {
        if (!g_roster) return "error: no roster\n";
        const std::string name = line.substr(5);
        return g_roster->debugOpenVoice(name)
                   ? "ok\n"
                   : "error: no online agent matching '" + name + "'\n";
    }

    if (line.rfind("say ", 0) == 0) {
        if (!g_voice) return "error: no voice session open\n";
        g_voice->debugSay(line.substr(4));
        return "ok\n";
    }

    if (line.rfind("phase ", 0) == 0) {
        if (!g_voice) return "error: no voice session open\n";
        const std::string p = line.substr(6);
        SphereState s;
        if (p == "idle") s = SphereState::Idle;
        else if (p == "listening") s = SphereState::Listening;
        else if (p == "thinking") s = SphereState::Thinking;
        else if (p == "speaking") s = SphereState::Speaking;
        else if (p == "processing") s = SphereState::Processing;
        else return "error: unknown phase '" + p + "'\n";
        g_voice->debugSetSphere(s, 0.6f);
        return "ok\n";
    }

    if (line.rfind("level ", 0) == 0) {
        if (!g_voice) return "error: no voice session open\n";
        try {
            g_voice->debugSetSphereLevel(std::stof(line.substr(6)));
            return "ok\n";
        } catch (...) {
            return "error: bad level\n";
        }
    }

    return "error: unknown command — " + std::string(kHelp);
}

}  // namespace hv
