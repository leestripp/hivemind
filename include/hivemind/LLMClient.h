#pragma once

#include <string>
#include <functional>
#include <stop_token>
#include <vector>
#include <cstdint>

/**
 * LLMClient — in-process streaming LLM API client.
 *
 * Makes HTTP requests to an OpenAI-compatible chat endpoint.
 * Supports chunked/Server-Sent-Events streaming with a per-token callback.
 *
 * Thread-safety: one LLMClient per Agent Node execution (created per-call).
 * The HTTP request runs on a background thread; callbacks fire on that thread.
 */

struct LLMMessage {
    std::string role;      // "system", "user", "assistant"
    std::string content;  // text content
};

class LLMClient {
public:
    /** Called for each streamed text chunk. */
    using ChunkCallback = std::function<void(std::string_view chunk)>;

    /** Called when a tool_call is detected (for future tool-use). */
    using ToolCallback = std::function<void(std::string_view function_name, std::string_view args_json)>;

    /** Result of a completed (non-streaming) call. */
    struct Result {
        std::string content;         // concatenated text
        int prompt_tokens = 0;
        int completion_tokens = 0;
        int total_tokens = 0;
        std::string error;
        bool ok = true;
    };

    /**
     * @param base_url     e.g. "https://api.minimax.chat"
     * @param api_key      Bearer token
     * @param model        Model ID e.g. "MiniMax-M3"
     * @param endpoint     Chat endpoint e.g. "/v1/chat/completions"
     */
    LLMClient(std::string base_url,
               std::string api_key,
               std::string model,
               std::string endpoint = "/v1/chat/completions");

    /**
     * Streaming completion.
     *
     * @param messages       List of {role, content} messages
     * @param on_chunk       Called for each streamed text delta
     * @param on_tool_call   Called for each tool_call delta (future use)
     * @param stop           Cancellation token
     * @return              Full concatenated content
     */
    std::string stream(const std::vector<LLMMessage>& messages,
                       ChunkCallback on_chunk,
                       ToolCallback on_tool_call = {},
                       std::stop_token stop = {});

    /**
     * Non-streaming completion.
     */
    Result complete(const std::vector<LLMMessage>& messages,
                    std::stop_token stop = {});

    /** Get last response token usage. */
    int prompt_tokens()  const { return prompt_tokens_; }
    int completion_tokens() const { return completion_tokens_; }
    int total_tokens()   const { return total_tokens_; }

    /** Error from last call. */
    const std::string& error() const { return error_; }

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
    std::string endpoint_;

    int prompt_tokens_ = 0;
    int completion_tokens_ = 0;
    int total_tokens_ = 0;
    std::string error_;

    std::string escape_json_string(const std::string& s) const;
};
