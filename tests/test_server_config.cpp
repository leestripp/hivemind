// tests/test_server_config.cpp — ServerConfig workspace_root, no LLM.
#include "hivemind/ServerConfig.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_server_config";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    ServerConfig cfg{tmp / "server.json"};

    // default when unset
    std::string def = cfg.workspace_root();
    assert(def.find(".local/share/hivemind/workspace") != std::string::npos);
    assert(fs::is_directory(def));          // auto-created

    // tilde expansion (pure)
    std::string expanded = ServerConfig::expand_home("~/projects");
    const char* home = std::getenv("HOME");
    assert(expanded == std::string(home) + "/projects");
    assert(ServerConfig::expand_home("/abs/path") == "/abs/path");
    assert(ServerConfig::expand_home("relative") == "relative");

    // set + persist + reload
    cfg.set_workspace_root((tmp / "company").string());
    assert(cfg.workspace_root() == (tmp / "company").string());
    assert(fs::is_directory(tmp / "company"));   // created on set
    {
        ServerConfig cfg2{tmp / "server.json"};
        assert(cfg2.workspace_root() == (tmp / "company").string());
    }

    // tilde path accepted and expanded at set time
    cfg.set_workspace_root("~/hivemind-ws-test");
    assert(cfg.workspace_root() == std::string(home) + "/hivemind-ws-test");
    fs::remove_all(std::string(home) + "/hivemind-ws-test");

    // relative path rejected
    bool threw = false;
    try { cfg.set_workspace_root("relative/path"); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    fs::remove_all(tmp);
    std::cout << "test_server_config: OK\n";
    return 0;
}
