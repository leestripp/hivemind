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
#include "hivemind/WorkerRouter.h"
#include "hivemind/ServerConfig.h"
#include "hivemind/Files.h"

#include <cstdio>
#include <cctype>
#include <sstream>
#include <system_error>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sys/wait.h>

namespace fs = std::filesystem;
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

// Find one of the agent's OWN routines by exact id, else exact name,
// else case-insensitive name. Agents think in names, not UUIDs.
std::optional<Routine> find_routine(RoutineStore& routines,
                                    const std::string& agent_id,
                                    const std::string& key) {
    auto own = [&]() {
        std::vector<Routine> out;
        for (auto& r : routines.list())
            if (r.agent_id == agent_id) out.push_back(r);
        return out;
    };
    if (auto r = routines.get(key))
        if (r->agent_id == agent_id) return r;
    for (auto& r : own())
        if (r.name == key) return r;
    std::string lower;
    for (char ch : key) lower += static_cast<char>(std::tolower(ch));
    for (auto& r : own()) {
        std::string ln;
        for (char ch : r.name) ln += static_cast<char>(std::tolower(ch));
        if (ln == lower) return r;
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

// ─── SendFileTool ───────────────────────────────────────────────────────────

json SendFileTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"path", {{"type", "string"},
                  {"description", "Source file in YOUR /workspace (e.g. 'drafts/report.md' or '/workspace/drafts/report.md')."}}}},
             json{{"dest", {{"type", "string"},
                  {"description", "Destination path relative to the company workspace root (e.g. 'departments/engineering/report.md'). The folder must already exist."}}}},
             json{{"overwrite", {{"type", "boolean"},
                  {"description", "Replace an existing file at dest (default false)."}}}},
             json{{"notify", {{"type", "string"},
                  {"description", "Optional teammate name to notify in your shared thread when the file lands."}}}}},
            {"path", "dest"}));
}

json SendFileTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string path = args.value("path", "");
    std::string dest = args.value("dest", "");
    const bool overwrite = args.value("overwrite", false);
    const std::string notify = args.value("notify", "");
    if (path.empty() || dest.empty())
        return error("need 'path' and 'dest'");

    // Normalise the source to a /workspace-relative path.
    std::string rel = path;
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    if (rel.rfind("workspace/", 0) == 0) rel.erase(0, 10);
    if (rel.empty() || rel.find("..") != std::string::npos)
        return error("path must be a file inside your /workspace");

    constexpr size_t kMaxBytes = 50u * 1024 * 1024;
    const fs::path ws_root = ServerConfig::instance().workspace_root();

    // ── fetch the bytes ─────────────────────────────────────────────
    std::string bytes;
    if (auto remote = WorkerRouter::fetch_file(sender_id_, rel)) {
        if (!remote->ok) return error("fetch failed: " + remote->error);
        bytes = std::move(remote->data);
    } else {
        // Local agent: the company workspace IS the agent's /workspace.
        auto src = hm::files::resolve_confined(ws_root, rel);
        if (!src) return error("not found in workspace: " + rel);
        std::error_code ec;
        const auto sz = fs::file_size(*src, ec);
        if (ec) return error("stat failed");
        if (sz > kMaxBytes) return error("file too large (50MB cap)");
        std::ifstream in(*src, std::ios::binary);
        if (!in) return error("read failed");
        bytes.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
        // Same-dir no-op guard: sending a file onto itself.
        auto dst_check = hm::files::resolve_confined_create(ws_root, dest);
        if (dst_check && fs::equivalent(*src, *dst_check, ec) && !ec)
            return error("source and destination are the same file");
    }
    if (bytes.size() > kMaxBytes) return error("file too large (50MB cap)");

    // ── confined write into the company workspace ───────────────────
    auto dst = hm::files::resolve_confined_create(ws_root, dest);
    if (!dst)
        return error("bad destination (must be inside the company "
                     "workspace, in an existing folder)");
    std::error_code ec;
    if (fs::exists(*dst, ec) && !overwrite)
        return error("'" + dest + "' already exists (pass overwrite=true "
                     "to replace it)");

    {
        std::ofstream out(*dst, std::ios::binary | std::ios::trunc);
        if (!out) return error("write failed (permissions?)");
        out.write(bytes.data(), (std::streamsize)bytes.size());
        if (!out) return error("write failed mid-file");
    }

    // ── optional teammate notification ──────────────────────────────
    json res{{"delivered", true},
             {"dest", dest},
             {"bytes", (int64_t)bytes.size()}};
    if (!notify.empty()) {
        auto target = find_agent(agents_, notify);
        auto* chat = registry_.chat_service();
        if (target && chat && target->id != sender_id_) {
            auto me = agents_.get_agent(sender_id_);
            const std::string from = me ? me->name : "A teammate";
            const double kb = (double)bytes.size() / 1024.0;
            char sz[64];
            std::snprintf(sz, sizeof sz, "%.1f KB", kb);
            try {
                chat->post_agent_message(
                    sender_id_, target->id,
                    "📎 " + from + " sent a file to the company workspace: "
                    "`/workspace/" + dest + "` (" + sz + ")");
                res["notified"] = target->name;
            } catch (...) {}
        }
    }
    return ok(res);
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
    // Duplicate names silently made second routines — point at update instead.
    if (find_routine(routines_, agent_id_, name))
        return error("you already have a routine named '" + name +
                     "' — use update_routine to change it, or pick another name");
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
        json sched = json::parse(r.schedule_json, nullptr, false);
        arr.push_back({{"id", r.id}, {"name", r.name},
                       {"schedule", sched},
                       {"schedule_text", sched.is_object()
                            ? hm::describe_schedule(sched) : r.schedule_json},
                       {"prompt", r.prompt},
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
            {json{{"routine", {{"type", "string"},
                               {"description", "Routine name or id (see list_routines)."}}}},
             json{{"name", {{"type", "string"},
                            {"description", "New name (omit to keep)."}}}},
             json{{"prompt", {{"type", "string"},
                              {"description", "New self-contained prompt (omit to keep)."}}}},
             json{{"schedule", {{"type", "object"},
                                {"description", "New schedule spec (UTC) — same shape as save_routine (omit to keep)."}}}},
             json{{"enabled", {{"type", "boolean"},
                               {"description", "false pauses, true resumes (omit to keep)."}}}}},
            {"routine"}));
}

