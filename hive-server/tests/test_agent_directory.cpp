// tests/test_agent_directory.cpp — agent_directory_json(): the Phase 40
// task-frame roster shape behind the agents' list_agents tool.
// 2026-10-18 (Plan.md): entries must carry the WORKER (machine name —
// "droplet", "ubuntu-pc", …) alongside id + name; agents coordinate
// across rigs and had no way to tell who lives where. No LLM, no
// WorkerChannel required.
#include "hivemind/AgentStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>

namespace fs = std::filesystem;
using nlohmann::json;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_agent_directory";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore store{tmp / "agents.db"};
    auto buzz  = store.create_agent("Buzz",  "", "", "", false, "", "droplet");
    auto bob   = store.create_agent("Bob",   "", "", "", false, "", "ubuntu-pc");
    auto local = store.create_agent("Local", "", "", "", false, "", "");

    const json dir = agent_directory_json(store.list_agents());
    assert(dir.is_array());
    assert(dir.size() == 3);

    // Every entry: id + name + worker — worker is the 2026-10-18 field.
    std::map<std::string, json> by_name;
    for (const auto& e : dir) {
        assert(e.contains("id"));
        assert(e.contains("name"));
        assert(e.contains("worker") && "directory entry must carry the machine");
        by_name[e["name"]] = e;
    }
    assert(by_name.at("Buzz").at("worker") == "droplet");
    assert(by_name.at("Bob").at("worker") == "ubuntu-pc");
    // Unset worker persists as the "local" default (AgentStore col-10 rule),
    // never an empty string — agents key on this value.
    assert(by_name.at("Local").at("worker") == "local");
    // Ids survive the round trip (message_agent targets them).
    assert(by_name.at("Buzz").at("id") == buzz.id);
    assert(by_name.at("Bob").at("id") == bob.id);

    fs::remove_all(tmp);
    std::cout << "test_agent_directory: OK\n";
    return 0;
}
