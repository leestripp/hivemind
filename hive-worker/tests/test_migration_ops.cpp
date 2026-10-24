// =============================================================================
// test_migration_ops — Phase 73: worker-side migration staging round-trip
// WITHOUT podman/tar mocks where possible (real tar+tar is exercised —
// the dev box has GNU tar with zstd). Fleet interactions are injected.
// =============================================================================

#include "migration_ops.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path make_ws(const char* tag) {
    fs::path ws = fs::temp_directory_path() /
                  ("hivemind_test_migration_ops_" + std::string(tag));
    fs::remove_all(ws);
    fs::create_directories(ws / ".hive-agent" / "Buzz");
    { std::ofstream f(ws / ".hive-agent" / "Buzz" / "state.json");
      f << "{\"ok\":1}"; }
    { std::ofstream f(ws / ".hive-agent" / "Buzz" / "memory.md");
      f << "hello migration\n"; }
    return ws;
}

hwmigration::Ctx make_ctx(const fs::path& ws, std::vector<json>* sent) {
    hwmigration::Ctx ctx;
    ctx.ws = ws;
    ctx.staging_root = ws / "migrations";
    ctx.send = [sent](const json& f) { sent->push_back(f); };
    ctx.hooks.is_running = [](const std::string&) { return false; };
    return ctx;
}

json last_of(const std::vector<json>& v, const std::string& op) {
    for (auto it = v.rbegin(); it != v.rend(); ++it)
        if (it->value("op", "") == op) return *it;
    return json();
}

}  // namespace

int main() {
    // ── base64 round-trip incl. binary bytes ───────────────────────────
    const std::string bin = [] {
        std::string s;
        for (int i = 0; i < 1000; ++i) s += char(i % 251);
        return s;
    }();
    assert(hwmigration::b64_decode(hwmigration::b64_encode(bin)) == bin);
    assert(hwmigration::b64_encode("f") == "Zg==");
    assert(hwmigration::b64_encode("fo") == "Zm8=");
    bool threw = false;
    try { hwmigration::b64_decode("!!!"); } catch (...) { threw = true; }
    assert(threw);

    // ── export on the source workspace ─────────────────────────────────
    const fs::path src = make_ws("src");
    std::vector<json> src_frames;
    auto src_ctx = make_ctx(src, &src_frames);
    const std::string job = "job-test-1";
    hwmigration::run_op({{"op", "agent_export"},
                         {"job_id", job}, {"agent_id", "aid-buzz"},
                         {"name", "Buzz"}, {"force", false}},
                        src_ctx);
    json ready = last_of(src_frames, "agent_export_ready");
    assert(!ready.empty());
    const fs::path arc = ready["path"].get<std::string>();
    assert(fs::exists(arc) && ready["size"].get<size_t>() > 0);
    // idempotent: second export reuses the archive (no error frames)
    src_frames.clear();
    hwmigration::run_op({{"op", "agent_export"},
                         {"job_id", job}, {"agent_id", "aid-buzz"},
                         {"name", "Buzz"}, {"force", false}},
                        src_ctx);
    assert(last_of(src_frames, "agent_export_error").empty());

    // ── chunk streaming (source) → reassembly (target) ─────────────────
    src_frames.clear();
    hwmigration::run_op({{"op", "agent_data_begin"}, {"job_id", job}},
                        src_ctx);
    const fs::path dst = make_ws("dst");
    std::vector<json> dst_frames;
    auto dst_ctx = make_ctx(dst, &dst_frames);
    bool any_last = false;
    for (const auto& f : src_frames) {
        if (f.value("op", "") != "agent_data_chunk") continue;
        hwmigration::run_op(f, dst_ctx);
        any_last = any_last || f.value("last", false);
    }
    assert(any_last);
    assert(!last_of(dst_frames, "agent_data_done").empty());
    assert(fs::file_size(dst / "migrations" / job / "import.tar.zst") ==
           fs::file_size(arc));

    // ── import on the target (container kind) ──────────────────────────
    hwfleet::AgentSpec spawned;
    bool spawned_called = false;
    dst_ctx.hooks.spawn_agent = [&](const hwfleet::AgentSpec& sp) {
        spawned = sp; spawned_called = true;
    };
    hwmigration::run_op({{"op", "agent_import"}, {"job_id", job},
                         {"agent_id", "aid-buzz"}, {"name", "Buzz"},
                         {"target_type", "container"},
                         {"model", "kimi-k2.5"}, {"memory", "2g"}},
                        dst_ctx);
    assert(spawned_called && spawned.id == "aid-buzz" &&
           spawned.name == "Buzz" && !spawned.host_agent);
    assert(fs::exists(dst / ".hive-agent" / "Buzz" / "state.json"));
    assert(fs::exists(dst / ".hive-agent" / "Buzz" / "memory.md"));
    assert(!last_of(dst_frames, "agent_import_done").empty());
    // target staging pruned after import
    assert(!fs::exists(dst / "migrations" / job));

    // ── cleanup on the source: state dir + staging gone ────────────────
    bool removed = false;
    src_ctx.hooks.remove_agent = [&](const std::string& id) {
        assert(id == "aid-buzz"); removed = true;
        fs::remove_all(src / ".hive-agent" / "Buzz");
    };
    src_ctx.hooks.host_user_of = [](const std::string&) { return ""; };
    src_frames.clear();
    hwmigration::run_op({{"op", "agent_cleanup"}, {"job_id", job},
                         {"agent_id", "aid-buzz"}, {"name", "Buzz"}},
                        src_ctx);
    assert(removed && !fs::exists(src / ".hive-agent" / "Buzz"));
    assert(!fs::exists(src / "migrations" / job));
    assert(!last_of(src_frames, "agent_cleanup_done").empty());

    // ── abort removes staging only ─────────────────────────────────────
    const fs::path third = make_ws("third");
    std::vector<json> t3;
    auto c3 = make_ctx(third, &t3);
    hwmigration::run_op({{"op", "agent_export"}, {"job_id", "j2"},
                         {"agent_id", "a"}, {"name", "Buzz"}}, c3);
    assert(fs::exists(third / "migrations" / "j2"));
    hwmigration::run_op({{"op", "agent_migrate_abort"}, {"job_id", "j2"}},
                        c3);
    assert(!fs::exists(third / "migrations" / "j2"));
    assert(fs::exists(third / ".hive-agent" / "Buzz"));

    // ── error: missing state dir on export ─────────────────────────────
    t3.clear();
    hwmigration::run_op({{"op", "agent_export"}, {"job_id", "j3"},
                         {"agent_id", "a"}, {"name", "Nobody"}}, c3);
    assert(!last_of(t3, "agent_export_error").empty());

    std::cout << "test_migration_ops: OK\n";
    return 0;
}
