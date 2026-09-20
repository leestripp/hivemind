/**
 * @file test_tool_definitions.cpp
 * @brief Unit tests for ToolHandler tool definitions
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
// Include ToolHandler after doctest
#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"
#include "tools/BrowserTools.h"
#include "tools/SkillTool.h"
#include "core/SkillManager.h"
#include "core/FileIO.h"

using json = nlohmann::json;

TEST_SUITE("ToolDefinitions") {
    
    TEST_CASE("get_tool_definitions returns non-empty string") {
        std::string defs = ToolHandler::get_tool_definitions();
        CHECK(!defs.empty());
    }
    
    TEST_CASE("get_tool_definitions returns valid JSON") {
        std::string defs = ToolHandler::get_tool_definitions();
        REQUIRE_NOTHROW(
            json parsed = json::parse(defs);
            CHECK(parsed.is_array());
        );
    }
    
    TEST_CASE("get_tool_definitions contains expected tools") {
        std::string defs = ToolHandler::get_tool_definitions();
        json parsed = json::parse(defs);

        // Track tool names found
        std::vector<std::string> tool_names;
        for (const auto& item : parsed) {
            if (item.contains("function") && item["function"].contains("name")) {
                tool_names.push_back(item["function"]["name"]);
            }
        }

        // §TOOL-TRIM (prompt.txt): the default library surface is minimal.
        // Only core agent tools are registered by ToolFactory. File / git /
        // search / system-info / background / network tools live in
        // demos/example-tools/ and are registered by hosts that need them.
        CHECK(tool_names.size() >= 8);

        // Check for core tools we expect
        bool has_execute_bash = false;
        bool has_tell_user = false;
        bool has_stop = false;
        bool has_set_status = false;

        for (const auto& name : tool_names) {
            if (name == "execute_bash") has_execute_bash = true;
            if (name == "tell_user") has_tell_user = true;
            if (name == "stop") has_stop = true;
            if (name == "set_status") has_set_status = true;
        }

        CHECK(has_execute_bash);
        CHECK(has_tell_user);
        CHECK(has_stop);
        CHECK(has_set_status);

        // §COMPRESS-TOOL (2026-08-14): compress_context is back as a
        // model-facing tool. It only SCHEDULES compression — the actual
        // work runs at the cycle-boundary checkpoint, same path as the
        // 90% threshold and Agent::compress_context() triggers.
        CHECK(std::find(tool_names.begin(), tool_names.end(), "compress_context") != tool_names.end());

        // §PDF-CORE (2026-08-31): read_pdf is a built-in again
        // (poppler-cpp is a REQUIRED dependency of the library build).
        CHECK(std::find(tool_names.begin(), tool_names.end(), "read_pdf") != tool_names.end());

        // Non-core tools are NOT in the default set.
        CHECK(std::find(tool_names.begin(), tool_names.end(), "read_file") == tool_names.end());
        CHECK(std::find(tool_names.begin(), tool_names.end(), "create_file") == tool_names.end());
        CHECK(std::find(tool_names.begin(), tool_names.end(), "edit_file_patch") == tool_names.end());
    }
    
    TEST_CASE("get_tool_definitions has required fields for each tool") {
        std::string defs = ToolHandler::get_tool_definitions();
        json parsed = json::parse(defs);
        
        for (const auto& item : parsed) {
            // Each tool should have function with name and description
            REQUIRE(item.contains("type"));
            CHECK(item["type"] == "function");
            
            REQUIRE(item.contains("function"));
            const json& func = item["function"];
            
            REQUIRE(func.contains("name"));
            REQUIRE(func.contains("description"));
            REQUIRE(!func["name"].get<std::string>().empty());
            REQUIRE(!func["description"].get<std::string>().empty());
            
            // If parameters exist, they should be an object
            if (func.contains("parameters")) {
                CHECK(func["parameters"].is_object());
                // §MOONSHOT-SCHEMA: providers validate strictly — a
                // mis-braced nlohmann literal silently turns properties
                // into an ARRAY and Moonshot 400s EVERY turn with
                // "properties must be an object" (Buzz mute incident,
                // 2026-09-15). Catch the class, not the instance.
                if (func["parameters"].contains("properties"))
                    CHECK(func["parameters"]["properties"].is_object());
                if (func["parameters"].contains("required"))
                    CHECK(func["parameters"]["required"].is_array());
            }
        }
    }
    
    TEST_CASE("browser tool schemas are provider-valid (§MOONSHOT-SCHEMA)") {
        // 2026-09-15: browser_fill_secret's mis-braced definition made
        // parameters.properties a LIST — Moonshot rejected every request
        // and the agent silently died turn after turn. Validate the shape
        // of every browser tool (specs + fill_secret) directly.
        std::vector<json> defs;
        for (const auto& spec : hive::BrowserTool::specs())
            defs.push_back(hive::BrowserTool(spec).get_definition());
        defs.push_back(hive::BrowserFillSecretTool().get_definition());

        REQUIRE(defs.size() > 5);
        for (const auto& def : defs) {
            const json& func = def["function"];
            CAPTURE(func["name"].get<std::string>());
            REQUIRE(func.contains("parameters"));
            const json& params = func["parameters"];
            CHECK(params.is_object());
            CHECK(params.value("type", "") == "object");
            REQUIRE(params.contains("properties"));
            CHECK(params["properties"].is_object());
            for (const auto& [key, prop] : params["properties"].items()) {
                CAPTURE(key);
                CHECK(prop.is_object());
                CHECK(prop.contains("type"));
            }
            if (params.contains("required"))
                CHECK(params["required"].is_array());
        }
        // The exact regression: fill_secret must expose all three fields.
        const json& fill = defs.back()["function"]["parameters"];
        CHECK(fill["properties"].contains("selector"));
        CHECK(fill["properties"].contains("secret_name"));
        CHECK(fill["properties"].contains("field"));
        CHECK(fill["properties"]["field"]["enum"] ==
              json::array({"username", "password"}));
    }

    TEST_CASE("compress_context tool definition is well-formed (§COMPRESS-TOOL)") {
        std::string defs = ToolHandler::get_tool_definitions();
        json parsed = json::parse(defs);

        bool found = false;
        for (const auto& item : parsed) {
            if (item["function"]["name"] != "compress_context") continue;
            found = true;
            const json& func = item["function"];
            CHECK(!func["description"].get<std::string>().empty());
            REQUIRE(func.contains("parameters"));
            CHECK(func["parameters"]["type"] == "object");
            REQUIRE(func["parameters"]["properties"].contains("reason"));
            CHECK(func["parameters"]["properties"]["reason"]["type"] == "string");
            REQUIRE(func["parameters"].contains("required"));
            CHECK(func["parameters"]["required"] == json::array({"reason"}));
        }
        CHECK(found);
    }

    TEST_CASE("get_tool_definitions_filtered excludes specified tools") {
        std::string all_defs = ToolHandler::get_tool_definitions();
        json all_parsed = json::parse(all_defs);
        
        // Filter out execute_bash
        std::string filtered_defs = ToolHandler::get_tool_definitions_filtered({"execute_bash"});
        json filtered_parsed = json::parse(filtered_defs);
        
        // Filtered should have fewer tools
        CHECK(filtered_parsed.size() < all_parsed.size());
        
        // Verify execute_bash is NOT in filtered
        for (const auto& item : filtered_parsed) {
            if (item.contains("function") && item["function"].contains("name")) {
                std::string name = item["function"]["name"];
                CHECK(name != "execute_bash");
            }
        }
    }
    
    TEST_CASE("get_tool_definitions_filtered with multiple exclusions") {
        std::string filtered_defs = ToolHandler::get_tool_definitions_filtered({
            "execute_bash", "tell_user", "compress_context"
        });
        json filtered = json::parse(filtered_defs);

        std::vector<std::string> names;
        for (const auto& item : filtered) {
            if (item.contains("function") && item["function"].contains("name")) {
                names.push_back(item["function"]["name"]);
            }
        }

        CHECK(std::find(names.begin(), names.end(), "execute_bash") == names.end());
        CHECK(std::find(names.begin(), names.end(), "tell_user") == names.end());
        CHECK(std::find(names.begin(), names.end(), "compress_context") == names.end());
    }

    TEST_CASE("matches_pattern wildcard semantics (§FIX-leading-wildcard)") {
        // Exact and prefix (no wildcard)
        CHECK(ToolHandler::matches_pattern("git status", "git status") == true);
        CHECK(ToolHandler::matches_pattern("git status -s", "git status") == true);

        // Pure wildcard matches everything
        CHECK(ToolHandler::matches_pattern("anything at all", "*") == true);
        CHECK(ToolHandler::matches_pattern("ls -la", "***") == true);

        // Trailing wildcard = prefix match
        CHECK(ToolHandler::matches_pattern("rm -rf build", "rm -rf *") == true);
        CHECK(ToolHandler::matches_pattern("ls build", "rm -rf *") == false);

        // Leading wildcard = suffix match
        CHECK(ToolHandler::matches_pattern("cat notes.txt", "*.txt") == true);
        CHECK(ToolHandler::matches_pattern("cat notes.md", "*.txt") == false);

        // Leading + trailing wildcard = contains match
        CHECK(ToolHandler::matches_pattern("git status -s", "*status*") == true);
        CHECK(ToolHandler::matches_pattern("git log", "*status*") == false);

        // Anchored on both ends with middle parts in order
        CHECK(ToolHandler::matches_pattern("make all install", "make*install") == true);
        CHECK(ToolHandler::matches_pattern("make install all", "make*all") == true);
        CHECK(ToolHandler::matches_pattern("xmake all", "make*all") == false);
    }
}

TEST_CASE("discover_skills reports the skill directories (§SKILL-DIRS)") {
    // Regression: the agent had no way to learn where skills live -
    // discover_skills only returned paths of skills that already exist,
    // so empty directories were invisible and the model could not know
    // where to CREATE a skill. The output must carry the configured
    // scan directories with exists flags.
    SkillManager sm;
    sm.init_after_settings();  // adds the default scan directories
    SkillManager::set_current(&sm);

    DiscoverSkillsTool tool;
    FileIO file_io(".");  // unused by discover_skills
    const json result = tool.execute(json::object(), file_io);

    SkillManager::set_current(nullptr);

    REQUIRE(result.value("status", "") == "success");
    REQUIRE(result.contains("skill_directories"));
    const auto& dirs = result["skill_directories"];
    REQUIRE(dirs.is_array());
    // Defaults: daedalus user-global + ~/.agents/skills + .agents/skills
    CHECK(dirs.size() >= 3);

    const char* home = std::getenv("HOME");
    REQUIRE(home != nullptr);
    // The defaults are stored with trailing slashes and absolute() does
    // not normalise them away - compare lexically_normal forms.
    const auto norm = [](const std::string& s) {
        std::string n = std::filesystem::path(s).lexically_normal().string();
        while (n.size() > 1 && n.back() == '/') n.pop_back();
        return n;
    };
    const std::string want_user =
        norm(std::string(home) + "/.agents/skills");
    const std::string want_project =
        norm(std::filesystem::absolute(std::filesystem::path(".agents/skills"))
                 .string());

    bool saw_user = false;
    bool saw_project = false;
    for (const auto& d : dirs) {
        REQUIRE(d.contains("path"));
        REQUIRE(d.contains("exists"));
        const std::string path = d["path"].get<std::string>();
        CHECK(!path.empty());
        CHECK(path.front() == '/');  // expanded + absolute
        const std::string n = norm(path);
        if (n == want_user) saw_user = true;
        if (n == want_project) saw_project = true;
    }
    CHECK(saw_user);
    CHECK(saw_project);
}

// §LIST-TOOLS (2026-09-20): ground-truth tool introspection.
TEST_CASE("list_tools is registered and reports the live registry") {
    auto factory = ToolHandler::get_tool_factory();
    REQUIRE(factory != nullptr);
    CHECK(factory->has_tool("list_tools"));

    FileIO file_io(".");
    const json result = factory->execute_tool("list_tools", json::object(), file_io);
    REQUIRE(result["status"] == "success");
    REQUIRE(result.contains("tools"));
    REQUIRE(result["tools"].is_array());
    CHECK(result["count"].get<size_t>() == result["tools"].size());

    // It must list itself plus the well-known core tools.
    std::vector<std::string> names;
    for (const auto& t : result["tools"]) {
        REQUIRE(t.contains("name"));
        names.push_back(t["name"].get<std::string>());
    }
    const auto has = [&](const char* n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    CHECK(has("list_tools"));
    CHECK(has("execute_bash"));
    CHECK(has("tell_user"));
    CHECK(has("stop"));

    // Unfiltered call: nothing reported disabled.
    CHECK(!result.contains("disabled_by_host"));
}

TEST_CASE("list_tools honours the per-agent ToolFilter via execute_tool(filter)") {
    auto factory = ToolHandler::get_tool_factory();
    REQUIRE(factory != nullptr);

    ToolFilter filter;
    filter.disabled_tools.insert("execute_bash");

    FileIO file_io(".");
    const json result = factory->execute_tool("list_tools", json::object(),
                                              file_io, nullptr, filter);
    REQUIRE(result["status"] == "success");

    std::vector<std::string> names;
    for (const auto& t : result["tools"]) {
        names.push_back(t["name"].get<std::string>());
    }
    CHECK(std::find(names.begin(), names.end(), "execute_bash") == names.end());
    CHECK(std::find(names.begin(), names.end(), "tell_user") != names.end());

    REQUIRE(result.contains("disabled_by_host"));
    const auto& disabled = result["disabled_by_host"];
    CHECK(std::find(disabled.begin(), disabled.end(), "execute_bash") != disabled.end());

    // And the filter still blocks direct execution of the disabled tool.
    const json blocked = factory->execute_tool("execute_bash", json::object(),
                                               file_io, nullptr, filter);
    CHECK(blocked["status"] == "error");
}
