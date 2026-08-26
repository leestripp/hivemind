// =============================================================================
// src/AgentHost.cpp — long-lived daedalus::Agent per roster agent + ChatService
// (HiveMind 2.0, Plan.md Phase 1)
// =============================================================================
#include "hivemind/AgentHost.h"
#include "hivemind/AgentSerialization.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/ContainerTools.h"
#include "hivemind/BrowserTools.h"
#include "hivemind/AgentTools.h"

#include "hivemind/LLMStore.h"
#include "daedalus/default_paths.h"

#include <algorithm>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace {

// ─── StreamingSink ─────────────────────────────────────────────────────────
// Forwards assistant deltas to a host-provided callback while capturing the
// full output (same spirit as AgentExecutor's CapturingSink, but streaming).
class StreamingSink final : public daedalus::AgentSink {
public:
    using DeltaFn = std::function<void(std::string_view)>;
    explicit StreamingSink(DeltaFn fn) : on_delta_(std::move(fn)) {}

    void on_assistant_delta(std::string_view text) noexcept override {
        if (text.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            output_ += text;
            user_output_ += text;   // natural-language text is user-facing
        }
        if (on_delta_) on_delta_(text);
    }

    void on_tool_call(std::string_view name,
                      std::string_view /*call_id*/,
                      const nlohmann::json& args) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        output_ += "\n[tool: ";
        output_ += name;
        try {
            if (!args.is_null() && args.is_object()) {
                for (auto it = args.begin(); it != args.end(); ++it) {
                    if (it.value().is_string()) {
                        std::string s = it.value().get<std::string>();
                        if (s.size() > 80) s = s.substr(0, 77) + "...";
                        output_ += " ";
                        output_ += it.key();
                        output_ += "=";
                        output_ += s;
                    }
                }
            }
        } catch (...) {}
        output_ += "]\n";
    }

    void on_tool_result(std::string_view name,
                        std::string_view /*call_id*/,
                        const nlohmann::json& result) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        std::string r;
        try { r = result.dump(); } catch (...) {}
        if (r.size() > 500) r = r.substr(0, 497) + "...";
        output_ += "  → ";
        output_ += r;
        output_ += "\n";
        // tell_user carries a complete user-facing message (monkeybyte §7.3):
        // capture it UNTRUNCATED — it is exactly what the chat should show.
        if (name == "tell_user") {
            const std::string content = result.value("content", "");
            if (!content.empty()) {
                if (!user_output_.empty() && user_output_.back() != '\n')
                    user_output_ += '\n';
                user_output_ += '\n';
                user_output_ += content;
            }
        }
    }

    void on_error(daedalus::ErrorKind /*kind*/,
                  std::string_view msg,
                  bool will_retry) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        errored_ = true;
        output_ += "\n[error";
        if (will_retry) output_ += " (retrying)";
        output_ += "] ";
        output_ += msg;
        output_ += "\n";
    }

    void on_log(daedalus::LogLevel /*lvl*/, std::string_view msg) noexcept override {
        std::fwrite(msg.data(), 1, msg.size(), stderr);
        std::fputc('\n', stderr);
    }

    // Phase 3: routed through the approval callback when the roster agent
    // has require_approval=true (AgentHost sets it per turn); default allow.
    bool request_command_approval(std::string_view cmd) override {
        std::lock_guard<std::mutex> lk(m_);
        if (approval_fn_) return approval_fn_(cmd);
        return true;
    }

    void set_approval_fn(std::function<bool(std::string_view)> fn) {
        std::lock_guard<std::mutex> lk(m_);
        approval_fn_ = std::move(fn);
    }

    std::string output() const {
        std::lock_guard<std::mutex> lk(m_);
        return output_;
    }
    // Everything intended for the user, in arrival order: assistant prose
    // plus full tell_user payloads. This is what gets posted to the thread.
    std::string user_output() const {
        std::lock_guard<std::mutex> lk(m_);
        return user_output_;
    }
    bool errored() const { return errored_.load(); }

