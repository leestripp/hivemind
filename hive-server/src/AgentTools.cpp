// =============================================================================
// src/AgentTools.cpp — collaboration tools (HiveMind 2.0, Plan.md Phase 3)
// =============================================================================
#include "hivemind/AgentTools.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/AgentStore.h"
#include "hivemind/AgentHost.h"     // AgentRegistry
#include "hivemind/ContainerManager.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/Schedule.h"
#include "hivemind/ThreadStore.h"

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

using json = nlohmann::json;


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

// ─── worker discovery (Phase 30) ────────────────────────────────────────────
namespace {
WorkerStore*   g_workers = nullptr;
WorkerChannel* g_wchannel = nullptr;
}

void agent_tools::install_worker_access(WorkerStore* workers,
                                        WorkerChannel* channel) {
    g_workers = workers;
    g_wchannel = channel;
}

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

    auto agent = find_agent(agents_, target);
    if (!agent) return error("no agent named '" + target + "'");
    if (agent->id == sender_id_)
        return error("cannot message yourself");

    // Async delivery (2026-08-25 rewrite): the OLD synchronous design ran
    // the teammate's entire turn inline via run_turn() — nested agent
    // loops, recursive mutexes, and a hang on its first live test. Now the
    // message is POSTED to the pair thread (visible in the client) and the
    // target is fanned in normally; its reply fans back, generation-capped.
    auto* chat = registry_.chat_service();
    if (!chat) return error("chat service unavailable");
    try {
        std::string tid =
            chat->post_agent_message(sender_id_, agent->id, message);
        return ok({{"delivered", true},
                   {"to", agent->name},
                   {"thread", tid},
                   {"note", "Async: the reply arrives later as a new "
                            "message from " + agent->name +
                            ". Do not wait for it."}});
    } catch (const std::exception& e) {
        return error(std::string{"delivery failed: "} + e.what());
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
                             {"description", "Optional model name (default: store default)."}}}},
             json{{"worker", {{"type", "string"},
                              {"description", "Optional hive-worker rig name to run this teammate on (see list_workers). Default 'local' = the server's own machine. Only online workers can start containers."}}}}},
            {"name", "role"}));
}

json CreateAgentTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string name = args.value("name", "");
    std::string role = args.value("role", "");
    std::string model = args.value("model", "");
    std::string worker = args.value("worker", "");
    // Validate placement up front so the agent gets a useful error instead of
    // a teammate that only fails later, at first container exec.
    if (!worker.empty() && worker != "local") {
        if (!g_workers || !g_workers->get(worker))
            return error("unknown worker '" + worker +
                         "' — use list_workers to see enrolled rigs");
        // No channel installed => nothing can be online (matches
        // ListWorkersTool's null-channel => online:false semantics).
        if (!g_wchannel || !g_wchannel->is_online(worker))
            return error("worker '" + worker +
                         "' is offline — it cannot start a container right now");
    }
    try {
        auto a = agents_.create_agent(name, role, model, "shared", false, "",
                                      worker);
        return ok({{"id", a.id}, {"name", a.name}, {"role", a.role},
                   {"worker", a.worker}});
    } catch (const std::exception& e) {
        return error(e.what());
    }
}

// ─── ListWorkersTool (Phase 30) ─────────────────────────────────────────────

json ListWorkersTool::get_definition() const {
    return fn_def(get_name(), get_description(), params_object({}, {}));
}

json ListWorkersTool::execute(const json&, daedalus::HostToolContext&) {
    if (!g_workers) return error("worker registry unavailable");
    json arr = json::array();
    for (auto& w : g_workers->list()) {
        json caps = json::object();
        try { caps = json::parse(w.caps); } catch (...) {}
        arr.push_back({{"name", w.name},
                       {"online", g_wchannel ? g_wchannel->is_online(w.name)
                                             : false},
                       {"caps", caps},
                       {"last_seen", w.last_seen}});
    }
    return ok({{"workers", arr},
               {"note", "Pass a worker's name as 'worker' to create_agent. "
                        "Empty list = only the server itself ('local') is "
                        "available."}});
}

// ─── SaveRoutineTool (Phase 20: learn-by-demonstration) ─────────────────────

json SaveRoutineTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"name", {{"type", "string"},
                            {"description", "Short routine name, e.g. 'morning-standup-digest'."}}}},
             json{{"prompt", {{"type", "string"},
                              {"description", "Self-contained instruction for future runs: the distilled steps, URLs, paths, values. No references to 'the chat above'."}}}},
             json{{"schedule", {{"type", "object"},
                                {"description", "Schedule spec: {type:'once',at:'YYYY-MM-DDTHH:MM'} | {type:'interval',every_minutes:N} | {type:'daily',time:'HH:MM'} | {type:'weekly',days:[0-6],time:'HH:MM'} (UTC)."}}}}},
            {"name", "prompt", "schedule"}));
}

