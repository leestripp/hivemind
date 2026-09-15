#pragma once

#include "types/BaseTool.h"

#include <string>
#include <vector>

/**
 * BrowserTool — computer use. One tool per browser action, backed by
 * browser_driver.py inside the agent's own container: a persistent
 * Chromium over CDP on the Xvfb display — the same browser the user sees
 * (and can take over) in the client's Computer view.
 *
 * Recreation of the original hub-side tools (hive-server/src/BrowserTools.cpp,
 * lost in the Phase C hub-routing refactor `d989ad8` and never carried
 * into the daedalus-merge hive-agent). The agent now runs IN the
 * container, so the driver is invoked directly via Subprocess (no podman
 * exec). Specs preserved verbatim from 9a5f230 so agent prompts and docs
 * stay accurate.
 *
 * The driver only ships in the hive-agent:browser image — on plain
 * :latest containers every action returns a descriptive error.
 */
namespace hive {

using nlohmann::json;

class BrowserTool final : public BaseTool {
public:
    struct Spec {
        std::string name;                    // browser_navigate
        std::string description;
        std::string action;                  // driver argv[1]
        json properties;                     // JSON-schema properties
        std::vector<std::string> required;
        std::vector<std::string> arg_keys;   // ordered → positional argv
    };

    explicit BrowserTool(Spec spec) : spec_(std::move(spec)) {}

    [[nodiscard]] std::string get_name() const override { return spec_.name; }
    [[nodiscard]] std::string get_description() const override {
        return spec_.description;
    }
    [[nodiscard]] json get_definition() const override {
        return {
            {"type", "function"},
            {"function",
             {{"name", spec_.name},
              {"description", spec_.description},
              {"parameters",
               {{"type", "object"},
                {"properties", spec_.properties},
                {"required", spec_.required}}}}},
        };
    }

    json execute(const json& args, HostToolContext& ctx) override;

    /// The specs for every browser action tool (sans fill_secret).
    static std::vector<Spec> specs();

private:
    Spec spec_;
};

/**
 * BrowserFillSecretTool — types a granted secret into a web form. The LLM
 * only ever sees the secret NAME: the value reaches the driver via the
 * inherited HIVE_SECRET_<NAME> environment variable (injected by the hub
 * at container create for granted secrets only) and is filled without
 * echoing. Grant check = env presence.
 */
class BrowserFillSecretTool final : public BaseTool {
public:
    [[nodiscard]] std::string get_name() const override {
        return "browser_fill_secret";
    }
    [[nodiscard]] std::string get_description() const override {
        return "Type a granted secret (by name) into a web form field. "
               "The value never appears to you or in any output.";
    }
    [[nodiscard]] json get_definition() const override {
        // Named json variables, NOT one nested literal: a brace mis-count
        // in a deep nlohmann initializer list silently flips an object
        // into an ARRAY (2026-09-15: secret_name's missing '}' made
        // properties a list; Moonshot 400'd every turn with
        // "properties must be an object" and Buzz went mute).
        const json selector = {{"type", "string"},
                               {"description", "CSS selector of the field."}};
        const json secret_name = {
            {"type", "string"},
            {"description",
             "Name of a secret granted to you (e.g. GITHUB_TOKEN)."}};
        const json field = {{"type", "string"},
                            {"enum", json::array({"username", "password"})},
                            {"description",
                             "Login secrets only: which field to fill. Omit "
                             "for api_key/note secrets (the value fills "
                             "directly)."}};
        const json params = {
            {"type", "object"},
            {"properties",
             {{"selector", selector},
              {"secret_name", secret_name},
              {"field", field}}},
            {"required", json::array({"selector", "secret_name"})}};
        return {{"type", "function"},
                {"function",
                 {{"name", get_name()},
                  {"description", get_description()},
                  {"parameters", params}}}};
    }

    json execute(const json& args, HostToolContext& ctx) override;
};

}  // namespace hive
