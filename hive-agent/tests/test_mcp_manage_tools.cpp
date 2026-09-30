// tests/test_mcp_manage_tools.cpp
//
// Phase 71: agent self-service MCP management tools.
//   MT1.  mcp_add persists an origin=local row; bogus command reports
//         connected:false but stays persisted.
//   MT2.  mcp_add refuses invalid names.
//   MT3.  mcp_add/mcp_disable/mcp_remove refuse origin=hub rows.
//   MT4.  mcp_add refuses duplicate local names.
//   MT5.  mcp_disable persists enabled=false; mcp_remove deletes the row.
//   MT6.  mcp_list redacts env VALUES (keys only) and shows origin.
//   MT7.  mcp_enable on unknown server errors; approval denial blocks add.
//   MT8.  definitions expose valid function schemas (brace regression).

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "core/FileIO.h"
#include "core/settings.h"
#include "mcp/McpServerManager.h"
#include "tools/McpManageTools.h"
#include "tools/ToolHandler.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Fixture {
    fs::path home;
    std::unique_ptr<hive::SettingsStore> store;
    std::unique_ptr<mcp::McpServerManager> mgr;
    McpToolServices svc;
    McpListTool    list;
    McpAddTool     add;
    McpEnableTool  enable;
    McpDisableTool disable;
    McpRemoveTool  remove;

    Fixture()
        : list(svc), add(svc), enable(svc), disable(svc), remove(svc)
    {
        home = fs::temp_directory_path() /
               ("hive-mtools-" + std::to_string(std::chrono::system_clock::now()
                                  .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
        fs::remove_all(home);
        fs::create_directories(home);
        store = std::make_unique<hive::SettingsStore>(
            hive::SettingsStore::for_home(home));
        mgr = std::make_unique<mcp::McpServerManager>();
        svc = McpToolServices{mgr.get(), store.get()};
        list    = McpListTool(svc);
        add     = McpAddTool(svc);
        enable  = McpEnableTool(svc);
        disable = McpDisableTool(svc);
        remove  = McpRemoveTool(svc);
        ToolHandler::set_auto_mode(true);   // bypass approval prompts
    }
    ~Fixture() {
        ToolHandler::set_auto_mode(false);
        std::error_code ec; fs::remove_all(home, ec);
    }

    void seed_hub_row(const std::string& name) {
        hive::SettingsStore::McpServer s;
        s.name = name; s.command = "hub-mcp"; s.origin = "hub";
        store->upsert_mcp_server(s);
    }
};

json run(Fixture& f, BaseTool& t, const json& args) {
    FileIO io(f.home);
    return t.execute(args, io);
}

}  // namespace

TEST_CASE("MT1: mcp_add persists origin=local row; bad command = not connected") {
    Fixture f;
    auto r = run(f, f.add, {{"name", "comfy"},
                         {"command", "/nonexistent/comfy-mcp"},
                         {"args", json::array({"--port", "8188"})},
                         {"env", {{"COMFYUI_URL", "http://localhost:8188"}}}});
    CHECK(r.value("status", "") == "success");
    CHECK(r.value("persisted", false));
    CHECK(r.value("connected", true) == false);   // bogus binary can't spawn
    CHECK(r.contains("error"));
    auto row = f.store->get_mcp_server("comfy");
    REQUIRE(row.has_value());
    CHECK(row->origin == "local");
    CHECK(row->args.size() == 2);
    CHECK(row->env.at("COMFYUI_URL") == "http://localhost:8188");
}

TEST_CASE("MT2: mcp_add refuses invalid names") {
    Fixture f;
    for (const char* bad : {"", "Has Caps", "dot.name", "-lead", "slash/x"}) {
        auto r = run(f, f.add, {{"name", bad}, {"command", "x"}});
        CHECK(r.value("status", "") == "error");
    }
    CHECK(!f.store->has_mcp_servers());
}