json SaveRoutineTool::execute(const json& args, daedalus::HostToolContext&) {
    const std::string name = args.value("name", "");
    const std::string prompt = args.value("prompt", "");
    if (name.empty()) return error("need 'name'");
    if (prompt.empty()) return error("need 'prompt'");
    if (!args.contains("schedule") || !args["schedule"].is_object())
        return error("need 'schedule' object");
    const json schedule = args["schedule"];
    const std::string bad = hm::validate_schedule(schedule);
    if (!bad.empty()) return error("invalid schedule: " + bad);
    try {
        // Same shape as RoutineRoutes: one dedicated thread per routine.
        auto t = threads_.create_thread("routine: " + name, "dm", {agent_id_});
        auto next = hm::compute_next_fire(schedule, hm::now_iso_local(), "");
        auto r = routines_.create(agent_id_, t.id, name, prompt,
                                  schedule.dump(), next.value_or(""));
        return ok({{"routine_id", r.id}, {"thread_id", t.id},
                   {"name", r.name},
                   {"next_fire_at", r.next_fire_at},
                   {"note", "Routine saved and scheduled. Tell the user its "
                            "name and next fire time."}});
    } catch (const std::exception& e) {
        return error(e.what());
    }
}

// ─── ListRoutinesTool ───────────────────────────────────────────────────────

json ListRoutinesTool::get_definition() const {
    return fn_def(get_name(), get_description(), params_object({}, {}));
}

json ListRoutinesTool::execute(const json&, daedalus::HostToolContext&) {
    json arr = json::array();
    for (auto& r : routines_.list()) {
        if (r.agent_id != agent_id_) continue;   // own routines only
        arr.push_back({{"id", r.id}, {"name", r.name},
                       {"schedule", json::parse(r.schedule_json, nullptr, false)},
                       {"enabled", r.enabled},
                       {"next_fire_at", r.next_fire_at},
                       {"last_fired_at", r.last_fired_at}});
    }
    return ok({{"routines", arr},
               {"note", arr.empty()
                   ? "You have no routines. save_routine creates one."
                   : "Use update_routine / delete_routine with an id."}});
}

// ─── UpdateRoutineTool ──────────────────────────────────────────────────────

json UpdateRoutineTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"id", {{"type", "string"},
                          {"description", "Routine id (see list_routines)."}}}},
             json{{"name", {{"type", "string"},
                            {"description", "New name (omit to keep)."}}}},
             json{{"prompt", {{"type", "string"},
                              {"description", "New self-contained prompt (omit to keep)."}}}},
             json{{"schedule", {{"type", "object"},
                                {"description", "New schedule spec (UTC) — same shape as save_routine (omit to keep)."}}}},
             json{{"enabled", {{"type", "boolean"},
                               {"description", "false pauses, true resumes (omit to keep)."}}}}},
            {"id"}));
}

json UpdateRoutineTool::execute(const json& args, daedalus::HostToolContext&) {
    const std::string id = args.value("id", "");
    if (id.empty()) return error("need 'id'");
    auto r = routines_.get(id);
    if (!r) return error("unknown routine id '" + id + "'");
    if (r->agent_id != agent_id_)
        return error("routine '" + r->name + "' belongs to another agent — "
                     "you can only change your own");

    const std::string name = args.value("name", r->name);
    const std::string prompt = args.value("prompt", r->prompt);
    if (name.empty() || prompt.empty())
        return error("name and prompt must not be empty");

    json schedule = json::parse(r->schedule_json, nullptr, false);
    std::string next = r->next_fire_at;
    if (args.contains("schedule")) {
        if (!args["schedule"].is_object()) return error("schedule must be an object");
        const std::string bad = hm::validate_schedule(args["schedule"]);
        if (!bad.empty()) return error("invalid schedule: " + bad);
        schedule = args["schedule"];
        // Recompute from now — same policy as RoutineRoutes PATCH.
        next = hm::compute_next_fire(schedule, hm::now_iso_local(), r->created_at)
                   .value_or("");
    }
    if (!routines_.update(id, name, prompt, schedule.dump(), next))
        return error("update failed");
    if (args.contains("enabled") && args["enabled"].is_boolean())
        routines_.set_enabled(id, args["enabled"].get<bool>());
    return ok({{"id", id}, {"name", name}, {"next_fire_at", next},
               {"enabled", routines_.get(id)->enabled}});
}

// ─── DeleteRoutineTool ──────────────────────────────────────────────────────

json DeleteRoutineTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"id", {{"type", "string"},
                          {"description", "Routine id (see list_routines)."}}}}},
            {"id"}));
}

json DeleteRoutineTool::execute(const json& args, daedalus::HostToolContext&) {
    const std::string id = args.value("id", "");
    if (id.empty()) return error("need 'id'");
    auto r = routines_.get(id);
    if (!r) return error("unknown routine id '" + id + "'");
    if (r->agent_id != agent_id_)
        return error("routine '" + r->name + "' belongs to another agent — "
                     "you can only delete your own");
    if (!routines_.remove(id)) return error("delete failed");
    return ok({{"deleted", true}, {"name", r->name}});
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
