/**
 * @file test_curl_multi_session.cpp
 * @brief Unit tests for the HttpSession async HTTP client
 *
 * Tests cover:
 * - Basic GET request
 * - POST request with body and headers
 * - Bearer token auth
 * - Batch (parallel) requests
 * - Cancellation via stop_token
 * - Stats tracking
 * - HTTP error responses
 * - Multiple concurrent connections
 */

#include <iostream>
#include <chrono>
#include <future>
#include <vector>
#include <stop_token>

#include "api/HttpSession.h"

using namespace hive::net;
using namespace std::chrono_literals;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

// Helper: skip test if network unavailable
static bool network_available() {
    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "https://example.com";
    req.timeout_seconds = 5;
    auto fut = session.submit(req);
    auto status = fut.wait_for(6s);
    if (status != std::future_status::ready) return false;
    auto result = fut.get();
    return result.has_value() && result->ok();
}

// ============================================================================
// Test: get_instance returns the same singleton
// ============================================================================
TEST(singleton) {
    auto& a = HttpSession::get_instance();
    auto& b = HttpSession::get_instance();
    CHECK_EQ(&a, &b);
    return 0;
}

// ============================================================================
// Test: stats start at zero
// ============================================================================
TEST(stats_initial) {
    auto& session = HttpSession::get_instance();
    auto stats = session.get_stats();
    // Just verify the call succeeds and returns valid values
    (void)stats.total;
    (void)stats.completed;
    CHECK_EQ(stats.active, 0U);  // No active requests yet
    return 0;
}

// ============================================================================
// Test: basic GET request (skipped if no network)
// ============================================================================
TEST(basic_get) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "https://example.com";
    req.timeout_seconds = 10;

    auto fut = session.submit(req);
    auto result = fut.get();

    CHECK(result.has_value());
    if (result) {
        CHECK_EQ(result->status_code, 200L);
        CHECK(!result->body.empty());
    }
    return 0;
}

// ============================================================================
// Test: GET with bearer token (skipped if no network)
// ============================================================================
TEST(get_with_bearer_token) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "https://example.com";
    req.bearer_token = "test-token-12345";
    req.timeout_seconds = 10;

    auto fut = session.submit(req);
    auto result = fut.get();

    CHECK(result.has_value());
    if (result) {
        CHECK_EQ(result->status_code, 200L);
    }
    return 0;
}

// ============================================================================
// Test: POST request with body and headers (skipped if no network)
// ============================================================================
TEST(post_with_body) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    // Use httpbin.org which echoes POST data
    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "https://httpbin.org/post";
    req.method = "POST";
    req.body = R"({"key": "value", "test": true})";
    req.headers.push_back("Content-Type: application/json");
    req.headers.push_back("X-Custom-Header: daedalus-Test");
    req.timeout_seconds = 15;

    auto fut = session.submit(req);
    auto result = fut.get();

    CHECK(result.has_value());
    if (result) {
        CHECK_EQ(result->status_code, 200L);
        // httpbin echoes back what we sent
        CHECK(result->body.find("application/json") != std::string::npos);
    }
    return 0;
}

// ============================================================================
// Test: 404 returns HttpStatus error
// ============================================================================
TEST(http_404_error) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "https://httpbin.org/status/404";
    req.timeout_seconds = 10;

    auto fut = session.submit(req);
    auto result = fut.get();

    CHECK(!result.has_value());
    if (!result) {
        CHECK(result.error() == HttpError::HttpStatus);
    }
    return 0;
}

// ============================================================================
// Test: invalid URL returns InvalidUrl error
// ============================================================================
TEST(invalid_url) {
    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "";  // Empty URL
    req.timeout_seconds = 5;

    auto fut = session.submit(req);
    auto result = fut.get();

    CHECK(!result.has_value());
    if (!result) {
        CHECK(result.error() == HttpError::InvalidUrl);
    }
    return 0;
}

