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
    nlohmann::json ui_toggle_theme();                // POST /ui/action/theme
    nlohmann::json ui_open_dialog(const std::string& name);  // test hook                // POST /ui/action/theme
    nlohmann::json ui_stop();
    nlohmann::json ui_delete_thread(const std::string& id);

private:
    /// Run fn on the GTK main thread and return its result (blocks caller).
    nlohmann::json invoke_on_ui(std::function<nlohmann::json()> fn);
    // ── connection config (Phase 9) ───────────────────────────────────────
    void load_config();          // ~/.config/hivemind/config.json
    void save_config();
    void apply_theme();        // push dark_mode_ into Gtk::Settings + retint icons
    void on_theme_toggle();
    void refresh_themed_icons();  // re-tints pixbufs in place (no reparenting!)
    void set_button_icon(Gtk::Button& btn, const std::string& icon,
                         const std::string& tooltip);
    // Persistent icon widgets, created ONCE in setup_icons() — retinted, never
    // reparented (reparenting a managed member widget can destroy it → crash).
    struct IconSlot { Gtk::Button* btn; Gtk::Image* img; std::string icon; };
    std::vector<IconSlot> icon_slots_;
    Gtk::Image* theme_icon_ = nullptr;   // swaps sun/moon per mode
    void setup_icons();
    void maybe_show_connect_dialog();   // when no key is configured

    // ── data loading (worker threads → dispatcher → UI) ───────────────────
    void run_async(std::function<void()> work);
    void refresh_all();
    void refresh_threads();
    void poll_chat();           // messages + activity for current thread
    void poll_approvals();
    void poll_roster();          // agents busy/state (Phase 27)
    std::string roster_sig_;   // last busy/state signature

    // ── UI callbacks ──────────────────────────────────────────────────────
    void on_agent_selected();
    void on_thread_selected();
    void on_attach();   // Phase 18: pick + upload files

    void on_send();
    void on_new_agent();
    void show_agent_dialog(const nlohmann::json* existing);  // nullptr = create
    void on_edit_agent(const nlohmann::json& agent);
    void on_delete_agent(const nlohmann::json& agent);
    void show_agent_screen(const nlohmann::json& agent);
    void show_agent_menu(Gtk::Widget& anchor, const nlohmann::json& agent,
                         double x, double y);
    void on_new_task();
    void show_task_dialog(const std::string& preselect_agent_id = "");
    void show_skills_dialog(const nlohmann::json& agent);   // Phase 14.2
    void show_inspector(const nlohmann::json& agent);       // Phase 14.4
    void load_inspector_container(Gtk::Box* box, Gtk::Window* dialog,
                                  const std::string& agent_id);
    void update_thread_title();
    void notify(const Glib::ustring& title, const Glib::ustring& body);
    /// Badge/notify pass over threads_ after a refresh (Phase 14.1).
    void process_thread_activity();
    void on_new_thread();
    void on_approvals();
    void on_stop();
    void on_learn_routine();   // Phase 20: distill thread into a routine
    void on_delete_thread();

    // ── rendering helpers ─────────────────────────────────────────────────
    void rebuild_roster();
    void rebuild_thread_list();
    void append_message_bubble(const std::string& sender_kind,
                               const std::string& sender_label,
                               const std::string& body,
                               const std::string& created_at = "",
                               const nlohmann::json& attachments
                                   = nlohmann::json::array());
    void set_status(const std::string& text);

    ApiClient api_;
    std::string server_url_ = "http://127.0.0.1:8421";

    // state (GUI thread only)
    nlohmann::json agents_ = nlohmann::json::array();
    nlohmann::json threads_ = nlohmann::json::array();
    std::string current_thread_;
    std::string last_message_id_;
    int pending_approvals_ = -1;   // -1 = first poll (don't notify on load)
    bool thread_busy_ = false;
    // Phase 14.1 — attention: per-thread last-seen activity (ISO strings
    // compare lexicographically) driving unread badges + notifications.
    std::unordered_map<std::string, std::string> last_seen_activity_;
    bool activity_seeded_ = false;  // first refresh records without notifying
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
    Gtk::Button tasks_btn_;              // schedule a routine ("Add Task")
    Gtk::Button approvals_btn_;          // icon + count label (approvals_label_)
    Gtk::Label approvals_label_;         // child of approvals_btn_
    Gtk::Image approvals_icon_;          // child of approvals_btn_
    Gtk::Button theme_btn_;              // dark/light toggle
    Gtk::Button connect_btn_;
    Gtk::Button stop_btn_;
    Gtk::Button learn_btn_;              // Phase 20: learn-by-demonstration
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
    Gtk::Label thread_title_;            // active thread name above the chat
    Gtk::ScrolledWindow chat_scroll_;
    Gtk::Box messages_box_{Gtk::Orientation::VERTICAL};
    Gtk::Box composer_{Gtk::Orientation::HORIZONTAL};
    Gtk::Button attach_btn_;             // Phase 18: 📎 attach files
    Gtk::Entry entry_;
    Gtk::Button send_btn_;
    Gtk::Box attach_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label attach_label_;            // pending-attachment summary line
    Gtk::Button attach_clear_btn_;       // drops pending attachments
    nlohmann::json pending_attachments_ = nlohmann::json::array();
    void update_attach_row();

    sigc::connection poll_timer_;

    // at most one connect dialog may exist — guards against the 1.5 s poll
    // timer spawning a fresh modal dialog on every 401 response
    Gtk::Dialog* connect_dialog_ = nullptr;
};

} // namespace hive
