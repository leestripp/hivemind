#include "HiveWindow.h"

#include <thread>

#ifdef HIVEMIND_WITH_WEBKIT
#include <webkit/webkit.h>
#endif
#include <giomm/memoryinputstream.h>
#include <giomm/resource.h>
#include <gdkmm/pixbuf.h>
#include <future>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

using nlohmann::json;

namespace hive {

namespace {

const char* kCss = R"(
.hv-sidebar { background: @shade; }
.hv-sidebar list { background: transparent; }
.hv-sidebar row { border-radius: 8px; }
.hv-sidebar row:selected {
  background: alpha(@accent_color, 0.28);
  box-shadow: inset 3px 0 0 @accent_color;
}
.hv-msg-user   { background: alpha(@accent_color, 0.15); border-radius: 10px;
                 padding: 8px; margin: 4px 48px 4px 12px; }
.hv-msg-agent  { background: alpha(@window_fg_color, 0.07); border-radius: 10px;
                 padding: 8px; margin: 4px 12px 4px 48px; }
.hv-msg-system { color: alpha(@window_fg_color, 0.6); font-style: italic;
                 margin: 4px 12px; }
.hv-sender { font-weight: 600; font-size: 0.85em;
             color: alpha(@window_fg_color, 0.65); }
.hv-section { font-weight: 700; margin: 8px 8px 2px 8px; }
.hv-time   { color: alpha(@window_fg_color, 0.45); font-size: 0.75em; }
.hv-code   { font-family: monospace; background: alpha(@window_fg_color, 0.08);
             border-radius: 8px; padding: 8px 10px; margin: 4px 0; }
.hv-tool   { color: alpha(@window_fg_color, 0.45); font-family: monospace;
             font-size: 0.8em; margin: 2px 12px; }
.hv-avatar { border-radius: 50%; min-width: 32px; min-height: 32px;
             font-weight: 700; color: white; margin: 6px 8px 0 4px; }
.hv-avatar-0 { background: #3584e4; }
.hv-avatar-1 { background: #33d17a; }
.hv-avatar-2 { background: #f6d32d; color: #3d3846; }
.hv-avatar-3 { background: #ff7800; }
.hv-avatar-4 { background: #9141ac; }
.hv-avatar-5 { background: #e01b24; }
.hv-avatar-user { background: @accent_color; }
.hv-name { font-weight: 700; font-size: 1.0em; }
.hv-name-0 { color: #3584e4; }
.hv-name-1 { color: #2ec27e; }
.hv-name-2 { color: #c19100; }
.hv-name-3 { color: #ff7800; }
.hv-name-4 { color: #9141ac; }
.hv-name-5 { color: #e01b24; }
.hv-name-user { color: @accent_color; }

/* Avatar picker grid (New Agent dialog) */
.avatar-pick { padding: 4px; border-radius: 10px; }
.avatar-pick:checked {
  background: alpha(@accent_color, 0.18);
  box-shadow: 0 0 0 2px @accent_color;
}
.avatar-pick-auto { padding: 8px 10px; font-size: 12px; opacity: 0.8; }

/* Context menus (right-click roster/threads) */
.hv-menu-item { padding: 6px 12px; min-width: 140px; }
.hv-menu-item label { margin: 0; }
.hv-prompt-frame { border: 1px solid alpha(@window_fg_color, 0.2);
                   border-radius: 8px; }
.hv-prompt-frame textview { padding: 6px; }
.error { color: @error_color; }
.hv-unread-dot { color: @accent_color; font-weight: bold; }
.hv-state-tag { font-size: 10px; padding: 1px 8px; border-radius: 8px;
                background: alpha(@accent_color, 0.15); color: @accent_color; }
)";

std::string agent_name_of(const json& agents, const std::string& id) {
    for (auto& a : agents)
        if (a.value("id", "") == id) return a.value("name", id);
    return id;
}


// ── chat message rendering ──────────────────────────────────────────────────
// Agent message bodies arrive as daedalus turn transcripts: natural language
// interleaved with "[tool: name args…]" call lines and "  → {json}" result
// lines. Strip the transcript plumbing for display; the raw body can still be
// dumped to a debug log (HIVEMIND_DEBUG=1 → ~/.local/share/hivemind/client-raw.log).

bool starts_with(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}

// Remove "[tool: …]" call lines and "→ {…}" result lines, collapse the blank
// runs they leave behind.
std::string strip_tool_transcript(const std::string& body) {
    std::string out;
    std::istringstream in(body);
    std::string line;
    int blank_run = 0;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        const auto ws = trimmed.find_first_not_of(" \t");
        if (ws != std::string::npos) trimmed = trimmed.substr(ws);
        if (starts_with(trimmed, "[tool:")) continue;
        if (starts_with(trimmed, "\xe2\x86\x92")) continue;   // "→" result line
        if (trimmed.empty()) { ++blank_run; if (blank_run > 1) continue; }
        else blank_run = 0;
        out += line;
        out += '\n';
    }
    // trim trailing blank space
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// Split text on ``` fences into prose / code segments.
struct Seg { bool code; std::string text; };

std::vector<Seg> split_segments(const std::string& text) {
    std::vector<Seg> segs;
    std::istringstream in(text);
    std::string line, buf;
    bool code = false;
    auto flush = [&] {
        while (!buf.empty() && buf.back() == '\n') buf.pop_back();
        if (!buf.empty()) segs.push_back({code, buf});
        buf.clear();
    };
    while (std::getline(in, line)) {
        if (starts_with(line, "```")) {
            flush();
            code = !code;
            continue;
        }
        buf += line;
        buf += '\n';
    }
    flush();
    return segs;
}

// Minimal inline markdown → Pango markup: **bold**, *italic*, `code`.
// Line-level: # / ## / ### headers, "- " bullets.
std::string inline_markup(const std::string& line) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        if (starts_with(line.substr(i), "**")) {
            auto j = line.find("**", i + 2);
            if (j != std::string::npos) {
                out += "<b>" + Glib::Markup::escape_text(line.substr(i + 2, j - i - 2)) + "</b>";
                i = j + 2;
                continue;
            }
        }
        if (line[i] == '`') {
            auto j = line.find('`', i + 1);
            if (j != std::string::npos) {
                out += "<span font_family='monospace' background='#88888830'>"
                     + Glib::Markup::escape_text(line.substr(i + 1, j - i - 1)) + "</span>";
                i = j + 1;
                continue;
            }
        }
        if (line[i] == '*' && i + 1 < line.size() && line[i + 1] != '*' && line[i + 1] != ' ') {
            auto j = line.find('*', i + 1);
            if (j != std::string::npos) {
                out += "<i>" + Glib::Markup::escape_text(line.substr(i + 1, j - i - 1)) + "</i>";
                i = j + 1;
                continue;
            }
        }
        out += Glib::Markup::escape_text(std::string(1, line[i]));
        ++i;
    }
    return out;
}

std::string prose_to_markup(const std::string& text) {
    std::string out;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!first) out += '\n';
        first = false;
        if (starts_with(line, "### ")) {
            out += "<span size='large' weight='bold'>" + inline_markup(line.substr(4)) + "</span>";
        } else if (starts_with(line, "## ")) {
            out += "<span size='large' weight='bold'>" + inline_markup(line.substr(3)) + "</span>";
        } else if (starts_with(line, "# ")) {
            out += "<span size='x-large' weight='bold'>" + inline_markup(line.substr(2)) + "</span>";
        } else if (starts_with(line, "- ")) {
            out += "\xe2\x80\xa2 " + inline_markup(line.substr(2));   // "• "
        } else {
            out += inline_markup(line);
        }
    }
    return out;
}

// "2026-08-23T06:09:43.6Z" → "06:09" (today) or "Aug 22, 22:14"
std::string format_msg_time(const std::string& iso) {
    if (iso.size() < 16) return {};
    std::string hm = iso.substr(11, 5);
    std::string date = iso.substr(0, 10);
    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    char today[11];
    std::strftime(today, sizeof today, "%Y-%m-%d", lt);
    if (date == today) return hm;
    // older: short date + time
    std::tm tm{};
    strptime(date.c_str(), "%Y-%m-%d", &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%b %d", &tm);
    return std::string(buf) + ", " + hm;
}

// ── GResource SVG icons ───────────────────────────────────────────────────
// Icons ship as currentColor-stroked SVGs bundled into the binary. GTK's
// pixbuf loader doesn't resolve currentColor from CSS, so we substitute the
// theme foreground explicitly — this is what makes icons follow dark/light
// mode. Avatars carry their own identity colors and load verbatim.
extern "C" { GResource* hivemind_get_resource(void); }

static std::string resource_text(const std::string& path) {
    auto bytes = Gio::Resource::lookup_data_global(path);
    if (!bytes) return {};
    gsize sz = 0;
    auto* data = static_cast<const char*>(bytes->get_data(sz));
    return {data, sz};
}

static Glib::RefPtr<Gdk::Pixbuf> svg_pixbuf(const std::string& svg, int size) {
    auto stream = Gio::MemoryInputStream::create();
    stream->add_bytes(Glib::Bytes::create(svg.data(), svg.size()));
    return Gdk::Pixbuf::create_from_stream_at_scale(stream, size, size, true);
}

static Glib::RefPtr<Gdk::Pixbuf> themed_icon_pixbuf(const std::string& name,
                                                    int size, bool dark) {
    std::string svg = resource_text("/com/hivemind/client/icons/" + name + ".svg");
    const std::string fg = dark ? "#eeeeec" : "#241f31";
    std::string::size_type pos = 0;
    while ((pos = svg.find("currentColor", pos)) != std::string::npos) {
        svg.replace(pos, 12, fg);
    }
    return svg_pixbuf(svg, size);
}

static Glib::RefPtr<Gdk::Pixbuf> avatar_pixbuf(const std::string& name, int size) {
    return svg_pixbuf(resource_text("/com/hivemind/client/avatars/" + name + ".svg"),
                      size);
}

// Stable per-agent palette index — avatar and name label share it so an
// agent is identifiable by color across a busy multi-agent thread.
int palette_index(const std::string& name) {
    return (int)(std::hash<std::string>{}(name) % 6);
}

// Hexagon hive-bot avatar from the bundled SVG set. palette_index keeps an
// agent's bot consistent across the roster and every bubble; the user gets
// the person-silhouette hex.
//
// HiDPI (Wayland/4K): GTK works in logical pixels and the compositor scales;
// for raster content to stay crisp the pixbuf must carry scale × logical
// pixels. Our avatars are SVG, so supersampling is free — render at 2× the
// logical size and let Gtk::Image downscale to the logical allocation. On a
// 2× surface the full data is used (crisp); on 1× it just looks smoother.
constexpr int AVATAR_LOGICAL = 64;    // bubble avatar edge, logical px
constexpr int AVATAR_SUPERSAMPLE = 2; // HiDPI oversampling factor

Gtk::Widget* make_avatar(const std::string& name, bool is_user,
                         int logical = AVATAR_LOGICAL,
                         const std::string& avatar = "") {
    // Chosen avatar wins; "" = auto palette hash of the agent's name.
    const std::string res = is_user
        ? "user"
        : (avatar.empty() ? "avatar-" + std::to_string(palette_index(name))
                          : avatar);
    auto* img = Gtk::make_managed<Gtk::Image>();
    img->set(avatar_pixbuf(res, logical * AVATAR_SUPERSAMPLE));
    img->set_pixel_size(logical);
    img->set_valign(Gtk::Align::START);
    return img;
}

} // namespace

// Install a HeaderBar as a dialog's titlebar (CSD) so the compositor doesn't
// draw a second server-side title bar above it — same fix as the main
// window's set_titlebar(). Close button only, matching GNOME dialog style.
static void install_dialog_titlebar(Gtk::Dialog& dialog,
                                    const std::string& title) {
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_decoration_layout(":close");   // dialogs: no min/max
    hb->set_show_title_buttons(true);
    auto* label = Gtk::make_managed<Gtk::Label>(title);
    label->add_css_class("title");
    hb->set_title_widget(*label);
    dialog.set_titlebar(*hb);
}

void HiveWindow::setup_icons() {
    // Build the persistent widget structure exactly once. Retinting on theme
    // change only swaps pixbufs into these same Gtk::Image widgets —
    // reparenting a managed member widget on retint let GTK destroy
    // approvals_label_ and crashed poll_approvals (2026-08-23).
    auto slot = [this](Gtk::Button& btn, const std::string& icon,
                       const std::string& tip) {
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set(themed_icon_pixbuf(icon, 16, dark_mode_));
        btn.set_child(*img);
        btn.set_tooltip_text(tip);
        icon_slots_.push_back({&btn, img, icon});
    };
    slot(new_agent_btn_,     "agent-add",  "Create a new agent");
    slot(new_thread_btn_,    "thread-add", "Start a new thread");
    slot(tasks_btn_,         "task-add",   "Schedule a task (routine)");
    slot(connect_btn_,       "connect",    "Connect to a hive-server");
    slot(stop_btn_,          "stop",       "Cancel the running turn in this thread");
    slot(delete_thread_btn_, "trash",      "Delete the selected thread");
    slot(send_btn_,          "send",       "Send message");

    // Theme toggle: single image, swaps sun/moon (shows the mode you'd get).
    theme_icon_ = Gtk::make_managed<Gtk::Image>();
    theme_btn_.set_child(*theme_icon_);

    // Approvals: icon + live count label, stable structure.
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    approvals_icon_.set(themed_icon_pixbuf("approvals", 16, dark_mode_));
    approvals_label_.set_label("Approvals");
    box->append(approvals_icon_);
    box->append(approvals_label_);
    approvals_btn_.set_child(*box);
    approvals_btn_.set_tooltip_text("Pending approvals");
}

void HiveWindow::refresh_themed_icons() {
    for (auto& s : icon_slots_)
        s.img->set(themed_icon_pixbuf(s.icon, 16, dark_mode_));
    approvals_icon_.set(themed_icon_pixbuf("approvals", 16, dark_mode_));
    theme_icon_->set(themed_icon_pixbuf(
        dark_mode_ ? "theme-light" : "theme-dark", 16, dark_mode_));
    theme_btn_.set_tooltip_text(dark_mode_ ? "Switch to light mode"
                                           : "Switch to dark mode");
}

nlohmann::json HiveWindow::ui_open_dialog(const std::string& name) {
    return invoke_on_ui([this, name] {
        if (name == "new-thread")      on_new_thread();
        else if (name == "new-agent")  on_new_agent();
        else if (name == "approvals")  on_approvals();
        else return json{{"error", "unknown dialog"}};
        return json{{"opened", name}};
    });
}

nlohmann::json HiveWindow::ui_toggle_theme() {
    return invoke_on_ui([this] {
        on_theme_toggle();
        return json{{"dark_mode", dark_mode_}};
    });
}

void HiveWindow::on_theme_toggle() {
    dark_mode_ = !dark_mode_;
    apply_theme();
    save_config();
}

void HiveWindow::apply_theme() {
    Gtk::Settings::get_default()
        ->property_gtk_application_prefer_dark_theme()
        .set_value(dark_mode_);
    refresh_themed_icons();
}

HiveWindow::HiveWindow() {
    set_title("HiveMind");
    set_default_size(1100, 760);

    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(),
        [] {
            auto p = Gtk::CssProvider::create();
            p->load_from_string(kCss);
            return p;
        }(),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // ── header ────────────────────────────────────────────────────────────
    new_agent_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_new_agent));
    new_thread_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_new_thread));
    tasks_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_new_task));
    approvals_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_approvals));
    connect_btn_.signal_clicked().connect(
        [this] { maybe_show_connect_dialog(); });
    theme_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_theme_toggle));
    stop_btn_.signal_clicked().connect([this] { on_stop(); });
    stop_btn_.set_sensitive(false);   // enabled while a turn is running
    stop_btn_.add_css_class("destructive-action");
    learn_btn_.set_label("🎓 Learn");
    learn_btn_.set_tooltip_text(
        "Learn-by-demonstration: the agent distills this thread's workflow "
        "into a scheduled routine (save_routine)");
    learn_btn_.signal_clicked().connect([this] { on_learn_routine(); });
    learn_btn_.set_sensitive(false);   // enabled with a thread selection
    delete_thread_btn_.signal_clicked().connect([this] { on_delete_thread(); });
    delete_thread_btn_.set_sensitive(false);  // enabled with a selection
    header_.pack_start(new_agent_btn_);
    header_.pack_start(new_thread_btn_);
    header_.pack_start(tasks_btn_);
    header_.pack_end(connect_btn_);
    header_.pack_end(theme_btn_);
    header_.pack_end(delete_thread_btn_);
    header_.pack_end(stop_btn_);
    header_.pack_end(learn_btn_);
    header_.pack_end(approvals_btn_);
    // CSD: install our HeaderBar AS the window titlebar so the compositor
    // doesn't draw a second (server-side) title bar above it.
    set_titlebar(header_);

    // ── sidebar ───────────────────────────────────────────────────────────
    roster_label_.add_css_class("hv-section");
    roster_label_.set_halign(Gtk::Align::START);
    threads_label_.add_css_class("hv-section");
    threads_label_.set_halign(Gtk::Align::START);

    roster_scroll_.set_child(roster_list_);
    roster_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    roster_scroll_.set_size_request(240, 180);
    roster_scroll_.set_vexpand(false);
    threads_scroll_.set_child(threads_list_);
    threads_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    threads_scroll_.set_vexpand(true);

    roster_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    threads_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    roster_list_.signal_row_activated().connect(
        [this](Gtk::ListBoxRow*) { on_agent_selected(); });
    threads_list_.signal_row_selected().connect(
        [this](Gtk::ListBoxRow*) { on_thread_selected(); });

    sidebar_.add_css_class("hv-sidebar");
    sidebar_.append(roster_label_);
    sidebar_.append(roster_scroll_);
    sidebar_.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    sidebar_.append(threads_label_);
    sidebar_.append(threads_scroll_);

    // ── chat area ─────────────────────────────────────────────────────────
    status_label_.set_halign(Gtk::Align::START);
    status_label_.set_margin(6);
    status_label_.set_selectable(true);   // Phase 19: error ids are copyable
    chat_scroll_.set_child(messages_box_);
    chat_scroll_.set_vexpand(true);
    entry_.set_hexpand(true);
    entry_.set_placeholder_text("Message the team…  (Enter to send)");
    entry_.signal_activate().connect([this] { on_send(); });
    send_btn_.signal_clicked().connect([this] { on_send(); });
    send_btn_.add_css_class("suggested-action");
    // Phase 18: 📎 attach files (uploaded into the shared workspace).
    attach_btn_.set_label("📎");
    attach_btn_.set_tooltip_text(
        "Attach a file — saved into the shared workspace where agents can read it");
    attach_btn_.signal_clicked().connect([this] { on_attach(); });
    attach_clear_btn_.set_label("✕");
    attach_clear_btn_.set_tooltip_text("Drop pending attachments");
    attach_clear_btn_.add_css_class("flat");
    attach_clear_btn_.signal_clicked().connect([this] {
        pending_attachments_ = nlohmann::json::array();
        update_attach_row();
    });
    attach_label_.set_halign(Gtk::Align::START);
    attach_label_.set_hexpand(true);
    attach_label_.set_ellipsize(Pango::EllipsizeMode::END);
    attach_row_.set_margin_start(6);
    attach_row_.append(attach_label_);
    attach_row_.append(attach_clear_btn_);
    attach_row_.set_visible(false);
    composer_.set_margin(6);
    composer_.set_spacing(6);
    composer_.append(attach_btn_);
    composer_.append(entry_);
    composer_.append(send_btn_);

    thread_title_.add_css_class("heading");
    thread_title_.set_halign(Gtk::Align::START);
    thread_title_.set_margin_start(6);
    thread_title_.set_ellipsize(Pango::EllipsizeMode::END);
    chat_box_.append(thread_title_);
    chat_box_.append(status_label_);
    chat_box_.append(chat_scroll_);
    chat_box_.append(attach_row_);
    chat_box_.append(composer_);

    paned_.set_start_child(sidebar_);
    paned_.set_end_child(chat_box_);
    paned_.set_position(260);
    paned_.set_vexpand(true);
    root_.append(paned_);
    set_child(root_);

    // ── async plumbing ────────────────────────────────────────────────────
    dispatcher_.connect([this] {
        std::vector<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            jobs.swap(done_queue_);
        }
        for (auto& j : jobs) j();
    });

    // 1.5 s poll: chat stream + approvals badge.
    poll_timer_ = Glib::signal_timeout().connect([this] {
        // While the connect dialog is up, every request would 401 and (pre-fix)
        // spawn yet another modal dialog. Skip polling until we're connected.
        if (connect_dialog_ || api_.api_key().empty()) return alive_;
        poll_chat();
        poll_approvals();
        poll_roster();
        return alive_;
    }, 1500);

    load_config();
    setup_icons();   // persistent icon structure, once
    apply_theme();   // dark/light + initial pixbuf tint
    set_status("Connecting to hive-server…");
    if (api_.api_key().empty())
        maybe_show_connect_dialog();
    else
        refresh_all();
}

