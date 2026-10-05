#include "VoicePickerWindow.h"

#include "AudioPlayback.h"
#include "Prefs.h"
#include "TtsEngine.h"

#include <cstdio>

namespace hv {

VoicePickerWindow::VoicePickerWindow(Prefs& prefs, std::string agentId,
                                     std::string agentName)
    : Gtk::Window(), prefs_(prefs), agentId_(std::move(agentId)) {
    set_title("Voice for " + agentName);
    set_default_size(400, 420);
    set_modal(true);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(16);
    set_child(*root);
    auto* title = Gtk::make_managed<Gtk::Label>("Pick a voice");
    title->add_css_class("title-3");
    root->append(*title);

    // Global speech-rate slider (piper --length_scale under the hood).
    // Applies to every voice; previews use it live.
    tts_->setSpeed(prefs_.ttsSpeed());
    auto* speedLabel = Gtk::make_managed<Gtk::Label>();
    const auto fmtSpeed = [](double s) {
        char b[40];
        snprintf(b, sizeof b, "Speech speed: %.2f\xc3\x97", s);
        return std::string(b);
    };
    speedLabel->set_text(fmtSpeed(prefs_.ttsSpeed()));
    speedLabel->add_css_class("dim-label");
    root->append(*speedLabel);
    auto* speed = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    speed->set_range(0.5, 2.0);
    speed->set_increments(0.05, 0.25);
    speed->set_value(prefs_.ttsSpeed());
    speed->signal_value_changed().connect([this, speed, speedLabel, fmtSpeed] {
        const double s = speed->get_value();
        prefs_.setTtsSpeed(s);
        prefs_.save();
        tts_->setSpeed(s);
        speedLabel->set_text(fmtSpeed(s));
    });
    root->append(*speed);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    scroll->set_child(*list_);
    root->append(*scroll);

    playback_ = std::make_unique<AudioPlayback>();
    playback_->start();
    tts_ = std::make_unique<TtsEngine>(*playback_);

    const auto voices = TtsEngine::voiceOptions(Prefs::voiceDir());
    const std::string current = prefs_.voiceFor(agentId_);
    if (voices.empty()) {
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "No .onnx voices found in\n" + Prefs::voiceDir());
        hint->add_css_class("dim-label");
        list_->append(*hint);
    }
    for (const auto& v : voices) {
        auto* row = Gtk::make_managed<Gtk::Button>(v.label +
                                                   (v.path == current ? " ✓" : ""));
        std::string path = v.path;
        row->signal_clicked().connect([this, path] { pick(path); });
        list_->append(*row);
    }
    auto* none = Gtk::make_managed<Gtk::Button>("Default voice");
    none->signal_clicked().connect([this] { pick(""); });
    root->append(*none);
}

VoicePickerWindow::~VoicePickerWindow() = default;

void VoicePickerWindow::pick(const std::string& path) {
    prefs_.setVoiceFor(agentId_, path);
    prefs_.save();
    if (!path.empty()) tts_->preview("Hi, this is my voice.", path);
    // Stay open so the user can audition others; Close via the window
    // manager. (The Android picker finishes immediately; keeping the
    // preview window alive is a desktop nicety.)
}

}  // namespace hv
