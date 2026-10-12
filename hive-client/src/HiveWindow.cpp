#include "HiveWindow.h"
#include "text_utils.h"

#include <thread>

#ifdef HIVEMIND_WITH_WEBKIT
#include <webkit/webkit.h>
#endif
#include <giomm/memoryinputstream.h>
#include <giomm/resource.h>
#include <gdkmm/pixbuf.h>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <future>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

using nlohmann::json;

namespace hive {

using namespace text;   // factored pure helpers (unit-testable)

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
.hv-msg-live   { opacity: 0.75; font-style: italic;
                 box-shadow: inset 0 0 0 1px alpha(@accent_color, 0.35); }
.hv-sender { font-weight: 600; font-size: 0.85em;
             color: alpha(@window_fg_color, 0.65); }
.hv-section { font-weight: 700; margin: 0; padding: 7px 10px;
  background: alpha(@window_fg_color, 0.06);
  border-bottom: 1px solid alpha(@window_fg_color, 0.08); }
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

/* Settings dialog: section sidebar + content stack */
.hv-settings-sidebar { background: alpha(@window_fg_color, 0.04);
  min-width: 150px; }
.hv-settings-sidebar row { padding: 10px 14px; }
.hv-settings-sidebar row:selected { background: alpha(@accent_color, 0.18); }
.hv-settings-page { margin: 16px; }
.hv-font-scale { min-width: 220px; }

/* Context menus (right-click roster/threads) */
.hv-menu-item { padding: 6px 12px; min-width: 140px; }
.hv-menu-item label { margin: 0; }
.hv-prompt-frame { border: 1px solid alpha(@window_fg_color, 0.2);
                   border-radius: 8px; }
.hv-prompt-frame textview { padding: 6px; }
.error { color: @error_color; }
.hv-unread-dot { color: @accent_color; font-weight: bold; }
.hv-group-header { font-weight: bold; }
.hv-group-meta { opacity: 0.65; font-size: 0.9em; }
.hv-state-tag { font-size: 10px; padding: 1px 8px; border-radius: 8px;
                background: alpha(@accent_color, 0.15); color: @accent_color; }
.hv-state-ready { font-size: 10px; padding: 1px 8px; border-radius: 8px;
  background: alpha(@success_color, 0.18); color: @success_color; }
.hv-state-down { font-size: 10px; padding: 1px 8px; border-radius: 8px;
  background: alpha(@window_fg_color, 0.08); color: alpha(@window_fg_color, 0.5); }
/* Worker group headers in the roster (chevron + name + online pill) */
.hv-worker-header { font-weight: 600; font-size: 0.85em;
  color: alpha(@window_fg_color, 0.75); }
.hv-worker-chevron { color: alpha(@window_fg_color, 0.5);
  font-size: 0.8em; min-width: 14px; }
.hv-worker-offline { opacity: 0.55; }
/* Agents homed on an offline worker — visible but clearly inert */
.hv-offline { opacity: 0.45; }
)";



// ── chat message rendering ──────────────────────────────────────────────────
// Agent message bodies arrive as hive-brain turn transcripts: natural language
// interleaved with "[tool: name args…]" call lines and "  → {json}" result
// lines. Strip the transcript plumbing for display; the raw body can still be
// dumped to a debug log (HIVEMIND_DEBUG=1 → ~/.local/share/hivemind/client-raw.log).



// ── GResource SVG icons ───────────────────────────────────────────────────
// Icons ship as currentColor-stroked SVGs bundled into the binary. GTK's
// pixbuf loader doesn't resolve currentColor from CSS, so we substitute the
// theme foreground explicitly — this is what makes icons follow dark/light
// mode. Avatars carry their own identity colors and load verbatim.
extern "C" { GResource* hivemind_get_resource(void); }

static std::string resource_text(const std::string& path) {
    // C API with nullptr GError: returns NULL on a missing resource and
    // NEVER throws. glibmm's lookup_data_global RAISES Glib::Error on a
    // miss — and a throw inside a GTK signal handler kills the callback
    // (2026-09-14: a stale stored avatar name aborted the roster rebuild
    // mid-render and left the Agents list EMPTY).
    GBytes* bytes = g_resources_lookup_data(
        path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes) return {};
    gsize sz = 0;
    const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &sz));
    std::string out{data, sz};
    g_bytes_unref(bytes);
    return out;
}

static Glib::RefPtr<Gdk::Pixbuf> svg_pixbuf(std::string svg, int size) {
    // A corrupt/missing avatar must NEVER crash the app: render a
    // transparent square when the input is empty or unparseable.
    static const char* kBlank =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 1 1'/>";
    if (svg.empty()) svg = kBlank;
    auto stream = Gio::MemoryInputStream::create();
    stream->add_bytes(Glib::Bytes::create(svg.data(), svg.size()));
    try {
        return Gdk::Pixbuf::create_from_stream_at_scale(stream, size, size,
                                                        true);
    } catch (const Glib::Error&) {
        auto retry = Gio::MemoryInputStream::create();
        retry->add_bytes(Glib::Bytes::create(kBlank, strlen(kBlank)));
        try {
            return Gdk::Pixbuf::create_from_stream_at_scale(retry, size,
                                                            size, true);
        } catch (...) {
            return Glib::RefPtr<Gdk::Pixbuf>();   // image clears; no crash
        }
    }
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


// Hive-garden avatar set (data/avatars): 20 cartoon creatures on hexagon
// badges — five bees first (they are the HiveMind brand), then the garden
// friends. Ordered so picker grids open with the bees.
const std::vector<std::string>& avatar_names() {
    static const std::vector<std::string> names = {
        "queen-bee", "worker-bee", "drone-bee", "honey-bee", "bumblebee",
        "ladybug", "butterfly", "blue-butterfly", "dragonfly", "firefly",
        "caterpillar", "snail", "ant", "beetle", "spider",
        "moth", "grasshopper", "worm", "mantis", "cricket",
    };
    return names;
}

// "Auto" avatar choice: stable hash of the agent name into the dept set, so
// an uncustomized agent keeps the same face across roster and bubbles.
const std::string& auto_avatar(const std::string& name) {
    return avatar_names()[std::hash<std::string>{}(name) % avatar_names().size()];
}

// Department-head avatar from the bundled SVG set. auto_avatar keeps an
// agent's face consistent across the roster and every bubble; the user gets
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
    // Chosen avatar wins; "" = user silhouette for the human, stable
    // name-hash into the set for agents. Fallback chain for STALE choices
    // (stored avatar names from a replaced set, e.g. old department heads
    // after the hive-garden redesign): missing resource -> auto hash ->
    // user silhouette. Never render nothing — and never crash.
    std::string res = avatar.empty()
        ? (is_user ? "user" : auto_avatar(name))
        : avatar;
    if (resource_text("/com/hivemind/client/avatars/" + res + ".svg").empty())
        res = is_user ? "user" : auto_avatar(name);
    if (resource_text("/com/hivemind/client/avatars/" + res + ".svg").empty())
        res = "user";
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
    slot(settings_btn_,      "settings",   "Settings — your display name & avatar");
    slot(send_btn_,          "send",       "Send message");


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
}

nlohmann::json HiveWindow::ui_open_dialog(const std::string& name) {
    return invoke_on_ui([this, name] {
        if (name == "new-thread")      on_new_thread();
        else if (name == "new-agent")  on_new_agent();
        else if (name == "settings")   on_settings();
        else if (name == "approvals")  on_approvals();
        else return json{{"error", "unknown dialog"}};
        return json{{"opened", name}};
    });
}

// Phase 37: the client runs on the HOST desktop now (not in a podman
// container), so the in-app Dark/Light switch is gone — appearance
// follows GNOME's system style. Ubuntu drives dark via TWO keys, so
// read BOTH: color-scheme == 'prefer-dark' (GNOME standard) OR
// gtk-theme ending in '-dark' (Ubuntu's Appearance panel flips
// 'Yaru' ↔ 'Yaru-dark' and leaves color-scheme at 'default').
// Non-GNOME desktops without the schema keep GTK's default.
void HiveWindow::follow_system_theme() {
    auto src = Gio::SettingsSchemaSource::get_default();
    const std::string kIfaceSchema = "org.gnome.desktop.interface";
    if (!src || !src->lookup(kIfaceSchema, true)) return;
    iface_settings_ = Gio::Settings::create(kIfaceSchema);
    auto apply_from_gnome = [this] {
        const bool cs_dark =
            iface_settings_->get_string("color-scheme") == "prefer-dark";
        std::string gt = iface_settings_->get_string("gtk-theme");
        for (auto& c : gt) c = (char)std::tolower((unsigned char)c);
        const bool theme_dark =
            gt.size() > 5 && gt.compare(gt.size() - 5, 5, "-dark") == 0;
        const bool dark = cs_dark || theme_dark;
        if (dark != dark_mode_) {
            dark_mode_ = dark;
            apply_theme();
        }
    };
    iface_settings_->signal_changed("color-scheme")
        .connect([this, apply_from_gnome](const Glib::ustring&) {
            apply_from_gnome();
        });
    iface_settings_->signal_changed("gtk-theme")
        .connect([this, apply_from_gnome](const Glib::ustring&) {
            apply_from_gnome();
        });
    apply_from_gnome();   // initial state
}

void HiveWindow::apply_theme() {
    auto settings = Gtk::Settings::get_default();
    settings->property_gtk_application_prefer_dark_theme()
        .set_value(dark_mode_);
    // Ubuntu's Yaru ships dark as a SEPARATE theme name (gtk-theme-name =
    // "Yaru-dark" when GNOME Appearance = Dark). prefer-dark=false alone
    // cannot reach light from there — the dark theme has no light variant.
    // Going light: strip a "-dark" suffix so the base theme loads.
    // GtkSettings is process-local; no other app is affected.
    std::string theme = settings->property_gtk_theme_name().get_value();
    const bool has_dark_suffix =
        theme.size() > 5 && theme.compare(theme.size() - 5, 5, "-dark") == 0;
    if (!dark_mode_ && has_dark_suffix) {
        // Going light: strip "-dark" so the base (light-capable) theme loads.
        theme.resize(theme.size() - 5);
        settings->property_gtk_theme_name().set_value(theme);
    } else if (dark_mode_ && !has_dark_suffix && theme == "Yaru") {
        // Going dark: Yaru ships dark as a separate theme name.
        settings->property_gtk_theme_name().set_value("Yaru-dark");
    }
    refresh_themed_icons();
}

// Font scale slider → gtk-xft-dpi (1/1024 pt units). 1.0 maps to -1 =
// "follow the system", so the default always respects the desktop's own
// DPI/scaling; other values override it app-locally.
// GTK4 gotcha: the adjustment "changed" signal fires DURING the viewport's
// size-allocation pass. Setting value synchronously there updates the
// scrollbar but leaves the viewport's child allocation stale (chat looks
// frozen at the old position until the user scrolls). Deferring to idle —
// after the frame's layout completes — makes the viewport actually move.
void HiveWindow::scroll_chat_to_bottom() {
    if (chat_scroll_idle_) return;   // coalesce bursts of "changed" signals
    chat_scroll_idle_ = true;
    Glib::signal_idle().connect_once([this] {
        chat_scroll_idle_ = false;
        if (!chat_stick_bottom_) return;
        auto a = chat_scroll_.get_vadjustment();
        chat_scroll_prog_ = true;   // don't let our own scroll flip the sticky flag
        a->set_value(a->get_upper() - a->get_page_size());
        Glib::signal_idle().connect_once([this] { chat_scroll_prog_ = false; });
    });
}

void HiveWindow::apply_font_scale() {
    const int dpi = font_scale_ == 1.0
        ? -1 : (int)std::lround(96.0 * 1024 * font_scale_);
    Gtk::Settings::get_default()->property_gtk_xft_dpi().set_value(dpi);
}

