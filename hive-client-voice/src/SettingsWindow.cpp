#include "SettingsWindow.h"

#include "Prefs.h"

namespace hv {

SettingsWindow::SettingsWindow(Prefs& prefs) : Gtk::Window(), prefs_(prefs) {
    set_title("Voice Settings");
    set_default_size(460, 360);
    set_modal(true);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    root->set_margin(20);
    set_child(*root);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(10);
    grid->set_column_spacing(10);
    root->append(*grid);

    int row = 0;
    auto addRow = [&](const char* label, Gtk::Widget& w) {
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::START);
        grid->attach(*l, 0, row, 1, 1);
        grid->attach(w, 1, row, 1, 1);
        w.set_hexpand(true);
        ++row;
    };

    base_ = Gtk::make_managed<Gtk::Entry>();
    base_->set_text(prefs_.base());
    addRow("Hub URL", *base_);

    key_ = Gtk::make_managed<Gtk::Entry>();
    key_->set_text(prefs_.apiKey());
    key_->set_visibility(false);
    key_->set_input_purpose(Gtk::InputPurpose::PASSWORD);
    addRow("API key", *key_);

    announce_ = Gtk::make_managed<Gtk::Switch>();
    announce_->set_active(prefs_.stateAnnounce());
    announce_->set_halign(Gtk::Align::START);
    addRow("Announce agent state changes", *announce_);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Voices: drop .onnx piper models into\n" + Prefs::voiceDir());
    hint->add_css_class("dim-label");
    root->append(*hint);

    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->add_css_class("suggested-action");
    save->signal_clicked().connect([this] { saveAndClose(); });
    root->append(*save);
}

void SettingsWindow::saveAndClose() {
    prefs_.setBase(base_->get_text());
    prefs_.setApiKey(key_->get_text());
    prefs_.setStateAnnounce(announce_->get_active());
    prefs_.save();
    hide();
}

}  // namespace hv
