#pragma once

#include <daedalus.h>

#include <functional>
#include <string>
#include <vector>

namespace hivemind { class SecretStore; }

/**
 * BrowserTool — Phase 6 computer use. One tool per browser action, backed
 * by browser_driver.py inside the agent's container: a persistent Chromium
 * over CDP on the Xvfb display — the same browser the user sees (and can
 * take over) in the web UI's Computer view.
 *
 * Registered for containerized roster agents only. Actions run through
 * ContainerManager::exec; failures return {"status":"error"} instead of
 * throwing so the agent can report them.
 */
class BrowserTool final : public daedalus::BaseTool {
public:
    struct Spec {
        std::string name;                    // browser_navigate
        std::string description;
        std::string action;                  // driver argv[1]
        nlohmann::json properties;           // JSON-schema properties
        std::vector<std::string> required;
        std::vector<std::string> arg_keys;   // ordered → positional argv
    };

    BrowserTool(std::string agent_id, Spec spec)
        : agent_id_(std::move(agent_id)), spec_(std::move(spec)) {}

    /// Optional approval hook (same gate as ContainerBashTool).
    void set_approval_fn(std::function<bool(std::string_view)> fn) {
        approval_fn_ = std::move(fn);
    }

    [[nodiscard]] std::string get_name() const override { return spec_.name; }
    [[nodiscard]] std::string get_description() const override {
        return spec_.description;
    }
    [[nodiscard]] nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", spec_.name},
                {"description", spec_.description},
                {"parameters", {
                    {"type", "object"},
                    {"properties", spec_.properties},
                    {"required", spec_.required},
                }},
            }},
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;

    /// The specs for every browser action tool (sans fill_secret).
    static std::vector<Spec> specs();

private:
    std::string agent_id_;
    Spec spec_;
    std::function<bool(std::string_view)> approval_fn_;
};

/**
 * BrowserFillSecretTool — types a granted SecretStore secret into a web
 * form. The LLM only ever sees the secret NAME: the value rides into the
 * container via ContainerManager::exec's env injection
 * (HIVE_SECRET_<NAME>) and the driver fills it without echoing. The grant
 * is checked on the host BEFORE any container call.
 */
class BrowserFillSecretTool final : public daedalus::BaseTool {
public:
    explicit BrowserFillSecretTool(std::string agent_id,
                                   hivemind::SecretStore* store = nullptr)
        : agent_id_(std::move(agent_id)), store_(store) {}

    void set_approval_fn(std::function<bool(std::string_view)> fn) {
        approval_fn_ = std::move(fn);
    }

    [[nodiscard]] std::string get_name() const override {
        return "browser_fill_secret";
    }
    [[nodiscard]] std::string get_description() const override {
        return "Type a granted secret (by name) into a web form field. "
               "The value never appears to you or in any output.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", get_name()},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"selector", {
                            {"type", "string"},
                            {"description", "CSS selector of the field."},
                        }},
                        {"secret_name", {
                            {"type", "string"},
                            {"description", "Name of a secret granted to you "
                                            "(e.g. GITHUB_TOKEN)."},
                        }},
                    }},
                    {"required", nlohmann::json::array({"selector", "secret_name"})},
                }},
            }},
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;

private:
    std::string agent_id_;
    hivemind::SecretStore* store_ = nullptr;   // null → global (tests override)
    std::function<bool(std::string_view)> approval_fn_;
};
