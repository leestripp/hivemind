#pragma once

#include <OmniNet.h>

#include "hivemind/AuthStore.h"

/**
 * API key management routes (HiveMind 2.0, Plan.md Phase 9).
 * All three require an admin-scope key (enforced by the auth middleware):
 *
 *   GET    /api/keys        list keys (public ids only — never secrets)
 *   POST   /api/keys        create {name, scopes} → returns the RAW key once
 *   DELETE /api/keys/:id    revoke by public id
 */
void register_key_routes(OmniNet::HTTP::Server& server, AuthStore& auth);
