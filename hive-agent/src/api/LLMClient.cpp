#include "api/LLMClient.h"
#include "api/MiniMaxSseParser.h"   // §BUG-001: exposed for tests
#include "api/ThinkTagFilter.h"     // inline <think>...</think> stripping
#include "api/ContextStatus.h"      // §ContextCache: prompt-cache helpers
#include "api/ProviderRegistry.h"   // §ProviderPlugins
#include "api/ErrorMapping.h"       // §on_error: HTTP status / error-type → ErrorKind
#include "core/version.h"
#include "core/FileIO.h"
#include "core/utils.h"
#include "core/ImageProcessor.h"
#include "api/ImageRefs.h"
#include "core/SkillManager.h"
#include "core/Logger.h"
#include "core/SystemPromptBuilder.h"
#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"
#include "tools/BashTool.h"
#include "core/ThreadPool.h"
#include "core/Utf8Utils.h"  // §B11/B28: shared utf8_sanitize_replace_invalid
#include "library/default_paths.h"   // §Phase 8.8 — resolve_agent_dir()
#include "types/AgentState.h"  // §State Machine — agent_state_from()
#include "mcp/McpServerManager.h"  // §Phase 11 — injected via ctor

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <set>
#include <vector>
#include <sys/select.h>
#include <iconv.h>
#include <pwd.h>
#include <ctime>

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR via the existing
// Logger. Same formatted text routed through the Logger instead
// of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)

// Replaces invalid UTF-8 bytes with a replacement character.
// §B11/B28: state-machine fallback uses shared utf8_sanitize_replace_invalid
// from Utf8Utils.h (which also fixes the B12 truncation bug).
static std::string sanitize_utf8(const std::string& input) {
    // Try iconv conversion to ensure valid UTF-8
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    if (cd != (iconv_t)-1) {
        char* in_ptr = const_cast<char*>(input.data());
        size_t in_left = input.size();
        std::string output(input.size() * 4, '\0');
        char* out_ptr = &output[0];
        size_t out_left = output.size();
        size_t conv_result = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        iconv_close(cd);
        if (conv_result != (size_t)-1 && in_left == 0) {
            return output.c_str();  // conversion succeeded
        }
        // Fall through to state-machine path
    }
    // State-machine fallback via shared utility (fixes B12 truncation bug)
    return utf8_sanitize_replace_invalid(input);
}

// §Phase 5.1: TerminalGuard removed. Library no longer touches the
// host's terminal state — no tcgetattr, no tcsetattr, no raw mode,
// no isatty. Hosts that want ESC/Ctrl-C capture wire it themselves
// and call Agent::cancel() (or LLMClient::cancel()) on detection.

// Helper functions (moved from main.cpp)

// §NC-8 (2026-07-16): WriteBuffer / write_callback removed. The
// sync fallback path now uses OmniNet::HTTP::Client::send_blocking,
// which fills response_buffer directly — no manual write callback.

// ============================================================================
// Error Handling and Retry Helpers
// ============================================================================

/**
 * @brief Error category classification for API errors
 */
enum class ErrorCategory {
    TRANSIENT,  // Retryable errors (rate limit, internal errors, etc.)
    FATAL,      // Non-retryable errors (auth, permissions, etc.)
    UNKNOWN     // Default for unrecognized errors
};

/**
 * @brief Classify an API error type as transient or fatal
 */
static ErrorCategory classify_api_error(const std::string& error_type, const std::string& error_message = "") {
    // Fatal errors - don't retry (these won't be fixed by retrying)
    if (error_type == "authentication_error" ||
        error_type == "invalid_request" ||
        error_type == "permission_error" ||
        error_type == "invalid_api_key" ||
        error_type == "context_length_exceeded") {
        return ErrorCategory::FATAL;
    }
    
    // MiniMax rejects multiple "role": "system" - this is fatal, don't retry
    if (error_type == "bad_request_error" && 
        error_message.find("invalid message role") != std::string::npos) {
        return ErrorCategory::FATAL;
    }
    
    // "tool call result does not follow tool call" - FATAL, don't retry
    // This means messages got out of sync - retrying won't fix it
    if (error_type == "bad_request_error" && 
        error_message.find("tool call result does not follow tool call") != std::string::npos) {
        return ErrorCategory::FATAL;
    }
    
    // All other bad_request_error - also FATAL (don't retry on client errors)
    if (error_type == "bad_request_error") {
        return ErrorCategory::FATAL;
    }
    
    // Transient errors - retry with backoff (these are server-side issues)
    if (error_type == "rate_limit" ||
        error_type == "rate_limit_error" ||
        error_type == "internal_error" ||
        error_type == "service_unavailable" ||
        error_type == "timeout" ||
        error_type == "server_error" ||
        error_type == "internal_server_error" ||
        error_type == "service_overloaded" ||
        error_type == "empty_response") {
        return ErrorCategory::TRANSIENT;
    }
    
    // Unknown - treat as FATAL by default to avoid polluting messages with retries
    return ErrorCategory::FATAL;
}

// §B9 refactor: extract "find last tool_call" walk into a single helper.
// Used by both the FATAL error path (with args for debugging) and the
// retry-fatal path (id + name only).
struct LastToolCall {
    std::string id;
    std::string name;
    std::string args;  // optional; empty if not requested
};

static LastToolCall find_last_tool_call(const json& messages,
                                        bool want_args = false) {
    LastToolCall out;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        if (!messages[i].is_object()) continue;
        if (messages[i].value("role", "") != "assistant") continue;
        if (!messages[i].contains("tool_calls") ||
            !messages[i]["tool_calls"].is_array()) continue;
        auto& tcs = messages[i]["tool_calls"];
        if (tcs.empty() || !tcs[tcs.size() - 1].is_object()) continue;
        auto& last_tc = tcs[tcs.size() - 1];
        out.id = last_tc.value("id", "UNKNOWN");
        if (last_tc.contains("function") && last_tc["function"].is_object()) {
            out.name = last_tc["function"].value("name", "UNKNOWN");
            if (want_args &&
                last_tc["function"].contains("arguments") &&
                last_tc["function"]["arguments"].is_string()) {
                out.args = last_tc["function"]["arguments"].get<std::string>();
                if (out.args.length() > 200) {
                    out.args = out.args.substr(0, 200) + "...";
                }
            }
        }
        break;
    }
    return out;
}



// §ProviderPlugins: resolve the plugin for a config. An explicit plugin
// name from the providers row wins; empty = auto-detect from provider
// label/host; unknown names fall back to auto-detect with a warning.
static std::shared_ptr<hive::ProviderPlugin> resolve_provider_plugin(const AppConfig& c) {
    if (!c.plugin.empty()) {
        if (auto p = hive::ProviderRegistry::plugin_for_name(c.plugin)) return p;
        LOG_WARNING("[ProviderPlugin] providers row names unknown plugin '" + c.plugin +
                    "' — falling back to auto-detect");
    }
    return hive::ProviderRegistry::resolve(c.provider, c.base_url);
}

// Implementation of LLMClient methods
LLMClient::LLMClient(const AppConfig& config, hive::SettingsStore* settings_store,
                       mcp::McpServerManager* mcp_manager,
                       SkillManager* skill_manager,
                       const ToolFilter* tool_filter) :
    config_(config),
    file_io_(std::make_unique<FileIO>(config.target_directory, config.agent_name)),
    settings_store_(settings_store),
    iteration_count_(0),   // §Phase 11: order follows declaration; sink_ defaulted
    mcp_manager_(mcp_manager),   // §Phase 11: injected; nullptr = MCP disabled
    skill_manager_(skill_manager),   // §Phase 16: per-agent skill manager
    tool_filter_(tool_filter)        // §Phase 16: per-agent tool filter
{
    // §ProviderPlugins: resolve this client's provider plugin once per
    // config. Falls back to the OpenAI-compatible default when the
    // provider label/host names no registered plugin.
    plugin_ = resolve_provider_plugin(config_);
    LOG_INFO(std::string("[ProviderPlugin] active plugin: ") + plugin_->name() +
             " (provider='" + config_.provider + "', model='" + config_.model + "')");

    // Load persisted command approvals
    ToolHandler::load_persisted_approvals(config.target_directory, config.agent_name);
    // Initialize current working directory tracker with project root
    ToolHandler::init_current_working_directory(config.target_directory);
    
    // Initialize Session Memory (if not disabled)
    if (!config_.memory_disabled) {
        // §Phase 8.8 / §Phase 16: SessionMemory takes the resolved
        // per-agent namespace dir. Empty agent_name yields
        // `<project>/.daedalus/hive-brain/memory/`.
        session_memory_ = std::make_unique<SessionMemory>(
            hive::resolve_agent_dir(config.target_directory,
                                       config.agent_name));
        session_memory_->on_session_start();
    }
    
    // Initialize ToolCallManager for tracking tool execution state (now via Messages)
    // Note: messages_ is used for tool call tracking via Messages::add_tool_calls etc.
    
    // Set up BashTool callback for build event tracking
    BashTool::set_build_callback([this](const std::string& command, bool success) {
        if (session_memory_) {
            session_memory_->on_build_complete(command, success);
        }
    });
    
    // Initialize memory tools with SessionMemory pointer
    ToolHandler::init_memory_tools(session_memory_.get());
    
    // §v0.5.0: load settings from SettingsStore (no is_loaded() check needed — SQLite is always ready)
    if (settings_store) {
        max_iterations_ = settings_store->get_max_agent_iterations();
        max_tool_result_size_ = settings_store->get_max_tool_result_size();
        max_request_size_ = settings_store->get_max_request_size();
        max_response_size_ = settings_store->get_max_response_size();

        // §BUG-007 fix: honour AppConfig::auto_mode (computed by AgentImpl,
        // the single authority) unconditionally. Previously this only ever
        // set the global flag ON from the settings store, so a host-forced
        // OFF was silently re-enabled when the LLMClient was constructed.
        ToolHandler::set_auto_mode(config_.auto_mode);
        
        // Initialize Messages with settings
        MessageCompressionConfig messages_config;
        // §Phase 8.5+: use the *selected* model's max_context_size, not the
        // settings.json default. Previously this used get_max_context_size()
        // which always returns the default model's context, causing the
        // Token Usage progress bar to show e.g. 500000 (m3's) when running
        // m2.7 (which actually has 200000).
        if (!config_.model_name.empty()) {
            messages_config.max_context_size =
                settings_store->get_max_context_size_for_model(config_.model_name);
        } else {
            messages_config.max_context_size =
                settings_store->get_max_context_size_for_model(config_.model);
        }
        messages_config.model = config_.model;
        messages_config.max_compress_output = 1500;
        // Compression timeout. First attempt is 30s, retries grow to 60s and 120s
        // (see Messages::call_llm_for_summary).
        messages_config.compression_timeout = 30;
        messages_config.debug_output = true; // Enable for debugging
        messages_config.api_url = config_.base_url + config_.chat_endpoint;
        messages_config.api_key = config_.api_key;
        // §Phase 8.7 / §BUG-COMPRESS-TEMP: the compression call posts its
        // own payload (Messages::call_llm_for_summary) and never passes
        // through run_agent_cycle(), so the provider plugin's temperature
        // clamp was skipped — models with a fixed temperature (e.g. Kimi
        // k2.7-code = 1.0) rejected the compression request outright.
        // Clamp here, same as the main chat path does.
        messages_config.temperature =
            plugin_->clamp_temperature(config_.model, config_.temperature);

        // §Phase 9.10: wire compression token callback so the main agent's
        // token bar stays accurate between streaming turns (SSE is always null).
        messages_config.on_compression_tokens =
            [this](int pt, int ct, int tt) {
                report_compression_token_usage(pt, ct, tt);
            };

        // §Phase 8.8 / §Phase 16: resolve the per-agent namespace dir.
        // Empty agent_name yields `<project>/.daedalus/hive-brain/`.
        messages_ = std::make_unique<Messages>(
            hive::resolve_agent_dir(config.target_directory, config.agent_name),
            messages_config);
    } else {
        // Settings not loaded - create Messages with defaults for compression functionality
        MessageCompressionConfig messages_config;
        messages_config.max_context_size = 200000;
        messages_config.debug_output = true;
        messages_config.temperature =  // §BUG-COMPRESS-TEMP: clamp like the main path
            plugin_->clamp_temperature(config_.model, config_.temperature);
        messages_config.on_compression_tokens =
            [this](int pt, int ct, int tt) {
                report_compression_token_usage(pt, ct, tt);
            };
        messages_ = std::make_unique<Messages>(
            hive::resolve_agent_dir(config.target_directory, config.agent_name),
            messages_config);
    }

    // §18/§18.9 — at startup, refresh the MCP tool surface from the
    // configured servers (if any). Failures are isolated per server by
    // the Hub's circuit breaker — we never abort startup because of MCP.
    refresh_mcp_tools();

    // §18.9.C — soft-cap warning. If the combined tool schema exceeds
    // mcp.tool_schema_soft_cap_percent of the configured context window,
    // log a warning so the user can prune servers.json. Default 5% is
    // a conservative bound; well-tuned configs sit well below.
    // §Phase 11: guard with mcp_manager_ (nullptr when mcp_enabled=false).
    // §v0.5.0: soft-cap check uses settings_store_ (nullptr is safe — check guards it)
    if (mcp_manager_ && settings_store_) {
        const size_t est_tokens = mcp_manager_->estimated_token_count();
        if (est_tokens > 0) {
            const int cap_pct = settings_store_->get_mcp_tool_schema_soft_cap_percent();
            const size_t max_ctx = static_cast<size_t>(get_max_context_size());
            const size_t cap_tokens = (max_ctx * static_cast<size_t>(cap_pct)) / 100;
            if (est_tokens > cap_tokens) {
                std::ostringstream w;
                w << "[MCP] WARNING: tool schemas consume ~"
                  << est_tokens << " tokens ("
                  << cap_pct << "% cap is "
                  << cap_tokens << "). Consider pruning servers.json.";
                if (sink_) sink_->on_log(hive::LogLevel::Warn, w.str());
                else LOG_WARNING(w.str());
            }
        }
    }
}

void LLMClient::refresh_mcp_tools() {
    // §18.9 — refresh via the singleton ToolFactory (which lives in
    // tools/ToolFactory.cpp). The factory iterates McpServerManager and
    // registers one McpProxiedTool per discovered tool. We deliberately
    // keep the LLMClient thin: no transport knowledge.
    auto factory = ToolHandler::get_tool_factory();
    if (factory) factory->refresh_mcp_tools();
    mcp_tools_refresh_pending_ = false;
}

// §MODEL-SWITCH: reconfigure API endpoint/credentials and the Messages
// compression config when the host switches models.
void LLMClient::set_config(const AppConfig& config) {
    config_ = config;
    // §ProviderPlugins: a model switch may cross providers — re-resolve.
    plugin_ = resolve_provider_plugin(config_);
    LOG_INFO(std::string("[ProviderPlugin] active plugin: ") + plugin_->name() +
             " (provider='" + config_.provider + "', model='" + config_.model + "')");
    if (!messages_) return;

    MessageCompressionConfig mc = messages_->get_config();
    if (settings_store_) {
        mc.max_context_size = settings_store_->get_max_context_size_for_model(
            config_.model_name.empty() ? config_.model : config_.model_name);
    }
    mc.model        = config_.model;
    mc.api_url      = config_.base_url + config_.chat_endpoint;
    mc.api_key      = config_.api_key;
    // §BUG-COMPRESS-TEMP: clamp through the (possibly new) provider plugin,
    // same as the main chat path — a model switch can change the rule.
    mc.temperature  = plugin_->clamp_temperature(config_.model, config_.temperature);
    messages_->set_config(mc);
}

void LLMClient::clear_messages() {
    if (!messages_) return;
    messages_->clear();
    messages_->save();
    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "[MODEL] Message history cleared");
    }
}

// §Phase 5.1: ESCKeyDetector removed. The library no longer polls
// stdin for ESC bytes. Hosts that want ESC-cancel capture it in
// their own thread and call Agent::cancel() / LLMClient::cancel().