private:
    DeltaFn on_delta_;
    std::function<bool(std::string_view)> approval_fn_;
    mutable std::mutex m_;
    std::string output_;
    std::string user_output_;
    std::atomic<bool> errored_{false};
};


} // namespace

// Sanitize a roster display name into a valid daedalus agent_name
// (^[A-Za-z0-9_.-]+$, 1..64 chars, no leading '.'/'-').
std::string sanitize_agent_name(std::string name) {
    for (auto& c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) c = '-';
    }
    while (!name.empty() && (name.front() == '.' || name.front() == '-'))
        name.erase(name.begin());
    if (name.size() > 64) name.resize(64);
    if (name.empty() || !daedalus::is_valid_agent_name(name))
        throw std::invalid_argument("name is not usable as an agent name");
    return name;
}

// ─── ProxySink ─────────────────────────────────────────────────────────────
// Stable sink handed to daedalus::Agent at construction; forwards every
// callback to a per-turn target set by run_turn(). The Agent lives across
// turns, so its sink must too — the per-turn StreamingSink is swapped in
// here.
class ProxySink final : public daedalus::AgentSink {
public:
    void set_target(std::shared_ptr<daedalus::AgentSink> t) {
        std::lock_guard<std::mutex> lk(m_);
        target_ = std::move(t);
    }

    void on_assistant_delta(std::string_view text) noexcept override {
        if (auto t = target()) t->on_assistant_delta(text);
    }
    void on_tool_call(std::string_view n, std::string_view c,
                      const nlohmann::json& a) noexcept override {
        if (auto t = target()) t->on_tool_call(n, c, a);
    }
    void on_tool_result(std::string_view n, std::string_view c,
                        const nlohmann::json& r) noexcept override {
        if (auto t = target()) t->on_tool_result(n, c, r);
    }
    void on_state_change(int f, int to) noexcept override {
        if (auto t = target()) t->on_state_change(f, to);
    }
    void on_error(daedalus::ErrorKind k, std::string_view m,
                  bool r) noexcept override {
        if (auto t = target()) t->on_error(k, m, r);
    }
    void on_compaction(std::size_t b, std::size_t a) noexcept override {
        if (auto t = target()) t->on_compaction(b, a);
    }
    void on_log(daedalus::LogLevel l, std::string_view m) noexcept override {
        if (auto t = target()) t->on_log(l, m);
    }
    bool request_command_approval(std::string_view cmd) override {
        if (auto t = target()) return t->request_command_approval(cmd);
        return true;
    }

private:
    std::shared_ptr<daedalus::AgentSink> target() const {
        std::lock_guard<std::mutex> lk(m_);
        return target_;
    }
    mutable std::mutex m_;
    std::shared_ptr<daedalus::AgentSink> target_;
};



// ─── AgentHost ──────────────────────────────────────────────────────────────

AgentHost::AgentHost(RosterAgent info, fs::path dir,
                     AgentStore& agents, ApprovalStore& approvals,
                     AgentRegistry& registry, LLMStore& llm,
                     RoutineStore& routines, ThreadStore& threads)
    : info_(std::move(info)), dir_(std::move(dir)),
      agents_(agents), approvals_(approvals), registry_(registry),
      llm_(llm), routines_(routines), threads_(threads),
      proxy_sink_(std::make_shared<ProxySink>()) {
    fs::create_directories(dir_);
}

AgentHost::~AgentHost() {
    // daedalus 0.9.6: destruct under the global serialization mutex too
    // (shared global state — see AgentSerialization.h; recursive, so a
    // turn thread releasing the last ref mid-hold is safe).
    std::lock_guard<std::recursive_mutex> lk{agent_exec_mutex()};
    // Phase 10 fix: ~Agent does NOT save history — only shutdown() does.
    // Without this, evicting an idle-but-unsaved host (PATCH/DELETE)
    // silently discarded its conversation.
    if (agent_) {
        try { agent_->shutdown(); } catch (...) {}
        agent_.reset();
    }
}

