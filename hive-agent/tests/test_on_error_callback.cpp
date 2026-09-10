#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "library/AgentSink.h"
#include "types/ErrorKind.h"
#include "api/ErrorMapping.h"

// §on_error: tests for the structured error channel added to AgentSink.
//  1. The default AgentSink::on_error forwards to on_log(Error) so hosts
//     written before on_error existed keep seeing every error.
//  2. An overriding sink receives kind / message / will_retry verbatim.
//  3. The HTTP-status and provider-error-type → ErrorKind mappings used
//     by LLMClient are correct.

namespace {

struct LogOnlySink : hive::AgentSink {
    struct LogEvent {
        hive::LogLevel level;
        std::string message;
    };
    std::vector<LogEvent> logs;

    void on_log(hive::LogLevel level, std::string_view message) noexcept override {
        logs.push_back({level, std::string(message)});
    }
};

struct CapturingSink : hive::AgentSink {
    struct ErrorEvent {
        hive::ErrorKind kind;
        std::string message;
        bool will_retry;
    };
    std::vector<ErrorEvent> errors;

    void on_error(hive::ErrorKind kind, std::string_view message,
                  bool will_retry) noexcept override {
        errors.push_back({kind, std::string(message), will_retry});
    }
};

}  // namespace

TEST_CASE("to_string(ErrorKind) names every kind") {
    using hive::ErrorKind;
    CHECK(std::string(hive::to_string(ErrorKind::Unknown))     == "Unknown");
    CHECK(std::string(hive::to_string(ErrorKind::Network))     == "Network");
    CHECK(std::string(hive::to_string(ErrorKind::Timeout))     == "Timeout");
    CHECK(std::string(hive::to_string(ErrorKind::RateLimited)) == "RateLimited");
    CHECK(std::string(hive::to_string(ErrorKind::Auth))        == "Auth");
    CHECK(std::string(hive::to_string(ErrorKind::ClientError)) == "ClientError");
    CHECK(std::string(hive::to_string(ErrorKind::ServerError)) == "ServerError");
    CHECK(std::string(hive::to_string(ErrorKind::Parse))       == "Parse");
    CHECK(std::string(hive::to_string(ErrorKind::ApiError))    == "ApiError");
    CHECK(std::string(hive::to_string(ErrorKind::Internal))    == "Internal");
}

TEST_CASE("default AgentSink::on_error forwards to on_log(Error)") {
    LogOnlySink sink;
    sink.on_error(hive::ErrorKind::RateLimited, "HTTP 429 (rate limited)",
                  /*will_retry=*/true);
    REQUIRE(sink.logs.size() == 1);
    CHECK(sink.logs[0].level == hive::LogLevel::Error);
    CHECK(sink.logs[0].message == "HTTP 429 (rate limited)");
}

TEST_CASE("overridden on_error captures kind, message and will_retry verbatim") {
    CapturingSink sink;
    sink.on_error(hive::ErrorKind::Auth, "invalid_api_key: bad key", false);
    sink.on_error(hive::ErrorKind::Network, "connection reset", true);

    REQUIRE(sink.errors.size() == 2);
    CHECK(sink.errors[0].kind == hive::ErrorKind::Auth);
    CHECK(sink.errors[0].message == "invalid_api_key: bad key");
    CHECK(sink.errors[0].will_retry == false);
    CHECK(sink.errors[1].kind == hive::ErrorKind::Network);
    CHECK(sink.errors[1].will_retry == true);
}

TEST_CASE("error_kind_from_http_status maps status classes") {
    using hive::ErrorKind;
    using hive::error_kind_from_http_status;

    CHECK(error_kind_from_http_status(401) == ErrorKind::Auth);
    CHECK(error_kind_from_http_status(403) == ErrorKind::Auth);
    CHECK(error_kind_from_http_status(429) == ErrorKind::RateLimited);
    CHECK(error_kind_from_http_status(400) == ErrorKind::ClientError);
    CHECK(error_kind_from_http_status(404) == ErrorKind::ClientError);
    CHECK(error_kind_from_http_status(422) == ErrorKind::ClientError);
    CHECK(error_kind_from_http_status(500) == ErrorKind::ServerError);
    CHECK(error_kind_from_http_status(503) == ErrorKind::ServerError);
    CHECK(error_kind_from_http_status(200) == ErrorKind::Unknown);
    CHECK(error_kind_from_http_status(0)   == ErrorKind::Unknown);
}

TEST_CASE("error_kind_from_api_type maps provider error types") {
    using hive::ErrorKind;
    using hive::error_kind_from_api_type;

    SUBCASE("auth family") {
        CHECK(error_kind_from_api_type("authentication_error") == ErrorKind::Auth);
        CHECK(error_kind_from_api_type("invalid_api_key")      == ErrorKind::Auth);
        CHECK(error_kind_from_api_type("permission_error")     == ErrorKind::Auth);
        CHECK(error_kind_from_api_type("authorized_error")     == ErrorKind::Auth);
        // §robust-client: Kimi/Moonshot's actual 401 error type — this is
        // what makes its auth failures terminal instead of looping.
        CHECK(error_kind_from_api_type("invalid_authentication_error") == ErrorKind::Auth);
        CHECK(error_kind_from_api_type("invalid_authentication")       == ErrorKind::Auth);
    }
    SUBCASE("rate limit family") {
        CHECK(error_kind_from_api_type("rate_limit")       == ErrorKind::RateLimited);
        CHECK(error_kind_from_api_type("rate_limit_error") == ErrorKind::RateLimited);
    }
    SUBCASE("client error family") {
        CHECK(error_kind_from_api_type("invalid_request")         == ErrorKind::ClientError);
        CHECK(error_kind_from_api_type("bad_request_error")       == ErrorKind::ClientError);
        CHECK(error_kind_from_api_type("context_length_exceeded") == ErrorKind::ClientError);
    }
    SUBCASE("server error family") {
        CHECK(error_kind_from_api_type("internal_error")        == ErrorKind::ServerError);
        CHECK(error_kind_from_api_type("internal_server_error") == ErrorKind::ServerError);
        CHECK(error_kind_from_api_type("server_error")          == ErrorKind::ServerError);
        CHECK(error_kind_from_api_type("service_unavailable")   == ErrorKind::ServerError);
        CHECK(error_kind_from_api_type("service_overloaded")    == ErrorKind::ServerError);
        CHECK(error_kind_from_api_type("empty_response")        == ErrorKind::ServerError);
    }
    SUBCASE("timeout") {
        CHECK(error_kind_from_api_type("timeout") == ErrorKind::Timeout);
    }
    SUBCASE("unrecognised types default to ApiError") {
        CHECK(error_kind_from_api_type("unknown")            == ErrorKind::ApiError);
        CHECK(error_kind_from_api_type("some_new_provider_type") == ErrorKind::ApiError);
    }
}
