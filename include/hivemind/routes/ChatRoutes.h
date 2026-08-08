#pragma once

#include <OmniNet.h>

class GraphStore;
class ExecutorRegistry;

/// Chat endpoints for the HiveMind assistant:
///   POST   /api/chat/message   {"message": "..."} → 202 (turn runs async)
///   GET    /api/chat/messages  → {messages: [...], pending: bool}
///   DELETE /api/chat/messages  → clear history, fresh agent session
void register_chat_routes(OmniNet::HTTP::Server& server,
                          GraphStore& store,
                          ExecutorRegistry& registry);