void AgentHost::ensure_agent() {
    if (agent_) return;  // caller holds turn_mtx_

    // ── Container mode (Plan.md Phase 2) ─────────────────────────────
    // Containers are mandatory (user decision 2026-08-25 — the "host"
    // workspace_mode escape hatch is REMOVED; host-side execution was an
    // unprivileged dead end). AgentStore/RosterRoutes normalize any legacy
    // "host" value to "shared" before this ever sees it.
    containerized_ = true;
    std::string container_err;
    if (containerized_) {
        try {
            container_manager().ensure_running(info_.id);
        } catch (const std::exception& e) {
            // Degrade gracefully: chat still works, command execution
            // returns the runtime error to the agent at call time.
            container_err = e.what();
        }
    }

    daedalus::AgentConfig cfg;
    cfg.agent_name  = sanitize_agent_name(info_.name);
    cfg.project_root = dir_;
    cfg.model_name  = info_.model;   // "" = SettingsStore default
    // Phase 3: require_approval agents get auto_approve OFF so daedalus
    // asks the sink before running commands; the sink blocks on the
    // human approval queue (set per turn in run_turn).
    cfg.auto_approve = !info_.require_approval;

    std::string prompt_addendum;
    if (containerized_) {
        // The built-in host-side execute_bash is replaced by the
        // container-backed tool registered below (same name).
        cfg.tools_disabled.insert("execute_bash");
        prompt_addendum =
            "\n\nEnvironment: you work inside your own Linux container "
            "(Ubuntu, user 'hive' with passwordless sudo — install CLI tools "
            "with apt/pip as needed). /workspace is the shared company "
            "workspace visible to your teammates; /home/hive is private to "
            "you. Use the container_exec tool for ALL command execution — it "
            "runs inside your container (there is no host shell). "
            "Before doing file work in /workspace, read /workspace/index.md "
            "— it maps the company drive (departments, projects, conventions)."
            "\n\nComputer use: your container has a full desktop with a "
            "persistent Chrome browser you drive with the browser_* tools "
            "(browser_navigate to open a URL, browser_read to see the page, "
            "browser_click/browser_type/browser_press to interact, "
            "browser_screenshot to capture what you see, browser_eval for "
            "JavaScript). Use these for ANY website task — reading pages, "
            "filling forms, logging into services. The user can watch your "
            "screen live (the Computer button); if you hit a login wall or "
            "captcha, tell the user and PAUSE — they can take over the same "
            "browser session, log in, and hand control back (the profile "
            "persists). Never say you lack browsing tools: you have them.";
        if (!container_err.empty())
            prompt_addendum +=
                "\n\nWARNING: the container runtime is currently unavailable ("
                + container_err +
                "). container_exec will return errors until it is fixed — "
                "tell the user if you need to run commands.";
    }

    if (!info_.role.empty()) {
        cfg.system_prompt =
            "You are " + info_.name + ", a persistent AI teammate in the "
            "HiveMind system. The user messages you like a colleague and you "
            "finish work end to end, reporting back in the conversation.\n\n"
            "Your role: " + info_.role + prompt_addendum;
    } else if (!prompt_addendum.empty()) {
        cfg.system_prompt =
            "You are " + info_.name + ", a persistent AI teammate in the "
            "HiveMind system." + prompt_addendum;
    }

    // Phase 13: resolve the model in HiveMind's own LLMStore and inject
    // credentials via AgentConfig's explicit fields. daedalus' SettingsStore
    // is never consulted (model_name carries the remote model_id, not a
    // daedalus settings key, so make_app_config falls through to these).
    if (!info_.model.empty()) {
        auto r = llm_.resolve(info_.model);
        if (!r)
            throw std::invalid_argument(
                "unknown model '" + info_.model + "' for agent '" +
                info_.name + "' — configure it on the Models page");
        cfg.api_host       = r->host;
        cfg.api_key        = r->api_key;
        cfg.chat_endpoint  = r->chat_endpoint;
        cfg.model_name     = r->model_id;
        cfg.max_context_size = r->max_context_size;
        cfg.temperature    = r->temperature;
    } else if (auto d = llm_.resolve("")) {
        // No per-agent model: LLMStore default wins; empty store falls back
        // to daedalus' own SettingsStore default (legacy path).
        cfg.api_host       = d->host;
        cfg.api_key        = d->api_key;
        cfg.chat_endpoint  = d->chat_endpoint;
        cfg.model_name     = d->model_id;
        cfg.max_context_size = d->max_context_size;
        cfg.temperature    = d->temperature;
    }

    bool containerized = containerized_;
    agent_ = std::make_unique<daedalus::Agent>(std::move(cfg), proxy_sink_);
    agent_->initialize();
    // So a SIGSEGV/SIGABRT still writes messages.json.crash (daedalus crash
    // resilience) — the normal save path only runs on shutdown().
    agent_->install_crash_handlers();

    if (containerized) {
        // Container-backed command execution (host built-in disabled above).
        // Same approval gate as the sink path: host tools bypass daedalus'
        // own command approval, so the tool enforces it itself.
        auto tool = std::make_unique<ContainerBashTool>(info_.id);
        if (info_.require_approval) {
            tool->set_approval_fn([this](std::string_view cmd) -> bool {
                return request_approval("command", std::string{cmd});
            });
        }
        agent_->register_tool(std::move(tool));

        // Phase 6 computer use: browser_* tools (persistent Chromium over
        // CDP on the container's Xvfb display — the same browser the user
        // watches/takes over in the Computer view). Same approval gate.
        auto approval = [this](std::string_view what) -> bool {
            return request_approval("browser", std::string{what});
        };
        for (auto& spec : BrowserTool::specs()) {
            auto bt = std::make_unique<BrowserTool>(info_.id, std::move(spec));
            if (info_.require_approval) bt->set_approval_fn(approval);
            agent_->register_tool(std::move(bt));
        }
        auto fs = std::make_unique<BrowserFillSecretTool>(info_.id);
        if (info_.require_approval) fs->set_approval_fn(approval);
        agent_->register_tool(std::move(fs));
    }

    // Phase 3 collaboration tools — every roster agent gets these.
    agent_->register_tool(std::make_unique<ListAgentsTool>(agents_));
    agent_->register_tool(
        std::make_unique<MessageAgentTool>(info_.id, agents_, registry_));
    agent_->register_tool(std::make_unique<CreateAgentTool>(agents_));
    agent_->register_tool(
        std::make_unique<SaveRoutineTool>(info_.id, routines_, threads_));
    agent_->register_tool(
        std::make_unique<RunIcarusTool>(info_.id, dir_.string()));
}

