// hive-agent - the v2 "brain in the box" (Plan.md Phase A).
//
// A thin host around hive::Agent running INSIDE a podman container
// (or standalone on any host). Speaks NDJSON over stdio with the
// hive-worker; no HTTP, no OmniNet.
//
// Ops in (stdin, one JSON object per line):
//   {"op":"task","id":"t1","text":"...","thread_id":"...",
//    "attachments":[{"path":"/workspace/attachments/<tid>/x.png"}]}
//   {"op":"cancel"}
//   {"op":"shutdown"}                     (persists history, exits 0)
//
// Events out (stdout, one JSON object per line):
//   {"ev":"ready","agent":"Name","version":"...","pid":123}
//   {"ev":"delta","task":"t1","text":"..."}
//   {"ev":"message","task":"t1","body":"...","error":""}
//   {"ev":"background_done","process_id":"bg_1","thread_id":"...",...}
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
//   HIVEMIND_SKILLS_DIR          (read-only synced skills library mount;
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
#include "core/McpSeed.h"
#include "core/BackgroundProcessManager.h"
#include "core/Utf8Utils.h"
#include "tools/BrowserTools.h"
#include "tools/SecretTools.h"
#include "tools/ServiceTools.h"

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
#include <unordered_set>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string env_or(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// NDJSON stdout writer (serialized: deltas stream off the turn thread
// while response events may interleave).
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

// Routine bridge (Phase 53): request/response to the hub for scheduled
// tasks. Tools emit {"ev":"routine_req",...}; the worker forwards it as
// an op frame; the hub applies it against RoutineStore and replies with
// {"op":"routine_resp","id":...,"ok":bool,"result"|"error"} on stdin.
// Request/response bridge with a 30s timeout.
class RoutineBridge {
public:
    json call(const std::string& action, const json& payload) {
        const std::string id = "r" + std::to_string(++seq_);
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_[id] = std::nullopt;
        }
        json ev = {{"ev", "routine_req"}, {"id", id}, {"action", action}};
        for (auto& [k, v] : payload.items()) ev[k] = v;
        emit(ev);
        std::unique_lock<std::mutex> lk(m_);
        const bool got = cv_.wait_for(lk, std::chrono::seconds(30), [&] {
            return pending_[id].has_value();
        });
        json out = got
            ? *pending_[id]
            : json{{"ok", false},
                   {"error", "routine request timed out (hub offline?)"}};
        pending_.erase(id);
        return out;
    }

    void resolve(const std::string& id, const json& frame) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            it->second = frame;
            cv_.notify_all();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::optional<json>> pending_;
    int seq_ = 0;
};
RoutineBridge g_routines;

// Message bridge (Phase 66d): request/response to the hub for confirmed
// agent messaging. message_agent emits {"ev":"msg_send_req",...} and the
// hub replies {"op":"msg_send_resp","id":...,"ok","delivery_id",...};
// message_status emits {"ev":"msg_status_req",...} and gets
// {"op":"msg_status_resp","id":...,"deliveries":[...]}. Same blocking
// pattern as RoutineBridge, 30s timeout.
class MsgBridge {
public:
    json call(const std::string& ev_name, const json& payload) {
        const std::string id = "m" + std::to_string(++seq_);
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_[id] = std::nullopt;
        }
        json ev = {{"ev", ev_name}, {"id", id}};
        for (auto& [k, v] : payload.items()) ev[k] = v;
        emit(ev);
        std::unique_lock<std::mutex> lk(m_);
        const bool got = cv_.wait_for(lk, std::chrono::seconds(30), [&] {
            return pending_[id].has_value();
        });
        json out = got
            ? *pending_[id]
            : json{{"ok", false},
                   {"error", "message request timed out (hub offline?)"}};
        pending_.erase(id);
        return out;
    }

    void resolve(const std::string& id, const json& frame) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            it->second = frame;
            cv_.notify_all();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::optional<json>> pending_;
    int seq_ = 0;
};
MsgBridge g_msgbridge;

// Share bridge (2026-10-11, Plan.md "Agent File Sharing"):
// request/response to the hub for agent-to-agent file shares. Tools
// emit {"ev":"share_*_req",...}; the worker forwards them as op frames
// (share_create_req additionally stages the blob worker-side first) and
// the hub replies {"op":"share_*_resp","id":...,"ok":bool,...} on stdin.
// Same blocking pattern as MsgBridge, but a 120s timeout: a cross-rig
// fetch relays up to 50 MB through the hub as chunked file ops.
class ShareBridge {
public:
    json call(const std::string& ev_name, const json& payload) {
        const std::string id = "f" + std::to_string(++seq_);
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_[id] = std::nullopt;
        }
        json ev = {{"ev", ev_name}, {"id", id}};
        for (auto& [k, v] : payload.items()) ev[k] = v;
        emit(ev);
        std::unique_lock<std::mutex> lk(m_);
        const bool got = cv_.wait_for(lk, std::chrono::seconds(120), [&] {
            return pending_[id].has_value();
        });
        json out = got
            ? *pending_[id]
            : json{{"ok", false},
                   {"error", "share request timed out (hub offline?)"}};
        pending_.erase(id);
        return out;
    }

    void resolve(const std::string& id, const json& frame) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            it->second = frame;
            cv_.notify_all();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::optional<json>> pending_;
    int seq_ = 0;
};
ShareBridge g_shares;

// Secret bridge: request/response to the hub for secrets_create. Tools
// emit {"ev":"secret_req",...}; the worker forwards it as an op frame;
// the hub applies it against SecretStore and replies with
// {"op":"secret_resp","id":...,"ok":bool,"result"|"error"} on stdin.
// Same blocking pattern as RoutineBridge, 30s timeout.
class SecretBridge {
public:
    json call(const std::string& action, const json& payload) {
        const std::string id = "s" + std::to_string(++seq_);
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_[id] = std::nullopt;
        }
        json ev = {{"ev", "secret_req"}, {"id", id}, {"action", action}};
        for (auto& [k, v] : payload.items()) ev[k] = v;
        emit(ev);
        std::unique_lock<std::mutex> lk(m_);
        const bool got = cv_.wait_for(lk, std::chrono::seconds(30), [&] {
            return pending_[id].has_value();
        });
        json out = got
            ? *pending_[id]
            : json{{"ok", false},
                   {"error", "secret request timed out (hub offline?)"}};
        pending_.erase(id);
        return out;
    }

    void resolve(const std::string& id, const json& frame) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            it->second = frame;
            cv_.notify_all();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::optional<json>> pending_;
    int seq_ = 0;
};
SecretBridge g_secrets;

