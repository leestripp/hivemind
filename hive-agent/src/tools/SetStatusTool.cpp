#include "tools/SetStatusTool.h"
#include "core/FileIO.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace {

// Public AgentState names — keep in sync with
// include/hive-brain/types/AgentState.h and
// demos/daedalus-webui/web_sse_sink.h.
//
// §IdleLoopFix (2026-08-20): the list is split into model-driven and
// runtime-owned states.  A model that had just finished its work used
// to read "Idle = no work in flight" and call set_status("Idle")
// expecting the agent loop to exit — but set_status only *reports*
// state to the host; the loop ends exclusively via the `stop` tool
// (or host cancel).  The tool returned status=success, so the model
// looped forever.  Idle / AwaitingApproval / Compressing are now
// rejected with an actionable error instead of being accepted.
constexpr std::array<std::string_view, 11> kValidStates = {
    "Idle",
    "Understanding",
    "Planning",
    "Acting",
    "Verifying",
    "Documenting",
    "AwaitingApproval",
    "Compressing",
    "Done",
    "Cancelled",
    "Error",
};

// States the library sets itself — never accepted from the model.
constexpr std::array<std::string_view, 3> kRuntimeOwnedStates = {
    "Idle",
    "AwaitingApproval",
    "Compressing",
};

// States the model may pass to set_status.
constexpr std::array<std::string_view, 8> kModelStates = {
    "Understanding",
    "Planning",
    "Acting",
    "Verifying",
    "Documenting",
    "Done",
    "Cancelled",
    "Error",
};

// Title-case helper: lowercase input, then upper-case the first letter.
// e.g. "planning" -> "Planning", "ACTING" -> "Acting".

// Return the canonical PascalCase form of the matched state, or ""
// if no match (case-insensitive).
std::string canonicalise(std::string_view name) {
    auto eq_ci = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (ca != cb) return false;
        }
        return true;
    };
    for (auto v : kValidStates) {
        if (eq_ci(name, v)) return std::string(v);
    }
    return {};
}

bool is_runtime_owned(std::string_view canonical) {
    for (auto v : kRuntimeOwnedStates) {
        if (v == canonical) return true;
    }
    return false;
}

std::string valid_states_list() {
    std::string out;
    for (size_t i = 0; i < kModelStates.size(); ++i) {
        if (i) out += ", ";
        out += std::string(kModelStates[i]);
    }
    return out;
}

}  // namespace

json SetStatusTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"state", json::object({
                        {"type", "string"},
                        {"description",
                         "The state to transition to. One of: Understanding, Planning, "
                         "Acting, Verifying, Documenting, Done, Cancelled, Error. "
                         "(Idle, AwaitingApproval and Compressing are runtime-managed — the "
                         "library sets them itself; passing them is an error.) "
                         "NOTE: set_status only updates the host UI stage — it NEVER exits "
                         "the agent loop, not even 'Done'. To end your turn, call the 'stop' "
                         "tool. "
                         "Case-insensitive; normalised to title-case in the response."}
                    })}
                })},
                {"required", json::array({"state"})}
            })}
        })}
    });
}

json SetStatusTool::execute(const json& args, FileIO& /*file_io*/) {
    try {
        // §Robustness: accept either {"state": "..."} object form, or a
        // JSON-string form that some legacy LLM clients emit.
        std::string raw_state;
        if (args.is_object()) {
            if (!args.contains("state")) {
                return json::object({
                    {"status", "error"},
                    {"message", "Missing 'state' in arguments"},
                });
            }
            if (!args["state"].is_string()) {
                return json::object({
                    {"status", "error"},
                    {"message", "'state' must be a string"},
                });
            }
            raw_state = args["state"].get<std::string>();
        } else if (args.is_string()) {
            try {
                auto parsed = json::parse(args.get<std::string>());
                if (!parsed.contains("state") || !parsed["state"].is_string()) {
                    return json::object({
                        {"status", "error"},
                        {"message", "Missing 'state' in arguments"},
                    });
                }
                raw_state = parsed["state"].get<std::string>();
            } catch (...) {
                raw_state = args.get<std::string>();
            }
        } else {
            return json::object({
                {"status", "error"},
                {"message", "Invalid argument format for set_status"},
            });
        }

        // Trim whitespace.
        auto first = raw_state.find_first_not_of(" \t\r\n");
        auto last  = raw_state.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return json::object({
                {"status", "error"},
                {"message", "'state' is empty"},
            });
        }
        std::string trimmed = raw_state.substr(first, last - first + 1);

        // Validate (case-insensitive match against the canonical list).
        std::string normalised = canonicalise(trimmed);
        if (normalised.empty()) {
            return json::object({
                {"status", "error"},
                {"message",
                 "Unknown state '" + trimmed + "'. Valid: " + valid_states_list() + "."},
            });
        }

        // §IdleLoopFix: runtime-owned states are recognised above (so we
        // can give a precise message) but never accepted.  Most important
        // is "Idle": the model must learn that no set_status call — not
        // even "Done" — exits the agent loop; only the `stop` tool does.
        if (is_runtime_owned(normalised)) {
            std::string msg =
                "'" + normalised + "' is runtime-owned — the library sets it "
                "itself; never pass it to set_status. ";
            if (normalised == "Idle") {
                msg +=
                    "Note: set_status only reports your stage to the host UI; "
                    "it does NOT exit the agent loop (no state does — not even "
                    "'Done'). To end your turn, call the 'stop' tool. ";
            }
            msg += "Model-driven states: " + valid_states_list() + ".";
            return json::object({
                {"status", "error"},
                {"message", msg},
            });
        }

        // The host-side state-change side-effect is triggered by
        // LLMClient::run_agent_cycle() when it sees status == "set_state".
        return json::object({
            {"status", "set_state"},
            {"state", normalised},
        });
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string(e.what())},
        });
    }
}