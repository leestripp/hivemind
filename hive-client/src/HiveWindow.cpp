#include "HiveWindow.h"

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
)";

std::string agent_name_of(const json& agents, const std::string& id) {
    for (auto& a : agents)
        if (a.value("id", "") == id) return a.value("name", id);
    return id;
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
    header_.pack_start(new_agent_btn_);
    header_.pack_start(new_thread_btn_);
    header_.pack_start(workflows_btn_);
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
        poll_chat();
        poll_approvals();
        return alive_;
    }, 1500);

    set_status("Connecting to hive-server…");
    refresh_all();
}

HiveWindow::~HiveWindow() {
    alive_ = false;
    poll_timer_.disconnect();
}

// ── async plumbing ──────────────────────────────────────────────────────────

void HiveWindow::run_async(std::function<void()> work) {
    std::thread([this, work = std::move(work)]() mutable {
        try { work(); } catch (const std::exception& e) {
            std::string msg = e.what();
            {
                std::lock_guard<std::mutex> lk(queue_m_);
                done_queue_.push_back([this, msg] {
                    if (alive_) set_status("Error: " + msg);
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
                        m.value("body", ""));
                    last_message_id_ = m.value("id", last_message_id_);
                }
                if (!arr.empty()) {
                    // scroll to bottom
                    auto adj = chat_scroll_.get_vadjustment();
                    adj->set_value(adj->get_upper() - adj->get_page_size());
                }
                bool busy = act.value("busy", false);
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
                                       const std::string& body) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    if (sender_kind == "user") box->add_css_class("hv-msg-user");
    else if (sender_kind == "agent") box->add_css_class("hv-msg-agent");
    else box->add_css_class("hv-msg-system");

    auto* who = Gtk::make_managed<Gtk::Label>(sender_label);
    who->set_halign(Gtk::Align::START);
    who->add_css_class("hv-sender");
    auto* text = Gtk::make_managed<Gtk::Label>(body);
    text->set_halign(Gtk::Align::START);
    text->set_wrap(true);
    text->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    text->set_selectable(true);
    text->set_xalign(0);
    box->append(*who);
    box->append(*text);
    messages_box_.append(*box);
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
        json t = api_.create_thread("", {aid});
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            done_queue_.push_back([this, t] {
                if (!alive_) return;
                current_thread_ = t.value("id", "");
                last_message_id_.clear();
                while (auto* c = messages_box_.get_first_child())
                    messages_box_.remove(*c);
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
    while (auto* c = messages_box_.get_first_child())
        messages_box_.remove(*c);
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
    dialog->add_button("Create", Gtk::ResponseType::OK);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_margin(12);
    box->set_spacing(8);
    auto* title = Gtk::make_managed<Gtk::Entry>();
    title->set_placeholder_text("Title (optional)");
    box->append(*title);
    box->append(*Gtk::make_managed<Gtk::Label>("Participants:"));

    std::vector<std::pair<Gtk::CheckButton*, std::string>> picks;
    for (auto& a : agents_) {
        auto* cb = Gtk::make_managed<Gtk::CheckButton>(a.value("name", "?"));
        box->append(*cb);
        picks.emplace_back(cb, a.value("id", ""));
    }
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
