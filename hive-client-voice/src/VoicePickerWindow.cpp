#include "VoicePickerWindow.h"

#include "AudioPlayback.h"
#include "Prefs.h"
#include "TtsEngine.h"

namespace hv {

VoicePickerWindow::VoicePickerWindow(Prefs& prefs, std::string agentId,
                                     std::string agentName)
    : Gtk::Window(), prefs_(prefs), agentId_(std::move(agentId)),
      pending_(prefs_.voiceFor(agentId_)) {
    set_title("Voice for " + agentName);
    set_default_size(420, 480);
    set_modal(true);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(16);
    set_child(*root);

    auto* title =
        Gtk::make_managed<Gtk::Label>("Pick a voice for " + agentName);
    title->add_css_class("title-3");
    root->append(*title);

    // Live indicator of the pending selection — updates on every click.
    selected_ = Gtk::make_managed<Gtk::Label>();
    selected_->add_css_class("dim-label");
    root->append(*selected_);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    root->append(*scroll);
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    scroll->set_child(*list);

    playback_ = std::make_unique<AudioPlayback>();
    playback_->start();
    tts_ = std::make_unique<TtsEngine>(*playback_, prefs_.chimpflowUrl(),
                                       prefs_.chimpflowKey());

    const auto voices = tts_->voices();

    auto addRow = [this, list](const std::string& voice,
                               const std::string& label) {
        Row r;
        r.voice = voice;
        r.row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        box->set_margin(6);
        r.mark = Gtk::make_managed<Gtk::Label>("");
        r.mark->set_width_chars(1);
        r.name = Gtk::make_managed<Gtk::Label>(label);
        box->append(*r.mark);
        box->append(*r.name);
        r.row->set_child(*box);
        list->append(*r.row);
        rows_.push_back(r);
    };

    addRow("", "Default voice (engine default)");
    if (voices.empty()) {
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "No voices available — is the ChimpFlow voice service\nrunning at " +
            prefs_.chimpflowUrl() + "?");
        hint->add_css_class("dim-label");
        auto* hintRow = Gtk::make_managed<Gtk::ListBoxRow>();
        hintRow->set_child(*hint);
        hintRow->set_activatable(false);
        list->append(*hintRow);
    }
    for (const auto& v : voices) addRow(v, v);

    list->signal_row_activated().connect([this](Gtk::ListBoxRow* activated) {
        for (const auto& r : rows_)
            if (r.row == activated) select(r.voice);
    });

    updateMarks();

    // Commit bar: Cancel discards, OK saves. No silent persistence.
    auto* buttons =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("_Cancel", true);
    auto* ok = Gtk::make_managed<Gtk::Button>("_OK", true);
    ok->add_css_class("suggested-action");
    cancel->signal_clicked().connect([this] { close(); });
    ok->signal_clicked().connect([this] {
        prefs_.setVoiceFor(agentId_, pending_);
        prefs_.save();
        close();
    });
    buttons->append(*cancel);
    buttons->append(*ok);
    root->append(*buttons);
}

VoicePickerWindow::~VoicePickerWindow() = default;

void VoicePickerWindow::select(const std::string& voice) {
    pending_ = voice;
    updateMarks();
    tts_->preview("Hi, this is my voice.", voice);
}

void VoicePickerWindow::updateMarks() {
    for (auto& r : rows_) {
        const bool sel = r.voice == pending_;
        const std::string label = r.voice.empty() ? "Default voice (engine default)"
                                                  : r.voice;
        r.mark->set_text(sel ? "✓" : "");
        r.name->set_markup(sel ? "<b>" + label + "</b>" : label);
    }
    selected_->set_label("Selected: " +
                         (pending_.empty() ? std::string("default (engine voice)")
                                           : pending_));
}

}  // namespace hv
