// Prefs — persisted voice settings (server base URL, API key, default
// agent, voice assignments, thresholds). JSON at
// ~/.config/hive-client-voice/prefs.json. Mirrors the Android Prefs.kt.
//
// Speech (TTS+STT) is served by the local ChimpFlow gateway; its URL
// and key are NOT persisted secrets — the key comes from the
// CHIMPFLOW_KEY environment variable or a ~/.config/hive-client-voice/
// chimpflow.key file (mode 600), the URL from CHIMPFLOW_URL.
#pragma once

#include <map>
#include <string>

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

    // Per-agent voice assignment: agent id -> ChimpFlow voice/model id
    // ("pocket-tts") or "" for the engine default.
    std::string voiceFor(const std::string& agent_id) const;
    void setVoiceFor(const std::string& agent_id, const std::string& voice);

    bool stateAnnounce() const { return state_announce_; }
    void setStateAnnounce(bool v) { state_announce_ = v; }

    // ChimpFlow gateway. Not persisted: url falls back to the
    // CHIMPFLOW_URL env var (default http://127.0.0.1:8090); the key
    // comes from CHIMPFLOW_KEY or the chimpflow.key file next to
    // prefs.json — a secret never written into prefs.json.
    static std::string chimpflowUrl();
    static std::string chimpflowKey();

    static std::string configPath();

private:
    std::string base_{"http://127.0.0.1:8090"};
    std::string api_key_;
    std::string agent_id_;
    std::string agent_name_;
    bool onboarding_done_{false};
    std::map<std::string, std::string> voices_;
    bool state_announce_{false};
};
