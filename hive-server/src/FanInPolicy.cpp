// FanInPolicy (Phase 65c/65h) — see FanInPolicy.h for the design notes.

#include "hivemind/FanInPolicy.h"

#include <algorithm>
#include <cctype>

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// 65c markers — harvested from the actual 2026-09-16 storm messages
// ("closing quietly", "context heartbeat", "no response required"…).
const char* kAckMarkers[] = {
    "closing quietly",
    "context heartbeat",
    "nothing to action",
    "no action needed",
    "no action requested",
    "nothing requested",
    "no response required",
    "no ack warranted",
    "no reply required",
    "nothing new",
    "nothing pending",
    "nothing further",
    "steady state",
    "going silent",
    "full silence",
    "no new message",
    "acknowledged",
    "stopping now",
    "stopped.",
    "thanks, lee",
    "thank you",
    "state is banked",
    "state banked",
    "machine self-running",
    "self-running",
    "fresh at 0%",
    "fresh context",
};

int count_markers(const std::string& lc) {
    int n = 0;
    for (const char* m : kAckMarkers)
        if (lc.find(m) != std::string::npos) ++n;
    return n;
}

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Case-insensitive whole-word search of `needle` in `hay` starting at
// `from`. Returns the match position or npos.
std::string::size_type find_word(const std::string& hay,
                                 const std::string& needle,
                                 std::string::size_type from) {
    if (needle.empty()) return std::string::npos;
    auto pos = hay.find(needle, from);
    while (pos != std::string::npos) {
        const bool left_ok  = (pos == 0) || !word_char(hay[pos - 1]);
        const auto end = pos + needle.size();
        const bool right_ok = (end >= hay.size()) || !word_char(hay[end]);
        if (left_ok && right_ok) return pos;
        pos = hay.find(needle, pos + 1);
    }
    return std::string::npos;
}

} // namespace

namespace hivemind {

bool fanin_is_ack_only(const std::string& body) {
    const std::string lc = lower(body);
    const int markers = count_markers(lc);
    if (markers >= 2) return true;
    // A single marker only qualifies a SHORT message — long substantive
    // replies can legitimately contain "acknowledged" mid-sentence.
    if (markers == 1 && body.size() <= 600) return true;
    return false;
}

std::vector<std::string> fanin_mentioned_ids(
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& id_names) {
    const std::string lc = lower(body);
    std::vector<std::string> out;
    for (const auto& [id, name] : id_names) {
        if (name.empty()) continue;
        const std::string ln = lower(name);
        bool hit = false;

        // Form 1: @Name (word-bounded)
        for (auto pos = lc.find('@'); pos != std::string::npos;
             pos = lc.find('@', pos + 1)) {
            if (find_word(lc, ln, pos + 1) == pos + 1) { hit = true; break; }
        }
        // Form 2: [to:Name] / [to: Name] (also [to:name1,name2])
        if (!hit) {
            for (auto pos = lc.find("[to:"); pos != std::string::npos;
                 pos = lc.find("[to:", pos + 1)) {
                const auto close = lc.find(']', pos);
                if (close == std::string::npos) break;
                const std::string inner = lc.substr(pos + 4, close - (pos + 4));
                if (find_word(inner, ln, 0) != std::string::npos) {
                    hit = true; break;
                }
            }
        }
        if (hit && std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    }
    return out;
}

} // namespace hivemind
