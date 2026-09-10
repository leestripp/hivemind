/**
 * @file test_tool_factory_dedup.cpp
 * @brief Regression tests for the duplicate-tool-registration bug reported
 *        by the MonkeyByte host (daedalus_bugs.md §1).
 *
 * ToolFactory::register_tool() used to be a bare push_back. Hosts that
 * rebuild their Agent (model switch, workspace change, session reset)
 * re-register host tools per build, so from the second instance onward the
 * chat payload contained the same function definition twice and strict
 * providers rejected every request with:
 *   HTTP 400 invalid_request_error: function name X is duplicated
 *
 * The fix makes register_tool() replace-by-name and adds unregister_tool().
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "core/FileIO.h"
#include "tools/ToolFactory.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("hive_agent_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Minimal tool whose execute() echoes a marker so tests can tell which
// instance actually ran.
class DummyTool : public BaseTool {
public:
    DummyTool(std::string name, std::string marker)
        : name_(std::move(name)), marker_(std::move(marker)) {}

    std::string get_name() const override { return name_; }
    std::string get_description() const override { return "dummy " + name_; }
    json get_definition() const override {
        return {{"type", "function"},
                {"function", {{"name", name_},
                              {"description", get_description()},
                              {"parameters", {{"type", "object"},
                                              {"properties", json::object()}}}}}};
    }
    json execute(const json&, FileIO&) override {
        return {{"status", "ok"}, {"marker", marker_}};
    }

private:
    std::string name_;
    std::string marker_;
};

size_t count_named(const ToolFactory& f, const std::string& name) {
    const auto names = f.get_tool_names();
    return static_cast<size_t>(
        std::count(names.begin(), names.end(), name));
}

} // namespace

TEST_SUITE("ToolFactory dedup") {

    TEST_CASE("re-registering the same name replaces instead of duplicating") {
        ToolFactory factory;
        const size_t baseline = factory.get_tool_names().size();

        factory.register_tool(std::make_shared<DummyTool>("app_ui_control", "first"));
        CHECK(count_named(factory, "app_ui_control") == 1);

        // Simulate the host rebuilding its agent and re-registering.
        factory.register_tool(std::make_shared<DummyTool>("app_ui_control", "second"));
        factory.register_tool(std::make_shared<DummyTool>("app_ui_control", "third"));

        // Still exactly one definition — no duplicated function name.
        CHECK(count_named(factory, "app_ui_control") == 1);
        CHECK(factory.get_tool_names().size() == baseline + 1);

        size_t def_count = 0;
        for (const auto& def : factory.get_all_definitions()) {
            if (def.contains("function") &&
                def["function"].value("name", "") == "app_ui_control") {
                ++def_count;
            }
        }
        CHECK(def_count == 1);
    }

    TEST_CASE("replacement dispatches to the newest instance") {
        ToolFactory factory;
        ScratchDir scratch("toolfactory-dedup");
        FileIO file_io(scratch.path);

        factory.register_tool(std::make_shared<DummyTool>("host_tool", "stale"));
        factory.register_tool(std::make_shared<DummyTool>("host_tool", "fresh"));

        const json result = factory.execute_tool("host_tool", json::object(), file_io);
        CHECK(result.value("marker", "") == "fresh");
    }

    TEST_CASE("registration order is preserved on replace") {
        ToolFactory factory;
        factory.register_tool(std::make_shared<DummyTool>("zz_last", "v1"));
        factory.register_tool(std::make_shared<DummyTool>("aa_first", "v1"));

        const auto before = factory.get_tool_names();
        factory.register_tool(std::make_shared<DummyTool>("zz_last", "v2"));
        const auto after = factory.get_tool_names();

        CHECK(before.size() == after.size());
        // Replacing must not move the slot to the end.
        CHECK(after == before);
    }

    TEST_CASE("unregister_tool removes by name") {
        ToolFactory factory;
        factory.register_tool(std::make_shared<DummyTool>("ephemeral", "v1"));
        REQUIRE(factory.has_tool("ephemeral"));

        CHECK(factory.unregister_tool("ephemeral"));
        CHECK(!factory.has_tool("ephemeral"));
        CHECK(factory.get_tool("ephemeral") == nullptr);

        // Unknown name: no-op, returns false.
        CHECK(!factory.unregister_tool("ephemeral"));

        // Re-register after unregister works (append path).
        factory.register_tool(std::make_shared<DummyTool>("ephemeral", "v2"));
        CHECK(factory.has_tool("ephemeral"));
    }

    TEST_CASE("null tool is ignored") {
        ToolFactory factory;
        const size_t before = factory.get_tool_names().size();
        factory.register_tool(nullptr);
        CHECK(factory.get_tool_names().size() == before);
    }
}