void AgentHost::enqueue(PendingTurn turn) {
    if (dead_)
        throw std::runtime_error("agent '" + info_.name +
                                 "' was deleted or replaced");
    {
        std::lock_guard<std::mutex> lk(queue_m_);
        if (stopping_)
            throw std::runtime_error("agent '" + info_.name +
                                     "' is shutting down");
        touch();
        queue_.push_back(std::move(turn));
        if (!driver_running_) {
            driver_running_ = true;
            std::thread([this] { driver_loop(); }).detach();
        }
    }
    queue_cv_.notify_one();
}

void AgentHost::driver_loop() {
    for (;;) {
        PendingTurn turn;
        {
            std::unique_lock<std::mutex> lk(queue_m_);
            queue_cv_.wait(lk, [&] { return !queue_.empty(); });
            turn = std::move(queue_.front());
            queue_.pop_front();
            if (queue_.empty()) driver_running_ = false;   // exit when idle
        }
        try { run_one(turn); }
        catch (const std::exception& e) {
            try { turn.on_done("", e.what()); } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lk(queue_m_);
            if (!queue_.empty()) driver_running_ = true;   // more work queued
            else return;   // idle — a later enqueue() restarts the driver
        }
    }
}

void AgentHost::run_one(PendingTurn& turn) {
    // turn_mtx_ serializes against shutdown()/evict(); the driver thread
    // already serializes turns against each other.
    std::lock_guard<std::mutex> turn_lk{turn_mtx_};

    // Phase 8: in-flight for the whole turn; eviction skips busy hosts.
    in_flight_.fetch_add(1, std::memory_order_relaxed);
    struct FlightGuard {
        AgentHost* h;
        ~FlightGuard() {
            h->in_flight_.fetch_sub(1, std::memory_order_relaxed);
            h->touch();
        }
    } flight_guard{this};

    // Whole turn under the global agent mutex — daedalus 0.9.6 global state
    // is not multi-agent safe. Held only for the LLM round, never while
    // waiting on the queue.
    std::lock_guard<std::recursive_mutex> global_lk{agent_exec_mutex()};

    ensure_agent();

    auto sink = std::make_shared<StreamingSink>(std::move(turn.on_delta));
    sink->set_approval_fn([this](std::string_view cmd) -> bool {
        return request_approval("command", std::string{cmd});
    });
    proxy_sink_->set_target(sink);

    // RAII guard: a throwing turn must still restore delegation depth and
    // detach the per-turn sink.
    // RAII guard: detach the per-turn sink even if the turn throws.
    struct TurnGuard {
        ProxySink* proxy;
        ~TurnGuard() { proxy->set_target(nullptr); }
    };
    TurnGuard guard{proxy_sink_.get()};

    // Watchdog: a hung LLM call blocks this agent's queue forever; cancel
    // past the timeout so later queued turns unblock.
    static const long kTurnTimeoutS = [] {
        const char* e = std::getenv("HIVEMIND_TURN_TIMEOUT_S");
        return (e && std::atol(e) > 0) ? std::atol(e) : 240;
    }();
    std::atomic<bool> turn_done{false};
    std::thread([this, &turn_done] {
        for (long i = 0;
             i < kTurnTimeoutS && !turn_done.load(std::memory_order_acquire);
             ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!turn_done.load(std::memory_order_acquire) && agent_)
            agent_->cancel();
    }).detach();

    agent_->submit_user_message(turn.prompt.empty() ? "(no input)"
                                                    : turn.prompt);
    agent_->run_one_turn();
    turn_done.store(true, std::memory_order_release);

    // daedalus warns submit/run_one_turn pairs must stay 1:1 — catch drift.
    if (agent_->pending_count() != 0)
        std::fprintf(stderr,
            "[hivemind] WARN agent '%s' queue drift: pending_count=%zu\n",
            info_.name.c_str(), agent_->pending_count());

    std::string out = sink->user_output();
    if (out.empty()) out = sink->output();   // never lose a turn entirely
    static const bool debug = std::getenv("HIVEMIND_DEBUG") != nullptr;
    if (debug) {
        namespace fs = std::filesystem;
        fs::path dir = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                     / ".local" / "share" / "hivemind";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f{dir / "turn-transcripts.log", std::ios::app};
        f << "=== " << info_.name << " ===\n" << sink->output() << "\n\n";
    }
    if (sink->errored())
        turn.on_done("", "agent reported errors (see output)");
    else
        turn.on_done(out.empty() ? "" : out, "");
}

