#pragma once

#include <gtkmm.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ApiClient.h"

namespace hive {

/**
 * HiveWindow — chat-first main window (HiveMind 2.0, Plan.md Phase 4).
 *
 * Left sidebar: agent roster + threads. Right: chat transcript with live
 * streaming (polled /activity + incremental /messages) and a composer.
 * Header: new-agent / new-thread buttons and the approvals inbox (badge
 * shows pending count).
 */
class HiveWindow : public Gtk::Window {
public:
    HiveWindow();
    ~HiveWindow() override;

    // ── UiApi hooks (Phase 11.5) — called from OmniNet worker threads. ────
    // Every method marshals onto the GTK main loop and drives the SAME code
    // paths the buttons use; the REST layer adds no parallel logic.
    nlohmann::json ui_state();                       // GET /ui/state
    nlohmann::json ui_messages();                    // GET /ui/messages
    nlohmann::json ui_create_thread(const std::string& title,
                                    const std::vector<std::string>& ids);
    nlohmann::json ui_select_thread(const std::string& id);
    nlohmann::json ui_send(const std::string& body);
    nlohmann::json ui_stop();
    nlohmann::json ui_delete_thread(const std::string& id);

private:
    /// Run fn on the GTK main thread and return its result (blocks caller).
    nlohmann::json invoke_on_ui(std::function<nlohmann::json()> fn);
    // ── connection config (Phase 9) ───────────────────────────────────────
    void load_config();          // ~/.config/hivemind/config.json
    void save_config();
    void apply_theme();        // push dark_mode_ into Gtk::Settings + retint icons
    void refresh_themed_icons();
    void set_button_icon(Gtk::Button& btn, const std::string& icon,
                         const std::string& tooltip);
    void maybe_show_connect_dialog();   // when no key is configured

    // ── data loading (worker threads → dispatcher → UI) ───────────────────
    void run_async(std::function<void()> work);
    void refresh_all();
    void refresh_threads();
    void poll_chat();           // messages + activity for current thread
    void poll_approvals();

    // ── UI callbacks ──────────────────────────────────────────────────────
    void on_agent_selected();
    void on_thread_selected();
    void on_send();
    void on_new_agent();
    void on_new_thread();
    void on_approvals();
    void on_stop();
    void on_delete_thread();

    // ── rendering helpers ─────────────────────────────────────────────────
    void rebuild_roster();
    void rebuild_thread_list();
    void append_message_bubble(const std::string& sender_kind,
                               const std::string& sender_label,
                               const std::string& body,
                               const std::string& created_at = "");
    void set_status(const std::string& text);

    ApiClient api_;
    std::string server_url_ = "http://127.0.0.1:8421";

    // state (GUI thread only)
    nlohmann::json agents_ = nlohmann::json::array();
    nlohmann::json threads_ = nlohmann::json::array();
    std::string current_thread_;
    std::string last_message_id_;
    int pending_approvals_ = 0;
    bool thread_busy_ = false;
    bool alive_ = true;
    std::vector<nlohmann::json> rendered_;   // bubbles actually on screen

    // async marshalling
    Glib::Dispatcher dispatcher_;
    std::mutex queue_m_;
    std::vector<std::function<void()>> done_queue_;

    // ── widgets ───────────────────────────────────────────────────────────
    Gtk::Box root_{Gtk::Orientation::VERTICAL};
    Gtk::HeaderBar header_;
    // Icon-only buttons (icons are GResource SVGs, re-tinted per theme).
    Gtk::Button new_agent_btn_;
    Gtk::Button new_thread_btn_;
    Gtk::Button approvals_btn_;          // icon + count label (approvals_label_)
    Gtk::Label approvals_label_;         // child of approvals_btn_
    Gtk::Button workflows_btn_;
    Gtk::Button theme_btn_;              // dark/light toggle
    Gtk::Button connect_btn_;
    Gtk::Button stop_btn_;
    Gtk::Button delete_thread_btn_;
    bool dark_mode_ = false;             // persisted in config.json

    Gtk::Paned paned_{Gtk::Orientation::HORIZONTAL};
    Gtk::Box sidebar_{Gtk::Orientation::VERTICAL};
    Gtk::Label roster_label_{"Agents"};
    Gtk::ScrolledWindow roster_scroll_;
    Gtk::ListBox roster_list_;
    Gtk::Label threads_label_{"Threads"};
    Gtk::ScrolledWindow threads_scroll_;
    Gtk::ListBox threads_list_;

    Gtk::Box chat_box_{Gtk::Orientation::VERTICAL};
    Gtk::Label status_label_{""};
    Gtk::ScrolledWindow chat_scroll_;
    Gtk::Box messages_box_{Gtk::Orientation::VERTICAL};
    Gtk::Box composer_{Gtk::Orientation::HORIZONTAL};
    Gtk::Entry entry_;
    Gtk::Button send_btn_;

    sigc::connection poll_timer_;

    // at most one connect dialog may exist — guards against the 1.5 s poll
    // timer spawning a fresh modal dialog on every 401 response
    Gtk::Dialog* connect_dialog_ = nullptr;
};

} // namespace hive
