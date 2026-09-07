// hive-agent - the v2 "brain in the box" (Plan.md Phase A).
//
// A thin host around hive::Agent running INSIDE a podman container
// (or standalone on any host). Speaks NDJSON over stdio with the
// hive-worker; no HTTP, no OmniNet.
//
// Ops in (stdin, one JSON object per line):
//   {"op":"task","id":"t1","text":"...","thread_id":"...",
//    "attachments":[{"path":"/workspace/attachments/<tid>/x.png"}]}
//   {"op":"approval_resp","id":"a1","allow":true}
//   {"op":"cancel"}
//   {"op":"shutdown"}                     (persists history, exits 0)
//
// Events out (stdout, one JSON object per line):
//   {"ev":"ready","agent":"Name","version":"...","pid":123}
//   {"ev":"delta","task":"t1","text":"..."}
//   {"ev":"message","task":"t1","body":"...","error":""}
//   {"ev":"approval_request","id":"a1","command":"..."}
//   {"ev":"error","text":"..."}           (fatal/startup errors)
//
// Config via environment (injected by the worker at spawn; credentials
// live on the worker host, never on the server):
//   HIVEMIND_AGENT_NAME          (required)
//   HIVEMIND_AGENT_ID
//   HIVEMIND_API_HOST / HIVEMIND_API_KEY / HIVEMIND_CHAT_ENDPOINT
//   HIVEMIND_MODEL               (remote model id)
//   HIVEMIND_MAX_CONTEXT / HIVEMIND_TEMPERATURE
//   HIVEMIND_SYSTEM_PROMPT or HIVEMIND_SYSTEM_PROMPT_FILE
//   HIVEMIND_AUTO_APPROVE=1      (default: approvals become events)
//   HIVEMIND_SKILLS_DIR          (extra read-only skills mount;
//                                 ':'-separated for several)
//   HIVEMIND_MCP_SERVERS         (JSON array; seeded into hive-agent.db)
//   HIVEMIND_AGENT_HOME       (default <project>/.daedalus/daedalus-<Name>)
//
// argv[1] = project root (default CWD; the worker launches with
// /workspace so hive-brain state lands on the shared volume).

#include "library/Agent.h"
#include "core/settings.h"
#include "types/ProviderConfig.h"
#include "library/AgentSink.h"
#include "types/AgentConfig.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string env_or(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// NDJSON stdout writer (serialized: deltas stream off the turn thread
// while approval events may interleave).
std::mutex g_out_mtx;

void emit(const json& j) {
    std::lock_guard<std::mutex> lk(g_out_mtx);
    std::string line = j.dump();
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void log_err(const std::string& msg) {
    std::fprintf(stderr, "[hive-agent] %s\n", msg.c_str());
    std::fflush(stderr);
}

// Approval broker: hive-brain' sink hook blocks until an approval_resp
// arrives on stdin (or a 10-minute safety timeout denies).
class ApprovalBroker {
public:
    bool ask(const std::string& command) {
        const std::string id = "a" + std::to_string(++seq_);
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_[id] = std::nullopt;
        }
        emit({{"ev", "approval_request"}, {"id", id}, {"command", command}});
        std::unique_lock<std::mutex> lk(m_);
        const bool decided = cv_.wait_for(lk, std::chrono::minutes(10), [&] {
            return pending_[id].has_value();
        });
        const bool allow = decided && *pending_[id];
        pending_.erase(id);
        return allow;
    }

    void resolve(const std::string& id, bool allow) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            it->second = allow;
            cv_.notify_all();
        }
    }

    void deny_all() {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& kv : pending_)
            if (!kv.second) kv.second = false;
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::optional<bool>> pending_;
    std::atomic<int> seq_{0};
};

// NDJSON sink: hive-brain events -> protocol events; collects the turn's
// user-facing output (assistant prose + untruncated tell_user payloads,
// same semantics as the v1 hive-server sink).
class NdjsonSink : public hive::AgentSink {
public:
    explicit NdjsonSink(ApprovalBroker& approvals) : approvals_(approvals) {}

    void begin_task(const std::string& id) {
        std::lock_guard<std::mutex> lk(m_);
        task_id_ = id;
        user_output_.clear();
        errored_ = false;
    }

    std::string user_output() const {
        std::lock_guard<std::mutex> lk(m_);
        return user_output_;
    }
    bool errored() const { return errored_.load(); }

