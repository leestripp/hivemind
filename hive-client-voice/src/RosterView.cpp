#include "RosterView.h"

#include "DebugShell.h"

#include "AudioPlayback.h"
#include "Avatars.h"
#include "Prefs.h"
#include "SettingsWindow.h"
#include "TtsEngine.h"
#include "VoicePickerWindow.h"

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

RosterView::RosterView(Prefs& prefs)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 10), prefs_(prefs) {
    DebugShell::registerRoster(this);
    set_margin(16);

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
    append(*header);

    error_ = Gtk::make_managed<Gtk::Label>();
    error_->add_css_class("error");
    error_->set_halign(Gtk::Align::START);
    error_->set_wrap(true);
    error_->set_visible(false);
    append(*error_);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    scroll->set_child(*list_);
    append(*scroll);

    api_ = std::make_unique<ApiClient>(prefs_.base(), prefs_.apiKey());
    refresh();
    poll_ = Glib::signal_timeout().connect(
        [this] {
            refresh();
            return true;
        },
        kPollMs);
}

RosterView::~RosterView() {
    DebugShell::registerRoster(nullptr); poll_.disconnect(); }

void RosterView::refresh() {
    spinner_->start();
    ApiClient* api = api_.get();
    // ApiClient is blocking — fetch on a worker, paint on the UI thread.
    std::thread([this, api] {
        std::vector<AgentInfo> agents;
        std::vector<WorkerInfo> workers;
        std::string err;
        try {
            agents = api->listAgents();
        } catch (const std::exception& e) {
            err = e.what();
        }
        // Workers power the group headers; if that call fails we still
        // show agents (groups render as online so nothing is hidden).
        try {
            workers = api->listWorkers();
        } catch (const std::exception&) {
        }
        Glib::signal_idle().connect_once(
            [this, agents = std::move(agents),
             workers = std::move(workers), err = std::move(err)] {
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
                rebuildList(agents, workers);
                announce(agents);
            });
    }).detach();
}

void RosterView::rebuildList(const std::vector<AgentInfo>& agents,
                             const std::vector<WorkerInfo>& workers) {
    lastAgents_ = agents;
    lastWorkers_ = workers;
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

    // Grouped roster — port of the Android RosterAdapter: one header per
    // home worker ("" = hub), online groups first then alphabetical,
    // offline groups auto-collapsed and greyed. An agent homed on a
    // worker that isn't enrolled any more counts as offline.
    std::map<std::string, bool> workerOnline;
    for (const auto& w : workers) workerOnline[w.name] = w.online;
    const bool workersKnown = !workers.empty();

    struct Group {
        std::string worker;
        bool online;
        std::vector<AgentInfo> agents;
    };
    std::map<std::string, std::vector<AgentInfo>> byWorker;
    for (const auto& a : agents)
        byWorker[a.worker.empty() ? "hub" : a.worker].push_back(a);
    std::vector<Group> groups;
    for (auto& [w, list] : byWorker) {
        const auto it = workerOnline.find(w);
        const bool on =
            workersKnown ? (it != workerOnline.end() && it->second) : true;
        groups.push_back({w, on, std::move(list)});
    }
    std::sort(groups.begin(), groups.end(),
              [](const Group& a, const Group& b) {
                  if (a.online != b.online) return a.online > b.online;
                  return a.worker < b.worker;
              });
    // Android update(): offline groups (re)collapse on every refresh;
    // online groups keep the user's choice.
    for (const auto& g : groups)
        if (!g.online) collapsedGroups_.insert(g.worker);

    for (const auto& g : groups) {
        const bool expanded =
            collapsedGroups_.find(g.worker) == collapsedGroups_.end();
        appendGroupHeader(g.worker, g.online, g.agents.size(), expanded);
        if (expanded)
            for (const auto& a : g.agents) appendAgentCard(a);
    }
}

