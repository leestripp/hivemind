/**
 * @file src/library/Agent.cpp
 * @brief Implementation of the hive::Agent facade.
 *
 * Holds std::unique_ptr<detail::AgentImpl>. The PIMPL ensures the public
 * Agent.h header stays small and doesn't drag in the heavy subsystem
 * types (LLMClient, McpServerManager, ...).
 */
#include "library/Agent.h"

#include "AgentImpl.h"

#include <stop_token>
#include <utility>

namespace hive {

Agent::Agent(AgentConfig cfg, std::shared_ptr<AgentSink> sink)
    : impl_(std::make_unique<detail::AgentImpl>(std::move(cfg), std::move(sink)))
{}

Agent::~Agent() = default;
Agent::Agent(Agent&&) noexcept = default;
Agent& Agent::operator=(Agent&&) noexcept = default;

// ---- Lifecycle -------------------------------------------------------------

void Agent::initialize()       { impl_->initialize(); }
void Agent::shutdown()         { impl_->shutdown(); }
// §CRASH-4 (2026-07-16): forward the crash-handler install/uninstall
// to AgentImpl, which forwards to LLMClient's Messages instance.
void Agent::install_crash_handlers()   { impl_->install_crash_handlers(); }
void Agent::uninstall_crash_handlers() { impl_->uninstall_crash_handlers(); }

// ---- Conversation control --------------------------------------------------

void Agent::run_one_turn(std::stop_token st)                 { impl_->run_one_turn(st); }
void Agent::submit_user_message(std::string text)             { impl_->submit_user_message(std::move(text)); }
void Agent::submit_user_message(std::string text,
                                std::vector<ImageAttachment> imgs)
{
    impl_->submit_user_message(std::move(text), std::move(imgs));
}
void Agent::cancel() noexcept { impl_->cancel(); }
size_t Agent::pending_count() const noexcept { return impl_->pending_count(); }

// ---- Read-only views -------------------------------------------------------

AgentState  Agent::state() const noexcept { return impl_->state(); }
AgentStats  Agent::stats() const noexcept { return impl_->stats(); }

std::string Agent::fatal_breaker_error() const
{
    return impl_->fatal_breaker_error();
}
const AgentConfig& Agent::config() const noexcept { return impl_->config(); }

std::string_view Agent::agent_name() const noexcept { return impl_->agent_name(); }

std::vector<ToolInfo>  Agent::list_tools()  const { return impl_->list_tools(); }
std::vector<SkillInfo> Agent::list_skills() const { return impl_->list_skills(); }
std::vector<ModelInfo> Agent::list_models() const { return impl_->list_models(); }
std::vector<ThinkingLevel> Agent::supported_thinking_levels() const { return impl_->supported_thinking_levels(); }

std::vector<std::string> Agent::mcp_configured_servers() const
{
    // §FIX-MCP-STORE-LOAD (2026-07-17): thin forwarder. Previously
    // unreachable from outside the library — AgentImpl built an
    // McpServerManager but never loaded it from the store, so this
    // would have returned []. Now returns whatever was loaded by
    // AgentImpl::AgentImpl's load_from_store() + load_config() chain.
    return impl_->mcp_configured_servers();
}

ModelInfo Agent::active_model() const noexcept { return impl_->active_model(); }
bool        Agent::auto_approve() const noexcept { return impl_->auto_approve(); }
void        Agent::compress_context() noexcept { impl_->compress_context(); }
void        Agent::set_thinking_level(ThinkingLevel level) { impl_->set_thinking_level(level); }

// §BUG-006/007/008: approval control
void Agent::set_auto_approve(bool enabled)        { impl_->set_auto_approve(enabled); }
void Agent::clear_session_approvals()             { impl_->clear_session_approvals(); }
void Agent::clear_persisted_approvals()           { impl_->clear_persisted_approvals(); }
void Agent::set_remember_approvals(bool enabled)  { impl_->set_remember_approvals(enabled); }
std::string Agent::library_version() noexcept   { return detail::AgentImpl::library_version(); }

ModelDiscoveryResult Agent::discover_models(
    const std::string& model_name,
    const std::filesystem::path& agent_home)
{
    return detail::AgentImpl::discover_models(model_name, agent_home);
}

nlohmann::json Agent::execute_tool(const std::string& name,
                                   const nlohmann::json& args)
{
    return impl_->execute_tool(name, args);
}

nlohmann::json Agent::add_todo(const std::string& text,
                               const std::optional<std::string>& status,
                               const std::optional<std::string>& group)
{
    return impl_->add_todo(text, status, group);
}

nlohmann::json Agent::edit_todo(int id,
                                const std::optional<std::string>& text,
                                const std::optional<std::string>& status,
                                const std::optional<std::string>& group)
{
    return impl_->edit_todo(id, text, status, group);
}

nlohmann::json Agent::delete_todo(int id)
{
    return impl_->delete_todo(id);
}

nlohmann::json Agent::list_todos(const std::string& filter,
                                 const std::optional<std::string>& group)
{
    return impl_->list_todos(filter, group);
}

nlohmann::json Agent::count_todos()
{
    return impl_->count_todos();
}

nlohmann::json Agent::set_max_todos(int max)
{
    return impl_->set_max_todos(max);
}

nlohmann::json Agent::clear_todos()
{
    return impl_->clear_todos();
}

void Agent::register_tool(std::unique_ptr<BaseTool> tool)
{
    impl_->register_tool(std::move(tool));
}

void Agent::clear_memory() { impl_->clear_memory(); }

bool Agent::switch_model(const std::string& model_name)
{
    return impl_->switch_model(model_name);
}

SettingsStore* Agent::settings()
{
    return impl_->settings();
}

std::vector<std::string> Agent::list_providers() const
{
    return impl_->list_providers();
}

std::vector<ProviderConfig> Agent::list_providers_detailed() const
{
    return impl_->list_providers_detailed();
}

bool Agent::upsert_provider(const ProviderConfig& provider)
{
    return impl_->upsert_provider(provider);
}

bool Agent::delete_provider(const std::string& provider)
{
    return impl_->delete_provider(provider);
}

std::string Agent::active_provider_plugin() const
{
    return impl_->active_provider_plugin();
}

bool Agent::upsert_model(const ModelConfig& model)
{
    return impl_->upsert_model(model);
}

bool Agent::delete_model(const std::string& name)
{
    return impl_->delete_model(name);
}

bool Agent::switch_provider(const std::string& provider)
{
    return impl_->switch_provider(provider);
}

ModelDiscoveryResult Agent::discover_provider_models() const
{
    return impl_->discover_provider_models();
}

std::string Agent::last_assistant_text() const
{
    return impl_->last_assistant_text();
}

// ----------------------------------------------------------------------------
// Runtime enable/disable
// ----------------------------------------------------------------------------

bool Agent::set_skill_enabled(const std::string& name, bool enabled)
{
    return impl_->set_skill_enabled(name, enabled);
}

bool Agent::is_skill_enabled(const std::string& name) const
{
    return impl_->is_skill_enabled(name);
}

std::vector<std::string> Agent::disabled_skills() const
{
    return impl_->disabled_skills();
}

bool Agent::set_mcp_server_enabled(const std::string& name, bool enabled)
{
    return impl_->set_mcp_server_enabled(name, enabled);
}

bool Agent::is_mcp_server_enabled(const std::string& name) const
{
    return impl_->is_mcp_server_enabled(name);
}

std::vector<std::string> Agent::disabled_mcp_servers() const
{
    return impl_->disabled_mcp_servers();
}

bool Agent::set_tool_enabled(const std::string& name, bool enabled)
{
    return impl_->set_tool_enabled(name, enabled);
}

bool Agent::is_tool_enabled(const std::string& name) const
{
    return impl_->is_tool_enabled(name);
}

std::vector<std::string> Agent::disabled_tools() const
{
    return impl_->disabled_tools();
}

}  // namespace hive