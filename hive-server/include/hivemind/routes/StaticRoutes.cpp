// =============================================================================
// include/hivemind/routes/StaticRoutes.cpp
// -----------------------------------------------------------------------------
// Serves the web UI (web/dist/) from the same port as the API:
//   GET /          -> web/dist/index.html
//   GET /app.js    -> web/dist/app.js
//   GET /styles.css -> web/dist/styles.css
//
// IMPORTANT: this catch-all is registered LAST so specific API routes
// take precedence.
//
// Implementation note: we deliberately do NOT use OmniNet's
// StaticHandler here. It fails at runtime with "Failed to resolve path"
// for this project's route setup, and we do not patch OmniNet (see
// project preference). Instead we serve files with std::filesystem +
// OmniNet's public Response API, which is all StaticHandler would do
// internally anyway.
// =============================================================================

#include "hivemind/routes/StaticRoutes.h"

#include "hivemind/routes/RouteUtil.h"

#include <OmniNet.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path discover_web_dir() {
    if (const char* pwd = std::getenv("PWD")) {
        fs::path p = fs::path(pwd) / "web" / "dist";
        if (fs::exists(p)) return p;
    }
    fs::path p = fs::current_path() / "web" / "dist";
    if (fs::exists(p)) return p;
    // Fallback: directory next to the executable (argv[0] is not available
    // here; this is a best-effort fallback for packaged installs).
    return fs::path("web/dist");
}

std::string mime_type_for(const fs::path& p) {
    const std::string ext = p.extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js")                   return "application/javascript; charset=utf-8";
    if (ext == ".css")                  return "text/css; charset=utf-8";
    if (ext == ".png")                  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg")                  return "image/svg+xml";
    if (ext == ".json")                 return "application/json";
    if (ext == ".wasm")                 return "application/wasm";
    return "application/octet-stream";
}

std::optional<std::string> read_file_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f && !f.eof()) return std::nullopt;
    return ss.str();
}

// Map a request path (e.g. "/", "/app.js") to a safe file path under
// web_dir. Returns nullopt if the path escapes web_dir or does not exist.
std::optional<fs::path> resolve_static_path(const fs::path& web_dir, std::string_view req_path) {
    if (req_path.empty() || req_path[0] != '/') return std::nullopt;

    std::string rel{req_path.substr(1)};
    if (rel.empty() || rel == "index.html") rel = "index.html";

    // Reject obvious traversal attempts before touching the filesystem.
    if (rel.find("..") != std::string::npos) return std::nullopt;

    fs::path target = web_dir / rel;
    fs::path canonical = fs::weakly_canonical(target);

    fs::path root = fs::canonical(web_dir);
    std::string root_str = root.string();
    std::string target_str = canonical.string();

    // Ensure canonical target is inside root (or equals root, which would be a
    // directory, caught by the regular-file check below).
    if (target_str != root_str &&
        target_str.find(root_str + static_cast<char>(fs::path::preferred_separator)) != 0) {
        return std::nullopt;
    }

    if (!fs::exists(canonical) || !fs::is_regular_file(canonical)) return std::nullopt;
    return canonical;
}

void serve_fallback(const fs::path& web_dir, const OmniNet::HTTP::Request& req,
                    OmniNet::HTTP::Response& resp) {
    (void)req;
    static const std::string fallback = std::string(R"(<!DOCTYPE html>
<html><head><title>HiveMind</title></head>
<body><h1>HiveMind Node Graph</h1>
<p>web/dist/ not found at <code>)") + web_dir.string() + R"(</code>.</p>
<p>The API is up; the UI bundle is missing.</p>
</body></html>)";
    resp = OmniNet::HTTP::Response::ok();
    resp.setContentType("text/html; charset=utf-8");
    resp.setHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    hivemind::routes::apply_security_headers(resp);
    resp.setBody(fallback);
}

}  // namespace

void register_static_routes(OmniNet::HTTP::Server& server) {
    const fs::path web_dir = discover_web_dir();

    if (!fs::exists(web_dir)) {
        server.get("/", [web_dir](const OmniNet::HTTP::Request& req,
                                  OmniNet::HTTP::Response& resp) {
            serve_fallback(web_dir, req, resp);
        });
        return;
    }

    // Catch-all registered LAST — specific API routes match first.
    server.get("/*", [web_dir](const OmniNet::HTTP::Request& req,
                              OmniNet::HTTP::Response& resp) {
        try {
            const std::string_view path = req.path();
            if (path.rfind("/omninet/", 0) == 0) {
                resp = OmniNet::HTTP::Response::notFound();
                return;
            }

            auto file_opt = resolve_static_path(web_dir, path);
            if (!file_opt) {
                resp = OmniNet::HTTP::Response::notFound();
                return;
            }

            auto content_opt = read_file_text(*file_opt);
            if (!content_opt) {
                std::cerr << "[StaticRoutes] failed to read " << file_opt->string()
                          << ": " << std::strerror(errno) << "\n";
                resp = OmniNet::HTTP::Response::internalError("failed to read file");
                return;
            }

            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType(mime_type_for(*file_opt));
            // The UI is a local dev app — never let the browser serve a
            // stale index.html/app.js from cache (this has bitten us twice:
            // old JS + new HTML = broken page wiring).
            resp.setHeader("Cache-Control", "no-cache, no-store, must-revalidate");
            hivemind::routes::apply_security_headers(resp);
            resp.setBody(*content_opt);
        } catch (const std::exception& e) {
            std::cerr << "[StaticRoutes] error serving '" << req.path()
                      << "': " << e.what() << "\n";
            resp = OmniNet::HTTP::Response::internalError(e.what());
        }
    });
}
