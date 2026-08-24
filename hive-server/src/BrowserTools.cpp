// include/hivemind/BrowserTools.cpp — Phase 6 computer-use agent tools.
#include "hivemind/BrowserTools.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/SecretStore.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json prop(const std::string& desc) {
    return json{{"type", "string"}, {"description", desc}};
}

/// Copy the driver into the agent's home mount (visible in the container
/// at /home/hive/browser_driver.py). Cheap; refresh on every call so image
/// rebuilds never run a stale driver.
void install_driver(const std::string& agent_id) {
    const fs::path src = "hive-server/data/browser_driver.py";
    const fs::path dst =
        ContainerManager::agent_home_dir(agent_id) / "browser_driver.py";
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("cannot install browser driver: " +
                                 ec.message());
}

/// Run one driver action; returns the driver's JSON (or an error object).
json run_driver(const std::string& agent_id, const std::string& cmdline,
                int timeout_s) {
    try { install_driver(agent_id); }
    catch (const std::exception& e) {
        return {{"status", "error"}, {"error", e.what()}};
    }
    auto r = container_manager().exec(agent_id, cmdline, timeout_s);
    if (r.exit_code == -1)   // podman itself unavailable
        return {{"status", "error"}, {"error", r.output}};
    // The driver prints one JSON line; exec merges stderr, so parse the
    // LAST line that looks like JSON (nohup/pgrep noise may precede it).
    std::string last_json;
    size_t pos = 0;
    while (pos < r.output.size()) {
        auto nl = r.output.find('\n', pos);
        std::string line = r.output.substr(
            pos, nl == std::string::npos ? nl : nl - pos);
        if (!line.empty() && line.front() == '{') last_json = line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (!last_json.empty()) {
        auto j = json::parse(last_json, nullptr, false);
        if (!j.is_discarded()) return j;
    }
    return {{"status", "error"},
            {"error", "driver produced no JSON"},
            {"output", r.output.substr(0, 500)},
            {"exit_code", r.exit_code}};
}

std::string q(const std::string& s) { return ContainerManager::shell_quote(s); }

} // namespace

std::vector<BrowserTool::Spec> BrowserTool::specs() {
    return {
        {"browser_navigate",
         "Navigate the browser to a URL. Returns the final URL and page "
         "title.",
         "goto", {{"url", prop("The URL to open.")}}, {"url"}, {"url"}},
        {"browser_click",
         "Click an element by CSS selector.",
         "click", {{"selector", prop("CSS selector.")}}, {"selector"},
         {"selector"}},
        {"browser_type",
         "Clear a field and type text into it (CSS selector).",
         "type", {{"selector", prop("CSS selector.")},
                  {"text", prop("Text to type.")}},
         {"selector", "text"}, {"selector", "text"}},
        {"browser_press",
         "Press a keyboard key (e.g. Enter, Tab, Escape).",
         "press", {{"key", prop("Key name.")}}, {"key"}, {"key"}},
        {"browser_read",
         "Read the page's visible text (truncated to ~6000 chars). Call "
         "after navigating to see what's on the page.",
         "text", json::object(), {}, {}},
        {"browser_screenshot",
         "Screenshot the current viewport into the workspace.",
         "screenshot",
         {{"path", prop("Output path (default /workspace/screenshot.png).")}},
         {}, {"path"}},
        {"browser_eval",
         "Evaluate JavaScript on the page and return the result (JSON).",
         "eval", {{"script", prop("JavaScript expression.")}}, {"script"},
         {"script"}},
        {"browser_tabs",
         "List open browser tabs (url + title).",
         "tabs", json::object(), {}, {}},
        {"browser_back",
         "Navigate back in history.",
         "back", json::object(), {}, {}},
        {"browser_reload",
         "Reload the current page.",
         "reload", json::object(), {}, {}},
    };
}

json BrowserTool::execute(const json& args, daedalus::HostToolContext&) {
    std::string cmdline = "python3 /home/hive/browser_driver.py " + spec_.action;
    for (const auto& key : spec_.arg_keys) {
        std::string v = args.value(key, "");
        if (v.empty() && !args.contains(key)) {
            if (std::find(spec_.required.begin(), spec_.required.end(), key)
                != spec_.required.end())
                return {{"status", "error"},
                        {"error", "missing required argument: " + key}};
            continue;   // optional arg not provided
        }
        cmdline += " " + q(v);
    }
    if (approval_fn_) {
        std::string summary = spec_.name + " " +
            (spec_.arg_keys.empty() ? "" : args.value(spec_.arg_keys[0], ""));
        if (!approval_fn_(summary))
            return {{"status", "error"}, {"error", "denied by user"}};
    }
    const int timeout = args.value("timeout", 90);
    return run_driver(agent_id_, cmdline, timeout);
}

json BrowserFillSecretTool::execute(const json& args,
                                    daedalus::HostToolContext&) {
    const std::string selector = args.value("selector", "");
    const std::string name = args.value("secret_name", "");
    if (selector.empty() || name.empty())
        return {{"status", "error"},
                {"error", "selector and secret_name are required"}};
    if (!hivemind::SecretStore::valid_name(name))
        return {{"status", "error"}, {"error", "invalid secret name"}};

    // Grant check on the HOST, before any container call.
    auto& secrets = store_ ? *store_ : ContainerManager::secret_store();
    auto grants = secrets.grants_for_agent(agent_id_);
    if (std::find(grants.begin(), grants.end(), name) == grants.end())
        return {{"status", "error"},
                {"error", "secret '" + name + "' is not granted to you"}};

    if (approval_fn_ &&
        !approval_fn_("browser_fill_secret " + selector + " ←" + name))
        return {{"status", "error"}, {"error", "denied by user"}};

    // Value rides in via exec's HIVE_SECRET_<NAME> env injection — never
    // through this tool's args or output.
    return run_driver(agent_id_,
                      "python3 /home/hive/browser_driver.py secret " +
                          q(selector) + " " + q("HIVE_SECRET_" + name),
                      /*timeout_s=*/60);
}
