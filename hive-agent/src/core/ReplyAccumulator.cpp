#include "core/ReplyAccumulator.h"

#include <algorithm>
#include <cctype>

void ReplyAccumulator::begin() {
    std::lock_guard<std::mutex> lk(m_);
    pending_.clear();
    committed_.clear();
    stop_summary_.clear();
    last_intermediate_.clear();
}

std::string ReplyAccumulator::user_output() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string out = committed_;
    append_paragraph(out, pending_);
    if (out.empty()) out = stop_summary_;
    if (out.empty()) out = last_intermediate_;
    return out;
}

void ReplyAccumulator::append_assistant(std::string_view text) {
    std::lock_guard<std::mutex> lk(m_);
    pending_ += text;
}

void ReplyAccumulator::on_tool_call(std::string_view name,
                                    const nlohmann::json& args) {
    // stop and set_status(Done) end the turn, so prose immediately before
    // either one is the final reply rather than intermediate narration.
    if (name == "stop" || is_turn_terminal_status(name, args)) return;

    std::lock_guard<std::mutex> lk(m_);
    if (!pending_.empty()) {
        last_intermediate_ = pending_;
        pending_.clear();
    }
}

void ReplyAccumulator::on_tool_result(std::string_view name,
                                      const nlohmann::json& result) {
    std::lock_guard<std::mutex> lk(m_);
    if (name == "tell_user") {
        const std::string content = result.value("content", "");
        if (!content.empty()) append_paragraph(committed_, content);
    } else if (name == "stop") {
        const std::string summary = result.value("summary", "");
        if (!summary.empty()) stop_summary_ = summary;
    }
}

void ReplyAccumulator::append_paragraph(std::string& dst,
                                        const std::string& src) {
    if (src.empty()) return;
    if (!dst.empty()) {
        if (dst.back() != '\n') dst += '\n';
        dst += '\n';
    }
    dst += src;
}

bool ReplyAccumulator::is_turn_terminal_status(
    std::string_view name, const nlohmann::json& args) {
    if (name != "set_status") return false;
    std::string state = args.value("state", "");
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    state.erase(state.begin(),
                std::find_if(state.begin(), state.end(), not_space));
    state.erase(std::find_if(state.rbegin(), state.rend(), not_space).base(),
                state.end());
    return state.size() == 4 &&
           std::tolower(static_cast<unsigned char>(state[0])) == 'd' &&
           std::tolower(static_cast<unsigned char>(state[1])) == 'o' &&
           std::tolower(static_cast<unsigned char>(state[2])) == 'n' &&
           std::tolower(static_cast<unsigned char>(state[3])) == 'e';
}