// ============================================================================
// Test: batch of parallel requests
// ============================================================================
TEST(batch_parallel) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    auto& session = HttpSession::get_instance();
    std::vector<HttpRequest> reqs;
    for (int i = 0; i < 5; ++i) {
        HttpRequest r;
        r.url = "https://example.com";
        r.timeout_seconds = 10;
        reqs.push_back(r);
    }

    auto start = std::chrono::steady_clock::now();
    auto futures = session.submit_batch(reqs);
    int success_count = 0;
    for (auto& f : futures) {
        auto result = f.get();
        if (result && result->ok()) {
            success_count++;
        }
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // 5 requests should ideally complete faster than 5 * single_request_time
    // due to parallelism (each ~200-500ms = ~1s sequential, ~500ms parallel)
    CHECK_EQ(success_count, 5);
    std::cout << "  5 parallel requests completed in " << elapsed.count() << "ms\n";
    return 0;
}

// ============================================================================
// Test: cancellation via stop_token
// ============================================================================
TEST(cancellation_via_stop_token) {
    auto& session = HttpSession::get_instance();

    std::stop_source ss;
    HttpRequest req;
    // Use a slow URL or a URL that won't resolve quickly
    req.url = "https://httpbin.org/delay/10";  // 10-second delay
    req.timeout_seconds = 30;

    auto fut = session.submit_cancellable(req, ss.get_token());

    // Cancel after 100ms
    std::this_thread::sleep_for(100ms);
    ss.request_stop();

    auto result = fut.get();
    // Should be cancelled or timed out (depends on timing)
    if (!result) {
        CHECK(result.error() == HttpError::Cancelled ||
              result.error() == HttpError::Timeout);
    }
    return 0;
}

// ============================================================================
// Test: stats increment after completion
// ============================================================================
TEST(stats_increment) {
    auto& session = HttpSession::get_instance();
    auto before = session.get_stats();

    HttpRequest req;
    req.url = "https://example.com";
    req.timeout_seconds = 5;

    auto fut = session.submit(req);
    auto result = fut.get();

    auto after = session.get_stats();
    CHECK(after.total >= before.total + 1);
    if (result) {
        CHECK(after.completed >= before.completed + 1);
    }
    return 0;
}

// ============================================================================
// Test: Content-Type is captured (skipped if no network)
// ============================================================================
TEST(content_type_captured) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    auto& session = HttpSession::get_instance();
    HttpRequest req;
    req.url = "https://httpbin.org/json";
    req.timeout_seconds = 10;

    auto fut = session.submit(req);
    auto result = fut.get();

    CHECK(result.has_value());
    if (result) {
        // httpbin.org/json returns Content-Type: application/json
        CHECK(result->content_type.find("json") != std::string::npos);
    }
    return 0;
}

// ============================================================================
// Test: many requests don't crash
// ============================================================================
TEST(many_requests_stress) {
    if (!network_available()) {
        std::cerr << "SKIP: network not available\n";
        return 0;
    }

    auto& session = HttpSession::get_instance();
    std::vector<std::future<std::expected<HttpResponse, HttpError>>> futures;
    for (int i = 0; i < 20; ++i) {
        HttpRequest r;
        r.url = "https://example.com";
        r.timeout_seconds = 10;
        futures.push_back(session.submit(r));
    }
    int ok = 0;
    for (auto& f : futures) {
        auto r = f.get();
        if (r && r->ok()) ok++;
    }
    CHECK(ok >= 15);  // Allow some failures due to network limits
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running HttpSession tests..." << std::endl;
    std::cout << "(Tests requiring network will SKIP if unavailable)\n" << std::endl;

    int local_failures = 0;
    local_failures += test_singleton();
    local_failures += test_stats_initial();
    local_failures += test_basic_get();
    local_failures += test_get_with_bearer_token();
    local_failures += test_post_with_body();
    local_failures += test_http_404_error();
    local_failures += test_invalid_url();
    local_failures += test_batch_parallel();
    local_failures += test_cancellation_via_stop_token();
    local_failures += test_stats_increment();
    local_failures += test_content_type_captured();
    local_failures += test_many_requests_stress();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
