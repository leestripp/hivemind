/**
 * @file test_searxng_search.cpp
 * @brief Unit tests for SearxngSearch — the private SearXNG metasearch
 *        client (§SEARXNG-CORE, 2026-09-21).
 *
 * The JSON parser is exercised against canned SearXNG format=json
 * bodies (no network). config_from_env() is driven by setting/unsetting
 * the HIVE_SECRET_SEARXNG_* variables the hub renders from a granted
 * login secret named "searxng" (Phase 60). No live query is made here.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/FileIO.h"
#include "tools/SearxngSearch.h"
#include "tools/SearxngSearchTool.h"

namespace {

void unset_searxng_env() {
    unsetenv("HIVE_SECRET_SEARXNG_URL");
    unsetenv("HIVE_SECRET_SEARXNG_USER");
    unsetenv("HIVE_SECRET_SEARXNG_PASS");
}

struct EnvGuard {
    EnvGuard() { unset_searxng_env(); }
    ~EnvGuard() { unset_searxng_env(); }
};

const char* kBody = R"JSON({
  "query": "hivemind",
  "number_of_results": 3,
  "results": [
    {
      "url": "https://example.org/one",
      "title": "First Result",
      "content": "Snippet one.",
      "engine": "duckduckgo"
    },
    {
      "url": "https://example.org/two",
      "title": "Second Result",
      "engine": "brave"
    },
    {
      "url": "https://example.org/three",
      "title": "Third Result",
      "content": "Snippet three.",
      "engines": ["google", "bing"]
    }
  ],
  "answers": [],
  "infoboxes": []
})JSON";

} // namespace

TEST_CASE("parse extracts title/url/content from a SearXNG response") {
    const auto results = SearxngSearch::parse(kBody, 25);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 3);
    CHECK((*results)[0].title == "First Result");
    CHECK((*results)[0].url == "https://example.org/one");
    CHECK((*results)[0].snippet == "Snippet one.");
    // Missing content is tolerated with an empty snippet.
    CHECK((*results)[1].title == "Second Result");
    CHECK((*results)[1].snippet.empty());
    CHECK((*results)[2].snippet == "Snippet three.");
}

TEST_CASE("parse honours max_results") {
    const auto results = SearxngSearch::parse(kBody, 2);
    REQUIRE(results.has_value());
    CHECK(results->size() == 2);
    CHECK(results->back().url == "https://example.org/two");
}

TEST_CASE("parse skips non-object items and results without title/url") {
    const std::string body = R"JSON({"results": [
        "not an object",
        {"title": "No URL"},
        {"url": "https://example.org/no-title"},
        {"url": "https://example.org/ok", "title": "OK", "content": "c"}
    ]})JSON";
    const auto results = SearxngSearch::parse(body, 25);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 1);
    CHECK((*results)[0].title == "OK");
}

TEST_CASE("parse tolerates non-string fields (no type_error.306)") {
    // title as an ARRAY must be skipped, not throw — the 2026-09-02
    // nlohmann value() incident class.
    const std::string body = R"JSON({"results": [
        {"url": "https://example.org/a", "title": ["array"], "content": 42},
        {"url": "https://example.org/b", "title": "Real", "content": "x"}
    ]})JSON";
    const auto results = SearxngSearch::parse(body, 25);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 1);
    CHECK((*results)[0].url == "https://example.org/b");
}

TEST_CASE("parse rejects invalid JSON and a missing results array") {
    CHECK(!SearxngSearch::parse("not json", 10).has_value());
    CHECK(!SearxngSearch::parse("{}", 10).has_value());
    // results present but not an array → error, never a throw.
    CHECK(!SearxngSearch::parse(R"({"results": "nope"})", 10).has_value());
    // Empty results array is a valid, empty answer.
    const auto empty = SearxngSearch::parse(R"({"results": []})", 10);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("config_from_env defaults and password requirement") {
    const EnvGuard guard;

    SUBCASE("missing password is a descriptive error") {
        const auto cfg = SearxngSearch::config_from_env();
        REQUIRE(!cfg.has_value());
        CHECK(cfg.error().find("searxng") != std::string::npos);
        CHECK(cfg.error().find("HIVE_SECRET_SEARXNG_PASS") !=
              std::string::npos);
    }

    SUBCASE("password set → defaults for url and user") {
        setenv("HIVE_SECRET_SEARXNG_PASS", "s3cret", 1);
        const auto cfg = SearxngSearch::config_from_env();
        REQUIRE(cfg.has_value());
        CHECK(cfg->base_url == "https://leestrippai.com:8443");
        CHECK(cfg->username == "agent");
        CHECK(cfg->password == "s3cret");
    }

    SUBCASE("overrides apply; trailing slashes are stripped") {
        setenv("HIVE_SECRET_SEARXNG_URL", "https://search.internal:9443//", 1);
        setenv("HIVE_SECRET_SEARXNG_USER", "svc", 1);
        setenv("HIVE_SECRET_SEARXNG_PASS", "s3cret", 1);
        const auto cfg = SearxngSearch::config_from_env();
        REQUIRE(cfg.has_value());
        CHECK(cfg->base_url == "https://search.internal:9443");
        CHECK(cfg->username == "svc");
    }

    SUBCASE("plain http base URL is rejected") {
        setenv("HIVE_SECRET_SEARXNG_URL", "http://leestrippai.com:8443", 1);
        setenv("HIVE_SECRET_SEARXNG_PASS", "s3cret", 1);
        const auto cfg = SearxngSearch::config_from_env();
        REQUIRE(!cfg.has_value());
        CHECK(cfg.error().find("https://") != std::string::npos);
    }
}

TEST_CASE("searxng_search tool definition is a strict-schema object") {
    const SearxngSearchTool tool;
    CHECK(tool.get_name() == "searxng_search");
    const json def = tool.get_definition();
    REQUIRE(def.contains("function"));
    const auto& fn = def["function"];
    CHECK(fn["name"] == "searxng_search");
    // Phase 60c: properties must be an OBJECT or strict providers 400.
    REQUIRE(fn["parameters"].contains("properties"));
    CHECK(fn["parameters"]["properties"].is_object());
    CHECK(fn["parameters"]["properties"].contains("query"));
    CHECK(fn["parameters"]["properties"].contains("max_results"));
    CHECK(fn["parameters"]["required"] == json::array({"query"}));
}

TEST_CASE("searxng_search execute validates args and reports missing grant") {
    const EnvGuard guard;
    SearxngSearchTool tool;
    FileIO file_io(std::filesystem::temp_directory_path());

    SUBCASE("missing query") {
        const json out = tool.execute(json::object(), file_io);
        CHECK(out["status"] == "error");
    }

    SUBCASE("type-mismatched args are guarded, not thrown") {
        // query as number / max_results as string must produce a clean
        // error (or be ignored), never a type_error escaping execute().
        const json out = tool.execute(
            {{"query", 42}, {"max_results", "lots"}}, file_io);
        CHECK(out["status"] == "error");
        CHECK(out["error"] == "missing query");
    }

    SUBCASE("no granted secret → descriptive error, never a throw") {
        const json out = tool.execute({{"query", "test"}}, file_io);
        CHECK(out["status"] == "error");
        CHECK(out["error"].get<std::string>().find(
                  "HIVE_SECRET_SEARXNG_PASS") != std::string::npos);
    }
}
