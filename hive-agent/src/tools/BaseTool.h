#pragma once

#include <functional>
#include <string>
#include <nlohmann/json.hpp>

class FileIO;  // Forward declaration

using json = nlohmann::json;

/**
 * ToolProgressFn — Optional callback a tool can invoke to report
 * progress during a (potentially slow) execution. The library wires
 * this through to AgentSink::on_tool_progress so hosts (CLI, webui,
 * TUI) can show streaming feedback instead of waiting in silence.
 *
 * The `progress` JSON shape is:
 *   {
 *     "stage":   "starting" | "executing" | "output" | "finalizing" | "done",
 *     "percent": 0..100,           // optional
 *     "message": "human-readable", // optional
 *     "output_chunk": "..."        // optional, sanitised live stdout/stderr
 *   }
 *
 * The "output_chunk" stage is used by tools such as execute_bash to stream
 * live command output to the host while a long-running command is still
 * executing. The chunk is already sanitised (TTY control sequences stripped,
 * UTF-8 and ANSI SGR colour/style preserved).
 *
 * Tools MAY ignore the callback entirely. Tools that do emit progress
 * should avoid flooding the host; output chunks are rate-limited by the
 * emitting tool (e.g. on line boundaries and size thresholds).
 */
using ToolProgressFn = std::function<void(const json& progress)>;

/**
 * BaseTool - Abstract base class for all tools
 * Each tool implements execute() to perform its operation
 */
class BaseTool {
public:
    virtual ~BaseTool() = default;

    /**
     * Get the name of this tool
     */
    [[nodiscard]] virtual std::string get_name() const = 0;

    /**
     * Get the description of this tool
     */
    [[nodiscard]] virtual std::string get_description() const = 0;

    /**
     * Get the tool definition in JSON format for LLM
     */
    [[nodiscard]] virtual json get_definition() const = 0;

    /**
     * Execute the tool with given arguments.
     *
     * This is the primary interface — every concrete tool overrides
     * this. The library always calls the 3-arg overload below; tools
     * that don't care about progress simply never receive it (the
     * default 3-arg delegates here and discards `progress`).
     *
     * @param args     JSON object containing tool arguments
     * @param file_io  Reference to FileIO for file operations
     * @return         JSON result of the tool execution
     */
    virtual json execute(const json& args, FileIO& file_io) = 0;

    /**
     * Execute with an optional progress callback.
     *
     * Default implementation delegates to the 2-arg execute() and
     * discards `progress`. Tools that want to emit streaming
     * progress should override THIS overload instead — that way they
     * receive the callback and can call it freely.
     *
     * If a tool overrides only the 2-arg version (the common case),
     * the library's call to this 3-arg version will hit this default
     * which virtual-dispatches back to the tool's 2-arg override.
     */
    virtual json execute(const json& args, FileIO& file_io, ToolProgressFn progress)
    {
        (void)progress;
        return execute(args, file_io);
    }

protected:
    BaseTool() = default;
};