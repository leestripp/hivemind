// =============================================================================
// src/AgentTools.cpp — collaboration tools (HiveMind 2.0, Plan.md Phase 3)
// =============================================================================
#include "hivemind/AgentTools.h"
#include "hivemind/AgentStore.h"
#include "hivemind/AgentHost.h"     // AgentRegistry
#include "hivemind/ContainerManager.h"

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

using json = nlohmann::json;

thread_local int MessageAgentTool::depth_ = 0;

namespace {

json error(const std::string& msg) { return {{"status", "error"}, {"error", msg}}; }
json ok(json payload) { payload["status"] = "success"; return payload; }

// Find an agent by exact id, else by exact name (case-insensitive fallback).
std::optional<RosterAgent> find_agent(AgentStore& agents, const std::string& key) {
    if (auto a = agents.get_agent(key)) return a;
    if (auto a = agents.get_agent_by_name(key)) return a;
    std::string lower;
    for (char ch : key) lower += static_cast<char>(std::tolower(ch));
    for (auto& a : agents.list_agents()) {
        std::string ln;
        for (char ch : a.name) ln += static_cast<char>(std::tolower(ch));
        if (ln == lower) return a;
    }
    return std::nullopt;
}

json params_object(std::initializer_list<json> props,
                   std::initializer_list<const char*> required) {
    json properties = json::object();
    for (auto& p : props) properties.update(p);
    json req = json::array();
    for (auto* r : required) req.push_back(r);
    return {{"type", "object"}, {"properties", properties}, {"required", req}};
}

json fn_def(const std::string& name, const std::string& desc, json params) {
    return {{"type", "function"},
            {"function", {{"name", name}, {"description", desc},
                          {"parameters", params}}}};
}

} // namespace

// ─── ListAgentsTool ─────────────────────────────────────────────────────────

json ListAgentsTool::get_definition() const {
    return fn_def(get_name(), get_description(),
                  params_object({}, {}));
}

json ListAgentsTool::execute(const json&, daedalus::HostToolContext&) {
    json list = json::array();
    for (auto& a : agents_.list_agents()) {
        list.push_back({{"id", a.id}, {"name", a.name}, {"role", a.role}});
    }
    return ok({{"agents", list}});
}

// ─── MessageAgentTool ───────────────────────────────────────────────────────

json MessageAgentTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"agent", {{"type", "string"},
                             {"description", "Teammate name or id."}}}},
             json{{"message", {{"type", "string"},
                               {"description", "The task or question to send."}}}}},
            {"agent", "message"}));
}

json MessageAgentTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string target = args.value("agent", "");
    std::string message = args.value("message", "");
    if (target.empty() || message.empty())
        return error("need 'agent' and 'message'");

    if (depth_ >= kMaxDepth)
        return error("delegation depth limit reached — answer directly instead");

    auto agent = find_agent(agents_, target);
    if (!agent) return error("no agent named '" + target + "'");
    if (agent->id == sender_id_)
        return error("cannot message yourself");

    // Run the teammate's turn inline. Nested turns are same-thread, so the
    // recursive agent_exec_mutex admits them; cross-thread serialization is
    // unchanged. Depth guard above prevents delegation loops.
    auto& host = registry_.get_or_create(*agent);
    std::string prompt = "Message from your teammate " +
        ([&]{ auto s = agents_.get_agent(sender_id_); return s ? s->name : sender_id_; }()) +
        ": " + message;
    try {
        std::string reply = host.run_turn(prompt, nullptr);
        if (reply.size() > 4000) reply = reply.substr(0, 3997) + "...";
        return ok({{"from", agent->name}, {"reply", reply}});
    } catch (const std::exception& e) {
        return error(std::string{"teammate turn failed: "} + e.what());
    }
}

// ─── CreateAgentTool ────────────────────────────────────────────────────────

json CreateAgentTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"name", {{"type", "string"},
                            {"description", "Unique teammate name."}}}},
             json{{"role", {{"type", "string"},
                            {"description", "What this teammate does."}}}},
             json{{"model", {{"type", "string"},
                             {"description", "Optional model name (default: store default)."}}}}},
            {"name", "role"}));
}

json CreateAgentTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string name = args.value("name", "");
    std::string role = args.value("role", "");
    std::string model = args.value("model", "");
    try {
        auto a = agents_.create_agent(name, role, model);
        return ok({{"id", a.id}, {"name", a.name}, {"role", a.role}});
    } catch (const std::exception& e) {
        return error(e.what());
    }
}

// ─── RunIcarusTool ──────────────────────────────────────────────────────────

json RunIcarusTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"task", {{"type", "string"},
                            {"description", "Self-contained task for icarus."}}}},
             json{{"timeout", {{"type", "integer"},
                               {"description", "Timeout seconds (default 600)."}}}}},
            {"task"}));
}

json RunIcarusTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string task = args.value("task", "");
    if (task.empty()) return error("need 'task'");
    int timeout = args.value("timeout", 600);
    if (timeout <= 0 || timeout > 3600) timeout = 600;

    // Locate icarus: PATH first, then the usual user install location.
    std::string bin;
    if (std::system("command -v icarus >/dev/null 2>&1") == 0) {
        bin = "icarus";
    } else {
        std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
        std::string local = home + "/.local/bin/icarus";
        if (std::filesystem::exists(local)) bin = local;
    }
    if (bin.empty())
        return error("icarus is not installed on this machine "
                     "(gitlab.com/leestripp/icarus)");

    // Run icarus non-interactively in the agent's working directory.
    // CLI: icarus --help shows flags; we use the common prompt-arg form.
    auto quote = [](const std::string& s) {
        std::string out = "'";
        for (char ch : s) out += (ch == '\'') ? "'\\''" : std::string(1, ch);
        return out + "'";
    };
    std::string cmd = "cd " + quote(workdir_) + " && timeout " +
                      std::to_string(timeout) + " " + bin + " " + quote(task) +
                      " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return error("failed to spawn icarus");
    std::string output;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
        if (output.size() > 1 << 20) { output += "\n[output truncated]"; break; }
    }
    int rc = pclose(pipe);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return ok({{"exit_code", code},
               {"timed_out", code == 124},
               {"output", output}});
}
