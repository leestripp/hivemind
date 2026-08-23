// tests/test_llm_store.cpp — LLMStore CRUD + resolution, no LLM calls.
#include "hivemind/LLMStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_llm_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    LLMStore store{tmp / "llm.db"};

    // ── providers ─────────────────────────────────────────────────────
    LlmProvider kimi;
    kimi.name = "kimi";
    kimi.host = "https://api.kimi.com/coding";
    kimi.api_key = "sk-test-123";
    auto p = store.create_provider(kimi);
    assert(p.name == "kimi");
    assert(p.chat_endpoint == "/v1/chat/completions");   // default filled
    assert(p.discovery_endpoint == "/v1/models");        // default filled
    assert(p.timeout_seconds == 30);

    // duplicate name rejected
    bool threw = false;
    try { store.create_provider(kimi); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // missing host rejected
    threw = false;
    try { store.create_provider(LlmProvider{.name = "bad"}); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    LlmProvider local;
    local.name = "lmstudio";
    local.host = "http://dev-rig:1234";
    local.api_key = "";
    store.create_provider(local);
    assert(store.list_providers().size() == 2);

    // ── models ────────────────────────────────────────────────────────
    LlmModel k27;
    k27.name = "kimi-k2.7";
    k27.provider = "kimi";
    k27.model_id = "kimi-for-coding";
    k27.temperature = 0.6;
    auto m = store.create_model(k27);
    assert(m.name == "kimi-k2.7" && !m.is_default);

    // unknown provider rejected
    threw = false;
    try { store.create_model(LlmModel{.name="x", .provider="nope", .model_id="y"}); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    LlmModel qwen;
    qwen.name = "qwen36";
    qwen.provider = "lmstudio";
    qwen.model_id = "qwen/qwen3.6-35b-a3b";
    qwen.max_context_size = 128000;
    store.create_model(qwen);

    // default handling: exactly one default at a time
    assert(!store.default_model());
    assert(store.set_default_model("qwen36"));
    assert(store.default_model()->name == "qwen36");
    assert(store.set_default_model("kimi-k2.7"));
    assert(store.default_model()->name == "kimi-k2.7");
    auto models = store.list_models();
    int defaults = 0;
    for (auto& mm : models) if (mm.is_default) ++defaults;
    assert(defaults == 1);
    assert(!store.set_default_model("nonexistent"));

    // ── resolution ────────────────────────────────────────────────────
    auto r = store.resolve("kimi-k2.7");
    assert(r && r->host == "https://api.kimi.com/coding");
    assert(r->api_key == "sk-test-123");
    assert(r->model_id == "kimi-for-coding");
    assert(r->temperature == 0.6);
    assert(r->chat_endpoint == "/v1/chat/completions");

    // empty name resolves the default
    auto d = store.resolve("");
    assert(d && d->name == "kimi-k2.7");

    // unknown name resolves to nothing
    assert(!store.resolve("nope"));

    // ── referential integrity ─────────────────────────────────────────
    assert(!store.delete_provider("kimi"));        // model references it
    assert(store.delete_model("kimi-k2.7"));
    assert(store.delete_provider("kimi"));
    assert(store.list_providers().size() == 1);
    assert(store.delete_model("qwen36"));
    assert(!store.default_model());

    fs::remove_all(tmp);
    std::cout << "test_llm_store OK\n";
    return 0;
}
