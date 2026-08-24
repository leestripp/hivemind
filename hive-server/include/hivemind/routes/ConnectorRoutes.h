#pragma once

// =============================================================================
// ConnectorRoutes.h — Phase 23: OAuth2 authorization-code flow for external
// services. Tokens land in the secret broker (<NAME>_ACCESS_TOKEN /
// <NAME>_REFRESH_TOKEN) so grants/env-injection/browser-fill just work.
//
//   POST   /api/connectors                 register (admin)
//   GET    /api/connectors                 list (secret masked, connected flag)
//   DELETE /api/connectors/:name           remove (admin)
//   GET    /api/connectors/:name/auth-url  mint state, return provider URL
//   GET    /api/oauth/callback?code&state  PUBLIC — provider redirect target;
//                                          the one-shot state is the credential
// =============================================================================

#include <OmniNet.h>

class ConnectorStore;
namespace hivemind { class SecretStore; }

void register_connector_routes(OmniNet::HTTP::Server& server,
                               ConnectorStore& connectors,
                               hivemind::SecretStore& secrets);