    void on_assistant_delta(std::string_view text) noexcept override {
        {
            std::lock_guard<std::mutex> lk(m_);
            user_output_ += text;
        }
        emit({{"ev", "delta"}, {"task", current_task()}, {"text", std::string(text)}});
    }

    void on_tool_result(std::string_view name,
                        std::string_view /*call_id*/,
                        const nlohmann::json& result) noexcept override {
        // tell_user carries a complete user-facing message: capture it
        // UNTRUNCATED - exactly what the chat should show.
        if (name == "tell_user") {
            const std::string content = result.value("content", "");
            if (!content.empty()) {
                std::lock_guard<std::mutex> lk(m_);
                if (!user_output_.empty() && user_output_.back() != '\n')
                    user_output_ += '\n';
                user_output_ += '\n';
                user_output_ += content;
            }
        }
    }

    // Brain state machine -> roster presence. hive-brain notifies every
    // transition; terminal turn states map back to "ready" (the agent is
    // idle and waiting for the next task, not "done" forever).
    void on_state_change(int /*from*/, int to) noexcept override {
        std::string st = hive::to_string(static_cast<hive::AgentState>(to));
        for (auto& c : st) c = (char)tolower((unsigned char)c);
        if (st == "done" || st == "cancelled") st = "ready";
        if (st == "idle") return;   // constructed/reset noise; fleet already knows
        emit({{"ev", "status"}, {"state", st}});
    }

    void on_error(hive::ErrorKind /*kind*/, std::string_view msg,
                  bool will_retry) noexcept override {
        errored_ = true;
        emit({{"ev", "log"},
              {"level", will_retry ? "warn" : "error"},
              {"text", std::string(msg)}});
    }

    void on_log(hive::LogLevel /*lvl*/,
                std::string_view msg) noexcept override {
        // Operational chatter goes to stderr, NOT the protocol stream.
        std::fwrite(msg.data(), 1, msg.size(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    bool request_command_approval(std::string_view command) override {
        return approvals_.ask(std::string(command));
    }

private:
    std::string current_task() const {
        std::lock_guard<std::mutex> lk(m_);
        return task_id_;
    }

    ApprovalBroker& approvals_;
    mutable std::mutex m_;
    std::string task_id_;
    std::string user_output_;
    std::atomic<bool> errored_{false};
};

// MCP seeding: write the roster-provided server specs into the agent's
// own hive-agent.db BEFORE Agent construction (same approach as v1's
// seed_mcp_servers, but hive-agent owns its db outright). hive-brain then
// spawns MCP servers as local child processes - in the container.
void seed_mcp_servers(const fs::path& home, const json& servers) {
    if (servers.empty()) return;
    std::error_code ec;
    fs::create_directories(home, ec);
    const fs::path db = home / "daedalus.db";
    sqlite3* s = nullptr;
    if (sqlite3_open(db.c_str(), &s) != SQLITE_OK) {
        log_err("mcp seed: cannot open " + db.string());
        if (s) sqlite3_close(s);
        return;
    }
    auto exec = [&](const char* sql) {
        char* err = nullptr;
        sqlite3_exec(s, sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    };
    exec("CREATE TABLE IF NOT EXISTS mcp_servers ("
         "name TEXT PRIMARY KEY, transport TEXT, command TEXT,"
         " args_json TEXT, env_json TEXT, url TEXT, bearer_token TEXT,"
         " description TEXT, enabled INTEGER DEFAULT 1)");
    // Config-authoritative: rows no longer declared are dropped.
    exec("DELETE FROM mcp_servers");
    for (const auto& srv : servers) {
        const std::string name = srv.value("name", "");
        const std::string command = srv.value("command", "");
        if (name.empty() || command.empty()) continue;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(s,
                "INSERT OR REPLACE INTO mcp_servers (name, transport,"
                " command, args_json, env_json, url, enabled)"
                " VALUES (?,?,?,?,?,?,1)",
                -1, &st, nullptr) != SQLITE_OK)
            continue;
        const std::string args = srv.value("args", json::array()).dump();
        const std::string env  = srv.value("env", json::object()).dump();
        const std::string url  = srv.value("url", "");
        const std::string transport =
            url.empty() ? "stdio" : "streamable-http";
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, transport.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, command.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, args.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, env.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, url.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE)
            log_err("mcp seed: insert failed for " + name);
        sqlite3_finalize(st);
    }
    sqlite3_close(s);
    log_err("mcp seed: " + std::to_string(servers.size()) + " server(s) seeded");
}

} // namespace

// Phase 35: current turn's thread id — set by the turn thread before
// each turn, read by AttachFileTool. Single turn thread → no locking.
static std::string g_thread_id;

// Phase 40: company roster, cached from each task frame (hub injects it
// in HubRouter::dispatch). The list_agents tool reads it so agents can
// discover one another before message_agent — without it an agent told
// to "message Forge" looped on echo-notes begging for a directory tool
// (2026-09-07 incident).
static json g_roster = json::array();
static std::mutex g_roster_mu;

int main(int argc, char** argv) {
    // -- config ----------------------------------------------------------
    const fs::path project_root =
        fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::current_path());
    const std::string name = env_or("HIVEMIND_AGENT_NAME");
    if (name.empty()) {
        log_err("HIVEMIND_AGENT_NAME is required");
        return 2;
    }