// Settings — classic two-pane dialog: section sidebar on the left
// (Interface / User), settings stack on the right. Interface switches apply
// live; User identity saves with the Save button. Add a row to a page (or a
// sidebar row + stack page) per future preference.
void HiveWindow::on_settings() {
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Settings");
    install_dialog_titlebar(*dialog, "Settings");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->set_default_size(620, 480);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button("Save", Gtk::ResponseType::OK);
    dialog->set_default_response(Gtk::ResponseType::OK);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* sidebar = Gtk::make_managed<Gtk::ListBox>();
    sidebar->add_css_class("hv-settings-sidebar");
    auto* stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_hexpand(true);
    stack->set_vexpand(true);
    stack->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    body->append(*sidebar);
    body->append(*sep);
    body->append(*stack);

    auto add_section = [&](const char* id, const char* label) {
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::START);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*l);
        row->set_data("page", (void*)id);
        sidebar->append(*row);
    };
    add_section("interface", "🖥  Interface");
    add_section("user", "👤  User");
    sidebar->signal_row_selected().connect([stack](Gtk::ListBoxRow* row) {
        if (row) stack->set_visible_child(
            (const char*)row->get_data("page"));
    });

    // ── Interface page ──────────────────────────────────────────────────
    auto* iface = Gtk::make_managed<Gtk::Grid>();
    iface->add_css_class("hv-settings-page");
    iface->set_row_spacing(14);
    iface->set_column_spacing(12);

    // Phase 37: no in-app theme switch — the app follows the GNOME
    // system style (Settings → Appearance). Note only.
    auto* dark_lbl = Gtk::make_managed<Gtk::Label>("Appearance");
    dark_lbl->set_halign(Gtk::Align::END);
    auto* dark_note = Gtk::make_managed<Gtk::Label>(
        "Follows the GNOME system style (Settings → Appearance)");
    dark_note->set_halign(Gtk::Align::START);
    dark_note->add_css_class("hv-time");
    iface->attach(*dark_lbl, 0, 0);
    iface->attach(*dark_note, 1, 0);

    auto* fs_lbl = Gtk::make_managed<Gtk::Label>("Font scale");
    fs_lbl->set_halign(Gtk::Align::END);
    auto* fs = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    fs->add_css_class("hv-font-scale");
    fs->set_range(0.75, 1.75);
    fs->set_increments(0.05, 0.25);
    fs->add_mark(1.0, Gtk::PositionType::BOTTOM, "100%");
    fs->set_value(font_scale_);
    fs->set_tooltip_text("1.00 follows the system DPI/scaling");
    fs->signal_value_changed().connect([this, fs] {
        font_scale_ = fs->get_value();
        apply_font_scale();
        save_config();
    });
    iface->attach(*fs_lbl, 0, 1);
    iface->attach(*fs, 1, 1);

    auto* srv_lbl = Gtk::make_managed<Gtk::Label>("Server");
    srv_lbl->set_halign(Gtk::Align::END);
    auto* srv = Gtk::make_managed<Gtk::Label>(server_url_);
    srv->set_halign(Gtk::Align::START);
    srv->add_css_class("dim-label");
    iface->attach(*srv_lbl, 0, 2);
    iface->attach(*srv, 1, 2);
    stack->add(*iface, "interface");

    // ── User page ───────────────────────────────────────────────────────
    auto* user = Gtk::make_managed<Gtk::Grid>();
    user->add_css_class("hv-settings-page");
    user->set_row_spacing(14);
    user->set_column_spacing(12);

    auto* name = Gtk::make_managed<Gtk::Entry>();
    name->set_placeholder_text("You");
    name->set_text(display_name_);

    // Avatar picker: default user silhouette tile, then the 20 dept heads.
    auto* avatar_grid = Gtk::make_managed<Gtk::Grid>();
    avatar_grid->set_column_spacing(6);
    avatar_grid->set_row_spacing(6);
    auto* chosen = new std::string(user_avatar_);          // freed in response lambda
    auto* tiles = new std::vector<
        std::pair<std::string, Gtk::ToggleButton*>>();     // freed likewise
    Gtk::ToggleButton* leader = nullptr;
    auto add_tile = [&](const std::string& val, Gtk::Widget* content,
                        const std::string& tip, int col, int row) {
        auto* b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_child(*content);
        b->add_css_class("avatar-pick");
        b->set_tooltip_text(tip);
        tiles->push_back({val, b});
        if (!leader) { leader = b; if (chosen->empty()) b->set_active(true); }
        else { b->set_group(*leader); if (val == *chosen) b->set_active(true); }
        b->signal_toggled().connect([b, chosen, val] {
            if (b->get_active()) *chosen = val;
        });
        avatar_grid->attach(*b, col, row);
    };
    {
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set(avatar_pixbuf("user", 64));
        img->set_pixel_size(32);
        add_tile("", img, "Default user avatar", 0, 0);
    }
    const auto& avatars = avatar_names();
    for (size_t i = 0; i < avatars.size(); ++i) {
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set(avatar_pixbuf(avatars[i], 64));
        img->set_pixel_size(32);
        add_tile(avatars[i], img, avatars[i], (int)((i + 1) % 5), (int)((i + 1) / 5));
    }

    auto* name_lbl = Gtk::make_managed<Gtk::Label>("Display name");
    name_lbl->set_halign(Gtk::Align::END);
    user->attach(*name_lbl, 0, 0);
    name->set_hexpand(true);
    user->attach(*name, 1, 0);
    auto* av_lbl = Gtk::make_managed<Gtk::Label>("Avatar");
    av_lbl->set_halign(Gtk::Align::END);
    av_lbl->set_valign(Gtk::Align::START);
    user->attach(*av_lbl, 0, 1);
    user->attach(*avatar_grid, 1, 1);
    stack->add(*user, "user");

    if (auto* r0 = sidebar->get_row_at_index(0)) sidebar->select_row(*r0);

    dialog->get_content_area()->append(*body);
    dialog->signal_response().connect(
        [this, dialog, name, chosen, tiles](int r) {
            if (r == (int)Gtk::ResponseType::OK) {
                display_name_ = name->get_text();
                user_avatar_  = *chosen;
                save_config();
                // Re-render the open thread so the new identity shows now.
                if (!current_thread_.empty()) {
                    last_message_id_.clear();
                    rendered_msg_ids_.clear();
                    while (auto* c = messages_box_.get_first_child())
                        messages_box_.remove(*c);
                        live_row_ = nullptr; live_label_ = nullptr; live_agent_key_.clear();
                        load_earlier_btn_ = nullptr; has_more_history_ = false;
                        oldest_message_id_.clear(); loading_earlier_ = false;
                    rendered_.clear();
                    poll_chat();
                }
            }
            delete chosen;
            delete tiles;
            dialog->destroy();
        });
    dialog->present();
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
    settings_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_settings));
    learn_btn_.set_label("🎓 Learn");
    learn_btn_.set_tooltip_text(
        "Learn-by-demonstration: the agent distills this thread's workflow "
        "into a scheduled routine (save_routine)");
    learn_btn_.signal_clicked().connect([this] { on_learn_routine(); });
    learn_btn_.set_sensitive(false);   // enabled with a thread selection
    header_.pack_start(new_agent_btn_);
    header_.pack_start(new_thread_btn_);
    header_.pack_start(tasks_btn_);
    header_.pack_end(connect_btn_);
    header_.pack_end(settings_btn_);
    header_.pack_end(learn_btn_);
    header_.pack_end(approvals_btn_);
    // CSD: install our HeaderBar AS the window titlebar so the compositor
    // doesn't draw a second (server-side) title bar above it.
    set_titlebar(header_);

    // ── sidebar ───────────────────────────────────────────────────────────
    // Section headers are full-width darker bands (CSS above) so they read
    // as titles, not list items. xalign keeps the text left while the band
    // spans the panel.
    roster_label_.add_css_class("hv-section");
    roster_label_.set_xalign(0.0f);
    threads_label_.add_css_class("hv-section");
    threads_label_.set_xalign(0.0f);

    roster_scroll_.set_child(roster_list_);
    roster_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    roster_scroll_.set_size_request(240, 120);
    roster_scroll_.set_vexpand(true);
    threads_scroll_.set_child(threads_list_);
    threads_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    threads_scroll_.set_vexpand(true);

    roster_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    threads_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    roster_list_.signal_row_activated().connect(
        [this](Gtk::ListBoxRow*) { on_agent_selected(); });
    roster_list_.signal_row_selected().connect(
        [this](Gtk::ListBoxRow* row) {
            if (restoring_selection_ || !row) return;
            on_agent_selected();
        });
    threads_list_.signal_row_selected().connect(
        [this](Gtk::ListBoxRow*) { on_thread_selected(); });

    sidebar_.add_css_class("hv-sidebar");
    // Agents / Threads split: draggable vertical paned (neither pane may
    // shrink away), position persisted as "sidebar_split" in config.json.
    auto* agents_panel = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    agents_panel->append(roster_label_);
    agents_panel->append(roster_scroll_);
    auto* threads_panel = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    threads_panel->append(threads_label_);
    threads_panel->append(threads_scroll_);
    auto* split = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::VERTICAL);
    split->set_start_child(*agents_panel);
    split->set_end_child(*threads_panel);
    split->set_shrink_start_child(false);
    split->set_shrink_end_child(false);
    split->set_vexpand(true);
    split->set_wide_handle(true);
    split->property_position().signal_changed().connect([this, split] {
        sidebar_split_ = split->get_position();
        if (split_save_pending_ || !alive_) return;
        split_save_pending_ = true;
        Glib::signal_timeout().connect_once([this] {
            split_save_pending_ = false;
            if (alive_) save_config();
        }, 800);
    });
    Glib::signal_idle().connect_once(
        [split, this] { split->set_position(sidebar_split_); });
    sidebar_.append(*split);

    // ── chat area ─────────────────────────────────────────────────────────
    status_label_.set_halign(Gtk::Align::START);
    status_label_.set_margin(6);
    status_label_.set_selectable(true);   // Phase 19: error ids are copyable
    chat_scroll_.set_child(messages_box_);
    chat_scroll_.set_vexpand(true);
    // Sticky-bottom chat: auto-scroll to the newest message whenever content
    // grows (thread opened, message arrives) UNLESS the user has scrolled
    // up to read; scrolling back to within ~10px of the bottom re-engages.
    {
        auto adj = chat_scroll_.get_vadjustment();
        adj->signal_changed().connect([this] {   // upper/page: new content or resize
            if (chat_stick_bottom_ && !chat_scroll_prog_)
                scroll_chat_to_bottom();
        });
        adj->property_value().signal_changed().connect([this] {
            if (chat_scroll_prog_) return;   // our own auto-scroll, not the user
            auto a = chat_scroll_.get_vadjustment();
            chat_stick_bottom_ =
                a->get_value() >= a->get_upper() - a->get_page_size() - 10.0;
        });
    }
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
    paned_.set_position(main_split_);
    paned_.set_vexpand(true);
    // Persist the sidebar/chat split exactly like the Agents/Threads one
    // (debounced — drags fire position-changed continuously).
    paned_.property_position().signal_changed().connect([this] {
        main_split_ = paned_.get_position();
        if (split_save_pending_ || !alive_) return;
        split_save_pending_ = true;
        Glib::signal_timeout().connect_once([this] {
            split_save_pending_ = false;
            if (alive_) save_config();
        }, 800);
    });
    root_.append(paned_);
    set_child(root_);

    // ── async plumbing ────────────────────────────────────────────────────
    dispatcher_.connect([this] {
        std::vector<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            jobs.swap(done_queue_);
        }
        for (auto& j : jobs) {
            // Per-job guard: one throwing job must not kill the rest of
            // the batch — an exception escaping a GTK handler silently
            // kills the callback (2026-09-14 empty-roster incident).
            try { j(); }
            catch (const std::exception& e) {
                std::fprintf(stderr, "[hive-client] UI job threw: %s\n",
                             e.what());
            } catch (...) {
                std::fprintf(stderr, "[hive-client] UI job threw (unknown)\n");
            }
        }
    });

    // 1.5 s poll: chat stream + approvals badge.
    poll_timer_ = Glib::signal_timeout().connect([this] {
        // While the connect dialog is up, every request would 401 and (pre-fix)
        // spawn yet another modal dialog. Skip polling until we're connected.
        if (connect_dialog_ || api_.api_key().empty()) return alive_;
        try {
            poll_chat();
            poll_approvals();
            poll_roster();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[hive-client] poll threw: %s\n", e.what());
        } catch (...) {}
        return alive_;
    }, 1500);

    // 10 s thread-list poll: pair threads created by agents messaging
    // each other (and activity badges on existing threads) appear without
    // waiting for a user action. refresh_threads() is signature-gated,
    // so this only rebuilds the list when something actually changed.
    threads_timer_ = Glib::signal_timeout().connect([this] {
        if (connect_dialog_ || api_.api_key().empty()) return alive_;
        try { refresh_threads(); }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[hive-client] threads poll threw: %s\n",
                         e.what());
        } catch (...) {}
        return alive_;
    }, 10000);

    load_config();
    // Restore persisted geometry: window size + sidebar/chat split. The split
    // is applied from idle — a Paned ignores set_position before it has an
    // allocation (same trick as the Agents/Threads split above).
    set_default_size(window_w_, window_h_);
    Glib::signal_idle().connect_once(
        [this] { paned_.set_position(main_split_); });
    // Track user resizes (debounced persist). GTK4 mirrors user resizes into
    // default-width/height; skip while maximized so restoring never bakes in
    // the fullscreen dimensions.
    auto on_resize = [this] {
        if (is_maximized()) return;
        window_w_ = property_default_width().get_value();
        window_h_ = property_default_height().get_value();
        if (split_save_pending_ || !alive_) return;
        split_save_pending_ = true;
        Glib::signal_timeout().connect_once([this] {
            split_save_pending_ = false;
            if (alive_) save_config();
        }, 800);
    };
    property_default_width().signal_changed().connect(on_resize);
    property_default_height().signal_changed().connect(on_resize);
    // Snapshot everything one last time on the way out.
    signal_close_request().connect([this] {
        if (!is_maximized() && get_width() > 0 && get_height() > 0) {
            window_w_ = get_width();
            window_h_ = get_height();
        }
        main_split_ = paned_.get_position();
        save_config();
        return false;   // let the close proceed
    }, false);
    follow_system_theme();   // Phase 37: track GNOME Dark/Light
    setup_icons();   // persistent icon structure, once
    apply_theme();   // dark/light + initial pixbuf tint
    apply_font_scale();
    set_status("Connecting to hive-server…");
    if (api_.api_key().empty())
        maybe_show_connect_dialog();
    else
        refresh_all();
}

