// Curated starter-skill installation into writable workspace-global skills.
// The hub tracks only catalog entries it installed; it does not inventory
// agents' personal skills or arbitrary workspace skills.
#pragma once

#include <string>

namespace OmniNet::HTTP { class Server; class Request; class Response; }

class AgentStore;
class WorkerChannel;
class WorkerStore;

void register_skill_routes(OmniNet::HTTP::Server& server, WorkerStore& workers,
                           WorkerChannel& channel);
void sync_installed_starter_skills(WorkerChannel& channel,
                                   const std::string& worker);
