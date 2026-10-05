#pragma once

#include "ApiClient.h"

#include <gtkmm.h>

#include <memory>
#include <vector>

class Prefs;

namespace hv {

// Agent roster: the entry screen. Lists agents from /api/agents with an
// online/busy dot; picking one saves it as the default and opens the
// voice session. Mirrors RosterActivity.kt.
class RosterWindow : public Gtk::Window {
 public:
    explicit RosterWindow(Prefs& prefs);
    ~RosterWindow() override;

    void refresh();

 private:
    void rebuildList(const std::vector<AgentInfo>& agents);
    void openVoice(const AgentInfo& agent);

    Prefs& prefs_;
    Gtk::Box* list_ = nullptr;
    Gtk::Spinner* spinner_ = nullptr;
    Gtk::Label* error_ = nullptr;
    std::unique_ptr<ApiClient> api_;
};

}  // namespace hv