HiveWindow::~HiveWindow() {
    alive_ = false;
    poll_timer_.disconnect();
}

// ── connection config (Phase 9) ─────────────────────────────────────────────

void HiveWindow::load_config() {
    namespace fs = std::filesystem;
    fs::path dir = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                 / ".config" / "hivemind";
    std::ifstream f{dir / "config.json"};
    if (!f) return;
    try {
        auto j = json::parse(f);
        server_url_ = j.value("server", server_url_);
        api_.set_base_url(server_url_);
        api_.set_api_key(j.value("api_key", ""));
        dark_mode_ = j.value("dark_mode", false);
    } catch (...) {}
}

void HiveWindow::save_config() {
    namespace fs = std::filesystem;
    fs::path dir = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                 / ".config" / "hivemind";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / "config.json";
    std::ofstream f{file, std::ios::trunc};
    f << json{{"server", server_url_}, {"api_key", api_.api_key()},
              {"dark_mode", dark_mode_}}.dump(2);
    f.close();
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
}

void HiveWindow::maybe_show_connect_dialog() {
    if (!alive_) return;
    // gtkmm4 dialogs are NON-BLOCKING: present() returns immediately and the
    // response arrives later via signal_response(). Without this guard every
    // 401 from the poll timer stacked another modal dialog on screen.
    if (connect_dialog_) {
        connect_dialog_->present();   // raise the existing one, don't stack
        return;
    }

    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    connect_dialog_ = dialog;
    install_dialog_titlebar(*dialog, "Connect to server");
    dialog->signal_destroy().connect([this] { connect_dialog_ = nullptr; });

    dialog->set_title("Connect to hive-server");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    auto* cancel_btn = dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    auto* ok_btn = dialog->add_button("Connect", Gtk::ResponseType::OK);
    ok_btn->add_css_class("suggested-action");
    dialog->set_default_response(Gtk::ResponseType::OK);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_margin(12);
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);
    auto* server = Gtk::make_managed<Gtk::Entry>();
    server->set_text(server_url_);
    auto* key = Gtk::make_managed<Gtk::Entry>();
    key->set_text(api_.api_key());
    key->set_placeholder_text("API key (hm_…)");
    key->set_visibility(false);
    auto* sl = Gtk::make_managed<Gtk::Label>("Server");
    sl->set_halign(Gtk::Align::END);
    auto* kl = Gtk::make_managed<Gtk::Label>("API key");
    kl->set_halign(Gtk::Align::END);
    auto* error = Gtk::make_managed<Gtk::Label>();
    error->add_css_class("error");
    error->set_visible(false);
    error->set_wrap(true);
    grid->attach(*sl, 0, 0);
    grid->attach(*server, 1, 0);
    grid->attach(*kl, 0, 1);
    grid->attach(*key, 1, 1);
    grid->attach(*error, 0, 2, 2);
    server->set_hexpand(true);
    key->set_hexpand(true);
    dialog->get_content_area()->append(*grid);

    dialog->signal_response().connect(
        [this, dialog, server, key, error, cancel_btn, ok_btn](int response) {
            // Any non-OK response — Cancel, Escape, window close (DELETE_EVENT)
            // — just dismisses. Never save or retry from here.
            if (response != Gtk::ResponseType::OK) {
                dialog->destroy();
                set_status("Not connected — press Connect to enter server details.");
                return;
            }
            const std::string url = server->get_text();
            const std::string k = key->get_text();
            if (url.empty() || k.empty()) {
                error->set_text("Server and API key are both required.");
                error->set_visible(true);
                return;   // keep the dialog open
            }
            // Verify the credentials asynchronously BEFORE committing them
            // (shared with the UiApi connect action — same probe+save path).
            error->set_text("Checking connection…");
            error->set_visible(true);
            cancel_btn->set_sensitive(false);
            ok_btn->set_sensitive(false);
            server->set_sensitive(false);
            key->set_sensitive(false);
            connect_with_credentials(url, k,
                [this, dialog, error, cancel_btn, ok_btn,
                 server, key](std::string err) {
                    if (!alive_ || connect_dialog_ != dialog) return;
                    if (!err.empty()) {
                        error->set_text("Connection failed: " + err);
                        cancel_btn->set_sensitive(true);
                        ok_btn->set_sensitive(true);
                        server->set_sensitive(true);
                        key->set_sensitive(true);
                        return;   // keep the dialog open for a retry
                    }
                    dialog->destroy();
                });
        });
    dialog->present();
}

