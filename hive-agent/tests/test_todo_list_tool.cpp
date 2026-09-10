/**
 * @file test_todo_list_tool.cpp
 * @brief Tests for the built-in todo list tools.
 *
 * Hermetic: uses a temporary project directory; no LLM or network needed.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/FileIO.h"
#include "tools/TodoListTool.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

struct TempProject {
    fs::path dir;

    explicit TempProject(std::string_view suffix) {
        dir = fs::temp_directory_path() /
              ("daedalus_todo_test_" + std::string(suffix));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    ~TempProject() { fs::remove_all(dir); }

    fs::path todos_file(std::string_view agent_name = {}) const {
        fs::path base = dir / ".hive-agent";
        if (agent_name.empty()) {
            base = base / "default";
        } else {
            base = base / std::string(agent_name);
        }
        return base / "todos.json";
    }
};

static json todo_add(FileIO& io, const std::string& text,
                     const std::optional<std::string>& status = std::nullopt,
                     const std::optional<std::string>& group = std::nullopt) {
    TodoAddTool tool;
    json args = json::object({{"text", text}});
    if (status) args["status"] = *status;
    if (group) args["group"] = *group;
    return tool.execute(args, io);
}

static json todo_edit(FileIO& io, int id,
                      const std::optional<std::string>& text = std::nullopt,
                      const std::optional<std::string>& status = std::nullopt,
                      const std::optional<std::string>& group = std::nullopt) {
    TodoEditTool tool;
    json args = json::object({{"id", id}});
    if (text) args["text"] = *text;
    if (status) args["status"] = *status;
    if (group) args["group"] = *group;
    return tool.execute(args, io);
}

static json todo_delete(FileIO& io, int id) {
    TodoDeleteTool tool;
    return tool.execute(json::object({{"id", id}}), io);
}

static json todo_list(FileIO& io, const std::string& filter = "all") {
    TodoListTool tool;
    return tool.execute(json::object({{"filter", filter}}), io);
}

static json todo_count(FileIO& io) {
    TodoCountTool tool;
    return tool.execute(json::object(), io);
}

static json todo_clear(FileIO& io) {
    TodoClearTool tool;
    return tool.execute(json::object(), io);
}

static json set_max(FileIO& io, int max) {
    TodoSetMaxTool tool;
    return tool.execute(json::object({{"max", max}}), io);
}

// ============================================================================
// Schema tests
// ============================================================================

TEST_SUITE("TodoListTool — definitions") {

    TEST_CASE("All six tools are declared") {
        CHECK(TodoAddTool().get_name() == "todo_add");
        CHECK(TodoEditTool().get_name() == "todo_edit");
        CHECK(TodoDeleteTool().get_name() == "todo_delete");
        CHECK(TodoListTool().get_name() == "todo_list");
        CHECK(TodoSetMaxTool().get_name() == "set_max_todos");
        CHECK(TodoCountTool().get_name() == "todo_count");
    }

    TEST_CASE("todo_add requires text") {
        json def = TodoAddTool().get_definition();
        REQUIRE(def.contains("function"));
        const auto& params = def["function"]["parameters"];
        REQUIRE(params.contains("required"));
        REQUIRE(params["required"].is_array());
        CHECK(params["required"].size() == 1);
        CHECK(params["required"][0] == "text");
    }

    TEST_CASE("todo_edit requires id") {
        json def = TodoEditTool().get_definition();
        const auto& params = def["function"]["parameters"];
        REQUIRE(params.contains("required"));
        REQUIRE(params["required"].is_array());
        CHECK(params["required"].size() == 1);
        CHECK(params["required"][0] == "id");
    }
}

// ============================================================================
// Basic CRUD
// ============================================================================

TEST_SUITE("TodoListTool — CRUD") {

    TEST_CASE("Add and list a todo") {
        TempProject tp("crud");
        FileIO io(tp.dir);

        json r = todo_add(io, "Fix the build");
        CHECK(r["status"] == "success");
        CHECK(r["todo"]["id"] == 1);
        CHECK(r["todo"]["text"] == "Fix the build");
        CHECK(r["todo"]["status"] == "pending");

        json list = todo_list(io);
        CHECK(list["status"] == "success");
        CHECK(list["total"] == 1);
        CHECK(list["pending"] == 1);
        CHECK(list["done"] == 0);
        std::string out = list.value("output", "");
        CHECK(out.find("Fix the build") != std::string::npos);
    }

    TEST_CASE("Edit text and mark done") {
        TempProject tp("edit");
        FileIO io(tp.dir);

        todo_add(io, "Original text");
        json r = todo_edit(io, 1, "Updated text", "done");
        CHECK(r["status"] == "success");
        CHECK(r["todo"]["text"] == "Updated text");
        CHECK(r["todo"]["status"] == "done");

        json count = todo_count(io);
        CHECK(count["total"] == 1);
        CHECK(count["pending"] == 0);
        CHECK(count["done"] == 1);
    }

    TEST_CASE("Delete a todo") {
        TempProject tp("delete");
        FileIO io(tp.dir);

        todo_add(io, "To remove");
        json r = todo_delete(io, 1);
        CHECK(r["status"] == "success");

        json count = todo_count(io);
        CHECK(count["total"] == 0);
        CHECK(count["pending"] == 0);
        CHECK(count["done"] == 0);
    }

    TEST_CASE("Ids are stable across multiple adds") {
        TempProject tp("ids");
        FileIO io(tp.dir);

        todo_add(io, "First");
        todo_add(io, "Second");
        todo_delete(io, 1);
        json r = todo_add(io, "Third");
        CHECK(r["todo"]["id"] == 3);

        json list = todo_list(io);
        CHECK(list["total"] == 2);
    }
}

// ============================================================================
// Max todos
// ============================================================================

TEST_SUITE("TodoListTool — max todos") {

    TEST_CASE("Default max is 100") {
        TempProject tp("max_default");
        FileIO io(tp.dir);

        for (int i = 0; i < 100; ++i) {
            json r = todo_add(io, "task " + std::to_string(i));
            CHECK(r["status"] == "success");
        }
        json r = todo_add(io, "overflow");
        CHECK(r["status"] == "error");
        CHECK(r.contains("message"));

        json count = todo_count(io);
        CHECK(count["total"] == 100);
        CHECK(count["max_todos"] == 100);
    }

    TEST_CASE("set_max_todos changes the limit") {
        TempProject tp("max_change");
        FileIO io(tp.dir);

        json r = set_max(io, 2);
        CHECK(r["status"] == "success");
        CHECK(r["max_todos"] == 2);

        CHECK(todo_add(io, "a")["status"] == "success");
        CHECK(todo_add(io, "b")["status"] == "success");
        CHECK(todo_add(io, "c")["status"] == "error");
    }

    TEST_CASE("Cannot set max below current count") {
        TempProject tp("max_below");
        FileIO io(tp.dir);

        todo_add(io, "a");
        todo_add(io, "b");
        json r = set_max(io, 1);
        CHECK(r["status"] == "error");
    }
}

// ============================================================================
// Filtering and listing
// ============================================================================

TEST_SUITE("TodoListTool — listing") {

    TEST_CASE("Filter shows only pending or done") {
        TempProject tp("filter");
        FileIO io(tp.dir);

        todo_add(io, "Pending task");
        todo_add(io, "Done task", "done");

        json pending = todo_list(io, "pending");
        CHECK(pending["pending"] == 1);
        CHECK(pending["done"] == 0);

        json done = todo_list(io, "done");
        CHECK(done["pending"] == 0);
        CHECK(done["done"] == 1);
    }
}

// ============================================================================
// Error handling
// ============================================================================

TEST_SUITE("TodoListTool — errors") {

    TEST_CASE("Edit missing id errors") {
        TempProject tp("err_edit");
        FileIO io(tp.dir);

        json r = TodoEditTool().execute(json::object({{"text", "x"}}), io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Edit non-existent id errors") {
        TempProject tp("err_edit_id");
        FileIO io(tp.dir);

        json r = todo_edit(io, 99, "x");
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Delete non-existent id errors") {
        TempProject tp("err_delete");
        FileIO io(tp.dir);

        json r = todo_delete(io, 42);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Add empty text errors") {
        TempProject tp("err_add");
        FileIO io(tp.dir);

        json r = todo_add(io, "");
        CHECK(r["status"] == "error");
    }
}

// ============================================================================
// Clear
// ============================================================================

TEST_SUITE("TodoListTool — clear") {

    TEST_CASE("todo_clear removes all todos") {
        TempProject tp("clear");
        FileIO io(tp.dir);

        todo_add(io, "One");
        todo_add(io, "Two");
        CHECK(todo_count(io)["total"] == 2);

        json r = todo_clear(io);
        CHECK(r["status"] == "success");
        CHECK(r["removed"] == 2);

        json count = todo_count(io);
        CHECK(count["total"] == 0);
        CHECK(count["max_todos"] == 100);
    }
}

// ============================================================================
// Persistence and per-agent isolation
// ============================================================================

TEST_SUITE("TodoListTool — persistence & isolation") {

    TEST_CASE("Todos are persisted to disk") {
        TempProject tp("persist");
        {
            FileIO io(tp.dir);
            todo_add(io, "Persist me");
        }

        fs::path f = tp.todos_file();
        REQUIRE(fs::exists(f));

        std::ifstream in(f);
        json data = json::parse(in);
        REQUIRE(data["todos"].is_array());
        CHECK(data["todos"].size() == 1);
        CHECK(data["todos"][0]["text"] == "Persist me");
    }

    TEST_CASE("Different agent names have isolated lists") {
        TempProject tp("isolate");
        FileIO io_a(tp.dir, "agent_a");
        FileIO io_b(tp.dir, "agent_b");

        todo_add(io_a, "Task for A");
        todo_add(io_b, "Task for B");

        json count_a = todo_count(io_a);
        json count_b = todo_count(io_b);

        CHECK(count_a["total"] == 1);
        CHECK(count_b["total"] == 1);

        std::string list_a = todo_list(io_a)["output"].get<std::string>();
        std::string list_b = todo_list(io_b)["output"].get<std::string>();
        CHECK(list_a.find("Task for A") != std::string::npos);
        CHECK(list_a.find("Task for B") == std::string::npos);
        CHECK(list_b.find("Task for B") != std::string::npos);
        CHECK(list_b.find("Task for A") == std::string::npos);

        CHECK(fs::exists(tp.todos_file("agent_a")));
        CHECK(fs::exists(tp.todos_file("agent_b")));
    }
}

// ============================================================================
// Status and groups
// ============================================================================

TEST_SUITE("TodoListTool — status lifecycle") {

    TEST_CASE("Default status is pending and default group is General") {
        TempProject tp("status_default");
        FileIO io(tp.dir);

        json r = todo_add(io, "A task");
        CHECK(r["status"] == "success");
        CHECK(r["todo"]["status"] == "pending");
        CHECK(r["todo"]["group"] == "General");

        json list = todo_list(io);
        CHECK(list["pending"] == 1);
        CHECK(list["in_progress"] == 0);
        CHECK(list["done"] == 0);
        CHECK(list["blocked"] == 0);
    }

    TEST_CASE("Status transitions through todo_edit") {
        TempProject tp("status_transition");
        FileIO io(tp.dir);

        todo_add(io, "Task");
        CHECK(todo_edit(io, 1, std::nullopt, "in_progress")["todo"]["status"] ==
              "in_progress");
        CHECK(todo_edit(io, 1, std::nullopt, "blocked")["todo"]["status"] ==
              "blocked");
        CHECK(todo_edit(io, 1, std::nullopt, "done")["todo"]["status"] == "done");
        CHECK(todo_edit(io, 1, std::nullopt, "pending")["todo"]["status"] ==
              "pending");
    }

    TEST_CASE("Invalid status normalises to pending") {
        TempProject tp("status_invalid");
        FileIO io(tp.dir);

        TodoAddTool tool;
        json args = json::object({
            {"text", "Task"},
            {"status", "not_a_status"}
        });
        json r = tool.execute(args, io);
        CHECK(r["todo"]["status"] == "pending");
    }
}

TEST_SUITE("TodoListTool — groups") {

    TEST_CASE("Group is stored and returned") {
        TempProject tp("group_store");
        FileIO io(tp.dir);

        json r = todo_add(io, "Read code", std::nullopt, "Understand");
        CHECK(r["todo"]["group"] == "Understand");
    }

    TEST_CASE("Group filter narrows listing") {
        TempProject tp("group_filter");
        FileIO io(tp.dir);

        todo_add(io, "Read code", std::nullopt, "Understand");
        todo_add(io, "Write code", std::nullopt, "Implement");
        todo_add(io, "Run tests", std::nullopt, "Verify");

        json all = todo_list(io);
        CHECK(all["total"] == 3);

        json impl = TodoListTool().execute(
            json::object({{"filter", "all"}, {"group", "Implement"}}), io);
        CHECK(impl["total"] == 1);
        CHECK(impl["pending"] == 1);
        std::string out = impl["output"].get<std::string>();
        CHECK(out.find("Write code") != std::string::npos);
        CHECK(out.find("Read code") == std::string::npos);
        CHECK(out.find("Run tests") == std::string::npos);
    }

    TEST_CASE("Status filter works across groups") {
        TempProject tp("status_filter_groups");
        FileIO io(tp.dir);

        todo_add(io, "A", "done", "Implement");
        todo_add(io, "B", "pending", "Implement");
        todo_add(io, "C", "done", "Verify");

        json done = TodoListTool().execute(
            json::object({{"filter", "done"}}), io);
        CHECK(done["done"] == 2);
        std::string out = done["output"].get<std::string>();
        CHECK(out.find("[1] A") != std::string::npos);
        CHECK(out.find("[2] B") == std::string::npos);
        CHECK(out.find("[3] C") != std::string::npos);
    }
}

TEST_SUITE("TodoListTool — count summary") {

    TEST_CASE("Count returns per-status and per-group breakdown") {
        TempProject tp("count_summary");
        FileIO io(tp.dir);

        todo_add(io, "Read code", std::nullopt, "Understand");
        todo_add(io, "Plan fix", "in_progress", "Plan");
        todo_add(io, "Apply fix", "pending", "Implement");
        todo_add(io, "Run tests", "blocked", "Verify");

        json count = todo_count(io);
        CHECK(count["total"] == 4);
        CHECK(count["pending"] == 2);
        CHECK(count["in_progress"] == 1);
        CHECK(count["done"] == 0);
        CHECK(count["blocked"] == 1);
        REQUIRE(count.contains("by_group"));
        CHECK(count["by_group"]["Understand"] == 1);
        CHECK(count["by_group"]["Plan"] == 1);
        CHECK(count["by_group"]["Implement"] == 1);
        CHECK(count["by_group"]["Verify"] == 1);
    }
}

TEST_SUITE("TodoListTool — legacy migration") {

    TEST_CASE("Legacy 'done': true is migrated to status 'done'") {
        TempProject tp("legacy_done");
        fs::path f = tp.todos_file();
        fs::create_directories(f.parent_path());

        json legacy = json::object({
            {"max_todos", 100},
            {"next_id", 2},
            {"todos", json::array({
                json::object({
                    {"id", 1},
                    {"text", "Legacy task"},
                    {"done", true},
                    {"created_at", "2026-01-01 00:00"}
                })
            })}
        });
        {
            std::ofstream out(f);
            out << legacy.dump(2);
        }

        FileIO io(tp.dir);
        json list = todo_list(io);
        CHECK(list["done"] == 1);
        CHECK(list["pending"] == 0);

        json count = todo_count(io);
        CHECK(count["done"] == 1);
    }

    TEST_CASE("Legacy pending todo keeps default status") {
        TempProject tp("legacy_pending");
        fs::path f = tp.todos_file();
        fs::create_directories(f.parent_path());

        json legacy = json::object({
            {"max_todos", 100},
            {"next_id", 2},
            {"todos", json::array({
                json::object({
                    {"id", 1},
                    {"text", "Legacy pending"},
                    {"done", false},
                    {"created_at", "2026-01-01 00:00"}
                })
            })}
        });
        {
            std::ofstream out(f);
            out << legacy.dump(2);
        }

        FileIO io(tp.dir);
        json list = todo_list(io);
        CHECK(list["pending"] == 1);
        CHECK(list["done"] == 0);
    }
}
