// =============================================================================
// src/AgentHost.cpp — long-lived daedalus::Agent per roster agent + ChatService
// (HiveMind 2.0, Plan.md Phase 1)
// =============================================================================
#include "hivemind/AgentHost.h"
#include "hivemind/AgentSerialization.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/ContainerTools.h"

#include "daedalus/settings.h"
#include "daedalus/default_paths.h"

#include <cstdio>
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

    void on_tool_result(std::string_view /*name*/,
                        std::string_view /*call_id*/,
                        const nlohmann::json& result) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        std::string r;
        try { r = result.dump(); } catch (...) {}
        if (r.size() > 500) r = r.substr(0, 497) + "...";
        output_ += "  → ";
        output_ += r;
        output_ += "\n";
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

    // Chat context has no approval UI yet — auto-approve (Phase 3 adds the
    // approvals service that gates these).
    bool request_command_approval(std::string_view /*cmd*/) override { return true; }

    std::string output() const {
        std::lock_guard<std::mutex> lk(m_);
        return output_;
    }
    bool errored() const { return errored_.load(); }

private:
    DeltaFn on_delta_;
    mutable std::mutex m_;
    std::string output_;
    std::atomic<bool> errored_{false};
};


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

} // namespace

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

AgentHost::AgentHost(RosterAgent info, fs::path dir)
    : info_(std::move(info)), dir_(std::move(dir)),
      proxy_sink_(std::make_shared<ProxySink>()) {
    fs::create_directories(dir_);
}

AgentHost::~AgentHost() {
    // daedalus 0.9.6: destruct under the global serialization mutex too
    // (shared global state — see AgentSerialization.h).
    std::lock_guard<std::mutex> lk{agent_exec_mutex()};
    agent_.reset();
}

void AgentHost::ensure_agent() {
    if (agent_) return;  // caller holds turn_mtx_

    // ── Container mode (Plan.md Phase 2) ─────────────────────────────
    // workspace_mode "host" is the explicit escape hatch (dev boxes
    // without podman); every other mode ("shared" today, dept volumes
    // later) means the agent's command execution happens in its podman
    // container as user `hive` with passwordless sudo.
    containerized_ = info_.workspace_mode != "host";
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
    cfg.auto_approve = true;         // no approval UI until Phase 3

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
            "runs inside your container (there is no host shell).";
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

    // Fail fast on unknown model names (same guard as AgentExecutor).
    if (!cfg.model_name.empty()) {
        auto& store = daedalus::SettingsStore::instance();
        auto resolved = store.resolve_model(cfg.model_name);
        if (!resolved || resolved->host.empty())
            throw std::invalid_argument(
                "unknown model '" + cfg.model_name + "' for agent '" +
                info_.name + "'");
    }

    bool containerized = containerized_;
    agent_ = std::make_unique<daedalus::Agent>(std::move(cfg), proxy_sink_);
    agent_->initialize();
    // So a SIGSEGV/SIGABRT still writes messages.json.crash (daedalus crash
    // resilience) — the normal save path only runs on shutdown().
    agent_->install_crash_handlers();

    if (containerized) {
        // Container-backed execute_bash (built-in disabled via config above).
        agent_->register_tool(std::make_unique<ContainerBashTool>(info_.id));
    }
}

std::string AgentHost::run_turn(const std::string& prompt,
                                std::function<void(std::string_view)> on_delta) {
    std::lock_guard<std::mutex> turn_lk{turn_mtx_};

    // Whole turn (construct-first-time + run) under the global agent mutex —
    // daedalus 0.9.6 global state is not multi-agent safe.
    std::lock_guard<std::mutex> global_lk{agent_exec_mutex()};

    ensure_agent();

    // daedalus takes the sink at construction and the Agent outlives turns,
    // so the stable ProxySink forwards to this turn's StreamingSink.
    auto sink = std::make_shared<StreamingSink>(std::move(on_delta));
    proxy_sink_->set_target(sink);

    agent_->submit_user_message(prompt.empty() ? "(no input)" : prompt);
    agent_->run_one_turn();
    proxy_sink_->set_target(nullptr);

    std::string out = sink->output();
    if (sink->errored())
        throw std::runtime_error("agent reported errors (see output)");
    return out.empty() ? "(agent produced no output)" : out;
}

void AgentHost::shutdown() {
    std::lock_guard<std::mutex> turn_lk{turn_mtx_};
    std::lock_guard<std::mutex> global_lk{agent_exec_mutex()};
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
    if (it == hosts_.end()) {
        auto dir = AgentStore::agents_base_dir() / agent.id;
        auto host = std::make_unique<AgentHost>(agent, dir);
        it = hosts_.emplace(agent.id, std::move(host)).first;
    }
    return *it->second;
}

void AgentRegistry::evict(const std::string& agent_id) {
    std::lock_guard<std::mutex> lk(m_);
    hosts_.erase(agent_id);
}

void AgentRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& [id, host] : hosts_) {
        try { host->shutdown(); } catch (...) {}
    }
}

// ─── ChatService ────────────────────────────────────────────────────────────

std::string ChatService::post_user_message(const std::string& thread_id,
                                           const std::string& body) {
    auto thread = threads_.get_thread(thread_id);
    if (!thread)
        throw std::invalid_argument("thread not found: " + thread_id);

    std::string msg_id = threads_.add_message(thread_id, "user", "", body);

    for (const auto& agent_id : thread->agent_ids) {
        auto agent = agents_.get_agent(agent_id);
        if (!agent) continue;  // participant deleted from roster — skip

        auto live = std::make_shared<LiveTurn>();
        live->agent_id = agent->id;
        live->agent_name = agent->name;
        {
            std::lock_guard<std::mutex> lk(m_);
            live_[thread_id].push_back(live);
        }

        std::thread([this, thread_id, agent = *agent, body, live]() mutable {
            try {
                auto& host = registry_.get_or_create(agent);
                std::string final = host.run_turn(body,
                    [live](std::string_view delta) {
                        live->partial += delta;
                    });
                if (final.empty()) final = live->partial;
                threads_.add_message(thread_id, "agent", agent.id, final);
            } catch (const std::exception& e) {
                live->error = e.what();
                threads_.add_message(thread_id, "agent", agent.id,
                                     std::string{"[turn failed] "} + e.what());
            }
            live->done = true;
        }).detach();
    }

    return msg_id;
}

std::vector<ChatService::LiveTurn> ChatService::activity(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<LiveTurn> out;
    auto it = live_.find(thread_id);
    if (it == live_.end()) return out;
    for (auto& t : it->second) out.push_back(*t);
    return out;
}

bool ChatService::busy(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = live_.find(thread_id);
    if (it == live_.end()) return false;
    for (auto& t : it->second)
        if (!t->done) return true;
    return false;
}
