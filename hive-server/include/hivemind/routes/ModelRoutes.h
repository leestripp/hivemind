#pragma once

namespace OmniNet::HTTP { class Server; }
class LlmStore;

/// Phase 43: hub-local provider/model CRUD (replaces the worker relay).
/// Hub control-plane CRUD. These are NOT hive-agent tools: the hub resolves
/// provider/model configuration into each task, and an agent cannot change
/// its own provider/model through this API.
void register_model_routes(OmniNet::HTTP::Server& server, LlmStore& llm);
