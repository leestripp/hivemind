// ConnectorRoutes.cpp — Phase 23: OAuth2 authorization-code flow.
#include "hivemind/routes/ConnectorRoutes.h"

#include "hivemind/ConnectorStore.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ContainerManager.h"   // shell_quote
#include "hivemind/routes/RouteUtil.h"

#include <cstdio>
#include <string>

using json = nlohmann::json;
using hivemind::SecretStore;

namespace {

using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;

// OAuth code/token values are URL-safe base64-ish; anything else is bogus.
bool valid_oauth_value(const std::string& v) {
    if (v.empty() || v.size() > 2048) return false;
    for (char c : v) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.' || c == '~';
        if (!ok) return false;
    }
    return true;
}

// connector name "my-svc" → "MY_SVC_ACCESS_TOKEN"
std::string secret_name(const std::string& conn, const char* suffix) {
    std::string out;
    for (char c : conn)
        out += (c == '-') ? '_' : static_cast<char>(std::toupper((unsigned char)c));
    return out + suffix;
}

std::string url_encode(const std::string& v) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void send_html(OmniNet::HTTP::Response& resp, const std::string& title,
               const std::string& body, int status = 200) {
    resp = OmniNet::HTTP::Response::ok();
    resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
    resp.setContentType("text/html; charset=utf-8");
    resp.setBody("<!doctype html><title>" + title + "</title>"
                 "<body style=\"font-family:system-ui;background:#0e1116;color:#d8dee9;"
                 "display:grid;place-items:center;height:100vh;margin:0\">"
                 "<div style=\"text-align:center\">" + body + "</div></body>");
}

} // namespace