// Probe-then-commit. Used by the Connect dialog AND the UiApi connect action
// (one code path — a headless test exercises the real login flow).
void HiveWindow::connect_with_credentials(const std::string& url,
                                          const std::string& key,
                                          std::function<void(std::string)> done) {
    std::thread([this, url, key, done = std::move(done)]() mutable {
        std::string err;
        try {
            ApiClient probe{url};
            probe.set_api_key(key);
            probe.list_agents();   // throws on network/auth failure
        } catch (const std::exception& e) {
            err = e.what();
        }
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, url, key, err, done = std::move(done)] {
                if (!alive_) return;
                if (err.empty()) {
                    server_url_ = url;
                    api_.set_base_url(server_url_);
                    api_.set_api_key(key);
                    save_config();
                    refresh_all();
                }
                done(err);
            });
        }
        dispatcher_.emit();
    }).detach();
}

// ── async plumbing ──────────────────────────────────────────────────────────

void HiveWindow::run_async(std::function<void()> work) {
    std::thread([this, work = std::move(work)]() mutable {
        try { work(); } catch (const std::exception& e) {
            std::string msg = e.what();
            {
                std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this, msg] {
                    if (!alive_) return;
                    set_status("Error: " + msg);
                    if (msg.find("401") != std::string::npos ||
                        msg.find("bearer") != std::string::npos ||
                        msg.find("api key") != std::string::npos)
                        maybe_show_connect_dialog();
                });
            }
            dispatcher_.emit();
        }
    }).detach();
}

void HiveWindow::set_status(const std::string& text) {
    status_label_.set_text(text);
}

// ── data loading ────────────────────────────────────────────────────────────

