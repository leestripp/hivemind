// tests/test_agent_tools.cpp — Phase 20: SaveRoutineTool (learn-by-
// demonstration). No LLM; the tool is executed directly with a stub ctx.
#include "hivemind/AgentTools.h"
#include "hivemind/AgentStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerStore.h"

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

    // ── CreateAgentTool worker placement + ListWorkersTool (Phase 30) ──
    WorkerStore workers_db{tmp / "workers.db"};
    agent_tools::install_worker_access(&workers_db, nullptr);  // no channel: all offline

    CreateAgentTool cat{agents};
    ListWorkersTool lwt;

    // default placement is local
    auto r = cat.execute(json{{"name", "LocalOne"}, {"role", "tester"}}, ctx);
    assert(r["status"] == "success");
    assert(r["worker"] == "local");

    // unknown worker rejected
    r = cat.execute(json{{"name", "Ghost"}, {"role", "x"}, {"worker", "no-such-rig"}}, ctx);
    assert(r["status"] == "error");

    // enrolled-but-offline worker rejected with a useful error
    workers_db.enroll("rig-a");
    r = cat.execute(json{{"name", "Offliner"}, {"role", "x"}, {"worker", "rig-a"}}, ctx);
    assert(r["status"] == "error");
    assert(r["error"].get<std::string>().find("offline") != std::string::npos);

    // list_workers reports enrolled rigs (offline without a channel)
    auto lw = lwt.execute(json::object(), ctx);
    assert(lw["status"] == "success");
    assert(lw["workers"].size() == 1);
    assert(lw["workers"][0]["name"] == "rig-a");
    assert(lw["workers"][0]["online"] == false);


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

    // ── routine management tools (list / update / delete, own-only) ──
    auto other = agents.create_agent("OtherAgent", "someone else");
    ListRoutinesTool lrt{a.id, routines};
    UpdateRoutineTool urt{a.id, routines};
    DeleteRoutineTool drt{a.id, routines};

    // list: sees both of Tester's routines, not other agents'
    auto lr = lrt.execute(json::object(), ctx);
    assert(lr["status"] == "success");
    assert(lr["routines"].size() == 2);

    // update: reschedule + rename + pause own routine
    const std::string rid = routines.list()[0].id;
    auto up = urt.execute(json{{"id", rid},
                               {"name", "renamed-digest"},
                               {"schedule", json{{"type", "interval"},
                                                 {"every_minutes", 30}}},
                               {"enabled", false}}, ctx);
    assert(up["status"] == "success");
    assert(up["name"] == "renamed-digest");
    assert(up["enabled"] == false);
    assert(!up.value("next_fire_at", "").empty());
    auto ru = routines.get(rid);
    assert(ru && ru->name == "renamed-digest" && !ru->enabled);
    assert(ru->schedule_json.find("interval") != std::string::npos);

    // update: invalid schedule rejected
    up = urt.execute(json{{"id", rid}, {"schedule", json{{"type", "daily"}}}}, ctx);
    assert(up["status"] == "error");

    // cross-agent protection: other agent cannot touch Tester's routine
    UpdateRoutineTool urt_other{other.id, routines};
    DeleteRoutineTool drt_other{other.id, routines};
    ListRoutinesTool lrt_other{other.id, routines};
    assert(lrt_other.execute(json::object(), ctx)["routines"].empty());
    up = urt_other.execute(json{{"id", rid}, {"name", "hijacked"}}, ctx);
    assert(up["status"] == "error");
    assert(drt_other.execute(json{{"id", rid}}, ctx)["status"] == "error");
    assert(routines.get(rid));   // untouched

    // delete own routine
    auto del = drt.execute(json{{"id", rid}}, ctx);
    assert(del["status"] == "success");
    assert(!routines.get(rid));
    assert(drt.execute(json{{"id", rid}}, ctx)["status"] == "error");  // gone

    fs::remove_all(tmp);
    std::cout << "test_agent_tools: OK\n";
    return 0;
}
