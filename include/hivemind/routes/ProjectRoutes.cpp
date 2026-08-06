#include "hivemind/routes/ProjectRoutes.h"
#include <OmniNet.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using json = nlohmann::json;

static json scan_dir(const fs::path& dir, int depth = 0) {
    json arr = json::array();
    if (depth > 2) return arr; // limit depth for safety
    try {
        for (auto& e : fs::directory_iterator(dir)) {
            json entry;
            entry["name"] = e.path().filename().string();
            entry["is_dir"] = e.is_directory();
            if (e.is_regular_file())
                entry["size"] = static_cast<int>(e.file_size());
            if (e.is_directory()) {
                entry["children"] = scan_dir(e.path(), depth + 1);
            }
            arr.push_back(std::move(entry));
        }
    } catch (const fs::filesystem_error&) {}
    return arr;
}

static json list_projects() {
    json arr = json::array();

    // ~/.codemax/projects/
    auto codemax_projects = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                             / ".codemax" / "projects";
    if (fs::exists(codemax_projects)) {
        try {
            for (auto& e : fs::directory_iterator(codemax_projects)) {
                if (e.is_directory()) {
                    arr.push_back({
                        {"name",  e.path().filename().string()},
                        {"path",  e.path().string()},
                        {"source", "codemax"}
                    });
                }
            }
        } catch (const fs::filesystem_error&) {}
    }

    // ~/projects/AI/  (our own code)
    auto ai_projects = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                        / "projects" / "AI";
    if (fs::exists(ai_projects)) {
        try {
            for (auto& e : fs::directory_iterator(ai_projects)) {
                if (e.is_directory()) {
                    arr.push_back({
                        {"name",  e.path().filename().string()},
                        {"path",  e.path().string()},
                        {"source", "local"}
                    });
                }
            }
        } catch (const fs::filesystem_error&) {}
    }

    return arr;
}

void register_project_routes(OmniNet::HTTP::Server& server) {

    // GET /api/projects — list available projects
    server.get("/api/projects",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(list_projects().dump());
            resp = r;
        });

    // GET /api/projects/:name — list files in a project
    server.get("/api/projects/:name",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};

            // Search in both locations
            std::vector<fs::path> search = {
                fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                    / ".codemax" / "projects" / name,
                fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                    / "projects" / "AI" / name,
            };

            fs::path found;
            for (auto& p : search) {
                if (fs::exists(p) && fs::is_directory(p)) { found = p; break; }
            }

            if (found.empty()) {
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::notFound("project not found: " + name);
                r.setContentType("application/json");
                r.setBody(json{{"error", "project not found: " + name}}.dump());
                resp = r;
                return;
            }

            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(scan_dir(found).dump());
            resp = r;
        });
}
