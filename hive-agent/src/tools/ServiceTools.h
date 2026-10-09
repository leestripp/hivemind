#pragma once

#include "types/BaseTool.h"

namespace hive {

using nlohmann::json;

// Service sharing tools (2026-10-09, Bob/SlateApe ingress).
//
// Every agent container gets a fixed host-port range published 1:1 onto
// container ports HIVEMIND_SERVICE_CONTAINER_BASE.. (worker-side, see
// hive-worker/src/runner.cpp). These tools let the agent REGISTER a
// service it runs (e.g. a web app on 8000) and learn the stable URL the
// worker publishes it at — no tunnels, no asking the operator.
//
//   share_service(name, port, health_path?) -> {url, healthy, ...}
//   list_services()                          -> [entries + live health]
//   unshare_service(name)                    -> removed
//
// Shared state is a manifest at $HIVE_WORKSPACE/.hivemind/services.json
// (agent-owned, survives container recreation with the workspace mount).
// The tools need HIVEMIND_SERVICE_HOST / _PORT_BASE / _PORT_COUNT /
// _CONTAINER_BASE from the worker's container env; absent env yields a
// clear error, not a crash.

class ShareServiceTool : public BaseTool {
public:
    std::string get_name() const override { return "share_service"; }

    std::string get_description() const override {
        return "Register a network service you run in this container "
               "(e.g. a web app) and get its stable public URL on the "
               "worker host. Ports "
               "HIVEMIND_SERVICE_CONTAINER_BASE..(+COUNT-1) are "
               "pre-published by the worker; binding one of them makes "
               "the service reachable at "
               "http://HOST:PORT_BASE+(port-CONTAINER_BASE) immediately — "
               "no tunnels, no recreate. Idempotent: re-sharing the same "
               "name updates the entry. Health-checks the port and "
               "reports healthy/http_status.";
    }

    json get_definition() const override {
        return {
            {"type", "function"},
            {"function",
             {{"name", get_name()},
              {"description", get_description()},
              {"parameters",
               {{"type", "object"},
                {"properties",
                 {{"name",
                   {{"type", "string"},
                    {"description",
                     "service name, [a-zA-Z0-9_-], e.g. \"slateape\""}}},
                  {"port",
                   {{"type", "integer"},
                    {"description",
                     "container port the service LISTENS on; must be in "
                     "the shared range CONTAINER_BASE..+COUNT-1"}}},
                  {"health_path",
                   {{"type", "string"},
                    {"description",
                     "optional HTTP path to GET for health, e.g. "
                     "\"/healthz\" (default: none — TCP connect only)"}}}}},
                {"required", json::array({"name", "port"})}}}}},
        };
    }

    json execute(const json& args, HostToolContext& ctx) override;
};

class ListServicesTool : public BaseTool {
public:
    std::string get_name() const override { return "list_services"; }

    std::string get_description() const override {
        return "List the services you have shared with share_service, "
               "with their stable URLs and a live health check (TCP + "
               "optional HTTP GET). Also reports your shared port range "
               "and host so you can plan new services.";
    }

    json get_definition() const override {
        return {
            {"type", "function"},
            {"function",
             {{"name", get_name()},
              {"description", get_description()},
              {"parameters", {{"type", "object"}, {"properties", json::object()}}}}},
        };
    }

    json execute(const json& args, HostToolContext& ctx) override;
};

class UnshareServiceTool : public BaseTool {
public:
    std::string get_name() const override { return "unshare_service"; }

    std::string get_description() const override {
        return "Remove a service previously shared with share_service. "
               "The port mapping itself stays published (it is per-agent, "
               "not per-service); this just drops the registry entry.";
    }

    json get_definition() const override {
        return {
            {"type", "function"},
            {"function",
             {{"name", get_name()},
              {"description", get_description()},
              {"parameters",
               {{"type", "object"},
                {"properties",
                 {{"name",
                   {{"type", "string"},
                    {"description", "service name to remove"}}}}},
                {"required", json::array({"name"})}}}}},
        };
    }

    json execute(const json& args, HostToolContext& ctx) override;
};

} // namespace hive