std::future<LLMClient::ApiResponse> LLMClient::make_api_request_async(
    const std::string& url,
    const std::string& payload,
    std::stop_token stop_token,
    std::function<void(std::string_view)> streaming_callback
) {
    using hive::net::HttpRequest;
    using hive::net::HttpSession;
    using hive::net::HttpError;
    using hive::net::to_string;

    // Get server error settings
    hive::ServerErrorSettings server_error_settings;
    try {
        if (settings_store_) {
            server_error_settings = settings_store_->get_server_error_settings();
        }
    } catch (...) {
        server_error_settings.enabled = true;
        server_error_settings.initial_wait_seconds = 30;
        server_error_settings.max_wait_seconds = 300;
        server_error_settings.backoff_multiplier = 2.0;
        server_error_settings.max_retries_per_call = 3;
    }

    // Capture sink for error routing from the worker thread.
    std::weak_ptr<hive::AgentSink> weak_sink = sink_;
    // Plain log line (kept for notices that are not structured errors).
    auto log_err = [weak_sink](std::string msg) {
        if (auto s = weak_sink.lock()) s->on_log(hive::LogLevel::Error, msg);
        else LOG_ERROR(msg);
    };
    // §on_error: structured error report. The default AgentSink::on_error
    // forwards to on_log(Error), so hosts see the same output as before.
    // Every error is also file-logged unconditionally — hosts that override
    // on_error/on_log for console rendering still get a project log trace.
    auto report_err = [weak_sink](hive::ErrorKind kind, std::string msg, bool will_retry) {
        LOG_ERROR("[" + std::string(hive::to_string(kind)) + "] " + msg
                  + (will_retry ? " (will_retry=true)" : " (will_retry=false)"));
        if (auto s = weak_sink.lock()) s->on_error(kind, msg, will_retry);
    };

    // Submit the entire retry loop to the ThreadPool so it doesn't block
    // the calling thread.
    return hive::concurrent::ThreadPool::get_instance().submit(
        hive::concurrent::TaskPriority::HIGH,
        [this, url, payload, stop_token, server_error_settings,
         streaming_callback, weak_sink, log_err, report_err]() mutable -> ApiResponse {
            int max_retries = server_error_settings.max_retries_per_call;
            int wait_time = server_error_settings.initial_wait_seconds;
            bool last_attempt_was_transport = false;  // §B14 fix
            ApiResponse last_response;

            for (int attempt = 1; attempt <= max_retries; ++attempt) {
                if (stop_token.stop_requested()) {
                    throw std::runtime_error("API request cancelled");
                }

                HttpRequest req;
                req.url = url;
                req.method = "POST";
                req.body = payload;
                req.headers.push_back("Content-Type: application/json");
                if (streaming_callback) {
                    req.headers.push_back("Accept: text/event-stream");
                    // Streaming: pass per-chunk callback to HttpSession.
                    // Guard against stop_requested() so in-flight chunks from the
                    // kernel buffer don't fire one more on_chunk after cancellation.
                    auto safe_callback = [&streaming_callback,
                                          &stop_token](std::string_view chunk) {
                        if (stop_token.stop_requested()) return;
                        streaming_callback(chunk);
                    };
                    req.stream_callback = safe_callback;
                } else {
                    req.headers.push_back("Accept: application/json");
                }
                if (!config_.api_key.empty()) {
                    req.bearer_token = config_.api_key;
                }
                req.timeout_seconds = 900;  // 15 minutes
                req.follow_redirects = true;
                req.user_agent = config_.user_agent;  // §UA

                // Submit via HttpSession (async, non-blocking)
                auto fut = HttpSession::get_instance().submit_cancellable(req, stop_token);
                auto result = fut.get();

                if (result) {
                    last_response.status_code = result->status_code;
                    last_response.body        = std::move(result->body);
                    long code = last_response.status_code;
                    int  retry_after = result->retry_after_seconds;
                    if (code >= 200 && code < 300) {
                        // Non-streaming responses have no per-chunk callback,
                        // so fire the response-started hook here on the HTTP
                        // worker thread (consistent with streaming deltas).
                        if (!streaming_callback) {
                            if (auto s = weak_sink.lock()) s->on_llm_response_started();
                        }
                        return last_response;  // Success
                    } else if (code == 401) {
                        // Auth failure: never retry. The body (now preserved
                        // even in streaming mode) is returned so the
                        // downstream status-first handler can surface the
                        // provider's structured error via on_error — here we
                        // only log (avoids a duplicate structured event).
                        log_err("[API] HTTP " + std::to_string(code) + " (auth failed). "
                                "Response body (first 500 chars): " + last_response.body.substr(0, 500));
                        return last_response;
                    } else if (code == 429) {
                        // §B14 fix: rate-limited. Honour Retry-After if present;
                        // otherwise fall through to the exponential-backoff path.
                        report_err(hive::ErrorKind::RateLimited,
                                   "[API] HTTP 429 (rate limited)",
                                   attempt < max_retries && server_error_settings.enabled);
                        if (retry_after > 0) {
                            wait_time = retry_after;
                            log_err("[API] Honoring Retry-After: " +
                                    std::to_string(retry_after) + "s");
                        }
                        // Continue to retry block below.
                    } else if (code >= 400 && code < 500) {
                        // 4xx (other than 401/429): client error, no point retrying.
                        // Same as 401: body flows downstream for the structured
                        // on_error; log only here.
                        log_err("[API] HTTP " + std::to_string(code) + " (client error). "
                                "Response body (first 500 chars): " + last_response.body.substr(0, 500));
                        return last_response;
                    } else {
                        report_err(hive::ErrorKind::ServerError,
                                   "[API] HTTP " + std::to_string(code) + " (server error)",
                                   attempt < max_retries && server_error_settings.enabled);
                    }
                } else {
                    HttpError err = result.error();
                    if (err == HttpError::Cancelled) {
                        throw std::runtime_error("API request cancelled");
                    } else if (err == HttpError::Timeout) {
                        // §B14: transport errors always retry (subject to
                        // max_retries), hence will_retry = attempt < max.
                        report_err(hive::ErrorKind::Timeout,
                                   "[API] request timed out (attempt " + std::to_string(attempt) + ")",
                                   attempt < max_retries);
                    } else {
                        report_err(hive::ErrorKind::Network,
                                   "[API] request failed (attempt " + std::to_string(attempt) + "): " + to_string(err),
                                   attempt < max_retries);
                    }
                    // §B14 fix: transport errors (timeout / network) should retry
                    // regardless of server_error_settings.enabled — a single blip
                    // shouldn't drop the whole turn. The setting gates HTTP 5xx / 429.
                    last_attempt_was_transport = true;
                }

                // Wait before retry (exponential backoff, cancellable)
                // §B14 fix: split retry gate. Transport errors retry always
                // (subject to max_retries + stop_token). HTTP 5xx / 429 retry
                // only when server_error_settings.enabled is true.
                const bool is_transport_retry = last_attempt_was_transport;
                const bool is_http_retry = !last_attempt_was_transport;
                const bool http_retry_allowed =
                    is_http_retry && server_error_settings.enabled;
                const bool should_retry =
                    attempt < max_retries &&
                    !stop_token.stop_requested() &&
                    (is_transport_retry || http_retry_allowed);
                if (should_retry) {
                    std::ostringstream w;
                    w << "Waiting " << wait_time << "s before retry...";
                    if (auto s = weak_sink.lock()) s->on_log(hive::LogLevel::Info, w.str());
                    for (int i = 0; i < wait_time * 10; ++i) {
                        if (stop_token.stop_requested()) {
                            throw std::runtime_error("API request cancelled during retry wait");
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    wait_time = std::min(
                        static_cast<int>(wait_time * server_error_settings.backoff_multiplier),
                        server_error_settings.max_wait_seconds
                    );
                    last_attempt_was_transport = false;  // reset for next attempt
                }
            }

            // Retries exhausted (or loop otherwise ended without success).
            // If the server gave us an HTTP response at all (e.g. a 5xx body
            // after the final attempt), return it so the caller can handle
            // the status; otherwise this was a pure transport failure.
            if (last_response.status_code != 0) return last_response;
            throw std::runtime_error("API request failed after " +
                                     std::to_string(max_retries) + " attempts");
        });
}

// §NC-8 (2026-07-16): make_http_request_with_retry rewritten on
// OmniNet::HTTP::Client. The CURL* parameter is dropped — callers
// no longer need to manage a curl handle.
//
// Retry policy is preserved:
//   - 2xx → return body
//   - 401 → log + return body (no retry, auth won't fix itself)
//   - 4xx (other) → log + return body (no retry, client error)
//   - 5xx → log + exponential backoff via server_error_settings
//   - Transport error → log + exponential backoff (if enabled)
//
// §robust-client: returns ApiResponse (status + body) so callers can key
// error handling on the HTTP status instead of re-parsing error bodies.
LLMClient::ApiResponse LLMClient::make_http_request_with_retry(
    const std::string& url, const std::string& payload, int max_retries)
{
    ApiResponse response;

    // Get server error settings for configurable retry behavior
    hive::ServerErrorSettings server_error_settings;
    try {
        if (settings_store_) {
            server_error_settings = settings_store_->get_server_error_settings();
            max_retries = server_error_settings.max_retries_per_call;
        }
    } catch (...) {
        // Use defaults if settings not available
        server_error_settings.enabled = true;
        server_error_settings.initial_wait_seconds = 30;
        server_error_settings.max_wait_seconds = 300;
        server_error_settings.backoff_multiplier = 2.0;
        server_error_settings.max_retries_per_call = 3;
    }

    int attempt = 0;
    int wait_time = server_error_settings.initial_wait_seconds;

    while (attempt < max_retries) {
        attempt++;

        response.body.clear();

        // Build the OmniNet request.
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method("POST");
        req.body(payload);
        req.header("Content-Type", "application/json");
        req.header("Accept",       "application/json");
        if (!config_.api_key.empty()) {
            // OmniNet::HTTP::ClientRequest::header() expects JUST the
            // header value (the library prepends "name: " itself).
            // Previously this passed "Authorization: Bearer XXX" as
            // the value, producing an invalid HTTP header
            // ("Authorization: Authorization: Bearer XXX") and
            // triggering MiniMax's `authorized_error` (401).
            req.header("Authorization", "Bearer " + config_.api_key);
        }

        OmniNet::HTTP::ClientConfig cfg;
        cfg.user_agent       = config_.user_agent;  // §UA
        cfg.connect_timeout = std::chrono::seconds(10);
        cfg.request_timeout = std::chrono::seconds(900);  // 15 minutes
        cfg.follow_redirects = true;
        cfg.verify_tls       = true;

        // Execute synchronously (this is the sync fallback path used
        // when SSE parsing fails).
        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

        if (result.has_value()) {
            const auto& resp = result.value();
            response.body = resp.bodyAsString();
            // Enforce response size cap (matches old WriteBuffer::max_size).
            if (response.body.size() > max_response_size_) {
                response.body.resize(max_response_size_);
                std::ostringstream trunc;
                trunc << "Response was truncated to " << max_response_size_ << " bytes";
                if (sink_) sink_->on_log(hive::LogLevel::Warn, trunc.str());
            }

            const long code = resp.statusCode();
            response.status_code = code;
            if (code >= 200 && code < 300) {
                // Sync fallback path: the response body is now available,
                // so the LLM has "started responding" from the host's POV.
                if (sink_) sink_->on_llm_response_started();
                break; // Success
            } else if (code == 401) {
                // Don't retry on authentication errors - they won't succeed
                if (sink_) {
                    std::ostringstream e401;
                    e401 << "HTTP " << code
                         << " (authentication failed, not retrying)\n"
                         << "Response body: " << response.body;
                    sink_->on_log(hive::LogLevel::Error, e401.str());
                } else {
                    LOG_ERROR("HTTP error " + std::to_string(code)
                              + " (authentication failed, not retrying). "
                              + "Response body: " + response.body);
                }
                break;
            } else if (code >= 400 && code < 500) {
                // Client error - no point retrying
                if (sink_) {
                    std::ostringstream e4xx;
                    e4xx << "HTTP " << code
                         << " (client error, not retrying)\n"
                         << "Response body: " << response.body;
                    sink_->on_log(hive::LogLevel::Error, e4xx.str());
                } else {
                    LOG_ERROR("HTTP error " + std::to_string(code)
                              + " (client error, not retrying). "
                              + "Response body: " + response.body);
                }
                break;
            } else if (code >= 500) {
                // Server error - retry with exponential backoff if enabled.
                // §on_error: structured report (default sink forwards to
                // on_log(Error), matching the previous Warn visibility).
                std::ostringstream e5xx;
                e5xx << "HTTP " << code << " (server error";
                if (server_error_settings.enabled) {
                    e5xx << ", retrying in " << wait_time << "s";
                }
                e5xx << ")\nResponse body (first 500 chars): "
                      << response.body.substr(0, 500);
                emit_error(hive::ErrorKind::ServerError, e5xx.str(),
                           server_error_settings.enabled && attempt < max_retries);
            }
        } else {
            // Transport-level failure (OmniNet::HTTP::ClientError).
            const char* err_name = OmniNet::HTTP::clientErrorName(result.error());
            std::ostringstream msg;
            msg << "transport error on attempt " << attempt
                << ": " << err_name;
            if (server_error_settings.enabled) {
                msg << " (retrying in " << wait_time << "s)";
            }
            emit_error(hive::ErrorKind::Network, msg.str(),
                       server_error_settings.enabled && attempt < max_retries);
        }

        // If we're not done yet, wait before retrying with exponential backoff
        if (attempt < max_retries && server_error_settings.enabled) {
            std::ostringstream wait;
            wait << "Waiting " << wait_time << " seconds before retry...";
            if (sink_) sink_->on_log(hive::LogLevel::Info, wait.str());
            // §logging: per-attempt detail at DEBUG (below default INFO).
            LOG_DEBUG("[API] attempt " + std::to_string(attempt) + "/"
                      + std::to_string(max_retries) + " failed; backoff "
                      + std::to_string(wait_time) + "s before next attempt");
            std::this_thread::sleep_for(std::chrono::seconds(wait_time));
            // Exponential backoff: multiply by backoff_multiplier, cap at max_wait
            wait_time = std::min(static_cast<int>(wait_time * server_error_settings.backoff_multiplier),
                                 server_error_settings.max_wait_seconds);
        }
    }

    if (attempt == max_retries && response.status_code != 0) {
        // We exhausted retries but still got an HTTP response (e.g. a 5xx
        // body on the final attempt) — return it so the caller can handle
        // the status.
        return response;
    } else if (attempt == max_retries) {
        // Note: no terminal emit_error here — the final attempt already
        // reported on_error(will_retry=false) above.
        throw std::runtime_error("API request failed after "
                                 + std::to_string(max_retries) + " attempts");
    }

    // Extract and store token usage (via shared helper)
    try {
        update_token_usage(json::parse(response.body));
    } catch (...) {
        // Ignore parsing errors - usage is best-effort
    }

    return response;
}



// Extract and store token usage from a parsed LLM response.
// This MUST be called after every successful API response so the
// last_total_tokens_ / last_prompt_tokens_ / last_completion_tokens_
// members are up to date for:
//   1. The token usage bar in the status display
//   2. The 90% compression threshold check
//   3. The session memory stats
//   4. The metrics tracked by Metrics class
//
// Without this, the user sees "0/1000000 (0.0%)" even though the LLM
// is responding - because we never capture the usage field from responses.
void LLMClient::update_token_usage(const json& response_json) {
    // §Phase 9.10: MiniMax SSE API returns "usage": null on every chunk
    // (confirmed by /tmp/test_sse_raw.py). The non-streaming path gets real
    // counts. Log a warning so operators know why token counts are 0 during
    // SSE streaming — this is expected behaviour, not a bug.
    // §B32: suppress duplicate warnings per turn (multiple SSE chunks without usage).
    if (!response_json.contains("usage")) {
        if (!warned_no_usage_this_turn_) {
            if (sink_) sink_->on_log(hive::LogLevel::Warn,
                "[LLM] No 'usage' field in response (SSE stream? — token counts are 0)");
            warned_no_usage_this_turn_ = true;
        }
        return;
    }
    if (response_json["usage"].is_null()) {
        if (!warned_no_usage_this_turn_) {
            if (sink_) sink_->on_log(hive::LogLevel::Warn,
                "[LLM] 'usage' is null in SSE stream — token counts unavailable");
            warned_no_usage_this_turn_ = true;
        }
        return;
    }
    try {
        const auto& usage = response_json["usage"];
        last_prompt_tokens_ = usage.value("prompt_tokens", 0);
        last_completion_tokens_ = usage.value("completion_tokens", 0);
        last_total_tokens_ = usage.value("total_tokens", 0);

        // §ContextCache: provider prompt-cache hit count (both wire
        // shapes folded in hive::context_status::extract_cached_tokens).
        last_cached_tokens_ = hive::context_status::extract_cached_tokens(usage);
        if (last_cached_tokens_ > 0) {
            LOG_INFO("[LLM] prompt cache hit: " +
                     std::to_string(last_cached_tokens_) + "/" +
                     std::to_string(last_prompt_tokens_) + " prompt tokens cached");
        }
    } catch (...) {
        // Ignore parsing errors - usage is best-effort
    }
}

// ===================================================================
// §Phase 3 - MiniMax SSE streaming parser.
//
// `MiniMaxSseParser` is declared in `src/api/MiniMaxSseParser.h`
// (private header, exposed for unit tests). This translation unit
// provides its method bodies.
//
// MiniMax SSE line format:
//   data: [{"id":"...","choices":[{"delta":{"content":"..."},"index":0}]}]
//   data: [DONE]
// ===================================================================
void MiniMaxSseParser::on_chunk(std::string_view data) {
    buffer_.append(data.data(), data.size());
    size_t pos = 0;
    while ((pos = buffer_.find('\n', pos)) != std::string::npos) {
        std::string line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        pos = 0;
        parse_line(line);
    }
}

void MiniMaxSseParser::finish() {
    if (!buffer_.empty()) {
        parse_line(buffer_);
        buffer_.clear();
    }
    // §ThinkTagFilter: flush bytes held for tag disambiguation. Literal
    // text is re-emitted; an unterminated think block's tail is dropped.
    const std::string tail = think_filter_.flush();
    if (!tail.empty()) {
        accumulated_text_ += tail;
        if (auto w = weak_sink_.lock()) w->on_assistant_delta(tail);
    }
}

nlohmann::json MiniMaxSseParser::synthesized_message() const {
    nlohmann::json m;
    m["role"] = accumulated_role_.empty() ? "assistant" : accumulated_role_;
    m["content"] = accumulated_text_;
    m["audio_content"] = "";
    if (!accumulated_tool_calls_.empty()) {
        m["tool_calls"] = accumulated_tool_calls_;
    }
    // §ProviderPlugins Phase 4: preserve thinking text for providers
    // (Kimi) that require reasoning_content back in the history during
    // multi-step tool calling.
    if (!accumulated_reasoning_.empty()) {
        m["reasoning_content"] = accumulated_reasoning_;
    }
    if (!accumulated_finish_reason_.empty()) {
        m["finish_reason"] = accumulated_finish_reason_;
    }
    return m;
}

void MiniMaxSseParser::parse_line(const std::string& line) {
    constexpr std::string_view PREFIX = "data: ";
    if (line.rfind(PREFIX, 0) != 0) return;
    std::string json_str = line.substr(PREFIX.size());
    if (json_str == "[DONE]" || json_str == "DONE") return;
    try {
        nlohmann::json parsed = nlohmann::json::parse(json_str);

        // Normalise into a list of top-level objects.
        // MiniMax streams: `data: {"id":"...","choices":[{...delta...}]}` (one object).
        // Some OpenAI-compatible endpoints stream: `data: [{...}]` (array of one).
        // We must support both.  If `parsed` is already an array, iterate it.
        // If it's an object, treat it as a one-element list.
        // If neither (string, number, etc.), skip silently.
        std::vector<nlohmann::json> items;
        if (parsed.is_array()) {
            for (const auto& el : parsed) {
                if (el.is_object()) items.push_back(el);
            }
        } else if (parsed.is_object()) {
            items.push_back(parsed);
        }
        // Anything else (string, primitive) -- skip.

        for (const auto& item : items) {
            // Guard: item must be a JSON object (not an array or primitive).
            if (!item.is_object()) continue;

            // Capture the complete item for final_event_ -- it carries the
            // final `usage` (token counts) and `id`/`model` metadata that
            // aren't present on every chunk.
            final_event_ = item;

            for (const auto& choice : item.value("choices", nlohmann::json::array())) {
                // §fix (2026-07-18): a non-object choice (e.g.
                // `data: {"choices":[null]}`) made value("delta", ...)
                // throw json::type_error, which escaped the old
                // parse_error-only catch onto HttpSession's detached
                // streaming thread → std::terminate.
                if (!choice.is_object()) continue;
                const auto& delta = choice.value("delta", nlohmann::json::object());

                // Role (typically "assistant"; MiniMax also puts name here).
                if (delta.contains("role") && delta["role"].is_string()) {
                    accumulated_role_ = delta["role"].get<std::string>();
                }

                // §ProviderPlugins Phase 4: thinking text. Kimi/Moonshot
                // streams `reasoning_content` string fragments alongside
                // `content`; accumulate them for the synthesized message
                // (Kimi rejects tool-loop histories without it). Inert
                // for providers that never send the field.
                const nlohmann::json& reasoning =
                    delta.value("reasoning_content", nlohmann::json());
                if (reasoning.is_string() && !reasoning.get<std::string>().empty()) {
                    accumulated_reasoning_ += reasoning.get_ref<const std::string&>();
                }

                // Text content streaming — the basic case.
                // §ThinkTagFilter: feed deltas through the think-block
                // stripper so inline <think>...</think> never reaches the
                // sink or history (tags split across chunks are held).
                const nlohmann::json& content = delta.value("content", nlohmann::json());
                if (content.is_string() && !content.get<std::string>().empty()) {
                    const std::string s =
                        think_filter_.feed(content.get_ref<const std::string&>());
                    if (!s.empty()) {
                        accumulated_text_ += s;
                        if (auto w = weak_sink_.lock()) w->on_assistant_delta(s);
                    }
                } else if (content.is_array()) {
                    // MiniMax sometimes sends content as an array of parts.
                    for (const auto& part : content) {
                        if (part.is_string()) {
                            const std::string s =
                                think_filter_.feed(part.get_ref<const std::string&>());
                            if (!s.empty()) {
                                accumulated_text_ += s;
                                if (auto w = weak_sink_.lock()) w->on_assistant_delta(s);
                            }
                        } else if (part.is_object() && part.value("type", "") == "text") {
                            const std::string& raw = part.value("text", std::string{});
                            const std::string txt = think_filter_.feed(raw);
                            if (!txt.empty()) {
                                accumulated_text_ += txt;
                                if (auto w = weak_sink_.lock()) w->on_assistant_delta(txt);
                            }
                        }
                    }
                }

                // Tool calls — the part Phase 9.8 missed.
                // MiniMax streams the whole tool_call object in one chunk
                // (id, name, complete arguments JSON string).
                // OpenAI spec streams arguments char-by-char; we handle
                // both by keying on `index` for accumulation.
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                    for (const auto& tc : delta["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        int tc_index = tc.value("index",
                                                static_cast<int>(accumulated_tool_calls_.size()));
                        // Grow the list to fit this index.
                        while (static_cast<int>(accumulated_tool_calls_.size()) <= tc_index) {
                            accumulated_tool_calls_.push_back(nlohmann::json::object());
                        }
                        auto& slot = accumulated_tool_calls_[tc_index];
                        // Copy known fields onto the slot.  Streaming
                        // arguments arrive as fragments -- always concatenate
                        // strings rather than overwriting.
                        if (tc.contains("id") && tc["id"].is_string()) {
                            slot["id"] = tc["id"];
                        } else if (!slot.contains("id")) {
                            slot["id"] = "";
                        }
                        if (tc.contains("type") && tc["type"].is_string()) {
                            slot["type"] = tc["type"];
                        } else if (!slot.contains("type")) {
                            slot["type"] = "function";
                        }
                        if (tc.contains("function") && tc["function"].is_object()) {
                            if (!slot.contains("function")) slot["function"] = nlohmann::json::object();
                            if (tc["function"].contains("name") && tc["function"]["name"].is_string()) {
                                slot["function"]["name"] = tc["function"]["name"];
                            } else if (!slot["function"].contains("name")) {
                                slot["function"]["name"] = "";
                            }
                            if (tc["function"].contains("arguments")) {
                                if (tc["function"]["arguments"].is_string()) {
                                    // Streaming fragment -- append.
                                    std::string prev = slot["function"].value("arguments", std::string{});
                                    slot["function"]["arguments"] =
                                        prev + tc["function"]["arguments"].get<std::string>();
                                } else if (tc["function"]["arguments"].is_object()) {
                                    // Already-parsed object (rare).  Keep as JSON string.
                                    slot["function"]["arguments"] = tc["function"]["arguments"].dump();
                                }
                            } else if (!slot["function"].contains("arguments")) {
                                slot["function"]["arguments"] = "";
                            }
                        } else if (!slot.contains("function")) {
                            slot["function"] = nlohmann::json::object();
                        }
                    }
                }

                // finish_reason comes on the LAST chunk for tool_calls /
                // stop decisions. Capture it once.
                if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                    if (choice["finish_reason"].is_string()) {
                        accumulated_finish_reason_ = choice["finish_reason"].get<std::string>();
                    }
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        // §fix (2026-07-18): was parse_error only — a json::type_error
        // (e.g. value() on a non-object member) escaped onto HttpSession's
        // detached streaming thread → std::terminate. Malformed SSE
        // line -- skip silently.
    }
}

// // Helper method to get state name as string
std::string LLMClient::get_state_name(AgentState state) const {
    switch (state) {
        case AgentState::UNDERSTAND: return "UNDERSTAND";
        case AgentState::PLANNING: return "PLANNING";
        case AgentState::ACT: return "ACT";
        case AgentState::BUILD_TEST: return "BUILD_TEST";
        case AgentState::DONE: return "DONE";
        case AgentState::CANCELLED: return "CANCELLED";
        default: return "UNKNOWN";
    }
}

// §State Machine: handle a `set_status` tool result carrying a state
// transition. Extracted from run_agent_cycle() so it is unit-testable.
bool LLMClient::handle_set_state_result(const json& result) {
    if (!result.is_object()
        || result.value("status", "") != "set_state"
        || !result.contains("state")
        || !result["state"].is_string()) {
        return false;
    }
    bool ok = false;
    auto new_state = hive::agent_state_from(
        result["state"].get<std::string>(), &ok);
    if (!ok) return false;

    if (state_change_cb_) {
        // Normal path: the owning AgentImpl updates its atomic public
        // state and fires AgentSink::on_state_change with the correct
        // prev — Agent::state() stays live during the turn.
        state_change_cb_(new_state);
    } else if (sink_) {
        // Fallback for direct LLMClient users (no AgentImpl above us):
        // preserve the old direct-sink behaviour with locally tracked prev.
        int current = static_cast<int>(new_state);
        sink_->on_state_change(fallback_prev_public_state_, current);
        fallback_prev_public_state_ = current;
    }
    LOG_INFO("State machine: set_status → "
             + std::string(hive::to_string(new_state)));
    return true;
}

// §COMPRESS-TOOL (2026-08-14): handle a `compress_context` tool result.
// The model-facing tool was removed on 2026-08-01 because the agent never
// called it; it is back by request. The tool cannot compress inline — the
// run loop holds a reference to the live message array across the cycle —
// so this sets the same compress_requested_ flag as the host-facing
// compress_context() and the cycle-boundary checkpoint (§AutoCompress)
// does the work: same code path as the 90% threshold trigger, safe
// mid-turn, result reported via on_compaction().
bool LLMClient::handle_compress_context_result(const json& result) {
    if (!result.is_object()
        || result.value("status", "") != "compress_context") {
        return false;
    }
    const std::string reason = result.value("reason", "");
    compress_requested_.store(true, std::memory_order_relaxed);
    LOG_INFO("[COMPRESS] Agent requested context compression"
             + (reason.empty() ? std::string(".")
                               : ": " + reason));
    return true;
}

// §on_error: structured error routing to the host sink. Every error is also
// written to the file Logger unconditionally, so hosts that override on_error
// / on_log (console rendering) still leave a trace in the project log file.
void LLMClient::emit_error(hive::ErrorKind kind, const std::string& message, bool will_retry) {
    LOG_ERROR("[" + std::string(hive::to_string(kind)) + "] " + message
              + (will_retry ? " (will_retry=true)" : " (will_retry=false)"));
    if (sink_) {
        sink_->on_error(kind, message, will_retry);
    }
}

// §fix (2026-07-30, hardened 2026-08-01): circuit breaker for repeated
// FATAL API errors. Counts ANY consecutive fatal-classified cycle — not just
// identical signatures: alternating 400 shapes (each recovery injection can
// corrupt the history into the next shape) defeat a same-signature check.
bool LLMClient::fatal_breaker_should_end_turn(const std::string& signature) {
    static constexpr int kMaxConsecutiveFatal = 2;
    consecutive_fatal_count_++;
    if (signature != last_fatal_signature_) {
        LOG_DEBUG("Fatal signature changed: [" + last_fatal_signature_ + "] -> [" + signature + "]");
        last_fatal_signature_ = signature;
    }
    if (consecutive_fatal_count_ >= kMaxConsecutiveFatal) {
        LOG_ERROR("Fatal API errors repeated " +
                  std::to_string(consecutive_fatal_count_) +
                  " times consecutively - ending turn instead of looping.");
        return true;
    }
    return false;
}

// §IdleExitBreaker (2026-08-20): circuit breaker for set_status-only
// iterations. A finished model used to call set_status("Idle"/"Done")
// expecting the agent loop to exit — it doesn't; only the `stop` tool
// ends a turn. The model then answers each re-invocation with yet
// another set_status and the turn loops forever (observed in the wild:
// the host re-sending only the auto context-status block, the model
// replying set_status("Done") every iteration). Threshold 2 mirrors the
// fatal breaker: one status-only iteration is a legitimate UI update,
// two in a row means the model is done and forgot how to say so.
bool LLMClient::status_only_breaker_should_end_turn() {
    static constexpr int kMaxConsecutiveStatusOnly = 2;
    ++consecutive_status_only_count_;
    if (consecutive_status_only_count_ >= kMaxConsecutiveStatusOnly) {
        LOG_ERROR("set_status-only iterations repeated " +
                  std::to_string(consecutive_status_only_count_) +
                  " times consecutively - model is trying to exit the "
                  "loop; ending turn instead of looping. "
                  "(To end a turn explicitly, call the stop tool.)");
        return true;
    }
    return false;
}

// Helper method to extract error message from raw API response
std::string LLMClient::extract_error_message(const std::string& response) const {
    // Try to find JSON error object in the response
    try {
        json j = json::parse(response);
        if (j.contains("error") && j["error"].is_object()) {
            return j["error"].value("message", "Unknown error");
        }
    } catch (...) {
        // If JSON parsing fails, try to extract from raw text
        size_t msg_pos = response.find("\"message\":");
        if (msg_pos != std::string::npos) {
            size_t start = response.find("\"", msg_pos + 9);
            size_t end = response.find("\"", start + 1);
            if (start != std::string::npos && end != std::string::npos) {
                return response.substr(start + 1, end - start - 1);
            }
        }
    }
    return "";
}

// §READ-IMAGE-CORE: consult the active plugin's static model table for the
// vision flag. Case-insensitive because the store key ("kimi-k3") and the
// wire id may differ in case. Models absent from the table (custom /
// freshly released) are assumed vision-capable — the provider API is the
// final arbiter, and the fatal breaker bounds any 400 loop.
bool LLMClient::model_supports_vision() const {
    if (!plugin_) return true;
    for (const auto& m : plugin_->known_models()) {
        const std::string& id = m.model_id;
        if (id.size() == config_.model.size() &&
            std::equal(id.begin(), id.end(), config_.model.begin(),
                       [](char x, char y) {
                           return std::tolower(static_cast<unsigned char>(x)) ==
                                  std::tolower(static_cast<unsigned char>(y));
                       })) {
            return m.supports_vision;
        }
    }
    return true;  // unknown model — let the API decide
}

// §READ-IMAGE-CORE follow-up (2026-08-10): host attachment loading.
// Local paths go through ImageProcessor (decode + <=512px downscale for
// png/jpeg, passthrough for gif/bmp/webp); http(s) URLs are downloaded to
// base64 (Kimi rejects public URLs outright, so this is the only way they
// can work); data URIs pass through unchanged. Failures log and skip —
// a broken attachment must never abort the user's turn.
void LLMClient::queue_image_ref(const std::string& ref, const std::string& via) {
    std::string data_uri;
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) {
        data_uri = download_image_as_base64(ref);
    } else if (ref.rfind("data:", 0) == 0) {
        data_uri = ref;
    } else {
        data_uri = ImageProcessor::load_local_image(
            ref, config_.target_directory.string());
    }
    if (data_uri.empty()) {
        LOG_WARNING("image attachment could not be loaded: " + ref);
        if (sink_) sink_->on_log(hive::LogLevel::Warn,
            "Could not load image attachment: " + ref);
        return;
    }
    PendingImage pi;
    pi.path     = ref;
    pi.via      = via;
    pi.data_uri = std::move(data_uri);
    pending_images_.push_back(std::move(pi));
}

void LLMClient::queue_image_attachments(
        const std::vector<hive::ImageAttachment>& images) {
    for (const auto& img : images) {
        using Source = hive::ImageAttachment::Source;
        if (img.source == Source::Bytes) {
            if (img.bytes.empty() || img.mime_type.empty()) continue;
            PendingImage pi;
            pi.path = img.path_or_url.empty() ? "<bytes>" : img.path_or_url;
            pi.via  = "user attachment";
            pi.data_uri = "data:" + img.mime_type + ";base64," +
                          base64_encode(img.bytes.data(), img.bytes.size());
            pending_images_.push_back(std::move(pi));
        } else {
            // Path and Url share the ref loader.
            queue_image_ref(img.path_or_url, "user attachment");
        }
    }
}

// Run a single agent cycle (one LLM call + tool execution)
// §NC-8 (2026-07-16): CURL* parameter dropped. The sync fallback
// path now uses OmniNet::HTTP::Client internally.
AgentState LLMClient::run_agent_cycle(const std::string& api_url,
                                          json& messages, const json& tool_defs) {
    warned_no_usage_this_turn_ = false;  // §B32: reset per-turn

    // §ProviderPlugins: enforce provider conversation rules on the
    // history before it goes on the wire (default plugin: no-op).
    plugin_->sanitize_history(messages);

    // Build the request payload
    json request_payload = {
        {"model",     config_.model},
        {"messages",  messages},
        {"tools",     tool_defs},
        {"stream",    config_.stream},  // §Phase 9.8: true = SSE tokens, false = buffered JSON
        {"temperature", plugin_->clamp_temperature(config_.model, config_.temperature)}
    };

    // §ProviderPlugins: provider-specific request shaping. The default
    // OpenAiCompatiblePlugin adds stream_options.include_usage when
    // streaming (§Phase 9.10); provider plugins add their own parameters
    // here (thinking switches, cache keys, clamps — e.g. MiniMax's
    // reasoning_split, §BUG-MINIMAX-105).
    {
        hive::ProviderChatContext pctx;
        pctx.model_id    = config_.model;
        pctx.model_name  = config_.model_name;
        pctx.provider    = config_.provider;
        pctx.base_url    = config_.base_url;
        // §ContextCache: the prompt-cache key must be stable across a
        // session (Kimi docs: "session id ... if the session is exited
        // and later resumed, this value should remain the same"; Kimi
        // Code Plan *requires* it for good hit rates). Agent namespace
        // wins when set; otherwise fall back to the session id so the
        // default (unnamed) agent doesn't silently opt out.
        pctx.cache_key   = !config_.agent_name.empty()
                               ? config_.agent_name : config_.session_id;
        pctx.stream      = config_.stream;
        pctx.temperature = config_.temperature;
        pctx.thinking_level = config_.thinking_level;
        plugin_->mutate_chat_payload(request_payload, pctx);
    }

    // Validate JSON serialization before sending.
    // §BUG-UTF8-TRUNC: invalid UTF-8 in a message string (byte-level
    // truncation splitting a codepoint, binary tool output, etc.) must
    // NEVER stop the agent loop. Serialize with error_handler_t::replace
    // — bad bytes become U+FFFD and the request still goes out, so the
    // agent keeps working. A warning tells the host it happened.
    std::string payload_str;
    try {
        payload_str = request_payload.dump();
    } catch (const std::exception& e) {
        LOG_WARNING("Request payload held invalid UTF-8 (" + std::string(e.what())
                    + ") — reserializing with replacement characters and continuing.");
        if (sink_) sink_->on_log(hive::LogLevel::Warn,
            "⚠️ Invalid UTF-8 in the conversation (likely a truncated tool "
            "result) — offending bytes replaced, continuing the turn.");
        payload_str = request_payload.dump(-1, ' ', false,
                                           json::error_handler_t::replace);
    }
    // Verify payload can be parsed back (catch JSON corruption)
    try {
        json parsed_verify = json::parse(payload_str);
        if (!parsed_verify.contains("messages") || !parsed_verify["messages"].is_array()) {
            LOG_ERROR("JSON validation failed: messages array missing or invalid");
            emit_error(hive::ErrorKind::Internal,
                       "JSON validation failed: messages array missing or invalid",
                       /*will_retry=*/false);
            return AgentState::DONE;
        }
        
        // Validate each tool_call's arguments are valid JSON
        auto& msgs = parsed_verify["messages"];
        for (size_t i = 0; i < msgs.size(); i++) {
            auto& msg = msgs[i];
            if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
                for (size_t j = 0; j < msg["tool_calls"].size(); j++) {
                    auto& tc = msg["tool_calls"][j];
                    std::string tc_id = tc.value("id", "MISSING");
                    
                    if (tc.contains("function") && tc["function"].contains("arguments")) {
                        std::string args_str = tc["function"]["arguments"];
                        try {
                            // This will throw if args_str is not valid JSON
                            json args_parsed = json::parse(args_str);
                        } catch (const json::parse_error& e) {
                            LOG_ERROR("INVALID arguments JSON at msg[" + std::to_string(i) + "][" + std::to_string(j) + "] id=" + tc_id);
                            LOG_ERROR("args_len=" + std::to_string(args_str.length()) + " chars");
                            LOG_ERROR("parse error at char " + std::to_string(e.byte) + ": " + e.what());
                            
                            // Show the last 100 chars to see what went wrong
                            std::string last_100 = args_str.length() > 100 ? args_str.substr(args_str.length() - 100) : args_str;
                            LOG_ERROR("last 100 chars: " + last_100);
                            
                            // Mark this tool_call as having invalid arguments
                            // We'll add an error tool result instead of executing it
                            tc["_invalid_args"] = true;
                            tc["_parse_error"] = e.what();
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("JSON parse verification failed: " + std::string(e.what()));
        LOG_ERROR("Payload (first 500 chars): " + payload_str.substr(0, 500));
        emit_error(hive::ErrorKind::Internal,
                   "JSON parse verification failed: " + std::string(e.what()),
                   /*will_retry=*/false);
        return AgentState::DONE;
    }
    
    // Use Messages class to sanitize messages before API call
    // This validates tool_calls and removes broken ones, tracking state to prevent cascading errors
    // IMPORTANT: sanitize_message_chain returns a NEW json (by value), not a reference.
    // We must use set_messages to update Messages class, otherwise the local copy
    // shadows the reference and tool results never get synced back!
    json sanitized = messages_->sanitize_message_chain(messages);
    messages_->set_messages(sanitized);
    messages = messages_->get_messages();  // Re-get reference after update
    
    // Rebuild payload after sanitization
    // §BUG-UTF8-TRUNC: error_handler_t::replace — this dump previously
    // escaped run_one_turn as type_error.316 and killed the turn.
    request_payload["messages"] = messages;
    payload_str = request_payload.dump(-1, ' ', false,
                                       json::error_handler_t::replace);
    
    // Check for tool_calls in messages and verify their IDs are valid strings
    for (size_t i = 0; i < messages.size(); i++) {
        const auto& msg = messages[i];
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tc_id = tc.value("id", "");
                if (tc_id.empty() || tc_id.find('\0') != std::string::npos) {
                    LOG_ERROR("Invalid tool_call_id at message " + std::to_string(i) + ": empty or contains null");
                    emit_error(hive::ErrorKind::Internal,
                               "Invalid tool_call_id at message " + std::to_string(i) + ": empty or contains null",
                               /*will_retry=*/false);
                    return AgentState::DONE;
                }
                // Check for control characters that might break JSON
                for (char c : tc_id) {
                    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                        LOG_ERROR("Invalid control char in tool_call_id at message " + std::to_string(i));
                        emit_error(hive::ErrorKind::Internal,
                                   "Invalid control char in tool_call_id at message " + std::to_string(i),
                                   /*will_retry=*/false);
                        return AgentState::DONE;
                    }
                }
            }
        }
        if (msg.value("role", "") == "tool") {
            std::string tc_id = msg.value("tool_call_id", "");
            if (tc_id.empty() || tc_id.find('\0') != std::string::npos) {
                LOG_ERROR("Invalid tool_call_id in tool result at message " + std::to_string(i));
                emit_error(hive::ErrorKind::Internal,
                           "Invalid tool_call_id in tool result at message " + std::to_string(i),
                           /*will_retry=*/false);
                return AgentState::DONE;
            }
        }
    }

    // Check if request payload exceeds max size and truncate if needed
    messages_->truncate_if_needed(max_request_size_);

    // §fix (2026-07-30): re-sanitize after truncation. Truncation rewrites
    // the chain, so the payload must be rebuilt from a structurally valid
    // chain even if a truncation edge case ever drops a parent assistant.
    {
        json truncated_sanitized = messages_->sanitize_message_chain(messages);
        messages_->set_messages(truncated_sanitized);
        messages = messages_->get_messages();
    }

    // Rebuild payload after truncation
    request_payload["messages"] = messages;

    // §READ-IMAGE-CORE (2026-08-10): attach any images queued by the
    // read_image tool as a trailing user message with image_url parts —
    // the exact multipart shape Kimi/OpenAI vision endpoints expect
    // (content is a real JSON array, never a serialized string). The
    // message lives in the REQUEST PAYLOAD ONLY: stored history keeps
    // just the stripped text reference written above, so the base64
    // payload is paid (in tokens) exactly once instead of on every
    // subsequent turn.
    //
    // Gated on model_supports_vision(): for text-only models the image
    // is dropped with a log line rather than risking a provider 400.
    if (!pending_images_.empty()) {
        if (model_supports_vision()) {
            json content = json::array();
            std::string text_block;
            for (const auto& pi : pending_images_) {
                content.push_back({
                    {"type", "image_url"},
                    {"image_url", {{"url", pi.data_uri}}}
                });
                if (!text_block.empty()) text_block += "\n";
                text_block += "[Image attached via " + pi.via + ": " + pi.path + "]";
                if (!pi.prompt.empty()) {
                    text_block += "\n" + pi.prompt;
                }
            }
            content.push_back({{"type", "text"}, {"text", text_block}});

            request_payload["messages"].push_back({
                {"role", "user"},
                {"content", content}
            });
            LOG_INFO("read_image: attached " +
                     std::to_string(pending_images_.size()) +
                     " image(s) to this request");
        } else {
            LOG_WARNING("read_image: model '" + config_.model +
                        "' is not vision-capable; dropping " +
                        std::to_string(pending_images_.size()) +
                        " queued image(s)");
            if (sink_) sink_->on_log(hive::LogLevel::Warn,
                "read_image: current model does not support vision input; "
                "image was not sent.");
        }
        pending_images_.clear();
    }

    // Reset cancellation flag

    // Make the API call with retry logic - run in thread for ESC cancellation support
    ApiResponse api_response;
    std::string response_body;

    // Make the API call asynchronously via HttpSession
    // Benefits: HTTP keep-alive, connection reuse, true async I/O,
    // §Phase 5.1: the cancel source is a member of LLMClient
    // (cancel_source_) so Agent::cancel() can fire it from any
    // thread. Polled below in the wait loop.
    std::stop_token cancel_token = cancel_source_.get_token();

    // §Phase 9.8: SSE streaming — only wired when config_.stream is true.
    // When false the existing buffered JSON path is used unchanged.
    // §ProviderPlugins: the parser comes from the active provider plugin
    // (default: the classic MiniMaxSseParser behavior).
    auto sse_parser_holder = plugin_->create_stream_parser(
        std::weak_ptr<hive::AgentSink>(sink_));
    hive::ProviderStreamParser& sse_parser = *sse_parser_holder;
    std::function<void(std::string_view)> streaming_callback;
    // Lifecycle hooks for host UI spinners. started/finished guard ensures
    // on_llm_request_finished always fires once even on cancellation or
    // early returns.
    struct LlmLifecycle {
        std::shared_ptr<hive::AgentSink> sink;
        std::atomic<bool> wait_started{false};
        std::atomic<bool> response_started{false};
        std::atomic<bool> finished{false};
        explicit LlmLifecycle(std::shared_ptr<hive::AgentSink> s)
            : sink(std::move(s)) {}
        void start_wait() {
            if (!wait_started.exchange(true) && sink) {
                sink->on_llm_wait_started();
            }
        }
        void start_response() {
            if (!response_started.exchange(true) && sink) {
                sink->on_llm_response_started();
            }
        }
        void finish() {
            // Only fire the finished hook if the wait hook was actually
            // started (i.e. we reached the HTTP dispatch point).
            if (!wait_started.load() || finished.exchange(true)) return;
            if (sink) sink->on_llm_request_finished();
        }
        ~LlmLifecycle() { finish(); }
    } lifecycle{sink_};
    if (config_.stream) {
        // Capture sse_parser by reference — it lives on run_agent_cycle's
        // stack and the async task fires its callbacks while we are still
        // in the wait loop below, so the reference is always valid.
        streaming_callback = [&sse_parser, &lifecycle](std::string_view chunk) {
            lifecycle.start_response();
            sse_parser.on_chunk(chunk);
        };
    }
    lifecycle.start_wait();
    auto api_future = make_api_request_async(
        api_url, request_payload.dump(-1, ' ', false, json::error_handler_t::replace),
        cancel_token, streaming_callback);

    // §Phase 5.1: wait for the API response. The host may call
    // Agent::cancel() (which fires cancel_source_) at any time; we
    // exit the wait loop on cancel_token.stop_requested(). Library
    // no longer reads stdin / sets raw mode / captures ESC bytes.
    while (true) {
        auto status = api_future.wait_for(std::chrono::milliseconds(100));
        if (cancel_token.stop_requested()) {
            std::ostringstream msg;
            msg << "\n[INTERRUPTED] Agent execution halted by user.";
            if (sink_) sink_->on_log(hive::LogLevel::Info, msg.str());
            messages = messages_->sanitize_message_chain(messages);
            messages_->clear_executing_markers();
            return AgentState::CANCELLED;
        }
        if (status != std::future_status::timeout) {
            break;
        }
    }

    // Flush any remaining SSE data from the buffer.
    sse_parser.finish();

    // Get the result from the API call.
    // §on_error fix: actually consume the future. Previously
    // api_future.get() was never called — the task's terminal exception
    // ("API request failed after N attempts") and the HTTP error body
    // were silently discarded, and non-streaming turns fell into the
    // parse-failure fallback below and issued a duplicate request.
    try {
        api_response = api_future.get();
        response_body = api_response.body;
    } catch (const std::exception& e) {
        if (cancel_token.stop_requested()) {
            // Cancellation is reported via the wait-loop branch, not as
            // an error.
            return AgentState::CANCELLED;
        }
        // Terminal transport failure (retries already exhausted inside
        // make_api_request_async — which also fired per-attempt
        // on_error events; this is the terminal one).
        emit_error(hive::ErrorKind::Network, e.what(), /*will_retry=*/false);
        return AgentState::DONE;
    }

    // With config_.stream=true, response_body may be empty (HttpSession
    // takes the streaming branch and doesn't accumulate 2xx bodies when
    // stream_callback is set). In that case sse_parser.final_event()
    // carries the data.
    // With config_.stream=false, response_body holds the full JSON and
    // sse_parser.final_event() is always null.
    const int max_retries = 3;
    bool parse_success = false;
    json response_json;

    // §robust-client: handle HTTP error statuses ONCE, keyed on the status
    // code — never by trying to json::parse an (often empty, in streaming
    // mode) error body. Returns the AgentState to exit with when the
    // response is a terminal HTTP error; std::nullopt when the caller may
    // continue. On a structured {"error": {...}} body from a non-auth 4xx,
    // fills `out_json` and returns nullopt so the existing FATAL/TRANSIENT
    // pipeline below can classify it (request-shape fatals are still
    // recoverable by the model).
    auto handle_http_error_status =
        [this](const ApiResponse& resp, json& out_json) -> std::optional<AgentState> {
        if (resp.ok()) return std::nullopt;

        const long code = resp.status_code;
        std::string err_type, err_msg;
        try {
            json j = json::parse(resp.body);
            if (j.contains("error") && j["error"].is_object()) {
                err_type = j["error"].value("type", "");
                err_msg  = j["error"].value("message", "");
                if (!err_type.empty()) out_json = std::move(j);
            }
        } catch (...) {
            // Non-JSON error body (proxy HTML, empty, ...) — handled below.
        }

        // Auth: always terminal. The model cannot fix credentials —
        // retrying or injecting "try a different approach" just loops
        // the 401 until the iteration cap.
        if (code == 401 || code == 403) {
            const std::string msg = err_msg.empty()
                ? "HTTP " + std::to_string(code) + " (authentication failed)"
                : err_type + ": " + err_msg;
            emit_error(hive::ErrorKind::Auth, msg, /*will_retry=*/false);
            return AgentState::DONE;
        }

        // Structured provider error on a non-auth 4xx: hand it to the
        // existing classification pipeline (rate-limit retry / request-shape
        // recovery preserved).
        if (!err_type.empty() && code < 500) return std::nullopt;

        // 5xx (retries already exhausted in the request layer) or an
        // unstructured error body: terminal, keyed on the status code.
        const auto kind = hive::error_kind_from_http_status(code);
        std::string msg = "HTTP " + std::to_string(code) + " error";
        if (!err_msg.empty()) {
            msg += ": " + err_type + ": " + err_msg;
        } else if (!resp.body.empty()) {
            msg += ": " + resp.body.substr(0, 300);
        }
        emit_error(kind, msg, /*will_retry=*/false);
        return AgentState::DONE;
    };

    {
        json err_json;
        if (auto terminal = handle_http_error_status(api_response, err_json)) {
            return *terminal;
        } else if (!err_json.is_null()) {
            // Structured provider error on a non-auth 4xx — feed it to the
            // FATAL/TRANSIENT pipeline as if it had parsed cleanly.
            response_json = std::move(err_json);
            parse_success = true;
        }
    }

    // Load server_error_settings to gate the SSE retry loop (B27)
    hive::ServerErrorSettings server_error_settings;
    try {
        if (settings_store_) {
            server_error_settings = settings_store_->get_server_error_settings();
        }
    } catch (...) {
        server_error_settings.enabled = true;
    }

    for (int retry = 1; retry <= max_retries && !parse_success; retry++) {
        if (retry == 1 && !sse_parser.final_event().is_null()) {
            // First attempt: use the JSON from SSE streaming (has complete
            // usage + finish_reason from the final SSE event). Fall through
            // to token update and tool extraction below.
            response_json = sse_parser.final_event();
            parse_success = true;
            break;
        }

        // §robust-client: in streaming mode a 2xx response body is not
        // accumulated — if the SSE parser produced no final event there is
        // NOTHING to parse. Skip the pointless json::parse("") attempt (and
        // its misleading "JSON parse failed" warning) and go straight to
        // the synchronous fallback request.
        const bool nothing_to_parse =
            config_.stream && sse_parser.final_event().is_null()
            && response_body.empty();
        if (nothing_to_parse && retry < max_retries) {
            retry++;  // consume this attempt; fall into the fallback below
        } else if (nothing_to_parse) {
            break;  // handled by the !parse_success path below
        }

        // §Phase 9.8: if streaming was on but the SSE parser got no final
        // event (e.g. server doesn't support SSE, or the Accept header wasn't
        // honoured), fall back to a synchronous non-streaming request for this
        // retry instead of trying to json::parse an empty response_body.
        if (retry >= 2 && config_.stream && sse_parser.final_event().is_null()) {
            try {
                // §SYNC-FALLBACK-STREAM (2026-08-01): the sync fallback
                // cannot consume SSE — force a buffered response or the
                // server streams "data: ..." and json::parse fails on 'd'.
                json sync_payload = request_payload;
                sync_payload["stream"] = false;
                sync_payload.erase("stream_options");
                api_response = make_http_request_with_retry( api_url,
                    sync_payload.dump(-1, ' ', false, json::error_handler_t::replace));
                response_body = api_response.body;
                // §robust-client: the fallback may itself return an HTTP
                // error status — handle it by status, not by parse luck.
                {
                    json fb_err_json;
                    if (auto terminal = handle_http_error_status(api_response, fb_err_json)) {
                        return *terminal;
                    } else if (!fb_err_json.is_null()) {
                        response_json = std::move(fb_err_json);
                        parse_success = true;
                        break;
                    }
                }
            } catch (const std::exception& api_e) {
                std::ostringstream api_retry;
                api_retry << "API request also failed: " << api_e.what();
                if (sink_) sink_->on_log(hive::LogLevel::Error, api_retry.str());
            }
        }

        try {
            response_json = json::parse(response_body);
            parse_success = true;
            break;  // Success, continue processing
        } catch (const json::parse_error& e) {
            if (!server_error_settings.enabled) {
                // User disabled server-error retry via settings store (B27)
                break;
            }
            if (retry < max_retries) {
                int delay = 1 << (retry - 1);  // 1s, 2s, 4s...
                std::ostringstream retry_msg;
                retry_msg << "JSON parse failed (attempt " << retry << "/" << max_retries
                          << "), waiting " << delay << "s...";
                if (sink_) sink_->on_log(hive::LogLevel::Warn, retry_msg.str());
                std::this_thread::sleep_for(std::chrono::seconds(delay));

                // Try to get a fresh response from the API (non-streaming fallback)
                try {
                    // §SYNC-FALLBACK-STREAM: see above — force stream off.
                    json sync_payload = request_payload;
                    sync_payload["stream"] = false;
                    sync_payload.erase("stream_options");
                    api_response = make_http_request_with_retry( api_url,
                        sync_payload.dump(-1, ' ', false, json::error_handler_t::replace));
                    response_body = api_response.body;
                    // §robust-client: the fallback may itself return an HTTP
                    // error status — handle it by status, not by parse luck.
                    {
                        json fb_err_json;
                        if (auto terminal = handle_http_error_status(api_response, fb_err_json)) {
                            return *terminal;
                        } else if (!fb_err_json.is_null()) {
                            response_json = std::move(fb_err_json);
                            parse_success = true;
                            break;
                        }
                    }
                } catch (const std::exception& api_e) {
                    std::ostringstream api_retry;
                    api_retry << "API request also failed: " << api_e.what();
                    if (sink_) sink_->on_log(hive::LogLevel::Error, api_retry.str());
                }
            } else {
                // All retries exhausted — terminal parse failure.
                std::string error_msg = extract_error_message(response_body);
                if (!error_msg.empty()) {
                    emit_error(hive::ErrorKind::Parse, error_msg,
                               /*will_retry=*/false);
                    LOG_ERROR("JSON parse error with API message: " + error_msg);
                } else {
                    std::ostringstream parse_err;
                    parse_err << "Failed to parse JSON response after " << max_retries
                              << " attempts: " << e.what();
                    emit_error(hive::ErrorKind::Parse, parse_err.str(),
                               /*will_retry=*/false);
                    LOG_ERROR("Response was: " + response_body.substr(0, 500));
                    LOG_ERROR("JSON parse failed: " + std::string(e.what()));
                }
                return AgentState::DONE;
            }
        }
    }
    
    if (!parse_success) {
        // Reached only when server-error retry is disabled (B27) and the
        // first parse attempt failed — was a silent DONE before §on_error.
        emit_error(hive::ErrorKind::Parse,
                   "Failed to parse JSON response (server-error retry disabled)",
                   /*will_retry=*/false);
        return AgentState::DONE;
    }

    // Track token usage from this response (for stats display + compression threshold)
    update_token_usage(response_json);

    // Check for API errors
    if (response_json.contains("error")) {
        auto& error = response_json["error"];
        std::string error_message = error.value("message", "Unknown error");
        std::string error_type = error.value("type", "unknown");
        // §on_error: keep the untruncated provider message for the host;
        // the truncation below is only for the conversation-injected copy.
        const std::string full_error_message = error_message;
        const hive::ErrorKind error_kind =
            plugin_->classify_api_error_type(error_type);

        // Remove provider-specific text from the error message (URLs, help text)
        // Find the first period followed by space, or " at ", or " You " and cut there
        size_t cut_pos = error_message.find(". You ");
        if (cut_pos != std::string::npos) {
            error_message = error_message.substr(0, cut_pos);
        } else {
            cut_pos = error_message.find(" at ");
            if (cut_pos != std::string::npos) {
                error_message = error_message.substr(0, cut_pos);
            }
        }

        // §B9: extract the "find last tool_call" walk once and reuse it.
        LastToolCall last = find_last_tool_call(messages, /*want_args=*/true);
        const std::string& last_tool_call_id = last.id;
        const std::string& last_tool_name   = last.name;
        const std::string& last_tool_args   = last.args;

        // Classify error and decide whether to retry
        ErrorCategory category = classify_api_error(error_type, error_message);
        
        if (category == ErrorCategory::FATAL) {
            // Fatal error - add error message to conversation and continue.
            // §on_error: structured terminal report with the FULL provider
            // message (replaces the previous truncated on_log line — the
            // default sink forwards on_error to on_log, preserving output).
            emit_error(error_kind, error_type + ": " + full_error_message,
                       /*will_retry=*/false);
            LOG_DEBUG("Last tool call: id=" + last_tool_call_id + " name=" + last_tool_name);
            LOG_DEBUG("Args: " + last_tool_args);
            LOG_ERROR("API error [FATAL, " + error_type + "]: " + error_message + " | Tool: " + last_tool_name + " (id=" + last_tool_call_id + ")");

            // §robust-client: auth failures are terminal for the turn. The
            // model cannot fix credentials — injecting "try a different
            // approach" and returning ACT just re-issues the same request
            // and loops the 401 until the iteration cap.
            if (error_kind == hive::ErrorKind::Auth) {
                return AgentState::DONE;
            }

            // §fix (2026-07-30, hardened 2026-08-01): circuit breaker — ANY
            // fatal-classified cycle counts, not just identical signatures.
            // Alternating 400 shapes (each injected recovery message can
            // corrupt the history into the next shape) defeat a
            // same-signature check and looped forever in practice. When it
            // trips we end the turn WITHOUT injecting another recovery
            // message; the host has already seen every error via emit_error.
            const std::string fatal_signature = error_type + "|" + full_error_message;
            if (fatal_breaker_should_end_turn(fatal_signature)) {
                emit_error(error_kind,
                           "Unrecoverable repeated API error (" + error_type +
                           ") - turn ended to avoid an infinite retry loop.",
                           /*will_retry=*/false);
                return AgentState::DONE;
            }

            // §fix (2026-07-30): add a tool result for the failed call ONLY
            // when it is genuinely pending at the tail. Injecting a result
            // for a mid-history call creates a misplaced tool message that
            // the provider rejects and the loop never escapes.
            Messages::append_tool_results_if_pending(messages,
                "[API ERROR] This tool call was REJECTED by the API server with: " + error_type + " - " + error_message + ".\n\nThe arguments you provided were:\n" + last_tool_args + "\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call. Read the tool description and the error carefully, then use different arguments or a different tool.");

            // Also add a user message for additional context
            messages.push_back({
                {"role", "user"},
                {"content", "[SYSTEM ERROR] Your last tool call (" + last_tool_name + ") was rejected by the API: " + error_message + ". Please try a different approach - do NOT repeat the same call."}
            });

            // Return ACT state to continue the agent loop
            return AgentState::ACT;
        }
        
        // Transient error - retry with backoff
        std::ostringstream retry_msg2;
        retry_msg2 << error_type << ": " << error_message << " - will retry...";
        if (sink_) sink_->on_log(hive::LogLevel::Warn, retry_msg2.str());
        emit_error(error_kind, error_type + ": " + full_error_message,
                   /*will_retry=*/true);
        LOG_ERROR("API error [TRANSIENT, " + error_type + "]: " + error_message);
        
        bool retry_success = false;
        for (int retry = 1; retry <= max_retries; retry++) {
            int delay = 1 << (retry - 1);  // 1s, 2s, 4s...
            std::ostringstream wait_msg;
            wait_msg << "Waiting " << delay << "s before retry " << retry << "/" << max_retries << "...";
            if (sink_) sink_->on_log(hive::LogLevel::Info, wait_msg.str());
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            
            try {
                // §SYNC-FALLBACK-STREAM: see above — force stream off.
                json sync_payload = request_payload;
                sync_payload["stream"] = false;
                sync_payload.erase("stream_options");
                api_response = make_http_request_with_retry( api_url, sync_payload.dump(-1, ' ', false, json::error_handler_t::replace));
                response_body = api_response.body;
                // §robust-client: status first — a terminal HTTP error
                // (auth / 5xx / unstructured body) ends the turn here.
                {
                    json fb_err_json;
                    if (auto terminal = handle_http_error_status(api_response, fb_err_json)) {
                        return *terminal;
                    } else if (!fb_err_json.is_null()) {
                        response_json = std::move(fb_err_json);
                    } else {
                        response_json = json::parse(response_body);
                    }
                }

                // Check if this response is valid
                if (!response_json.contains("error") &&
                    response_json.contains("choices") &&
                    !response_json["choices"].empty()) {
                    LOG_INFO("API call succeeded on retry " + std::to_string(retry));
                    retry_success = true;
                    break;
                }
                
                // Check for error again
                if (response_json.contains("error")) {
                    auto& retry_error = response_json["error"];
                    std::string retry_error_type = retry_error.value("type", "unknown");
                    std::string retry_error_msg = retry_error.value("message", "Unknown error");
                    if (classify_api_error(retry_error_type, retry_error_msg) == ErrorCategory::FATAL) {
                        LOG_ERROR("Fatal error during retry: " + retry_error_type);
                        // §on_error: was LOG-only — the host never saw the
                        // fatal classification, only the earlier transient
                        // warning.
                        const auto retry_kind = plugin_->classify_api_error_type(retry_error_type);
                        emit_error(retry_kind,
                                   retry_error_type + ": " + retry_error_msg,
                                   /*will_retry=*/false);
                        // §robust-client: auth failures are terminal for the
                        // turn — the model cannot fix credentials, so do not
                        // inject a "try a different approach" prompt (that
                        // just loops the 401).
                        if (retry_kind == hive::ErrorKind::Auth) {
                            return AgentState::DONE;
                        }
                        // §fix (2026-08-01): this path previously bypassed
                        // the fatal circuit breaker entirely — a transient
                        // error that turns fatal during retry re-entered the
                        // cycle (and appended another [SYSTEM ERROR] message)
                        // forever. Route it through the same breaker; when it
                        // trips, end the turn without further injection.
                        if (fatal_breaker_should_end_turn(
                                retry_error_type + "|" + retry_error_msg)) {
                            emit_error(retry_kind,
                                       "Unrecoverable repeated API error (" + retry_error_type +
                                       ") - turn ended to avoid an infinite retry loop.",
                                       /*will_retry=*/false);
                            return AgentState::DONE;
                        }
                        // §B9: reuse the shared helper.
                        LastToolCall last_retry = find_last_tool_call(messages);
                        const std::string& last_tool_id   = last_retry.id;
                        const std::string& last_tool_name = last_retry.name;
                        // §fix (2026-07-30): guarded injection only (see
                        // append_tool_results_if_pending).
                        Messages::append_tool_results_if_pending(messages,
                            "[API ERROR] This tool call was REJECTED by the API server with: " + retry_error_type + " - " + retry_error_msg + ".\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call.");
                        messages.push_back({
                            {"role", "user"},
                            {"content", "[SYSTEM ERROR] API error: " + retry_error_type + " - " + retry_error_msg + "\n\nLast tool: " + last_tool_name + " (id=" + last_tool_id + ")\n\nPlease try a DIFFERENT approach - do NOT repeat the same call."}
                        });
                        return AgentState::ACT;
                    }
                    std::ostringstream persist_msg;
                    persist_msg << "Error persisted: " << retry_error_type;
                    emit_error(plugin_->classify_api_error_type(retry_error_type),
                               persist_msg.str(), /*will_retry=*/true);
                }
            } catch (const std::exception& e) {
                std::ostringstream exc_msg;
                exc_msg << "Exception during retry: " << e.what();
                emit_error(hive::ErrorKind::Unknown, exc_msg.str(),
                           /*will_retry=*/true);
            }
        }
        
        if (!retry_success) {
            std::ostringstream failed_msg;
            failed_msg << "All API retries failed. Adding error to conversation and continuing.";
            emit_error(error_kind, failed_msg.str(), /*will_retry=*/false);
            // §fix (2026-08-01): route through the fatal circuit breaker.
            // An unresolvable error that exhausts retries on every cycle
            // otherwise loops forever, appending one [SYSTEM ERROR] message
            // per cycle. When the breaker trips, end the turn without
            // further injection.
            if (fatal_breaker_should_end_turn(
                    "retries_exhausted|" + error_type + "|" + full_error_message)) {
                emit_error(error_kind,
                           "Unrecoverable repeated API error (" + error_type +
                           ") - turn ended to avoid an infinite retry loop.",
                           /*will_retry=*/false);
                return AgentState::DONE;
            }
            // §fix (2026-07-30): guarded injection only (see
            // append_tool_results_if_pending).
            Messages::append_tool_results_if_pending(messages,
                "[API ERROR] All " + std::to_string(max_retries) + " retries failed.\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call.");
            messages.push_back({
                {"role", "user"},
                {"content", "[SYSTEM ERROR] API error after " + std::to_string(max_retries) + " retries. Please try a DIFFERENT approach - do NOT repeat the same tool call."}
            });
            // Return ACT state to continue the agent loop
            return AgentState::ACT;
        }
        // If retry_success is true, we have a valid response_json, continue processing
    }

    // The LLM request/response cycle is complete. Signal hosts so they can
    // stop any "thinking" spinner before we move on to tool execution.
    lifecycle.finish();

    // Check for valid response
    // §Phase 9.10: when streaming with `stream_options: {include_usage: true}`,
    // MiniMax (and OpenAI) send a separate final SSE chunk whose ONLY payload
    // is the `usage` object — `choices` is `[]` and there is no `message`.
    // That IS a valid response: the assistant message has already been
    // accumulated by `MiniMaxSseParser` from the earlier delta chunks.  Skip
    // the "no choices" retry path in that case.
    //
    // §BUG-001 (2026-07-17): the original gate keyed on
    // `!response_json["usage"].is_null()`. That fails for MiniMax,
    // which sends `"usage": null` on the final chunk. The result: a
    // successfully-streamed response (text or tool_calls) is
    // discarded, and the library falls into the 3-retry exponential
    // backoff (1s+2s+4s=7s of silent spinner) before giving up.
    // The fix is to gate on what the SSE parser has actually
    // accumulated — text OR tool_calls — regardless of what `usage`
    // contains. See `is_usage_only_chunk()` in MiniMaxSseParser.h
    // for the truth table.
    if (!response_json.contains("choices") || response_json["choices"].empty()) {
        if (is_usage_only_chunk(config_.stream, response_json, sse_parser)) {
            // §Phase 9.10 / §BUG-001: usage-only final SSE chunk.
            // Build a synthetic single-choice response so the rest of
            // the pipeline (which reads `response_json["choices"][0]`)
            // continues to work. `usage` data is preserved as-is for
            // `update_token_usage`.
            nlohmann::json synth_message = sse_parser.synthesized_message();
            response_json["choices"] = nlohmann::json::array({
                {{"index", 0},
                 {"message", synth_message},
                 {"finish_reason", "stop"}}
            });
            // §Phase 9.10 / §BUG-001: this path is normal for streaming
            // responses. Keep the trace in the file log only so TUI/CLI
            // hosts are not spammed with a routine SSE housekeeping message.
            LOG_INFO("Stream finished with usage-only final chunk "
                     "(recovered via synthesized_message)");
            // Fall through to message/tool-call processing below.
        }
        else
        {
        LOG_ERROR("No choices in response - will retry API call...");
        
        // Retry with exponential backoff
        bool api_success = false;
        
        for (int retry = 1; retry <= max_retries; retry++) {
            int delay = 1 << (retry - 1);  // 1s, 2s, 4s...
            std::ostringstream nochoices_msg;
            nochoices_msg << "No choices - Attempt " << retry << "/" << max_retries
                           << " (waiting " << delay << "s)...";
            if (sink_) sink_->on_log(hive::LogLevel::Warn, nochoices_msg.str());
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            
            try {
                // §SYNC-FALLBACK-STREAM: see above — force stream off.
                json sync_payload = request_payload;
                sync_payload["stream"] = false;
                sync_payload.erase("stream_options");
                api_response = make_http_request_with_retry( api_url, sync_payload.dump(-1, ' ', false, json::error_handler_t::replace));
                response_body = api_response.body;
                // §robust-client: status first — a terminal HTTP error
                // (auth / 5xx / unstructured body) ends the turn here.
                {
                    json fb_err_json;
                    if (auto terminal = handle_http_error_status(api_response, fb_err_json)) {
                        return *terminal;
                    } else if (!fb_err_json.is_null()) {
                        response_json = std::move(fb_err_json);
                    } else {
                        response_json = json::parse(response_body);
                    }
                }
                
                if (response_json.contains("choices") && !response_json["choices"].empty()) {
                    LOG_INFO("Got valid response on retry " + std::to_string(retry));
                    api_success = true;
                    break;
                }
                
                // Check if we got an error instead
                if (response_json.contains("error")) {
                    auto& retry_error = response_json["error"];
                    std::string retry_error_type = retry_error.value("type", "unknown");
                    std::string retry_error_msg = retry_error.value("message", "Unknown error");
                    std::ostringstream got_err_msg;
                    got_err_msg << "Got error: " << retry_error_type << ": " << retry_error_msg;
                    emit_error(plugin_->classify_api_error_type(retry_error_type),
                               got_err_msg.str(), /*will_retry=*/true);
                    
                    if (classify_api_error(retry_error_type) == ErrorCategory::FATAL) {
                        LOG_ERROR("Fatal error: " + retry_error_type);
                        const auto nochoices_kind = plugin_->classify_api_error_type(retry_error_type);
                        emit_error(nochoices_kind,
                                   retry_error_type + ": " + retry_error_msg,
                                   /*will_retry=*/false);
                        // §robust-client: auth failures are terminal for the
                        // turn — the model cannot fix credentials, so do not
                        // inject a recovery prompt (that just loops the 401).
                        if (nochoices_kind == hive::ErrorKind::Auth) {
                            return AgentState::DONE;
                        }
                        // §fix (2026-08-01): this path previously bypassed
                        // the fatal circuit breaker — route it through the
                        // same check; when it trips, end the turn without
                        // further injection.
                        if (fatal_breaker_should_end_turn(
                                retry_error_type + "|" + retry_error_msg)) {
                            emit_error(nochoices_kind,
                                       "Unrecoverable repeated API error (" + retry_error_type +
                                       ") - turn ended to avoid an infinite retry loop.",
                                       /*will_retry=*/false);
                            return AgentState::DONE;
                        }
                        // Find the last tool call for debugging
                        std::string last_tool_name = "UNKNOWN";
                        std::string last_tool_id = "UNKNOWN";
                        try {
                            for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
                                if (!messages[i].is_object()) continue;
                                if (messages[i].value("role", "") != "assistant") continue;
                                if (!messages[i].contains("tool_calls") || !messages[i]["tool_calls"].is_array()) continue;
                                auto& tcs = messages[i]["tool_calls"];
                                if (tcs.empty() || !tcs[tcs.size()-1].is_object()) continue;
                                if (!tcs[tcs.size()-1].contains("function") || !tcs[tcs.size()-1]["function"].is_object()) continue;
                                last_tool_id = tcs[tcs.size()-1].value("id", "UNKNOWN");
                                last_tool_name = tcs[tcs.size()-1]["function"].value("name", "UNKNOWN");
                                break;
                            }
                        } catch (const std::exception& e) {
                            LOG_DEBUG(std::string("Error extracting tool call: ") + e.what());
                        }
                        // Add error message and RETURN immediately
                        messages.push_back({
                            {"role", "user"},
                            {"content", "[SYSTEM ERROR] No choices + got error: " + retry_error_type + " - " + retry_error_msg + "\n\nLast tool: " + last_tool_name + " (id=" + last_tool_id + ")\n\nPlease re-read the file and try again with corrected parameters."}
                        });
                        return AgentState::ACT;
                    }
                }
            } catch (const std::exception& e) {
                std::ostringstream exc2_msg;
                exc2_msg << "Exception during retry: " << e.what();
                emit_error(hive::ErrorKind::Unknown, exc2_msg.str(),
                           /*will_retry=*/true);
            }
        }
        
        if (!api_success) {
            LOG_ERROR("All API retries failed. Adding error to conversation and continuing.");
            // §on_error: was LOG-only — terminal empty-response failure
            // never reached the host.
            emit_error(hive::ErrorKind::ServerError,
                       "API returned empty response (no choices) after "
                           + std::to_string(max_retries) + " retries",
                       /*will_retry=*/false);
            // §fix (2026-08-01): route through the fatal circuit breaker —
            // an endlessly empty response otherwise loops ACT forever,
            // appending one [SYSTEM ERROR] message per cycle.
            if (fatal_breaker_should_end_turn("empty_response|no_choices")) {
                emit_error(hive::ErrorKind::ServerError,
                           "Unrecoverable repeated empty API response - "
                           "turn ended to avoid an infinite retry loop.",
                           /*will_retry=*/false);
                return AgentState::DONE;
            }
            // §fix (2026-07-30): guarded injection only (see
            // append_tool_results_if_pending) — replaces the ad-hoc
            // backward walk + unconditional push that could inject a
            // misplaced tool result for an already-answered call.
            Messages::append_tool_results_if_pending(messages,
                "[API ERROR] All " + std::to_string(max_retries) + " retries failed - API returned empty response.\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call.");
            messages.push_back({
                {"role", "user"},
                {"content", "[SYSTEM ERROR] API returned empty response after " + std::to_string(max_retries) + " retries. Please try a DIFFERENT approach - do NOT repeat the same tool call."}
            });
            // Return ACT state to continue the agent loop
            return AgentState::ACT;
        }
        // If api_success is true, we have a valid response_json, continue processing
        }
    }

    // §fix (2026-07-30): a usable response resets the fatal-error circuit
    // breaker — the conversation is making progress again.
    last_fatal_signature_.clear();
    consecutive_fatal_count_ = 0;

    json choice = response_json["choices"][0];
    json message = choice["message"];

    // §Phase 9.8: when we streamed via SSE the response_json is the LAST
    // SSE chunk, which only has `choices[0].delta`, NOT `choices[0].message`.
    // Reconstruct the standard non-streaming message from the SSE parser's
    // accumulated role/content/tool_calls/finish_reason fragments so the
    // tool-call processing below continues to work unchanged.
    if (!message.is_object() && config_.stream) {
        message = sse_parser.synthesized_message();
        // Wrap it back into a choice so the rest of the pipeline reads it
        // uniformly.  `response_json` itself keeps its full metadata
        // (usage, model, id, ...) for token tracking.
        response_json["choices"][0]["message"] = message;
    }

    // §ThinkTagFilter: strip inline <think>...</think> blocks from the
    // buffered (non-streaming) path so neither history nor the display
    // path below carries them. Streamed content was already filtered
    // delta-by-delta in the SSE parser — strip() on clean text is the
    // identity, so both paths converge here.
    if (message.contains("content") && message["content"].is_string()) {
        message["content"] =
            ThinkTagFilter::strip(message["content"].get<std::string>());
    }

    // Add the assistant's message to history
    messages.push_back(message);
    // PERSIST-DIRTY: raw-reference push - keep the flush worker in the
    // loop so a crash/kill mid-turn doesn't lose the reply.
    if (messages_) {
        messages_->mark_dirty();
    }

    // Check for tool calls
    if (message.contains("tool_calls")) {
        json tool_calls = message["tool_calls"];
        
        // NOTE: tool_calls are already in the message object that was pushed above,
        // and messages is a reference to messages_->get_messages(), so the tool_calls
        // are already properly stored. No need to call add_tool_calls() again.
        // The mark_tool_executing() call below tracks ephemeral runtime state only.

        // Process each tool call and prepare results
        json tool_results = json::array();

        for (const auto& tool_call : tool_calls) {
            // §fix (2026-07-18): a malformed API tool_call may lack
            // "function"/"name"/"id" — the unguarded
            // tool_call["function"]["name"] threw json::out_of_range
            // outside any try and escaped to the host. Guarded lookups
            // instead; a missing name yields an error tool-result (same
            // shape as the _invalid_args path below).
            std::string function_name;
            std::string call_id;
            if (tool_call.is_object()) {
                if (tool_call.contains("id") && tool_call["id"].is_string()) {
                    call_id = tool_call["id"].get<std::string>();
                }
                if (tool_call.contains("function") && tool_call["function"].is_object()
                    && tool_call["function"].contains("name")
                    && tool_call["function"]["name"].is_string()) {
                    function_name = tool_call["function"]["name"].get<std::string>();
                }
            }
            if (function_name.empty()) {
                LOG_ERROR("Skipping malformed tool_call (missing function/name): "
                          + tool_call.dump());
                tool_results.push_back({
                    {"role", "tool"},
                    {"tool_call_id", call_id},
                    {"content", "[ERROR] Malformed tool_call: missing function name"},
                    {"status", "error"}
                });
                continue;  // Skip execution, go to next tool call
            }
            
            // Mark as EXECUTING in Messages (ephemeral runtime state)
            messages_->mark_tool_executing(call_id);

            // Parse arguments for the structured on_tool_call event.
            json args_for_display = json::object();
            if (tool_call["function"].contains("arguments")) {
                auto& args = tool_call["function"]["arguments"];
                if (args.is_object()) {
                    args_for_display = args;
                } else if (args.is_string()) {
                    try {
                        args_for_display = json::parse(args.get<std::string>());
                    } catch (...) {
                        // Leave empty if arguments string is not valid JSON.
                    }
                }
            }

            // §Phase 9.6: notify the host that a tool is about to run.
            // This was missing from the library — AgentSink::on_tool_call
            // was declared and overridden by every demo's sink, but the
            // library never invoked it. Hosts could only see tool
            // activity via on_log() breadcrumbs or by parsing the
            // transcript. See Agent.md §Phase 9.6.
            // §Phase D1: correlate tool_call/result/progress events via call_id.
            // Sinks that don't care (e.g. ConsoleAgentSink) override only
            // the 2-arg versions — the 3-arg defaults above discard the
            // id and forward. WebSseSink overrides 3-arg and uses the
            // id as the DOM key for its per-tool card.
            if (sink_) sink_->on_tool_call(function_name, call_id,
                                           args_for_display);

            // Check if this tool_call has invalid arguments (marked during validation)
            if (tool_call.contains("_invalid_args") && tool_call["_invalid_args"] == true) {
                LOG_ERROR("Skipping tool_call " + call_id + " due to invalid arguments");
                std::string parse_error = tool_call.value("_parse_error", "unknown parse error");
                // Add error tool result - state derives from message chain
                tool_results.push_back({
                    {"role", "tool"},
                    {"tool_call_id", call_id},
                    {"content", "[ERROR] Invalid arguments JSON: " + parse_error},
                    {"status", "error"}
                });
                continue;  // Skip execution, go to next tool call
            }

            // Execute tool once - no retries. If it fails, the LLM will fix it in the next response.
            json result;
            try {
                // Wire streaming progress through the sink. Tools that
                // opt into the 3-arg execute() overload (e.g. bash,
                // network, MCP) will call this between on_tool_call
                // and on_tool_result, giving hosts live feedback for
                // long-running operations.
                ToolProgressFn progress_cb = nullptr;
                if (sink_) {
                    progress_cb = [this, function_name, call_id](const json& p) {
                        sink_->on_tool_progress(function_name, call_id, p);
                    };
                }
                result = tool_filter_
                    ? ToolHandler::execute_tool_call(
                        tool_call, *file_io_, progress_cb, *tool_filter_)
                    : ToolHandler::execute_tool_call(
                        tool_call, *file_io_, progress_cb);
                result["tool_name"] = function_name;

                std::string status = result.value("status", "unknown");
                LOG_INFO("Tool executed: " + function_name + " | Status: " + status);

                // §Phase 9.6: notify the host of the tool's outcome.
                // Same shape hosts already consume via Agent::execute_tool().
                if (sink_) sink_->on_tool_result(function_name, call_id, result);

                // §State Machine: when the agent explicitly drives its
                // own state via the `set_status` tool, dispatch the
                // transition so the host UI sees it immediately (e.g.
                // webui chip row updates). Routed through the owner's
                // callback when wired (normal Agent path); falls back to
                // firing the sink directly for direct LLMClient users.
                handle_set_state_result(result);

                // §COMPRESS-TOOL: when the agent asks for compression via
                // the `compress_context` tool, set the deferred flag — the
                // checkpoint at the end of this cycle performs the actual
                // compression (same path as Agent::compress_context()).
                handle_compress_context_result(result);

                // Log tool failures for operators; the structured result is
                // already delivered via on_tool_result above.
                if (status == "error") {
                    const std::string msg = result.value("message",
                                              result.value("error", "unknown error"));
                    LOG_ERROR("Tool " + function_name + " failed: " + msg);
                }

            } catch (const std::exception& e) {
                result = json{{"status", "error"}, {"message", std::string(e.what())}};
                LOG_ERROR("Tool " + function_name + " exception: " + std::string(e.what()));
                LOG_ERROR("Tool execution failed [" + function_name + "]: " + std::string(e.what()));

                // §Phase 9.6: also notify on the exception path, so the
                // sink sees one on_tool_result per on_tool_call even
                // when the tool throws.
                if (sink_) {
                    result["tool_name"] = function_name;
                    sink_->on_tool_result(function_name, call_id, result);
                }
            }
            
            // Update Messages tool state based on execution result
            // In the single-source-of-truth architecture, we don't track state separately.
            // Tool state is derived from the message chain. The call to add_tool_result()
            // below adds the tool result message which will make get_tool_state() return
            // COMPLETED or FAILED accordingly.
            // Note: mark_tool_executing() was already called at line 1188 BEFORE execution.
            // The tool result is added to tool_results array below and then inserted
            // into messages. For errors, we prepend "Error:" to the content so
            // is_error_result() detects it when checking state.
            
            // Determine error status and build error prefix for content
            bool is_error = (result.value("status", "") == "error");
            std::string error_prefix;
            if (is_error) {
                std::string error_msg = result.value("message", "Unknown error");
                error_prefix = "Error: " + error_msg + "\n\n";
            }
            
            // Tool result will be added to tool_results array below with content
            // After tool completes, clear the EXECUTING marker (ephemeral state)
            messages_->clear_executing(call_id);

            // Add tool result to message history (truncate if too large)
            // Special handling for read_image: strip the large data_uri to save context space
            // The LLM already received the data_uri in this response and can include images in
            // its next message using ![alt](data:...) syntax
            json result_for_storage = result;
            if (function_name == "read_image" && result.contains("data_uri")) {
                // Replace the large data_uri with just a reference to save memory
                std::string data_uri = result["data_uri"].get<std::string>();
                size_t data_size = data_uri.length();

                // §READ-IMAGE-CORE: queue the image for attachment to the
                // NEXT request as an image_url content part (see the
                // injection block right before the HTTP dispatch). Only
                // successful reads queue; error results carry no data_uri
                // in practice, but guard anyway.
                if (!is_error && !data_uri.empty()) {
                    PendingImage pi;
                    pi.path     = result.value("path", "unknown");
                    pi.prompt   = result.value("prompt", "");
                    pi.data_uri = data_uri;
                    pi.via      = "read_image";
                    pending_images_.push_back(std::move(pi));
                }

                result_for_storage["data_uri"] = "[IMAGE_DATA_REMOVED: " + result.value("path", "unknown") +
                    " (" + std::to_string(data_size) + " bytes) - image attached to next request automatically]";
                result_for_storage["image_data_removed"] = true;
            }
            
            // Sanitize result content to ensure valid UTF-8 before JSON serialization
            std::string result_content;
            bool needs_sanitize = false;
            try {
                result_content = result_for_storage.dump();
                // Verify it's valid JSON by parsing it back
                try {
                    [[maybe_unused]] auto parsed = json::parse(result_content);
                } catch (const json::exception&) {
                    // If parsing fails, trigger sanitization
                    needs_sanitize = true;
                }
            } catch (...) {
                // Any exception means we need to sanitize
                needs_sanitize = true;
            }
            
            if (needs_sanitize) {
                // If JSON serialization fails, sanitize and retry
                if (sink_) sink_->on_log(hive::LogLevel::Warn,
                    std::string("Tool result contains invalid UTF-8, sanitizing..."));
                
                // Function to recursively sanitize ALL strings in JSON
                // (both KEYS and VALUES - keys can have invalid UTF-8 too!)
                std::function<void(json&)> sanitize_json = [&](json& j) {
                    if (j.is_object()) {
                        // Build new object with sanitized keys
                        json sanitized = json::object();
                        for (auto it = j.begin(); it != j.end(); ++it) {
                            std::string clean_key = sanitize_utf8(it.key());
                            if (it.value().is_string()) {
                                sanitized[clean_key] = sanitize_utf8(it.value().get<std::string>());
                            } else if (it.value().is_object() || it.value().is_array()) {
                                sanitize_json(it.value());
                                sanitized[clean_key] = it.value();
                            } else {
                                sanitized[clean_key] = it.value();
                            }
                        }
                        j = std::move(sanitized);
                    } else if (j.is_array()) {
                        for (auto& item : j) {
                            if (item.is_string()) {
                                item = sanitize_utf8(item.get<std::string>());
                            } else if (item.is_object() || item.is_array()) {
                                sanitize_json(item);
                            }
                        }
                    }
                };

                sanitize_json(result_for_storage);
                // Use error_handler_t::replace as a safety net for any UTF-8
                // bytes that might have slipped through (e.g. in non-string values)
                result_content = result_for_storage.dump(-1, ' ', false, json::error_handler_t::replace);
            }
            
            if (result_content.size() > max_tool_result_size_) {
                std::ostringstream trunc_msg;
                trunc_msg << "Tool result truncated from " << result_content.size()
                          << " to " << max_tool_result_size_ << " bytes";
                if (sink_) sink_->on_log(hive::LogLevel::Warn, trunc_msg.str());
                
                // Truncate middle: keep start and end, remove middle
                size_t total_size = result_content.size();
                size_t max_size = max_tool_result_size_;
                
                // Reserve space for the truncation message
                const size_t truncation_msg_size = 60;
                size_t keep_each = (max_size > truncation_msg_size) ? (max_size - truncation_msg_size) / 2 : max_size / 2;
                
                // §BUG-UTF8-TRUNC: land both cuts on codepoint boundaries —
                // a split sequence here poisoned the history and aborted
                // the NEXT payload dump with type_error.316.
                std::string start_part = utf8_truncate_safe(result_content, keep_each);
                std::string end_part;
                if (total_size > keep_each) {
                    end_part = result_content.substr(total_size - keep_each, keep_each);
                    while (!end_part.empty() &&
                           is_utf8_continuation_byte(static_cast<uint8_t>(end_part.front())))
                        end_part.erase(end_part.begin());
                }
                
                std::ostringstream truncated;
                truncated << start_part << "\n\n[...] " << (total_size - 2 * keep_each) 
                          << " bytes removed [...]\n\n" << end_part;
                result_content = truncated.str();
            }
            tool_results.push_back({
                {"role", "tool"},
                {"tool_call_id", call_id},
                {"content", error_prefix + result_content},
                {"status", is_error ? "error" : "success"}
            });
            
            // DEBUG: Verify tool_result has correct tool_call_id

            // Check if this is a stop tool call - agent is done
            // Note: Summary is already printed by print_tool_result() above
            if (function_name == "stop") {
                messages.insert(messages.end(), tool_results.begin(), tool_results.end());
                if (messages_) messages_->mark_dirty();  // PERSIST-DIRTY
                return AgentState::DONE;
            }

        }

        // Add tool results to message history
        messages.insert(messages.end(), tool_results.begin(), tool_results.end());
        if (messages_) messages_->mark_dirty();  // PERSIST-DIRTY

        // Determine next state based on tool types used
        // If execute_bash was called, we just completed BUILD_TEST
        bool has_build_test = false;
        bool status_only = !tool_calls.empty();
        for (const auto& tool_call : tool_calls) {
            // §fix (2026-07-18): guarded lookup — malformed tool_calls
            // (missing function/name) must not throw here.
            std::string fn;
            if (tool_call.is_object()
                && tool_call.contains("function") && tool_call["function"].is_object()) {
                fn = tool_call["function"].value("name", std::string{});
            }
            if (fn == "execute_bash") {
                has_build_test = true;
            }
            if (fn != "set_status") {
                status_only = false;
            }
        }

        // §IdleExitBreaker (2026-08-20): this iteration's tool calls were
        // exclusively set_status. Once is a UI update; twice in a row is
        // a model stuck trying to exit — end the turn like `stop` would.
        // Tool results were already appended to the history above.
        if (status_only) {
            if (status_only_breaker_should_end_turn()) {
                return AgentState::DONE;
            }
        } else {
            consecutive_status_only_count_ = 0;
        }

        // Agent decides next state - continue in ACT or go to BUILD_TEST
        return has_build_test ? AgentState::BUILD_TEST : AgentState::ACT;
    }

    // If no tool calls, the agent provided a text response
    // This could mean it's done, answering a question, or thinking - display the response
    // §fix (2026-07-18): "content": null (common on reasoning-only turns)
    // made get<std::string>() throw json::type_error — require a string.
    if (message.contains("content") && message["content"].is_string()) {
        std::string content = message["content"].get<std::string>();
        if (!content.empty()) {
            // Remove thinking tags like <answer> or ```answer blocks if present
            size_t think_start = content.find("<answer>");
            if (think_start != std::string::npos) {
                size_t think_end = content.find("</answer>");
                if (think_end != std::string::npos) {
                    content = content.substr(think_start + 8, think_end - think_start - 8);
                }
            }
            
            // Also handle ```answer markdown blocks
            size_t md_start = content.find("```answer");
            if (md_start != std::string::npos) {
                size_t md_end = content.find("```", md_start + 8);
                if (md_end != std::string::npos) {
                    content = content.substr(md_start + 8, md_end - md_start - 8);
                }
            }

            // Final text response — emit through sink as a single delta
            // so the host can display it consistently with streamed output.
            // §Phase 5.1: no sink attached is a programmer error —
            // the library will never reach for std::cout. The host
            // must wire a sink to receive assistant output.
            // §Phase 9.11: skip the emit if the SSE stream already
            // delivered the text via deltas (otherwise the host sees
            // every fragment twice: once live-streamed, once here).
            if (sink_ && !sse_parser.had_text_deltas()) {
                sink_->on_assistant_delta("\n" + content + "\n");
            } else if (sink_) {
                // Already streamed live; nothing to re-emit.  Note this
                // in the sink log so operators can confirm dedup behaviour.
                // (Silent success is fine — this is the expected path.)
            } else {
                LOG_WARNING("Assistant produced content but no AgentSink "
                             "is attached. Provide a sink to receive output.");
            }
        }
    }

    // §IdleExitBreaker: a plain-text response is real progress (and ends
    // the turn anyway) — reset the set_status-only counter.
    consecutive_status_only_count_ = 0;

    // When agent just responds with text (no tools), task is complete
    // The user will be prompted to continue or quit when DONE state is handled
    return AgentState::DONE;
}

void LLMClient::inject_context_stats(json& messages) {
    // §ContextCache (2026-07-31): this block changes EVERY iteration.
    // It used to be embedded near the top of the system message, which
    // invalidated server-side prompt caches (MiniMax / Kimi match on a
    // stable tools → system → messages prefix) for the rest of the
    // system prompt AND the entire conversation history. The stats now
    // ride as a transient TRAILING system message, so the static prefix
    // (system prompt + conversation history) stays byte-identical
    // between requests. See src/api/ContextStatus.h for the mechanism.

    // Build context statistics string
    std::ostringstream stats;

    // Token usage
    float token_percent = 0.0f;
    bool at_threshold = false;
    if (get_max_context_size_value() > 0) {
        token_percent = (static_cast<float>(last_total_tokens_) / get_max_context_size_value()) * 100.0f;
        at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
    }

    // Add urgent warning if at 90% threshold - system handles compression automatically
    if (at_threshold) {
        stats << "\n🚨 **Context at 90% threshold** - SYSTEM will auto-compress\n\n";
        stats << "**No action needed** - the system compresses at the end of this cycle\n";
        stats << "- Just continue your work - compression never interrupts in-flight tool calls\n";
        stats << "- You may also call `compress_context` yourself at any time to schedule compression earlier\n\n";
    } else if (messages_ && get_max_context_size_value() > 0) {
        // §COMPRESS-TOOL: proactive nudge above 70% — the agent can shed
        // stale context BEFORE the hard 90% trigger instead of panicking
        // at the threshold.
        const float ratio = messages_->get_compression_ratio(last_total_tokens_);
        if (ratio >= 0.70f) {
            stats << "\n⚠️ **Context pressure above 70%**\n\n";
            stats << "If older parts of the conversation are no longer needed, call the "
                     "`compress_context` tool to schedule compression now - "
                     "it runs at the end of the cycle and never interrupts your work.\n\n";
        }
    }

    stats << "# Context Status\n";
    stats << "<!-- This section is auto-updated each iteration -->\n";
    stats << "| Metric | Value |\n";
    stats << "|--------|-------|\n";
    stats << "| Token Usage | " << last_total_tokens_ << "/" << get_max_context_size_value()
          << " (" << std::fixed << std::setprecision(1) << token_percent << "%) |\n";
    stats << "| Messages | " << messages.size() << " |\n";
    stats << "| Iterations | " << iteration_count_ << "/" << max_iterations_ << " |\n";

    // §ContextCache: current time moved here from the static prompt
    // header — a per-session timestamp in the system prompt defeats
    // cross-session prefix-cache reuse.
    {
        std::time_t now = std::time(nullptr);
        std::tm* local_tm = std::localtime(&now);
        std::ostringstream dt;
        dt << std::put_time(local_tm, "%Y-%m-%d %H:%M:%S %Z");
        stats << "| Date/Time | " << dt.str() << " |\n";
    }

    // §ContextCache: last-turn prompt-cache hit count, when the provider
    // reports it (Moonshot / MiniMax). Lets the model (and log readers)
    // see cache health without a UI.
    if (last_cached_tokens_ > 0) {
        stats << "| Prompt Cache | " << last_cached_tokens_ << "/"
              << last_prompt_tokens_ << " prompt tokens cached |\n";
    }

    // Compression status
    if (messages_) {
        float compress_ratio = messages_->get_compression_ratio(last_total_tokens_);
        stats << "| Context Pressure | " << std::fixed << std::setprecision(1) << (compress_ratio * 100) << "% |\n";
    }

    // Session memory stats
    if (session_memory_) {
        auto mem_stats = session_memory_->get_stats();
        stats << "| Session Memory | " << mem_stats.resolved_errors << "/" << mem_stats.total_errors << " errors resolved |\n";
        stats << "| | " << mem_stats.total_builds << " builds, " << mem_stats.total_sessions << " sessions |\n";
    }

    // Active skills
    if (skill_manager_) {
        auto active = skill_manager_->get_visible_active_skills();
        stats << "| Active Skills | " << active.size() << " |\n";
        if (!active.empty()) {
            stats << "| | ";
            for (size_t i = 0; i < active.size() && i < 5; i++) {
                if (i > 0) stats << ", ";
                stats << active[i].metadata.name;
            }
            if (active.size() > 5) stats << " (+" << (active.size() - 5) << " more)";
            stats << " |\n";
        }
    }

    // Strip any legacy embedded block, drop the previous transient
    // status message, and append this one last.
    hive::context_status::upsert_context_status_message(messages, stats.str());
}

// Format: ![alt text](path_or_url)
// Returns a vector of pairs: (image_path_or_url, position in string)
// Manager mode system prompt - makes the agent act as a manager
// This is a STRICT override that tells the agent to not do work itself,
// but instead spawn sub-agents and wait for them
void LLMClient::initialize() {
    // Display startup banner — §Phase 3: routed through sink.
    if (sink_) {
        std::ostringstream banner;
        banner << "╔═══════════════════════════════════════════════════════════════════\n"
               << "║  hive-agent v" << VERSION_STRING << " - Autonomous Coding Agent\n"
               << "║  Base URL: " << config_.base_url << "\n"
               << "║  Chat path: " << config_.chat_endpoint << "\n"
               << "║  Model:    " << config_.model << "\n"
               << "╚═══════════════════════════════════════════════════════════════════";
        sink_->on_log(hive::LogLevel::Info, banner.str());
    } else {
        // §Phase 5.1: no sink fallback. Banner goes through the
        // Logger, not std::cout.
        std::ostringstream banner;
        banner << "╔═══════════════════════════════════════════════════════════════════\n"
               << "║  hive-agent v" << VERSION_STRING << " - Autonomous Coding Agent\n"
               << "║  Base URL: " << config_.base_url << "\n"
               << "║  Chat path: " << config_.chat_endpoint << "\n"
               << "║  Model:    " << config_.model << "\n"
               << "╚═══════════════════════════════════════════════════════════════════";
        LOG_INFO(banner.str());
    }

    // Load persisted messages from previous session.
    if (messages_ && messages_->load()) {
        std::ostringstream persist;
        persist << "[PERSIST] Restored " << messages_->size()
                << " messages from previous session";
        if (sink_) sink_->on_log(hive::LogLevel::Info, persist.str());
        else LOG_INFO(persist.str());
        LOG_INFO("Successfully restored " + std::to_string(messages_->size())
                 + " persisted messages");
    } else {
        LOG_DEBUG("No messages file found or load failed, starting fresh session");
    }
}

void LLMClient::cleanup() {
    // Session end — save memory.
    if (session_memory_) {
        std::string summary = "Session with " + std::to_string(iteration_count_) + " iterations";
        session_memory_->on_session_end(summary, {}, iteration_count_);
    }

    // Save persistent messages for next session.
    if (messages_) {
        messages_->save();
    }
}

// §Phase 5.1: cancel an in-flight turn. Fires cancel_source_ which
// the API wait loop and the per-iteration check both consult. Hosts
// typically call this from Agent::cancel() (which we wire up in
// AgentImpl::cancel). Safe to call from any thread. Idempotent —
// multiple calls just leave cancel_source_ in the requested state.
void LLMClient::cancel() noexcept {
    if (cancel_source_.stop_possible()) {
        cancel_source_.request_stop();
    }
}

// §AutoCompress: compress immediately on the caller's thread. Caller must
// hold turn_mutex_ (or be inside a turn): run_one_turn() caches a
// reference to messages_' array across cycles, so an unguarded compress
// would dangle it.
CompressOutcome LLMClient::compress_now_unlocked() {
    if (!messages_) return CompressOutcome::NothingToCompress;
    const size_t before_tokens = messages_->estimate_tokens();
    auto result = messages_->compress();
    switch (result.outcome) {
    case CompressOutcome::Compressed:
        messages_->replace_messages(result.compressed_json);
        if (sink_) sink_->on_compaction(before_tokens, messages_->estimate_tokens());
        LOG_INFO("[CONTEXT] Compressed successfully (host request).");
        break;
    case CompressOutcome::FallbackDeleted:
        // §COMPRESS-REPORT: the summary call failed but the fallback
        // still reduced the context. That is a real state change — fire
        // on_compaction so host stats/UI don't go stale (hosts keying
        // success off on_compaction otherwise misreport this as a
        // failure even though the context shrank).
        if (sink_) sink_->on_compaction(before_tokens, messages_->estimate_tokens());
        LOG_WARNING("[CONTEXT] Summary failed; oldest messages deleted as "
                    "fallback (host request). Context reduced without summary.");
        break;
    case CompressOutcome::NothingToCompress:
    case CompressOutcome::Failed:
        break;
    }
    return result.outcome;
}

// §AutoCompress: host-facing trigger. Idle → compress right now on the
// caller's thread. Busy → flag consumed at the end-of-cycle checkpoint
// (also honored on the cancelled-turn path). Nothing to compress yet →
// carry the request over to the next turn's first cycle.
void LLMClient::compress_context() noexcept {
    if (turn_active_.load(std::memory_order_acquire)) {
        compress_requested_.store(true, std::memory_order_relaxed);
        return;
    }
    std::unique_lock lk(turn_mutex_, std::try_to_lock);
    if (!lk.owns_lock()) {
        // A turn is just starting — defer to its first checkpoint.
        compress_requested_.store(true, std::memory_order_relaxed);
        return;
    }
    try {
        const CompressOutcome outcome = compress_now_unlocked();
        if (outcome == CompressOutcome::NothingToCompress ||
            outcome == CompressOutcome::Failed) {
            // Nothing to compress yet (or the summary call failed and the
            // history was left untouched) — carry the request over to the
            // next turn's first checkpoint. Compressed/FallbackDeleted are
            // NOT re-armed: the context was already reduced, so re-arming
            // would just compress again next turn for no reason
            // (§COMPRESS-REPORT).
            compress_requested_.store(true, std::memory_order_relaxed);
        }
    } catch (...) {
        // noexcept contract: a failed immediate compress is dropped, the
        // host can retry.
    }
}

// §CRASH-4 (2026-07-16): forward to Messages::install_crash_handlers.
// No-op if messages_ hasn't been initialized yet (which would only
// happen if a host calls install_crash_handlers before initialize()).
void LLMClient::install_crash_handlers() {
    if (messages_) {
        messages_->install_crash_handlers();
    }
}

void LLMClient::uninstall_crash_handlers() {
    if (messages_) {
        messages_->uninstall_crash_handlers();
    }
}

    // ===================================================================
    // §Phase 3 - SSE streaming via sink
    //
    // MiniMax SSE line format:
    //   data: [{"id":"...","choices":[{"delta":{"content":"..."},"index":0}]}]
    //   data: [DONE]
    //
    // We accumulate a line buffer and parse each complete "data: " line.
    // Accumulated assistant text goes to sink_->on_assistant_delta(text).
    // ===================================================================

AgentState LLMClient::run_one_turn(const std::string& user_message) {
    // §AutoCompress: hold the turn guard for the whole turn so
    // compress_context() can compress immediately when (and only when) no
    // turn is running. Also serializes concurrent turns.
    std::lock_guard<std::mutex> turn_lock(turn_mutex_);
    turn_active_.store(true, std::memory_order_release);
    struct TurnActiveGuard {
        std::atomic<bool>& flag;
        ~TurnActiveGuard() { flag.store(false, std::memory_order_release); }
    } active_guard{turn_active_};

    // §fix (2026-07-18): reseat the cancel source. A std::stop_source
    // can never be un-stopped, so after cancel() fired once, every
    // subsequent turn saw stop_requested() on its first iteration and
    // returned CANCELLED forever. Install a fresh source before any
    // token is taken from it (run_agent_cycle calls get_token()).
    cancel_source_ = std::stop_source{};

    // §IdleExitBreaker: fresh turn — a status-only iteration at the end
    // of the PREVIOUS turn must not count against this one.
    consecutive_status_only_count_ = 0;

    // §Phase 16: route skill tools to this Agent's owned SkillManager while
    // this turn is executing.
    struct SkillManagerGuard {
        SkillManager* prev_;
        explicit SkillManagerGuard(SkillManager* sm)
            : prev_(SkillManager::current()) {
            SkillManager::set_current(sm);
        }
        ~SkillManagerGuard() { SkillManager::set_current(prev_); }
    } skill_guard(skill_manager_);

    std::string api_url = config_.base_url + config_.chat_endpoint;

    // Get current user for the system prompt header
    std::string username = "unknown";
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        username = pw->pw_name;
    }

    // Build header with user. §ContextCache: the Date/Time line was
    // moved OUT of the static system prompt into the transient
    // context-status message — a per-session timestamp here makes the
    // whole prompt unique per session and defeats cross-session
    // prefix-cache reuse (MiniMax / Kimi match on a stable prefix).
    std::string prompt_header = "# Session Info\n";
    prompt_header += "- **User**: " + username + "\n\n";

    // Build base system instruction using SystemPromptBuilder
    // §Phase 11: removed AgentMode from SystemPromptOptions.
    // §Phase 14: SystemPromptBuilder::compose() is now a thin composer.
    // The host's full system_prompt (from AgentConfig) is taken verbatim
    // as the first section; the library appends ONLY the dynamic adders
    // (MCP server status, tool definitions, skills catalog, session
    // memory, runtime stats). No identity / rules / workflow prose
    // lives in the library.
    //
    // §Phase 14 fix: refresh_mcp_tools() MUST happen BEFORE compose()
    // so the MCP status prose and tool_definitions both reflect the
    // live tool surface. (Previously compose() ran first, then
    // refresh_mcp_tools() updated the JSON schema AFTER system_instruction
    // was already built — the MCP server status prose was never added
    // to the prompt at all.)
    refresh_mcp_tools();

    SystemPromptOptions options;
    options.system_prompt = config_.system_prompt;  // verbatim, ALWAYS first

    // Optional system_info note (host's policy on tool discovery).
    if (!config_.system_info_note.empty()) {
        options.system_info_note = config_.system_info_note;
    }

    // MCP server status prose. The MCP **tools** also appear in the
    // JSON schema block below; this is the human-readable "what servers
    // are connected and what they offer" summary.
    if (mcp_manager_) {
        const auto connected = mcp_manager_->list_connected_servers();
        if (!connected.empty()) {
            std::ostringstream mcp_block;
            for (const auto& server_name : connected) {
                mcp_block << "- **" << server_name << "**";
                // Pull this server's tools from get_all_tools().
                // McpProxiedTool prefixes tool names with the server
                // name, so we filter by prefix.
                const auto prefix = server_name + "__";
                bool first_tool = true;
                for (const auto& t : mcp_manager_->get_all_tools()) {
                    if (t.name.rfind(prefix, 0) == 0) {
                        if (first_tool) {
                            mcp_block << " — tools: ";
                            first_tool = false;
                        } else {
                            mcp_block << ", ";
                        }
                        mcp_block << t.name.substr(prefix.size());
                    }
                }
                mcp_block << "\n";
            }
            options.mcp_servers = mcp_block.str();
        }
    }

    // Live tool JSON schema (post-MCP-refresh). Empty if no tools are
    // registered (degenerate — the agent has nothing to call).
    {
        std::string defs = tool_filter_
            ? ToolHandler::get_tool_definitions_filtered(*tool_filter_)
            : ToolHandler::get_tool_definitions();
        if (!defs.empty() && defs != "[]") {
            options.tool_definitions = std::move(defs);
        }
    }

    // Session memory fact dump. Emitted LAST in the composed prompt so
    // the volatile content doesn't truncate the cacheable prefix (see
    // SystemPromptBuilder::compose()). Hosts can suppress the prompt
    // section entirely via AgentConfig::memory_in_prompt while memory
    // collection continues.
    if (session_memory_ && !config_.memory_disabled && config_.memory_in_prompt) {
        options.session_memory = session_memory_->get_system_prompt_addition();
    }

    // Skills: active = summary table only (full content is fetched on
    // demand via the get_skill_context tool). Available catalog = name +
    // description, with active skills filtered out so each skill appears
    // in exactly one of the two sections.
    if (skill_manager_) {
        // Active skills — use the §Phase 15 filtered view (excludes
        // host-disabled skills even if they were previously activated).
        const auto active_skills = skill_manager_->get_visible_active_skills();
        if (!active_skills.empty()) {
            std::ostringstream active_block;
            active_block << "| Skill | Description |\n";
            active_block << "|-------|-------------|\n";
            for (const auto& s : active_skills) {
                active_block << "| " << s.metadata.name
                             << " | " << s.metadata.description << " |\n";
            }
            active_block << "\nFull content available via `get_skill_context` tool.\n";
            options.active_skills = active_block.str();
        }

        // Available skills — use the §Phase 15 filtered view (only
        // non-disabled skills appear). Already excludes active skills.
        // (§Phase 14 fix: was duplicating them in both sections.)
        const auto visible_skills = skill_manager_->get_visible_cached_skills();
        if (visible_skills.size() <= 50) {
            std::ostringstream cat;
            int listed = 0;
            for (const auto& [name, skill] : visible_skills) {
                if (skill_manager_->is_skill_active(name)) continue;
                cat << "- **" << name << "**: " << skill.metadata.description << "\n";
                ++listed;
            }
            if (listed > 0) {
                options.available_skills = cat.str();
            }
        }
    }

    std::string system_instruction = prompt_header + SystemPromptBuilder::compose(options);

    // §NC-8 (2026-07-16): CURL handle removed. The sync fallback path
    // inside run_agent_cycle now uses OmniNet::HTTP::Client directly;
    // the async path always used HttpSession (now also OmniNet).

    json& messages = messages_->get_messages();

    // Add the user message provided by the host (the *one* input we accept).
    if (messages.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_instruction}});
    }
    if (!user_message.empty()) {
        // §B19 fix: clean up a dangling tool result from an interrupted turn.
        // If the last message is a tool result, the previous turn ended mid-cycle
        // (e.g. user pressed Ctrl-C). The next run_one_turn would push a user
        // message after the tool result, causing the API to reject with
        // "tool call result does not follow tool call". Remove the incomplete
        // trailing tool cycle instead of refusing the user's message.
        // SANITIZE-NOTHROW: guard is_object() - a corrupt chain tail
        // (bare string/number) would make value() throw type_error.306.
        if (!messages.empty() && messages.back().is_object()
            && messages.back().value("role", "") == "tool") {
            LOG_WARNING("run_one_turn: cleaning up dangling tool result from "
                        "interrupted turn.");
            messages_->cleanup_dangling_tool_results();
        }
        // §READ-IMAGE-CORE follow-up (2026-08-10): host image attachments
        // written as Markdown ![alt](ref) are queued for payload-only
        // injection — the SAME pipeline as the read_image tool (vision
        // gate + stripped history). The stored message keeps the cheap
        // Markdown text; the base64 never persists in context. This
        // replaces the dead build_multimodal_message() path, which used
        // to embed the data URI in history (re-paid every turn).
        for (const auto& ref : hive::image_refs::extract(user_message)) {
            queue_image_ref(ref, "user attachment");
        }
        messages.push_back({{"role", "user"}, {"content", user_message}});
    }

    // PERSIST-DIRTY (2026-08-31): the pushes above went through the raw
    // json& reference, bypassing add_*() - mark the chain dirty so the
    // write-through flush worker persists the new turn promptly instead
    // of waiting for cleanup() (which hosts that never call shutdown()
    // - e.g. monkeybyte's ~ChatController - never reach). Without this,
    // the on-disk chain lagged the live one, often ending in an
    // unanswered user message that the NEXT session then re-answered
    // alongside the user's fresh prompt.
    if (messages_) {
        messages_->mark_dirty();
    }

    // Reset per-turn state
    iteration_count_ = 0;
    last_total_tokens_ = 0;
    last_cached_tokens_ = 0;
    consecutive_compress_count_ = 0;  // §BUG-COMPRESS-LOOP

    bool agent_running = true;
    AgentState final_state = AgentState::DONE;

    // Live tool definitions (re-fetched when MCP dirty flag flips).
    json tool_defs = json::parse(tool_filter_
        ? ToolHandler::get_tool_definitions_filtered(*tool_filter_)
        : ToolHandler::get_tool_definitions());

    while (agent_running) {
        // §Phase 5.1: cancel via cancel_source_ (fired by
        // Agent::cancel()). Library no longer polls stdin.
        if (cancel_source_.get_token().stop_requested()) {
            std::ostringstream msg;
            msg << "\n[INTERRUPTED] Agent execution halted by user.";
            if (sink_) sink_->on_log(hive::LogLevel::Info, msg.str());
            // §AutoCompress: honor a pending host compress request even
            // though the turn ends CANCELLED before any cycle ran —
            // otherwise a "cancel current work → compress_context()" host
            // flow strands the request. We hold turn_mutex_, so the
            // history is stable.
            if (compress_requested_.exchange(false, std::memory_order_relaxed)) {
                LOG_INFO("[COMPRESS] Host requested context compression (cancelled turn)...");
                if (messages_) {
                    messages_->set_messages(messages);
                    (void)compress_now_unlocked();
                }
            }
            final_state = AgentState::CANCELLED;
            break;
        }

        // §Phase 5.1: route the max-iterations diagnostic through the
        // Logger / sink instead of std::cout.
        if (iteration_count_ >= max_iterations_) {
            std::ostringstream msg;
            msg << "\n[WARNING] Maximum iterations (" << max_iterations_
                << ") reached. Stopping agent to prevent infinite loops.";
            if (sink_) sink_->on_log(hive::LogLevel::Warn, msg.str());
            break;
        }

        iteration_count_++;
        inject_context_stats(messages);

        if (mcp_tools_refresh_pending_ ||
            (mcp_manager_ && mcp_manager_->mcp_tools_dirty())) {
            refresh_mcp_tools();
            tool_defs = json::parse(ToolHandler::get_tool_definitions());
        }

        AgentState result_state = run_agent_cycle(api_url, messages, tool_defs);

        // Auto-compress at 90% threshold OR when the host asked for it via
        // Agent::compress_context() (§AutoCompress).
        bool at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
        if (at_threshold) {
            // §BUG-MEMORY-BLOAT: compression only rewrites NON-system
            // messages. When the history is a handful of messages, the
            // pressure comes from the system prompt (session memory dump,
            // project docs, tool schemas) which compression can NEVER
            // reduce — shredding the tiny history gains ~0 tokens and the
            // threshold stays tripped, looping "Compressing" every cycle.
            // Skip and keep working; the provider accepts the request.
            const auto& hist = messages_->get_messages();
            const size_t non_system =
                (!hist.empty() && hist[0].value("role", "") == "system")
                    ? hist.size() - 1
                    : hist.size();
            constexpr size_t kMinAutoCompressMessages = 10;
            if (non_system < kMinAutoCompressMessages) {
                LOG_WARNING("[COMPRESS] At 90% threshold but only " +
                            std::to_string(non_system) +
                            " non-system messages — context is dominated by "
                            "the system prompt, which compression cannot "
                            "reduce. Skipping auto-compress.");
                at_threshold = false;
            }
        }
        const bool host_requested =
            compress_requested_.exchange(false, std::memory_order_relaxed);
        if (at_threshold || host_requested) {
            // §BUG-COMPRESS-LOOP: circuit breaker. Compression that cannot
            // bring the context below the threshold used to retry forever —
            // every attempt reset iteration_count_ (so the max-iterations
            // guard never fired) and `continue`d, looping "Compressing"
            // until the user cancelled. Each failed attempt also blocks up
            // to ~210s in Messages::compress()'s internal retries. Allow
            // kMaxConsecutiveCompressions attempts (a legitimate partial
            // compress converges in 1-2: each success deletes ~70% of the
            // remainder), then end the turn with a terminal error instead.
            static constexpr int kMaxConsecutiveCompressions = 2;
            ++consecutive_compress_count_;
            if (consecutive_compress_count_ > kMaxConsecutiveCompressions) {
                emit_error(hive::ErrorKind::Internal,
                    "Context compression could not bring the conversation "
                    "below the auto-compress threshold after " +
                    std::to_string(kMaxConsecutiveCompressions) +
                    " attempts — ending the turn instead of looping. The "
                    "summary call may be failing, or the model's max context "
                    "size may be set far smaller than the real window "
                    "(check the model configuration); otherwise start a new "
                    "chat.", /*will_retry=*/false);
                final_state = AgentState::DONE;
                break;
            }
            if (host_requested) {
                LOG_INFO("[COMPRESS] Host requested context compression...");
            } else {
                LOG_INFO("[COMPRESS] Auto-triggering context compression at 90% threshold...");
            }
            if (messages_) {
                const size_t before_tokens = last_total_tokens_;
                messages_->set_messages(messages);
                auto compress_result = messages_->compress();
                if (compress_result.outcome == CompressOutcome::Compressed) {
                    messages_->replace_messages(compress_result.compressed_json);
                    messages = messages_->get_messages();
                    const size_t after_tokens = messages_->estimate_tokens();
                    if (sink_) sink_->on_compaction(before_tokens, after_tokens);
                    LOG_INFO("[CONTEXT] Compressed successfully. Continuing...");
                } else if (compress_result.outcome == CompressOutcome::FallbackDeleted) {
                    // §COMPRESS-REPORT: summary failed but the fallback
                    // still reduced the context — report it so host
                    // stats/UI stay in sync.
                    messages = messages_->get_messages();
                    const size_t after_tokens = messages_->estimate_tokens();
                    if (sink_) sink_->on_compaction(before_tokens, after_tokens);
                    LOG_WARNING("[COMPRESS] Summary failed; oldest messages "
                                "deleted as fallback. Continuing...");
                } else {
                    // Nothing to compress, or summary failed AND the
                    // history was too small to safely reduce — the
                    // context is unchanged.
                    LOG_ERROR("[COMPRESS] Failed - context unchanged");
                }
            }
            iteration_count_ = 0;
            last_total_tokens_ = 0;
            last_cached_tokens_ = 0;
            continue;
        }
        // §BUG-COMPRESS-LOOP: this cycle completed below the threshold —
        // the conversation is making progress again.
        consecutive_compress_count_ = 0;

        // §Phase 5.1: route through Logger.
        if (result_state == AgentState::CANCELLED) {
            LOG_INFO("[AGENT] Execution cancelled by user.");
            final_state = AgentState::CANCELLED;
            break;
        }
        if (result_state == AgentState::ACT || result_state == AgentState::BUILD_TEST) {
            continue;
        }

        LOG_INFO("✓ Task Complete");
        final_state = AgentState::DONE;
        break;
    }

    // PERSIST-DIRTY: catch every early-return mutation (API-error
    // injections, dangling-cleanup edits) that didn't mark the
    // chain dirty on its own path.
    if (messages_) {
        messages_->mark_dirty();
    }
    return final_state;
}