void AgentHost::cancel() noexcept {
    // daedalus: cancel() is stop_token-equivalent and noexcept. Idle agents
    // ignore it. agent_ is process-immortal during turns (graveyard rule).
    try { if (agent_) agent_->cancel(); } catch (...) {}
}

std::string AgentHost::run_turn(const std::string& prompt,
                                std::function<void(std::string_view)> on_delta) {
    // Synchronous compat wrapper over the queue (Phase 11.1).
    auto p = std::make_shared<std::promise<std::string>>();
    auto fut = p->get_future();
    PendingTurn t;
    t.prompt = prompt;
    t.on_delta = std::move(on_delta);
    t.on_done = [p](std::string out, std::string err) {
        if (!err.empty()) {
            if (out.empty())
                p->set_exception(std::make_exception_ptr(
                    std::runtime_error(err)));
            else
                p->set_value(out + "\n[error] " + err);
        } else {
            p->set_value(out.empty() ? "(agent produced no output)" : out);
        }
    };
    enqueue(std::move(t));
    return fut.get();
}

bool AgentHost::request_approval(const std::string& kind,
                                 const std::string& payload) {
    // Phase 16: rule-based auto-review — a matching rule short-circuits
    // the human queue; the decision is still recorded for the inbox.
    if (auto m = approvals_.match_rule(info_.id, kind, payload)) {
        approvals_.record_auto(info_.id, info_.name, kind, payload, *m);
        return *m;
    }
    std::string id = approvals_.create(info_.id, info_.name, kind, payload);
    return approvals_.wait(id, /*timeout_s=*/600);
}