void RosterView::appendGroupHeader(const std::string& worker, bool online,
                                   std::size_t count, bool expanded) {
    auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    hdr->add_css_class("hv-group");
    if (!online) hdr->set_opacity(0.55);
    auto* chevron = Gtk::make_managed<Gtk::Label>(expanded ? "\u25BE" : "\u25B8");
    chevron->add_css_class("hv-chevron");
    chevron->set_valign(Gtk::Align::CENTER);
    hdr->append(*chevron);
    auto* name = Gtk::make_managed<Gtk::Label>(worker + " \u00B7 " +
                                               std::to_string(count));
    name->add_css_class("hv-group-name");
    if (!online) name->add_css_class("hv-group-off");
    name->set_halign(Gtk::Align::START);
    name->set_hexpand(true);
    name->set_valign(Gtk::Align::CENTER);
    hdr->append(*name);
    auto* state = Gtk::make_managed<Gtk::Label>(online ? "\u25CF online"
                                                       : "\u25CB offline");
    state->add_css_class("hv-group-state");
    state->add_css_class(online ? "hv-group-on" : "hv-group-off");
    state->set_valign(Gtk::Align::CENTER);
    hdr->append(*state);

    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_released().connect([this, worker](int, double, double) {
        if (!collapsedGroups_.erase(worker)) collapsedGroups_.insert(worker);
        rebuildList(lastAgents_, lastWorkers_);
    });
    hdr->add_controller(click);
    list_->append(*hdr);
}

void RosterView::appendAgentCard(const AgentInfo& a) {
    std::string st = a.state;
    std::transform(st.begin(), st.end(), st.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    const bool working = a.busy || workingState(st);

    // Card is a styled Box + click controller (not a Button) so the
    // nested 🎙 button owns its own clicks — no nested activatables.
    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    card->add_css_class("hv-card");
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    row->set_hexpand(true);
    card->append(*row);

    // Avatar (HiDPI: render at 2× logical, downscale in the Image).
    auto* avatar = Gtk::make_managed<Gtk::Image>();
    avatar->set(avatarPixbuf(resolveAvatar(a.name, a.avatar), kAvatarPx * 2));
    avatar->set_pixel_size(kAvatarPx);
    avatar->set_valign(Gtk::Align::CENTER);
    row->append(*avatar);

    // Name + role.
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
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
    auto* voiceBtn = Gtk::make_managed<Gtk::Button>("\U0001F399");
    voiceBtn->add_css_class("hv-voice-btn");
    voiceBtn->add_css_class("flat");
    voiceBtn->set_tooltip_text("Pick a voice for " + a.name);
    voiceBtn->set_valign(Gtk::Align::CENTER);
    voiceBtn->signal_clicked().connect([this, a] { openVoicePicker(a); });
    row->append(*voiceBtn);

    const bool ready = a.online && !working;
    auto* pill = Gtk::make_managed<Gtk::Label>(
        working ? "\u25CF busy" : ready ? "\u25CF ready" : "\u25CF offline");
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

void RosterView::openVoice(const AgentInfo& agent) {
    signal_open_voice_.emit(agent);
}

void RosterView::openVoicePicker(const AgentInfo& agent) {
    auto* win = Gtk::make_managed<VoicePickerWindow>(prefs_, agent.id,
                                                     agent.name);
    if (auto* top = dynamic_cast<Gtk::Window*>(get_root())) {
        win->set_transient_for(*top);
        win->set_modal(true);
    }
    win->present();
}

void RosterView::openSettings() {
    auto* win = Gtk::make_managed<SettingsWindow>(prefs_);
    if (auto* top = dynamic_cast<Gtk::Window*>(get_root())) {
        win->set_transient_for(*top);
        win->set_modal(true);
    }
    win->signal_hide().connect([this] {
        // Hub URL / key may have changed: rebuild the client + roster.
        api_ = std::make_unique<ApiClient>(prefs_.base(), prefs_.apiKey());
        announcer_.reset();
        refresh();
    });
    win->present();
}

void RosterView::announce(const std::vector<AgentInfo>& agents) {
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

bool RosterView::debugOpenVoicePicker(const std::string& nameSub) {
    const Glib::ustring needle = Glib::ustring(nameSub).lowercase();
    for (const auto& a : lastAgents_) {
        if (Glib::ustring(a.name).lowercase().find(needle) !=
            Glib::ustring::npos) {
            openVoicePicker(a);
            return true;
        }
    }
    return false;
}

bool RosterView::debugOpenVoice(const std::string& nameSub) {
    const Glib::ustring needle = Glib::ustring(nameSub).lowercase();
    for (const auto& a : lastAgents_) {
        if (!a.online) continue;
        if (Glib::ustring(a.name).lowercase().find(needle) !=
            Glib::ustring::npos) {
            openVoice(a);
            return true;
        }
    }
    return false;
}

}  // namespace hv
