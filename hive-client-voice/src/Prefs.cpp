#include "Prefs.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
std::string home() {
    if (const char* h = std::getenv("HOME")) return h;
    throw std::runtime_error("HOME not set");
}
}  // namespace

std::string Prefs::configPath() { return home() + "/.config/hive-client-voice/prefs.json"; }
std::string Prefs::voiceDir() { return home() + "/.local/share/hive-client-voice/voices"; }

void Prefs::load() {
    std::ifstream f(configPath());
    if (!f) return;
    nlohmann::json j;
    try { f >> j; } catch (...) { return; }
    auto str = [&](const char* k) {
        return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : "";
    };
    if (!str("base").empty()) base_ = str("base");
    api_key_ = str("api_key");
    agent_id_ = str("agent_id");
    agent_name_ = str("agent_name");
    onboarding_done_ = j.value("onboarding_done", false);
    gate_open_ = j.value("gate_open", gate_open_);
    gate_close_ = j.value("gate_close", gate_close_);
    vad_peak_ = j.value("vad_peak", vad_peak_);
    state_announce_ = j.value("state_announce", false);
    if (j.contains("voices") && j["voices"].is_object())
        for (auto& [k, v] : j["voices"].items())
            if (v.is_string()) voices_[k] = v.get<std::string>();
}

void Prefs::save() const {
    nlohmann::json j;
    j["base"] = base_;
    j["api_key"] = api_key_;
    j["agent_id"] = agent_id_;
    j["agent_name"] = agent_name_;
    j["onboarding_done"] = onboarding_done_;
    j["gate_open"] = gate_open_;
    j["gate_close"] = gate_close_;
    j["vad_peak"] = vad_peak_;
    j["state_announce"] = state_announce_;
    j["voices"] = voices_;
    std::filesystem::create_directories(
        std::filesystem::path(configPath()).parent_path());
    std::ofstream f(configPath(), std::ios::trunc);
    f << j.dump(2) << '\n';
}

std::string Prefs::voiceFor(const std::string& agent_id) const {
    auto it = voices_.find(agent_id);
    return it == voices_.end() ? "" : it->second;
}

void Prefs::setVoiceFor(const std::string& agent_id, const std::string& voice) {
    voices_[agent_id] = voice;
}
