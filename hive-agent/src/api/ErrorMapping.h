// SPDX-License-Identifier: BSL-1.0
// src/api/ErrorMapping.h
//
// Internal helpers mapping HTTP status codes and provider error-type
// strings onto the public hive::ErrorKind enum used by
// AgentSink::on_error. Inline so unit tests can exercise the mapping
// without linking LLMClient.cpp (same pattern as is_usage_only_chunk
// in MiniMaxSseParser.h).
//
// Not part of the public libdaedalus API. Do not include from
// include/hive-brain/*.

#pragma once

#include <string_view>

#include "types/ErrorKind.h"

namespace hive {

/// Map an HTTP status code to an ErrorKind.
inline ErrorKind error_kind_from_http_status(long code) noexcept {
    if (code == 401 || code == 403) return ErrorKind::Auth;
    if (code == 429) return ErrorKind::RateLimited;
    if (code >= 400 && code < 500) return ErrorKind::ClientError;
    if (code >= 500) return ErrorKind::ServerError;
    return ErrorKind::Unknown;
}

/// Map a provider error `type` string (from a structured
/// {"error": {"type": ...}} response) to an ErrorKind. Mirrors the
/// classification lists in classify_api_error() (LLMClient.cpp).
/// Unrecognised types default to ApiError ("the provider sent a
/// structured error object").
inline ErrorKind error_kind_from_api_type(std::string_view error_type) noexcept {
    if (error_type == "authentication_error" ||
        error_type == "invalid_authentication_error" ||   // Kimi/Moonshot
        error_type == "invalid_authentication" ||
        error_type == "invalid_api_key" ||
        error_type == "permission_error" ||
        error_type == "authorized_error") {
        return ErrorKind::Auth;
    }
    if (error_type == "rate_limit" ||
        error_type == "rate_limit_error") {
        return ErrorKind::RateLimited;
    }
    if (error_type == "invalid_request" ||
        error_type == "bad_request_error" ||
        error_type == "context_length_exceeded") {
        return ErrorKind::ClientError;
    }
    if (error_type == "internal_error" ||
        error_type == "internal_server_error" ||
        error_type == "server_error" ||
        error_type == "service_unavailable" ||
        error_type == "service_overloaded" ||
        error_type == "empty_response") {
        return ErrorKind::ServerError;
    }
    if (error_type == "timeout") {
        return ErrorKind::Timeout;
    }
    return ErrorKind::ApiError;
}

}  // namespace hive