void register_connector_routes(OmniNet::HTTP::Server& server,
                               ConnectorStore& connectors,
                               SecretStore& secrets) {
    // ── POST /api/connectors — register (admin) ───────────────────────
    server.post("/api/connectors",
        [&connectors](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            Connector c;
            c.name          = body.value("name", "");
            c.auth_url      = body.value("auth_url", "");
            c.token_url     = body.value("token_url", "");
            c.client_id     = body.value("client_id", "");
            c.client_secret = body.value("client_secret", "");
            c.scopes        = body.value("scopes", "");
            try {
                if (!connectors.create(c)) {
                    send_error(resp, 409, "connector exists (delete first)"); return;
                }
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what()); return;
            }
            send_json(resp, json{{"created", c.name}}, 201);
        });

    // ── GET /api/connectors — list (secret masked) ────────────────────
    server.get("/api/connectors",
        [&connectors, &secrets](const OmniNet::HTTP::Request&,
                                OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& c : connectors.list()) {
                const bool connected =
                    secrets.list().empty() ? false :
                    [&] {
                        const std::string want = secret_name(c.name, "_ACCESS_TOKEN");
                        for (auto& s : secrets.list())
                            if (s.name == want) return true;
                        return false;
                    }();
                list.push_back({
                    {"name", c.name}, {"auth_url", c.auth_url},
                    {"token_url", c.token_url}, {"client_id", c.client_id},
                    {"has_secret", !c.client_secret.empty()},
                    {"scopes", c.scopes}, {"created_at", c.created_at},
                    {"connected", connected},
                    {"token_secret", secret_name(c.name, "_ACCESS_TOKEN")},
                });
            }
            send_json(resp, json{{"connectors", list}});
        });

    // ── DELETE /api/connectors/:name ──────────────────────────────────
    server.delete_("/api/connectors/:name",
        [&connectors](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "bad path"); return; }
            if (!connectors.remove(seg[2])) { send_error(resp, 404, "not found"); return; }
            send_json(resp, json{{"deleted", seg[2]}});
        });

    // ── GET /api/connectors/:name/auth-url ────────────────────────────
    server.get("/api/connectors/:name/auth-url",
        [&connectors](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api connectors :name auth-url
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            auto c = connectors.get(seg[2]);
            if (!c) { send_error(resp, 404, "connector not found"); return; }
            const std::string state = connectors.issue_state(c->name);
            if (state.empty()) { send_error(resp, 500, "cannot mint state"); return; }
            const auto host = req.header("Host");
            const std::string redirect =
                "http://" + (host ? std::string{*host} : "127.0.0.1:8421") +
                "/api/oauth/callback";
            std::string url = c->auth_url +
                (c->auth_url.find('?') == std::string::npos ? "?" : "&") +
                "response_type=code&client_id=" + url_encode(c->client_id) +
                "&redirect_uri=" + url_encode(redirect) +
                "&state=" + url_encode(state);
            if (!c->scopes.empty()) url += "&scope=" + url_encode(c->scopes);
            send_json(resp, json{{"url", url}, {"state", state}});
        });

    // ── GET /api/oauth/callback?code&state — PUBLIC (state = credential)
    server.get("/api/oauth/callback",
        [&connectors, &secrets](const OmniNet::HTTP::Request& req,
                                OmniNet::HTTP::Response& resp) {
            const std::string code  = req.queryParameter("code", "");
            const std::string state = req.queryParameter("state", "");
            const std::string err   = req.queryParameter("error", "");
            if (!err.empty()) {
                send_html(resp, "Authorization failed",
                          "<h2>Authorization failed</h2><p>" + err + "</p>", 400);
                return;
            }
            auto cname = connectors.consume_state(state);
            if (!cname) {
                send_html(resp, "Invalid state",
                          "<h2>Invalid or expired state</h2>"
                          "<p>Restart the connection from HiveMind.</p>", 400);
                return;
            }
            auto c = connectors.get(*cname);
            if (!c || !valid_oauth_value(code)) {
                send_html(resp, "Bad callback", "<h2>Bad callback parameters</h2>", 400);
                return;
            }
            const auto host = req.header("Host");
            const std::string redirect =
                "http://" + (host ? std::string{*host} : "127.0.0.1:8421") +
                "/api/oauth/callback";

            // Token exchange via curl (values single-quoted after the
            // strict charset check above; endpoints validated at create).
            std::string cmd =
                "curl -sS --max-time 20 -X POST " +
                ContainerManager::shell_quote(c->token_url) +
                " -H 'Content-Type: application/x-www-form-urlencoded'"
                " --data-urlencode 'grant_type=authorization_code'"
                " --data-urlencode " + ContainerManager::shell_quote("code=" + code) +
                " --data-urlencode " + ContainerManager::shell_quote("client_id=" + c->client_id) +
                " --data-urlencode " + ContainerManager::shell_quote("redirect_uri=" + redirect);
            if (!c->client_secret.empty())
                cmd += " --data-urlencode " +
                       ContainerManager::shell_quote("client_secret=" + c->client_secret);
            cmd += " 2>/dev/null";

            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                send_html(resp, "Exchange failed", "<h2>Token exchange failed</h2>", 502);
                return;
            }
            std::string out;
            char buf[2048];
            while (fgets(buf, sizeof buf, pipe)) out += buf;
            if (pclose(pipe) != 0 || out.empty()) {
                send_html(resp, "Exchange failed",
                          "<h2>Token exchange failed</h2><p>Provider unreachable.</p>", 502);
                return;
            }
            json tok = json::parse(out, nullptr, false);
            const std::string access  = tok.value("access_token", "");
            if (access.empty()) {
                send_html(resp, "Exchange failed",
                          "<h2>Token exchange failed</h2>"
                          "<p>No access_token in the provider response.</p>", 502);
                return;
            }
            // Rotation = delete + put (broker rejects duplicates by design).
            const std::string aname = secret_name(c->name, "_ACCESS_TOKEN");
            secrets.remove(aname);
            secrets.put(aname, access);
            const std::string refresh = tok.value("refresh_token", "");
            if (!refresh.empty()) {
                const std::string rname = secret_name(c->name, "_REFRESH_TOKEN");
                secrets.remove(rname);
                secrets.put(rname, refresh);
            }
            send_html(resp, "Connected",
                      "<h2>✅ " + c->name + " connected</h2>"
                      "<p>Token stored as <code>" + aname + "</code> — "
                      "grant it to agents from the Secrets page.</p>"
                      "<p>You can close this tab.</p>");
        });
}
