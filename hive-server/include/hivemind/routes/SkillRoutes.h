// include/hivemind/routes/SkillRoutes.h
// -----------------------------------------------------------------------------
// Global skills library (HiveMind 2.0). daedalus discovers per-agent skills in
// <agent_dir>/.agents/skills (project-local). HiveMind adds a managed GLOBAL
// library at ~/.local/share/hivemind/skills/; "enabling" a global skill for an
// agent symlinks it into the agent's skills dir, so there is one source of
// truth and edits propagate to every enabled agent.
//
//   GET    /api/skills                        library + per-skill enabled agents
//   POST   /api/skills                        {name, description, content}
//   DELETE /api/skills/:name                  remove from library + all symlinks
//   GET    /api/agents/:id/global-skills      enabled global skill names
//   POST   /api/agents/:id/global-skills      {name, enabled}  (toggle symlink)
// -----------------------------------------------------------------------------
#pragma once

namespace OmniNet::HTTP { class Server; class Request; class Response; }

class AgentStore;

void register_skill_routes(OmniNet::HTTP::Server& server, AgentStore& agents);
