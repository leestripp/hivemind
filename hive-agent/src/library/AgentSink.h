/**
 * @file hive-brain/AgentSink.h
 * @brief Callback interface that host applications implement to receive
 *        events from the agent.
 *
 * AgentSink is the *only* way the library talks to the outside world.
 * All logging, streaming text, tool lifecycle, state transitions, and
 * host events flow through here.
 *
 * Default implementations of every method are no-ops (or sane defaults),
 * so hosts only override what they care about. The methods marked
 * "MANDATORY" in the comments below are not — they all have defaults —
 * but a typical host will want to override on_log at minimum.
 *
 * Threading: most methods are called on the agent's worker thread. The
 * streaming methods on_assistant_delta and on_llm_response_started may be
 * called from the HTTP worker thread that receives SSE chunks. Hosts that
 * need to touch UI frameworks with thread affinity (GTK, Qt) MUST marshal
 * the call to the UI thread before acting.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "types/ErrorKind.h"
#include "types/LogLevel.h"

namespace hive {

class AgentSink {
public:
    virtual ~AgentSink() = default;

    // ------------------------------------------------------------------
    // Streaming output
    // ------------------------------------------------------------------

    /// @brief A chunk of assistant text streamed from the LLM.
    /// Called repeatedly as tokens arrive. Empty chunks are possible.
    virtual void on_assistant_delta(std::string_view text) noexcept
    {
        (void)text;
    }


    // ------------------------------------------------------------------
    // Tool lifecycle
    // ------------------------------------------------------------------

    /// @brief A tool call is about to be executed. `call_id` is the
    /// OpenAI-style identifier for this specific invocation (empty
    /// string if the caller has no id); sinks that want to correlate
    /// `on_tool_progress` / `on_tool_result` events with the
    /// originating call should key off it.
    virtual void on_tool_call(std::string_view name,
                              std::string_view call_id,
                              const nlohmann::json& args) noexcept
    {
        (void)call_id;
        return on_tool_call(name, args);   // default delegates to 2-arg
    }

    /// @brief Back-compat 2-arg overload. Sinks that don't need the
    /// call_id can keep overriding only this version — the 3-arg
    /// default above forwards here.
    virtual void on_tool_call(std::string_view name,
                              const nlohmann::json& args) noexcept
    {
        (void)name; (void)args;
    }

    /// @brief A tool call has finished (success or failure indicated in result).
    virtual void on_tool_result(std::string_view name,
                                std::string_view call_id,
                                const nlohmann::json& result) noexcept
    {
        (void)call_id;
        return on_tool_result(name, result);
    }

    /// @brief Back-compat 2-arg overload.
    virtual void on_tool_result(std::string_view name,
                                const nlohmann::json& result) noexcept
    {
        (void)name; (void)result;
    }

    /// @brief A tool emitted a progress event during execution. Only
    /// tools that opt in (by overriding the 3-arg `BaseTool::execute`
    /// overload) emit this — for them, it fires between
    /// `on_tool_call` and `on_tool_result`. Default: no-op.
    ///
    /// `progress` JSON shape:
    ///   {
    ///     "stage":   "starting" | "executing" | "output" | "finalizing" | "done",
    ///     "percent": 0..100,            // optional
    ///     "message": "human-readable",  // optional
    ///     "output_chunk": "..."         // optional, live sanitised output
    ///   }
    ///
    /// The "output_chunk" stage is emitted by execute_bash (and potentially
    /// other long-running tools) to stream live stdout/stderr to the host.
    /// The chunk has already been sanitised so it is safe to print to a TTY.
    virtual void on_tool_progress(std::string_view name,
                                  std::string_view call_id,
                                  const nlohmann::json& progress) noexcept
    {
        (void)call_id;
        return on_tool_progress(name, progress);
    }

    /// @brief Back-compat 2-arg overload.
    virtual void on_tool_progress(std::string_view name,
                                  const nlohmann::json& progress) noexcept
    {
        (void)name; (void)progress;
    }

    // ------------------------------------------------------------------
    // LLM request lifecycle
    // ------------------------------------------------------------------

    /// @brief The agent has dispatched an LLM request and is now waiting
    /// for the response. Hosts can use this to start a "thinking" spinner.
    /// Always paired with a later on_llm_request_finished().
    virtual void on_llm_wait_started() noexcept {}

    /// @brief The LLM has started responding. For streaming responses this
    /// fires on the first token/chunk; for non-streaming responses it fires
    /// when the complete response body has been received. Hosts can use
    /// this to switch from a "waiting" spinner to streaming output.
    virtual void on_llm_response_started() noexcept {}

    /// @brief The LLM request/response cycle has ended, regardless of
    /// whether it succeeded, failed, or was cancelled. This always pairs
    /// with a preceding on_llm_wait_started().
    virtual void on_llm_request_finished() noexcept {}

    // ------------------------------------------------------------------
    // State / lifecycle
    // ------------------------------------------------------------------

    /// @brief Agent transitioned from one state to another. `to` is the
    /// cast-to-int value of the new AgentState.
    virtual void on_state_change(int /*from*/, int /*to*/) noexcept {}

    /// @brief An LLM/API error occurred. This is the structured error
    /// channel: unlike on_log (free-form diagnostics), on_error carries a
    /// machine-readable ErrorKind so hosts can *act* on failures — show an
    /// error banner, back off, switch model, or abort the session.
    ///
    /// `will_retry` is true when the library will attempt the request again
    /// (transient failure); false means the failure is terminal for this
    /// turn. Terminal LLM failures still end the turn with public
    /// AgentState::Done (the internal state machine has no error state), so
    /// hosts that need to know a turn failed MUST track on_error.
    ///
    /// Threading: may be called on the HTTP worker thread (same rule as
    /// on_assistant_delta) as well as the agent worker thread.
    ///
    /// Default: forwards to on_log(LogLevel::Error, message), so existing
    /// hosts that only override on_log see identical output.
    virtual void on_error(hive::ErrorKind kind,
                          std::string_view message,
                          bool will_retry) noexcept;

    /// @brief Free-form log message from the agent.
    /// Default: writes to stderr. Hosts typically override this.
    virtual void on_log(hive::LogLevel level,
                        std::string_view message) noexcept;

    /// @brief Context was just compacted.
    virtual void on_compaction(std::size_t /*before_tokens*/,
                               std::size_t /*after_tokens*/) noexcept {}

    /// @brief Agent wants the terminal to flash / system to beep.
    /// Default: no-op. CLI/TUI may ring the bell, web may play a sound.
    virtual void on_bell() noexcept {}

};

}  // namespace hive
