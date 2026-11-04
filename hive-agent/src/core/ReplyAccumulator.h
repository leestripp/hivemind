#pragma once

#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Collects user-facing output for one agent turn. Deliberate tell_user
// messages stay in call order; only the final assistant message's prose is
// appended. Tool-boundary prose is retained as a fallback, never concatenated
// into a later reply. stop.summary is a rescue path when no prose exists.
class ReplyAccumulator {
public:
    void begin();
    [[nodiscard]] std::string user_output() const;
    void append_assistant(std::string_view text);
    void on_tool_call(std::string_view name, const nlohmann::json& args);
    void on_tool_result(std::string_view name, const nlohmann::json& result);

private:
    static void append_paragraph(std::string& dst, const std::string& src);
    static bool is_turn_terminal_status(std::string_view name,
                                        const nlohmann::json& args);

    mutable std::mutex m_;
    std::string pending_;            // current assistant message's prose
    std::string committed_;          // tell_user messages, call order
    std::string stop_summary_;       // rescue only — see user_output()
    std::string last_intermediate_;  // last stashed pre-tool prose
};
