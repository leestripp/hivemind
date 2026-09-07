/**
 * @file AgentImpl.h
 * @brief PIMPL for hive::Agent.
 *
 * §Phase 1+: backs the public Agent facade with the real subsystems. Hosts
 * see only `hive::Agent` (in include/hive-brain/Agent.h); the heavy
 * implementation (LLMClient, McpServerManager) lives
 * here in src/library/.
 *
 * Responsibilities:
 *   - Own an LLMClient instance (the real state machine + loop).
 *   - Translate AgentConfig <-> AppConfig (private).
 *   - Translate LLMClient::AgentState <-> public hive::AgentState.
 *   - Route AgentSink events into LLMClient's sink expectations.
 *   - Track AgentStats (incremental updates from LLMClient getters).
 *   - Forward cancel() into LLMClient's stop_source.
 */
#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "library/AgentSink.h"
#include "library/model_discovery.h"
#include "types/AgentConfig.h"
#include "types/AgentState.h"
#include "types/AgentStats.h"
#include "types/BaseTool.h"
#include "types/HostTool.h"   // deprecated alias
#include "types/ImageAttachment.h"
#include "types/ModelInfo.h"
#include "types/SkillInfo.h"
#include "types/ToolInfo.h"
#include "core/settings.h"

#include "api/LLMClient.h"
#include "core/SkillManager.h"
#include "mcp/McpServerManager.h"  // §Phase 11
#include "tools/ToolFilter.h"

namespace hive::detail {

class AgentImpl {
public:
    explicit AgentImpl(AgentConfig cfg, std::shared_ptr<AgentSink> sink);

    // Lifecycle
    void initialize();
    void shutdown();
    // §CRASH-4 (2026-07-16): forward to LLMClient::install_crash_handlers.
    // Idempotent. Call after initialize() and before run_one_turn().
    void install_crash_handlers();
    void uninstall_crash_handlers();
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // Conversation control
    void run_one_turn(std::stop_token st = {});
    void submit_user_message(std::string text);
    void submit_user_message(std::string text,
                             std::vector<ImageAttachment> images);
    [[nodiscard]] size_t pending_count() const noexcept { return pending_user_messages_.size(); }
    void cancel() noexcept;

    // Read-only views
    [[nodiscard]] AgentState state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] AgentStats stats() const noexcept;
    [[nodiscard]] const AgentConfig& config() const noexcept { return cfg_; }

    // §Phase 8.8: read-only mirror of cfg_.agent_name for the
    // public Agent's agent_name() getter. Validated in initialize().
    [[nodiscard]] std::string_view agent_name() const noexcept { return cfg_.agent_name; }

    [[nodiscard]] std::vector<ToolInfo>  list_tools()  const;
    [[nodiscard]] std::vector<SkillInfo> list_skills() const;
    [[nodiscard]] std::vector<ModelInfo> list_models() const;

    // Thinking levels the current model honors (§ThinkingLevel).
    // Empty when the model has no thinking control or no LLMClient
    // exists yet (no model configured).
    [[nodiscard]] std::vector<hive::ThinkingLevel>
        supported_thinking_levels() const;

    // Runtime thinking-level toggle (§ThinkingLevel). Applies from the
    // next chat request; no agent rebuild needed.
    void set_thinking_level(hive::ThinkingLevel level) noexcept;

    // Host-requested context compression (§AutoCompress). Flags the
    // LLMClient to compress at the end of the current/next agent cycle.
    void compress_context() noexcept;

    /// @brief Names of MCP servers currently configured (loaded from the
    ///        SQLite SettingsStore during initialize(), with the legacy
    ///        `mcp/servers.json` merged in as a fallback). Empty when
    ///        `cfg.mcp_enabled == false` or no servers are configured.
    [[nodiscard]] std::vector<std::string> mcp_configured_servers() const;
    [[nodiscard]] ModelInfo              active_model() const noexcept;
    [[nodiscard]] bool                   auto_approve() const noexcept;

    // §BUG-006/007/008: host-facing approval control. See hive-brain/Agent.h.
    void set_auto_approve(bool enabled) noexcept;
    void clear_session_approvals() noexcept;
    void clear_persisted_approvals() noexcept;
    void set_remember_approvals(bool enabled) noexcept;
    static std::string     library_version() noexcept;
    static ModelDiscoveryResult discover_models(
        const std::string& model_name,
        const std::filesystem::path& daedalus_home = {});
    nlohmann::json execute_tool(const std::string& name,
                                const nlohmann::json& args);

