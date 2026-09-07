/**
 * @file hive-brain/ErrorKind.h
 * @brief Classification of LLM/API errors reported via AgentSink::on_error.
 *
 * Part of the public C++ API of the the hive-agent engine static library.
 * Stable across minor versions.
 */
#pragma once

#include <cstdint>

namespace hive {

enum class ErrorKind : std::uint8_t {
    Unknown,      ///< Unclassified error
    Network,      ///< Transport failure (DNS, connect, connection reset)
    Timeout,      ///< Request timed out
    RateLimited,  ///< HTTP 429
    Auth,         ///< HTTP 401/403, invalid_api_key, authentication_error
    ClientError,  ///< Other HTTP 4xx / bad_request_error / context_length_exceeded
    ServerError,  ///< HTTP 5xx / internal_error / empty response
    Parse,        ///< Malformed JSON / SSE received from the provider
    ApiError,     ///< Provider returned a structured {"error": {...}} object
    Internal,     ///< Library-side failure (payload serialization, validation)
};

/// @brief Human-readable name (e.g. "RateLimited"). Never returns nullptr.
const char* to_string(ErrorKind kind) noexcept;

}  // namespace hive
