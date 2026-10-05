#include "RosterWindow.h"

#include "AudioPlayback.h"
#include "Avatars.h"
#include "Prefs.h"
#include "SettingsWindow.h"
#include "TtsEngine.h"
#include "VoicePickerWindow.h"
#include "VoiceWindow.h"

#include <algorithm>
#include <thread>

namespace hv {

namespace {
constexpr int kPollMs = 2500;  // MainActivity POLL_MS
constexpr int kAvatarPx = 56;  // Android card avatar 64dp, trimmed for density

// Fleet heartbeat states that mean "working" — hive-client's list.
bool workingState(const std::string& st) {
    return st == "acting" || st == "starting" || st == "restarting" ||
           st == "understanding" || st == "planning" || st == "verifying" ||
           st == "documenting" || st == "compressing";
}
}  // namespace

RosterWindow::RosterWindow(Prefs& prefs) : Gtk::Window(), prefs_(prefs) {
    set_title("HiveMind");
    set_default_size(440, 620);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    root->set_margin(16);
    set_child(*root);

    // Header: title + status (activity_main.xml) + ⚙ settings button.
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* headText = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    headText->set_hexpand(true);
    auto* title = Gtk::make_managed<Gtk::Label>("HiveMind");
    title->add_css_class("hv-title");
    title->set_halign(Gtk::Align::START);
    status_ = Gtk::make_managed<Gtk::Label>("Loading agents…");
    status_->add_css_class("hv-status");
    status_->set_halign(Gtk::Align::START);
    headText->append(*title);
    headText->append(*status_);
    header->append(*headText);
    spinner_ = Gtk::make_managed<Gtk::Spinner>();
    spinner_->set_valign(Gtk::Align::CENTER);
    header->append(*spinner_);
    auto* gear = Gtk::make_managed<Gtk::Button>("⚙");
    gear->add_css_class("hv-gear");
    gear->add_css_class("flat");
    gear->set_tooltip_text("Settings");
    gear->signal_clicked().connect([this] { openSettings(); });
    header->append(*gear);
    root->append(*header);

    error_ = Gtk::make_managed<Gtk::Label>();
    error_->add_css_class("error");
    error_->set_halign(Gtk::Align::START);
    error_->set_wrap(true);
    error_->set_visible(false);
    root->append(*error_);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    scroll->set_child(*list_);
    root->append(*scroll);

    api_ = std::make_unique<ApiClient>(prefs_.base(), prefs_.apiKey());
    refresh();
    poll_ = Glib::signal_timeout().connect(
        [this] {
            refresh();
            return true;
        },
        kPollMs);
}

RosterWindow::~RosterWindow() { poll_.disconnect(); }

void RosterWindow::refresh() {
    spinner_->start();
    ApiClient* api = api_.get();
    // ApiClient is blocking — fetch on a worker, paint on the UI thread.
    std::thread([this, api] {
        std::vector<AgentInfo> agents;
        std::string err;
        try {
            agents = api->listAgents();
        } catch (const std::exception& e) {
            err = e.what();
        }
        Glib::signal_idle().connect_once(
            [this, agents = std::move(agents), err = std::move(err)] {
                spinner_->stop();
                if (!err.empty()) {
                    status_->set_label("Connection failed");
                    error_->set_label(err);
                    error_->set_visible(true);
                    return;
                }
                error_->set_visible(false);
                std::size_t online = 0;
                for (const auto& a : agents)
                    if (a.online) ++online;
                status_->set_label(std::to_string(online) + " of " +
                                   std::to_string(agents.size()) + " online");
                rebuildList(agents);
                announce(agents);
            });
    }).detach();
}

void RosterWindow::rebuildList(const std::vector<AgentInfo>& agents) {
    while (auto* child = list_->get_first_child()) list_->remove(*child);

    if (agents.empty()) {
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "No agents on this hub.\nOpen ⚙ Settings to check the hub URL.");
        hint->add_css_class("hv-dim");
        hint->set_justify(Gtk::Justification::CENTER);
        hint->set_margin_top(40);
        list_->append(*hint);
        return;
    }