void HiveWindow::refresh_all() {
    run_async([this] {
        json agents = api_.list_agents();
        json threads = api_.list_threads();
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, agents, threads] {
                if (!alive_) return;
                agents_ = agents.value("agents", json::array());
                threads_ = threads.value("threads", json::array());
                process_thread_activity();
                rebuild_roster();
                rebuild_thread_list();
                // Chat-first UX: open the most recent conversation automatically.
                if (current_thread_.empty() && !threads_.empty()) {
                    current_thread_ = threads_[0].value("id", "");
                    last_message_id_.clear();
                    if (auto* row = threads_list_.get_row_at_index(0))
                        threads_list_.select_row(*row);
                    poll_chat();
                }
                set_status("Connected — " +
                           std::to_string(agents_.size()) + " agents, " +
                           std::to_string(threads_.size()) + " threads");
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::refresh_threads() {
    run_async([this] {
        json threads = api_.list_threads();
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, threads] {
                if (!alive_) return;
                threads_ = threads.value("threads", json::array());
                process_thread_activity();
                rebuild_thread_list();
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::poll_chat() {
    if (current_thread_.empty()) return;
    std::string tid = current_thread_;
    std::string after = last_message_id_;
    run_async([this, tid, after] {
        json msgs = api_.list_messages(tid, after);
        json act = api_.activity(tid);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, tid, msgs, act] {
                if (!alive_ || tid != current_thread_) return;
                auto arr = msgs.value("messages", json::array());
                for (auto& m : arr) {
                    append_message_bubble(
                        m.value("sender_kind", ""),
                        m.value("sender_kind", "") == "agent"
                            ? agent_name_of(agents_, m.value("sender_id", ""))
                            : (m.value("sender_kind", "") == "user" ? "You" : "system"),
                        m.value("body", ""),
                        m.value("created_at", ""),
                        m.value("attachments", json::array()));
                    last_message_id_ = m.value("id", last_message_id_);
                }
                if (!arr.empty()) {
                    // scroll to bottom
                    auto adj = chat_scroll_.get_vadjustment();
                    adj->set_value(adj->get_upper() - adj->get_page_size());
                }
                bool busy = act.value("busy", false);
                thread_busy_ = busy;
                stop_btn_.set_sensitive(busy);
                if (busy) {
                    std::string who;
                    for (auto& t : act.value("turns", json::array()))
                        if (!t.value("done", true)) {
                            if (!who.empty()) who += ", ";
                            who += t.value("agent_name", "?");
                        }
                    set_status(who + " is working…");
                }
            });
        }
        dispatcher_.emit();
    });
}

// Roster activity poll (Phase 27): the spinner/state tag must track live
// turns even when the user isn't in that agent's chat. Rebuild ONLY when
// the busy/state signature changes — rebuilding rows every tick would
// fight the user's selection/scroll.
void HiveWindow::poll_roster() {
    run_async([this] {
        json agents = api_.list_agents();
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, agents] {
                if (!alive_) return;
                auto arr = agents.value("agents", json::array());
                std::string sig;
                for (auto& a : arr) {
                    sig += a.value("id", "") + ":" +
                           (a.value("busy", false) ? "1" : "0") + ":" +
                           a.value("state", "") + ";";
                }
                if (sig == roster_sig_) return;
                roster_sig_ = sig;
                agents_ = arr;
                rebuild_roster();
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::poll_approvals() {
    run_async([this] {
        json ap = api_.list_approvals("pending");
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, ap] {
                if (!alive_) return;
                int n = (int)ap.value("approvals", json::array()).size();
                if (n != pending_approvals_) {
                    // Phase 14.1: a NEW parked approval deserves a notification.
                    if (pending_approvals_ >= 0 && n > pending_approvals_)
                        notify("Approval needed",
                               std::to_string(n) + " command(s) waiting for review");
                    pending_approvals_ = n;
                    approvals_label_.set_label(n > 0
                        ? "Approvals (" + std::to_string(n) + ")"
                        : "Approvals");
                    if (n > 0) approvals_btn_.add_css_class("destructive-action");
                    else approvals_btn_.remove_css_class("destructive-action");
                }
            });
        }
        dispatcher_.emit();
    });
}

// ── rendering ───────────────────────────────────────────────────────────────

void HiveWindow::rebuild_roster() {
    while (auto* row = roster_list_.get_row_at_index(0))
        roster_list_.remove(*row);
    for (auto& a : agents_) {
        auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        row_box->set_margin_top(4);
        row_box->set_margin_bottom(4);
        row_box->append(*make_avatar(a.value("name", "?"), false, 40,
                                     a.value("avatar", "")));
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        box->set_valign(Gtk::Align::CENTER);
        auto* name = Gtk::make_managed<Gtk::Label>(a.value("name", "?"));
        name->set_halign(Gtk::Align::START);
        name->add_css_class("heading");
        // Busy indicators: spinner + daedalus state tag (Phase 27).
        // The roster is visible even when the agent's chat isn't — the
        // spinner catches the eye; the tag says what it's doing
        // ("acting", "planning", "verifying", …).
        // The daedalus state is authoritative (straight from the Agent);
        // the LiveTurn 'busy' flag misses some turn paths — OR them.
        std::string st_ = a.value("state", "");
        for (auto& ch : st_) ch = (char)tolower((unsigned char)ch);
        const bool working = a.value("busy", false) ||
            (!st_.empty() && st_ != "idle" && st_ != "done" &&
             st_ != "cancelled" && st_ != "error");
        if (working) {
            auto* spin = Gtk::make_managed<Gtk::Spinner>();
            spin->start();
            spin->set_valign(Gtk::Align::CENTER);
            spin->set_tooltip_text("Working…");
            row_box->append(*spin);
            const std::string state = a.value("state", "");
            if (!state.empty() && state != "idle") {
                auto* tag = Gtk::make_managed<Gtk::Label>(state);
                tag->add_css_class("hv-state-tag");
                tag->set_valign(Gtk::Align::CENTER);
                row_box->append(*tag);
            }
        }
        auto* role = Gtk::make_managed<Gtk::Label>(a.value("role", ""));
        role->set_halign(Gtk::Align::START);
        role->set_ellipsize(Pango::EllipsizeMode::END);
        role->set_max_width_chars(28);
        role->add_css_class("dim-label");
        box->append(*name);
        box->append(*role);
        row_box->append(*box);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*row_box);
        row->set_data("agent_id", new std::string(a.value("id", "")),
                      [](void* p) { delete static_cast<std::string*>(p); });
        // Right-click → agent context menu (Edit / Delete)
        auto rc = Gtk::GestureClick::create();
        rc->set_button(0);   // listen to all buttons, filter in the handler
        rc->signal_pressed().connect(
            [this, rc, agent = a](int, double x, double y) {
                if (rc->get_current_button() == GDK_BUTTON_SECONDARY)
                    show_agent_menu(*rc->get_widget(), agent, x, y);
            });
        row->add_controller(rc);
        roster_list_.append(*row);
    }
}

// Desktop notification via the app ID (com.hivemind.client). Best-effort:
// if the portal/notification daemon is absent this is a silent no-op.
void HiveWindow::notify(const Glib::ustring& title, const Glib::ustring& body) {
    auto app = get_application();
    if (!app) return;
    auto n = Gio::Notification::create(title);
    n->set_body(body);
    n->set_icon(themed_icon_pixbuf("hive", 48, dark_mode_));
    app->send_notification("hivemind-activity", n);
}

// After each threads refresh: badge rows with unseen activity, and pop a
// desktop notification for threads we're not looking at while the window
// is unfocused. First call only seeds the baseline (no storm at startup).
void HiveWindow::process_thread_activity() {
    for (auto& t : threads_) {
        const std::string id = t.value("id", "");
        const std::string act = t.value("last_activity", "");
        if (id.empty()) continue;
        // First pass ever: record the baseline, notify/badge nothing.
        if (!activity_seeded_) { last_seen_activity_[id] = act; continue; }
        // The thread on screen is always considered seen.
        if (id == current_thread_) { last_seen_activity_[id] = act; continue; }
        // Other threads: notify on new activity while unfocused. The badge
        // itself is computed in rebuild_thread_list() from this same map —
        // last_seen stays untouched here so the badge persists.
        auto it = last_seen_activity_.find(id);
        const bool is_newer = !act.empty() &&
                              (it == last_seen_activity_.end() || act > it->second);
        if (is_newer && it != last_seen_activity_.end() && !is_active()) {
            std::string title = t.value("title", "");
            if (title.empty())
                for (auto& aid : t.value("agent_ids", json::array())) {
                    if (!title.empty()) title += ", ";
                    title += agent_name_of(agents_, aid.get<std::string>());
                }
            notify(title, "New messages in this thread");
        }
    }
    activity_seeded_ = true;
}

void HiveWindow::rebuild_thread_list() {
    while (auto* row = threads_list_.get_row_at_index(0))
        threads_list_.remove(*row);
    for (auto& t : threads_) {
        std::string title = t.value("title", "");
        if (title.empty()) {
            // fall back to participant names
            for (auto& id : t.value("agent_ids", json::array())) {
                if (!title.empty()) title += ", ";
                title += agent_name_of(agents_, id.get<std::string>());
            }
        }
        auto* label = Gtk::make_managed<Gtk::Label>(title);
        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_hexpand(true);
        auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        row_box->set_margin(6);
        row_box->append(*label);
        // Unread dot: activity newer than last-seen and not on screen.
        const std::string tid = t.value("id", "");
        const std::string act = t.value("last_activity", "");
        auto seen = last_seen_activity_.find(tid);
        const bool unread = activity_seeded_ && !act.empty() && tid != current_thread_ &&
            (seen == last_seen_activity_.end() || act > seen->second);
        if (unread) {
            auto* dot = Gtk::make_managed<Gtk::Label>("\u25CF");  // ●
            dot->add_css_class("hv-unread-dot");
            dot->set_valign(Gtk::Align::CENTER);
            row_box->append(*dot);
        }
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*row_box);
        row->set_data("thread_id", new std::string(t.value("id", "")),
                      [](void* p) { delete static_cast<std::string*>(p); });
        threads_list_.append(*row);
        // Keep the visible selection on the thread the chat is showing —
        // rebuilding the list drops it otherwise (refresh clears highlight).
        if (!current_thread_.empty() && t.value("id", "") == current_thread_)
            threads_list_.select_row(*row);
    }
    update_thread_title();
}

void HiveWindow::update_thread_title() {
    if (current_thread_.empty()) { thread_title_.set_text(""); return; }
    for (auto& t : threads_) {
        if (t.value("id", "") != current_thread_) continue;
        std::string title = t.value("title", "");
        if (title.empty())
            for (auto& id : t.value("agent_ids", json::array())) {
                if (!title.empty()) title += ", ";
                title += agent_name_of(agents_, id.get<std::string>());
            }
        thread_title_.set_text("\U0001F4AC " + title);
        return;
    }
    thread_title_.set_text("");
}

void HiveWindow::append_message_bubble(const std::string& sender_kind,
                                       const std::string& sender_label,
                                       const std::string& body,
                                       const std::string& created_at,
                                       const nlohmann::json& attachments) {
    rendered_.push_back({{"sender_kind", sender_kind},
                         {"sender_label", sender_label},
                         {"body", body},
                         {"created_at", created_at},
                         {"attachments", attachments}});
    // Raw transcript to debug log when HIVEMIND_DEBUG=1
    static const bool debug = std::getenv("HIVEMIND_DEBUG") != nullptr;
    if (debug) {
        namespace fs = std::filesystem;
        fs::path dir = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                     / ".local" / "share" / "hivemind";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f{dir / "client-raw.log", std::ios::app};
        f << "[" << created_at << "] " << sender_kind << "/" << sender_label
          << "\n" << body << "\n\n";
    }

    // Older thread history still holds raw daedalus transcripts (pre-fix
    // server posted those); new messages arrive clean. Filter defensively.
    const std::string clean = sender_kind == "agent" ? strip_tool_transcript(body) : body;
    if (clean.empty()) return;   // pure tool-call transcript — nothing to show

    // System messages stay minimal: centered dim line, no avatar.
    if (sender_kind != "user" && sender_kind != "agent") {
        auto* sys = Gtk::make_managed<Gtk::Label>(clean);
        sys->add_css_class("hv-msg-system");
        sys->set_wrap(true);
        sys->set_selectable(true);
        messages_box_.append(*sys);
        return;
    }

    // Bubble row: avatar | (name + time / content)
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->add_css_class(sender_kind == "user" ? "hv-msg-user" : "hv-msg-agent");
    std::string sender_avatar;
    if (sender_kind == "agent")
        for (auto& a : agents_)
            if (a.value("name", "") == sender_label) {
                sender_avatar = a.value("avatar", "");
                break;
            }
    row->append(*make_avatar(sender_label, sender_kind == "user",
                             AVATAR_LOGICAL, sender_avatar));

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_hexpand(true);
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* who = Gtk::make_managed<Gtk::Label>(sender_label);
    who->set_halign(Gtk::Align::START);
    who->add_css_class("hv-name");
    who->add_css_class(sender_kind == "user"
        ? "hv-name-user"
        : "hv-name-" + std::to_string(palette_index(sender_label)));
    header->append(*who);
    std::string when = format_msg_time(created_at);
    if (!when.empty()) {
        auto* time = Gtk::make_managed<Gtk::Label>(when);
        time->set_halign(Gtk::Align::END);
        time->set_hexpand(true);
        time->add_css_class("hv-time");
        header->append(*time);
    }
    box->append(*header);

    for (auto& seg : split_segments(clean)) {
        auto* text = Gtk::make_managed<Gtk::Label>();
        text->set_halign(Gtk::Align::START);
        text->set_wrap(true);
        text->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
        text->set_selectable(true);
        text->set_xalign(0);
        if (seg.code) {
            text->set_text(seg.text);
            text->add_css_class("hv-code");
        } else {
            text->set_markup(prose_to_markup(seg.text));
        }
        box->append(*text);
    }

    // Phase 18: attachment chips — click opens the local copy (the
    // workspace is a host folder; agents see it at /workspace).
    if (attachments.is_array() && !attachments.empty()) {
        auto* chips = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        for (auto& a : attachments) {
            const std::string name = a.value("name", "file");
            const std::string host = a.value("host_path", "");
            auto* chip = Gtk::make_managed<Gtk::Button>("📎 " + name);
            chip->add_css_class("flat");
            chip->set_tooltip_text(host.empty() ? name : host);
            if (!host.empty())
                chip->signal_clicked().connect([host] {
                    Gio::AppInfo::launch_default_for_uri("file://" + host);
                });
            chips->append(*chip);
        }
        box->append(*chips);
    }
    row->append(*box);
    messages_box_.append(*row);
}

// ── callbacks ───────────────────────────────────────────────────────────────

void HiveWindow::on_agent_selected() {
    auto* row = roster_list_.get_selected_row();
    if (!row) return;
    auto* id = static_cast<std::string*>(row->get_data("agent_id"));
    if (!id) return;
    // Start (or focus) a DM with this agent.
    std::string aid = *id;
    run_async([this, aid] {
        json t = api_.create_thread("", {aid}, /*reuse_dm=*/true);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, t] {
                if (!alive_) return;
                current_thread_ = t.value("id", "");
                last_message_id_.clear();
                while (auto* c = messages_box_.get_first_child())
                    messages_box_.remove(*c);
                rendered_.clear();
                refresh_threads();
                poll_chat();
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::on_thread_selected() {
    auto* row = threads_list_.get_selected_row();
    if (!row) return;
    auto* id = static_cast<std::string*>(row->get_data("thread_id"));
    if (!id || *id == current_thread_) return;
    current_thread_ = *id;
    last_message_id_.clear();
    delete_thread_btn_.set_sensitive(true);
    learn_btn_.set_sensitive(true);
    for (auto& t : threads_)
        if (t.value("id", "") == *id)
            last_seen_activity_[*id] = t.value("last_activity", "");
    update_thread_title();
    while (auto* c = messages_box_.get_first_child())
        messages_box_.remove(*c);
    rendered_.clear();
    poll_chat();
}

void HiveWindow::on_send() {
    std::string text = entry_.get_text();
    if (current_thread_.empty()) return;
    if (text.empty() && pending_attachments_.empty()) return;
    entry_.set_text("");
    json atts = pending_attachments_;            // take
    pending_attachments_ = json::array();
    update_attach_row();
    std::string tid = current_thread_;
    run_async([this, tid, text, atts] {
        api_.post_message(tid, text, atts);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this] { if (alive_) poll_chat(); });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::on_learn_routine() {
    if (current_thread_.empty()) return;
    const std::string tid = current_thread_;
    const std::string text =
        "[learn] Please save the workflow demonstrated in this thread as a "
        "routine using the save_routine tool. Distill the steps into a "
        "self-contained prompt (include every URL, file path, and value "
        "used — future runs have no access to this conversation). Pick a "
        "clear kebab-case name. If I haven't specified a schedule, ask me "
        "for one before saving.";
    run_async([this, tid, text] {
        api_.post_message(tid, text);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this] { if (alive_) poll_chat(); });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::update_attach_row() {
    const auto n = pending_attachments_.size();
    if (n == 0) { attach_row_.set_visible(false); return; }
    std::string s = "📎 " + pending_attachments_[0].value("name", "file");
    if (n > 1) s += "  (+" + std::to_string(n - 1) + " more)";
    attach_label_.set_label(s);
    attach_row_.set_visible(true);
}

void HiveWindow::on_attach() {
    if (current_thread_.empty()) return;
    auto dialog = Gtk::FileDialog::create();   // refcounted — kept by capture
    dialog->set_title("Attach a file");
    dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> file;
        try { file = dialog->open_finish(result); }
        catch (...) { return; }                // user cancelled
        if (!file) return;
        const std::string path = file->get_path();
        const std::string tid = current_thread_;
        if (path.empty() || tid.empty()) return;
        run_async([this, tid, path] {
            std::ifstream in(path, std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>()};
            if (bytes.empty())
                throw std::runtime_error("cannot read (or empty): " + path);
            namespace fs = std::filesystem;
            auto meta = api_.upload_attachment(
                tid, fs::path(path).filename().string(), bytes);
            {
                std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this, meta] {
                    if (!alive_) return;
                    pending_attachments_.push_back(meta);
                    update_attach_row();
                    set_status("Attached " + meta.value("name", "file") +
                               " — send a message to deliver it");
                });
            }
            dispatcher_.emit();
        });
    });
}

void HiveWindow::show_agent_menu(Gtk::Widget& anchor, const nlohmann::json& agent,
                                 double x, double y) {
    auto* pop = Gtk::make_managed<Gtk::Popover>();
    pop->set_has_arrow(false);
    pop->set_autohide(true);
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    box->set_margin(4);
    auto mk = [&](const char* label, const char* css,
                  std::function<void()> fn) {
        auto* b = Gtk::make_managed<Gtk::Button>(label);
        b->set_has_frame(false);
        b->add_css_class("hv-menu-item");
        if (css) b->add_css_class(css);
        b->signal_clicked().connect([pop, fn = std::move(fn)] {
            pop->popdown();
            fn();
        });
        box->append(*b);
    };
    mk("\U0001F5A5 Computer", nullptr, [this, agent] { show_agent_screen(agent); });
    mk("\U0001F441 Computer (view-only)", nullptr,
       [this, agent] { show_agent_screen(agent, true); });
    mk("\U0001F4CB Exec log", nullptr, [this, agent] { show_exec_log(agent); });
    mk("Add Task\u2026", nullptr,
       [this, agent] { show_task_dialog(agent.value("id", "")); });
    mk("Routines\u2026", nullptr, [this] { show_routines_dialog(); });
    mk("Skills\u2026", nullptr, [this, agent] { show_skills_dialog(agent); });
    mk("Inspector\u2026", nullptr, [this, agent] { show_inspector(agent); });
    mk("Edit Agent", nullptr, [this, agent] { on_edit_agent(agent); });
    mk("Delete Agent", "destructive-action", [this, agent] { on_delete_agent(agent); });
    pop->set_child(*box);
    pop->set_parent(anchor);
    Gdk::Rectangle rect((int)x, (int)y, 1, 1);
    pop->set_pointing_to(rect);
    pop->signal_closed().connect([pop] { pop->unparent(); });
    pop->popup();
}

// ── Agent Computer screen (embedded noVNC via WebKitGTK) ───────────────────
// Right-click an agent → Computer. Loads the server's screen URL, which
// redirects to noVNC with autoconnect + the per-agent VNC password filled.
// The ?key= sets the HttpOnly screen cookie on first load, so the WS
// handshake inside WebKit authenticates too. Without WebKitGTK the menu
// item falls back to the system browser.
void HiveWindow::show_agent_screen(const nlohmann::json& agent, bool view_only) {
    const std::string id = agent.value("id", "");
    if (id.empty()) return;
    std::string url = api_.base_url() + "/screen/" + id +
                      "/?key=" + api_.api_key();
    if (view_only) url += "&view_only=1";

    // The screen stack may not be running — POST screen/start first or
    // the page 503s. ApiClient is synchronous, so start in a worker
    // thread; the UI thread opens the window immediately.
    std::thread([this, id] {
        try { api_.screen_start(id); } catch (...) {}
    }).detach();

#ifdef HIVEMIND_WITH_WEBKIT
    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title("Computer — " + agent.value("name", id));
    win->set_default_size(1320, 860);
    win->set_transient_for(*this);

    // WebKitWebView is plain GTK4 C API — embeds fine in a gtkmm window.
    auto* wv = webkit_web_view_new();
    gtk_widget_set_vexpand(wv, TRUE);
    gtk_widget_set_hexpand(wv, TRUE);
    win->set_child(*Glib::wrap(GTK_WIDGET(wv)));
    win->present();

    // Give the bring-up a moment, then load. If the screen still isn't
    // up, noVNC shows the server's error page and the user can retry.
    Glib::signal_timeout().connect_once([wv, url] {
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(wv), url.c_str());
    }, 3000);
#else
    // System-browser fallback: same delay logic via a timeout, then open.
    Glib::signal_timeout().connect_once([url] {
        Gio::AppInfo::launch_default_for_uri(url);
    }, 3000);
#endif
}

// ── Exec log viewer (Phase 29) ──────────────────────────────────────────
// Right-click an agent → Exec log. Every command the agent ran in its
// container, newest first — "what did it do" without leaving the app.
void HiveWindow::show_exec_log(const nlohmann::json& agent) {
    const std::string id = agent.value("id", "");
    const std::string name = agent.value("name", "?");
    if (id.empty()) return;

    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title("Exec log — " + name);
    win->set_transient_for(*this);
    win->set_default_size(760, 480);

    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    vbox->set_margin(10);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    auto* view = Gtk::make_managed<Gtk::TextView>();
    view->set_editable(false);
    view->set_monospace(true);
    view->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    scroll->set_child(*view);
    vbox->append(*scroll);
    auto* status = Gtk::make_managed<Gtk::Label>("Loading…");
    status->set_halign(Gtk::Align::START);
    vbox->append(*status);
    win->set_child(*vbox);
    win->present();

    auto buf = view->get_buffer();
    auto tag_ok  = buf->create_tag("ok");   tag_ok->property_foreground() = "#8aadf4";
    auto tag_bad = buf->create_tag("bad");  tag_bad->property_foreground() = "#ed8796";
    auto tag_to  = buf->create_tag("to");   tag_to->property_foreground() = "#eed49f";
    auto tag_dim = buf->create_tag("dim");  tag_dim->property_foreground() = "#6e738d";

    run_async([this, id, name, buf, status, tag_ok, tag_bad, tag_to, tag_dim] {
        std::string err;
        json data;
        try { data = api_.exec_log(id); }
        catch (const std::exception& e) { err = e.what(); }
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([=] {
                if (!alive_) return;
                if (!err.empty()) { status->set_text("Error: " + err); return; }
                auto entries = data.value("entries", json::array());
                for (auto& e : entries) {
                    const int code = e.value("exit_code", -1);
                    const bool to = e.value("timed_out", false);
                    auto tag = to ? tag_to : (code == 0 ? tag_ok : tag_bad);
                    std::string head = e.value("ts", "") + "  exit " +
                        std::to_string(code) + (to ? " (timeout)" : "") +
                        "  " + std::to_string(e.value("duration_ms", 0)) + "ms";
                    buf->insert_with_tag(buf->end(), head + "\n", tag_dim);
                    buf->insert_with_tag(buf->end(),
                        "  $ " + e.value("command", "") + "\n", tag);
                }
                status->set_text(std::to_string(entries.size()) +
                                 " entries — newest first");
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::on_edit_agent(const nlohmann::json& agent) {
    show_agent_dialog(&agent);   // dialog reads fields during construction only
}

// ── Phase 14.2: per-agent skills dialog ─────────────────────────────────────
// Two sections: the SHARED global library (toggle per skill — creates/removes
// the agent's symlink via POST /api/agents/:id/global-skills) and the agent's
// own learned skills (read-only — those are the agent's to manage).
void HiveWindow::show_skills_dialog(const nlohmann::json& agent) {
    const std::string agent_id = agent.value("id", "");
    const std::string aname = agent.value("name", "?");
    if (agent_id.empty()) return;

    auto* dialog = Gtk::make_managed<Gtk::Window>();
    dialog->set_title("Skills — " + aname);
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->set_default_size(480, 440);

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    outer->set_margin(14);
    auto* loading = Gtk::make_managed<Gtk::Label>("Loading…");
    loading->add_css_class("dim-label");
    outer->append(*loading);
    dialog->set_child(*outer);
    dialog->present();

    run_async([this, dialog, outer, loading, agent_id, aname] {
        json global, enabled, local;
        std::string err;
        try {
            global  = api_.list_global_skills();
            enabled = api_.agent_global_skills(agent_id);
            local   = api_.list_agent_skills(agent_id);
        } catch (const std::exception& e) { err = e.what(); }
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, dialog, outer, loading, agent_id, aname,
                                   global, enabled, local, err] {
                if (!alive_) return;
                outer->remove(*loading);
                if (!err.empty()) {
                    auto* l = Gtk::make_managed<Gtk::Label>("Failed to load skills: " + err);
                    l->add_css_class("error");
                    outer->append(*l);
                    return;
                }
                std::set<std::string> enabled_set;
                for (auto& s : enabled.value("skills", json::array()))
                    enabled_set.insert(s.get<std::string>());

                auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
                scroll->set_vexpand(true);
                auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
                scroll->set_child(*list);

                auto* gh = Gtk::make_managed<Gtk::Label>("Shared library");
                gh->set_halign(Gtk::Align::START);
                gh->add_css_class("heading");
                list->append(*gh);

                auto skills = global.value("skills", json::array());
                if (skills.empty()) {
                    auto* l = Gtk::make_managed<Gtk::Label>(
                        "The global library is empty — add skills on the web Skills page.");
                    l->set_halign(Gtk::Align::START);
                    l->set_wrap(true);
                    l->add_css_class("dim-label");
                    list->append(*l);
                }
                for (auto& s : skills) {
                    const std::string sname = s.value("name", "");
                    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
                    text->set_hexpand(true);
                    std::string title = sname;
                    std::string ver = s.value("version", "");
                    if (!ver.empty()) title += "  v" + ver;
                    auto* nl = Gtk::make_managed<Gtk::Label>(title);
                    nl->set_halign(Gtk::Align::START);
                    nl->add_css_class("heading");
                    text->append(*nl);
                    std::string desc = s.value("description", "");
                    if (!desc.empty()) {
                        auto* dl = Gtk::make_managed<Gtk::Label>(desc);
                        dl->set_halign(Gtk::Align::START);
                        dl->set_wrap(true);
                        dl->set_xalign(0);
                        dl->add_css_class("dim-label");
                        text->append(*dl);
                    }
                    row->append(*text);
                    auto* sw = Gtk::make_managed<Gtk::Switch>();
                    sw->set_valign(Gtk::Align::CENTER);
                    sw->set_active(enabled_set.count(sname) > 0);
                    // Optimistic toggle: apply visually, POST async, revert on
                    // failure (guard suppresses the recursive signal_changed).
                    auto guard = std::make_shared<bool>(false);
                    sw->property_active().signal_changed().connect(
                        [this, sw, agent_id, sname, guard] {
                            if (*guard) return;
                            const bool on = sw->get_active();
                            sw->set_sensitive(false);
                            run_async([this, sw, agent_id, sname, on, guard] {
                                std::string err;
                                try { api_.set_global_skill(agent_id, sname, on); }
                                catch (const std::exception& e) { err = e.what(); }
                                {
                                    std::lock_guard<std::mutex> lk(queue_m_);
                                    done_queue_.push_back([this, sw, on, guard, err] {
                                        if (!alive_) return;
                                        sw->set_sensitive(true);
                                        if (!err.empty()) {
                                            *guard = true;
                                            sw->set_active(!on);
                                            *guard = false;
                                            set_status("Skill toggle failed: " + err);
                                        }
                                    });
                                }
                                dispatcher_.emit();
                            });
                        });
                    row->append(*sw);
                    list->append(*row);
                }

                auto locals = local.value("skills", json::array());
                if (!locals.empty()) {
                    auto* lh = Gtk::make_managed<Gtk::Label>("Learned by " + aname);
                    lh->set_halign(Gtk::Align::START);
                    lh->add_css_class("heading");
                    lh->set_margin_top(10);
                    list->append(*lh);
                    for (auto& s : locals) {
                        auto* l = Gtk::make_managed<Gtk::Label>(
                            "\u2022  " + s.get<std::string>());
                        l->set_halign(Gtk::Align::START);
                        l->add_css_class("dim-label");
                        list->append(*l);
                    }
                    auto* note = Gtk::make_managed<Gtk::Label>(
                        "Local skills belong to " + aname + " — it manages them itself.");
                    note->set_halign(Gtk::Align::START);
                    note->set_wrap(true);
                    note->set_xalign(0);
                    note->add_css_class("dim-label");
                    list->append(*note);
                }
                outer->append(*scroll);
            });
        }
        dispatcher_.emit();
    });
}

// ── Phase 14.4: agent inspector ─────────────────────────────────────────────
// Read-only details (model, workspace, timestamps, on-disk path) + live
// container status with Start/Stop. Config editing stays in Edit Agent.
void HiveWindow::show_inspector(const nlohmann::json& agent) {
    const std::string agent_id = agent.value("id", "");
    if (agent_id.empty()) return;
    auto* dialog = Gtk::make_managed<Gtk::Window>();
    dialog->set_title("Inspector — " + agent.value("name", "?"));
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->set_default_size(520, 380);

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    outer->set_margin(16);

    auto kv = [&](const std::string& k, const std::string& v) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* kl = Gtk::make_managed<Gtk::Label>(k);
        kl->add_css_class("dim-label");
        kl->set_halign(Gtk::Align::START);
        kl->set_size_request(130, -1);
        auto* vl = Gtk::make_managed<Gtk::Label>(v.empty() ? "—" : v);
        vl->set_halign(Gtk::Align::START);
        vl->set_selectable(true);
        vl->set_wrap(true);
        vl->set_xalign(0);
        row->append(*kl);
        row->append(*vl);
        outer->append(*row);
    };
    kv("Model", agent.value("model", ""));
    kv("Workspace mode", agent.value("workspace_mode", ""));
    kv("Require approval", agent.value("require_approval", false) ? "yes" : "no");
    kv("Created", agent.value("created_at", ""));
    kv("Updated", agent.value("updated_at", ""));
    kv("Data directory", "~/.local/share/hivemind/agents/" + agent_id + "/");

    auto* ch = Gtk::make_managed<Gtk::Label>("Container");
    ch->set_halign(Gtk::Align::START);
    ch->add_css_class("heading");
    outer->append(*ch);
    auto* cbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    outer->append(*cbox);
    dialog->set_child(*outer);
    dialog->present();
    load_inspector_container(cbox, dialog, agent_id);
}

void HiveWindow::load_inspector_container(Gtk::Box* box, Gtk::Window* dialog,
                                          const std::string& agent_id) {
    auto* loading = Gtk::make_managed<Gtk::Label>("Checking container…");
    loading->add_css_class("dim-label");
    loading->set_halign(Gtk::Align::START);
    box->append(*loading);
    run_async([this, box, dialog, loading, agent_id] {
        json st; std::string err;
        try { st = api_.container_status(agent_id); }
        catch (const std::exception& e) { err = e.what(); }
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, box, dialog, loading, agent_id, st, err] {
                if (!alive_) return;
                box->remove(*loading);
                if (!err.empty()) {
                    auto* l = Gtk::make_managed<Gtk::Label>("Status failed: " + err);
                    l->add_css_class("error");
                    box->append(*l);
                    return;
                }
                if (!st.value("podman_available", false)) {
                    auto* l = Gtk::make_managed<Gtk::Label>("podman not available on the server");
                    l->add_css_class("dim-label");
                    box->append(*l);
                    return;
                }
                const bool exists = st.value("exists", false);
                const bool running = st.value("running", false);
                auto* sl = Gtk::make_managed<Gtk::Label>(
                    running ? "\u25CF running" : (exists ? "\u25CB stopped" : "not created yet"));
                sl->set_halign(Gtk::Align::START);
                sl->add_css_class(running ? "hv-unread-dot" : "dim-label");
                box->append(*sl);
                auto ck = [&](const std::string& k, const std::string& v) {
                    if (v.empty()) return;
                    auto* l = Gtk::make_managed<Gtk::Label>(k + ": " + v);
                    l->set_halign(Gtk::Align::START);
                    l->add_css_class("dim-label");
                    l->set_selectable(true);
                    box->append(*l);
                };
                ck("container", st.value("container", ""));
                ck("image", st.value("image", ""));
                ck("workspace", st.value("workspace", ""));
                auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto act = [&](const char* label, bool sensitive, bool start) {
                    auto* b = Gtk::make_managed<Gtk::Button>(label);
                    b->set_sensitive(sensitive);
                    b->signal_clicked().connect([this, box, dialog, agent_id, start] {
                        while (auto* c = box->get_first_child()) box->remove(*c);
                        run_async([this, agent_id, start] {
                            std::string err;
                            try { start ? (void)api_.container_start(agent_id)
                                        : (void)api_.container_stop(agent_id); }
                            catch (const std::exception& e) { err = e.what(); }
                            {
                                std::lock_guard<std::mutex> lk(queue_m_);
                                done_queue_.push_back([this, err] {
                                    if (!alive_) return;
                                    if (!err.empty()) set_status("Container: " + err);
                                });
                            }
                            dispatcher_.emit();
                        });
                        // re-poll status shortly after (start/stop can take a moment)
                        Glib::signal_timeout().connect_once(
                            [this, box, dialog, agent_id] {
                                if (alive_) load_inspector_container(box, dialog, agent_id);
                            }, 1500);
                    });
                    btns->append(*b);
                };
                act("Start", !running, true);
                act("Stop", running, false);
                box->append(*btns);
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::on_delete_agent(const nlohmann::json& agent) {
    const std::string id   = agent.value("id", "");
    const std::string name = agent.value("name", "?");

    // What does this agent own? Threads come from the cached roster state;
    // routines are fetched fresh (blocking call, same pattern as the
    // provider/model dropdowns in the agent dialog).
    int n_threads_solo = 0, n_threads_group = 0;
    for (auto& t : threads_) {
        auto ids = t.value("agent_ids", nlohmann::json::array());
        bool has = false;
        for (auto& a : ids) if (a.get<std::string>() == id) { has = true; break; }
        if (!has) continue;
        if (ids.size() <= 1) ++n_threads_solo; else ++n_threads_group;
    }
    nlohmann::json owned_routines = nlohmann::json::array();
    try {
        for (auto& r : api_.list_routines().value("routines", nlohmann::json::array()))
            if (r.value("agent_id", "") == id) owned_routines.push_back(r);
    } catch (...) { /* server unreachable — treat as none */ }
    const int n_routines = (int)owned_routines.size();

    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    const std::string title = "Delete agent";
    dialog->set_title(title);
    install_dialog_titlebar(*dialog, title);
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button("Delete", Gtk::ResponseType::OK);
    dialog->set_default_response(Gtk::ResponseType::CANCEL);  // safe default

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(16);

    auto* head = Gtk::make_managed<Gtk::Label>("Delete \u201C" + name + "\u201D?");
    head->add_css_class("heading");
    head->set_halign(Gtk::Align::START);
    box->append(*head);

    std::string detail = "The roster entry is removed. Conversation history and the workspace stay on disk.";
    if (n_threads_solo > 0 || n_threads_group > 0) {
        detail += "\n\nThreads: ";
        std::vector<std::string> parts;
        if (n_threads_solo)
            parts.push_back(std::to_string(n_threads_solo) + " private thread(s) will be deleted");
        if (n_threads_group)
            parts.push_back(std::to_string(n_threads_group) + " group thread(s) continue without " + name);
        for (size_t i = 0; i < parts.size(); ++i) { if (i) detail += "; "; detail += parts[i]; }
        detail += ".";
    }
    auto* info = Gtk::make_managed<Gtk::Label>(detail);
    info->set_wrap(true);
    info->set_max_width_chars(52);
    info->set_halign(Gtk::Align::START);
    info->add_css_class("dim-label");
    box->append(*info);

    // Reassignment choice — only when the agent actually owns tasks and
    // there is someone else to take them.
    std::vector<Glib::ustring> choices;
    auto* reassign_ids = new std::vector<std::string>();   // freed on response
    if (n_routines > 0) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(
            std::to_string(n_routines) + " scheduled task(s):");
        lbl->set_halign(Gtk::Align::START);
        choices.push_back("Delete them");
        reassign_ids->push_back("");
        for (auto& a : agents_) {
            if (a.value("id", "") == id) continue;
            choices.push_back("Reassign to " + a.value("name", "?"));
            reassign_ids->push_back(a.value("id", ""));
        }
        auto* dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(choices));
        dd->set_selected(0);
        row->append(*lbl);
        row->append(*dd);
        box->append(*row);
        box->set_data("reassign_dd", dd);   // no ownership transfer — dialog-owned
    }

    dialog->get_content_area()->append(*box);
    dialog->signal_response().connect(
        [this, dialog, box, id, reassign_ids](int r) {
            std::string reassign;
            if (r == (int)Gtk::ResponseType::OK) {
                if (auto* dd = static_cast<Gtk::DropDown*>(box->get_data("reassign_dd"))) {
                    const auto sel = dd->get_selected();
                    if (sel > 0 && sel < reassign_ids->size())
                        reassign = (*reassign_ids)[sel];
                }
                run_async([this, id, reassign] {
                    api_.delete_agent(id, reassign);
                    {
                        std::lock_guard<std::mutex> lk(queue_m_);
                        done_queue_.push_back([this] { if (alive_) refresh_all(); });
                    }
                    dispatcher_.emit();
                });
            }
            delete reassign_ids;
            dialog->destroy();
        });
    dialog->present();
}

void HiveWindow::on_new_task() {
    show_task_dialog("");
}

// Schedule-a-task dialog (routines). Blocking create call on OK — same
// pattern as the provider/model fetch in the agent dialog — so server
// validation errors can be shown in-dialog without closing it.
void HiveWindow::show_task_dialog(const std::string& preselect_agent_id,
                                  const nlohmann::json* existing) {
    const bool editing = existing != nullptr;
    const json er = editing ? *existing : json::object();
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    const char* title = editing ? "Edit scheduled task" : "Add scheduled task";
    dialog->set_title(title);
    install_dialog_titlebar(*dialog, title);
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button(editing ? "Save" : "Create", Gtk::ResponseType::OK);
    dialog->set_default_response(Gtk::ResponseType::OK);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_margin(12);
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);

    auto* name = Gtk::make_managed<Gtk::Entry>();
    name->set_placeholder_text("Name (e.g. daily-brief)");

    // Agent dropdown (preselected when opened from an agent's menu)
    std::vector<Glib::ustring> agent_names;
    auto* agent_ids = new std::vector<std::string>();  // freed on response
    guint preselect = 0;
    for (auto& a : agents_) {
        agent_names.push_back(a.value("name", "?"));
        agent_ids->push_back(a.value("id", ""));
        if (a.value("id", "") == preselect_agent_id)
            preselect = agent_ids->size() - 1;
    }
    auto* agent_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(agent_names));
    if (!agent_ids->empty()) agent_dd->set_selected(preselect);

    auto* prompt = Gtk::make_managed<Gtk::TextView>();
    prompt->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    prompt->set_size_request(-1, 72);
    auto* prompt_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    prompt_scroll->set_child(*prompt);
    prompt_scroll->add_css_class("hv-prompt-frame");

    // Schedule: type dropdown + a stack of per-type field sets
    auto* type_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Once (one-shot)", "Interval (every N minutes)",
                                 "Daily", "Weekly"}));
    auto* stack = Gtk::make_managed<Gtk::Stack>();

    // once: date + time entries
    auto* once_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* once_date = Gtk::make_managed<Gtk::Entry>();
    once_date->set_placeholder_text("YYYY-MM-DD");
    once_date->set_max_width_chars(12);
    auto* once_time = Gtk::make_managed<Gtk::Entry>();
    once_time->set_placeholder_text("HH:MM");
    once_time->set_max_width_chars(6);
    once_box->append(*once_date);
    once_box->append(*once_time);
    stack->add(*once_box, "once", "Once");

    // interval: minutes spinner
    auto* every = Gtk::make_managed<Gtk::SpinButton>(
        Gtk::Adjustment::create(60, 1, 10080, 5, 30, 0));
    stack->add(*every, "interval", "Interval");

    // daily: time entry
    auto* daily_time = Gtk::make_managed<Gtk::Entry>();
    daily_time->set_placeholder_text("HH:MM (e.g. 09:00)");
    daily_time->set_max_width_chars(10);
    stack->add(*daily_time, "daily", "Daily");

    // weekly: time entry + weekday checkboxes
    auto* weekly_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto* weekly_time = Gtk::make_managed<Gtk::Entry>();
    weekly_time->set_placeholder_text("HH:MM (e.g. 09:00)");
    weekly_time->set_max_width_chars(10);
    weekly_box->append(*weekly_time);
    auto* days_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    const char* day_names[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    Gtk::CheckButton* day_checks[7];
    for (int i = 0; i < 7; ++i) {
        day_checks[i] = Gtk::make_managed<Gtk::CheckButton>(day_names[i]);
        if (i >= 1 && i <= 5) day_checks[i]->set_active(true);  // weekdays default
        days_box->append(*day_checks[i]);
    }
    weekly_box->append(*days_box);
    stack->add(*weekly_box, "weekly", "Weekly");

    stack->set_visible_child("once");
    // Edit mode (Phase 29): prefill from the existing routine.
    if (editing) {
        name->set_text(er.value("name", ""));
        prompt->get_buffer()->set_text(er.value("prompt", ""));
        const std::string raid = er.value("agent_id", "");
        for (size_t i = 0; i < agent_ids->size(); ++i)
            if ((*agent_ids)[i] == raid) { agent_dd->set_selected(i); break; }
        agent_dd->set_sensitive(false);   // routine stays bound to its thread/agent
        json sched = er.value("schedule", json::object());
        const std::string st = sched.value("type", "");
        if (st == "interval") {
            type_dd->set_selected(1); stack->set_visible_child("interval");
            every->set_value(sched.value("every_minutes", 60));
        } else if (st == "daily") {
            type_dd->set_selected(2); stack->set_visible_child("daily");
            daily_time->set_text(sched.value("time", ""));
        } else if (st == "weekly") {
            type_dd->set_selected(3); stack->set_visible_child("weekly");
            weekly_time->set_text(sched.value("time", ""));
            for (int i = 0; i < 7; ++i) day_checks[i]->set_active(false);
            for (auto& d : sched.value("days", json::array())) {
                int di = d.get<int>();
                if (di >= 0 && di < 7) day_checks[di]->set_active(true);
            }
        } else {
            const std::string at = sched.value("at", "");
            const auto tpos = at.find('T');
            if (tpos != std::string::npos) {
                once_date->set_text(at.substr(0, tpos));
                once_time->set_text(at.substr(tpos + 1));
            }
        }
    }
    type_dd->property_selected().signal_changed().connect([type_dd, stack] {
        const char* names[4] = {"once", "interval", "daily", "weekly"};
        const auto sel = type_dd->get_selected();
        if (sel < 4) stack->set_visible_child(names[sel]);
    });

    auto* err = Gtk::make_managed<Gtk::Label>("");
    err->add_css_class("error");
    err->set_halign(Gtk::Align::START);
    err->set_wrap(true);

    auto add_row = [grid](const char* label, Gtk::Widget* w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::END);
        grid->attach(*l, 0, row);
        w->set_hexpand(true);
        grid->attach(*w, 1, row);
    };
    add_row("Name", name, 0);
    add_row("Agent", agent_dd, 1);
    add_row("Prompt", prompt_scroll, 2);
    add_row("Schedule", type_dd, 3);
    grid->attach(*stack, 1, 4);
    grid->attach(*err, 1, 5);
    dialog->get_content_area()->append(*grid);

    dialog->signal_response().connect(
        [this, dialog, name, agent_dd, agent_ids, prompt, type_dd,
         once_date, once_time, every, daily_time, weekly_time, day_checks, err,
         editing, er](int r) {
            if (r != (int)Gtk::ResponseType::OK) {
                delete agent_ids;
                dialog->destroy();
                return;
            }
            const std::string n = name->get_text();
            const auto asel = agent_dd->get_selected();
            const std::string aid = asel < agent_ids->size() ? (*agent_ids)[asel] : "";
            const std::string p = prompt->get_buffer()->get_text();
            // Build the schedule spec from the visible stack page
            nlohmann::json sched;
            const auto tsel = type_dd->get_selected();
            if (tsel == 0) {
                sched = {{"type", "once"},
                         {"at", once_date->get_text().raw() + "T" + once_time->get_text().raw()}};
            } else if (tsel == 1) {
                sched = {{"type", "interval"},
                         {"every_minutes", every->get_value_as_int()}};
            } else if (tsel == 2) {
                sched = {{"type", "daily"}, {"time", daily_time->get_text().raw()}};
            } else {
                nlohmann::json days = nlohmann::json::array();
                for (int i = 0; i < 7; ++i)
                    if (day_checks[i]->get_active()) days.push_back(i);
                sched = {{"type", "weekly"}, {"time", weekly_time->get_text().raw()},
                         {"days", days}};
            }
            try {
                if (n.empty() || p.empty() || aid.empty())
                    throw std::runtime_error("name, agent and prompt are required");
                if (editing)
                    api_.update_routine(er.value("id", ""), n, p, sched);
                else
                    api_.create_routine(aid, n, p, sched);   // blocking — shows errors inline
                delete agent_ids;
                dialog->destroy();
                run_async([this] {   // nothing visual to refresh; keep queue pattern
                    std::lock_guard<std::mutex> lk(queue_m_);
                    done_queue_.push_back([this] { if (alive_) refresh_all(); });
                    dispatcher_.emit();
                });
            } catch (const std::exception& e) {
                err->set_text(e.what());   // keep the dialog open
            }
        });
    dialog->present();
}


