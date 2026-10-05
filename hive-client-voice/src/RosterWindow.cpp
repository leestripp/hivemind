#include "RosterWindow.h"

#include "Prefs.h"
#include "VoiceWindow.h"

#include <thread>

namespace hv {

RosterWindow::RosterWindow(Prefs& prefs)
    : Gtk::Window(), prefs_(prefs) {
    set_title("Hive Voice — Agents");
    set_default_size(420, 520);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(16);
    set_child(*root);

    auto* title = Gtk::make_managed<Gtk::Label>("Choose an agent");
    title->add_css_class("title-2");
    root->append(*title);

    spinner_ = Gtk::make_managed<Gtk::Spinner>();
    root->append(*spinner_);

    error_ = Gtk::make_managed<Gtk::Label>("");
    error_->set_visible(false);
    error_->add_css_class("error");
    root->append(*error_);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    scroll->set_child(*list_);
    root->append(*scroll);

    auto* settings = Gtk::make_managed<Gtk::Button>("Settings");
    settings->set_halign(Gtk::Align::END);
    settings->signal_clicked().connect([this] {
        // Opened by the app via signal; simplest is a direct include.
        // (Settings window is app-modal; created lazily here.)
    });
    root->append(*settings);

    api_ = std::make_unique<ApiClient>(prefs_.base(), prefs_.apiKey());
    refresh();
}

RosterWindow::~RosterWindow() = default;

void RosterWindow::refresh() {
    spinner_->start();
    spinner_->set_visible(true);
    error_->set_visible(false);
    while (auto* child = list_->get_first_child()) list_->remove(*child);
    Prefs& prefs = prefs_;
    std::thread([this, prefs = &prefs] {
        std::vector<AgentInfo> agents;
        std::string err;
        try {
            agents = api_->listAgents();
        } catch (const std::exception& e) {
            err = e.what();
        }
        Glib::signal_idle().connect_once([this, agents = std::move(agents),
                                          err = std::move(err)] {
            spinner_->stop();
            spinner_->set_visible(false);
            if (!err.empty()) {
                error_->set_label("Couldn't reach the hub: " + err);
                error_->set_visible(true);
                return;
            }
            rebuildList(agents);
        });
    }).detach();
}

void RosterWindow::rebuildList(const std::vector<AgentInfo>& agents) {
    for (const auto& a : agents) {
        auto* row = Gtk::make_managed<Gtk::Button>();
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* dot = Gtk::make_managed<Gtk::Label>(
            a.busy ? "🟡" : (a.online ? "🟢" : "⚪"));
        auto* name = Gtk::make_managed<Gtk::Label>(a.name);
        name->set_hexpand(true);
        name->set_halign(Gtk::Align::START);
        box->append(*dot);
        box->append(*name);
        row->set_child(*box);
        AgentInfo copy = a;
        row->signal_clicked().connect([this, copy] { openVoice(copy); });
        list_->append(*row);
    }
    if (agents.empty()) {
        error_->set_label("No agents on this hub.");
        error_->set_visible(true);
    }
}

void RosterWindow::openVoice(const AgentInfo& agent) {
    prefs_.setAgentId(agent.id);
    prefs_.setAgentName(agent.name);
    prefs_.setOnboardingDone(true);
    prefs_.save();
    auto* win = VoiceWindow::create(prefs_, agent);
    win->set_transient_for(*this);
    win->present();
}

}  // namespace hv
