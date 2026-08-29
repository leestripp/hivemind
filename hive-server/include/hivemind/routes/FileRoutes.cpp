// FileRoutes.cpp — Phase 18: chat attachments + shared-workspace files API.
#include "hivemind/routes/FileRoutes.h"
#include "hivemind/AgentStore.h"
#include "hivemind/WorkerChannel.h"

#include "hivemind/Files.h"
#include "hivemind/ServerConfig.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/routes/RouteUtil.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using namespace hivemind::routes;   // path_segments, send_json, …
namespace {

constexpr std::size_t kMaxUpload = 10 * 1024 * 1024;  // OmniNet body cap

// Map a workspace-relative path to the container-visible mount path.
std::string container_path_for(const fs::path& rel) {
    return (fs::path{"/workspace"} / rel).generic_string();
}

json entry_json(const fs::path& root, const fs::directory_entry& e) {
    const fs::path rel = fs::relative(e.path(), root);
    json j = {{"name", e.path().filename().string()},
              {"path", rel.generic_string()},
              {"kind", e.is_directory() ? "dir" : "file"}};
    if (!e.is_directory()) {
        std::error_code ec;
        j["size"] = static_cast<long long>(e.file_size(ec));
    }
    std::error_code ec;
    const auto mt = e.last_write_time(ec);
    if (!ec) {
        // file_clock → system_clock (portable form; its epoch differs on
        // libstdc++, so time_since_epoch() alone yields garbage).
        const auto sys = std::chrono::time_point_cast<
            std::chrono::system_clock::duration>(
                mt - fs::file_time_type::clock::now() +
                std::chrono::system_clock::now());
        j["mtime"] = static_cast<long long>(
            std::chrono::system_clock::to_time_t(sys));
    }
    return j;
}

} // namespace

void register_file_routes(OmniNet::HTTP::Server& server,
                          ThreadStore& threads,
                          AgentStore& agents,
                          WorkerChannel* wchannel) {
    // ── POST /api/threads/:id/attachments?name=<filename> ─────────────
    // Raw request body is the file content (no multipart parsing; the
    // client sets ?name=). Saved into the shared workspace so containers
    // see it at /workspace/attachments/<thread_id>/<name>.
    server.post("/api/threads/:id/attachments",
        [&threads](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api threads :id attachments
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            const std::string name =
                hm::files::sanitize_filename(req.queryParameter("name", ""));
            const auto body = req.body();
            if (body.empty()) {
                send_error(resp, 400, "empty upload"); return;
            }
            if (body.size() > kMaxUpload) {
                send_error(resp, 413, "file too large (10 MB max)"); return;
            }
            try {
                const fs::path root = ServerConfig::instance().workspace_root();
                const fs::path dir  = root / "attachments" / seg[2];
                fs::create_directories(dir);
                // World-writable: agents (containers run as a different
                // uid via rootless subuid mapping) must write alongside
                // the uploaded files. The workspace share is 0777 for the
                // same reason — see docs/deploy.md "Relocating the
                // workspace".
                fs::permissions(dir, fs::perms::all,
                                fs::perm_options::replace);
                const fs::path dest = hm::files::unique_path(dir, name);
                std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(body.data()),
                          static_cast<std::streamsize>(body.size()));
                out.close();
                if (!out) { send_error(resp, 500, "write failed"); return; }

                const fs::path rel = fs::relative(dest, root);
                send_json(resp, json{
                    {"name", dest.filename().string()},
                    {"rel_path", rel.generic_string()},
                    {"path", container_path_for(rel)},
                    {"host_path", dest.string()},
                    {"size", body.size()},
                }, 201);
            } catch (const std::exception& e) {
                send_error(resp, 500, e.what());
            }
        });

    // ── GET /api/files?path=<rel> — list a workspace directory ────────
    server.get("/api/files",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            const fs::path root = ServerConfig::instance().workspace_root();
            const std::string rel = req.queryParameter("path", "");
            auto target = hm::files::resolve_confined(root, rel);
            if (!target) { send_error(resp, 404, "path not found"); return; }
            std::error_code ec;
            if (!fs::is_directory(*target, ec)) {
                send_error(resp, 400, "not a directory (use download)"); return;
            }
            json entries = json::array();
            for (auto& e : fs::directory_iterator(*target, ec))
                entries.push_back(entry_json(root, e));
            // dirs first, then name — stable, predictable for the UI
            std::sort(entries.begin(), entries.end(), [](const json& a,
                                                         const json& b) {
                const bool ad = a.value("kind", "") == "dir";
                const bool bd = b.value("kind", "") == "dir";
                if (ad != bd) return ad;
                return a.value("name", "") < b.value("name", "");
            });
            send_json(resp, json{{"path", rel}, {"entries", entries}});
        });

    // ── GET /api/files/download?path=<rel> — fetch one file ───────────
    server.get("/api/files/download",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            const fs::path root = ServerConfig::instance().workspace_root();
            const std::string rel = req.queryParameter("path", "");
            auto target = hm::files::resolve_confined(root, rel);
            if (!target) { send_error(resp, 404, "path not found"); return; }
            std::error_code ec;
            if (!fs::is_regular_file(*target, ec)) {
                send_error(resp, 400, "not a file"); return;
            }
            std::ifstream in(*target, std::ios::binary);
            std::string data{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/octet-stream");
            resp.setHeader("Content-Disposition",
                           "attachment; filename=\"" +
                           target->filename().string() + "\"");
            resp.setBody(data);
            apply_cors(resp);
        });

    // ── POST /api/agents/:id/push-file?path=<workspace-rel> ───────────
    // Phase 33 D: push a file to the agent's WORKER rig workspace
    // (server→worker over the channel). Local agents: 400 (write to the
    // server workspace via the attachments endpoint instead). Raw body
    // = file content, 10 MB cap (channel overhead on top).
    server.post("/api/agents/:id/push-file",
        [&agents, wchannel](const OmniNet::HTTP::Request& req,
                            OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api agents :id push-file
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            auto agent = agents.get_agent(seg[2]);
            if (!agent) { send_error(resp, 404, "agent not found"); return; }
            if (agent->worker.empty() || agent->worker == "local") {
                send_error(resp, 400,
                    "agent runs locally — use /api/threads/:id/attachments");
                return;
            }
            if (!wchannel || !wchannel->is_online(agent->worker)) {
                send_error(resp, 503, "worker offline"); return;
            }
            std::string rel = req.queryParameter("path", "");
            while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
            if (rel.empty() || rel.find("..") != std::string::npos) {
                send_error(resp, 400, "bad path"); return;
            }
            const auto body = req.body();
            if (body.empty()) { send_error(resp, 400, "empty body"); return; }
            if (body.size() > kMaxUpload) {
                send_error(resp, 413, "file too large (10 MB max)"); return;
            }
            auto r = wchannel->push_file(agent->worker, rel,
                std::string(reinterpret_cast<const char*>(body.data()),
                            body.size()));
            if (!r) { send_error(resp, 503, "worker went offline"); return; }
            if (!r->ok) { send_error(resp, 502, r->error); return; }
            send_json(resp, json{{"pushed", rel},
                                 {"worker", agent->worker},
                                 {"bytes", r->bytes},
                                 {"note", "file is on the RIG at /workspace/" +
                                          rel + " — it was NOT copied to the "
                                          "server"}}, 201);
        });
}