// ── Routines manager (Phase 29) ─────────────────────────────────────────
// Roster right-click → Routines…: every scheduled task with Edit / Run now /
// enable toggle / Delete. Edit reuses the task dialog in edit mode (PATCH).
void HiveWindow::show_routines_dialog() {
    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title("Routines");
    win->set_transient_for(*this);
    win->set_default_size(640, 420);

    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    vbox->set_margin(10);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->add_css_class("boxed-list");
    scroll->set_child(*list);
    vbox->append(*scroll);
    auto* status = Gtk::make_managed<Gtk::Label>("Loading…");
    status->set_halign(Gtk::Align::START);
    vbox->append(*status);
    win->set_child(*vbox);

    auto reload = std::make_shared<std::function<void()>>();
    *reload = [this, list, status, reload] {
        run_async([this, list, status, reload] {
            std::string err; json data;
            try { data = api_.list_routines(); }
            catch (const std::exception& e) { err = e.what(); }
            {
                std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this, list, status, err, data, reload] {
                    if (!alive_) return;
                    if (!list->get_root()) return;   // window closed
                    while (auto* row = list->get_row_at_index(0))
                        list->remove(*row);
                    if (!err.empty()) { status->set_text("Error: " + err); return; }
                    auto rs = data.value("routines", json::array());
                    for (auto& r : rs) {
                        const std::string rid = r.value("id", "");
                        auto* rowbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                        rowbox->set_margin_top(6); rowbox->set_margin_bottom(6);
                        auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
                        text->set_hexpand(true);
                        auto* nm = Gtk::make_managed<Gtk::Label>(r.value("name", "?"));
                        nm->set_halign(Gtk::Align::START); nm->add_css_class("heading");
                        std::string sum = r.value("schedule_text", "");
                        sum += "  ·  next: " + r.value("next_fire_at", std::string("—"));
                        auto* sub = Gtk::make_managed<Gtk::Label>(sum);
                        sub->set_halign(Gtk::Align::START); sub->add_css_class("dim-label");
                        text->append(*nm); text->append(*sub);

                        auto* sw = Gtk::make_managed<Gtk::Switch>();
                        sw->set_active(r.value("enabled", true));
                        sw->set_valign(Gtk::Align::CENTER);
                        sw->property_active().signal_changed().connect(
                            [this, rid, sw] {
                                run_async([this, rid, on = sw->get_active()] {
                                    try { api_.set_routine_enabled(rid, on); }
                                    catch (...) {}
                                });
                            });
                        auto* run = Gtk::make_managed<Gtk::Button>("\u25B6");
                        run->set_tooltip_text("Run now");
                        run->set_valign(Gtk::Align::CENTER);
                        run->signal_clicked().connect([this, rid] {
                            run_async([this, rid] {
                                try { api_.run_routine(rid); } catch (...) {}
                            });
                        });
                        auto* edit = Gtk::make_managed<Gtk::Button>("Edit");
                        edit->set_valign(Gtk::Align::CENTER);
                        json rcopy = r;
                        edit->signal_clicked().connect([this, rcopy, reload] {
                            auto* d = Gtk::make_managed<Gtk::Window>();  // dummy anchor ref
                            (void)d;
                            show_task_dialog("", &rcopy);
                        });
                        auto* del = Gtk::make_managed<Gtk::Button>("\u2715");
                        del->add_css_class("destructive-action");
                        del->set_valign(Gtk::Align::CENTER);
                        del->signal_clicked().connect([this, rid, reload] {
                            run_async([this, rid, reload] {
                                try { api_.delete_routine(rid); } catch (...) {}
                                std::lock_guard<std::mutex> lk(queue_m_);
                                done_queue_.push_back([reload] { if (*reload) (*reload)(); });
                                dispatcher_.emit();
                            });
                        });
                        rowbox->append(*text); rowbox->append(*sw);
                        rowbox->append(*run); rowbox->append(*edit); rowbox->append(*del);
                        list->append(*rowbox);
                    }
                    status->set_text(std::to_string(rs.size()) + " routines");
                });
            }
            dispatcher_.emit();
        });
    };
    (*reload)();
    win->present();
}

