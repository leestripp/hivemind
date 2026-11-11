// Prefs — persisted voice settings (server base URL, API key, default
// agent, voice assignments, thresholds). JSON at
// ~/.config/hive-client-voice/prefs.json. Mirrors the Android Prefs.kt.
//
// Speech (TTS+STT) is served by the local ChimpFlow gateway. Its API key
// is never written into prefs.json: it comes from CHIMPFLOW_KEY or the
// separate mode-600 chimpflow.key file. CHIMPFLOW_URL remains a fallback
// until an endpoint is explicitly saved in Settings.
#pragma once

#include <map>
#include <string>

namespace voice_settings {

struct ChimpFlowEndpoint {
    std::string address{"127.0.0.1"};
    std::string port{"8090"};
};

ChimpFlowEndpoint parseEndpoint(const std::string& url);
std::string makeLoopbackUrl(const std::string& address,
                            const std::string& port);

}  // namespace voice_settings

class Prefs {
public:
    void load();
    void save() const;

    // Round-trip helpers.
    std::string base() const { return base_; }
    void setBase(const std::string& v) { base_ = v; }
    std::string apiKey() const { return api_key_; }
    void setApiKey(const std::string& v) { api_key_ = v; }
    std::string agentId() const { return agent_id_; }
    void setAgentId(const std::string& v) { agent_id_ = v; }
    std::string agentName() const { return agent_name_; }
    void setAgentName(const std::string& v) { agent_name_ = v; }
    bool onboardingDone() const { return onboarding_done_; }
    void setOnboardingDone(bool v) { onboarding_done_ = v; }

    // Per-agent voice assignment: agent id -> ChimpFlow speaker id
    // (for example "alba") or "" for the engine default.
    std::string voiceFor(const std::string& agent_id) const;
    void setVoiceFor(const std::string& agent_id, const std::string& voice);

    bool stateAnnounce() const { return state_announce_; }
    void setStateAnnounce(bool v) { state_announce_ = v; }

    // XDG GlobalShortcuts session token — persisted after a successful
    // bind so later launches restore the shortcut without a new
    // permission dialog. Empty until the first successful bind.
    std::string globalShortcutToken() const { return global_shortcut_token_; }
    void setGlobalShortcutToken(const std::string& v) {
        global_shortcut_token_ = v;
    }

    // Last main-window size; 0 means "never saved, use the default".
    int windowWidth() const { return window_width_; }
    void setWindowWidth(int v) { window_width_ = v; }
    int windowHeight() const { return window_height_; }
    void setWindowHeight(int v) { window_height_ = v; }

    // ChimpFlow gateway. The URL is persisted as ordinary configuration;
    // CHIMPFLOW_URL is used only while no URL has been saved. The key comes
    // from CHIMPFLOW_KEY or chimpflow.key until explicitly set in Settings,
    // which writes only the separate mode-600 key file.
    std::string chimpflowUrl() const;
    void setChimpflowUrl(const std::string& v) { chimpflow_url_ = v; }
    std::string chimpflowKey() const;
    void setChimpflowKey(const std::string& v);

    static std::string configPath();

private:
    std::string base_{"http://127.0.0.1:8090"};
    std::string api_key_;
    std::string agent_id_;
    std::string agent_name_;
    bool onboarding_done_{false};
    std::map<std::string, std::string> voices_;
    std::string global_shortcut_token_;
    bool state_announce_{false};
    int window_width_{0};
    int window_height_{0};
    std::string chimpflow_url_;
    std::string chimpflow_key_;
    bool chimpflow_key_set_{false};
    mutable bool chimpflow_key_dirty_{false};
};
