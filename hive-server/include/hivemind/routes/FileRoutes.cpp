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
#include <unordered_map>

namespace fs = std::filesystem;

using namespace hivemind::routes;   // path_segments, send_json, …
namespace {

constexpr std::size_t kMaxUpload = 10 * 1024 * 1024;  // OmniNet body cap

} // namespace

// Phase H: pick the connected worker that owns /workspace. Phase 1
// assumes a single worker; with several connected we take the first
// (multi-worker ?worker= selection is deferred).
std::optional<std::string> pick_worker(WorkerChannel* wchannel) {
    if (!wchannel) return std::nullopt;
    auto ws = wchannel->connected_workers();
    if (ws.empty()) return std::nullopt;
    std::sort(ws.begin(), ws.end());   // deterministic
    return ws.front();
}


void register_file_routes(OmniNet::HTTP::Server& server,
                          ThreadStore& threads,
                          AgentStore& agents,
                          WorkerChannel* wchannel) {
    // ── POST /api/threads/:id/attachments?name=<filename> ─────────────
    // Raw request body is the file content (no multipart parsing; the
    // client sets ?name=). Saved into the shared workspace so containers
    // see it at /workspace/attachments/<thread_id>/<name>.
    server.post("/api/threads/:id/attachments",
        [&threads, wchannel](const OmniNet::HTTP::Request& req,
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
            // Phase H: the worker owns /workspace - push the attachment
            // over the channel into attachments/<thread_id>/<name> on
            // the worker's workspace dir (agents see it at
            // /workspace/attachments/<thread_id>/<name>).
            const auto worker = pick_worker(wchannel);
            if (!worker) {
                send_error(resp, 503, "no worker connected"); return;
            }
            // Uniquify against the worker's current listing so repeated
            // same-name uploads do not clobber each other.
            const std::string dir_rel = "attachments/" + seg[2];
            std::string fname = name;
            if (auto l = wchannel->list_files(*worker, dir_rel, 15);
                l && l->ok) {
                std::unordered_set<std::string> taken;
                for (auto& e : l->entries)
                    taken.insert(e.value("name", ""));
                if (taken.count(fname)) {
                    const fs::path nm{name};
                    const std::string stem = nm.stem().string();
                    const std::string ext  = nm.extension().string();
                    for (int i = 2; taken.count(fname); i++)
                        fname = stem + "-" + std::to_string(i) + ext;
                }
            }
            const std::string rel = dir_rel + "/" + fname;
            auto r = wchannel->push_file(*worker, rel,
                std::string(reinterpret_cast<const char*>(body.data()),
                            body.size()));
            if (!r) { send_error(resp, 503, "worker went offline"); return; }
            if (!r->ok) { send_error(resp, 502, r->error); return; }
            send_json(resp, json{
                {"name", fname},
                {"rel_path", rel},
                {"path", "/workspace/" + rel},
                {"worker", *worker},
                {"size", body.size()},
            }, 201);
        });

    // ── GET /api/files?path=<rel> — list a workspace directory ────────
    server.get("/api/files",
        [wchannel](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            // Phase H: listing comes from the WORKER's workspace dir
            // over the channel (the worker owns /workspace).
            const auto worker = pick_worker(wchannel);
            if (!worker) {
                send_error(resp, 503, "no worker connected"); return;
            }
            const std::string rel = req.queryParameter("path", "");
            auto l = wchannel->list_files(*worker, rel);
            if (!l) { send_error(resp, 503, "worker went offline"); return; }
            if (!l->ok) {
                if (l->error.find("bad path") != std::string::npos)
                    send_error(resp, 404, "path not found");
                else
                    send_error(resp, 400, l->error);
                return;
            }
            json entries = l->entries;
            // dirs first, then name - stable, predictable for the UI
            std::sort(entries.begin(), entries.end(), [](const json& a,
                                                         const json& b) {
                const bool ad = a.value("kind", "") == "dir";
                const bool bd = b.value("kind", "") == "dir";
                if (ad != bd) return ad;
                return a.value("name", "") < b.value("name", "");
            });
            send_json(resp, json{{"path", rel}, {"entries", entries},
                                 {"worker", *worker}});
        });

    server.get("/api/files/download",
        [wchannel](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            // Phase H: fetch from the WORKER's workspace over the channel.
            const auto worker = pick_worker(wchannel);
            if (!worker) {
                send_error(resp, 503, "no worker connected"); return;
            }
            const std::string rel = req.queryParameter("path", "");
            auto r = wchannel->fetch_file(*worker, "", rel);
            if (!r) { send_error(resp, 503, "worker went offline"); return; }
            if (!r->ok) {
                if (r->error.rfind("not found", 0) == 0)
                    send_error(resp, 404, r->error);
                else
                    send_error(resp, 400, r->error);
                return;
            }
            resp = OmniNet::HTTP::Response::ok();
            // Real MIME type for images so browsers/clients can render
            // inline; everything else stays an octet-stream download.
            const fs::path rp{rel};
            std::string ext = rp.extension().string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            static const std::unordered_map<std::string, std::string> kMime{
                {".png", "image/png"},  {".jpg", "image/jpeg"},
                {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
                {".webp", "image/webp"},
            };
            auto it = kMime.find(ext);
            if (it != kMime.end()) {
                resp.setContentType(it->second);
                resp.setHeader("Content-Disposition",
                               "inline; filename=\"" +
                               rp.filename().string() + "\"");
            } else {
                resp.setContentType("application/octet-stream");
                resp.setHeader("Content-Disposition",
                               "attachment; filename=\"" +
                               rp.filename().string() + "\"");
            }
            resp.setBody(r->data);
            apply_cors(resp);
        });

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

