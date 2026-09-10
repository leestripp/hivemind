// Phase 30.1: WorkerStore — enroll/validate/list/remove, key hashing.
#include "hivemind/WorkerStore.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path dir = fs::temp_directory_path() / "hivemind-test-workers";
    fs::remove_all(dir);
    fs::create_directories(dir);

    WorkerStore ws{dir / "workers.db"};

    // ── name validation ────────────────────────────────────────────────
    assert(WorkerStore::valid_name("rig-gpu"));
    assert(WorkerStore::valid_name("a"));
    assert(!WorkerStore::valid_name(""));
    assert(!WorkerStore::valid_name("Has_Caps"));
    assert(!WorkerStore::valid_name(std::string(33, 'x')));

    // ── enroll returns a raw hmwk_ key, stores only the hash ──────────
    std::string raw = ws.enroll("rig-gpu");
    assert(raw.rfind("hmwk", 0) == 0);

    // duplicate enroll throws
    bool threw = false;
    try { ws.enroll("rig-gpu"); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // bad names throw
    threw = false;
    try { ws.enroll("BAD NAME"); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── validate: correct key → worker, last_seen refreshed ───────────
    auto w = ws.validate(raw, R"({"gpu":true,"arch":"x86_64"})", 1);
    assert(w && w->name == "rig-gpu");
    assert(!w->last_seen.empty());
    assert(w->caps.find("gpu") != std::string::npos);
    assert(w->proto == 1);

    // heartbeat validate (no caps) must NOT clobber the hello caps
    w = ws.validate(raw);
    assert(w && w->caps.find("gpu") != std::string::npos);

    // wrong key → nullopt
    assert(!ws.validate("hmwk_wrongwrongwrong"));

    // ── list / get ────────────────────────────────────────────────────
    ws.enroll("laptop");
    auto all = ws.list();
    assert(all.size() == 2);
    assert(all[0].name == "laptop");      // ORDER BY name
    assert(all[1].name == "rig-gpu");
    assert(ws.get("laptop")->last_seen.empty());   // never connected

    // ── remove revokes the key with the row ───────────────────────────
    assert(ws.remove("laptop"));
    assert(!ws.remove("laptop"));
    assert(ws.list().size() == 1);

    fs::remove_all(dir);
    std::cout << "test_worker_store: all assertions passed\n";
    return 0;
}
