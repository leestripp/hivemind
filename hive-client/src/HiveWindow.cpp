#include "HiveWindow.h"
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

// Stable per-agent palette index — avatar and name label share it so an
// agent is identifiable by color across a busy multi-agent thread.
int palette_index(const std::string& name) {
    return (int)(std::hash<std::string>{}(name) % 6);
}

// Circular avatar with the sender's initial, color-hashed per name so every
// agent gets a stable color. Swap the Gtk::Label for a Gtk::Image here when
// the roster gains avatar art — the bubble layout won't change.
Gtk::Label* make_avatar(const std::string& name, bool is_user) {
    std::string initial = "?";
    if (!name.empty()) {
        // first UTF-8 codepoint (good enough for ASCII names)
        initial = name.substr(0, 1);
        for (auto& ch : initial) ch = (char)std::toupper((unsigned char)ch);
    }
    auto* a = Gtk::make_managed<Gtk::Label>(initial);
    a->add_css_class("hv-avatar");
    if (is_user) a->add_css_class("hv-avatar-user");
    else a->add_css_class("hv-avatar-" + std::to_string(palette_index(name)));
    a->set_valign(Gtk::Align::START);
    return a;
}

} // namespace

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
    approvals_btn_.signal_clicked().connect(
        sigc::mem_fun(*this, &HiveWindow::on_approvals));
    // Phase 7-lite: the node-graph canvas stays in the web UI — the client
    // deep-links to it instead of reimplementing it.
    workflows_btn_.signal_clicked().connect([] {
        Gio::AppInfo::launch_default_for_uri("http://127.0.0.1:8421/");
    });
    connect_btn_.signal_clicked().connect(
        [this] { maybe_show_connect_dialog(); });
    stop_btn_.signal_clicked().connect([this] { on_stop(); });
    stop_btn_.set_sensitive(false);   // enabled while a turn is running
    stop_btn_.add_css_class("destructive-action");
    stop_btn_.set_tooltip_text("Cancel the running turn in this thread");
    delete_thread_btn_.signal_clicked().connect([this] { on_delete_thread(); });
    delete_thread_btn_.set_sensitive(false);  // enabled with a selection
    delete_thread_btn_.set_tooltip_text("Delete the selected thread");
    header_.pack_start(new_agent_btn_);
    header_.pack_start(new_thread_btn_);
    header_.pack_start(workflows_btn_);
    header_.pack_end(connect_btn_);
    header_.pack_end(delete_thread_btn_);
    header_.pack_end(stop_btn_);
    header_.pack_end(approvals_btn_);
    root_.append(header_);

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

    sidebar_.append(roster_label_);
    sidebar_.append(roster_scroll_);
    sidebar_.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    sidebar_.append(threads_label_);
    sidebar_.append(threads_scroll_);

    // ── chat area ─────────────────────────────────────────────────────────
    status_label_.set_halign(Gtk::Align::START);
    status_label_.set_margin(6);
    chat_scroll_.set_child(messages_box_);
    chat_scroll_.set_vexpand(true);
    entry_.set_hexpand(true);
    entry_.set_placeholder_text("Message the team…  (Enter to send)");
    entry_.signal_activate().connect([this] { on_send(); });
    send_btn_.signal_clicked().connect([this] { on_send(); });
    send_btn_.add_css_class("suggested-action");
    composer_.set_margin(6);
    composer_.set_spacing(6);
    composer_.append(entry_);
    composer_.append(send_btn_);

    chat_box_.append(status_label_);
    chat_box_.append(chat_scroll_);
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
        return alive_;
    }, 1500);

    load_config();
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
    f << json{{"server", server_url_}, {"api_key", api_.api_key()}}.dump(2);
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
            // Verify the credentials asynchronously BEFORE committing them:
            // previously an OK with a bad key saved it, called refresh_all(),
            // got a 401, and immediately reopened this dialog — forever.
            error->set_text("Checking connection…");
            error->set_visible(true);
            cancel_btn->set_sensitive(false);
            ok_btn->set_sensitive(false);
            server->set_sensitive(false);
            key->set_sensitive(false);
            std::thread([this, dialog, error, cancel_btn, ok_btn,
                         server, key, url, k] {
                std::string err;
                try {
                    ApiClient probe{url};
                    probe.set_api_key(k);
                    probe.list_agents();   // throws on network/auth failure
                } catch (const std::exception& e) {
                    err = e.what();
                }
                {
                    std::lock_guard<std::mutex> lk(queue_m_);
                    done_queue_.push_back(
                        [this, dialog, error, cancel_btn, ok_btn,
                         server, key, url, k, err] {
                            if (!alive_ || connect_dialog_ != dialog) return;
                            if (!err.empty()) {
                                error->set_text("Connection failed: " + err);
                                cancel_btn->set_sensitive(true);
                                ok_btn->set_sensitive(true);
                                server->set_sensitive(true);
                                key->set_sensitive(true);
                                return;   // keep the dialog open for a retry
                            }
                            server_url_ = url;
                            api_.set_base_url(server_url_);
                            api_.set_api_key(k);
                            save_config();
                            dialog->destroy();
                            refresh_all();
                        });
                }
                dispatcher_.emit();
            }).detach();
        });
    dialog->present();
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
                        m.value("created_at", ""));
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

