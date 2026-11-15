#include "Prefs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string home() {
    if (const char* h = std::getenv("HOME")) return h;
    throw std::runtime_error("HOME not set");
}

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string keyPath() {
    return home() + "/.config/hive-client-voice/chimpflow.key";
}

struct FileDescriptor {
    explicit FileDescriptor(int value) : value(value) {}
    ~FileDescriptor() { if (value >= 0) ::close(value); }
    int value;
};

void saveKey(const std::string& key) {
    const std::filesystem::path path{keyPath()};
    std::filesystem::create_directories(path.parent_path());
    // Create with the final permissions from the outset: the API key must
    // never have even a brief group/world-readable window.
    FileDescriptor fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600)};
    if (fd.value < 0)
        throw std::runtime_error("Could not open ChimpFlow key file");
    // O_CREAT's mode does not tighten permissions on an existing file.
    if (::fchmod(fd.value, 0600) < 0)
        throw std::runtime_error("Could not secure ChimpFlow key file");
    const std::string bytes = key + '\n';
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n =
            ::write(fd.value, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Could not write ChimpFlow key file");
        written += static_cast<std::size_t>(n);
    }
}

}  // namespace

namespace voice_settings {

ChimpFlowEndpoint parseEndpoint(const std::string& url) {
    ChimpFlowEndpoint out;
    std::string rest = trim(url);
    if (rest.empty()) return out;
    const auto scheme = rest.find("://");
    if (scheme != std::string::npos) {
        if (rest.substr(0, scheme) == "https") out.port = "443";
        rest = rest.substr(scheme + 3);
    }
    if (const auto slash = rest.find('/'); slash != std::string::npos)
        rest.erase(slash);
    if (rest.empty()) return out;

    if (rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string::npos) {
            out.address = rest;
            return out;
        }
        out.address = rest.substr(0, close + 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':')
            out.port = rest.substr(close + 2);
        return out;
    }

    const auto first_colon = rest.find(':');
    const auto last_colon = rest.rfind(':');
    if (first_colon != std::string::npos && first_colon == last_colon) {
        out.address = rest.substr(0, last_colon);
        out.port = rest.substr(last_colon + 1);
    } else {
        out.address = rest;
    }
    return out;
}

std::string makeLoopbackUrl(const std::string& address,
                            const std::string& port) {
    std::string host = trim(address);
    const std::string number = trim(port);
    if (host == "[::1]") host = "::1";
    if (host != "localhost" && host != "127.0.0.1" && host != "::1")
        throw std::invalid_argument(
            "ChimpFlow address must be localhost, 127.0.0.1, or ::1");
    if (number.empty() ||
        !std::all_of(number.begin(), number.end(),
                     [](unsigned char c) { return std::isdigit(c); }))
        throw std::invalid_argument("ChimpFlow port must be a number");
    const int value = std::stoi(number);
    if (value < 1 || value > 65535)
        throw std::invalid_argument("ChimpFlow port must be between 1 and 65535");
    if (host == "::1") host = "[::1]";
    return "http://" + host + ":" + number;
}

}  // namespace voice_settings

std::string Prefs::configPath() {
    return home() + "/.config/hive-client-voice/prefs.json";
}

std::string Prefs::chimpflowUrl() const {
    if (!chimpflow_url_.empty()) return chimpflow_url_;
    if (const char* u = std::getenv("CHIMPFLOW_URL"))
        if (*u) return u;
    return "http://127.0.0.1:8090";
}

std::string Prefs::chimpflowKey() const {
    if (chimpflow_key_set_) return chimpflow_key_;
    if (const char* k = std::getenv("CHIMPFLOW_KEY"))
        if (*k) return k;
    std::ifstream f(keyPath());
    std::string key;
    std::getline(f, key);
    while (!key.empty() &&
           (key.back() == '\n' || key.back() == '\r' || key.back() == ' '))
        key.pop_back();
    return key;
}

void Prefs::setChimpflowKey(const std::string& value) {
    chimpflow_key_ = value;
    chimpflow_key_set_ = true;
    chimpflow_key_dirty_ = true;
}

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
    state_announce_ = j.value("state_announce", false);
    window_width_ = j.value("win_width", 0);
    window_height_ = j.value("win_height", 0);
    global_shortcut_token_ = j.value("global_shortcut_token", "");
    chimpflow_url_ = str("chimpflow_url");
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
    j["state_announce"] = state_announce_;
    j["win_width"] = window_width_;
    j["win_height"] = window_height_;
    j["global_shortcut_token"] = global_shortcut_token_;
    j["chimpflow_url"] = chimpflow_url_;
    j["voices"] = voices_;
    std::filesystem::create_directories(
        std::filesystem::path(configPath()).parent_path());
    std::ofstream f(configPath(), std::ios::trunc);
    f << j.dump(2) << '\n';
    if (!f) throw std::runtime_error("Could not write voice preferences");
    if (chimpflow_key_dirty_) {
        saveKey(chimpflow_key_);
        chimpflow_key_dirty_ = false;
    }
}

std::string Prefs::voiceFor(const std::string& agent_id) const {
    auto it = voices_.find(agent_id);
    return it == voices_.end() ? "" : it->second;
}

void Prefs::setVoiceFor(const std::string& agent_id, const std::string& voice) {
    voices_[agent_id] = voice;
}