void HiveWindow::on_new_agent() {
    show_agent_dialog(nullptr);
}

// Shared create/edit dialog. existing == nullptr → create mode; otherwise the
// dialog is prefilled from the roster agent and OK PATCHes /api/agents/:id.
void HiveWindow::show_agent_dialog(const nlohmann::json* existing) {
    const bool editing = existing != nullptr;
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    const char* title = editing ? "Edit agent" : "New agent";
    dialog->set_title(title);
    install_dialog_titlebar(*dialog, title);
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button(editing ? "Save" : "Create", Gtk::ResponseType::OK);
    dialog->set_default_response(Gtk::ResponseType::OK);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_margin(12);
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);

    auto* name = Gtk::make_managed<Gtk::Entry>();
    name->set_placeholder_text("Name (e.g. Nova)");
    auto* role = Gtk::make_managed<Gtk::Entry>();
    role->set_placeholder_text("Role (e.g. research assistant)");
    if (existing) {
        name->set_text(existing->value("name", ""));
        role->set_text(existing->value("role", ""));
    }
    // Phase 13: provider/model dropdowns from the server's LLMStore —
    // no more hand-typed model names.
    struct ModelEntry { std::string name, provider, label; };
    std::vector<std::string> provider_names;
    std::vector<ModelEntry>  model_entries;
    std::string default_model;
    try {
        for (auto& p : api_.providers().value("providers", nlohmann::json::array()))
            provider_names.push_back(p.value("name", ""));
        for (auto& m : api_.models()) {
            model_entries.push_back({m.value("name", ""), m.value("provider", ""),
                                     m.value("name", "") + "  (" + m.value("model", "") + ")"});
            if (m.value("is_default", false)) default_model = m.value("name", "");
        }
    } catch (...) { /* server unreachable — dropdowns stay empty */ }

    std::vector<Glib::ustring> prov_items(provider_names.begin(), provider_names.end());
    auto* provider = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create(prov_items));
    auto* model = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({}));

    // Repopulate the model list for the selected provider. First entry is
    // always "(server default)" → sends "" (server picks the default model).
    auto* model_names = new std::vector<std::string>();   // owned via dialog lifetime below
    auto refill_models = [model, model_entries, model_names, default_model](const std::string& prov) {
        std::vector<Glib::ustring> items;
        model_names->clear();
        items.push_back(default_model.empty() ? "(server default)"
                                              : "(server default: " + default_model + ")");
        model_names->push_back("");
        for (auto& me : model_entries)
            if (me.provider == prov) { items.push_back(me.label); model_names->push_back(me.name); }
        auto sl = Gtk::StringList::create(items);
        model->set_model(sl);
        model->set_selected(0);
    };
    if (!provider_names.empty()) refill_models(provider_names[0]);
    provider->property_selected().signal_changed().connect(
        [provider, provider_names, refill_models] {
            const auto sel = provider->get_selected();
            if (sel < provider_names.size()) refill_models(provider_names[sel]);
        });
    // Edit mode: preselect the agent's current model + its provider.
    if (editing) {
        const std::string cur = existing->value("model", "");
        if (!cur.empty())
            for (size_t i = 0; i < provider_names.size(); ++i) {
                bool here = false;
                for (auto& me : model_entries)
                    if (me.name == cur && me.provider == provider_names[i]) { here = true; break; }
                if (!here) continue;
                provider->set_selected(i);   // fires the refill above
                for (size_t j = 0; j < model_names->size(); ++j)
                    if ((*model_names)[j] == cur) { model->set_selected(j); break; }
                break;
            }
    }
    // Host mode removed (2026-08-25): agents always run containerized.
    auto* appr = Gtk::make_managed<Gtk::CheckButton>("Require my approval for commands");
    if (existing) {
        appr->set_active(existing->value("require_approval", false));
    }

    // Avatar picker: grid of the bundled hive-bot SVGs. "Auto" (default)
    // keeps the name-hash palette color. ToggleButton group = radio behavior.
    auto* avatar_grid = Gtk::make_managed<Gtk::Grid>();
    avatar_grid->set_column_spacing(6);
    avatar_grid->set_row_spacing(6);
    auto* chosen_avatar = new std::string(
        existing ? existing->value("avatar", "") : "");   // freed in response lambda
    auto* avatar_tiles = new std::vector<
        std::pair<std::string, Gtk::ToggleButton*>>();    // freed likewise
    Gtk::ToggleButton* group_leader = nullptr;
    auto add_tile = [&](const std::string& val, Gtk::Widget* content, int pos) {
        auto* b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_child(*content);
        b->add_css_class("avatar-pick");
        avatar_tiles->push_back({val, b});
        if (!group_leader) { group_leader = b; if (chosen_avatar->empty()) b->set_active(true); }
        else { b->set_group(*group_leader); if (val == *chosen_avatar) b->set_active(true); }
        b->signal_toggled().connect([b, chosen_avatar, val] {
            if (b->get_active()) *chosen_avatar = val;
        });
        avatar_grid->attach(*b, pos % 4, pos / 4);
    };
    {
        auto* lbl = Gtk::make_managed<Gtk::Label>("Auto");
        lbl->add_css_class("avatar-pick-auto");
        add_tile("", lbl, 0);
    }
    for (int i = 0; i < 6; ++i) {
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set(avatar_pixbuf("avatar-" + std::to_string(i), 64));
        img->set_pixel_size(32);
        add_tile("avatar-" + std::to_string(i), img, i + 1);
    }

    // Phase 19: teammate templates — pick one to pre-fill the form.
    auto* templates_json = new nlohmann::json(nlohmann::json::array());
    if (!editing) {
        try { *templates_json =
                  api_.templates().value("templates", nlohmann::json::array()); }
        catch (...) { /* offline — dropdown stays "(blank)" */ }
    }
    std::vector<Glib::ustring> tpl_items{"(blank)"};
    for (auto& t : *templates_json)
        tpl_items.push_back(t.value("name", "") + "  —  template");
    auto* tpl = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create(tpl_items));
    tpl->property_selected().signal_changed().connect(
        [tpl, templates_json, name, role, appr, chosen_avatar, avatar_tiles] {
            const auto sel = tpl->get_selected();
            if (sel == 0 || sel > templates_json->size()) return;
            const auto& t = (*templates_json)[sel - 1];
            name->set_text(t.value("name", ""));
            role->set_text(t.value("role", ""));
            appr->set_active(t.value("require_approval", false));
            const std::string av = t.value("avatar", "");
            *chosen_avatar = av;
            for (auto& pr : *avatar_tiles)
                if (pr.first == av) { pr.second->set_active(true); break; }
        });

    auto add_row = [grid](const char* label, Gtk::Widget* w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::END);
        grid->attach(*l, 0, row);
        w->set_hexpand(true);
        grid->attach(*w, 1, row);
    };
    int row_i = 0;
    if (!editing) add_row("Template", tpl, row_i++);
    add_row("Name", name, row_i++);
    add_row("Role", role, row_i++);
    add_row("Provider", provider, row_i++);
    add_row("Model", model, row_i++);
    // (no Workspace row — always containerized)
    add_row("Avatar", avatar_grid, row_i++);
    grid->attach(*appr, 1, row_i);

    dialog->get_content_area()->append(*grid);
    const std::string edit_id = editing ? existing->value("id", "") : "";
    dialog->signal_response().connect([this, dialog, name, role, model, model_names, appr, chosen_avatar, edit_id, templates_json, avatar_tiles](int r) {
        if (r == (int)Gtk::ResponseType::OK) {
            std::string n = name->get_text();
            if (!n.empty()) {
                std::string ws = "shared";
                const auto msel = model->get_selected();
                std::string m = msel < model_names->size() ? (*model_names)[msel] : "";
                const std::string av = *chosen_avatar;
                delete chosen_avatar;
                delete model_names;
                delete templates_json;
                delete avatar_tiles;
                run_async([this, n, r2 = role->get_text().raw(),
                           m, ws, av, edit_id,
                           ap = appr->get_active()] {
                    if (edit_id.empty())
                        api_.create_agent(n, r2, m, ws, ap, av);
                    else
                        api_.update_agent(edit_id, n, r2, m, ws, ap, av);
                    {
                        std::lock_guard<std::mutex> lk(queue_m_);
                        done_queue_.push_back([this] { if (alive_) refresh_all(); });
                    }
                    dispatcher_.emit();
                });
            }
        }
        if (r != (int)Gtk::ResponseType::OK) {
            delete chosen_avatar;
            delete templates_json;
            delete avatar_tiles;
        }
        dialog->destroy();
    });
    dialog->present();
}

