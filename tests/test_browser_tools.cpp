// tests/test_browser_tools.cpp — Phase 6.2: browser tool schemas and the
// fill_secret grant gate. Live podman/chromium paths can't run in the dev
// sandbox; everything here fails gracefully instead.
#include "hivemind/BrowserTools.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/SecretStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
class StubCtx final : public daedalus::HostToolContext {
public:
    std::string read_file(const fs::path&) override { return {}; }
    void write_file(const fs::path&, const std::string&) override {}
    std::vector<std::string> list_directory(const fs::path&) override { return {}; }
    const fs::path& project_root() const override { return root_; }
    fs::path root_;
};
} // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_browser_tools";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    // Isolated store via the tool's override — NEVER the process-global
    // one (it uses HOME, not XDG, and holds real secrets).
    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};

    StubCtx ctx;
    const std::string aid = "agent-test-1";

    // ── schema sanity: 10 unique browser_* tools ──────────────────────────
    auto specs = BrowserTool::specs();
    assert(specs.size() == 10);
    std::set<std::string> names;
    for (auto& s : specs) {
        assert(s.name.rfind("browser_", 0) == 0);
        assert(names.insert(s.name).second);        // unique
        auto def = s.properties;
        for (auto& rk : s.required)
            assert(def.contains(rk));               // required keys exist
        for (auto& ak : s.arg_keys)
            assert(def.contains(ak));               // argv keys exist
    }

    // ── missing required arg → error JSON, no podman involved ────────────
    BrowserTool nav{aid, specs[0]};                 // browser_navigate
    auto r = nav.execute(json::object(), ctx);
    assert(r["status"] == "error");
    assert(r["error"].get<std::string>().find("url") != std::string::npos);

    // ── approval denial short-circuits before the container call ─────────
    BrowserTool nav2{aid, specs[0]};
    nav2.set_approval_fn([](std::string_view) { return false; });
    r = nav2.execute({{"url", "https://example.com"}}, ctx);
    assert(r["status"] == "error" && r["error"] == "denied by user");

    // ── approval approval summary mentions the tool + first arg ──────────
    std::string seen;
    BrowserTool nav3{aid, specs[0]};
    nav3.set_approval_fn([&seen](std::string_view s) {
        seen = std::string{s};
        return false;
    });
    (void)nav3.execute({{"url", "https://example.com"}}, ctx);
    assert(seen.find("browser_navigate") != std::string::npos);
    assert(seen.find("example.com") != std::string::npos);

    // ── fill_secret: grant gate ───────────────────────────────────────────
    BrowserFillSecretTool fill{aid, &secrets};
    r = fill.execute({{"selector", "#pw"}}, ctx);   // missing secret_name
    assert(r["status"] == "error");
    r = fill.execute({{"selector", "#pw"}, {"secret_name", "bad name!"}}, ctx);
    assert(r["status"] == "error" &&
           r["error"] == "invalid secret name");
    r = fill.execute({{"selector", "#pw"}, {"secret_name", "GITHUB_TOKEN"}}, ctx);
    assert(r["status"] == "error" &&
           r["error"].get<std::string>().find("not granted") != std::string::npos);

    // granted → proceeds past the gate (fails later in the sandbox: no
    // working podman — but the error must NOT be the grant error)
    assert(secrets.put("GITHUB_TOKEN", "ghp_test_value"));
    assert(secrets.grant("GITHUB_TOKEN", aid));
    r = fill.execute({{"selector", "#pw"}, {"secret_name", "GITHUB_TOKEN"}}, ctx);
    assert(r["status"] == "error");   // sandbox: exec fails
    assert(r["error"].get<std::string>().find("not granted") == std::string::npos);
    // the secret value must never appear in tool output
    assert(r.dump().find("ghp_test_value") == std::string::npos);

    // revoked → gate closes again
    assert(secrets.revoke("GITHUB_TOKEN", aid));
    r = fill.execute({{"selector", "#pw"}, {"secret_name", "GITHUB_TOKEN"}}, ctx);
    assert(r["error"].get<std::string>().find("not granted") != std::string::npos);

    fs::remove_all(tmp);
    std::cout << "test_browser_tools OK\n";
    return 0;
}