void AgentHost::shutdown() {
    std::lock_guard<std::mutex> turn_lk{turn_mtx_};
    std::lock_guard<std::recursive_mutex> global_lk{agent_exec_mutex()};
    if (!agent_) return;
    try {
        agent_->shutdown();   // → LLMClient::cleanup() → messages_->save()
    } catch (...) { /* shutting down anyway */ }
    agent_.reset();
}

// ─── AgentRegistry ──────────────────────────────────────────────────────────

AgentHost& AgentRegistry::get_or_create(const RosterAgent& agent) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = hosts_.find(agent.id);
    if (it != hosts_.end() && it->second->dead())
        it = hosts_.end();   // evicted since — construct a fresh host below
    if (it == hosts_.end()) {
        auto dir = AgentStore::agents_base_dir() / agent.id;
        auto host = std::make_unique<AgentHost>(agent, dir, agents_,
                                                approvals_, *this, llm_,
                                                routines_, threads_);
        it = hosts_.emplace(agent.id, std::move(host)).first;
    }
    return *it->second;
}

AgentHost* AgentRegistry::find(const std::string& agent_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = hosts_.find(agent_id);
    return it != hosts_.end() ? it->second.get() : nullptr;
}

void AgentRegistry::evict(const std::string& agent_id) {
    std::unique_ptr<AgentHost> host;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = hosts_.find(agent_id);
        if (it == hosts_.end()) return;
        it->second->mark_dead();
        host = std::move(it->second);
        hosts_.erase(it);
    }
    // Persist history BEFORE retiring: daedalus only saves on shutdown().
    // Blocks until any in-flight turn on this host finishes (turn_mtx_) —
    // correct by construction; the reference the turn holds stays valid
    // because the object lives on in the graveyard.
    try { host->shutdown(); } catch (...) {}
    std::lock_guard<std::mutex> lk(m_);
    graveyard_.push_back(std::move(host));   // dead + saved, not freed
}

/// Phase 8: evict hosts idle longer than `timeout` (and not busy).
namespace {

/// Phase 18: render a message's attachments as an explicit file-reference
/// block for the LLM (the agent's container sees the shared workspace at
/// /workspace). Empty when the message has no attachments.
std::string attachment_refs(const std::string& attachments_json) {
    if (attachments_json.empty() || attachments_json == "[]") return "";
    auto atts = nlohmann::json::parse(attachments_json, nullptr, false);
    if (!atts.is_array() || atts.empty()) return "";
    std::string out = "\n\n[Attached files in the shared workspace:";
    bool any = false;
    for (auto& a : atts) {
        const std::string p = a.value("path", "");
        if (p.empty()) continue;
        out += "\n- " + p;
        any = true;
    }
    if (!any) return "";
    return out + "]\n";
}

} // namespace

/// Eviction persists history via shutdown() — the host lazily re-creates
/// on next use, so idle eviction is pure memory relief.
void AgentRegistry::evict_idle(std::chrono::steady_clock::duration timeout) {
    std::vector<std::string> victims;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [id, host] : hosts_)
            if (!host->busy() && host->idle_for() > timeout)
                victims.push_back(id);
    }
    for (auto& id : victims) {
        std::cout << "[registry] evicting idle agent host " << id << "\n";
        evict(id);
    }
}

void AgentRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& [id, host] : hosts_) {
        try { host->shutdown(); } catch (...) {}
    }
    for (auto& host : graveyard_) {
        try { host->shutdown(); } catch (...) {}
    }
    // All turns drained (quiesce ran first) — safe to destroy everything.
    hosts_.clear();
    graveyard_.clear();
}

// ─── ChatService ────────────────────────────────────────────────────────────

std::string ChatService::post_agent_message(const std::string& sender_id,
                                            const std::string& target_id,
                                            const std::string& body) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto sender = agents_.get_agent(sender_id);
    auto target = agents_.get_agent(target_id);
    if (!sender || !target)
        throw std::invalid_argument("unknown sender or target agent");

    // Find-or-create the pair thread: kind "group" with exactly these two
    // participants. Reused across messages so the conversation accumulates
    // and is visible in the client ("Scout \u2194 Scribe").
    std::string tid;
    for (const auto& t : threads_.list_threads()) {
        if (t.kind == "group" && t.agent_ids.size() == 2 &&
            std::find(t.agent_ids.begin(), t.agent_ids.end(), sender_id) !=
                t.agent_ids.end() &&
            std::find(t.agent_ids.begin(), t.agent_ids.end(), target_id) !=
                t.agent_ids.end()) {
            tid = t.id;
            break;
        }
    }
    if (tid.empty()) {
        auto t = threads_.create_thread(
            sender->name + " \u2194 " + target->name, "group",
            {sender_id, target_id});
        tid = t.id;
    }

    threads_.add_message(tid, "agent", sender_id, body);
    // Generation 1: the target's reply fans back to the sender at gen 2,
    // one more hop at gen 3, then kMaxFanInGenerations stops the ping-pong.
    fan_in(tid, *target, sender->name, body, /*generation=*/1);
    return tid;
}

void ChatService::fan_in(const std::string& thread_id,
                         const RosterAgent& agent,
                         const std::string& sender_label,
                         const std::string& body,
                         int generation) {
    // daedalus persists each agent's conversation (messages.json) across
    // restarts, so the full thread context is already there — every turn
    // gets ONLY the new message. DO NOT add a thread-history backlog here
    // (removed 2026-08-26): threads fix participants at creation, so its
    // only trigger was restarts, where it duplicated restored history.
    // See docs/agents.md "Conversation context".
    std::string prompt = "[" + sender_label + "] " + body;

    auto live = std::make_shared<LiveTurn>();
    live->agent_id = agent.id;
    live->agent_name = agent.name;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto& vec = live_[thread_id];
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [](const auto& t) { return t->done; }),
                  vec.end());
        if (vec.size() >= kMaxLivePerThread)
            vec.erase(vec.begin());
        vec.push_back(live);
    }

    ++active_turns_;
    auto& host = registry_.get_or_create(agent);
    AgentHost::PendingTurn t;
    t.prompt = std::move(prompt);
    t.on_delta = [live](std::string_view delta) { live->partial += delta; };
    t.on_done = [this, thread_id, agent_id = agent.id,
                 agent_name = agent.name, live, generation](
                    std::string out, std::string err) mutable {
        struct Dec { std::atomic<int>& n; ~Dec() { --n; } } dec{active_turns_};
        if (!err.empty()) {
            live->error = err;
            if (!out.empty())
                out += "\n\n*(turn ended with errors)*";
        }
        // Post only substantive user-facing output; context-only turns that
        // produced nothing (the common agent→agent fan-in terminator) stay
        // silent instead of flooding the thread with acknowledgements.
        if (!out.empty() && out != "(agent produced no output)") {
            try {
                threads_.add_message(thread_id, "agent", agent_id, out);
                // Agent→agent fan-in: peers hear the reply, generation-capped
                // so two agents can't talk to each other forever.
                if (generation < kMaxFanInGenerations) {
                    auto thread = threads_.get_thread(thread_id);
                    if (thread) {
                        for (const auto& peer_id : thread->agent_ids) {
                            if (peer_id == agent_id) continue;
                            auto peer = agents_.get_agent(peer_id);
                            if (peer)
                                fan_in(thread_id, *peer, agent_name, out,
                                       generation + 1);
                        }
                    }
                }
            } catch (...) { /* thread deleted mid-turn, or shutting down */ }
        }
        live->done = true;
    };
    try {
        host.enqueue(std::move(t));
    } catch (const std::exception& e) {
        live->error = e.what();
        live->done = true;
        --active_turns_;
    }
}