// NDJSON sink: hive-brain events -> protocol events; collects the turn's
// user-facing output (assistant prose + untruncated tell_user payloads,
// same semantics as the v1 hive-server sink).
class NdjsonSink : public hive::AgentSink {
public:
    void begin_task(const std::string& id) {
        std::lock_guard<std::mutex> lk(m_);
        task_id_ = id;
        pending_.clear();
        committed_.clear();
        stop_summary_.clear();
        last_intermediate_.clear();
        errored_ = false;
    }

    // The posted reply: deliberate tell_user messages (in call order)
    // plus the FINAL assistant message's prose. Fallbacks, in order:
    // stop.summary (answer put there instead of in text), then the last
    // intermediate prose — a turn must never post an empty body, but a
    // closing summary is never APPENDED to an already-complete reply
    // (2026-10-22 reply-pipeline fix: kills the "... Closing." tail).
    std::string user_output() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string out = committed_;
        append_paragraph(out, pending_);
        if (out.empty()) out = stop_summary_;
        if (out.empty()) out = last_intermediate_;
        return out;
    }
    bool errored() const { return errored_.load(); }

    void on_assistant_delta(std::string_view text) noexcept override {
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_ += text;   // current assistant message only
        }
        emit({{"ev", "delta"}, {"task", current_task()}, {"text", std::string(text)}});
    }

    // Iteration boundary (2026-10-22 reply-pipeline fix): a tool call
    // means the assistant message that preceded it was INTERMEDIATE
    // (working out loud), not the reply — stash it as a fallback and
    // start a fresh pending buffer. Before this, every iteration's
    // prose was concatenated into the posted reply ("works.The route
    // test is fully closed" doubling). "stop" is the exception: the
    // loop ends there, so prose immediately before stop IS the reply.
    void on_tool_call(std::string_view name,
                      const nlohmann::json& args) noexcept override {
        if (name == "stop") return;
        // 2026-10-22: set_status({"state":"Done"}) is turn-terminal (see
        // LLMClient) — prose immediately before it IS the reply, same as
        // before stop; do not stash it as intermediate.
        if (name == "set_status") {
            const std::string st = args.value("state", "");
            if (st == "Done" || st == "done") return;
        }
        std::lock_guard<std::mutex> lk(m_);
        if (!pending_.empty()) {
            last_intermediate_ = pending_;
            pending_.clear();
        }
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
                append_paragraph(committed_, content);
            }
        }
        // 2026-10-22: stop.summary is now a RESCUE path, never an
        // append. Phase 52 concatenated it onto every reply, which
        // forced a posted closing summary each turn ("... Closing.").
        if (name == "stop") {
            const std::string summary = result.value("summary", "");
            if (!summary.empty()) {
                std::lock_guard<std::mutex> lk(m_);
                stop_summary_ = summary;
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

private:
    std::string current_task() const {
        std::lock_guard<std::mutex> lk(m_);
        return task_id_;
    }

    // Joins blocks with a blank line (no leading blank on the first).
    static void append_paragraph(std::string& dst, const std::string& src) {
        if (src.empty()) return;
        if (!dst.empty()) {
            if (dst.back() != '\n') dst += '\n';
            dst += '\n';
        }
        dst += src;
    }

    mutable std::mutex m_;
    std::string task_id_;
    std::string pending_;            // current assistant message's prose
    std::string committed_;          // tell_user messages, call order
    std::string stop_summary_;       // rescue only — see user_output()
    std::string last_intermediate_;  // last stashed pre-tool prose
    std::atomic<bool> errored_{false};
};

} // namespace

// Phase 35: current turn's thread id — set by the turn thread before
// each turn, read by AttachFileTool. Single turn thread → no locking.
static std::string g_thread_id;
// 2026-10-15 (Plan.md): current turn's task id — AttachFileTool emits
// an immediate attachment event keyed to the running task so the hub
// can post the file mid-turn (long renders hit the watchdog; attachments
// that only ride task_result die with the interrupt).
static std::string g_task_id;
// Runtime descriptor (2026-10-12): the SAME binary serves containers and
// bare-metal host agents; the worker's env says which. g_workspace_root
// is the absolute project root (argv[1]) — tools must build paths from
// it, never from a hard-coded /workspace literal.
static fs::path g_workspace_root;
static std::string g_runtime = "container";   // HIVEMIND_RUNTIME

// Phase 40: company roster, cached from each task frame (hub injects it
// in HubRouter::dispatch). The list_agents tool reads it so agents can
// discover one another before message_agent — without it an agent told
// to "message Forge" looped on echo-notes begging for a directory tool
// (2026-09-07 incident).
static json g_roster = json::array();
static std::mutex g_roster_mu;

// User Inbox snapshot, cached from each task frame (hub injects recent
// messages). read_user_inbox answers from it — the inbox was write-only
// for agents before this; they could message_user but never read back.
static json g_user_inbox = json::array();
static std::mutex g_inbox_mu;