    for (const auto& a : agents) {
        std::string st = a.state;
        std::transform(st.begin(), st.end(), st.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const bool working = a.busy || workingState(st);

        // Card is a styled Box + click controller (not a Button) so the
        // nested 🎙 button owns its own clicks — no nested activatables.
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        card->add_css_class("hv-card");
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL,
                                                12);
        row->set_hexpand(true);
        card->append(*row);

        // Avatar (HiDPI: render at 2× logical, downscale in the Image).
        auto* avatar = Gtk::make_managed<Gtk::Image>();
        avatar->set(avatarPixbuf(resolveAvatar(a.name, a.avatar),
                                 kAvatarPx * 2));
        avatar->set_pixel_size(kAvatarPx);
        avatar->set_valign(Gtk::Align::CENTER);
        row->append(*avatar);

        // Name + role.
        auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL,
                                                 2);
        text->set_hexpand(true);
        text->set_valign(Gtk::Align::CENTER);
        auto* name = Gtk::make_managed<Gtk::Label>(a.name);
        name->add_css_class("hv-name");
        if (working) name->add_css_class("hv-name-busy");
        name->set_halign(Gtk::Align::START);
        auto* role = Gtk::make_managed<Gtk::Label>(a.role);
        role->add_css_class("hv-role");
        role->set_halign(Gtk::Align::START);
        role->set_ellipsize(Pango::EllipsizeMode::END);
        role->set_max_width_chars(24);
        text->append(*name);
        text->append(*role);
        row->append(*text);

        // Right rail: spinner (busy) / voice button / state pill.
        if (working) {
            auto* spin = Gtk::make_managed<Gtk::Spinner>();
            spin->start();
            spin->set_valign(Gtk::Align::CENTER);
            row->append(*spin);
        }
        auto* voiceBtn = Gtk::make_managed<Gtk::Button>("🎙");
        voiceBtn->add_css_class("hv-voice-btn");
        voiceBtn->add_css_class("flat");
        voiceBtn->set_tooltip_text("Pick a voice for " + a.name);
        voiceBtn->set_valign(Gtk::Align::CENTER);
        voiceBtn->signal_clicked().connect(
            [this, a] { openVoicePicker(a); });
        row->append(*voiceBtn);

        const bool ready = a.online && !working;
        auto* pill = Gtk::make_managed<Gtk::Label>(
            working ? "● busy" : ready ? "● ready" : "● offline");
        pill->add_css_class("hv-pill");
        pill->add_css_class(working   ? "hv-pill-busy"
                            : ready ? "hv-pill-ready"
                                    : "hv-pill-off");
        pill->set_valign(Gtk::Align::CENTER);
        row->append(*pill);

        if (!a.online) card->set_opacity(0.45);
        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_PRIMARY);
        click->signal_released().connect([this, a](int, double, double) {
            if (a.online) openVoice(a);
        });
        card->add_controller(click);
        list_->append(*card);
    }
}

void RosterWindow::openVoice(const AgentInfo& agent) {
    prefs_.setAgentId(agent.id);
    prefs_.setAgentName(agent.name);
    prefs_.setOnboardingDone(true);
    prefs_.save();
    auto* win = VoiceWindow::create(prefs_, agent);
    win->present();
}

void RosterWindow::openVoicePicker(const AgentInfo& agent) {
    auto* win = Gtk::make_managed<VoicePickerWindow>(prefs_, agent.id,
                                                     agent.name);
    win->set_transient_for(*this);
    win->present();
}

void RosterWindow::openSettings() {
    auto* win = Gtk::make_managed<SettingsWindow>(prefs_);
    win->set_transient_for(*this);
    win->signal_hide().connect([this] {
        // Hub URL / key may have changed: rebuild the client + roster.
        api_ = std::make_unique<ApiClient>(prefs_.base(), prefs_.apiKey());
        announcer_.reset();
        refresh();
    });
    win->present();
}

void RosterWindow::announce(const std::vector<AgentInfo>& agents) {
    if (!prefs_.stateAnnounce()) return;
    std::vector<AgentSnapshot> snaps;
    snaps.reserve(agents.size());
    for (const auto& a : agents)
        snaps.push_back({a.id, a.name, a.online, a.busy});
    auto notes = announcer_.diff(snaps);
    if (notes.empty()) return;
    if (!tts_) {
        playback_ = std::make_unique<AudioPlayback>();
        playback_->start();
        tts_ = std::make_unique<TtsEngine>(*playback_);
    }
    for (const auto& n : notes) {
        tts_->applyVoice(prefs_.voiceFor(n.agentId));
        tts_->speakQueued(n.text);
    }
}

}  // namespace hv
