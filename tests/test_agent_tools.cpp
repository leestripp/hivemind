// tests/test_agent_tools.cpp — Phase 20: SaveRoutineTool (learn-by-
// demonstration). No LLM; the tool is executed directly with a stub ctx.
#include "hivemind/AgentTools.h"
#include "hivemind/AgentStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/ThreadStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
class StubCtx final : public daedalus::HostToolContext {
public:
    std::string read_file(const fs::path&) override { return {}; }
    void write_file(const fs::path&, const std::string&) override {}
    std::vector<std::string> list_directory(const fs::path&) override { return {}; }
    const fs::path& project_root() const override { return root_; }
    fs::path root_;
};
} // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_agent_tools";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    RoutineStore routines{tmp / "routines.db"};
    auto a = agents.create_agent("Tester", "test subject");
    StubCtx ctx;

    SaveRoutineTool tool{a.id, routines, threads};

    // schema sanity
    auto def = tool.get_definition();
    assert(def["function"]["name"] == "save_routine");
    assert(def["function"]["parameters"]["required"].size() == 3);

    // missing args rejected
    assert(tool.execute(json::object(), ctx)["status"] == "error");
    assert(tool.execute(json{{"name", "x"}}, ctx)["status"] == "error");
    assert(tool.execute(json{{"name", "x"}, {"prompt", "y"}}, ctx)["status"] == "error");

    // invalid schedule rejected, nothing persisted
    auto bad = tool.execute(json{{"name", "bad"},
                                 {"prompt", "do the thing"},
                                 {"schedule", json{{"type", "hourly"}}}}, ctx);
    assert(bad["status"] == "error");
    assert(routines.list().empty());
    assert(threads.list_threads().empty());

    // valid: routine + dedicated thread created, next_fire computed
    auto ok = tool.execute(json{{"name", "morning-digest"},
                                {"prompt", "Summarize overnight alerts from /workspace/reports"},
                                {"schedule", json{{"type", "daily"},
                                                  {"time", "09:00"}}}}, ctx);
    assert(ok["status"] == "success");
    assert(!ok.value("next_fire_at", "").empty());
    auto rs = routines.list();
    assert(rs.size() == 1);
    assert(rs[0].name == "morning-digest");
    assert(rs[0].agent_id == a.id);
    assert(rs[0].enabled);
    assert(rs[0].prompt.find("/workspace/reports") != std::string::npos);
    // routine owns its own thread (fires don't spam the demo thread)
    auto t = threads.get_thread(rs[0].thread_id);
    assert(t && t->title == "routine: morning-digest");
    assert(t->agent_ids.size() == 1 && t->agent_ids[0] == a.id);

    // once-schedule works too
    auto once = tool.execute(json{{"name", "one-off"},
                                  {"prompt", "ping the status page"},
                                  {"schedule", json{{"type", "once"},
                                                    {"at", "2030-01-01T08:00"}}}}, ctx);
    assert(once["status"] == "success");
    assert(routines.list().size() == 2);

    fs::remove_all(tmp);
    std::cout << "test_agent_tools: OK\n";
    return 0;
}