void HiveWindow::on_new_thread() {
    if (agents_.empty()) { set_status("Create an agent first"); return; }
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("New thread");
    install_dialog_titlebar(*dialog, "New thread");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    auto* create_btn = dialog->add_button("Create", Gtk::ResponseType::OK);
    create_btn->add_css_class("suggested-action");
    create_btn->set_sensitive(false);   // needs >= 1 participant

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_margin(12);
    box->set_spacing(8);
    auto* title = Gtk::make_managed<Gtk::Entry>();
    title->set_placeholder_text("Title (optional)");
    box->append(*title);
    box->append(*Gtk::make_managed<Gtk::Label>("Participants:"));

    auto* err = Gtk::make_managed<Gtk::Label>("Select at least one agent");
    err->add_css_class("error");
    err->set_visible(false);
    box->append(*err);

    std::vector<std::pair<Gtk::CheckButton*, std::string>> picks;
    for (auto& a : agents_) {
        auto* cb = Gtk::make_managed<Gtk::CheckButton>(a.value("name", "?"));
        box->append(*cb);
        picks.emplace_back(cb, a.value("id", ""));
    }
    // Enable Create only once at least one participant is checked.
    for (auto& [cb, id] : picks)
        // NB: capture picks BY VALUE — on_new_thread returns right after
        // present(), so a reference capture dangles the moment a checkbox
        // toggles (use-after-scope crash).
        cb->signal_toggled().connect([create_btn, err, picks] {
            bool any = false;
            for (auto& [b, i] : picks) any = any || b->get_active();
            create_btn->set_sensitive(any);
            if (any) err->set_visible(false);
        });
    dialog->get_content_area()->append(*box);
    dialog->signal_response().connect([this, dialog, title, picks](int r) {
        if (r == (int)Gtk::ResponseType::OK) {
            std::vector<std::string> ids;
            for (auto& [cb, id] : picks)
                if (cb->get_active()) ids.push_back(id);
            if (!ids.empty()) {
                run_async([this, t = title->get_text().raw(), ids] {
                    json nt = api_.create_thread(t, ids);
                    {
                        std::lock_guard<std::mutex> lk(queue_m_);
                        done_queue_.push_back([this, nt] {
                            if (!alive_) return;
                            current_thread_ = nt.value("id", "");
                            last_message_id_.clear();
                            while (auto* c = messages_box_.get_first_child())
                                messages_box_.remove(*c);
                            rendered_.clear();
                            refresh_threads();
                        });
                    }
                    dispatcher_.emit();
                });
            }
        }
        dialog->destroy();
    });
    dialog->present();
}

