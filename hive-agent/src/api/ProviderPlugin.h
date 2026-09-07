/**
 * @file hive-brain/ProviderPlugin.h
 * @brief Public provider-plugin interface.
 *
 * A ProviderPlugin encapsulates everything that differs between LLM
 * providers: base URL, endpoints, auth requirements, model knowledge,
 * request-payload shaping, conversation-history rules, streaming (SSE)
 * parsing, model-discovery parsing, and provider error-type
 * classification.
 *
 * `LLMClient` remains the single engine (retry/backoff, compression
 * triggers, size caps, token accounting, tool loop); plugins customize
 * behavior through the hooks below. `OpenAiCompatiblePlugin` (the
 * default) implements every hook with the library's classic generic
 * behavior, so unregistered providers keep working unchanged.
 *
 * Hosts may implement this interface and register it via
 * `ProviderRegistry::register_plugin()` (see
 * `include/hive-brain/ProviderRegistry.h`) to teach the library a new
 * provider without forking it.
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "types/DiscoveredModel.h"
#include "types/ErrorKind.h"
#include "types/ThinkingLevel.h"

namespace hive {

/// Result of parsing a models-endpoint response body.
struct ModelListParseResult {
    std::vector<DiscoveredModel> models;
    std::string error;  ///< empty = success (possibly with zero models)
};

class AgentSink;

/// Static knowledge about one model a provider offers.
struct ProviderModelInfo {
    std::string model_id;              ///< id sent in chat requests
    int64_t     context_size = 0;      ///< tokens; 0 = unknown
    double      default_temperature = 0.0;
    bool        supports_vision = false;
    bool        supports_thinking = false;
};

/// Read-only context handed to the payload hook for one chat request.
struct ProviderChatContext {
    std::string model_id;     ///< model id sent in the payload
    std::string model_name;   ///< settings-store key for the model
    std::string provider;     ///< provider label from the store row
    std::string base_url;     ///< API base URL (plugins may branch on host,
                              ///< e.g. Kimi platform vs Kimi Code endpoint)
    std::string cache_key;    ///< stable per-agent key for server-side prompt
                              ///< caches (e.g. Moonshot prompt_cache_key);
                              ///< empty = don't send one
    bool        stream = true;
    double      temperature = 0.0;
    /// Host-selected thinking level. Default = send nothing. Plugins map
    /// supported levels to provider parameters and ignore the rest.
    ThinkingLevel thinking_level = ThinkingLevel::Default;
};

/// Streaming (SSE) response parser interface. One instance per request.
/// Pulled out of the former concrete `MiniMaxSseParser` so each provider
/// can reassemble its own chunk dialect into the standard non-streaming
/// message shape the rest of the pipeline consumes.
class ProviderStreamParser {
public:
    virtual ~ProviderStreamParser() = default;

    /// Feed one raw HTTP chunk; SSE lines are buffered and parsed.
    virtual void on_chunk(std::string_view data) = 0;

    /// Flush any incomplete trailing line after the stream ends.
    virtual void finish() = 0;

    /// Reconstruct the assistant message in the standard non-streaming
    /// format from the accumulated delta fragments.
    virtual nlohmann::json synthesized_message() const = 0;

    /// The final SSE event as a JSON object (usage / finish_reason
    /// metadata). Null json when no valid event was received.
    virtual const nlohmann::json& final_event() const = 0;

    /// Tool-call objects accumulated across chunks (possibly empty).
    virtual const nlohmann::json& accumulated_tool_calls() const = 0;

    /// True when any assistant text streamed via the sink this turn.
    virtual bool had_text_deltas() const noexcept = 0;
};

class ProviderPlugin {
public:
    virtual ~ProviderPlugin() = default;

    // -- Identity & matching ------------------------------------------------

    /// Canonical plugin name, e.g. "minimax", "moonshot", "lmstudio",
    /// "openai-compatible".
    virtual std::string name() const = 0;

    /// Host-URL auto-detection used when a provider row has no explicit
    /// plugin set (e.g. matches "api.minimax.io"). Default: false.
    virtual bool matches(std::string_view host) const;

    // -- Endpoints & auth ----------------------------------------------------

    /// Base URL offered as the default when adding this provider.
    virtual std::string default_base_url() const = 0;

    virtual std::string chat_endpoint() const;    // "/v1/chat/completions"
    virtual std::string models_endpoint() const;  // "/v1/models"

    /// False for providers that need no API key (e.g. local LM Studio).
    virtual bool requires_auth() const;

    // -- Model knowledge -----------------------------------------------------

    /// Static model table. Empty means "discovery is the source of
    /// truth" (e.g. LM Studio, where model ids are user-chosen).
    virtual std::vector<ProviderModelInfo> known_models() const;

    /// Context size for a model id, 0 = unknown (caller falls back to
    /// the store value, then the global default).
    virtual int64_t context_size_for(std::string_view model_id) const;

    /// Thinking levels this model actually honors (subset of
    /// {None, Low, High, Max}). Empty = no thinking control; hosts use
    /// this to build per-model pickers. Default: empty.
    virtual std::vector<ThinkingLevel>
        supported_thinking_levels(std::string_view model_id) const;

    // -- Request shaping -----------------------------------------------------

    /// Mutate the chat-completions payload before it is sent. The
    /// default plugin applies the library's classic behavior. Provider
    /// plugins add their own parameters (thinking switches, usage
    /// options, cache keys) and clamp out-of-range values here.
    virtual void mutate_chat_payload(nlohmann::json& payload,
                                     const ProviderChatContext& ctx) const;

    /// Clamp/adjust the sampling temperature for a model. Default:
    /// returns `t` unchanged.
    virtual double clamp_temperature(std::string_view model_id, double t) const;

    /// Enforce provider conversation rules on the history before send
    /// (reasoning-field preservation, single-system-message rules).
    /// Default: no-op.
    virtual void sanitize_history(nlohmann::json& messages) const;

    // -- Streaming -----------------------------------------------------------

    /// Create the SSE parser for one streaming request.
    virtual std::unique_ptr<ProviderStreamParser>
        create_stream_parser(std::weak_ptr<AgentSink> sink) const;

    // -- Discovery -----------------------------------------------------------

    /// Parse a models-endpoint response body into DiscoveredModel
    /// records. Default: OpenAI `{"object":"list","data":[...]}` shape.
    virtual ModelListParseResult
        parse_models_response(const nlohmann::json& body) const;

    // -- Error classification --------------------------------------------------

    /// Map a provider error `type` string to an ErrorKind. Default:
    /// the library's classic classification list.
    virtual ErrorKind classify_api_error_type(std::string_view error_type) const;
};

} // namespace hive