    // Todo list
    nlohmann::json add_todo(
        const std::string& text,
        const std::optional<std::string>& status = std::nullopt,
        const std::optional<std::string>& group = std::nullopt);
    nlohmann::json edit_todo(
        int id,
        const std::optional<std::string>& text = std::nullopt,
        const std::optional<std::string>& status = std::nullopt,
        const std::optional<std::string>& group = std::nullopt);
    nlohmann::json delete_todo(int id);
    nlohmann::json list_todos(
        const std::string& filter = "all",
        const std::optional<std::string>& group = std::nullopt);
    nlohmann::json count_todos();
    nlohmann::json set_max_todos(int max);
    nlohmann::json clear_todos();

    // Extension
    void register_tool(std::unique_ptr<BaseTool> tool);
    void clear_memory();
    bool switch_model(const std::string& model_name);
    [[nodiscard]] std::vector<std::string> list_providers() const;
    [[nodiscard]] std::vector<ProviderConfig> list_providers_detailed() const;
    bool upsert_provider(const ProviderConfig& provider);
    bool delete_provider(const std::string& provider);
    /// §ProviderPlugins: resolved plugin name for the active session.
    std::string active_provider_plugin() const;
    bool upsert_model(const ModelConfig& model);
    bool delete_model(const std::string& name);
    bool switch_provider(const std::string& provider);
    [[nodiscard]] ModelDiscoveryResult discover_provider_models() const;
    [[nodiscard]] std::string last_assistant_text() const;

    // §Phase 16: runtime enable/disable
    bool set_skill_enabled(const std::string& name, bool enabled);
    [[nodiscard]] bool is_skill_enabled(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> disabled_skills() const;

    /// @brief §SKILL-ORDER regression hook: true when the LLMClient
    ///        exists and has the per-agent SkillManager attached.
    ///        Exposed for tests; not part of the public Agent API.
    [[nodiscard]] bool client_has_skill_manager() const noexcept;

    bool set_mcp_server_enabled(const std::string& name, bool enabled);
    [[nodiscard]] bool is_mcp_server_enabled(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> disabled_mcp_servers() const;

    bool set_tool_enabled(const std::string& name, bool enabled);
    [[nodiscard]] bool is_tool_enabled(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> disabled_tools() const;

private:
    void set_state(AgentState s) noexcept;

    AgentConfig cfg_;
    std::shared_ptr<AgentSink> sink_;
    std::atomic<AgentState> state_{AgentState::Idle};
    AgentStats stats_;
    std::atomic<bool> cancel_requested_{false};

    // Pending user messages — agents consume them one per turn.
    // §READ-IMAGE-CORE follow-up (2026-08-10): queued user messages carry
    // optional image attachments from the public
    // submit_user_message(text, imgs) overload.
    struct PendingUserMessage {
        std::string text;
        std::vector<hive::ImageAttachment> images;
    };
    std::vector<PendingUserMessage> pending_user_messages_;

    // Forward-declared body. We define it inline here so that std::unique_ptr<Body>
    // can be destroyed at the AgentImpl instantiation point (PIMPL needs the
    // complete type visible to the deleter).
    struct Body {
        // §B2 fix (v0.5.1): stored by value. SettingsStore is now copyable
        // (Impl fields inlined), so the optional holds the owned instance.
        // LLMClient receives std::addressof(*store) — the pointer is stable
        // for the lifetime of Body (LLMClient is destroyed before store in
        // shutdown()).
        std::optional<hive::SettingsStore> store;
        std::unique_ptr<mcp::McpServerManager> mcp;   // §Phase 11: owned; nullptr when mcp_enabled=false
        std::unique_ptr<SkillManager>          skill_manager;  // §Phase 16: per-agent
        std::unique_ptr<LLMClient>             client;
        std::stop_source                       cancel_source;
    };

    // §Phase 16: per-agent tool/MCP filter. Mutable so const list methods
    // can apply the current disabled sets.
    mutable ToolFilter tool_filter_;
    std::unique_ptr<Body> body_;

    // Host-registered tools (Phase 6: registered with ToolFactory).
    // Stored as shared_ptr so the HostToolAdapter can hold the same pointer.
    std::vector<std::shared_ptr<BaseTool>> host_tools_;
    std::shared_ptr<HostToolContext>       host_ctx_;

    bool initialized_ = false;
};

}  // namespace hive::detail
