#pragma once

// =============================================================================
// McpBridge — Phase 33: loopback streamable-http → worker-channel MCP tunnel.
//
// daedalus agents (brain on this host) speak MCP "streamable-http": POST
// JSON-RPC to a URL. For agents whose container runs on a hive-worker rig,
// AgentHost rewrites their mcp_servers entries to point at this bridge
// (http://127.0.0.1:<mcp_bridge_port>/mcp/<agent>/<server>) and registers
// the real spawn spec here via configure(). Each POST is wrapped in an
// mcp_rpc channel op; the worker lazily spawns the MCP server INSIDE the
// agent's container (podman exec -i) and bridges stdio NDJSON. Replies
// return synchronously (McpJob in WorkerChannel). Loopback only — never
// expose this port.
// =============================================================================

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace OmniNet::HTTP { class Server; }
class WorkerChannel;

class McpBridge {
public:
    McpBridge(WorkerChannel& channel, int port);
    ~McpBridge();

    void start();   // spawns the accept loop thread
    void stop();

    /// Register/replace the spawn spec for (agent, server). Called by
    /// AgentHost when it rewrites a worker agent's mcp_servers entry.
    void configure(const std::string& agent, const std::string& server,
                   const std::string& worker, std::string command,
                   std::vector<std::string> args, nlohmann::json env);
    /// Drop all specs for an agent (delete/evict).
    void remove_agent(const std::string& agent);

private:
    struct Spec {
        std::string worker;
        std::string command;
        std::vector<std::string> args;
        nlohmann::json env;
    };

    WorkerChannel& channel_;
    int port_;
    std::unique_ptr<OmniNet::HTTP::Server> http_;
    std::jthread thread_;

    std::mutex m_;
    std::map<std::pair<std::string, std::string>, Spec> specs_;  // (agent, server)
};
