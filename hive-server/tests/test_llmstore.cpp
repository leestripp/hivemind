// tests/v2/test_llmstore.cpp — Phase 43: hub LlmStore pure tests.
#include "hivemind/LlmStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
    const fs::path tmp = fs::temp_directory_path() / "hive_test_llmstore.db";
    fs::remove(tmp);

    LlmStore store(tmp);

    // provider upsert + validation
    ProviderInfo p;
    p.name = "KimiCode"; p.host = "https://api.kimi.com/coding";
    p.api_key = "sk-test-123"; p.chat_endpoint = "https://api.kimi.com/coding/v1/chat/completions";
    store.upsert_provider(p);
    bool threw = false;
    try { store.upsert_provider(ProviderInfo{}); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    assert(store.list_providers().size() == 1);
    assert(store.get_provider("KimiCode")->api_key == "sk-test-123");
    // upsert update keeps row, changes key
    p.api_key = "sk-test-456";
    store.upsert_provider(p);
    assert(store.list_providers().size() == 1);
    assert(store.get_provider("KimiCode")->api_key == "sk-test-456");

    // model upsert requires a known provider
    threw = false;
    try { LlmModelInfo m; m.name = "k3"; m.provider = "nope"; store.upsert_model(m); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    LlmModelInfo m; m.name = "k3"; m.provider = "KimiCode";
    m.model_id = "kimi-k3-0905-preview"; m.max_context = 262144; m.temperature = 0.6;
    store.upsert_model(m);
    LlmModelInfo m2; m2.name = "k3-256k"; m2.provider = "KimiCode";  // model_id defaults to name
    store.upsert_model(m2);
    assert(store.list_models().size() == 2);
    assert(store.list_models()[1].model_id == "k3-256k" ||
           store.list_models()[0].model_id == "k3-256k");

    // default model + resolve
    assert(store.default_model().empty());
    threw = false;
    try { store.set_default_model("ghost"); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    store.set_default_model("k3");
    assert(store.default_model() == "k3");

    LlmModelInfo r;
    auto prov = store.resolve("k3-256k", &r);
    assert(prov && prov->name == "KimiCode" && prov->api_key == "sk-test-456");
    assert(r.name == "k3-256k" && r.model_id == "k3-256k" && r.max_context == 0);
    auto prov2 = store.resolve("", &r);   // default resolution
    assert(prov2 && r.name == "k3");
    assert(!store.resolve("ghost"));

    // remove model clears a default pointing at it
    store.set_default_model("k3-256k");
    assert(store.remove_model("k3-256k"));
    assert(store.default_model().empty());
    assert(!store.remove_model("k3-256k"));

    // provider removal cascades its models
    assert(store.remove_provider("KimiCode"));
    assert(store.list_models().empty());
    assert(!store.remove_provider("KimiCode"));

    fs::remove(tmp);
    std::cout << "test_llmstore: all OK\n";
    return 0;
}