    hive::AgentConfig cfg;
    cfg.project_root = project_root;
    cfg.agent_name   = name;
    cfg.session_id   = "hive-" + name;   // stable prompt-cache key
    cfg.user_agent   = "hivemind-agent/" GIT_VERSION;

    // Per-agent state dir on the (shared) project volume: history, memory,
    // todos, logs, hive-agent.db — all at <project>/.hive-agent/<Name>/.
    // (Phase 43: was .daedalus/daedalus-<Name>/ — migrated in place below.)
    const std::string home_env = env_or("HIVEMIND_AGENT_HOME");
    if (!home_env.empty()) {
        cfg.agent_home = home_env;
    } else {
        fs::path home = project_root / ".hive-agent" / name;
        std::error_code ec;
        fs::path prev = project_root / ".daedalus" / ("daedalus-" + name);
        if (!fs::exists(home, ec) && fs::is_directory(prev, ec)) {
            fs::create_directories(home.parent_path(), ec);
            fs::rename(prev, home, ec);
            if (!ec) log_err("migrated agent state → " + home.string());
            ec.clear();
        }
        cfg.agent_home = home.string();
    }

    // Phase 43: no LLM env config — the hub owns the provider/model
    // registry and injects the resolved config (endpoint + key + tuning)
    // into every task frame; applied to the settings store before the turn.

    // Approvals: host-authoritative. Default off -> hive-brain asks the
    // sink; the sink emits approval_request and blocks for approval_resp.
    cfg.auto_approve = (env_or("HIVEMIND_AUTO_APPROVE") == "1");

    cfg.stream = true;
    cfg.max_agent_iterations = 600;

