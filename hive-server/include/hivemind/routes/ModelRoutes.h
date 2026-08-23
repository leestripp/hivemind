#pragma once

#include <OmniNet.h>

class LLMStore;

/// Providers + models API (Phase 13): HiveMind-owned LLM configuration
/// backed by LLMStore (~/.local/share/hivemind/llm.db). Replaces the old
/// read-only view over daedalus' SettingsStore.
void register_model_routes(OmniNet::HTTP::Server& server, LLMStore& llm);
