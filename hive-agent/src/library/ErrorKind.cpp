/**
 * @file ErrorKind.cpp
 * @brief Implementation of hive::to_string(ErrorKind).
 */
#include "types/ErrorKind.h"

namespace hive {

const char* to_string(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::Unknown:     return "Unknown";
        case ErrorKind::Network:     return "Network";
        case ErrorKind::Timeout:     return "Timeout";
        case ErrorKind::RateLimited: return "RateLimited";
        case ErrorKind::Auth:        return "Auth";
        case ErrorKind::ClientError: return "ClientError";
        case ErrorKind::ServerError: return "ServerError";
        case ErrorKind::Parse:       return "Parse";
        case ErrorKind::ApiError:    return "ApiError";
        case ErrorKind::Internal:    return "Internal";
    }
    return "Unknown";
}

}  // namespace hive
