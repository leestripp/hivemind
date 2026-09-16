#include "UiApi.h"

#include "HiveWindow.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace hive {

namespace {

void send_json(OmniNet::HTTP::Response& resp, const json& j, int status = 200) {
    resp.setStatus(status);
    resp.setHeader("Content-Type", "application/json");
    resp.setBody(j.dump());
}

void send_err(OmniNet::HTTP::Response& resp, int status, const std::string& msg) {
    send_json(resp, json{{"error", msg}}, status);
}

json body_json(const OmniNet::HTTP::Request& req) {
    try { return json::parse(std::string{req.bodyAsString()}); }
    catch (...) { return json::object(); }
}

} // namespace

UiApi::UiApi(HiveWindow& window) : window_(window) {}
UiApi::~UiApi() { stop(); }

void UiApi::start() {
    const char* env = std::getenv("HIVEMIND_UI_PORT");
    std::string port_s = env ? env : "8481";
    if (port_s == "0" || port_s == "off") return;   // explicitly disabled
    long port = std::atol(port_s.c_str());
    if (port <= 0 || port > 65535) port = 8481;

    OmniNet::HTTP::Server::Config cfg;
    cfg.address = "127.0.0.1";          // localhost only — test surface
    cfg.port = static_cast<uint16_t>(port);
    cfg.workerCount = 2;
    cfg.logRequests = false;
    server_ = std::make_unique<OmniNet::HTTP::Server>(cfg);

    auto& w = window_;

    server_->get("/ui/health",
        [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            send_json(resp, json{{"ok", true}, {"app", "hive-client"}});
        });

    server_->get("/ui/state",
        [&w](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            send_json(resp, w.ui_state());
        });

    server_->get("/ui/messages",
        [&w](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            send_json(resp, json{{"messages", w.ui_messages()}});
        });

    server_->post("/ui/action/new-thread",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b = body_json(req);
            std::vector<std::string> ids;
            if (b.contains("agent_ids") && b["agent_ids"].is_array())
                for (auto& v : b["agent_ids"])
                    if (v.is_string()) ids.push_back(v.get<std::string>());
            if (ids.empty()) { send_err(resp, 400, "agent_ids required"); return; }
            try {
                json t = w.ui_create_thread(b.value("title", ""), ids);
                send_json(resp, t, 201);
            } catch (const std::exception& e) { send_err(resp, 400, e.what()); }
        });

    server_->post("/ui/action/select-thread",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b = body_json(req);
            std::string id = b.value("id", "");
            if (id.empty()) { send_err(resp, 400, "id required"); return; }
            json r = w.ui_select_thread(id);
            if (r.contains("error")) send_err(resp, 404, r["error"]);
            else send_json(resp, r);
        });

    server_->post("/ui/action/select-agent",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b = body_json(req);
            std::string target = b.value("agent", "");
            if (target.empty()) { send_err(resp, 400, "agent required"); return; }
            send_json(resp, w.ui_select_agent(target));
        });

    server_->post("/ui/action/send",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b = body_json(req);
            std::string body = b.value("body", "");
            if (body.empty()) { send_err(resp, 400, "body required"); return; }
            json r = w.ui_send(body);
            if (r.contains("error")) send_err(resp, 400, r["error"]);
            else send_json(resp, r, 202);
        });

    // Same code path as the header theme toggle button.
    server_->post("/ui/action/open-dialog",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            send_json(resp, w.ui_open_dialog(
                body_json(req).value("name", "")));
        });

    // Drive the real connect flow headlessly (regression for the Connect
    // dialog): blocks until the probe finishes, reports the actual error.
    server_->post("/ui/action/connect",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b = body_json(req);
            std::mutex m; std::condition_variable cv; bool done_flag = false;
            std::string err_out = "no callback";
            w.connect_with_credentials(b.value("server", ""), b.value("key", ""),
                [&](std::string err) {
                    { std::lock_guard<std::mutex> lk(m); err_out = err; done_flag = true; }
                    cv.notify_one();
                });
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(20), [&] { return done_flag; });
            if (err_out.empty()) send_json(resp, json{{"ok", true}});
            else send_json(resp, json{{"ok", false}, {"error", err_out}});
        });

    server_->post("/ui/action/stop",
        [&w](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            send_json(resp, w.ui_stop());
        });

    server_->post("/ui/action/delete-thread",
        [&w](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b = body_json(req);
            std::string id = b.value("id", "");
            if (id.empty()) { send_err(resp, 400, "id required"); return; }
            json r = w.ui_delete_thread(id);
            if (r.contains("error")) send_err(resp, 404, r["error"]);
            else send_json(resp, r);
        });

    server_->startAsync();
    std::fprintf(stderr, "[hive-client] UiApi on http://127.0.0.1:%ld/ui/health\n",
                 port);
}

void UiApi::stop() {
    if (server_) { server_->stop(); server_.reset(); }
}

} // namespace hive
