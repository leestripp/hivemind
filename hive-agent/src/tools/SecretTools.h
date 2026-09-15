#pragma once

#include "types/BaseTool.h"

#include <string>

/**
 * @file src/tools/SecretTools.h
 * @brief Agent-facing tools for the hub's secret broker (Phase 61).
 *
 * The hub brokers secrets from the hive-server DB: granted secrets are
 * rendered by SecretStore::env_for_agent() and ride the spawn frame to
 * the worker, which writes them into the agent container's 0600 env
 * file as HIVE_SECRET_<NAME>[ _USER | _PASS | _URL ] (Phase 60). Until
 * now the only consumer was browser_fill_secret — the agent had no way
 * to discover or read its own grants.
 *
 * These two tools close that loop:
 *   - secrets_list — which secrets are granted to me (names, kind,
 *     login metadata). NEVER prints values.
 *   - secrets_get  — read the value of one granted secret (or one field
 *     of a login secret).
 *
 * Grant check = environment presence: a name absent from the
 * environment is "not granted to you", exactly like BrowserFillSecretTool.
 * Names are uppercased by the hub renderer; both tools uppercase the
 * input the same way so case doesn't matter to the caller.
 */
namespace hive {

using nlohmann::json;

/// True iff `name` matches the hub's env-rendered secret-name
/// convention: [A-Z0-9_], ≤64 chars, not starting with a digit.
bool secret_env_name_valid(const std::string& name);

/// Uppercase-copy helper shared by both tools (hub renders names in
/// uppercase; the LLM may type any case).
std::string secret_env_normalize(const std::string& name);

class SecretsListTool final : public BaseTool {
public:
    [[nodiscard]] std::string get_name() const override {
        return "secrets_list";
    }
    [[nodiscard]] std::string get_description() const override {
        return "List the secrets the hive broker has granted to you "
               "(names, kind, and login metadata only — values are never "
               "shown here). Use secrets_get to read a value, or "
               "browser_fill_secret to type one into a web form without "
               "seeing it.";
    }
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, HostToolContext& ctx) override;
};

class SecretsGetTool final : public BaseTool {
public:
    [[nodiscard]] std::string get_name() const override {
        return "secrets_get";
    }
    [[nodiscard]] std::string get_description() const override {
        return "Read the value of a secret granted to you by the hive "
               "broker (see secrets_list). For login secrets pick a "
               "field: username, password, or url. The value is returned "
               "to you directly — prefer browser_fill_secret when you "
               "only need to type it into a web form.";
    }
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, HostToolContext& ctx) override;
};

}  // namespace hive
