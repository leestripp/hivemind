#pragma once

#include <gtkmm.h>
#include <giomm/settings.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <set>
#include <vector>

#include "ApiClient.h"

namespace hive {

/**
 * HiveWindow — chat-first main window (HiveMind 2.0, Plan.md Phase 4).
 *
 * Left sidebar: agent roster + threads. Right: chat transcript with live
 * streaming (polled /activity + incremental /messages) and a composer.
 * Header: new-agent / new-thread / task and connection controls.
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
    nlohmann::json ui_select_agent(const std::string& name_or_id);
    nlohmann::json ui_send(const std::string& body);
    nlohmann::json ui_attach(const std::string& path);  // POST /ui/action/attach
    nlohmann::json ui_download_attachment(const std::string& name);
    /// Shared connect flow (Connect dialog OK + POST /ui/action/connect).
    void connect_with_credentials(const std::string& url, const std::string& key,
                                  std::function<void(std::string)> done);
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
    // Phase 37: follow the GNOME system style (host-run client) — reads
    // org.gnome.desktop.interface color-scheme and tracks it live.
    void follow_system_theme();
    Glib::RefPtr<Gio::Settings> iface_settings_;   // kept alive for watch
    void apply_font_scale();   // font_scale_ → gtk-xft-dpi (1.0 = system auto)
    void scroll_chat_to_bottom();  // sticky-bottom auto-scroll
    void on_settings();         // user profile: display name + avatar
    void refresh_themed_icons();  // re-tints pixbufs in place (no reparenting!)
    void set_button_icon(Gtk::Button& btn, const std::string& icon,
                         const std::string& tooltip);
    // Persistent icon widgets, created ONCE in setup_icons() — retinted, never
    // reparented (reparenting a managed member widget can destroy it → crash).
    struct IconSlot { Gtk::Button* btn; Gtk::Image* img; std::string icon; };
    std::vector<IconSlot> icon_slots_;
    void setup_icons();
    void maybe_show_connect_dialog();   // when no key is configured

    // ── data loading (worker threads → dispatcher → UI) ───────────────────
    void run_async(std::function<void()> work);
    void refresh_all();
    void refresh_threads();
    void poll_chat();           // messages + activity for current thread
    /// Phase 66c: after new bubbles render, POST /read for the current
    /// thread — advances ONLY the reserved "user" hub-side mark, never
    /// an agent's delivery receipt. Debounced 10s per thread.
    void maybe_post_user_read(const std::string& tid);
    void poll_roster();          // agents busy/state (Phase 27)
    std::string roster_sig_;   // last busy/state signature
    std::string current_dm_agent_;  // agent whose DM is focused ("" if none)
    bool restoring_selection_ = false;  // rebuild_roster reselect guard

    // ── UI callbacks ──────────────────────────────────────────────────────
    void on_agent_selected();
    void on_thread_selected();
    void on_attach();   // Phase 18: pick + upload files
    /// Post-dialog half of on_attach (upload + pending row). The UiApi
    /// test hook drives this directly so the flow is headlessly testable.
    void attach_picked_file(const std::string& path);

    void on_send();
    void on_new_agent();
    void show_agent_dialog(const nlohmann::json* existing);  // nullptr = create
    void on_edit_agent(const nlohmann::json& agent);
    void on_delete_agent(const nlohmann::json& agent);
    void show_agent_screen(const nlohmann::json& agent, bool view_only = false);
    void show_exec_log(const nlohmann::json& agent);   // Phase 29
    void show_agent_menu(Gtk::Widget& anchor, const nlohmann::json& agent,
                         double x, double y);
    void on_new_task();
    void show_task_dialog(const std::string& preselect_agent_id = "",
                          const nlohmann::json* existing = nullptr, // edit mode
                          std::function<void()> on_saved = nullptr);
    void show_routines_dialog(const std::string& preselect_agent_id = "");
    void show_skills_dialog(const nlohmann::json& agent);   // Phase 14.2
    void show_new_skill_dialog(Gtk::Window* parent);          // Phase 29
    void show_inspector(const nlohmann::json& agent);       // Phase 14.4
    void load_inspector_container(Gtk::Box* box, Gtk::Window* dialog,
                                  const std::string& agent_id);
    void update_thread_title();
    void notify(const Glib::ustring& title, const Glib::ustring& body);
    /// Badge/notify pass over threads_ after a refresh (Phase 14.1).
    void process_thread_activity();
    void on_new_thread();
    void on_stop_agent(const nlohmann::json& agent);
    void on_delete_thread(const std::string& thread_id);
    void on_clear_thread(const std::string& thread_id,
                         const std::string& title);
    void show_thread_menu(Gtk::Widget& anchor, const nlohmann::json& thread,
                          double x, double y);

    // ── rendering helpers ─────────────────────────────────────────────────
    void rebuild_roster();
    void rebuild_thread_list();
    /// Select `id`'s row in the Threads sidebar (no-op when absent).
    void select_thread_row(const std::string& id);
    /// Single switch for the composer (entry/send/attach). Two
    /// call sites used to set these independently and drifted apart.
    void set_composer_enabled(bool enabled);
    /// Phase 52: render/remove the live streaming bubble from the
    /// activity poll's turns[] (partial text of the first active turn).
    void update_live_bubble(const nlohmann::json& turns);
    // Phase 55: bounded history — page-back through older messages.
    Gtk::Widget* build_message_row(const std::string& sender_kind,
                                   const std::string& sender_label,
                                   const std::string& body,
                                   const std::string& created_at,
                                   const nlohmann::json& attachments,
                                   const nlohmann::json& stats
                                       = nlohmann::json::object());
    void update_load_earlier_row();
    void load_earlier();
    void append_message_bubble(const std::string& sender_kind,
                               const std::string& sender_label,
                               const std::string& body,
                               const std::string& created_at = "",
                               const nlohmann::json& attachments
                                   = nlohmann::json::array(),
                               const nlohmann::json& stats
                                   = nlohmann::json::object());
    /// Phase 34: render one image attachment inline (download + cache if
    /// needed, then Gtk::Picture). holder is a vertical box in the bubble.
    void load_image_attachment(Gtk::Box* holder, const nlohmann::json& att);
    void show_image_picture(Gtk::Box* holder, const std::string& file,
                            const std::string& caption);
    // Phase E: centred inline video player, paused on the first frame
    void load_video_attachment(Gtk::Box* holder, const nlohmann::json& att);
    /// Generic (non-image/video) attachment chip: download via the hub
    /// API into the client cache, then open locally.
    void open_file_attachment(Gtk::Button* chip, const nlohmann::json& att,
                          const std::string& tid);
    /// 2026-10-16 (lee): per-attachment "Save to Downloads" button.
    /// add_download_button appends it to parent (icon-only for chip rows,
    /// labelled under media cards); download_attachment saves the file —
    /// cached copy when present, hub download otherwise — into the
    /// user's Downloads folder under a unique-ified name.
    void add_download_button(Gtk::Box* parent, const nlohmann::json& att,
                             const std::string& tid, bool icon_only);
    void download_attachment(Gtk::Button* btn, const nlohmann::json& att,
                             const std::string& tid);
    void show_video_widget(Gtk::Box* holder, const std::string& file,
                           const std::string& name);
    // Audio attachments: card with a big round play/pause button plus
    // filename and size (Gtk::MediaFile playback, in place).
    void load_audio_attachment(Gtk::Box* holder, const nlohmann::json& att);
    void show_audio_widget(Gtk::Box* holder, const std::string& file,
                           const std::string& name);
    void set_status(const std::string& text);

    ApiClient api_;

    /// The single live audio pipeline (2026-10-19). Gtk::MediaFile spins
    /// up a GStreamer decodebin3 per instance; several preparing at once
    /// 2026-10-21: NO GStreamer runs in this process at all (decodebin3's
    /// assertion-failed g_error is uncatchable and killed us twice).
    /// Playback is a gst-play-1.0 child process (MediaPlayer.h); this is
    /// the single live one — starting playback stops the previous, so at
    /// most one player child ever exists.
    std::string server_url_ = "http://127.0.0.1:8421";

    // state (GUI thread only)
    nlohmann::json agents_ = nlohmann::json::array();
    nlohmann::json threads_ = nlohmann::json::array();
    nlohmann::json workers_ = nlohmann::json::array();   // /api/workers
    // Roster grouping by hive-worker: collapsed state survives the 1.5s
    // poll rebuilds. Offline workers start collapsed on first sight.
    std::unordered_set<std::string> collapsed_workers_;

    // Phase 67: user thread groups (folders). thread_groups_ mirrors
    // GET /api/thread-groups; collapsed_groups_ survives the rebuilds
    // and persists to ~/.local/share/hivemind/client-ui.json.
    nlohmann::json thread_groups_ = nlohmann::json::array();
    std::unordered_set<std::string> collapsed_groups_;
    /// Phase 67b: client-local bucket pins — thread id -> bucket id
    /// ("smart:…" or a custom group id). Lets the user force a thread
    /// into a bucket the hub data didn't pick up (e.g. a routine
    /// mis-tagged dm); persisted to client-ui.json. Consulted only
    /// when the hub carries no group_id for the thread.
    std::unordered_map<std::string, std::string> thread_bucket_overrides_;
    bool group_ui_loaded_ = false;
    void load_group_ui_state();
    void save_group_ui_state() const;
    void show_group_name_dialog(const std::string& title,
                                const std::string& initial,
                                const std::string& group_id,
                                const std::string& move_thread_id);
    void show_group_menu(Gtk::Widget& anchor, const std::string& group_id,
                         const std::string& group_name, double x, double y);
    void move_thread_to(const std::string& thread_id,
                        const std::string& group_id);
    /// Effective bucket id (custom group id or "smart:…") for a
    /// thread: hub group_id, else local override, else smart default.
    std::string thread_bucket_id(const nlohmann::json& thread) const;
    std::unordered_set<std::string> known_workers_;
    std::string current_thread_;
    std::string last_message_id_;
    // Phase 66c read-post debounce (main thread only).
    std::chrono::steady_clock::time_point last_read_post_{};
    std::string last_read_post_thread_;
    // Message IDs already rendered in the open thread. Overlapping poll_chat
    // calls (timer + post-send) both snapshot the same `after` cursor and can
    // return the same new message — without this dedupe the bubble appends
    // twice (observed 2026-08-27 over a high-latency link to hub.example.com).
    std::unordered_set<std::string> rendered_msg_ids_;
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

    // Phase 34: attachment image downloads in flight (cache-path keyed —
    // one download per file, no duplicates across re-renders).
    std::set<std::string> image_downloads_;

    // ── widgets ───────────────────────────────────────────────────────────
    Gtk::Box root_{Gtk::Orientation::VERTICAL};
    Gtk::HeaderBar header_;
    // Icon-only buttons (icons are GResource SVGs, re-tinted per theme).
    Gtk::Button new_agent_btn_;
    Gtk::Button new_thread_btn_;
    Gtk::Button tasks_btn_;              // schedule a routine ("Add Task")
    Gtk::Button settings_btn_;           // user profile settings
    Gtk::Button connect_btn_;
    bool dark_mode_ = false;             // persisted in config.json
    // User profile (persisted in config.json): chat identity for the human.
    std::string display_name_;           // "" = "You"
    std::string user_avatar_;            // "" = default user silhouette
    double font_scale_ = 1.0;            // UI font scale; 1.0 = system default
    int  sidebar_split_ = 320;           // Agents/Threads paned position (px)
    int  main_split_ = 260;              // sidebar/chat paned position (px)
    int  window_w_ = 1100, window_h_ = 760;  // main-window size (px)
    bool split_save_pending_ = false;    // debounce for persisting geometry
    std::string user_label() const { return display_name_.empty() ? "You" : display_name_; }

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
    bool chat_stick_bottom_ = true;   // auto-scroll unless user scrolled up
    bool chat_scroll_prog_ = false;   // guard: our own scroll in flight
    bool chat_scroll_idle_ = false;   // coalesces deferred scrolls
    Gtk::Box messages_box_{Gtk::Orientation::VERTICAL};
    // Phase 52: live streaming bubble (managed children of messages_box_;
    // raw pointers, nulled on remove / messages_box_ clears).
    Gtk::Box* live_row_ = nullptr;
    // Phase 55: bounded history paging state.
    Gtk::Button* load_earlier_btn_ = nullptr;
    bool has_more_history_ = false;
    bool loading_earlier_ = false;
    std::string oldest_message_id_;
    Gtk::Label* live_label_ = nullptr;
    std::string live_agent_key_;         // agent_id owning the bubble
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
    sigc::connection threads_timer_;   // 10s thread-list poll (agent↔agent threads appear live)
    std::string threads_sig_;          // change signature; rebuild only on change

    // at most one connect dialog may exist — guards against the 1.5 s poll
    // timer spawning a fresh modal dialog on every 401 response
    Gtk::Dialog* connect_dialog_ = nullptr;
};

} // namespace hive