json UpdateRoutineTool::execute(const json& args, daedalus::HostToolContext&) {
    const std::string key = args.value("routine", args.value("id", ""));
    if (key.empty()) return error("need 'routine' (name or id)");
    auto r = find_routine(routines_, agent_id_, key);
    if (!r) return error("no routine of yours named '" + key +
                         "' — see list_routines");
    const std::string id = r->id;

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
            {json{{"routine", {{"type", "string"},
                               {"description", "Routine name or id (see list_routines)."}}}}},
            {"routine"}));
}

json DeleteRoutineTool::execute(const json& args, daedalus::HostToolContext&) {
    const std::string key = args.value("routine", args.value("id", ""));
    if (key.empty()) return error("need 'routine' (name or id)");
    auto r = find_routine(routines_, agent_id_, key);
    if (!r) return error("no routine of yours named '" + key +
                         "' — see list_routines");
    const std::string id = r->id;
    if (!routines_.remove(id)) return error("delete failed");
    return ok({{"deleted", true}, {"name", r->name}});
}

// ─── skill marketplace tools ────────────────────────────────────────────────
namespace {

fs::path skills_registry_path() {
    return fs::current_path() / "web" / "dist" / "skills-registry.json";
}

fs::path global_skills_root() {
    return AgentStore::agents_base_dir().parent_path() / "skills";
}

fs::path agent_skills_root(const std::string& agent_id) {
    return AgentStore::agents_base_dir() / agent_id / ".agents" / "skills";
}

std::string lower_str(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

// Registry is either a bare array or {"entries":[...]}.
json load_registry() {
    std::ifstream f{skills_registry_path()};
    if (!f) return json::array();
    try {
        json d = json::parse(f);
        if (d.is_array()) return d;
        return d.value("entries", json::array());
    } catch (...) { return json::array(); }
}

bool valid_skill_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Minimal SKILL.md frontmatter reader (name/description/version).
json skill_frontmatter(const fs::path& p) {
    json fm = json::object();
    std::ifstream f{p};
    std::string line;
    if (!std::getline(f, line) || line != "---") return fm;
    while (std::getline(f, line)) {
        if (line == "---") break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        if (key == "name" || key == "description" || key == "version")
            fm[key] = val;
    }
    return fm;
}

} // namespace

json hivemind::SearchSkillsTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"query", {{"type", "string"},
                  {"description", "Keyword(s) to match against skill name, description, and tags (e.g. 'remotion', 'video', 'pdf')."}}}}},
            {"query"}));
}

json hivemind::SearchSkillsTool::execute(const json& args, daedalus::HostToolContext&) {
    const std::string q = lower_str(args.value("query", ""));
    if (q.empty()) return error("need 'query'");
    const json entries = load_registry();
    if (entries.empty())
        return error("skill registry not found (web/dist/skills-registry.json)");
    json matches = json::array();
    for (const auto& e : entries) {
        std::string hay = lower_str(e.value("name", "") + " " +
                                    e.value("description", ""));
        for (const auto& t : e.value("tags", json::array()))
            if (t.is_string()) hay += " " + lower_str(t.get<std::string>());
        bool match = true;   // every query word must appear
        std::istringstream iss(q);
        for (std::string w; iss >> w;)
            if (hay.find(w) == std::string::npos) { match = false; break; }
        if (!match) continue;
        const std::string name = e.value("name", "");
        std::error_code ec;
        const bool installed =
            fs::exists(global_skills_root() / name / "SKILL.md", ec);
        matches.push_back({{"name", name},
                           {"description", e.value("description", "")},
                           {"tags", e.value("tags", json::array())},
                           {"source", e.value("source", "")},
                           {"installed", installed}});
    }
    return ok(json{{"matches", matches}, {"count", matches.size()}});
}

