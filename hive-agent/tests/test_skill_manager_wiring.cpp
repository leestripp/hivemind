// ============================================================================
// tests/test_skill_manager_wiring.cpp
//
// §SKILL-ORDER regression test.
//
// Bug: AgentImpl::initialize() constructed the LLMClient (in the
// has_explicit_creds block) BEFORE the per-agent SkillManager was created,
// so the LLMClient constructor always received nullptr and
// LLMClient::skill_manager_ stayed null for the agent's lifetime. That
// null was installed into SkillManager::current() by the SkillManagerGuard
// at the top of every turn, so the discover_skills tool failed with
// "SkillManager not initialized" and the system-prompt skills catalog was
// never emitted.
//
// Fix: AgentImpl attaches the manager via LLMClient::set_skill_manager()
// once it has been created. This test asserts the wiring is in place after
// initialize().
//
// Hermetic — no network, no live LLM calls (dummy creds only force the
// LLMClient to be constructed; no requests are made during initialize()).
// ============================================================================

#include "library/AgentImpl.h"

#include "library/AgentSink.h"

// The default build is Release (-DNDEBUG), which would compile assert()
// out and make this regression test a no-op. Force assertions on.
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

struct NullSink : hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
    void on_assistant_delta(std::string_view) noexcept override {}
    void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
    void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
    void on_state_change(int, int) noexcept override {}
};

fs::path fresh_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("skill-wiring-" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

hive::AgentConfig base_config(const fs::path& root) {
    hive::AgentConfig cfg;
    cfg.project_root  = root / "project";
    cfg.agent_home = root / "home";
    fs::create_directories(cfg.project_root);
    fs::create_directories(cfg.agent_home);
    // Dummy explicit creds: forces LLMClient construction without any
    // network access (initialize() never issues a request).
    cfg.api_key  = "dummy";
    cfg.api_host = "http://127.0.0.1:1";
    return cfg;
}

// With skills configured, the LLMClient must have the SkillManager
// attached after initialize().
void test_skill_manager_attached_to_client() {
    auto root = fresh_dir("attached");

    // One minimal skill on disk so discovery has something to find.
    fs::path skill_dir = root / "skills" / "alpha";
    fs::create_directories(skill_dir);
    std::ofstream f(skill_dir / "SKILL.md");
    f << "---\nname: alpha\ndescription: alpha skill\n---\n\nbody\n";
    f.close();

    auto cfg = base_config(root);
    cfg.skills_dirs = { root / "skills" };

    hive::detail::AgentImpl impl(std::move(cfg),
                                     std::make_shared<NullSink>());
    impl.initialize();

    assert(impl.client_has_skill_manager());

    fs::remove_all(root);
    std::cout << "[ok] test_skill_manager_attached_to_client" << std::endl;
}

// Without any skills configuration no SkillManager is created, so the
// accessor must report false (sanity check that the accessor reflects
// the real wiring rather than always returning true).
void test_no_skills_no_manager() {
    auto root = fresh_dir("none");

    hive::detail::AgentImpl impl(base_config(root),
                                     std::make_shared<NullSink>());
    impl.initialize();

    assert(!impl.client_has_skill_manager());

    fs::remove_all(root);
    std::cout << "[ok] test_no_skills_no_manager" << std::endl;
}

}  // namespace

int main() {
    test_skill_manager_attached_to_client();
    test_no_skills_no_manager();
    std::cout << "all skill-manager wiring tests passed" << std::endl;
    return 0;
}
