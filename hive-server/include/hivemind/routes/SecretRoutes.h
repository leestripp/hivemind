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

#include <functional>
#include <string>

namespace OmniNet::HTTP { class Server; class Request; class Response; }

class AgentStore;
namespace hivemind { class SecretStore; }

/// Invoked after every grant-affecting write (grant/revoke toggle, secret
/// delete cascade) with the affected agent id. Grant writes are DB-only —
/// a RUNNING agent's env (HIVE_SECRET_*) only changes when the hub pushes
/// a fresh spawn frame, so the hub wires this to HubRouter::spawn_agent
/// (2026-09-29 vault-loss incident: UI re-grants never reached running
/// agents until a full worker restart).
using GrantsChangedFn = std::function<void(const std::string& agent_id)>;

void register_secret_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            hivemind::SecretStore& secrets,
                            GrantsChangedFn on_grants_changed = {});
