// src/AgentTools.cpp - Phase C: worker skill-sync helpers only.
// (The daedalus Tool classes moved into hive-agent on workers.)

#include "hivemind/AgentTools.h"
#include "hivemind/AgentStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {
WorkerStore*   g_workers = nullptr;
WorkerChannel* g_wchannel = nullptr;

fs::path agent_skills_root(const std::string& agent_id) {
    return AgentStore::agents_base_dir() / agent_id / ".agents" / "skills";
}
} // namespace

void agent_tools::install_worker_access(WorkerStore* workers,
                                        WorkerChannel* channel) {
    g_workers = workers;
    g_wchannel = channel;
}

void agent_tools::push_skill_to_worker(const RosterAgent& agent,
                                       const std::string& skill) {
    if (!g_wchannel || agent.worker.empty() || agent.worker == "local")
        return;
    if (!g_wchannel->is_online(agent.worker)) {
        std::fprintf(stderr, "[skills] worker %s offline - skill %s for %s "
                     "will sync on reconnect\n",
                     agent.worker.c_str(), skill.c_str(), agent.name.c_str());
        return;
    }
    auto r = g_wchannel->push_skill(agent.worker, agent.id, skill);
    if (!r)
        std::fprintf(stderr, "[skills] push %s -> %s: worker went offline\n",
                     skill.c_str(), agent.worker.c_str());
    else if (!r->ok)
        std::fprintf(stderr, "[skills] push %s -> %s FAILED: %s\n",
                     skill.c_str(), agent.worker.c_str(), r->error.c_str());
    else
        std::fprintf(stderr, "[skills] pushed %s -> %s (%d files, %lld B)\n",
                     skill.c_str(), agent.worker.c_str(), r->files,
                     (long long)r->bytes);
}

void agent_tools::remove_skill_on_worker(const RosterAgent& agent,
                                         const std::string& skill) {
    if (!g_wchannel || agent.worker.empty() || agent.worker == "local")
        return;
    if (!g_wchannel->is_online(agent.worker)) return;
    g_wchannel->remove_skill(agent.worker, agent.id, skill);
}

std::vector<std::string> agent_tools::list_enabled_skills(
        const std::string& agent_id) {
    std::vector<std::string> out;
    const fs::path dir = agent_skills_root(agent_id);
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (de.is_symlink(ec)) out.push_back(de.path().filename().string());
    }
    return out;
}
