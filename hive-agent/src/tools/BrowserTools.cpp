#include "tools/BrowserTools.h"

#include "core/Subprocess.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace hive {
namespace {

// /home/hive is a per-agent bind mount at runtime — image files there
// are MASKED. The driver lives with the other runtime scripts.
constexpr const char* kDriver = "/usr/local/bin/browser_driver.py";

json prop(const char* desc) {
    return {{"type", "string"}, {"description", desc}};
}

/// POSIX single-quote shell escaping: wrap in '...', escape embedded
/// quotes as '\'' (end quote, escaped quote, reopen).
std::string q(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

/// Secret names match the hub's SecretStore convention: uppercase
/// identifier (letters, digits, underscore, not starting with a digit).
bool valid_secret_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || std::isdigit(name.front()))
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isupper(c) || std::isdigit(c) || c == '_';
    });
}

/// Run the in-container driver and return its one-line JSON result.
/// Never throws: failures become {"status":"error"} so the agent can
/// report and recover.
json run_driver(const std::string& cmdline, int timeout_s) {
    if (!std::filesystem::exists(kDriver))
        return {{"status", "error"},
                {"error",
                 "browser driver not installed in this container — browser "
                 "tools require the hive-agent:browser image variant"}};

    timeout_s = std::clamp(timeout_s, 5, 180);
    auto res = Subprocess::execute_shell(cmdline, "/workspace",
                                         timeout_s * 1000);
    if (res.timed_out)
        return {{"status", "error"},
                {"error", "browser action timed out after " +
                              std::to_string(timeout_s) + "s"}};

    // The driver prints exactly one JSON object on stdout.
    std::string out = res.stdout;
    const auto first = out.find('{');
    const auto last = out.rfind('}');
    if (first != std::string::npos && last != std::string::npos &&
        last > first) {
        try {
            return json::parse(out.substr(first, last - first + 1));
        } catch (const json::exception&) {
            // fall through to the generic error below
        }
    }
    std::string detail = res.stderr.empty() ? out : res.stderr;
    if (detail.size() > 300) detail.resize(300);
    if (detail.empty())
        detail = "exit code " + std::to_string(res.exit_code);
    return {{"status", "error"},
            {"error", "browser driver failed: " + detail}};
}

}  // namespace

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
         "type",
         {{"selector", prop("CSS selector.")},
          {"text", prop("Text to type.")}},
         {"selector", "text"}, {"selector", "text"}},
        {"browser_press",
         "Press a keyboard key (e.g. Enter, Tab, Escape).",
         "press", {{"key", prop("Key name.")}}, {"key"}, {"key"}},
        {"browser_type_keys",
         "Type text one character at a time as REAL key presses "
         "(keydown/keyup per character) into a field — for sites whose "
         "composers only count physical key presses (e.g. X.com reply "
         "box, Facebook profile composer), where browser_type leaves "
         "the site blind to the text. Focuses the target first, does "
         "NOT clear it by default (browser_type already clears). "
         "Returns chars_sent and the target's resulting text length so "
         "you can detect a silent failure without a follow-up eval.",
         "typekeys",
         {{"selector",
           prop("CSS selector of the target field. Omit to type into "
                "whatever element currently has focus.")},
          {"text", prop("Text to type, one key press per character.")},
          {"delay_ms",
           prop("Milliseconds between key presses (default 75, min 20). "
                "Don't go below ~25 on X.com — it rate-limits.")},
          {"clear",
           prop("true to select-all + delete the existing content "
                "before typing (default false).")},
          {"timeout",
           prop("Overall timeout in seconds (default 150). Raise it "
                "for very long texts: chars x delay_ms / 1000 + ~15s.")}},
         {"text"},
         {"selector", "text", "delay_ms", "clear"},
         /*default_timeout=*/150},
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

json BrowserTool::execute(const json& args, HostToolContext& /*ctx*/) {
    std::string cmdline = std::string("python3 ") + kDriver + " " +
                          spec_.action;
    for (size_t i = 0; i < spec_.arg_keys.size(); ++i) {
        const auto& key = spec_.arg_keys[i];
        if (!args.contains(key) || args[key].is_null()) {
            if (std::find(spec_.required.begin(), spec_.required.end(),
                          key) != spec_.required.end())
                return {{"status", "error"},
                        {"error", "missing required argument: " + key}};
            // Skip only when no LATER positional arg is provided —
            // dropping a middle arg would shift the remaining argv out
            // of position (typekeys' optional selector sits first).
            const bool later_provided =
                std::any_of(spec_.arg_keys.begin() +
                                static_cast<std::ptrdiff_t>(i + 1),
                            spec_.arg_keys.end(),
                            [&args](const std::string& k) {
                                return args.contains(k) &&
                                       !args[k].is_null();
                            });
            if (!later_provided) continue;  // trailing optional, safe to drop
            cmdline += " ''";               // placeholder keeps positions
            continue;
        }
        const json& v = args[key];
        cmdline += " " + q(v.is_string() ? v.get<std::string>() : v.dump());
    }
    const int timeout = args.value("timeout", spec_.default_timeout);
    return run_driver(cmdline, timeout);
}

json BrowserFillSecretTool::execute(const json& args, HostToolContext& /*ctx*/) {
    const std::string selector = args.value("selector", "");
    const std::string name = args.value("secret_name", "");
    if (selector.empty() || name.empty())
        return {{"status", "error"},
                {"error", "selector and secret_name are required"}};
    if (!valid_secret_name(name))
        return {{"status", "error"}, {"error", "invalid secret name"}};

    // Grant check: the hub injects HIVE_SECRET_<NAME> at spawn for
    // granted secrets only — absence means "not granted to you".
    // Login secrets arrive as _USER/_PASS/_URL suffixes (Phase 60); the
    // optional `field` arg picks which, defaulting to the bare value for
    // api_key/note secrets.
    const std::string field = args.value("field", "");
    std::string env_name = "HIVE_SECRET_" + name;
    if (field == "username")      env_name += "_USER";
    else if (field == "password") env_name += "_PASS";
    if (std::getenv(env_name.c_str()) == nullptr)
        return {{"status", "error"},
                {"error", "secret '" + name +
                              (field.empty() ? "" : "' field '" + field) +
                              "' is not granted to you"}};

    // The value rides into the driver via the inherited environment —
    // never through this tool's args or output.
    return run_driver(std::string("python3 ") + kDriver + " secret " +
                          q(selector) + " " + q(env_name),
                      /*timeout_s=*/60);
}

}  // namespace hive