void HiveWindow::poll_approvals() {
    run_async([this] {
        json ap = api_.list_approvals("pending");
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, ap] {
                if (!alive_) return;
                int n = (int)ap.value("approvals", json::array()).size();
                if (n != pending_approvals_) {
                    pending_approvals_ = n;
                    approvals_btn_.set_label(n > 0
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
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        auto* name = Gtk::make_managed<Gtk::Label>(a.value("name", "?"));
        name->set_halign(Gtk::Align::START);
        name->add_css_class("heading");
        auto* role = Gtk::make_managed<Gtk::Label>(a.value("role", ""));
        role->set_halign(Gtk::Align::START);
        role->set_ellipsize(Pango::EllipsizeMode::END);
        role->set_max_width_chars(28);
        role->add_css_class("dim-label");
        box->append(*name);
        box->append(*role);
        box->set_margin(4);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*box);
        row->set_data("agent_id", new std::string(a.value("id", "")),
                      [](void* p) { delete static_cast<std::string*>(p); });
        roster_list_.append(*row);
    }
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
        label->set_margin(6);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*label);
        row->set_data("thread_id", new std::string(t.value("id", "")),
                      [](void* p) { delete static_cast<std::string*>(p); });
        threads_list_.append(*row);
    }
}

void HiveWindow::append_message_bubble(const std::string& sender_kind,
                                       const std::string& sender_label,
                                       const std::string& body,
                                       const std::string& created_at) {
    rendered_.push_back({{"sender_kind", sender_kind},
                         {"sender_label", sender_label},
                         {"body", body},
                         {"created_at", created_at}});
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
    row->append(*make_avatar(sender_label, sender_kind == "user"));

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
    while (auto* c = messages_box_.get_first_child())
        messages_box_.remove(*c);
    rendered_.clear();
    poll_chat();
}

void HiveWindow::on_send() {
    std::string text = entry_.get_text();
    if (text.empty() || current_thread_.empty()) return;
    entry_.set_text("");
    std::string tid = current_thread_;
    run_async([this, tid, text] {
        api_.post_message(tid, text);
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this] { if (alive_) poll_chat(); });
        }
        dispatcher_.emit();
    });
}

void HiveWindow::on_new_agent() {
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("New agent");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button("Create", Gtk::ResponseType::OK);
    dialog->set_default_response(Gtk::ResponseType::OK);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_margin(12);
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);

    auto* name = Gtk::make_managed<Gtk::Entry>();
    name->set_placeholder_text("Name (e.g. Nova)");
    auto* role = Gtk::make_managed<Gtk::Entry>();
    role->set_placeholder_text("Role (e.g. research assistant)");
    auto* model = Gtk::make_managed<Gtk::Entry>();
    model->set_placeholder_text("Model (blank = store default)");
    auto* mode = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"shared (container)", "host (no container)"}));
    auto* appr = Gtk::make_managed<Gtk::CheckButton>("Require my approval for commands");

    auto add_row = [grid](const char* label, Gtk::Widget* w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::END);
        grid->attach(*l, 0, row);
        w->set_hexpand(true);
        grid->attach(*w, 1, row);
    };
    add_row("Name", name, 0);
    add_row("Role", role, 1);
    add_row("Model", model, 2);
    add_row("Workspace", mode, 3);
    grid->attach(*appr, 1, 4);

    dialog->get_content_area()->append(*grid);
    dialog->signal_response().connect([this, dialog, name, role, model, mode, appr](int r) {
        if (r == (int)Gtk::ResponseType::OK) {
            std::string n = name->get_text();
            if (!n.empty()) {
                std::string ws = mode->get_selected() == 1 ? "host" : "shared";
                run_async([this, n, r2 = role->get_text().raw(),
                           m = model->get_text().raw(), ws,
                           ap = appr->get_active()] {
                    api_.create_agent(n, r2, m, ws, ap);
                    {
                        std::lock_guard<std::mutex> lk(queue_m_);
                        done_queue_.push_back([this] { if (alive_) refresh_all(); });
                    }
                    dispatcher_.emit();
                });
            }
        }
        dialog->destroy();
    });
    dialog->present();
}

void HiveWindow::on_new_thread() {
    if (agents_.empty()) { set_status("Create an agent first"); return; }
    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("New thread");
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
        cb->signal_toggled().connect([create_btn, err, &picks_ref = picks] {
            bool any = false;
            for (auto& [b, i] : picks_ref) any = any || b->get_active();
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