json hivemind::InstallSkillTool::get_definition() const {
    return fn_def(get_name(), get_description(),
        params_object(
            {json{{"name", {{"type", "string"},
                  {"description", "Skill name from search_skills results (registry lookup)."}}}},
             json{{"git_url", {{"type", "string"},
                  {"description", "Explicit git URL (only if the skill is not in the registry)."}}}},
             json{{"path", {{"type", "string"},
                  {"description", "Subdirectory inside the repo containing SKILL.md (monorepos; the registry's 'path' field)."}}}}},
            {}));
}

json hivemind::InstallSkillTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string name = args.value("name", "");
    std::string url  = args.value("git_url", "");
    std::string sub  = args.value("path", "");

    if (!name.empty() && url.empty()) {
        for (const auto& e : load_registry()) {
            if (e.value("name", "") == name) {
                url = e.value("git_url", "");
                if (sub.empty()) sub = e.value("path", "");
                break;
            }
        }
        if (url.empty())
            return error("skill '" + name +
                         "' not in registry — run search_skills first, or "
                         "pass git_url explicitly");
    }
    if (url.empty()) return error("need 'name' (registry) or 'git_url'");

    // Same input hygiene as the /api/skills/install route.
    auto safe = [](const std::string& v, size_t max) {
        if (v.empty() || v.size() > max) return false;
        for (unsigned char c : v) {
            if (std::isalnum(c)) continue;
            if (std::string(":/.-_@~%+").find(static_cast<char>(c)) !=
                std::string::npos)
                continue;
            return false;
        }
        return true;
    };
    if (!safe(url, 300)) return error("bad git_url");
    if (!sub.empty() && (!safe(sub, 120) || sub.find("..") != std::string::npos))
        return error("bad path");

    char tmpl[] = "/tmp/hive-skill-XXXXXX";
    if (!mkdtemp(tmpl)) return error("tmp dir failed");
    const fs::path tmp = tmpl;
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
            fs::remove(p.string() + ".log", ec);
        }
    } cleanup{tmp};

    const std::string cmd = "timeout 90 git clone --depth 1 '" + url + "' '" +
                            tmp.string() + "' >'" + tmp.string() + ".log' 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        std::ifstream lf(tmp.string() + ".log");
        std::string tail, line;
        while (std::getline(lf, line)) tail = line;   // last line
        return error("git clone failed: " + tail);
    }

    const fs::path src = sub.empty() ? tmp : tmp / sub;
    std::error_code ec;
    if (!fs::exists(src / "SKILL.md", ec))
        return error("no SKILL.md at repo " +
                     (sub.empty() ? std::string("root")
                                  : "path '" + sub + "'"));
    json fm = skill_frontmatter(src / "SKILL.md");
    std::string sname = fm.value("name", "");
    if (sname.empty()) sname = src.filename().string();
    if (!valid_skill_name(sname))
        return error("invalid skill name '" + sname + "'");

    fs::create_directories(global_skills_root(), ec);
    const fs::path dest = global_skills_root() / sname;
    if (fs::exists(dest, ec))
        return error("skill '" + sname + "' already installed in the shared "
                     "library — ask the user to enable it for you in the web UI");
    fs::copy(src, dest, fs::copy_options::recursive, ec);
    if (ec) return error("copy failed: " + ec.message());

    // Enable for the calling agent — same relative-link scheme as the
    // /api/agents/:id/global-skills route.
    const fs::path link = agent_skills_root(agent_id_) / sname;
    if (fs::exists(link, ec) && !fs::is_symlink(link, ec))
        return ok(json{{"installed", sname}, {"enabled", false},
                       {"warning", "installed to the shared library but NOT "
                                   "enabled: you have a local skill named '" +
                                   sname + "'"}});
    fs::create_directories(link.parent_path(), ec);
    fs::remove(link, ec);   // refresh existing link
    fs::create_symlink(fs::path("../../../../skills") / sname, link, ec);
    if (ec)
        return ok(json{{"installed", sname}, {"enabled", false},
                       {"warning", "installed but enable-symlink failed: " +
                                   ec.message()}});

    return ok(json{{"installed", sname},
                   {"description", fm.value("description", "")},
                   {"version", fm.value("version", "")},
                   {"enabled", true},
                   {"note", "Skill is at ~/.agents/skills/" + sname +
                            "/ — read its SKILL.md before using it."}});
}
