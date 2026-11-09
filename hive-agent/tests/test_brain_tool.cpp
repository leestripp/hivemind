#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>

#include "core/FileIO.h"
#include "core/SecondBrain.h"
#include "tools/BrainTool.h"

namespace fs = std::filesystem;

namespace {
struct TempProject {
    fs::path root;
    TempProject() {
        root = fs::temp_directory_path() /
               ("hive-brain-tool-" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempProject() { std::error_code ec; fs::remove_all(root, ec); }
};
}

TEST_CASE("BrainTool exposes a provider-valid schema and all actions") {
    BrainTool tool;
    const auto definition = tool.get_definition();
    CHECK(definition["function"]["name"] == "brain");
    const auto& parameters = definition["function"]["parameters"];
    CHECK(parameters.is_object());
    CHECK(parameters["properties"].is_object());
    CHECK(parameters["required"] == json::array({"action"}));
    CHECK(parameters["properties"]["action"]["enum"].size() == 10);

    TempProject temp;
    FileIO io(temp.root);
    auto captured = tool.execute({{"action", "capture"},
                                  {"folder", "projects"},
                                  {"title", "Tool Integration"},
                                  {"tags", json::array({"test"})},
                                  {"body", "A searchable tool fact."}}, io);
    REQUIRE(captured["status"] == "ok");
    CHECK(captured["id"] == "tool-integration");
    CHECK(fs::exists(temp.root / "second-brain" / "projects" /
                     "tool-integration.md"));

    CHECK(tool.execute({{"action", "append"}, {"id", captured["id"]},
                        {"markdown", "More detail."}}, io)["status"] == "ok");
    auto searched = tool.execute({{"action", "search"}, {"query", "searchable"}}, io);
    CHECK(searched["count"] == 1);
    auto read = tool.execute({{"action", "read"}, {"id", captured["id"]}}, io);
    CHECK(read["body"].get<std::string>().find("More detail") != std::string::npos);
    CHECK(tool.execute({{"action", "update"}, {"id", captured["id"]},
                        {"title", "Renamed"}}, io)["status"] == "ok");
    CHECK(tool.execute({{"action", "file"}, {"id", captured["id"]},
                        {"folder", "archives"}}, io)["status"] == "ok");
    CHECK(tool.execute({{"action", "list"}, {"folder", "archives"}}, io)["count"] == 1);
    CHECK(tool.execute({{"action", "backlinks"}, {"id", captured["id"]}},
                       io)["count"] == 0);
    CHECK(tool.execute({{"action", "reindex"}}, io)["status"] == "ok");
    CHECK(tool.execute({{"action", "delete"}, {"id", captured["id"]}},
                       io)["status"] == "ok");
}

TEST_CASE("BrainTool contains argument and missing-note errors") {
    TempProject temp;
    FileIO io(temp.root);
    BrainTool tool;
    CHECK(tool.execute(json::object(), io)["status"] == "error");
    CHECK(tool.execute({{"action", 42}}, io)["status"] == "error");
    CHECK(tool.execute({{"action", "capture"}}, io)["status"] == "error");
    CHECK(tool.execute({{"action", "read"}, {"id", "missing"}}, io)["status"] == "error");
    CHECK(tool.execute({{"action", "update"}, {"id", "missing"}}, io)["status"] == "error");
    CHECK(tool.execute({{"action", "capture"}, {"body", "x"},
                        {"folder", "../escape"}}, io)["status"] == "error");
    CHECK(tool.execute({{"action", "search"}, {"query", 42}}, io)["status"] == "error");
}
