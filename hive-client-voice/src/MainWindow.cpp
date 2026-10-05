#include "MainWindow.h"

#include "Prefs.h"
#include "RosterView.h"
#include "VoiceView.h"

namespace hv {

MainWindow::MainWindow(Prefs& prefs) : Gtk::Window(), prefs_(prefs) {
    set_title("HiveMind");
    set_default_size(440, 620);

    stack_ = Gtk::make_managed<Gtk::Stack>();
    stack_->set_transition_type(Gtk::StackTransitionType::SLIDE_LEFT_RIGHT);
    stack_->set_transition_duration(220);
    set_child(*stack_);

    roster_ = Gtk::make_managed<RosterView>(prefs_);
    stack_->add(*roster_, "roster");
    roster_->signal_open_voice().connect(
        [this](const AgentInfo& agent) { showVoice(agent); });
}

void MainWindow::showVoice(const AgentInfo& agent) {
    prefs_.setAgentId(agent.id);
    prefs_.setAgentName(agent.name);
    prefs_.setOnboardingDone(true);
    prefs_.save();

    if (voice_) showRoster();  // one session at a time

    voiceBuilder_ = Gtk::Builder::create_from_resource(
        "/org/hivemind/voice/ui/voice.ui");
    voice_ = Gtk::Builder::get_widget_derived<VoiceView>(
        voiceBuilder_, "voiceRoot", prefs_, agent);
    voice_->signal_back().connect([this] { showRoster(); });
    stack_->add(*voice_, "voice");
    stack_->set_visible_child("voice");
    set_title(agent.name + " — HiveMind");
}

void MainWindow::showRoster() {
    stack_->set_visible_child("roster");
    set_title("HiveMind");
    if (voice_) {
        stack_->remove(*voice_);
        voice_ = nullptr;
        voiceBuilder_.reset();  // drops the last ref -> ~VoiceView() runs
    }
}

}  // namespace hv