TEST_CASE("MT3: hub rows are read-only to add/disable/remove") {
    Fixture f;
    f.seed_hub_row("blender");
    auto r1 = run(f, f.add, {{"name", "blender"}, {"command", "evil"}});
    CHECK(r1.value("status", "") == "error");
    CHECK(r1.value("error", "").find("hub roster") != std::string::npos);
    auto r2 = run(f, f.disable, {{"name", "blender"}});
    CHECK(r2.value("status", "") == "error");
    auto r3 = run(f, f.remove, {{"name", "blender"}});
    CHECK(r3.value("status", "") == "error");
    // Row untouched.
    auto row = f.store->get_mcp_server("blender");
    REQUIRE(row.has_value());
    CHECK(row->enabled);
    CHECK(row->origin == "hub");
}

TEST_CASE("MT4: mcp_add refuses duplicate local name") {
    Fixture f;
    run(f, f.add, {{"name", "comfy"}, {"command", "/nonexistent/a"}});
    auto r = run(f, f.add, {{"name", "comfy"}, {"command", "/nonexistent/b"}});
    CHECK(r.value("status", "") == "error");
    CHECK(r.value("error", "").find("already exists") != std::string::npos);
}

TEST_CASE("MT5: disable persists enabled=false; remove deletes the row") {
    Fixture f;
    run(f, f.add, {{"name", "comfy"}, {"command", "/nonexistent/comfy-mcp"}});
    auto rd = run(f, f.disable, {{"name", "comfy"}});
    CHECK(rd.value("status", "") == "success");
    auto row = f.store->get_mcp_server("comfy");
    REQUIRE(row.has_value());
    CHECK(!row->enabled);
    auto rr = run(f, f.remove, {{"name", "comfy"}});
    CHECK(rr.value("status", "") == "success");
    CHECK(!f.store->get_mcp_server("comfy").has_value());
    CHECK(!f.store->has_mcp_servers());
}

TEST_CASE("MT6: mcp_list redacts env values, shows origin and state") {
    Fixture f;
    f.seed_hub_row("blender");
    run(f, f.add, {{"name", "comfy"},
                {"command", "/nonexistent/comfy-mcp"},
                {"env", {{"API_KEY", "sk-supersecretvalue123"}}}});
    auto r = run(f, f.list, json::object());
    CHECK(r.value("status", "") == "success");
    const auto dump = r.dump();
    CHECK(dump.find("sk-supersecretvalue123") == std::string::npos); // redacted
    CHECK(dump.find("API_KEY") != std::string::npos);                // key shown
    REQUIRE(r["servers"].size() == 2);
    bool saw_hub = false, saw_local = false;
    for (const auto& s : r["servers"]) {
        if (s["name"] == "blender") { saw_hub = true;  CHECK(s["origin"] == "hub"); }
        if (s["name"] == "comfy")   { saw_local = true; CHECK(s["origin"] == "local");
                                      CHECK(s["connected"] == false); }
    }
    CHECK(saw_hub);
    CHECK(saw_local);
}

TEST_CASE("MT7: enable unknown errors; approval denial blocks add") {
    Fixture f;
    auto r1 = run(f, f.enable, {{"name", "nope"}});
    CHECK(r1.value("status", "") == "error");

    ToolHandler::set_auto_mode(false);   // no sink attached -> deny
    auto r2 = run(f, f.add, {{"name", "comfy"}, {"command", "/bin/true"}});
    CHECK(r2.value("status", "") == "error");
    CHECK(r2.value("error", "").find("approval denied") != std::string::npos);
    CHECK(!f.store->get_mcp_server("comfy").has_value());
    ToolHandler::set_auto_mode(true);
}

TEST_CASE("MT8: tool definitions are well-formed function schemas") {
    Fixture f;
    for (BaseTool* t : {static_cast<BaseTool*>(&f.list),
                        static_cast<BaseTool*>(&f.add),
                        static_cast<BaseTool*>(&f.enable),
                        static_cast<BaseTool*>(&f.disable),
                        static_cast<BaseTool*>(&f.remove)}) {
        auto def = t->get_definition();
        CHECK(def["type"] == "function");
        CHECK(def["function"]["name"] == t->get_name());
        REQUIRE(def["function"]["parameters"].is_object());  // brace regression
        CHECK(def["function"]["parameters"]["type"] == "object");
        CHECK(def["function"]["parameters"]["properties"].is_object());
    }
    // mcp_add exposes the full parameter set.
    auto props = f.add.get_definition()["function"]["parameters"]["properties"];
    for (const char* k : {"name", "command", "args", "env", "url", "description"})
        CHECK(props.contains(k));
}
