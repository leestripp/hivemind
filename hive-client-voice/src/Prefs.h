// Prefs — persisted voice settings (server base URL, API key, default
// agent, voice assignments, thresholds). JSON at
// ~/.config/hive-client-voice/prefs.json. Mirrors the Android Prefs.kt.
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

    // Per-agent voice assignment: agent id -> piper voice name
    // ("en_US-lessac-medium") or "" for the session default.
    std::string voiceFor(const std::string& agent_id) const;
    void setVoiceFor(const std::string& agent_id, const std::string& voice);

    // Audio thresholds (0..1 scale, mic RMS).
    float gateOpen() const { return gate_open_; }
    float gateClose() const { return gate_close_; }
    float vadPeak() const { return vad_peak_; }
    void setGateOpen(float v) { gate_open_ = v; }
    void setGateClose(float v) { gate_close_ = v; }
    void setVadPeak(float v) { vad_peak_ = v; }


    bool stateAnnounce() const { return state_announce_; }

    // TTS speech-rate multiplier (1.0 = normal, 2.0 = twice as fast).
    // Applied as piper --length_scale = 1/speed.
    double ttsSpeed() const { return tts_speed_; }
    void setTtsSpeed(double v) { tts_speed_ = v; }
    void setStateAnnounce(bool v) { state_announce_ = v; }

    static std::string configPath();
    static std::string voiceDir();  // ~/.local/share/hive-client-voice/voices

private:
    std::string base_{"http://127.0.0.1:8090"};
    std::string api_key_;
    std::string agent_id_;
    std::string agent_name_;
    bool onboarding_done_{false};
    std::map<std::string, std::string> voices_;
    float gate_open_{0.018f};
    float gate_close_{0.010f};
    float vad_peak_{0.012f};
    bool state_announce_{false};
    double tts_speed_{1.0};
};
