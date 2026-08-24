// include/hivemind/routes/SecretRoutes.h
// -----------------------------------------------------------------------------
// Secret broker (Phase 15). Secrets are AES-256-GCM-encrypted at rest
// (SecretStore) and delivered to agents WITHOUT entering the LLM context:
// container exec env-file injection (and, later, browser fill_secret).
// Values are write-only over the API — GET returns names + grants only.
//
//   GET    /api/secrets                list {name, created_at, agents[]}
//   POST   /api/secrets                {name, value}  → 201 (409 if exists)
//   DELETE /api/secrets/:name          remove secret + all grants
//   GET    /api/agents/:id/secrets     granted secret names
//   POST   /api/agents/:id/secrets     {name, granted}  (toggle grant)
// All routes are admin-scoped (main.cpp middleware).
// -----------------------------------------------------------------------------
#pragma once

namespace OmniNet::HTTP { class Server; class Request; class Response; }

class AgentStore;
namespace hivemind { class SecretStore; }

void register_secret_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            hivemind::SecretStore& secrets);
