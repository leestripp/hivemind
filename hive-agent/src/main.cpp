// hive-agent - the v2 "brain in the box" (Plan.md Phase A).
//
// A thin host around daedalus::Agent running INSIDE a podman container
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
//   HIVEMIND_MCP_SERVERS         (JSON array; seeded into daedalus.db)
//   HIVEMIND_DAEDALUS_HOME       (default <project>/.daedalus/daedalus-<Name>)
//
// argv[1] = project root (default CWD; the worker launches with
// /workspace so daedalus state lands on the shared volume).

#include <daedalus/Agent.h>
#include <daedalus/AgentSink.h>
#include <daedalus/types/AgentConfig.h>

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

// Approval broker: daedalus' sink hook blocks until an approval_resp
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

// NDJSON sink: daedalus events -> protocol events; collects the turn's
// user-facing output (assistant prose + untruncated tell_user payloads,
// same semantics as the v1 hive-server sink).
class NdjsonSink : public daedalus::AgentSink {
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

    void on_error(daedalus::ErrorKind /*kind*/, std::string_view msg,
                  bool will_retry) noexcept override {
        errored_ = true;
        emit({{"ev", "log"},
              {"level", will_retry ? "warn" : "error"},
              {"text", std::string(msg)}});
    }

    void on_log(daedalus::LogLevel /*lvl*/,
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
// own daedalus.db BEFORE Agent construction (same approach as v1's
// seed_mcp_servers, but hive-agent owns its db outright). daedalus then
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

int main(int argc, char** argv) {
    // -- config ----------------------------------------------------------
    const fs::path project_root =
        fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::current_path());
    const std::string name = env_or("HIVEMIND_AGENT_NAME");
    if (name.empty()) {
        log_err("HIVEMIND_AGENT_NAME is required");
        return 2;
    }

    daedalus::AgentConfig cfg;
    cfg.project_root = project_root;
    cfg.agent_name   = name;
    cfg.session_id   = "hive-" + name;   // stable prompt-cache key
    cfg.user_agent   = "hivemind-agent/" GIT_VERSION;

    // Per-agent brain dir on the (shared) project volume: history, memory
    // and daedalus.db all live at <project>/.daedalus/daedalus-<Name>/.
    cfg.daedalus_home = env_or(
        "HIVEMIND_DAEDALUS_HOME",
        (project_root / ".daedalus" / ("daedalus-" + name)).string());

    // LLM credentials injected by the worker (explicit fields; daedalus
    // falls back to them when model_name is not a settings key).
    cfg.model_name     = env_or("HIVEMIND_MODEL");
    cfg.api_host       = env_or("HIVEMIND_API_HOST");
    cfg.api_key        = env_or("HIVEMIND_API_KEY");
    cfg.chat_endpoint  = env_or("HIVEMIND_CHAT_ENDPOINT");
    if (auto v = env_or("HIVEMIND_MAX_CONTEXT"); !v.empty())
        cfg.max_context_size = std::stoul(v);
    if (auto v = env_or("HIVEMIND_TEMPERATURE"); !v.empty())
        cfg.temperature = std::stof(v);

    // Approvals: host-authoritative. Default off -> daedalus asks the
    // sink; the sink emits approval_request and blocks for approval_resp.
    cfg.auto_approve = (env_or("HIVEMIND_AUTO_APPROVE") == "1");

    cfg.stream = true;
    cfg.max_agent_iterations = 600;

    // Skills: daedalus auto-scans <project>/.agents/skills/ (project-local,
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

    // System prompt: inline env wins, then file, then daedalus defaults.
    if (auto sp = env_or("HIVEMIND_SYSTEM_PROMPT"); !sp.empty()) {
        cfg.system_prompt = sp;
    } else if (auto f = env_or("HIVEMIND_SYSTEM_PROMPT_FILE"); !f.empty()) {
        std::ifstream in(f);
        cfg.system_prompt.assign(std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>());
    }

    // MCP specs arrive as JSON; seed before Agent construction.
    if (auto mcp = env_or("HIVEMIND_MCP_SERVERS"); !mcp.empty()) {
        try { seed_mcp_servers(cfg.daedalus_home, json::parse(mcp)); }
        catch (const std::exception& e) {
            log_err(std::string("mcp seed parse: ") + e.what());
        }
    }

    // -- agent + sink ----------------------------------------------------
    ApprovalBroker approvals;
    auto sink = std::make_shared<NdjsonSink>(approvals);

    daedalus::Agent agent(cfg, sink);
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
