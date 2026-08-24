// tests/test_connector_store.cpp — Phase 23: connector CRUD + OAuth state.
#include "hivemind/ConnectorStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

static Connector mk(const std::string& name) {
    Connector c;
    c.name = name;
    c.auth_url = "https://provider.example/authorize";
    c.token_url = "https://provider.example/token";
    c.client_id = "client-123";
    c.client_secret = "shh";
    c.scopes = "read write";
    return c;
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_connector_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    ConnectorStore store{tmp / "connectors.db"};

    // validation
    assert(ConnectorStore::valid_name("github"));
    assert(ConnectorStore::valid_name("my-svc-2"));
    assert(!ConnectorStore::valid_name(""));
    assert(!ConnectorStore::valid_name("GitHub"));        // no caps
    assert(!ConnectorStore::valid_name("bad name"));
    assert(ConnectorStore::valid_endpoint("https://x.example/cb"));
    assert(!ConnectorStore::valid_endpoint("https://"));
    assert(!ConnectorStore::valid_endpoint("http://evil.example/t"));
    assert(ConnectorStore::valid_endpoint("http://127.0.0.1:9999/t"));  // loopback ok
    assert(ConnectorStore::valid_endpoint("http://localhost:9999/t"));

    // create + get + list
    assert(store.create(mk("github")));
    assert(!store.create(mk("github")));                  // duplicate
    auto g = store.get("github");
    assert(g && g->client_secret == "shh");
    assert(store.list().size() == 1);
    // invalid endpoint THROWS (invalid input), nothing persisted
    { Connector b = mk("bad-ep"); b.token_url = "http://evil.example/t";
      try { store.create(b); assert(false); }
      catch (const std::invalid_argument&) {}
      assert(!store.get("bad-ep")); }
    // invalid name throws too
    try { Connector b = mk("BAD"); store.create(b); assert(false); }
    catch (const std::invalid_argument&) {}

    // state lifecycle: issue → consume once → replay dead
    assert(store.issue_state("nonexistent").empty());
    std::string st = store.issue_state("github");
    assert(!st.empty());
    auto c1 = store.consume_state(st);
    assert(c1 && *c1 == "github");
    assert(!store.consume_state(st));                     // one-shot
    assert(!store.consume_state("bogus-state"));

    // remove cascades pending states
    std::string st2 = store.issue_state("github");
    assert(store.remove("github"));
    assert(!store.consume_state(st2));
    assert(!store.remove("github"));
    assert(store.list().empty());

    fs::remove_all(tmp);
    std::cout << "test_connector_store: OK\n";
    return 0;
}