std::string ChatService::post_user_message(const std::string& thread_id,
                                           const std::string& body,
                                           const std::string& attachments_json) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto thread = threads_.get_thread(thread_id);
    if (!thread)
        throw std::invalid_argument("thread not found: " + thread_id);

    std::string msg_id =
        threads_.add_message(thread_id, "user", "", body, attachments_json);

    // The LLM gets an explicit reference list; the stored body stays clean
    // (attachments render as chips in the client from attachments_json).
    const std::string prompt_body = body + attachment_refs(attachments_json);

    for (const auto& agent_id : thread->agent_ids) {
        auto agent = agents_.get_agent(agent_id);
        if (!agent) continue;  // participant deleted from roster — skip
        fan_in(thread_id, *agent, "You", prompt_body, /*generation=*/0);
    }

    return msg_id;
}

std::string ChatService::post_nudge(const std::string& thread_id,
                                    const std::string& body) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto thread = threads_.get_thread(thread_id);
    if (!thread)
        throw std::invalid_argument("thread not found: " + thread_id);

    std::string msg_id =
        threads_.add_message(thread_id, "system", "hivemind", body);

    for (const auto& agent_id : thread->agent_ids) {
        auto agent = agents_.get_agent(agent_id);
        if (!agent) continue;
        fan_in(thread_id, *agent, "HiveMind", body, /*generation=*/0);
    }
    return msg_id;
}

void ChatService::cancel_thread(const std::string& thread_id) {
    std::vector<std::string> agent_ids;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = live_.find(thread_id);
        if (it == live_.end()) return;
        for (auto& t : it->second) {
            if (!t->done) {
                t->cancelling = true;
                agent_ids.push_back(t->agent_id);
            }
        }
    }
    for (const auto& id : agent_ids)
        if (auto* host = registry_.find(id))
            host->cancel();
}

void ChatService::quiesce() {
    stopping_ = true;
    while (active_turns_.load() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::vector<ChatService::LiveTurn> ChatService::activity(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<LiveTurn> out;
    auto it = live_.find(thread_id);
    if (it == live_.end()) return out;
    for (auto& t : it->second) out.push_back(*t);
    return out;
}

std::string AgentHost::state_name() const noexcept {
    if (!agent_) return "idle";
    // daedalus to_string is capitalized ("Acting") — the API contract is
    // lowercase (client filters compare case-sensitively).
    std::string s = daedalus::to_string(agent_->state());
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

std::string AgentRegistry::agent_state(const std::string& agent_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = hosts_.find(agent_id);
    if (it == hosts_.end() || it->second->dead()) return "idle";
    return it->second->state_name();
}

bool ChatService::busy(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = live_.find(thread_id);
    if (it == live_.end()) return false;
    for (auto& t : it->second)
        if (!t->done) return true;
    return false;
}

std::unordered_map<std::string, ChatService::LiveTurn> ChatService::agent_activity() {
    std::lock_guard<std::mutex> lk(m_);
    std::unordered_map<std::string, LiveTurn> out;
    for (auto& [tid, turns] : live_)
        for (auto& t : turns)
            if (!t->done) out[t->agent_id] = *t;  // later threads overwrite; fine for status
    return out;
}
