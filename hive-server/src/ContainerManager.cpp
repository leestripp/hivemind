// src/ContainerManager.cpp
// -----------------------------------------------------------------------------
// v2 statics-only: workers own podman. See ContainerManager.h.
// -----------------------------------------------------------------------------

#include "hivemind/ContainerManager.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ExecAuditStore.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

std::string ContainerManager::shell_quote(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

hivemind::SecretStore* ContainerManager::secrets() {
    static hivemind::SecretStore store;   // XDG path; key loads on first use
    return &store;
}

ExecAuditStore& ContainerManager::exec_audit() {
    static ExecAuditStore store{
        fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."} /
        ".local" / "share" / "hivemind" / "exec_audit.db"};
    return store;
}
