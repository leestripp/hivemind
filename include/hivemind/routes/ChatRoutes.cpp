// include/hivemind/routes/ChatRoutes.cpp — HTTP surface for the Chat page.

#include "hivemind/routes/ChatRoutes.h"
#include "hivemind/chat/ChatAgent.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

void register_chat_routes(OmniNet::HTTP::Server& server,
                          GraphStore& store,
                          ExecutorRegistry& registry) {

    server.post("/api/chat/message",
        [&store, &registry](const OmniNet::HTTP::Request& req,
                            OmniNet::HTTP::Response& resp) {
            json body = json::parse(req.bodyAsString(), nullptr, false);
            if (!body.is_object() || !body.contains("message")
                || !body["message"].is_string()
                || body["message"].get<std::string>().empty()) {
                resp = OmniNet::HTTP::Response::badRequest(
                    "expected {\"message\": \"...\"}");
                return;
            }
            auto& ca = chat_agent(&store, &registry);
            if (body.contains("model") && body["model"].is_string())
                ca.set_model(body["model"].get<std::string>());
            bool started = ca.send_async(body["message"].get<std::string>());
            json out{{"started", started}};
            if (!started) out["error"] = "a turn is already in flight";
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/json");
            resp.setBody(out.dump());
        });

    server.get("/api/chat/messages",
        [&store, &registry](const OmniNet::HTTP::Request&,
                            OmniNet::HTTP::Response& resp) {
            auto& ca = chat_agent(&store, &registry);
            json out{{"messages", ca.history()},
                     {"pending", ca.pending()},
                     {"model",   ca.current_model()}};
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/json");
            resp.setBody(out.dump());
        });

    server.delete_("/api/chat/messages",
        [&store, &registry](const OmniNet::HTTP::Request&,
                            OmniNet::HTTP::Response& resp) {
            chat_agent(&store, &registry).reset();
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/json");
            resp.setBody(R"({"cleared":true})");
        });
}
