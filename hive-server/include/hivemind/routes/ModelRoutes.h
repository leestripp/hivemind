#pragma once

namespace OmniNet::HTTP { class Server; }
class LlmStore;

/// Phase 43: hub-local provider/model CRUD (replaces the worker relay).
/// Registered with admin scope in main.cpp's middleware.
void register_model_routes(OmniNet::HTTP::Server& server, LlmStore& llm);