HiveWindow::~HiveWindow() {
    alive_ = false;
    poll_timer_.disconnect();
    threads_timer_.disconnect();
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
        display_name_ = j.value("display_name", "");
        user_avatar_  = j.value("user_avatar", "");
        font_scale_   = j.value("font_scale", 1.0);
        sidebar_split_ = j.value("sidebar_split", 320);
        main_split_   = std::clamp(j.value("main_split", 260), 120, 1200);
        window_w_     = std::clamp(j.value("window_w", 1100), 400, 7680);
        window_h_     = std::clamp(j.value("window_h", 760), 300, 4320);
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
{"display_name", display_name_},
              {"user_avatar", user_avatar_}, {"font_scale", font_scale_},
              {"sidebar_split", sidebar_split_},
              {"main_split", main_split_},
              {"window_w", window_w_}, {"window_h", window_h_}}.dump(2);
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
        json workers = json::object();
        try { workers = api_.list_workers(); } catch (...) {}
        json groups = json::object();
        try { groups = api_.list_thread_groups(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, agents, threads, workers, groups] {
                if (!alive_) return;
                agents_ = agents.value("agents", json::array());
                threads_ = threads.value("threads", json::array());
                workers_ = workers.value("workers", json::array());
                thread_groups_ = groups.value("groups", json::array());
                process_thread_activity();
                rebuild_roster();
                rebuild_thread_list();
                // Chat-first UX: open the most recent conversation automatically.
                if (current_thread_.empty() && !threads_.empty()) {
                    current_thread_ = threads_[0].value("id", "");
                    last_message_id_.clear();
                    rendered_msg_ids_.clear();
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
                auto arr = threads.value("threads", json::array());
                // Change signature: id + title + last_activity per row.
                // New pair thread (agents talking) → new id; new message
                // in any thread → last_activity moves. Skip the rebuild
                // entirely when nothing changed (10s poll would otherwise
                // rebuild the list ~500×/h for nothing).
                std::string sig;
                for (const auto& t : arr) {
                    sig += t.value("id", "");          sig += '\x1f';
                    sig += t.value("title", "");       sig += '\x1f';
                    sig += t.value("last_activity", ""); sig += '\x1e';
                }
                if (sig == threads_sig_) return;
                threads_sig_ = std::move(sig);
                threads_ = std::move(arr);
                // Phase 44: pin the User Inbox (kind "user") to the TOP —
                // it is the agent-to-human channel and must always be
                // visible above the churn of agent threads.
                std::stable_sort(threads_.begin(), threads_.end(),
                    [](const json& a, const json& b) {
                        return a.value("kind", "") == "user" &&
                               b.value("kind", "") != "user";
                    });
                if (!current_dm_agent_.empty()) {
                    bool dm_still_exists = false;
                    for (const auto& t : threads_) {
                        auto ids = t.value("agent_ids", json::array());
                        if (ids.size() == 1 && ids[0].is_string() &&
                            ids[0].get<std::string>() == current_dm_agent_) {
                            dm_still_exists = true; break;
                        }
                    }
                    if (!dm_still_exists) current_dm_agent_.clear();
                }
                process_thread_activity();
                rebuild_thread_list();
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::maybe_post_user_read(const std::string& tid) {
    const auto now = std::chrono::steady_clock::now();
    if (tid == last_read_post_thread_ &&
        now - last_read_post_ < std::chrono::seconds(10))
        return;
    last_read_post_ = now;
    last_read_post_thread_ = tid;
    // Fire-and-forget: a failed read-mark must never surface in the UI.
    run_async([this, tid] {
        try { api_.mark_thread_read(tid); } catch (...) {}
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
            done_queue_.push_back([this, tid, msgs, act, after] {
                if (!alive_ || tid != current_thread_) return;
                auto arr = msgs.value("messages", json::array());
                bool any_new = false;
                for (auto& m : arr) {
                    // Dedupe: a concurrent poll that snapshotted the same
                    // `after` cursor returns these messages too — render
                    // each id once. (last_message_id_ only advances in this
                    // callback, so overlapping fetches overlap in results.)
                    const std::string mid = m.value("id", "");
                    if (!mid.empty() && !rendered_msg_ids_.insert(mid).second)
                        continue;
                    append_message_bubble(
                        m.value("sender_kind", ""),
                        m.value("sender_kind", "") == "agent"
                            ? agent_name_of(agents_, m.value("sender_id", ""))
                            : (m.value("sender_kind", "") == "user" ? user_label() : "system"),
                        m.value("body", ""),
                        m.value("created_at", ""),
                        m.value("attachments", json::array()),
                        m.value("stats", json::object()));
                    last_message_id_ = m.value("id", last_message_id_);
                    any_new = true;
                }
                if (any_new) maybe_post_user_read(tid);
                // Phase 55: the initial load (empty `after`) is the
                // bounded window — record paging state. Live polls carry
                // no has_more key; leave the state alone there.
                if (after.empty()) {
                    has_more_history_ = msgs.value("has_more", false);
                    if (!arr.empty())
                        oldest_message_id_ = arr.front().value("id", "");
                    update_load_earlier_row();
                }
                // Sticky-bottom scroll happens via the vadjustment "changed"
                // handler (fires once the new bubbles are laid out).
                bool busy = act.value("busy", false);
                thread_busy_ = busy;
                if (busy) {
                    std::string who;
                    for (auto& t : act.value("turns", json::array()))
                        if (!t.value("done", true)) {
                            if (!who.empty()) who += ", ";
                            who += t.value("agent_name", "?");
                        }
                    set_status(who + " is working…");
                }
                update_live_bubble(act.value("turns", json::array()));
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
        // Worker online flags drive the roster grouping — a worker going
        // up/down must rebuild the list even when no agent state changed.
        json workers = json::object();
        try { workers = api_.list_workers(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, agents, workers] {
                if (!alive_) return;
                auto arr = agents.value("agents", json::array());
                auto warr = workers.value("workers", json::array());
                std::string sig;
                for (auto& a : arr) {
                    sig += a.value("id", "") + ":" +
                           (a.value("busy", false) ? "1" : "0") + ":" +
                           a.value("state", "") + ":" +
                           a.value("worker", "") + ";";
                }
                sig += "|";
                for (auto& w : warr) {
                    sig += w.value("name", "") + ":" +
                           (w.value("online", false) ? "1" : "0") + ";";
                }
                if (sig == roster_sig_) return;
                roster_sig_ = sig;
                agents_ = arr;
                workers_ = warr;
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
    // Preserve the user's selection: removing a ListBoxRow destroys it and
    // silently clears the selection (the "agent unselects itself" bug —
    // any agent's busy/state flip triggers a rebuild on the 1.5s poll).
    std::string selected_id;
    if (auto* sel = roster_list_.get_selected_row())
        if (auto* id = static_cast<std::string*>(sel->get_data("agent_id")))
            selected_id = *id;

    while (auto* row = roster_list_.get_row_at_index(0))
        roster_list_.remove(*row);

    // ── group agents by hive-worker (mirrors the Android app) ─────────
    // Online workers sort first (alphabetically), offline workers last.
    // Offline groups start collapsed and greyed out; the collapsed set
    // survives poll rebuilds so a manual expand/collapse sticks.
    std::unordered_map<std::string, bool> online_by_worker;
    for (auto& w : workers_)
        online_by_worker[w.value("name", "")] = w.value("online", false);

    struct Group {
        std::string worker;
        bool online;
        std::vector<json> agents;
    };
    std::map<std::string, std::vector<json>> by_worker;
    for (auto& a : agents_) {
        std::string w = a.value("worker", "");
        if (w.empty()) w = "hub";
        by_worker[w].push_back(a);
    }
    std::vector<Group> groups;
    for (auto& [w, list] : by_worker) {
        // An agent homed on a worker that isn't enrolled any more is
        // treated as offline — it can't be running.
        auto it = online_by_worker.find(w);
        groups.push_back({w, it != online_by_worker.end() && it->second,
                          std::move(list)});
    }
    std::sort(groups.begin(), groups.end(),
              [](const Group& a, const Group& b) {
                  if (a.online != b.online) return a.online > b.online;
                  return a.worker < b.worker;
              });
    for (auto& g : groups)
        if (known_workers_.insert(g.worker).second && !g.online)
            collapsed_workers_.insert(g.worker);

    for (auto& g : groups) {
        const bool expanded = !collapsed_workers_.count(g.worker);

        // ── worker group header: chevron + "name · N" + online pill ──
        auto* hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        hbox->set_margin_top(6);
        hbox->set_margin_bottom(2);
        hbox->set_margin_start(6);
        hbox->set_margin_end(6);
        auto* chevron = Gtk::make_managed<Gtk::Label>(expanded ? "\u25be" : "\u25b8");
        chevron->add_css_class("hv-worker-chevron");
        chevron->set_valign(Gtk::Align::CENTER);
        auto* wname = Gtk::make_managed<Gtk::Label>(
            g.worker + " \u00b7 " + std::to_string(g.agents.size()));
        wname->set_halign(Gtk::Align::START);
        wname->set_hexpand(true);
        wname->add_css_class("hv-worker-header");
        wname->set_valign(Gtk::Align::CENTER);
        auto* wpill = Gtk::make_managed<Gtk::Label>(g.online ? "online" : "offline");
        wpill->add_css_class(g.online ? "hv-state-ready" : "hv-state-down");
        wpill->set_valign(Gtk::Align::CENTER);
        hbox->append(*chevron);
        hbox->append(*wname);
        hbox->append(*wpill);
        auto* hrow = Gtk::make_managed<Gtk::ListBoxRow>();
        hrow->set_child(*hbox);
        hrow->set_selectable(false);    // headers never enter agent selection
        hrow->set_activatable(false);
        hrow->set_data("worker_name", new std::string(g.worker),
                       [](void* p) { delete static_cast<std::string*>(p); });
        if (!g.online) hrow->add_css_class("hv-worker-offline");
        // Click the header: expand/collapse the group.
        auto hclick = Gtk::GestureClick::create();
        hclick->set_button(GDK_BUTTON_PRIMARY);
        hclick->signal_pressed().connect(
            [this, worker = g.worker](int, double, double) {
                if (!collapsed_workers_.erase(worker))
                    collapsed_workers_.insert(worker);
                rebuild_roster();
            });
        hrow->add_controller(hclick);
        roster_list_.append(*hrow);

        if (!expanded) continue;
        for (auto& a : g.agents) {
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
            // v2 presence: fleet heartbeat states are down / starting / ready /
            // acting / restarting. Spinner when busy, a colored state tag
            // always, so "down" agents are visibly offline.
            std::string st_ = a.value("state", "down");
            for (auto& ch : st_) ch = (char)tolower((unsigned char)ch);
            // Brain state machine (hive-brain, via worker heartbeats):
            // understanding/planning/acting/verifying/documenting/compressing
            // are active phases; awaitingapproval parks on a human.
            const bool working = a.value("busy", false) ||
                st_ == "acting" || st_ == "starting" || st_ == "restarting" ||
                st_ == "understanding" || st_ == "planning" ||
                st_ == "verifying" || st_ == "documenting" ||
                st_ == "compressing";
            if (working) {
                auto* spin = Gtk::make_managed<Gtk::Spinner>();
                spin->start();
                spin->set_valign(Gtk::Align::CENTER);
                spin->set_tooltip_text("Working...");
                row_box->append(*spin);
            }
            {
                std::string label = st_;
                if (label == "awaitingapproval") label = "awaiting approval";
                auto* tag = Gtk::make_managed<Gtk::Label>(label);
                tag->add_css_class(
                    st_ == "ready" || st_ == "idle"   ? "hv-state-ready" :
                    st_ == "down"  || st_ == "error" ||
                    st_ == "cancelled"                ? "hv-state-down"  :
                                                        "hv-state-tag");
                if (st_ == "awaitingapproval")
                    tag->set_tooltip_text("Waiting on your approval");
                tag->set_valign(Gtk::Align::CENTER);
                row_box->append(*tag);
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
            if (!g.online) row->add_css_class("hv-offline");
            roster_list_.append(*row);
        }
    }

    if (!selected_id.empty()) {
        restoring_selection_ = true;
        for (int i = 0; auto* row = roster_list_.get_row_at_index(i); ++i) {
            auto* id = static_cast<std::string*>(row->get_data("agent_id"));
            if (id && *id == selected_id) { roster_list_.select_row(*row); break; }
        }
        restoring_selection_ = false;
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

// Phase 67: smart-bucket derivation (computed client-side, never
// stored hub-side). Routine threads have repeatedly shipped with the
// wrong kind on the hub (deploy lag ahead of the 2026-09-23 re-tag
// migration, agent-created threads titled "routine: …" as plain dm),
// but they all carry the "routine: " title prefix — trust the prefix
// over kind so the Routines bucket can't silently empty out.
static std::string thread_smart_bucket(const json& t) {
    if (t.value("title", "").rfind("routine: ", 0) == 0)
        return "smart:routines";
    const std::string k = t.value("kind", "");
    if (k == "user" || k == "dm") return "smart:you";
    if (k == "group") return "smart:chatter";
    return "smart:routines";
}

void HiveWindow::rebuild_thread_list() {
    if (!group_ui_loaded_) { load_group_ui_state(); group_ui_loaded_ = true; }
    while (auto* row = threads_list_.get_row_at_index(0))
        threads_list_.remove(*row);

    // Phase 67: bucket threads into groups. Hub membership ("group_id")
    // wins; then a client-local override (Phase 67b — a smart bucket the
    // user forced via Move to group); then the smart bucket.
    struct Bucket { std::string id, name; bool custom; std::vector<json> items; };
    std::vector<Bucket> buckets{{"smart:you", "You & agents", false, {}}};
    for (auto& g : thread_groups_)
        buckets.push_back({g.value("id", ""), g.value("name", ""), true, {}});
    buckets.push_back({"smart:chatter", "Agent chatter", false, {}});
    buckets.push_back({"smart:routines", "Routines", false, {}});

    // Drop overrides for threads deleted while the client was closed.
    // Guarded on a non-empty threads_: an empty list is a freshly
    // started client mid-first-poll, not a wipe signal.
    if (!threads_.empty()) {
        bool pruned = false;
        for (auto it = thread_bucket_overrides_.begin();
             it != thread_bucket_overrides_.end();) {
            const std::string& tid = it->first;
            const bool alive = std::any_of(threads_.begin(), threads_.end(),
                [&tid](const json& t) {
                    return t.value("id", "") == tid;
                });
            if (alive) { ++it; continue; }
            it = thread_bucket_overrides_.erase(it);
            pruned = true;
        }
        if (pruned) save_group_ui_state();
    }

    for (auto& t : threads_) {
        const std::string bid = thread_bucket_id(t);
        Bucket* home = nullptr;
        for (auto& b : buckets)
            if (b.id == bid) { home = &b; break; }
        if (!home && bid.rfind("smart:", 0) != 0)
            // Hub group_id names a group this client hasn't loaded
            // (deleted elsewhere, refresh in flight) — fall back smart.
            for (auto& b : buckets)
                if (b.id == thread_smart_bucket(t)) { home = &b; break; }
        if (home) home->items.push_back(t);
    }

    // One thread row — the pre-groups rendering, unchanged.
    auto render_thread_row = [this](const json& t) {
        std::string title = t.value("title", "");
        if (t.value("kind", "") == "user")
            title = "\U0001F4E5 " + title;   // 📥 User Inbox (Phase 44)
        if (title.empty()) {
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
        // Unread dot (Phase 66c): hub-computed from the PERSISTED
        // "user" read mark — survives restarts.
        const std::string tid = t.value("id", "");
        const bool unread = t.value("unread", 0) > 0 && tid != current_thread_;
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
        auto rc = Gtk::GestureClick::create();
        rc->set_button(0);
        rc->signal_pressed().connect(
            [this, rc, t](int, double x, double y) {
                if (rc->get_current_button() == GDK_BUTTON_SECONDARY)
                    show_thread_menu(*rc->get_widget(), t, x, y);
            });
        row->add_controller(rc);
        threads_list_.append(*row);
        if (!current_thread_.empty() && t.value("id", "") == current_thread_)
            threads_list_.select_row(*row);
    };

    for (auto& b : buckets) {
        if (b.items.empty() && !b.custom) continue;  // hide empty smart groups
        int unread_sum = 0;
        for (auto& t : b.items) unread_sum += t.value("unread", 0);
        const bool collapsed = collapsed_groups_.count(b.id) > 0;

        auto* hb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        hb->set_margin(4);
        auto* arrow = Gtk::make_managed<Gtk::Label>(collapsed ? "\u25B8" : "\u25BE");
        hb->append(*arrow);
        auto* name = Gtk::make_managed<Gtk::Label>(b.name);
        name->add_css_class("hv-group-header");
        name->set_halign(Gtk::Align::START);
        name->set_hexpand(true);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        hb->append(*name);
        std::string meta = std::to_string(b.items.size());
        if (unread_sum > 0) meta += " \u00B7 " + std::to_string(unread_sum) + " new";
        auto* meta_lbl = Gtk::make_managed<Gtk::Label>(meta);
        meta_lbl->add_css_class("hv-group-meta");
        hb->append(*meta_lbl);
        auto* hrow = Gtk::make_managed<Gtk::ListBoxRow>();
        hrow->set_child(*hb);
        hrow->set_activatable(false);
        hrow->set_selectable(false);
        const std::string gid = b.id, gname = b.name;
        const bool is_custom = b.custom;
        auto click = Gtk::GestureClick::create();
        click->set_button(0);
        click->signal_pressed().connect(
            [this, click, gid, gname, is_custom](int, double x, double y) {
                if (click->get_current_button() == GDK_BUTTON_SECONDARY) {
                    if (is_custom)
                        show_group_menu(*click->get_widget(), gid, gname, x, y);
                    return;
                }
                if (collapsed_groups_.count(gid)) collapsed_groups_.erase(gid);
                else collapsed_groups_.insert(gid);
                save_group_ui_state();
                rebuild_thread_list();
            });
        hrow->add_controller(click);
        threads_list_.append(*hrow);
        if (collapsed) continue;

        // Unread first, then recency (hub already orders last_activity desc).
        std::stable_sort(b.items.begin(), b.items.end(),
            [](const json& a, const json& z) {
                return a.value("unread", 0) > z.value("unread", 0);
            });
        for (auto& t : b.items) render_thread_row(t);
    }
    update_thread_title();
}

// ---- Phase 67 helpers ----
static std::filesystem::path group_ui_state_path() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") /
           ".local/share/hivemind/client-ui.json";
}

void HiveWindow::load_group_ui_state() {
    std::ifstream in(group_ui_state_path());
    if (!in) return;
    json j = json::parse(in, nullptr, /*exceptions=*/false);
    if (j.is_discarded()) return;
    for (auto& g : j.value("collapsed_groups", json::array()))
        if (g.is_string()) collapsed_groups_.insert(g.get<std::string>());
    if (j.contains("bucket_overrides") && j["bucket_overrides"].is_object())
        for (auto& [tid, bid] : j["bucket_overrides"].items())
            if (bid.is_string())
                thread_bucket_overrides_[tid] = bid.get<std::string>();
}

void HiveWindow::save_group_ui_state() const {
    std::error_code ec;
    std::filesystem::create_directories(group_ui_state_path().parent_path(), ec);
    json arr = json::array();
    for (auto& g : collapsed_groups_) arr.push_back(g);
    json overrides = json::object();
    for (auto& [tid, bid] : thread_bucket_overrides_) overrides[tid] = bid;
    std::ofstream out(group_ui_state_path());
    out << json{{"collapsed_groups", arr},
                {"bucket_overrides", overrides}}.dump(2);
}

// Effective bucket for a thread: hub membership wins (other clients
// see it too), then the client-local override, then the smart default.
std::string HiveWindow::thread_bucket_id(const json& t) const {
    if (t.contains("group_id") && t["group_id"].is_string()) {
        const std::string gid = t["group_id"].get<std::string>();
        if (!gid.empty()) return gid;
    }
    const auto it = thread_bucket_overrides_.find(t.value("id", ""));
    if (it != thread_bucket_overrides_.end()) return it->second;
    return thread_smart_bucket(t);
}

void HiveWindow::move_thread_to(const std::string& thread_id,
                                const std::string& group_id) {
    // "smart:…" targets are client-local: pin the override here and
    // CLEAR the hub membership so this client and every other surface
    // agree. Custom group ids (and "" = smart default) persist
    // hub-side; both also drop a stale local override.
    const bool smart = group_id.rfind("smart:", 0) == 0;
    if (smart)
        thread_bucket_overrides_[thread_id] = group_id;
    else
        thread_bucket_overrides_.erase(thread_id);
    save_group_ui_state();
    rebuild_thread_list();   // immediate feedback, no poll wait
    run_async([this, thread_id, smart, group_id] {
        try {
            api_.set_thread_group(thread_id, smart ? "" : group_id);
        } catch (...) {}
        std::lock_guard<std::mutex> lk(queue_m_);
        done_queue_.push_back([this] { if (alive_) refresh_all(); });
    });
}

void HiveWindow::show_group_name_dialog(const std::string& title,
                                        const std::string& initial,
                                        const std::string& group_id,
                                        const std::string& move_thread_id) {
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title(title);
    install_dialog_titlebar(*dialog, title);
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    auto* ok = dialog->add_button(
        group_id.empty() ? "Create" : "Rename", Gtk::ResponseType::OK);
    ok->add_css_class("suggested-action");
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(initial);
    entry->set_margin(18);
    entry->set_width_chars(28);
    entry->set_activates_default(true);
    dialog->get_content_area()->append(*entry);
    dialog->signal_response().connect(
        [this, dialog, entry, group_id, move_thread_id](int r) {
            const std::string name = entry->get_text();
            dialog->destroy();
            if (r != Gtk::ResponseType::OK || name.empty()) return;
            run_async([this, name, group_id, move_thread_id] {
                try {
                    if (group_id.empty()) {
                        const std::string gid = api_.create_thread_group(name);
                        if (!move_thread_id.empty())
                            api_.set_thread_group(move_thread_id, gid);
                    } else {
                        api_.rename_thread_group(group_id, name);
                    }
                } catch (...) {}
                std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this] { if (alive_) refresh_all(); });
            });
        });
    dialog->present();
}

void HiveWindow::show_group_menu(Gtk::Widget& anchor,
                                 const std::string& group_id,
                                 const std::string& group_name,
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
    mk("\u270F Rename group\u2026", nullptr,
       [this, group_id, group_name] {
           show_group_name_dialog("Rename group", group_name, group_id, "");
       });
    // Members revert to their smart groups hub-side.
    mk("\U0001F5D1 Delete group", "destructive-action",
       [this, group_id] {
           run_async([this, group_id] {
               try { api_.delete_thread_group(group_id); } catch (...) {}
               std::lock_guard<std::mutex> lk(queue_m_);
               done_queue_.push_back([this] { if (alive_) refresh_all(); });
           });
       });
    pop->set_child(*box);
    pop->set_parent(anchor);
    Gdk::Rectangle rect((int)x, (int)y, 1, 1);
    pop->set_pointing_to(rect);
    pop->signal_closed().connect([pop] { pop->unparent(); });
    pop->popup();
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

// Phase 52: live streaming bubble. The activity poll already delivers
// turns[].partial (the agent's running commentary between tool calls) —
// render the first active turn's partial as an ephemeral agent-styled
// bubble at the bottom of the chat. Removed when the turn ends; the
// stored final message then appears via the normal fetch.
void HiveWindow::update_live_bubble(const nlohmann::json& turns) {
    std::string agent_id, agent_name, partial;
    for (auto& t : turns) {
        if (t.value("done", true)) continue;
        agent_id   = t.value("agent_id", "");
        agent_name = t.value("agent_name", "?");
        partial    = strip_tool_transcript(t.value("partial", ""));
        if (!partial.empty()) break;
        agent_id.clear(); agent_name.clear(); partial.clear();
    }

    if (partial.empty()) {
        if (live_row_) {
            messages_box_.remove(*live_row_);
            live_row_ = nullptr;
            live_label_ = nullptr;
            live_agent_key_.clear();
        }
        return;
    }

    // A different agent's turn took over: rebuild the row identity.
    if (live_row_ && live_agent_key_ != agent_id) {
        messages_box_.remove(*live_row_);
        live_row_ = nullptr;
        live_label_ = nullptr;
    }

    if (!live_row_) {
        live_row_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        live_row_->add_css_class("hv-msg-agent");
        live_row_->add_css_class("hv-msg-live");
        std::string avatar;
        for (auto& a : agents_)
            if (a.value("name", "") == agent_name) {
                avatar = a.value("avatar", "");
                break;
            }
        live_row_->append(*make_avatar(agent_name, false, AVATAR_LOGICAL,
                                       avatar));
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        box->set_hexpand(true);
        auto* header = Gtk::make_managed<Gtk::Label>(agent_name + " — live");
        header->add_css_class("hv-sender");
        header->set_halign(Gtk::Align::START);
        box->append(*header);
        live_label_ = Gtk::make_managed<Gtk::Label>();
        live_label_->set_wrap(true);
        live_label_->set_selectable(true);
        live_label_->set_halign(Gtk::Align::START);
        live_label_->set_xalign(0);
        box->append(*live_label_);
        live_row_->append(*box);
        messages_box_.append(*live_row_);
        live_agent_key_ = agent_id;
    }
    live_label_->set_text(partial + " \u258C");
}

// Phase 55: row construction extracted so history page-back can PREPEND
// older bubbles as well as append new ones. Returns nullptr when there
// is nothing to show (pure tool transcript). Caller owns placement and
// rendered_ bookkeeping.
Gtk::Widget* HiveWindow::build_message_row(const std::string& sender_kind,
                                           const std::string& sender_label,
                                           const std::string& body,
                                           const std::string& created_at,
                                           const nlohmann::json& attachments,
                                           const nlohmann::json& stats) {
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

    // Older thread history still holds raw hive-brain transcripts (pre-fix
    // server posted those); new messages arrive clean. Filter defensively.
    const std::string clean = sender_kind == "agent" ? strip_tool_transcript(body) : body;
    if (clean.empty()) return nullptr;   // pure tool-call transcript — nothing to show

    // System messages stay minimal: centered dim line, no avatar.
    if (sender_kind != "user" && sender_kind != "agent") {
        auto* sys = Gtk::make_managed<Gtk::Label>(clean);
        sys->add_css_class("hv-msg-system");
        sys->set_wrap(true);
        sys->set_selectable(true);
        return sys;
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
                             AVATAR_LOGICAL,
                             sender_kind == "user" ? user_avatar_ : sender_avatar));

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

    // Phase 64: per-turn stats footer on agent bubbles — context usage,
    // prompt-cache hits and iteration count for the turn that produced
    // this message. Small dim line, same style class as the timestamp.
    if (sender_kind == "agent" && stats.is_object() && !stats.empty()) {
        const int total   = stats.value("total_tokens", 0);
        const int maxctx  = stats.value("max_context_size", 0);
        const int cached  = stats.value("cached_tokens", 0);
        const int iters   = stats.value("iterations", 0);
        const int comp    = stats.value("completion_tokens", 0);
        if (total > 0) {
            const double pct =
                maxctx > 0 ? (100.0 * total / maxctx) : 0.0;
            std::ostringstream line;
            line << "ctx " << total << "/" << maxctx << " ("
                 << std::fixed << std::setprecision(1) << pct << "%)"
                 << " · +" << comp << " out";
            if (cached > 0)
                line << " · cache " << cached << "/"
                     << stats.value("prompt_tokens", 0);
            line << " · " << iters << " iter" << (iters == 1 ? "" : "s");
            auto* foot = Gtk::make_managed<Gtk::Label>(line.str());
            foot->set_halign(Gtk::Align::START);
            foot->add_css_class("hv-time");   // reuse the dim small style
            foot->set_selectable(true);
            box->append(*foot);
        }
    }

    // Phase 18 + 34: attachments. Images render inline (downloaded via
    // the API into the client cache — host_path is the SERVER's path and
    // is wrong cross-machine); other files keep the Phase 18 chips.
    if (attachments.is_array() && !attachments.empty()) {
        auto* chips = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        bool any_chip = false;
        for (auto& a : attachments) {
            const std::string name = a.value("name", "file");
            std::string ext = std::filesystem::path(name).extension().string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                ext == ".gif" || ext == ".webp") {
                auto* holder = Gtk::make_managed<Gtk::Box>(
                    Gtk::Orientation::VERTICAL, 4);
                box->append(*holder);
                load_image_attachment(holder, a);
                continue;
            }
            if (ext == ".mp4" || ext == ".mkv" || ext == ".webm" ||
                ext == ".mov" || ext == ".avi" || ext == ".m4v") {
                auto* holder = Gtk::make_managed<Gtk::Box>(
                    Gtk::Orientation::VERTICAL, 4);
                box->append(*holder);
                load_video_attachment(holder, a);
                continue;
            }
            // 2026-09-17: the old chip opened file://host_path — the
            // WORKER's path, meaningless on this machine, and empty for
            // agent-originated records (no click handler at all → dead
            // button). Download via the hub API instead, same as images.
            auto* chip = Gtk::make_managed<Gtk::Button>("📎 " + name);
            chip->add_css_class("flat");
            chip->set_tooltip_text("Download " + name);
            const std::string tid = current_thread_;
            chip->signal_clicked().connect([this, a, tid, chip] {
                open_file_attachment(chip, a, tid);
            });
            chips->append(*chip);
            any_chip = true;
        }
        if (any_chip) box->append(*chips);
    }
    row->append(*box);
    return row;
}

// ── Phase 55: history page-back ───────────────────────────────────────
// The "load earlier…" affordance sits at the top of the message list
// while the hub reports older messages beyond the current window.
void HiveWindow::update_load_earlier_row() {
    if (has_more_history_) {
        if (!load_earlier_btn_) {
            load_earlier_btn_ =
                Gtk::make_managed<Gtk::Button>("\u2B06 load earlier messages\u2026");
            load_earlier_btn_->add_css_class("flat");
            load_earlier_btn_->add_css_class("hv-time");
            load_earlier_btn_->signal_clicked().connect(
                [this] { load_earlier(); });
            messages_box_.prepend(*load_earlier_btn_);
        }
        load_earlier_btn_->set_sensitive(!loading_earlier_);
    } else if (load_earlier_btn_) {
        messages_box_.remove(*load_earlier_btn_);
        load_earlier_btn_ = nullptr;
    }
}

void HiveWindow::load_earlier() {
    if (current_thread_.empty() || oldest_message_id_.empty() ||
        loading_earlier_ || !has_more_history_)
        return;
    loading_earlier_ = true;
    if (load_earlier_btn_) load_earlier_btn_->set_sensitive(false);
    const std::string tid = current_thread_;
    const std::string before = oldest_message_id_;
    run_async([this, tid, before] {
        json msgs = api_.list_messages_before(tid, before);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, tid, msgs] {
                loading_earlier_ = false;
                if (!alive_ || tid != current_thread_) return;
                auto arr = msgs.value("messages", json::array());
                // Detach the button first: the page's bubbles prepend
                // ABOVE the previous oldest, then the button returns to
                // the very top (still more) or is gone (reached the start).
                auto* btn = load_earlier_btn_;
                if (btn) messages_box_.remove(*btn);
                // Transcript bookkeeping stays chronological — collect
                // the page ASC and splice at the front.
                std::vector<nlohmann::json> page_records;
                for (const auto& m : arr)
                    page_records.push_back(
                        {{"sender_kind", m.value("sender_kind", "")},
                         {"sender_label", ""},
                         {"body", m.value("body", "")},
                         {"created_at", m.value("created_at", "")},
                         {"attachments", m.value("attachments",
                                                 nlohmann::json::array())},
                         {"stats", m.value("stats",
                                           nlohmann::json::object())}});
                rendered_.insert(rendered_.begin(),
                                 page_records.begin(), page_records.end());
                // Prepend in REVERSE page order so the final on-screen
                // order is chronological.
                for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
                    const auto& m = *it;
                    const std::string mid = m.value("id", "");
                    if (!mid.empty() &&
                        !rendered_msg_ids_.insert(mid).second)
                        continue;
                    const std::string sk = m.value("sender_kind", "");
                    if (auto* w = build_message_row(
                            sk,
                            sk == "agent"
                                ? agent_name_of(agents_,
                                                m.value("sender_id", ""))
                                : (sk == "user" ? user_label() : "system"),
                            m.value("body", ""),
                            m.value("created_at", ""),
                            m.value("attachments", nlohmann::json::array()),
                            m.value("stats", nlohmann::json::object())))
                        messages_box_.prepend(*w);
                }
                if (!arr.empty())
                    oldest_message_id_ =
                        arr.front().value("id", oldest_message_id_);
                has_more_history_ = msgs.value("has_more", false);
                if (btn) {   // restore on top; update_ decides visibility
                    messages_box_.prepend(*btn);
                    load_earlier_btn_ = btn;
                }
                update_load_earlier_row();
            });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::append_message_bubble(const std::string& sender_kind,
                                       const std::string& sender_label,
                                       const std::string& body,
                                       const std::string& created_at,
                                       const nlohmann::json& attachments,
                                       const nlohmann::json& stats) {
    rendered_.push_back({{"sender_kind", sender_kind},
                         {"sender_label", sender_label},
                         {"body", body},
                         {"created_at", created_at},
                         {"attachments", attachments},
                         {"stats", stats}});
    if (auto* w = build_message_row(sender_kind, sender_label, body,
                                    created_at, attachments, stats))
        messages_box_.append(*w);
}

// ── Phase 34: inline image attachments ──────────────────────────────────────

void HiveWindow::show_image_picture(Gtk::Box* holder,
                                    const std::string& file,
                                    const std::string& caption) {
    auto tex = Gdk::Texture::create_from_filename(file);
    if (!tex) {
        auto* err = Gtk::make_managed<Gtk::Label>(
            "⚠ could not load image " + file);
        err->add_css_class("hv-time");
        holder->append(*err);
        return;
    }
    // Scale to fit a 480px box, preserving aspect.
    int w = tex->get_width(), h = tex->get_height();
    const double scale =
        std::min({1.0, 480.0 / std::max(1, w), 480.0 / std::max(1, h)});
    w = (int)std::lround(w * scale);
    h = (int)std::lround(h * scale);

    auto* pic = Gtk::make_managed<Gtk::Picture>();
    pic->set_paintable(tex);
    pic->set_content_fit(Gtk::ContentFit::CONTAIN);
    pic->set_size_request(w, h);
    auto* btn = Gtk::make_managed<Gtk::Button>();
    btn->set_child(*pic);
    btn->add_css_class("flat");
    btn->set_halign(Gtk::Align::START);
    btn->set_tooltip_text(file);
    btn->signal_clicked().connect([file] {
        Gio::AppInfo::launch_default_for_uri("file://" + file);
    });
    holder->append(*btn);
    if (!caption.empty()) {
        auto* cap = Gtk::make_managed<Gtk::Label>(caption);
        cap->add_css_class("hv-time");
        cap->set_halign(Gtk::Align::START);
        cap->set_wrap(true);
        cap->set_selectable(true);
        holder->append(*cap);
    }
}

void HiveWindow::load_image_attachment(Gtk::Box* holder,
                                       const nlohmann::json& att) {
    namespace fs = std::filesystem;
    const std::string name    = att.value("name", "image");
    const std::string caption = att.value("caption", "");
    // Phase 35: agent-originated records carry the producing worker —
    // without it pick_worker guesses (breaks multi-worker downloads).
    const std::string worker  = att.value("worker", "");
    // Server-provided path is container-style "/workspace/<rel>"; the
    // download endpoint wants the workspace-relative form.
    std::string rel = att.value("path", "");
    if (rel.rfind("/workspace/", 0) == 0) rel.erase(0, 11);
    if (rel.empty())
        rel = "attachments/" + current_thread_ + "/" + name;

    const fs::path cache =
        fs::path(Glib::get_user_cache_dir()) / "hivemind-client" /
        "attachments" / current_thread_ / name;

    // Cache hit → render immediately.
    std::error_code ec;
    if (fs::is_regular_file(cache, ec) && fs::file_size(cache, ec) > 0) {
        show_image_picture(holder, cache.string(), caption);
        return;
    }

    auto* ph = Gtk::make_managed<Gtk::Label>("⏳ " + name + " — loading…");
    ph->add_css_class("hv-time");
    ph->set_halign(Gtk::Align::START);
    holder->append(*ph);

    const std::string key = cache.string();
    if (!image_downloads_.insert(key).second)
        return;   // already downloading (re-render)

    // Widget-lifetime guard: if the bubble is destroyed (thread switch,
    // theme re-render) before the download lands, the completion no-ops.
    auto weak = std::make_shared<Gtk::Box*>(holder);
    holder->signal_destroy().connect([weak] { *weak = nullptr; });

    const std::string tid = current_thread_;
    run_async([this, rel, worker, key, tid, caption, weak] {
        std::string err;
        try {
            std::string bytes =
                api_.download("/api/files/download?path=" +
                              ApiClient::url_encode(rel) +
                              (worker.empty() ? "" :
                               "&worker=" + ApiClient::url_encode(worker)));
            std::error_code ecw;
            fs::create_directories(fs::path(key).parent_path(), ecw);
            std::ofstream out(key, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), (std::streamsize)bytes.size());
            out.close();
            if (!out || bytes.empty()) err = "write failed";
        } catch (const std::exception& e) {
            err = e.what();
        }
        std::lock_guard<std::mutex> lk(queue_m_);
        done_queue_.push_back([this, key, tid, caption, weak, err] {
            image_downloads_.erase(key);
            if (!alive_ || !*weak || tid != current_thread_) return;
            // Swap the placeholder for the picture (or an error note).
            while (auto* child = (*weak)->get_first_child())
                (*weak)->remove(*child);
            if (err.empty()) {
                show_image_picture(*weak, key, caption);
            } else {
                auto* el = Gtk::make_managed<Gtk::Label>(
                    "⚠ image unavailable: " + err);
                el->add_css_class("hv-time");
                el->set_halign(Gtk::Align::START);
                (*weak)->append(*el);
            }
        });
    });
}

namespace {

/// Resolve an attachment record to its workspace-relative path, the
/// producing worker, and the client-side cache file (shared by the image
/// path, the generic-file chip, and the UiApi download hook).
void resolve_attachment(const nlohmann::json& att, const std::string& tid,
                        std::string& rel, std::string& worker,
                        std::filesystem::path& cache) {
    const std::string name = att.value("name", "file");
    worker = att.value("worker", "");
    rel = att.value("path", "");
    if (rel.rfind("/workspace/", 0) == 0) rel.erase(0, 11);
    if (rel.empty()) rel = "attachments/" + tid + "/" + name;
    cache = std::filesystem::path(Glib::get_user_cache_dir()) /
            "hivemind-client" / "attachments" / tid / name;
}

/// Blocking download via the hub API into the client cache.
/// Returns "" on success, the error otherwise. Call OFF the GTK thread.
std::string download_to_cache(ApiClient& api, const std::string& rel,
                              const std::string& worker,
                              const std::filesystem::path& cache) {
    namespace fs = std::filesystem;
    try {
        std::string bytes = api.download(
            "/api/files/download?path=" + ApiClient::url_encode(rel) +
            (worker.empty() ? "" :
             "&worker=" + ApiClient::url_encode(worker)));
        std::error_code ec;
        fs::create_directories(cache.parent_path(), ec);
        std::ofstream out(cache, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize)bytes.size());
        out.close();
        if (!out || bytes.empty()) return "write failed";
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

} // namespace

void HiveWindow::open_file_attachment(Gtk::Button* chip,
                                      const nlohmann::json& att,
                                      const std::string& tid) {
    namespace fs = std::filesystem;
    std::string rel, worker;
    fs::path cache;
    resolve_attachment(att, tid, rel, worker, cache);
    const std::string name = att.value("name", "file");

    std::error_code ec;
    if (fs::is_regular_file(cache, ec) && fs::file_size(cache, ec) > 0) {
        Gio::AppInfo::launch_default_for_uri("file://" + cache.string());
        return;
    }
    chip->set_sensitive(false);
    chip->set_label("\u23F3 " + name);   // hourglass while downloading
    set_status("Downloading " + name + "\u2026");
    auto weak = std::make_shared<Gtk::Button*>(chip);
    chip->signal_destroy().connect([weak] { *weak = nullptr; });
    const std::string cache_s = cache.string();
    run_async([this, rel, worker, cache, cache_s, name, weak] {
        const std::string err = download_to_cache(api_, rel, worker, cache);
        std::lock_guard<std::mutex> lk(queue_m_);
        done_queue_.push_back([this, cache_s, name, weak, err] {
            if (!alive_) return;
            if (*weak) {
                (*weak)->set_sensitive(true);
                (*weak)->set_label("\U0001F4CE " + name);
            }
            if (err.empty()) {
                set_status("Downloaded " + name);
                Gio::AppInfo::launch_default_for_uri("file://" + cache_s);
            } else {
                set_status("Download failed: " + err);
            }
        });
        dispatcher_.emit();
    });
}

nlohmann::json HiveWindow::ui_download_attachment(const std::string& name) {
    // UiApi thread: find the record on the GTK thread, then block on the
    // download HERE (never network on the GTK thread).
    auto found = invoke_on_ui([this, name] {
        for (auto it = rendered_.rbegin(); it != rendered_.rend(); ++it)
            if ((*it)["attachments"].is_array())
                for (auto& a : (*it)["attachments"])
                    if (a.value("name", "") == name)
                        return nlohmann::json{{"att", a},
                                              {"tid", current_thread_}};
        return nlohmann::json{{"error", "no attachment named: " + name}};
    });
    if (found.contains("error")) return found;
    std::string rel, worker;
    std::filesystem::path cache;
    resolve_attachment(found["att"], found.value("tid", ""), rel, worker,
                       cache);
    const std::string err = download_to_cache(api_, rel, worker, cache);
    if (!err.empty()) return nlohmann::json{{"error", err}};
    return nlohmann::json{{"cache", cache.string()},
                          {"rel", rel},
                          {"size", std::filesystem::file_size(cache)}};
}

// -- Phase E: inline video attachments -----------------------------------
// Same download/cache pattern as images. The paused Gtk::Video doubles as
// the thumbnail (first frame); click toggles play/pause; a small button
// opens the cached copy in the system player. On media error (no gstreamer
// backend etc.) the card falls back to a plain attachment chip.

void HiveWindow::show_video_widget(Gtk::Box* holder, const std::string& file,
                                   const std::string& name) {
    auto media = Gtk::MediaFile::create_for_filename(file);
    auto* video = Gtk::make_managed<Gtk::Video>();
    video->set_media_stream(media);
    video->set_autoplay(false);
    video->set_size_request(320, 180);
    video->set_tooltip_text(name + "  -  click to play/pause");

    // Click toggles play/pause.
    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect([media](int, double, double) {
        if (media->get_playing()) media->pause(); else media->play();
    });
    video->add_controller(click);

    // Media error -> swap the video for a plain chip (widget may be gone
    // by then; removing a destroyed child is avoided by the box still
    // owning the widget tree - GTK no-ops a remove of an already-removed
    // child only if parented, so guard via get_parent()).
    auto* chip = Gtk::make_managed<Gtk::Button>("attachment: " + name);
    chip->add_css_class("flat");
    chip->set_halign(Gtk::Align::START);
    chip->set_visible(false);
    chip->signal_clicked().connect([file] {
        Gio::AppInfo::launch_default_for_uri("file://" + file);
    });
    media->property_error().signal_changed().connect([video, chip] {
        if (video->get_parent()) {
            video->unparent();   // make_managed + unparent drops it safely
        }
        chip->set_visible(true);
    });

    holder->append(*video);
    holder->append(*chip);

    auto* open_btn = Gtk::make_managed<Gtk::Button>("open in player: " + name);
    open_btn->add_css_class("flat");
    open_btn->set_halign(Gtk::Align::START);
    open_btn->signal_clicked().connect([file] {
        Gio::AppInfo::launch_default_for_uri("file://" + file);
    });
    holder->append(*open_btn);
}

void HiveWindow::load_video_attachment(Gtk::Box* holder,
                                       const nlohmann::json& att) {
    namespace fs = std::filesystem;
    const std::string name = att.value("name", "video");
    // Phase 35: carry the producing worker through to the download.
    const std::string worker = att.value("worker", "");
    std::string rel = att.value("path", "");
    if (rel.rfind("/workspace/", 0) == 0) rel.erase(0, 11);
    if (rel.empty())
        rel = "attachments/" + current_thread_ + "/" + name;

    const fs::path cache =
        fs::path(Glib::get_user_cache_dir()) / "hivemind-client" /
        "attachments" / current_thread_ / name;

    std::error_code ec;
    if (fs::is_regular_file(cache, ec) && fs::file_size(cache, ec) > 0) {
        show_video_widget(holder, cache.string(), name);
        return;
    }

    auto* ph = Gtk::make_managed<Gtk::Label>("loading video: " + name);
    ph->add_css_class("hv-time");
    ph->set_halign(Gtk::Align::START);
    holder->append(*ph);

    const std::string key = cache.string();
    if (!image_downloads_.insert(key).second)
        return;   // already downloading (re-render)

    auto weak = std::make_shared<Gtk::Box*>(holder);
    holder->signal_destroy().connect([weak] { *weak = nullptr; });

    const std::string tid = current_thread_;
    run_async([this, rel, worker, key, tid, name, weak] {
        std::string err;
        try {
            std::string bytes =
                api_.download("/api/files/download?path=" +
                              ApiClient::url_encode(rel) +
                              (worker.empty() ? "" :
                               "&worker=" + ApiClient::url_encode(worker)));
            std::error_code ecw;
            fs::create_directories(fs::path(key).parent_path(), ecw);
            std::ofstream out(key, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), (std::streamsize)bytes.size());
        } catch (const std::exception& e) {
            err = e.what();
        }
        std::lock_guard<std::mutex> lk(queue_m_);
        done_queue_.push_back([this, key, tid, name, weak, err] {
            image_downloads_.erase(key);
            if (!alive_ || !*weak || tid != current_thread_) return;
            while (auto* child = (*weak)->get_first_child())
                (*weak)->remove(*child);
            if (err.empty()) {
                show_video_widget(*weak, key, name);
            } else {
                auto* el = Gtk::make_managed<Gtk::Label>(
                    "video unavailable: " + err);
                el->add_css_class("hv-time");
                el->set_halign(Gtk::Align::START);
                (*weak)->append(*el);
            }
        });
        dispatcher_.emit();
    });
}

// ── callbacks ───────────────────────────────────────────────────────────────

void HiveWindow::set_composer_enabled(bool enabled) {
    learn_btn_.set_sensitive(enabled);
    entry_.set_sensitive(enabled);
    send_btn_.set_sensitive(enabled);
    attach_btn_.set_sensitive(enabled);
}

void HiveWindow::select_thread_row(const std::string& id) {
    if (id.empty()) return;
    for (auto* child = threads_list_.get_first_child(); child;
         child = child->get_next_sibling()) {
        auto* row = dynamic_cast<Gtk::ListBoxRow*>(child);
        if (!row) continue;
        auto* rid = static_cast<std::string*>(row->get_data("thread_id"));
        if (rid && *rid == id) {
            threads_list_.select_row(*row);   // on_thread_selected early-
            return;                           // returns: id == current_
        }
    }
}

void HiveWindow::on_agent_selected() {
    auto* row = roster_list_.get_selected_row();
    if (!row) return;
    auto* id = static_cast<std::string*>(row->get_data("agent_id"));
    if (!id) return;
    // Already in this agent's DM — don't churn threads on re-selection.
    std::string aid = *id;
    if (aid == current_dm_agent_) return;
    current_dm_agent_ = aid;
    // Start (or focus) a DM with this agent.
    run_async([this, aid] {
        json t = api_.create_thread("", {aid}, /*reuse_dm=*/true);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, t] {
                if (!alive_) return;
                current_thread_ = t.value("id", "");
                // 2026-09-16: only on_thread_selected() set composer
                // sensitivity, and it early-returns once current_thread_
                // is already set — so clicking an agent after viewing the
                // User Inbox left the composer greyed out. A DM is never
                // the inbox: always re-enable.
                set_composer_enabled(true);
                entry_.set_placeholder_text(
                    "Message the team\u2026  (Enter to send)");
                chat_stick_bottom_ = true;
                // Sidebar: light up this thread's row. refresh_threads()
                // is signature-gated and reusing an existing DM changes
                // no server data, so without this the Threads highlight
                // stayed on whatever row was selected before.
                select_thread_row(current_thread_);
                last_message_id_.clear();
                rendered_msg_ids_.clear();
                while (auto* c = messages_box_.get_first_child())
                    messages_box_.remove(*c);
                    live_row_ = nullptr; live_label_ = nullptr; live_agent_key_.clear();
                        load_earlier_btn_ = nullptr; has_more_history_ = false;
                        oldest_message_id_.clear(); loading_earlier_ = false;
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
    current_dm_agent_.clear();   // user picked a thread explicitly
    last_message_id_.clear();
    rendered_msg_ids_.clear();
    chat_stick_bottom_ = true;   // opening a thread always shows the newest
    // Phase 44: the User Inbox is a one-way agent->human channel. It can
    // never be deleted, and the composer is disabled — the inbox has zero
    // agent participants, so a reply here would dispatch to nobody.
    const bool is_inbox = (*id == "user");
    set_composer_enabled(!is_inbox);
    entry_.set_placeholder_text(is_inbox
        ? "Agents message you here — reply in an agent's own thread"
        : "Message the team…  (Enter to send)");
    for (auto& t : threads_)
        if (t.value("id", "") == *id)
            last_seen_activity_[*id] = t.value("last_activity", "");
    // 2026-10-01: VIEWING a thread marks it read — this is the ONLY poster
    // of the user read-mark besides poll_chat's new-message path, and its
    // absence was the stuck-unread-dot bug: clicking cleared the dot
    // locally, the 10 s refresh re-fetched unread > 0 from the hub, and
    // the dot resurrected. Fire-and-forget with a 10 s per-thread debounce.
    maybe_post_user_read(*id);
    // Clear the unread dot NOW: rows (and their dots) are otherwise only
    // rebuilt by the 10s signature-gated refresh — and clicking changes
    // no server-side data, so the dot could linger for minutes. The
    // rebuild re-selects the current row; the row_selected handler
    // re-fires but returns early on *id == current_thread_ (no loop).
    rebuild_thread_list();
    update_thread_title();
    while (auto* c = messages_box_.get_first_child())
        messages_box_.remove(*c);
        live_row_ = nullptr; live_label_ = nullptr; live_agent_key_.clear();
                        load_earlier_btn_ = nullptr; has_more_history_ = false;
                        oldest_message_id_.clear(); loading_earlier_ = false;
    rendered_.clear();
    poll_chat();
}

void HiveWindow::on_send() {
    std::string text = entry_.get_text();
    if (current_thread_.empty()) {
        set_status("Select an agent or thread first — message not sent.");
        return;
    }
    if (current_thread_ == "user") {
        set_status("The inbox is one-way — reply in an agent's own thread.");
        return;
    }
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
    if (current_thread_.empty()) {
        // 2026-09-17: this used to return silently — the click looked dead.
        set_status("Select an agent or thread first — nothing to attach to.");
        return;
    }
    auto dialog = Gtk::FileDialog::create();   // refcounted — kept by capture
    dialog->set_title("Attach a file");
    dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> file;
        try { file = dialog->open_finish(result); }
        catch (const Glib::Error& e) {
            // Cancel/dismiss is normal; anything else deserves feedback
            // (the old catch(...) swallowed real portal errors silently).
            if (e.domain() != GTK_DIALOG_ERROR)
                set_status(std::string{"File dialog error: "} + e.what());
            return;
        }
        catch (...) { return; }
        if (!file) return;
        const std::string path = file->get_path();
        if (path.empty()) return;
        attach_picked_file(path);
    });
}

void HiveWindow::attach_picked_file(const std::string& path) {
    const std::string tid = current_thread_;
    if (path.empty() || tid.empty()) return;
    set_status("Uploading " + path.substr(path.find_last_of("/\\") + 1) +
               "\u2026");
    {
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
    }
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
    mk("\u23F9 Stop", "destructive-action",
       [this, agent] { on_stop_agent(agent); });
    mk("\U0001F5A5 Computer", nullptr, [this, agent] { show_agent_screen(agent); });
    mk("\U0001F441 Computer (view-only)", nullptr,
       [this, agent] { show_agent_screen(agent, true); });
    mk("\U0001F4CB Exec log", nullptr, [this, agent] { show_exec_log(agent); });
    // Agent menu = agent-scoped action: Add a task FOR THIS agent
    // (preselected). The full manager lives on the title bar.
    mk("\u23F0 Schedule Task\u2026", nullptr,
       [this, agent] { show_task_dialog(agent.value("id", "")); });
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
    const std::string name = agent.value("name", id);
    std::string url = api_.base_url() + "/screen/" + id +
                      "/?key=" + api_.api_key();
    if (view_only) url += "&view_only=1";

#ifdef HIVEMIND_WITH_WEBKIT
    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title("Computer — " + name + " (starting…)");
    win->set_default_size(1320, 860);
    win->set_transient_for(*this);

    // WebKitWebView is plain GTK4 C API — embeds fine in a gtkmm window.
    auto* wv = webkit_web_view_new();
    gtk_widget_set_vexpand(wv, TRUE);
    gtk_widget_set_hexpand(wv, TRUE);
    win->set_child(*Glib::wrap(GTK_WIDGET(wv)));
    win->present();

    // Bring-up can take MINUTES (standard agents are recreated from the
    // browser image; x11vnc/websockify then start inside). The old fixed
    // 3s delay virtually guaranteed the 503 "screen not running" page.
    // Start, poll status until the worker reports the stack up, THEN
    // load noVNC. alive guards against the user closing the window
    // mid-poll (detached thread outliving the widgets).
    auto alive = std::make_shared<bool>(true);
    win->signal_destroy().connect([alive] { *alive = false; });
    std::thread([this, id, url, wv, win, name, alive] {
        try { api_.screen_start(id); } catch (...) {}
        bool up = false;
        for (int i = 0; i < 48 && !up; ++i) {   // ~4 min cap, 5s ticks
            try {
                up = api_.screen_status(id).value("running", false);
            } catch (...) {}
            if (!up) std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        Glib::signal_idle().connect_once([wv, win, url, name, up, alive] {
            if (!*alive) return;
            if (up) {
                win->set_title("Computer — " + name);
                webkit_web_view_load_uri(WEBKIT_WEB_VIEW(wv), url.c_str());
            } else {
                win->set_title("Computer — " + name +
                               " (screen failed to start — close and retry)");
            }
        });
    }).detach();
#else
    // System-browser fallback: same poll, then open.
    std::thread([this, id, url] {
        try { api_.screen_start(id); } catch (...) {}
        bool up = false;
        for (int i = 0; i < 48 && !up; ++i) {
            try {
                up = api_.screen_status(id).value("running", false);
            } catch (...) {}
            if (!up) std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        if (up)
            Glib::signal_idle().connect_once(
                [url] { Gio::AppInfo::launch_default_for_uri(url); });
    }).detach();
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
    // Theme-aware palettes: Catppuccin Macchiato in dark mode, Latte in
    // light — the dark palette's pale yellow/blue are unreadable on white.
    const bool dm = dark_mode_;
    auto tag_ok  = buf->create_tag("ok");
    tag_ok->property_foreground()  = dm ? "#8aadf4" : "#1e66f5";
    auto tag_bad = buf->create_tag("bad");
    tag_bad->property_foreground() = dm ? "#ed8796" : "#d20f39";
    auto tag_to  = buf->create_tag("to");
    tag_to->property_foreground()  = dm ? "#eed49f" : "#df8e1d";
    auto tag_dim = buf->create_tag("dim");
    tag_dim->property_foreground() = dm ? "#6e738d" : "#6c6f85";

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
    // Phase 29: create shared-library skills without leaving the client.
    auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* spacer = Gtk::make_managed<Gtk::Label>("");
    spacer->set_hexpand(true);
    auto* newbtn = Gtk::make_managed<Gtk::Button>("+ New skill");
    newbtn->add_css_class("suggested-action");
    hdr->append(*spacer);
    hdr->append(*newbtn);
    outer->append(*hdr);
    auto* loading = Gtk::make_managed<Gtk::Label>("Loading…");
    loading->add_css_class("dim-label");
    outer->append(*loading);
    dialog->set_child(*outer);
    newbtn->signal_clicked().connect([this, dialog] {
        show_new_skill_dialog(dialog);
    });
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
                    // Phase 29: delete from the shared library (all agents).
                    auto* del = Gtk::make_managed<Gtk::Button>("\u2715");
                    del->add_css_class("destructive-action");
                    del->set_valign(Gtk::Align::CENTER);
                    del->set_tooltip_text("Delete from shared library");
                    del->signal_clicked().connect([this, dialog, sname] {
                        run_async([this, dialog, sname] {
                            try { api_.delete_skill(sname); }
                            catch (...) {}
                            std::lock_guard<std::mutex> lk(queue_m_);
                            done_queue_.push_back([this, dialog] {
                                if (alive_ && dialog->get_root()) dialog->destroy();
                            });
                            dispatcher_.emit();
                        });
                    });
                    row->append(*del);
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


// ── New skill dialog (Phase 29) ─────────────────────────────────────────
// Skills dialog → "+ New skill": create a shared-library skill (SKILL.md)
// without leaving the client. POSTs /api/skills; on success the parent
// skills dialog is closed (user reopens to see the new entry — the dialog
// is modal and cheap).
void HiveWindow::show_new_skill_dialog(Gtk::Window* parent) {
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("New skill");
    install_dialog_titlebar(*dialog, "New skill");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button("Create", Gtk::ResponseType::OK);
    dialog->set_default_response(Gtk::ResponseType::OK);
    dialog->set_default_size(560, 420);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_margin(12);
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);

    auto* name = Gtk::make_managed<Gtk::Entry>();
    name->set_placeholder_text("kebab-case-name");
    auto* desc = Gtk::make_managed<Gtk::Entry>();
    desc->set_placeholder_text("One-line description (when to use this skill)");
    auto* content = Gtk::make_managed<Gtk::TextView>();
    content->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    content->set_monospace(true);
    auto* cscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    cscroll->set_child(*content);
    cscroll->set_vexpand(true);
    cscroll->add_css_class("hv-prompt-frame");
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
    add_row("Description", desc, 1);
    add_row("Instructions", cscroll, 2);
    grid->attach(*err, 1, 3);
    dialog->get_content_area()->append(*grid);

    dialog->signal_response().connect(
        [this, dialog, parent, name, desc, content, err](int r) {
            if (r != (int)Gtk::ResponseType::OK) { dialog->destroy(); return; }
            try {
                api_.create_skill(name->get_text(), desc->get_text(),
                                  content->get_buffer()->get_text());
                dialog->destroy();
                if (parent && parent->get_root()) parent->destroy();
            } catch (const std::exception& e) {
                err->set_text(e.what());   // keep the dialog open
            }
        });
    dialog->present();
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
    // Phase 72: host agents run bare-metal with sudo — make it unmissable.
    if (agent.value("host_agent", false)) {
        auto* hl = Gtk::make_managed<Gtk::Label>(
            "⚠ HOST AGENT — bare-metal with sudo, NO sandbox");
        hl->add_css_class("error-label");
        hl->set_halign(Gtk::Align::START);
        outer->append(*hl);
    }
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
            // v2: container lifecycle lives on the worker; the roster row
            // carries the live brain state (heartbeat-fed).
            try {
                for (auto& a : api_.list_agents().value("agents", json::array()))
                    if (a.value("id", "") == agent_id) { st = a; break; }
                if (st.is_null()) err = "agent not found";
            } catch (const std::exception& e) { err = e.what(); }
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
                const std::string state = st.value("state", "down");
                const bool running = (state != "down" && state != "crashed"
                                      && !state.empty());
                auto* sl = Gtk::make_managed<Gtk::Label>(
                    running ? "\u25CF " + state : "\u25CB " + state);
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
                ck("worker", st.value("worker", ""));
                ck("model", st.value("model", ""));
                auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto act = [&](const char* label, bool sensitive, bool start) {
                    auto* b = Gtk::make_managed<Gtk::Button>(label);
                    b->set_sensitive(sensitive);
                    b->signal_clicked().connect([this, box, dialog, agent_id, start] {
                        while (auto* c = box->get_first_child()) box->remove(*c);
                        run_async([this, agent_id, start] {
                            std::string err;
                            try { start ? (void)api_.agent_start(agent_id)
                                        : (void)api_.agent_stop(agent_id); }
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
    // Phase 58 follow-up: the title-bar button opens the Scheduled Tasks
    // MANAGER (same as the agent context menu route) — Add lives inside
    // the manager now. No preselect: the user picks the agent in the
    // Add dialog.
    show_routines_dialog("");
}

// Schedule-a-task dialog (routines). Blocking create call on OK — same
// pattern as the provider/model fetch in the agent dialog — so server
// validation errors can be shown in-dialog without closing it.
void HiveWindow::show_task_dialog(const std::string& preselect_agent_id,
                                  const nlohmann::json* existing,
                                  std::function<void()> on_saved) {
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
    once_time->set_placeholder_text("HH:MM UTC");
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
    daily_time->set_placeholder_text("HH:MM UTC (e.g. 09:00)");
    daily_time->set_max_width_chars(10);
    stack->add(*daily_time, "daily", "Daily");

    // weekly: time entry + weekday checkboxes
    auto* weekly_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto* weekly_time = Gtk::make_managed<Gtk::Entry>();
    weekly_time->set_placeholder_text("HH:MM UTC (e.g. 09:00)");
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
    // UTC reminder (2026-08-27): schedules fire on the UTC clock, and the
    // user must enter times accordingly — say so right under the fields.
    auto* utc_note = Gtk::make_managed<Gtk::Label>("All times are UTC (server clock)");
    utc_note->add_css_class("dim-label");
    utc_note->add_css_class("caption");
    utc_note->set_halign(Gtk::Align::START);
    grid->attach(*utc_note, 1, 5);
    grid->attach(*err, 1, 6);
    dialog->get_content_area()->append(*grid);

    dialog->signal_response().connect(
        [this, dialog, name, agent_dd, agent_ids, prompt, type_dd,
         once_date, once_time, every, daily_time, weekly_time, day_checks, err,
         editing, er, on_saved](int r) {
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
                // Phase 58: let the caller refresh (the Scheduled Tasks
                // list reloads itself after Add/Edit via this hook).
                if (on_saved) on_saved();
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
void HiveWindow::show_routines_dialog(const std::string& preselect_agent_id) {
    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title("Scheduled Tasks");
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
    // Phase 58: Add folds into the manager — one Scheduled Tasks entry
    // point (preselects the agent whose menu opened this).
    auto* add_btn = Gtk::make_managed<Gtk::Button>("\u2795 Add task\u2026");
    add_btn->set_halign(Gtk::Align::START);
    vbox->append(*add_btn);
    win->set_child(*vbox);

    auto reload = std::make_shared<std::function<void()>>();
    add_btn->signal_clicked().connect(
        [this, preselect_agent_id, reload] {
            show_task_dialog(preselect_agent_id, nullptr,
                             [reload] { if (*reload) (*reload)(); });
        });
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
                        // Lead with the owning agent — tasks from
                        // different agents were indistinguishable.
                        std::string sum = agent_name_of(agents_,
                                                        r.value("agent_id", ""));
                        // agent_name_of falls back to the raw id — show a
                        // readable label for deleted agents instead.
                        if (sum == r.value("agent_id", ""))
                            sum = "(former agent)";
                        if (!r.value("schedule_text", "").empty())
                            sum += "  ·  " + r.value("schedule_text", "");
                        sum += "  ·  next: " + r.value("next_fire_at", std::string("—"));
                        auto* sub = Gtk::make_managed<Gtk::Label>(sum);
                        sub->set_halign(Gtk::Align::START); sub->add_css_class("dim-label");
                        text->append(*nm); text->append(*sub);

                        auto* sw = Gtk::make_managed<Gtk::Switch>();
                        sw->set_active(r.value("enabled", true));
                        sw->set_valign(Gtk::Align::CENTER);
                        sw->set_tooltip_text(r.value("enabled", true)
                            ? "Enabled — click to pause this task"
                            : "Paused — click to resume this task");
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
                            // on_saved: reload the list after Save lands
                            // (it previously stayed stale until reopen).
                            show_task_dialog("", &rcopy,
                                             [reload] { if (*reload) (*reload)(); });
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
        // Phase 43: /api/models is hub-owned and FLAT again —
        // {models:[{name,provider,model_id,...}], default_model}. The hub
        // injects the provider config at dispatch, so any model runs on
        // any machine; machine choice is pure placement.
        std::set<std::string> provs;
        for (auto& m : api_.models().value("models", nlohmann::json::array())) {
            const std::string n = m.value("name", "");
            if (n.empty()) continue;
            const std::string p = m.value("provider", "");
            model_entries.push_back({n, p, n + "  (" + p + ")"});
            provs.insert(p);
        }
        default_model = api_.models().value("default_model", "");
        provider_names.assign(provs.begin(), provs.end());
    } catch (...) { /* server unreachable - dropdowns stay empty */ }

    std::vector<Glib::ustring> prov_items(provider_names.begin(), provider_names.end());
    auto* provider = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create(prov_items));
    auto* model = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({}));

    // Repopulate the model list for the selected provider. First entry is
    // always "(hub default)" → sends "" (server picks the default model).
    auto* model_names = new std::vector<std::string>();   // owned via dialog lifetime below
    auto refill_models = [model, model_entries, model_names, default_model](const std::string& prov) {
        std::vector<Glib::ustring> items;
        model_names->clear();
        items.push_back(default_model.empty() ? "(hub default)"
                                              : "(hub default: " + default_model + ")");
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
    // 2026-10-01: thinking-level override. "Inherit from model" ("") means
    // the hub uses the model's own thinking_level at dispatch; otherwise the
    // agent-level value wins. The hub validates (default|none|low|high|max).
    const std::vector<Glib::ustring> kThinkItems{
        "Inherit from model", "None", "Low", "High", "Max"};
    const std::vector<std::string> kThinkValues{"", "none", "low", "high", "max"};
    auto* think_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create(kThinkItems));
    think_dd->set_selected(0);
    if (editing) {
        const std::string cur_think =
            existing->value("thinking_level", "");
        for (size_t i = 0; i < kThinkValues.size(); ++i)
            if (kThinkValues[i] == cur_think) { think_dd->set_selected((guint)i); break; }
    }

    // Phase 30: worker placement. Query the enrolled hive-workers and offer
    // the ONLINE ones as execution targets. An offline rig can't start a
    // container, so it is hidden from the list — and when no worker is
    // online at all, the row is omitted entirely (creation stays local).
    // Exception: when editing an agent that already lives on a worker that
    // has since gone offline, that worker is shown flagged "(offline)" so
    // the agent can be moved back to the server.
    struct WorkerEntry { std::string name; bool online; std::string caps; };
    std::vector<WorkerEntry> all_workers;
    try {
        for (auto& w : api_.list_workers().value("workers", nlohmann::json::array())) {
            // /api/workers returns "caps" as a JSON OBJECT (the hub parses the
            // stored string before serialising). value("caps", "{}") THROWS
            // type_error on that — and the catch below then silently hid the
            // whole worker row, so no worker (not even a lone online one)
            // could be selected. Accept either shape and normalise to string.
            std::string caps = "{}";
            if (w.contains("caps"))
                caps = w["caps"].is_string() ? w["caps"].get<std::string>()
                                             : w["caps"].dump();
            all_workers.push_back({w.value("name", ""), w.value("online", false),
                                   std::move(caps)});
        }
    } catch (...) { /* server unreachable — worker row stays hidden */ }
    const std::string cur_worker =
        existing ? existing->value("worker", "") : "";
    auto* worker_names = new std::vector<std::string>();  // dialog lifetime
    auto* worker = Gtk::make_managed<Gtk::DropDown>();
    {
        std::vector<Glib::ustring> items;
        // v2 fix (2026-08-30): no "local" entry -- the hub cannot run
        // agents itself; only enrolled workers are valid placements.
        // (Creating on "local" produced a zombie agent stuck at (init).)
        for (auto& w : all_workers) {
            if (!w.online && w.name != cur_worker) continue;
            worker_names->push_back(w.name);
            items.push_back(w.online ? w.name : w.name + "  (offline)");
        }
        worker->set_model(Gtk::StringList::create(items));
        guint sel = 0;
        for (size_t i = 0; i < worker_names->size(); ++i)
            if ((*worker_names)[i] == cur_worker) { sel = (guint)i; break; }
        worker->set_selected(sel);
    }
    const bool show_worker_row = !worker_names->empty();

    // Phase 35+72: agent TYPE — the dialog's first choice. Standard and
    // GPU workhorse are containers; Host is bare-metal with sudo. GPU is
    // only offered when the selected worker advertises a gpu cap, Host
    // only with a host_agents cap (hello frame) — the hub enforces both.
    // The list is REBUILT on worker change; type_vals maps row -> value.
    const bool was_host = existing && existing->value("host_agent", false);
    auto* type_dd = Gtk::make_managed<Gtk::DropDown>();
    auto* type_vals = new std::vector<std::string>();   // dialog lifetime
    const std::string cur_type = editing ? existing->value("type", "") : "";
    auto worker_has_cap = [all_workers](const std::string& wname,
                                        const char* key) {
        for (auto& w : all_workers) {
            if (w.name != wname) continue;
            try {
                auto c = nlohmann::json::parse(w.caps);
                return c.is_object() && c.contains(key);
            } catch (...) { return false; }
        }
        return false;
    };
    auto rebuild_types = [type_dd, type_vals, worker, worker_names,
                          worker_has_cap, was_host] {
        const auto sel = worker->get_selected();
        const std::string wname =
            sel < worker_names->size() ? (*worker_names)[sel] : "";
        const std::string prev =
            type_dd->get_selected() < type_vals->size()
                ? (*type_vals)[type_dd->get_selected()] : "";
        std::vector<Glib::ustring> items;
        type_vals->clear();
        items.push_back("Standard Container");
        type_vals->push_back("");
        if (worker_has_cap(wname, "gpu")) {
            items.push_back("GPU Container (render/compute)");
            type_vals->push_back("gpu");
        }
        if (worker_has_cap(wname, "host_agents") || was_host) {
            // The ⚠ flags the danger BEFORE selection; the red warning
            // block below the form does the heavy lifting after it.
            items.push_back("Bare-Metal ⚠ (host — sudo, NO sandbox)");
            type_vals->push_back("host");
        }
        type_dd->set_model(Gtk::StringList::create(items));
        guint pick = 0;
        for (size_t i = 0; i < type_vals->size(); ++i)
            if ((*type_vals)[i] == prev) { pick = (guint)i; break; }
        type_dd->set_selected(pick);
    };
    worker->property_selected().signal_changed().connect(
        [rebuild_types] { rebuild_types(); });
    rebuild_types();   // initial list for the preselected worker
    // Edit-mode preselect: the host flag wins over the gpu type.
    const std::string want_type = was_host ? "host" : cur_type;
    if (!want_type.empty())
        for (size_t i = 0; i < type_vals->size(); ++i)
            if ((*type_vals)[i] == want_type) {
                type_dd->set_selected((guint)i);
                break;
            }

    // Phase 69: per-agent container resource caps. Blank = worker default
    // (4g / 2 cpus). Edit semantics mirror the web UI: clearing a
    // previously-set value resets it to the worker default.
    auto* memory_entry = Gtk::make_managed<Gtk::Entry>();
    memory_entry->set_placeholder_text("worker default (4g) — e.g. 32g, 1536m");
    const std::string cur_memory = editing ? existing->value("memory", "") : "";
    if (!cur_memory.empty()) memory_entry->set_text(cur_memory);
    auto* cpus_entry = Gtk::make_managed<Gtk::Entry>();
    cpus_entry->set_placeholder_text("worker default (2) — e.g. 12, 1.5");
    const double cur_cpus = editing ? existing->value("cpus", 0.0) : 0.0;
    if (cur_cpus > 0) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.4g", cur_cpus);
        cpus_entry->set_text(buf);
    }

    // Phase 72: host agents are the "Host" entry in the Type dropdown.
    auto* appr = Gtk::make_managed<Gtk::CheckButton>("Require my approval for commands");
    if (existing) {
        appr->set_active(existing->value("require_approval", false));
    }

    // Phase 72: Host is a TYPE (dropdown at the top). The red warning +
    // typed ack live at the bottom and appear only while Host is
    // selected — see refresh_type_ui after the layout below.
    auto* host_warn = Gtk::make_managed<Gtk::Label>(
        "⚠ HOST AGENT: runs DIRECTLY on the worker machine as a real user "
        "with passwordless sudo — NO container, NO sandbox. It can install "
        "software, modify system files, and DESTROY the machine — through "
        "error, hallucination, or prompt injection. Only use this on a "
        "machine you don't mind losing.\n"
        "Type I UNDERSTAND to enable:");
    host_warn->set_wrap(true);
    host_warn->set_xalign(0);
    host_warn->add_css_class("error-label");
    auto* host_ack = Gtk::make_managed<Gtk::Entry>();
    host_ack->set_placeholder_text("I UNDERSTAND");

    // Avatar picker: the 20 department-head SVGs (10 depts x male/female).
    // "Auto" (default) hashes the agent name into the set. ToggleButton
    // group = radio behavior. Layout: full-width Auto row, then dept pairs
    // side by side, two departments per row.
    auto* avatar_grid = Gtk::make_managed<Gtk::Grid>();
    avatar_grid->set_column_spacing(6);
    avatar_grid->set_row_spacing(6);
    auto* chosen_avatar = new std::string(
        existing ? existing->value("avatar", "") : "");   // freed in response lambda
    auto* avatar_tiles = new std::vector<
        std::pair<std::string, Gtk::ToggleButton*>>();    // freed likewise
    Gtk::ToggleButton* group_leader = nullptr;
    auto add_tile = [&](const std::string& val, Gtk::Widget* content,
                        const std::string& tip, int col, int row, int width = 1) {
        auto* b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_child(*content);
        b->add_css_class("avatar-pick");
        b->set_tooltip_text(tip);
        avatar_tiles->push_back({val, b});
        if (!group_leader) { group_leader = b; if (chosen_avatar->empty()) b->set_active(true); }
        else { b->set_group(*group_leader); if (val == *chosen_avatar) b->set_active(true); }
        b->signal_toggled().connect([b, chosen_avatar, val] {
            if (b->get_active()) *chosen_avatar = val;
        });
        avatar_grid->attach(*b, col, row, width);
    };
    {
        auto* lbl = Gtk::make_managed<Gtk::Label>("Auto  —  pick from the agent's name");
        lbl->add_css_class("avatar-pick-auto");
        add_tile("", lbl, "Auto  —  pick an avatar based on the agent's name",
                 0, 0, 4);
    }
    // "it-support-female" -> "IT Support (female)"
    auto pretty = [](const std::string& v) {
        std::string p = v;
        bool female = false;
        if (p.size() > 7 && p.compare(p.size() - 7, 7, "-female") == 0) {
            female = true;
            p.resize(p.size() - 7);
        }
        for (auto& c : p) if (c == '-') c = ' ';
        if (p == "it support") p = "IT Support";
        else if (!p.empty()) p[0] = (char)std::toupper((unsigned char)p[0]);
        return female ? p + " (female)" : p + " (male)";
    };
    const auto& avatars = avatar_names();
    for (size_t i = 0; i < avatars.size(); ++i) {
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set(avatar_pixbuf(avatars[i], 64));
        img->set_pixel_size(32);
        // pairs stay together: cols 0-1 = dept A, cols 2-3 = dept B
        add_tile(avatars[i], img, pretty(avatars[i]),
                 (int)(i % 4), (int)(1 + i / 4));
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
        return l;
    };
    int row_i = 0;
    if (!editing) add_row("Template", tpl, row_i++);
    Gtk::Label* mem_lbl = nullptr;
    Gtk::Label* cpu_lbl = nullptr;
    if (show_worker_row) {
        add_row("Worker", worker, row_i++);  // FIRST: Type depends on it
        add_row("Type", type_dd, row_i++);
    }
    add_row("Name", name, row_i++);
    add_row("Role", role, row_i++);
    add_row("Provider", provider, row_i++);
    add_row("Model", model, row_i++);
    add_row("Thinking", think_dd, row_i++);   // 2026-10-01
    if (show_worker_row) {
        mem_lbl = add_row("Memory cap", memory_entry, row_i++);  // Phase 69
        cpu_lbl = add_row("CPU cap", cpus_entry, row_i++);       // Phase 69
    }
    else if (!editing) {
        // No worker online: creation is impossible (the hub rejects unknown
        // workers) -- explain and disable OK.
        auto* nowork = Gtk::make_managed<Gtk::Label>(
            "No hive-worker online - enroll or connect one first");
        nowork->add_css_class("dim-label");
        nowork->set_hexpand(true);
        add_row("Worker", nowork, row_i++);
        dialog->set_response_sensitive((int)Gtk::ResponseType::OK, false);
    }
    // (no Workspace row — always containerized)
    add_row("Avatar", avatar_grid, row_i++);
    grid->attach(*appr, 1, row_i);
    ++row_i;
    grid->attach(*host_warn, 1, row_i);
    ++row_i;
    grid->attach(*host_ack, 1, row_i);

    // Phase 72: type-driven visibility. Host selected -> red warn + ack
    // shown, approvals forced on, and the container-only Memory/CPU rows
    // hidden (meaningless off-container). Worker switches rebuild the
    // type list, whose set_selected refires this via property_selected.
    auto refresh_type_ui = [type_dd, type_vals, host_warn, host_ack, appr,
                            memory_entry, cpus_entry, mem_lbl, cpu_lbl] {
        const bool host = type_dd->get_selected() < type_vals->size() &&
                          (*type_vals)[type_dd->get_selected()] == "host";
        host_warn->set_visible(host);
        host_ack->set_visible(host);
        if (host) appr->set_active(true);   // host agents are gated
        memory_entry->set_visible(!host);
        cpus_entry->set_visible(!host);
        if (mem_lbl) mem_lbl->set_visible(!host);
        if (cpu_lbl) cpu_lbl->set_visible(!host);
    };
    type_dd->property_selected().signal_changed().connect(refresh_type_ui);
    refresh_type_ui();   // initial state (Host preselected when editing)

    dialog->get_content_area()->append(*grid);
    const std::string edit_id = editing ? existing->value("id", "") : "";
    dialog->signal_response().connect([this, dialog, name, role, model, model_names, worker, worker_names, appr, chosen_avatar, edit_id, templates_json, avatar_tiles, type_dd, type_vals, cur_type, memory_entry, cpus_entry, cur_memory, cur_cpus, editing, think_dd, kThinkValues, existing, host_ack, was_host](int r) {
        if (r == (int)Gtk::ResponseType::OK) {
            std::string n = name->get_text();
            if (!n.empty()) {
                std::string ws = "shared";
                const auto msel = model->get_selected();
                std::string m = msel < model_names->size() ? (*model_names)[msel] : "";
                const auto wsel = worker->get_selected();
                const std::string wk = wsel < worker_names->size() ? (*worker_names)[wsel] : "";
                const std::string av = *chosen_avatar;
                // Phase 35+72: the Type list carries values in parallel
                // ("" standard / "gpu" / "host"). Host is its own flag,
                // so ty stays "" for it; Standard after gpu = "default"
                // (clears).
                const std::string tval =
                    type_dd->get_selected() < type_vals->size()
                        ? (*type_vals)[type_dd->get_selected()] : "";
                const std::string ty =
                    tval == "gpu" ? "gpu"
                    : tval == "host" ? ""
                    : (!cur_type.empty() ? "default" : "");
                // Phase 69: blank on create = worker default; blank on
                // edit with a previously-set value = explicit reset
                // ("default" / 0). Garbage cpus text = keep (-1).
                const std::string mem_in = memory_entry->get_text();
                std::string mem = mem_in;
                if (mem.empty() && editing && !cur_memory.empty())
                    mem = "default";
                double cp = -1;   // keep/default
                const std::string cpu_in = cpus_entry->get_text();
                if (!cpu_in.empty()) {
                    try { cp = std::stod(cpu_in); }
                    catch (...) { cp = -1; }
                    if (cp < 0) cp = -1;
                } else if (editing && cur_cpus > 0) {
                    cp = 0;   // cleared -> reset to worker default
                }
                if (tval == "host") {
                    // container caps are meaningless off-container:
                    // hidden rows reset any previously-set values
                    if (editing && !cur_memory.empty()) mem = "default";
                    if (editing && cur_cpus > 0) cp = 0;
                }
                // 2026-10-01: edit semantics — unchanged inherit = keep
                // (""), explicit Inherit after an override = "default"
                // (clear), a level = set. Create always sends the value.
                const auto tsel = think_dd->get_selected();
                std::string think = tsel < kThinkValues.size()
                                        ? kThinkValues[tsel] : "";
                if (editing && think.empty() &&
                    !existing->value("thinking_level", "").empty())
                    think = "default";
                // Phase 72: enabling a host agent needs the typed ack —
                // validated client-side for UX, re-validated by the hub.
                int host = -1;   // -1 = omit (keep)
                std::string host_ack_str;
                if (tval == "host" && !was_host) {
                    host_ack_str = host_ack->get_text();
                    if (host_ack_str != "I UNDERSTAND") {
                        auto* err = Gtk::make_managed<Gtk::MessageDialog>(
                            *dialog, "Type I UNDERSTAND in the red box to "
                            "enable a host agent.", false,
                            Gtk::MessageType::WARNING,
                            Gtk::ButtonsType::OK, true);
                        err->signal_response().connect(
                            [err](int) { err->destroy(); });
                        err->present();
                        return;   // stay in the dialog
                    }
                    host = 1;
                } else if (tval == "host" && was_host) {
                    host = 1;   // keep; hub only re-asks if approvals drop
                    if (!appr->get_active())
                        host_ack_str = host_ack->get_text();
                } else if (was_host) {
                    host = 0;   // clearing — safe direction, no ack
                }
                delete chosen_avatar;
                delete model_names;
                delete worker_names;
                delete templates_json;
                delete avatar_tiles;
                delete type_vals;
                run_async([this, n, r2 = role->get_text().raw(),
                           m, ws, av, wk, ty, edit_id,
                           ap = appr->get_active(), mem, cp, think,
                           host, host_ack = host_ack_str] {
                    if (edit_id.empty())
                        api_.create_agent(n, r2, m, ws, ap, av, wk, ty, mem, cp, think,
                                          host, host_ack);
                    else
                        api_.update_agent(edit_id, n, r2, m, ws, ap, av, wk, ty, mem, cp, think,
                                          host, host_ack);
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
            delete model_names;
            delete worker_names;
            delete templates_json;
            delete avatar_tiles;
            delete type_vals;
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
                            rendered_msg_ids_.clear();
                            while (auto* c = messages_box_.get_first_child())
                                messages_box_.remove(*c);
                                live_row_ = nullptr; live_label_ = nullptr; live_agent_key_.clear();
                        load_earlier_btn_ = nullptr; has_more_history_ = false;
                        oldest_message_id_.clear(); loading_earlier_ = false;
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

// Phase 57: agent-context-menu Stop — cancels the agent's live turn(s)
// in whatever thread it's working in (the old title-bar button cancelled
// the whole THREAD, which was confusing in shared threads).
void HiveWindow::on_stop_agent(const nlohmann::json& agent) {
    const std::string id = agent.value("id", "");
    const std::string name = agent.value("name", id);
    if (id.empty()) return;
    set_status("Cancelling " + name + "\u2026");
    run_async([this, id, name] {
        int cancelled = 0;
        try {
            cancelled = api_.cancel_agent(id).value("cancelled", 0);
        } catch (...) {}
        std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this, name, cancelled] {
            if (!alive_) return;
            set_status(cancelled > 0
                ? "Cancel sent to " + name
                : name + " wasn\u2019t working");
        });
        dispatcher_.emit();
    });
}

// Phase 56: right-click a thread row → context menu (Clear / Delete).
void HiveWindow::show_thread_menu(Gtk::Widget& anchor,
                                  const nlohmann::json& thread,
                                  double x, double y) {
    const std::string tid = thread.value("id", "");
    if (tid.empty()) return;
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
    const std::string title = thread.value("title", "");
    mk("\U0001F9F9 Clear history\u2026", nullptr,
       [this, tid, title] { on_clear_thread(tid, title); });
    // Phase 67: Move to group (folders — one group per thread). Lists
    // every custom group AND the three smart buckets, so a mis-tagged
    // thread (e.g. a routine stuck with dm kind) can be forced where
    // it belongs; ✓ marks the current bucket.
    {
        const std::string cur = thread_bucket_id(thread);
        auto mk_move = [&](const std::string& bid, const std::string& name) {
            const std::string label =
                (bid == cur ? "\u2713 " : "   ") + name;
            mk(label.c_str(), nullptr,
               [this, tid, bid] { move_thread_to(tid, bid); });
        };
        auto* lbl = Gtk::make_managed<Gtk::Label>("Move to group");
        lbl->add_css_class("hv-group-meta");
        lbl->set_halign(Gtk::Align::START);
        lbl->set_margin(4);
        box->append(*lbl);
        for (auto& g : thread_groups_) {
            const std::string gid = g.value("id", "");
            // An empty bid would falsely match unbucketed threads in
            // the checkmark comparison.
            if (gid.empty()) continue;
            mk_move(gid, "\U0001F4C1 " + g.value("name", ""));
        }
        mk("\u2795 New group\u2026", nullptr,
           [this, tid] { show_group_name_dialog("New group", "", "", tid); });
        // Smart buckets — hub-derived defaults, forceable per thread.
        mk_move("smart:you", "You & agents");
        mk_move("smart:chatter", "Agent chatter");
        mk_move("smart:routines", "Routines");
        mk("\u21A9 Smart default", nullptr,
           [this, tid] { move_thread_to(tid, ""); });
    }
    // Phase 44: the User Inbox can be cleared but never deleted.
    if (thread.value("kind", "") != "user")
        mk("\U0001F5D1 Delete thread\u2026", "destructive-action",
           [this, tid] { on_delete_thread(tid); });
    pop->set_child(*box);
    pop->set_parent(anchor);
    Gdk::Rectangle rect((int)x, (int)y, 1, 1);
    pop->set_pointing_to(rect);
    pop->signal_closed().connect([pop] { pop->unparent(); });
    pop->popup();
}

void HiveWindow::on_clear_thread(const std::string& thread_id,
                                 const std::string& title) {
    if (thread_id.empty()) return;
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Clear thread history?");
    install_dialog_titlebar(*dialog, "Clear thread history?");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    auto* clr = dialog->add_button("Clear", Gtk::ResponseType::OK);
    clr->add_css_class("destructive-action");
    auto* lbl = Gtk::make_managed<Gtk::Label>(
        "Clear every message in \u201C" + (title.empty() ? "this thread" : title) +
        "\u201D? The thread stays; the history is gone for good.");
    lbl->set_margin(18);
    lbl->set_wrap(true);
    lbl->set_max_width_chars(42);
    dialog->get_content_area()->append(*lbl);
    const std::string tid = thread_id;
    dialog->signal_response().connect([this, dialog, tid](int r) {
        dialog->destroy();
        if (r != Gtk::ResponseType::OK) return;
        run_async([this, tid] {
            api_.clear_thread(tid);
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, tid] {
                if (!alive_) return;
                // No activity left — any unread dot for it dies too.
                last_seen_activity_.erase(tid);
                if (current_thread_ == tid) {
                    // Open view on the cleared thread: wipe it back to
                    // the empty state (thread itself survives).
                    last_message_id_.clear();
                    rendered_msg_ids_.clear();
                    rendered_.clear();
                    while (auto* ch = messages_box_.get_first_child())
                        messages_box_.remove(*ch);
                    live_row_ = nullptr; live_label_ = nullptr;
                    live_agent_key_.clear();
                    load_earlier_btn_ = nullptr; has_more_history_ = false;
                    oldest_message_id_.clear(); loading_earlier_ = false;
                    poll_chat();
                }
                refresh_threads();
            });
            dispatcher_.emit();
        });
    });
    dialog->present();
}

void HiveWindow::on_delete_thread(const std::string& thread_id) {
    if (thread_id.empty()) return;
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
    std::string tid = thread_id;
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
                    current_dm_agent_.clear();  // the DM is gone — re-clicking
                                                // the agent must recreate it
                    last_message_id_.clear();
                    rendered_msg_ids_.clear();
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
            {"thread_groups", thread_groups_},
            {"rendered_count", rendered_.size()},
            {"entry_sensitive", entry_.get_sensitive()},
            {"attach_sensitive", attach_btn_.get_sensitive()},
            {"pending_attachments", pending_attachments_.size()},
            {"connected", !api_.api_key().empty()},
            // Rendered roster rows in visual order: worker group headers
            // (with online/collapsed flags) interleaved with agent rows.
            {"roster_rows", [this] {
                nlohmann::json rows = nlohmann::json::array();
                for (int i = 0; auto* row = roster_list_.get_row_at_index(i); ++i) {
                    if (auto* id = static_cast<std::string*>(
                            row->get_data("agent_id"))) {
                        rows.push_back({{"type", "agent"}, {"id", *id},
                            {"offline", row->has_css_class("hv-offline")}});
                    } else if (auto* w = static_cast<std::string*>(
                            row->get_data("worker_name"))) {
                        bool online = false;
                        for (auto& wi : workers_)
                            if (wi.value("name", "") == *w)
                                online = wi.value("online", false);
                        rows.push_back({{"type", "worker"}, {"name", *w},
                            {"online", online},
                            {"collapsed",
                             collapsed_workers_.count(*w) > 0}});
                    }
                }
                return rows;
            }()},
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
        rendered_msg_ids_.clear();
        while (auto* c = messages_box_.get_first_child())
            messages_box_.remove(*c);
            live_row_ = nullptr; live_label_ = nullptr; live_agent_key_.clear();
                        load_earlier_btn_ = nullptr; has_more_history_ = false;
                        oldest_message_id_.clear(); loading_earlier_ = false;
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
                rendered_msg_ids_.clear();
                while (auto* c = messages_box_.get_first_child())
                    messages_box_.remove(*c);
                    live_row_ = nullptr; live_label_ = nullptr; live_agent_key_.clear();
                        load_earlier_btn_ = nullptr; has_more_history_ = false;
                        oldest_message_id_.clear(); loading_earlier_ = false;
                rendered_.clear();
                set_composer_enabled(id != "user");
                select_thread_row(id);
                poll_chat();
                return nlohmann::json{{"selected", id}};
            }
        }
        return nlohmann::json{{"error", "thread not found: " + id}};
    });
}

nlohmann::json HiveWindow::ui_select_agent(const std::string& name_or_id) {
    return invoke_on_ui([this, name_or_id] {
        // Resolve display names (how agents/users refer to each other).
        std::string id = name_or_id;
        for (auto& a : agents_)
            if (a.value("name", "") == name_or_id) {
                id = a.value("id", "");
                break;
            }
        // Select the roster row so the real on_agent_selected() path
        // runs (DM reuse, composer re-enable, sidebar sync).
        for (auto* child = roster_list_.get_first_child(); child;
             child = child->get_next_sibling()) {
            auto* row = dynamic_cast<Gtk::ListBoxRow*>(child);
            if (!row) continue;
            auto* aid = static_cast<std::string*>(row->get_data("agent_id"));
            if (aid && *aid == id) {
                roster_list_.select_row(*row);
                return nlohmann::json{{"selected", id}};
            }
        }
        return nlohmann::json{{"error", "agent not found: " + name_or_id}};
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

nlohmann::json HiveWindow::ui_attach(const std::string& path) {
    return invoke_on_ui([this, path] {
        if (current_thread_.empty())
            return nlohmann::json{{"error", "no thread selected"}};
        if (path.empty()) {
            on_attach();   // real dialog (needs a human to pick)
            return nlohmann::json{{"dialog", true}};
        }
        attach_picked_file(path);   // headless: skip the dialog
        return nlohmann::json{{"attaching", path},
                              {"thread", current_thread_}};
    });
}

nlohmann::json HiveWindow::ui_stop() {
    return invoke_on_ui([this] {
        if (current_thread_.empty())
            return nlohmann::json{{"error", "no thread selected"}};
        // Thread-level cancel (the removed title-bar button's path) —
        // kept for the UiApi; human UI cancel is per-agent via the
        // roster context menu (Phase 57).
        std::string tid = current_thread_;
        set_status("Cancelling…");
        run_async([this, tid] {
            api_.cancel_thread(tid);
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this] {
                if (alive_) set_status("Cancelled");
            });
            dispatcher_.emit();
        });
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
            rendered_msg_ids_.clear();
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