    // Skills: hive-brain auto-scans <project>/.agents/skills/ (project-local,
    // shared by every agent on the worker because /workspace is shared).
    // The read-only synced library comes via HIVEMIND_SKILLS_DIR mounts.
    {
        std::vector<fs::path> dirs;
        std::string extra = env_or("HIVEMIND_SKILLS_DIR");
        size_t pos = 0;
        while (!extra.empty()) {
            const size_t colon = extra.find(':', pos);
            dirs.emplace_back(extra.substr(
                pos, colon == std::string::npos ? colon : colon - pos));
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
        if (!dirs.empty()) cfg.skills_dirs = dirs;
    }

    // System prompt: inline env wins, then file, then hive-brain defaults.
    if (auto sp = env_or("HIVEMIND_SYSTEM_PROMPT"); !sp.empty()) {
        cfg.system_prompt = sp;
    } else if (auto f = env_or("HIVEMIND_SYSTEM_PROMPT_FILE"); !f.empty()) {
        std::ifstream in(f);
        cfg.system_prompt.assign(std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>());
    }

    // Phase 35: every agent learns attach_file regardless of role prompt
    // (SystemPromptBuilder composes the remaining sections regardless).
    cfg.system_prompt +=
        "\n\n## Sharing files in chat\n"
        "To show the user a file (a rendered video, image, archive - any "
        "deliverable), call attach_file with its /workspace path. It "
        "attaches the file to your reply: images and video render inline "
        "in the chat, other files appear as a download. Prefer this over "
        "only mentioning the file path.";

    // MCP specs arrive as JSON; seed before Agent construction.
    if (auto mcp = env_or("HIVEMIND_MCP_SERVERS"); !mcp.empty()) {
        try { seed_mcp_servers(cfg.agent_home, json::parse(mcp)); }
        catch (const std::exception& e) {
            log_err(std::string("mcp seed parse: ") + e.what());
        }
    }

    // -- agent + sink ----------------------------------------------------
    ApprovalBroker approvals;
    auto sink = std::make_shared<NdjsonSink>(approvals);

    hive::Agent agent(cfg, sink);
    // message_agent: async agent-to-agent via the hub (v1-validated
    // design). The tool emits an NDJSON event; the worker relays it as
    // agent_msg; the hub posts to the pair thread and the reply arrives
    // later as a new task. NEVER a synchronous nested turn.
    class MessageAgentTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "message_agent"; }
        std::string get_description() const override {
            return "Send an async message to another agent (by name or id). "
                   "Use list_agents first if you are unsure who exists. "
                   "The reply arrives later as a new task in your shared "
                   "pair thread - do NOT wait for it inline; continue your "
                   "own work.";
        }
        json get_definition() const override {
            json target = {{"type","string"},
                           {"description","Agent name or id"}};
            json text   = {{"type","string"},
                           {"description","Message body"}};
            json params = {{"type","object"},
                           {"properties",{{"target",target},{"text",text}}},
                           {"required",{"target","text"}}};
            return {{"type","function"},
                    {"function",{{"name","message_agent"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string target = args.value("target","");
            const std::string text   = args.value("text","");
            if (target.empty() || text.empty())
                return {{"status","error"},
                        {"error","target and text required"}};
            emit({{"ev","send_agent_message"},
                  {"target",target},{"text",text}});
            return {{"status","success"},
                    {"note","posted to the pair thread; the reply arrives "
                            "asynchronously as a new task"}};
        }
    };
    agent.register_tool(std::make_unique<MessageAgentTool>());

    // list_agents (Phase 40): company directory from the roster the hub
    // injects into every task frame.
    class ListAgentsTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "list_agents"; }
        std::string get_description() const override {
            return "List the agents in the company (name + id). Use this "
                   "to discover who exists before calling message_agent.";
        }
        json get_definition() const override {
            return {{"type","function"},
                    {"function",{{"name","list_agents"},
                                 {"description", get_description()},
                                 {"parameters",{{"type","object"},
                                                {"properties",json::object()}}}}}};
        }
        json execute(const json&, hive::HostToolContext&) override {
            std::lock_guard<std::mutex> lk(g_roster_mu);
            return {{"status","success"}, {"agents", g_roster}};
        }
    };
    agent.register_tool(std::make_unique<ListAgentsTool>());

    // attach_file (Phase 35): agent → chat attachments. Copies a
    // workspace file into the thread's outgoing dir; the worker ships it
    // with task_result and clients render it inline (images/video).
    class AttachFileTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "attach_file"; }
        std::string get_description() const override {
            return "Attach a file from /workspace to your reply so the user "
                   "sees it in the chat (images and video render inline; "
                   "other files appear as a download). Use it for "
                   "deliverables (rendered videos, images, archives) "
                   "instead of only mentioning the file path.";
        }
        json get_definition() const override {
            json path = {{"type","string"},
                         {"description","File to attach: absolute "
                          "/workspace/... path or workspace-relative"}};
            json params = {{"type","object"},
                           {"properties",{{"path",path}}},
                           {"required",{"path"}}};
            return {{"type","function"},
                    {"function",{{"name","attach_file"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            std::string p = args.value("path","");
            if (p.empty())
                return {{"status","error"},{"error","path required"}};
            if (g_thread_id.empty())
                return {{"status","error"},{"error","no active thread"}};
            if (p.rfind("/workspace/", 0) == 0) p = p.substr(11);
            std::error_code ec;
            const fs::path rel = fs::path(p).lexically_normal();
            bool traversal = rel.empty() || rel.is_absolute();
            for (const auto& c : rel) if (c == "..") traversal = true;
            if (traversal)
                return {{"status","error"},
                        {"error","path must stay under /workspace"}};
            const fs::path src = fs::path("/workspace") / rel;
            if (!fs::is_regular_file(src, ec))
                return {{"status","error"},
                        {"error","not a file: /workspace/" +
                         rel.generic_string()}};
            const fs::path outdir = fs::path("/workspace/attachments") /
                                    g_thread_id / "outgoing";
            fs::create_directories(outdir, ec);
            if (ec) return {{"status","error"},{"error",ec.message()}};
            std::string fname = src.filename().string();
            const std::string stem = src.stem().string();
            const std::string ext  = src.extension().string();
            for (int i = 2; fs::exists(outdir / fname); i++)
                fname = stem + "-" + std::to_string(i) + ext;
            fs::copy_file(src, outdir / fname, ec);
            if (ec) return {{"status","error"},{"error",ec.message()}};
            return {{"status","success"},
                    {"attached", fname},
                    {"note","attached to your reply when this turn ends"}};
        }
    };
    agent.register_tool(std::make_unique<AttachFileTool>());
    try {
        agent.initialize();
    } catch (const std::exception& e) {
        emit({{"ev", "error"}, {"text", std::string("init: ") + e.what()}});
        return 2;
    }
    emit({{"ev", "ready"},
          {"agent", name},
          {"version", GIT_VERSION},
          {"pid", (long long)::getpid()}});

    // -- turn thread: drains the task queue, one turn at a time ----------
    struct Task { std::string id, text, thread_id; };
    std::mutex q_mtx;
    std::condition_variable q_cv;
    std::deque<Task> queue;
    std::atomic<bool> stopping{false};

    std::thread turn_thread([&] {
        for (;;) {
            Task t;
            {
                std::unique_lock<std::mutex> lk(q_mtx);
                q_cv.wait(lk, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                t = std::move(queue.front());
                queue.pop_front();
            }
            g_thread_id = t.thread_id;
            sink->begin_task(t.id);
            std::string err;
            try {
                agent.submit_user_message(t.text);
                agent.run_one_turn();
            } catch (const std::exception& e) {
                err = e.what();
            }
            std::string body = sink->user_output();
            if (body.empty() && err.empty() && !sink->errored())
                body = "(agent produced no output)";
            emit({{"ev", "message"},
                  {"task", t.id},
                  {"thread_id", t.thread_id},
                  {"body", body},
                  {"error", err}});
        }
    });

    // -- stdin loop: NDJSON ops ------------------------------------------
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json op;
        try { op = json::parse(line); }
        catch (...) { log_err("bad json line ignored"); continue; }

        const std::string kind = op.value("op", "");
        if (kind == "task") {
            if (op.contains("roster") && op["roster"].is_array()) {
                std::lock_guard<std::mutex> lk(g_roster_mu);
                g_roster = op["roster"];
            }

            // Phase 43: hub-owned LLM config. The hub resolves the agent's
            // model → provider (endpoint + key) and injects it here; the
            // brain upserts into its settings store and switches model.
            // Any model can run on any machine; config changes are live.
            if (op.contains("llm") && op["llm"].is_object()) {
                const auto& L = op["llm"];
                hive::ProviderConfig pc;
                pc.name          = L.value("provider", "");
                pc.host          = L.value("host", "");
                pc.api_key       = L.value("api_key", "");
                pc.chat_endpoint = L.value("chat_endpoint", "");
                pc.plugin        = L.value("plugin", "");
                const std::string model    = L.value("model", "");
                const std::string model_id = L.value("model_id", model);
                if (auto* st = agent.settings(); st && !pc.name.empty() && !model.empty()) {
                    st->upsert_provider(pc);
                    hive::ModelConfig mc;
                    mc.name = model; mc.provider = pc.name; mc.model_id = model_id;
                    st->upsert_model(mc);
                    if (agent.switch_model(model))
                        log_err("llm config applied: " + model + " via " + pc.name);
                    else
                        log_err("llm config apply failed for model '" + model + "'");
                }
            }
            Task t{op.value("id", ""), op.value("text", ""),
                   op.value("thread_id", "")};
            // Attachments: the worker places them under
            // /workspace/attachments/<thread_id>/ first; the prompt gets
            // an explicit reference list so the agent knows they exist.
            if (op.contains("attachments") && op["attachments"].is_array() &&
                !op["attachments"].empty()) {
                t.text += "\n\n[Attached files]:";
                for (const auto& a : op["attachments"]) {
                    const std::string p = a.value("path", "");
                    if (!p.empty()) t.text += "\n- " + p;
                }
            }
            if (t.text.empty()) continue;
            {
                std::lock_guard<std::mutex> lk(q_mtx);
                queue.push_back(std::move(t));
            }
            q_cv.notify_one();
        } else if (kind == "approval_resp") {
            approvals.resolve(op.value("id", ""), op.value("allow", false));
        } else if (kind == "cancel") {
            agent.cancel();
        } else if (kind == "shutdown") {
            break;
        }
    }

    // stdin EOF (worker stopped the pipe) == shutdown.
    stopping = true;
    approvals.deny_all();
    q_cv.notify_all();
    if (turn_thread.joinable()) turn_thread.join();
    try { agent.shutdown(); }           // persists history + memory
    catch (const std::exception& e) { log_err(std::string("shutdown: ") + e.what()); }
    return 0;
}