void HiveWindow::on_stop() {
    if (current_thread_.empty()) return;
    std::string tid = current_thread_;
    stop_btn_.set_sensitive(false);
    set_status("Cancelling…");
    run_async([this, tid] {
        api_.cancel_thread(tid);
        std::lock_guard<std::mutex> lk(queue_m_);
        done_queue_.push_back([this] {
            if (alive_) set_status("Cancel requested.");
        });
        dispatcher_.emit();
    });
}

void HiveWindow::on_delete_thread() {
    if (current_thread_.empty()) return;
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Delete thread?");
    install_dialog_titlebar(*dialog, "Delete thread?");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    auto* del = dialog->add_button("Delete", Gtk::ResponseType::OK);
    del->add_css_class("destructive-action");
    auto* lbl = Gtk::make_managed<Gtk::Label>(
        "Delete this thread and all its messages? This cannot be undone.");
    lbl->set_margin(18);
    lbl->set_wrap(true);
    lbl->set_max_width_chars(42);
    dialog->get_content_area()->append(*lbl);
    std::string tid = current_thread_;
    dialog->signal_response().connect([this, dialog, tid](int r) {
        dialog->destroy();
        if (r != Gtk::ResponseType::OK) return;
        run_async([this, tid] {
            api_.delete_thread(tid);
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, tid] {
                if (!alive_) return;
                if (current_thread_ == tid) {
                    current_thread_.clear();
                    last_message_id_.clear();
                    delete_thread_btn_.set_sensitive(false);
                    learn_btn_.set_sensitive(false);
                    while (auto* ch = messages_box_.get_first_child())
                        messages_box_.remove(*ch);
                    rendered_.clear();
                }
                refresh_threads();
            });
            dispatcher_.emit();
        });
    });
    dialog->present();
}


// ── UiApi hooks (Phase 11.5) ────────────────────────────────────────────────
// Called from OmniNet worker threads; invoke_on_ui marshals onto the GTK
// main loop so widget access stays single-threaded.

nlohmann::json HiveWindow::invoke_on_ui(std::function<nlohmann::json()> fn) {
    auto p = std::make_shared<std::promise<nlohmann::json>>();
    auto fut = p->get_future();
    {
        std::lock_guard<std::mutex> lk(queue_m_);
        done_queue_.push_back([this, fn = std::move(fn), p]() mutable {
            if (!alive_) { p->set_value({{"error", "window closed"}}); return; }
            try { p->set_value(fn()); }
            catch (const std::exception& e) { p->set_value({{"error", e.what()}}); }
        });
    }
    dispatcher_.emit();
    return fut.get();
}

nlohmann::json HiveWindow::ui_state() {
    return invoke_on_ui([this] {
        return nlohmann::json{
            {"current_thread", current_thread_},
            {"busy", thread_busy_},
            {"pending_approvals", pending_approvals_},
            {"status", std::string{status_label_.get_text()}},
            {"agents", agents_},
            {"threads", threads_},
            {"rendered_count", rendered_.size()},
            {"connected", !api_.api_key().empty()},
        };
    });
}

nlohmann::json HiveWindow::ui_messages() {
    // note: json{vec} would wrap in an extra array — copy-construct instead
    return invoke_on_ui([this] { return nlohmann::json(rendered_); });
}

nlohmann::json HiveWindow::ui_create_thread(const std::string& title,
                                            const std::vector<std::string>& ids) {
    // Network call off the GTK thread, UI update marshalled — same code as
    // the New-thread dialog's OK handler.
    json nt = api_.create_thread(title, ids, /*reuse_dm=*/false);
    std::string new_id = nt.value("id", "");
    return invoke_on_ui([this, nt, new_id] {
        current_thread_ = new_id;
        last_message_id_.clear();
        while (auto* c = messages_box_.get_first_child())
            messages_box_.remove(*c);
        rendered_.clear();
        refresh_threads();
        return nt;
    });
}

nlohmann::json HiveWindow::ui_select_thread(const std::string& id) {
    return invoke_on_ui([this, id] {
        for (auto& t : threads_) {
            if (t.value("id", "") == id) {
                current_thread_ = id;
                last_message_id_.clear();
                delete_thread_btn_.set_sensitive(true);
                while (auto* c = messages_box_.get_first_child())
                    messages_box_.remove(*c);
                rendered_.clear();
                poll_chat();
                return nlohmann::json{{"selected", id}};
            }
        }
        return nlohmann::json{{"error", "thread not found: " + id}};
    });
}

nlohmann::json HiveWindow::ui_send(const std::string& body) {
    return invoke_on_ui([this, body] {
        if (current_thread_.empty())
            return nlohmann::json{{"error", "no thread selected"}};
        entry_.set_text(body);
        on_send();   // exact button path: reads entry_, clears, posts async
        return nlohmann::json{{"sent", true}, {"thread", current_thread_}};
    });
}

nlohmann::json HiveWindow::ui_stop() {
    return invoke_on_ui([this] {
        if (current_thread_.empty())
            return nlohmann::json{{"error", "no thread selected"}};
        on_stop();   // exact button path
        return nlohmann::json{{"cancel_requested", true}};
    });
}

nlohmann::json HiveWindow::ui_delete_thread(const std::string& id) {
    // Confirm dialog is UI chrome; the API path performs the same delete +
    // view cleanup the dialog's OK response does.
    api_.delete_thread(id);
    return invoke_on_ui([this, id] {
        if (current_thread_ == id) {
            current_thread_.clear();
            last_message_id_.clear();
            delete_thread_btn_.set_sensitive(false);
            while (auto* ch = messages_box_.get_first_child())
                messages_box_.remove(*ch);
            rendered_.clear();
        }
        refresh_threads();
        return nlohmann::json{{"deleted", true}, {"id", id}};
    });
}

void HiveWindow::on_approvals() {
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Approvals");
    install_dialog_titlebar(*dialog, "Approvals");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->set_default_size(560, 360);
    dialog->add_button("Close", Gtk::ResponseType::CLOSE);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    list->set_margin(8);
    list->set_spacing(6);
    scroll->set_child(*list);
    dialog->get_content_area()->append(*scroll);
    dialog->signal_response().connect([dialog](int) { dialog->destroy(); });

    // Populate async; rebuild after each resolve.
    auto* alive = new bool(true);
    dialog->signal_destroy().connect([alive] { delete alive; });

    std::function<void()> populate;
    populate = [this, dialog, list, alive, &populate] {
        run_async([this, list, alive, &populate] {
            json ap = api_.list_approvals("pending");
            {
                std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this, list, alive, ap, &populate] {
                    if (!alive_ || !*alive) return;
                    while (auto* c = list->get_first_child()) list->remove(*c);
                    auto arr = ap.value("approvals", json::array());
                    if (arr.empty())
                        list->append(*Gtk::make_managed<Gtk::Label>("No pending approvals 🎉"));
                    for (auto& a : arr) {
                        std::string id = a.value("id", "");
                        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
                        row->add_css_class("card");
                        row->set_margin(4);
                        auto* head = Gtk::make_managed<Gtk::Label>(
                            a.value("agent_name", "?") + " wants to run:");
                        head->set_halign(Gtk::Align::START);
                        head->add_css_class("heading");
                        auto* cmd = Gtk::make_managed<Gtk::Label>(a.value("payload", ""));
                        cmd->set_halign(Gtk::Align::START);
                        cmd->set_selectable(true);
                        {
            Pango::AttrList attrs;
            auto family = Pango::Attribute::create_attr_family("monospace");
            attrs.insert(family);
            cmd->set_attributes(attrs);
        }
                        auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
                        btns->set_spacing(6);
                        auto* yes = Gtk::make_managed<Gtk::Button>("Approve");
                        yes->add_css_class("suggested-action");
                        auto* no = Gtk::make_managed<Gtk::Button>("Deny");
                        no->add_css_class("destructive-action");
                        btns->append(*yes);
                        btns->append(*no);
                        row->append(*head);
                        row->append(*cmd);
                        row->append(*btns);
                        list->append(*row);

                        auto resolve = [this, id, alive, &populate](bool ok) {
                            run_async([this, id, ok] {
                                api_.resolve_approval(id, ok);
                                dispatcher_.emit();
                            });
                            Glib::signal_timeout().connect_once(
                                [&populate] { populate(); }, 400);
                        };
                        yes->signal_clicked().connect([resolve] { resolve(true); });
                        no->signal_clicked().connect([resolve] { resolve(false); });
                    }
                });
            }
            dispatcher_.emit();
        });
    };
    populate();
    dialog->present();
}

} // namespace hive