int main(int argc, char** argv) {
    // Phase 72: --env-file <path> — KEY=VALUE lines setenv()'d before any
    // config is read. The host-agent spawn path uses this so secrets never
    // appear in the process argv (visible to every user via ps). Container
    // spawns keep using podman's --env-file; both paths end with the same
    // process environment. Positional argv[1] stays the project root, so
    // this option is scanned first and skipped over.
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--env-file" && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            std::string line;
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                ::setenv(line.substr(0, eq).c_str(),
                         line.substr(eq + 1).c_str(), 1);
            }
            continue;
        }
        positional.push_back(a);
    }
    // -- config ----------------------------------------------------------
    const fs::path project_root =
        fs::absolute(!positional.empty() ? fs::path(positional.front())
                                         : fs::current_path());
    // Phase 72: run WITH the project root as cwd — the container spawn
    // path gets this from podman's -w /workspace; the host-agent spawn
    // can't use sudo -D (sudo-rs rejects --chdir), so the binary chdirs
    // itself. A no-op for containers (cwd is already the same dir).
    {
        std::error_code ec;
        fs::current_path(project_root, ec);
        if (ec) log_err("warning: chdir to project root failed: " +
                        ec.message());
    }
    const std::string name = env_or("HIVEMIND_AGENT_NAME");
    if (name.empty()) {
        log_err("HIVEMIND_AGENT_NAME is required");
        return 2;
    }

    g_workspace_root = project_root;
    if (auto rt = env_or("HIVEMIND_RUNTIME"); !rt.empty()) g_runtime = rt;

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


    cfg.stream = true;
    cfg.max_agent_iterations = 600;

    // Skills (§Skills-Global, 2026-09-30): two tiers.
    //   1. HIVEMIND_SKILLS_DIR (=/skills)   - read-only synced library,
    //     agents cannot modify it (baked skills stay as-is).
    //   2. <project>/.agents/skills/        - THE writable global skills
    //     dir, shared by every agent on this worker via the shared
    //     /workspace mount. Listed LAST so a workspace skill shadows a
    //     same-named library skill. Created here best-effort (the worker
    //     also seeds it) and lazy-loaded at runtime: discovery re-scans,
    //     so skills agents create or edit are picked up without a restart.
    // ~/.agents/skills (container home) stays on the default scan list as
    // a PRIVATE per-agent tier — fine for personal skills, but it dies
    // with the container, so anything shared must go to the workspace dir.
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
        const fs::path global_dir = project_root / ".agents" / "skills";
        std::error_code ec;
        fs::create_directories(global_dir, ec);  // best-effort
        dirs.push_back(global_dir);
        // Phase 71: image-baked built-in skills (mcp-self-service etc),
        // COPY'd into the image by Containerfile.hive-agent. Optional —
        // absent on bare-metal dev runs, present in containers.
        const fs::path builtin_dir =
            env_or("HIVEMIND_BUILTIN_SKILLS_DIR").empty()
                ? fs::path{"/usr/local/share/hive-agent/skills"}
                : fs::path{env_or("HIVEMIND_BUILTIN_SKILLS_DIR")};
        if (fs::is_directory(builtin_dir, ec))
            dirs.push_back(builtin_dir);
        cfg.skills_dirs = dirs;
    }
    // Phase 50: per-skill gate from the hub (comma-joined names). Applied
    // at initialize() via cfg.skills_disabled; task frames refresh it live.
    {
        std::string dis = env_or("HIVEMIND_SKILLS_DISABLED");
        size_t pos = 0;
        while (!dis.empty()) {
            const size_t comma = dis.find(',', pos);
            std::string name = dis.substr(
                pos, comma == std::string::npos ? comma : comma - pos);
            if (!name.empty()) cfg.skills_disabled.insert(name);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
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
        "deliverable), call attach_file with its path under your workspace "
        "root (" + g_workspace_root.string() + "). The file posts to the chat "
        "immediately as its own message: images and video render inline, "
        "other files appear as a download. Prefer this over only mentioning "
        "the file path.";

    // 2026-10-12: host agents mis-modelled themselves as containers and
    // prescribed container remedies ("recreate my container") for
    // host-side gaps (QueenBee audit). State the runtime explicitly.
    if (g_runtime == "host") {
        cfg.system_prompt +=
            "\n\n## Your runtime\n"
            "You run BARE-METAL on this machine as the unix user for your "
            "agent (hivex-<name>) - there is NO container. Your workspace "
            "root is " + g_workspace_root.string() + " (a real host "
            "directory, not a container mount). Container remedies do not "
            "apply to you: there is no container to recreate, no container "
            "image variant to switch to, and your environment is not "
            "rebuilt from an image. If a tool or capability is missing, "
            "report it as a host-side gap instead of prescribing a "
            "container fix.";
    }

    // 2026-09-16 routing-noise report: routing is thread-scoped and
    // correct host-side, but agents NARRATED other threads' activity into
    // pair-thread replies (the peer read it as misrouted traffic) and
    // traded courtesy acks that each woke the peer again (8 acks in 5
    // min in one pair thread). Teach the etiquette host-side.
    cfg.system_prompt +=
        "\n\n## Thread etiquette\n"
        "Each message you receive names its sender as [Name]; your reply "
        "posts back into that ONE thread and is visible only to its "
        "participants. Rules:\n"
        "- Never narrate or summarize activity from other threads - the "
        "peer cannot see those threads and experiences the narration as "
        "misrouted traffic. If another thread's outcome matters to this "
        "peer, state only the decision or deliverable, not the play-by-"
        "play.\n"
        "- Address the peer directly (you/them), not in third person.\n"
        "- If a message is addressed to you, close visibly: reply with "
        "one short line ('Noted - closing the item', 'Confirmed, "
        "thanks'). Brief acknowledgments POST to the thread but are "
        "never fanned back to the peer (the hub guarantees this), so "
        "they are free - and they stop the peer from wondering whether "
        "you received the message at all.\n"
        "- Silence is for undirected traffic only: pure FYI/heartbeat "
        "messages not addressed to you warrant NO output. Empty turns "
        "are never posted and do not wake the peer.";

    // MCP specs arrive as JSON; seed before Agent construction.
    if (auto mcp = env_or("HIVEMIND_MCP_SERVERS"); !mcp.empty()) {
        try { hive::mcp_seed::seed_hub_mcp_servers(cfg.agent_home, json::parse(mcp), log_err); }
        catch (const std::exception& e) {
            log_err(std::string("mcp seed parse: ") + e.what());
        }
    }

    // -- agent + sink ----------------------------------------------------
    auto sink = std::make_shared<NdjsonSink>();

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
            // Phase 66d: CONFIRMED send — the hub durably queues the
            // delivery and replies with its id. Track the message with
            // message_status (queued / dispatched / done / read /
            // error) instead of guessing.
            const json resp = g_msgbridge.call("msg_send_req",
                {{"target", target}, {"text", text}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error", resp.value("error",
                                             "send failed (hub offline?)")}};
            json out = {{"status","success"},
                        {"note","message durably queued; the reply arrives "
                                "asynchronously as a new task. Use "
                                "message_status with the delivery_id to "
                                "check progress."}};
            if (resp.contains("delivery_id"))
                out["delivery_id"] = resp["delivery_id"];
            if (resp.value("redirected_to_user_inbox", false))
                out["note"] = "target resolved to the USER — message was "
                              "posted to the user's inbox instead";
            return out;
        }
    };
    agent.register_tool(std::make_unique<MessageAgentTool>());

    // message_status (Phase 66d): the agent's visibility into the
    // durable delivery queue — every message it sent (or a specific
    // one) with its live state and read receipt.
    class MessageStatusTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "message_status"; }
        std::string get_description() const override {
            return "Check the delivery status of messages you sent with "
                   "message_agent. With no arguments, lists your 10 most "
                   "recent sends with their state and read receipts. "
                   "States: queued (waiting — the other agent is busy or "
                   "its worker is offline), dispatched (delivered to the "
                   "agent, turn running), done (the agent's turn "
                   "completed), read=true (the agent has seen your "
                   "message), error (failed — see the error field), "
                   "dropped (blocked by a mute/circuit breaker/cancel — "
                   "see error), expired (waited too long). Pass "
                   "delivery_id (from message_agent's result) to check "
                   "one specific message.";
        }
        json get_definition() const override {
            json did = {{"type","string"},
                        {"description","Delivery id from a message_agent "
                                       "result (optional)"}};
            json params = {{"type","object"},
                           {"properties",{{"delivery_id",did}}}};
            return {{"type","function"},
                    {"function",{{"name","message_status"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            json payload = json::object();
            const std::string did = args.value("delivery_id","");
            if (!did.empty()) payload["delivery_id"] = did;
            const json resp = g_msgbridge.call("msg_status_req", payload);
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error", resp.value("error",
                                             "status query failed")}};
            return {{"status","success"},
                    {"deliveries",
                     resp.value("deliveries", json::array())}};
        }
    };
    agent.register_tool(std::make_unique<MessageStatusTool>());

    // message_mark (§MsgStatus 2026-10-22): the task-lifecycle marker —
    // claimed / done / verified on a THREAD message, broker-side so
    // every session of every agent sees the same state (QueenBee
    // parallel-session lesson). Stops two classes of repeated work:
    // redelivered/late task messages arrive already marked, and a
    // second agent sees the claim BEFORE starting. Emits
    // {"ev":"msg_mark_req",...} over the MsgBridge; hub applies the
    // transition rules (first-claim-wins; verify needs done + an
    // INDEPENDENT verifier — never your own done).
    class MessageMarkTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "message_mark"; }
        std::string get_description() const override {
            return "Mark the lifecycle state of a task message in a "
                   "thread so other agents can see it. state: 'claimed' "
                   "(you are taking this — check first, first-claim-wins, "
                   "a 409 tells you who holds it), 'done' (you finished "
                   "it), 'verified' (you INDEPENDENTLY confirmed someone "
                   "else's done — never verify your own), or 'clear'. "
                   "Pass thread_id + message_id (from the [msg <id>] tag "
                   "on wakes and thread reads) and an optional short "
                   "note ('verified: port 8090 answers 200').";
        }
        json get_definition() const override {
            json state = {{"type","string"},
                          {"enum",{"claimed","done","verified","clear"}},
                          {"description","Lifecycle state to set"}};
            json tid = {{"type","string"},
                        {"description","Thread id"}};
            json mid = {{"type","string"},
                        {"description","Message id to mark"}};
            json note = {{"type","string"},
                         {"description","Optional short note (<=500 bytes)"}};
            json params = {{"type","object"},
                           {"properties",{{"state",state},
                                          {"thread_id",tid},
                                          {"message_id",mid},
                                          {"note",note}}},
                           {"required",{"state","thread_id","message_id"}}};
            return {{"type","function"},
                    {"function",{{"name","message_mark"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string state = args.value("state","");
            const std::string tid   = args.value("thread_id","");
            const std::string mid   = args.value("message_id","");
            const std::string note  = args.value("note","");
            if (state.empty() || tid.empty() || mid.empty())
                return {{"status","error"},
                        {"error","state, thread_id and message_id required"}};
            const json resp = g_msgbridge.call("msg_mark_req",
                {{"state", state}, {"thread_id", tid},
                 {"message_id", mid}, {"note", note}});
            if (!resp.value("ok", false)) {
                json out = {{"status","error"},
                            {"error", resp.value("error", "mark failed")}};
                // 409 conflicts carry the CURRENT holder — surface it so
                // the agent can act on "already claimed by X" directly.
                if (resp.contains("status")) out["current"] = resp["status"];
                return out;
            }
            return {{"status","success"},
                    {"marked", resp.value("status", json(nullptr))}};
        }
    };
    agent.register_tool(std::make_unique<MessageMarkTool>());

    // ── Agent file sharing (2026-10-11, Plan.md) ─────────────────────
    // Four tools over the ShareBridge: share_file stages + registers +
    // notifies; list_shared_files is the discovery surface;
    // fetch_shared_file pulls the blob (same-rig = instant local copy,
    // cross-rig = hub relay); revoke_shared_file cancels a sent share.

    class ShareFileTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "share_file"; }
        std::string get_description() const override {
            return "Share a file with another agent. path: a file under "
                   "your workspace root (" + g_workspace_root.string() +
                   "), max 50 MB. target: the other agent's "
                   "name or id. The file is staged and the target is "
                   "notified automatically with the share_id; they download "
                   "it with fetch_shared_file. Returns the share_id. "
                   "Optional note: one line about what the file is. "
                   "Only the two of you can ever fetch it — shares are "
                   "addressed, not public.";
        }
        json get_definition() const override {
            json path = {{"type","string"},
                         {"description","Absolute path (under your "
                                        "workspace root) of the file to "
                                        "share"}};
            json target = {{"type","string"},
                           {"description","Recipient agent name or id"}};
            json note = {{"type","string"},
                         {"description","Optional one-line description "
                                        "for the recipient"}};
            json params = {{"type","object"},
                           {"properties",{{"path",path},{"target",target},
                                          {"note",note}}},
                           {"required",{"path","target"}}};
            return {{"type","function"},
                    {"function",{{"name","share_file"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string path   = args.value("path","");
            const std::string target = args.value("target","");
            if (path.empty() || target.empty())
                return {{"status","error"},
                        {"error","path and target required"}};
            const json resp = g_shares.call("share_create_req",
                {{"path", path}, {"target", target},
                 {"note", args.value("note","")}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error", resp.value("error",
                                             "share failed (hub offline?)")}};
            json out = {{"status","success"},
                        {"share_id", resp.value("share_id","")},
                        {"note","staged and registered; the target was "
                                "notified and can fetch_shared_file(\"" +
                                resp.value("share_id","") + "\") now."}};
            if (resp.contains("notify_error"))
                out["notify_error"] = resp["notify_error"];
            return out;
        }
    };
    agent.register_tool(std::make_unique<ShareFileTool>());

    class ListSharedFilesTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "list_shared_files"; }
        std::string get_description() const override {
            return "List file shares involving you (newest first): files "
                   "others shared WITH you and files YOU shared. Each row: "
                   "share_id, filename, size_bytes, peer_name, direction "
                   "(received/sent), state (active/revoked), created_at, "
                   "fetched_at, fetch_count. Use fetch_shared_file with a "
                   "received share_id to download it.";
        }
        json get_definition() const override {
            json params = {{"type","object"},
                           {"properties",json::object()}};
            return {{"type","function"},
                    {"function",{{"name","list_shared_files"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            (void)args;
            const json resp = g_shares.call("share_list_req",
                                            json::object());
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error", resp.value("error", "list failed")}};
            return {{"status","success"},
                    {"shares", resp.value("shares", json::array())}};
        }
    };
    agent.register_tool(std::make_unique<ListSharedFilesTool>());

    class FetchSharedFileTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "fetch_shared_file"; }
        std::string get_description() const override {
            return "Download a file another agent shared with you (or "
                   "re-fetch one you sent). share_id comes from a share "
                   "notification or list_shared_files. The file lands at "
                   "the returned /workspace/incoming/<share_id>/ path. A "
                   "large file coming from another rig may take a while — "
                   "that is normal, not an error. Revoked or wrongly "
                   "addressed shares are refused.";
        }
        json get_definition() const override {
            json sid = {{"type","string"},
                        {"description","share_id from list_shared_files "
                                       "or the share notification"}};
            json params = {{"type","object"},
                           {"properties",{{"share_id",sid}}},
                           {"required",{"share_id"}}};
            return {{"type","function"},
                    {"function",{{"name","fetch_shared_file"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string sid = args.value("share_id","");
            if (sid.empty())
                return {{"status","error"},{"error","share_id required"}};
            const json resp = g_shares.call("share_fetch_req",
                                            {{"share_id", sid}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error", resp.value("error", "fetch failed")}};
            return {{"status","success"},
                    {"path", resp.value("path","")},
                    {"name", resp.value("name","")},
                    {"size", resp.value("size", 0)}};
        }
    };
    agent.register_tool(std::make_unique<FetchSharedFileTool>());

    class RevokeSharedFileTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "revoke_shared_file"; }
        std::string get_description() const override {
            return "Revoke a file share YOU created (by share_id, from "
                   "list_shared_files). The staged copy is deleted and "
                   "fetch_shared_file stops working for it. Only the "
                   "sender can revoke; already-fetched copies on the "
                   "recipient's side are NOT removed.";
        }
        json get_definition() const override {
            json sid = {{"type","string"},
                        {"description","share_id of a share you sent"}};
            json params = {{"type","object"},
                           {"properties",{{"share_id",sid}}},
                           {"required",{"share_id"}}};
            return {{"type","function"},
                    {"function",{{"name","revoke_shared_file"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string sid = args.value("share_id","");
            if (sid.empty())
                return {{"status","error"},{"error","share_id required"}};
            const json resp = g_shares.call("share_revoke_req",
                                            {{"share_id", sid}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error", resp.value("error", "revoke failed")}};
            return {{"status","success"},
                    {"note","share revoked; the staged copy is being "
                            "deleted."}};
        }
    };
    agent.register_tool(std::make_unique<RevokeSharedFileTool>());

    // message_user (Phase 44): the agent's direct line to the HUMAN.
    // Posts into the hub's singleton User Inbox thread — a one-way,
    // loop-safe notification channel with zero agent participants, so
    // nothing sent here can wake another agent. Before this tool existed,
    // agents trying to address the user improvised into group threads,
    // fanned in to every participant and looped endlessly (2026-09-09).
    class MessageUserTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "message_user"; }
        std::string get_description() const override {
            return "Send a direct message to the human user. This is the "
                   "ONLY correct way to address the user outside your own "
                   "thread reply. Use it for: status updates the user should "
                   "see, questions only the user can answer, and finished "
                   "deliverables you want to hand over. The message lands in "
                   "the user's dedicated inbox thread and is NOT delivered "
                   "to any other agent, so it can never start an agent "
                   "loop. NEVER post user-directed messages into group "
                   "threads, and do NOT use message_agent for the user - "
                   "message_agent is strictly agent-to-agent. The user "
                   "reads the inbox when they are around; do not wait for "
                   "a reply, continue your own work.";
        }
        json get_definition() const override {
            json text   = {{"type","string"},
                           {"description","Message body for the human user"}};
            json params = {{"type","object"},
                           {"properties",{{"text",text}}},
                           {"required",{"text"}}};
            return {{"type","function"},
                    {"function",{{"name","message_user"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string text = args.value("text","");
            if (text.empty())
                return {{"status","error"},{"error","text required"}};
            emit({{"ev","send_user_message"},{"text",text}});
            return {{"status","success"},
                    {"note","posted to the user's inbox; do not wait for a "
                            "reply - the user reads it asynchronously"}};
        }
    };
    agent.register_tool(std::make_unique<MessageUserTool>());

    // list_agents (Phase 40): company directory from the roster the hub
    // injects into every task frame.
    class ListAgentsTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "list_agents"; }
        std::string get_description() const override {
            return "List the agents in the company: name, id, and the "
                   "machine each runs on (worker field, e.g. droplet, "
                   "ubuntu-pc). Use this to discover who exists before "
                   "calling message_agent, and to tell which rig an "
                   "agent lives on when coordinating cross-machine work.";
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

    // read_user_inbox: the inbox is the agent's direct line to the human
    // (message_user) — and now readable, so an agent can check what the
    // user has actually been sent (its own notes included) before writing
    // more. Answers from the hub's task-frame snapshot, so it is fresh at
    // task start; anything newer arrives as a new task by design.
    class ReadUserInboxTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "read_user_inbox"; }
        std::string get_description() const override {
            return "Read recent messages in the human user's inbox - notes "
                   "agents (including you via message_user) have delivered "
                   "to the user, with sender name and timestamp. Use it "
                   "before writing to the user to see what they have "
                   "already been told, or to confirm your own message_user "
                   "note landed. Snapshot from the start of this task.";
        }
        json get_definition() const override {
            return {{"type","function"},
                    {"function",{{"name","read_user_inbox"},
                                 {"description", get_description()},
                                 {"parameters",{{"type","object"},
                                                {"properties",json::object()}}}}}};
        }
        json execute(const json&, hive::HostToolContext&) override {
            std::lock_guard<std::mutex> lk(g_inbox_mu);
            return {{"status","success"}, {"messages", g_user_inbox}};
        }
    };
    agent.register_tool(std::make_unique<ReadUserInboxTool>());

    // attach_file (Phase 35): agent → chat attachments. Copies a
    // workspace file into the thread's outgoing dir; the worker ships it
    // with task_result and clients render it inline (images/video).
    class AttachFileTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "attach_file"; }
        std::string get_description() const override {
            return "Attach a file from your workspace (" +
                   g_workspace_root.string() + ") to the chat so the user "
                   "sees it immediately (images and video render inline; "
                   "other files appear as a download). Use it for "
                   "deliverables (rendered videos, images, archives) "
                   "instead of only mentioning the file path. The file "
                   "posts as its own message the moment you attach it — "
                   "you do not need to wait for your turn to end.";
        }
        json get_definition() const override {
            json path = {{"type","string"},
                         {"description","File to attach: absolute path "
                          "under your workspace root, or "
                          "workspace-relative"}};
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
            // Accept an absolute path under the workspace root, the
            // legacy container-style /workspace/... spelling, or a
            // workspace-relative path.
            const std::string root_str = g_workspace_root.string();
            if (p.rfind(root_str + "/", 0) == 0)
                p = p.substr(root_str.size() + 1);
            else if (p.rfind("/workspace/", 0) == 0)
                p = p.substr(11);
            std::error_code ec;
            const fs::path rel = fs::path(p).lexically_normal();
            bool traversal = rel.empty() || rel.is_absolute();
            for (const auto& c : rel) if (c == "..") traversal = true;
            if (traversal)
                return {{"status","error"},
                        {"error","path must stay under the workspace root"}};
            const fs::path src = g_workspace_root / rel;
            if (!fs::is_regular_file(src, ec))
                return {{"status","error"},
                        {"error","not a file: " + root_str + "/" +
                         rel.generic_string()}};
            const fs::path outdir = g_workspace_root / "attachments" /
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
            // 2026-10-15 (Plan.md): ship the file to the chat NOW, as
            // its own message. Attachments that wait for task_result
            // are lost when a long turn is interrupted (watchdog /
            // cancel) — the outgoing/ copy stays put so an old worker
            // still promotes it at turn end (mixed-version fallback).
            emit({{"ev", "attachment"},
                  {"task", g_task_id},
                  {"thread_id", g_thread_id},
                  {"name", fname}});
            return {{"status","success"},
                    {"attached", fname},
                    {"note","sent to the chat immediately as its own "
                     "message; your final reply posts separately when "
                     "this turn ends"}};
        }
    };
    agent.register_tool(std::make_unique<AttachFileTool>());

    // ── Phase 53: scheduled tasks (routines) — hub-managed via the
    // routine_req/routine_resp bridge. Tools mirror the hub's Scheduled
    // page semantics (each routine fires a prompt into its own thread).
    static const char* kScheduleShapes =
        "schedule is a JSON object, one of: "
        R"json({"type":"once","at":"YYYY-MM-DDTHH:MM"}, )json"
        R"json({"type":"interval","every_minutes":N}, )json"
        R"json({"type":"daily","time":"HH:MM"}, )json"
        R"json({"type":"weekly","time":"HH:MM","days":[0-6]} )json"
        "(0=Sunday). Times are UTC (the hub scheduler fires on UTC clock; "
        "e.g. daily 01:00 = 11:00 AEST summer / 10:00 AEST standard — "
        "convert from the target local time yourself).";

    class ScheduleTaskTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "schedule_task"; }
        std::string get_description() const {
            return std::string("Create a scheduled task (routine): a prompt "
                "that fires automatically on a schedule, in its own thread. ") +
                kScheduleShapes;
        }
        json get_definition() const override {
            json params = {{"type","object"},
                {"properties",{
                    {"name",{{"type","string"},
                      {"description","Short routine name"}}},
                    {"prompt",{{"type","string"},
                      {"description","The prompt that runs on each fire"}}},
                    {"schedule",{{"type","object"},
                      {"description","Schedule JSON (see shapes in the tool description)"}}}}},
                {"required",{"name","prompt","schedule"}}};
            return {{"type","function"},
                    {"function",{{"name",get_name()},
                                 {"description",get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            json resp = g_routines.call("add", {
                {"name", args.value("name", "")},
                {"prompt", args.value("prompt", "")},
                {"schedule", args.value("schedule", json::object())}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error",resp.value("error","hub rejected the request")}};
            return resp.value("result", json::object());
        }
    };
    agent.register_tool(std::make_unique<ScheduleTaskTool>());

    class ListSchedulesTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "list_schedules"; }
        std::string get_description() const {
            return "List your scheduled tasks (routines): id, name, "
                   "schedule, next fire time, enabled, prompt size. "
                   "Compact by design — use get_schedule for a routine's "
                   "full prompt body. update_schedule/delete_schedule "
                   "accept the id OR the exact name as their target.";
        }
        json get_definition() const override {
            json params = {{"type","object"},
                           {"properties",json::object()}};
            return {{"type","function"},
                    {"function",{{"name",get_name()},
                                 {"description",get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json&, hive::HostToolContext&) override {
            json resp = g_routines.call("list", json::object());
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error",resp.value("error","hub rejected the request")}};
            return resp.value("result", json::object());
        }
    };
    agent.register_tool(std::make_unique<ListSchedulesTool>());

    // 2026-10-18 (Plan.md): full-prompt read. list_schedules is compact
    // (no prompt body) so it can never truncate past the tool-result
    // cap; editing a prompt iteratively starts here.
    class GetScheduleTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "get_schedule"; }
        std::string get_description() const {
            return "Get one scheduled task (routine) in full, including "
                   "its complete prompt body. Target by id OR exact name.";
        }
        json get_definition() const override {
            json params = {{"type","object"},
                {"properties",{
                    {"id",{{"type","string"},
                      {"description","Routine id or exact name"}}}}},
                {"required",{"id"}}};
            return {{"type","function"},
                    {"function",{{"name",get_name()},
                                 {"description",get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            json resp = g_routines.call("get",
                                        {{"routine_id", args.value("id","")}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error",resp.value("error","hub rejected the request")}};
            return resp.value("result", json::object());
        }
    };
    agent.register_tool(std::make_unique<GetScheduleTool>());

    class UpdateScheduleTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "update_schedule"; }
        std::string get_description() const {
            return std::string("Edit a scheduled task (routine) you own: "
                "rename, change the prompt or schedule, or enable/disable "
                "it. Only provided fields change. The prompt can be edited "
                "as a full replace (prompt=) or surgically "
                "(prompt_append= / prompt_prepend= add text to the existing "
                "prompt without resubmitting it — use these for long "
                "prompts; do not combine with prompt=). ") + kScheduleShapes;
        }
        json get_definition() const override {
            json params = {{"type","object"},
                {"properties",{
                    {"id",{{"type","string"},
                      {"description","Routine id OR exact name (from "
                       "list_schedules, or the routine's fire header)"}}},
                    {"name",{{"type","string"},{"description","New name (optional)"}}},
                    {"prompt",{{"type","string"},{"description","New prompt — FULL REPLACE (optional; do not combine with prompt_append/prepend)"}}},
                    {"prompt_append",{{"type","string"},{"description","Text to append to the existing prompt (optional)"}}},
                    {"prompt_prepend",{{"type","string"},{"description","Text to prepend to the existing prompt (optional)"}}},
                    {"schedule",{{"type","object"},{"description","New schedule JSON (optional)"}}},
                    {"enabled",{{"type","boolean"},{"description","Enable/disable (optional)"}}}}},
                {"required",{"id"}}};
            return {{"type","function"},
                    {"function",{{"name",get_name()},
                                 {"description",get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            json payload = {{"routine_id", args.value("id", "")}};
            const bool has_replace = args.contains("prompt");
            const bool has_append = args.contains("prompt_append");
            const bool has_prepend = args.contains("prompt_prepend");
            if (has_replace && (has_append || has_prepend))
                return {{"status","error"},
                        {"error","prompt= is a full replace and cannot be "
                                 "combined with prompt_append=/prompt_prepend="}};
            if (has_append || has_prepend) {
                // Surgical edit: read-modify-write so the agent never
                // resubmits (or silently loses) a long prompt body.
                json cur = g_routines.call("get",
                                    {{"routine_id", args.value("id","")}});
                if (!cur.value("ok", false))
                    return {{"status","error"},
                            {"error","could not read current routine: " +
                                cur.value("error","hub error")}};
                std::string body = cur["result"].value("prompt", "");
                if (has_prepend)
                    body = args.value("prompt_prepend", "") + body;
                if (has_append)
                    body += args.value("prompt_append", "");
                payload["prompt"] = std::move(body);
            }
            for (const char* k : {"name", "prompt", "schedule", "enabled"})
                if (args.contains(k)) payload[k] = args[k];
            json resp = g_routines.call("update", payload);
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error",resp.value("error","hub rejected the request")}};
            return resp.value("result", json::object());
        }
    };
    agent.register_tool(std::make_unique<UpdateScheduleTool>());

    class DeleteScheduleTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "delete_schedule"; }
        std::string get_description() const {
            return "Delete a scheduled task (routine) you own. Its thread "
                   "is removed too. Target by id OR exact name.";
        }
        json get_definition() const override {
            json params = {{"type","object"},
                {"properties",{
                    {"id",{{"type","string"},
                      {"description","Routine id or exact name"}}}}},
                {"required",{"id"}}};
            return {{"type","function"},
                    {"function",{{"name",get_name()},
                                 {"description",get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            json resp = g_routines.call("delete",
                                        {{"routine_id", args.value("id","")}});
            if (!resp.value("ok", false))
                return {{"status","error"},
                        {"error",resp.value("error","hub rejected the request")}};
            return resp.value("result", json::object());
        }
    };
    agent.register_tool(std::make_unique<DeleteScheduleTool>());

    // browser_* (Phase 47): computer use — recreation of the original
    // hub-side tools lost in the Phase C refactor. Backed by browser_driver.py
    // in-container (CDP :9222, the same Chromium the user sees in the
    // client's Computer view). On plain :latest images the driver is
    // absent and every action returns a descriptive error.
    for (auto& spec : hive::BrowserTool::specs())
        agent.register_tool(std::make_unique<hive::BrowserTool>(std::move(spec)));
    agent.register_tool(std::make_unique<hive::BrowserFillSecretTool>());

    // secrets_* (Phase 61): the hub brokers granted secrets into the
    // secrets_create: mint a NEW credential into the broker instead of
    // writing it to a plaintext file in the container. The value crosses
    // the LLM context once (the agent generated it — unavoidable), then
    // lives encrypted hub-side (AES-256-GCM) and returns to tools on
    // future spawns as HIVE_SECRET_<NAME> — never in context again.
    class SecretsCreateTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "secrets_create"; }
        std::string get_description() const override {
            return "Create a NEW secret in the hive broker, granted to "
                   "you. Use this to store credentials you generate — a "
                   "password or API key for a new account — instead of "
                   "writing them to files in your container. Types: "
                   "api_key (give 'value'), login (give 'password' plus "
                   "optional 'username'/'url'), note (give 'text'). Names "
                   "are unique hive-wide; you get an error if taken. The "
                   "stored secret is usable by secrets_get and "
                   "browser_fill_secret after your next restart (env "
                   "secrets load at spawn); this session you already have "
                   "the value in context.";
        }
        json get_definition() const override {
            json name = {{"type","string"},
                         {"description","Secret name [A-Za-z0-9_], <=64 "
                                        "chars, e.g. myservice_login"}};
            json type = {{"type","string"},
                         {"description","api_key | login | note"},
                         {"default","api_key"}};
            json value = {{"type","string"},
                          {"description","api_key material"}};
            json username = {{"type","string"},
                             {"description","login only: username"}};
            json password = {{"type","string"},
                             {"description","login only: password "
                                            "(required for login)"}};
            json url = {{"type","string"},
                        {"description","login only: site URL"}};
            json text = {{"type","string"},
                         {"description","note only: free text"}};
            json params = {
                {"type","object"},
                {"properties",{{"name",name},{"type",type},{"value",value},
                               {"username",username},{"password",password},
                               {"url",url},{"text",text}}},
                {"required",{"name"}}};
            return {{"type","function"},
                    {"function",{{"name","secrets_create"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string name = args.value("name","");
            const std::string type = args.value("type","api_key");
            if (name.empty())
                return {{"status","error"},{"error","name required"}};
            json payload = json::object();
            if (type == "login") {
                payload = {{"username", args.value("username","")},
                           {"password", args.value("password","")},
                           {"url",      args.value("url","")}};
            } else if (type == "note") {
                payload = {{"text", args.value("text","")}};
            } else {
                payload = {{"value", args.value("value","")}};
            }
            json r = g_secrets.call("create", {{"name",name},
                                               {"type",type},
                                               {"payload",payload}});
            if (!r.value("ok", false))
                return {{"status","error"},
                        {"error", r.value("error","create failed")}};
            return {{"status","success"},
                    {"note","secret '" + name + "' created in the broker "
                            "and granted to you. It will reach tools "
                            "after your next restart; you still have the "
                            "value in this session's context."}};
        }
    };
    agent.register_tool(std::make_unique<SecretsCreateTool>());

    // secrets_delete (2026-10-20, lee-approved; Bob report: the agent
    // surface was list/get/create only — stale entries needed lee's
    // manual webUI cleanup). A secret can be granted to MANY agents, so
    // delete is "remove my access"; only the LAST holder's call destroys
    // the entry. The result is blind — it never reveals other holders.
    class SecretsDeleteTool final : public hive::BaseTool {
    public:
        std::string get_name() const override { return "secrets_delete"; }
        std::string get_description() const override {
            return "Remove a broker secret you hold. If other agents "
                   "also hold it this removes only YOUR access (they "
                   "keep it); if you are the last holder the entry is "
                   "destroyed and cannot be recovered. Use for stale or "
                   "broken credentials you no longer need. Names as "
                   "shown by secrets_list (case-insensitive). The change "
                   "reaches your tools after your next restart (env "
                   "secrets load at spawn).";
        }
        json get_definition() const override {
            json name = {{"type","string"},
                         {"description","Secret name as shown by "
                                        "secrets_list (case-insensitive, "
                                        "e.g. GITHUB_TOKEN)"}};
            json params = {{"type","object"},
                           {"properties",{{"name",name}}},
                           {"required",{"name"}}};
            return {{"type","function"},
                    {"function",{{"name","secrets_delete"},
                                 {"description", get_description()},
                                 {"parameters",params}}}};
        }
        json execute(const json& args, hive::HostToolContext&) override {
            const std::string raw = args.value("name","");
            if (raw.empty())
                return {{"status","error"},{"error","name required"}};
            const std::string name = hive::secret_env_normalize(raw);
            if (!hive::secret_env_name_valid(name))
                return {{"status","error"},{"error","invalid secret name"}};
            json r = g_secrets.call("delete", {{"name",name}});
            if (!r.value("ok", false))
                return {{"status","error"},
                        {"error", r.value("error","delete failed")}};
            const json res = r.value("result", json::object());
            if (res.contains("deleted"))
                return {{"status","success"},
                        {"note","secret '" + name + "' destroyed — you "
                                "were the last holder."}};
            return {{"status","success"},
                    {"note","your access to '" + name + "' was removed; "
                            "the entry stays in the broker because other "
                            "agents still hold it."}};
        }
    };
    agent.register_tool(std::make_unique<SecretsDeleteTool>());

    // container environment at spawn (Phase 60); these tools let the
    // agent discover (secrets_list) and read (secrets_get) them.
    agent.register_tool(std::make_unique<hive::SecretsListTool>());
    agent.register_tool(std::make_unique<hive::SecretsGetTool>());

    // Service sharing (2026-10-09): register a container service on a
    // pre-published port and get its stable worker-host URL. See
    // tools/ServiceTools.h and hive-worker/src/runner.cpp.
    agent.register_tool(std::make_unique<hive::ShareServiceTool>());
    agent.register_tool(std::make_unique<hive::ListServicesTool>());
    agent.register_tool(std::make_unique<hive::UnshareServiceTool>());
    try {
        agent.initialize();
    } catch (const std::exception& e) {
        emit({{"ev", "error"}, {"text", std::string("init: ") + e.what()}});
        return 2;
    }
    auto& background_processes = BackgroundProcessManager::get_instance();
    background_processes.set_completion_callback(
        [](const BackgroundProcessInfo& info, const ProcessOutput& output) {
            const auto clean = [](const std::string& text, size_t max_bytes) {
                std::string out = utf8_sanitize_replace_invalid(text);
                if (out.size() > max_bytes) {
                    out = utf8_truncate_safe(out, max_bytes) +
                          "\n...[truncated]";
                }
                return out;
            };
            emit({{"ev", "background_done"},
                  {"process_id", info.process_id},
                  {"pid", static_cast<int>(info.pid)},
                  {"task", info.origin_task_id},
                  {"thread_id", info.origin_thread_id},
                  {"command", clean(info.command, 1000)},
                  {"exit_code", output.exit_code},
                  {"signaled", output.signaled},
                  {"signal", output.signal_num},
                  {"stdout", clean(output.stdout, 4000)},
                  {"stderr", clean(output.stderr, 4000)}});
        });

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
            g_task_id = t.id;
            background_processes.set_current_origin(t.id, t.thread_id);
            sink->begin_task(t.id);
            std::string err;
            try {
                agent.submit_user_message(t.text);
                agent.run_one_turn();
            } catch (const std::exception& e) {
                err = e.what();
            }
            std::string body = sink->user_output();
            // §2026-09-30 silent-mute fix: a turn ended by the LLM fatal
            // breaker (every API call rejected, e.g. a poisoned MCP tool
            // schema) returned DONE with err empty — the hub saw done:true
            // + error:"" and the mute was invisible. Surface the cause.
            if (err.empty())
                err = agent.fatal_breaker_error();
            if (body.empty() && err.empty() && !sink->errored())
                body = "(agent produced no output)";
            // Phase 64: per-turn stats ride the message frame so the hub
            // can store them on the reply and clients can render context
            // usage / prompt-cache / iteration info per message.
            const auto st = agent.stats();
            emit({{"ev", "message"},
                  {"task", t.id},
                  {"thread_id", t.thread_id},
                  {"body", body},
                  {"error", err},
                  {"stats",
                   {{"prompt_tokens", st.prompt_tokens},
                    {"completion_tokens", st.completion_tokens},
                    {"total_tokens", st.total_tokens},
                    {"cached_tokens", st.cached_tokens},
                    {"max_context_size", st.max_context_size},
                    {"iterations", st.iteration},
                    {"message_count", st.message_count}}}});
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
            if (op.contains("user_inbox") && op["user_inbox"].is_array()) {
                std::lock_guard<std::mutex> lk(g_inbox_mu);
                g_user_inbox = op["user_inbox"];
            }
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
                    if (agent.switch_model(model)) {
                        log_err("llm config applied: " + model + " via " + pc.name);
                        // 2026-10-01: hub-selected thinking level rides the
                        // Phase 43 frame ("" = provider default). Runtime
                        // toggle stores into cfg_, so model rebuilds keep it;
                        // plugins silently ignore levels the model rejects.
                        const auto level = hive::thinking_level_from_string(
                            L.value("thinking_level", ""));
                        agent.set_thinking_level(level);
                        if (level != hive::ThinkingLevel::Default)
                            log_err(std::string("thinking level: ")
                                    + hive::to_string(level));
                    } else {
                        log_err("llm config apply failed for model '" + model + "'");
                    }
                }
            }
            // Phase 50: live disabled-skills gate. The hub computes the
            // agent's disabled set (hub-known skills minus its enabled
            // global-skills) and rides it on every task frame, so skill
            // enable/disable takes effect without a process restart.
            if (op.contains("disabled_skills") && op["disabled_skills"].is_array()) {
                std::set<std::string> desired;
                for (const auto& n : op["disabled_skills"])
                    if (n.is_string()) desired.insert(n.get<std::string>());
                std::set<std::string> current;
                for (const auto& n : agent.disabled_skills()) current.insert(n);
                for (const auto& n : desired)
                    if (!current.count(n)) agent.set_skill_enabled(n, false);
                for (const auto& n : current)
                    if (!desired.count(n)) agent.set_skill_enabled(n, true);
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
        } else if (kind == "routine_resp") {
            g_routines.resolve(op.value("id", ""), op);
        } else if (kind == "secret_resp") {
            g_secrets.resolve(op.value("id", ""), op);
        } else if (kind == "msg_send_resp" || kind == "msg_status_resp" ||
                   kind == "msg_mark_resp") {
            g_msgbridge.resolve(op.value("id", ""), op);
        } else if (kind == "share_create_resp" || kind == "share_list_resp" ||
                   kind == "share_fetch_resp" || kind == "share_revoke_resp") {
            g_shares.resolve(op.value("id", ""), op);
        } else if (kind == "cancel") {
            agent.cancel();
        } else if (kind == "shutdown") {
            break;
        }
    }

    // stdin EOF (worker stopped the pipe) == shutdown.
    stopping = true;
    q_cv.notify_all();
    if (turn_thread.joinable()) turn_thread.join();
    background_processes.shutdown();
    try { agent.shutdown(); }           // persists history + memory
    catch (const std::exception& e) { log_err(std::string("shutdown: ") + e.what()); }
    return 0;
}
