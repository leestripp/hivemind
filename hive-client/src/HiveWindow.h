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

private:
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

    // ── rendering helpers ─────────────────────────────────────────────────
    void rebuild_roster();
    void rebuild_thread_list();
    void append_message_bubble(const std::string& sender_kind,
                               const std::string& sender_label,
                               const std::string& body);
    void set_status(const std::string& text);

    ApiClient api_;

    // state (GUI thread only)
    nlohmann::json agents_ = nlohmann::json::array();
    nlohmann::json threads_ = nlohmann::json::array();
    std::string current_thread_;
    std::string last_message_id_;
    int pending_approvals_ = 0;
    bool alive_ = true;

    // async marshalling
    Glib::Dispatcher dispatcher_;
    std::mutex queue_m_;
    std::vector<std::function<void()>> done_queue_;

    // ── widgets ───────────────────────────────────────────────────────────
    Gtk::Box root_{Gtk::Orientation::VERTICAL};
    Gtk::HeaderBar header_;
    Gtk::Button new_agent_btn_{"+ Agent"};
    Gtk::Button new_thread_btn_{"+ Thread"};
    Gtk::Button approvals_btn_{"Approvals"};

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
    Gtk::Button send_btn_{"Send"};

    sigc::connection poll_timer_;
};

} // namespace hive
