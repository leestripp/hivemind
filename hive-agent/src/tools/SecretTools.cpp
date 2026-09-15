#include "tools/SecretTools.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

extern char** environ;  // POSIX process environment

namespace hive {

namespace {

constexpr std::string_view kPrefix = "HIVE_SECRET_";

/// One granted secret, reconstructed from the HIVE_SECRET_* environment.
/// Passwords are never copied out of the environment here — list output
/// records presence only.
struct SecretEntry {
    bool        has_value = false;  // bare HIVE_SECRET_<NAME>
    bool        has_user  = false;
    bool        has_pass  = false;
    bool        has_url   = false;
    std::string username;           // login metadata, safe to show
    std::string url;                // login metadata, safe to show
};

/// Snapshots the granted secrets from the process environment.
/// Sorted by name (std::map) so list output is stable.
std::map<std::string, SecretEntry> scan_granted_secrets() {
    std::map<std::string, SecretEntry> out;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string_view line(*e);
        if (line.substr(0, kPrefix.size()) != kPrefix) continue;
        const auto eq = line.find('=', kPrefix.size());
        const std::string_view var =
            line.substr(0, eq == std::string_view::npos ? line.size() : eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{}
                                         : line.substr(eq + 1);
        std::string rest(var.substr(kPrefix.size()));
        if (rest.empty()) continue;

        auto& entry = out[rest];  // may be re-keyed below on suffix match
        const auto strip = [&](std::string_view suffix) {
            return rest.size() > suffix.size() &&
                   rest.compare(rest.size() - suffix.size(),
                                suffix.size(), suffix) == 0;
        };
        if (strip("_USER")) {
            out.erase(rest);
            rest.resize(rest.size() - 5);
            auto& le = out[rest];
            le.has_user = true;
            le.username = value;
        } else if (strip("_PASS")) {
            out.erase(rest);
            rest.resize(rest.size() - 5);
            out[rest].has_pass = true;  // presence only — value not copied
        } else if (strip("_URL")) {
            out.erase(rest);
            rest.resize(rest.size() - 4);
            auto& le = out[rest];
            le.has_url = true;
            le.url = value;
        } else {
            entry.has_value = true;
        }
    }
    return out;
}

/// Maps a secrets_get field argument to its env-var suffix.
/// Returns false for an unknown field.
bool field_suffix(const std::string& field, std::string& suffix) {
    if (field.empty() || field == "value")    { suffix.clear();   return true; }
    if (field == "username") { suffix = "_USER"; return true; }
    if (field == "password") { suffix = "_PASS"; return true; }
    if (field == "url")      { suffix = "_URL";  return true; }
    return false;
}

}  // namespace

bool secret_env_name_valid(const std::string& name) {
    if (name.empty() || name.size() > 64 || std::isdigit(name.front()))
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isupper(c) || std::isdigit(c) || c == '_';
    });
}

std::string secret_env_normalize(const std::string& name) {
    std::string upper = name;
    for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
    return upper;
}

// ── secrets_list ──────────────────────────────────────────────────────

json SecretsListTool::get_definition() const {
    // Named json variables, NOT one deep nested literal — a brace
    // mis-count silently flips an object into an ARRAY and strict
    // providers 400 every turn (Phase 60c incident).
    const json params = {{"type", "object"},
                         {"properties", json::object()}};
    const json fn = {{"name", get_name()},
                     {"description", get_description()},
                     {"parameters", params}};
    return {{"type", "function"}, {"function", fn}};
}

json SecretsListTool::execute(const json& /*args*/, HostToolContext& /*ctx*/) {
    const auto granted = scan_granted_secrets();
    json list = json::array();
    for (const auto& [name, e] : granted) {
        const bool login = e.has_user || e.has_pass || e.has_url;
        json fields = json::array();
        if (login) {
            if (e.has_user) fields.push_back("username");
            if (e.has_pass) fields.push_back("password");
            if (e.has_url)  fields.push_back("url");
        } else if (e.has_value) {
            fields.push_back("value");
        }
        json item = {{"name", name},
                     {"kind", login ? "login" : "value"},
                     {"fields", fields}};
        if (login) {
            // Metadata only — the password NEVER appears in list output.
            if (e.has_user) item["username"] = e.username;
            if (e.has_url)  item["url"] = e.url;
        }
        list.push_back(std::move(item));
    }
    return {{"status", "ok"},
            {"secrets", list},
            {"count", list.size()}};
}

// ── secrets_get ───────────────────────────────────────────────────────

json SecretsGetTool::get_definition() const {
    const json name = {
        {"type", "string"},
        {"description",
         "Secret name as shown by secrets_list (case-insensitive, "
         "e.g. GITHUB_TOKEN)."}};
    const json field = {
        {"type", "string"},
        {"enum", json::array({"value", "username", "password", "url"})},
        {"description",
         "Which field to read. Omit or use 'value' for api_key/note "
         "secrets; login secrets require username, password, or url."}};
    const json params = {
        {"type", "object"},
        {"properties", {{"name", name}, {"field", field}}},
        {"required", json::array({"name"})}};
    const json fn = {{"name", get_name()},
                     {"description", get_description()},
                     {"parameters", params}};
    return {{"type", "function"}, {"function", fn}};
}

json SecretsGetTool::execute(const json& args, HostToolContext& /*ctx*/) {
    const std::string raw_name = args.value("name", "");
    const std::string field = args.value("field", "");
    if (raw_name.empty())
        return {{"status", "error"}, {"error", "name is required"}};
    const std::string name = secret_env_normalize(raw_name);
    if (!secret_env_name_valid(name))
        return {{"status", "error"}, {"error", "invalid secret name"}};

    std::string suffix;
    if (!field_suffix(field, suffix))
        return {{"status", "error"},
                {"error", "field must be value | username | password | url"}};

    // Grant check: the hub injects HIVE_SECRET_<NAME>[_SUFFIX] at spawn
    // for granted secrets only — absence means "not granted to you".
    const std::string env_name = std::string(kPrefix) + name + suffix;
    if (const char* v = std::getenv(env_name.c_str()))
        return {{"status", "ok"},
                {"name", name},
                {"field", field.empty() ? "value" : field},
                {"value", v}};

    // A bare-value miss on a LOGIN secret: steer the caller to a field
    // instead of a bare "not granted" (which would be wrong — it IS
    // granted, just fielded).
    if (suffix.empty()) {
        json avail = json::array();
        if (std::getenv((std::string(kPrefix) + name + "_USER").c_str()))
            avail.push_back("username");
        if (std::getenv((std::string(kPrefix) + name + "_PASS").c_str()))
            avail.push_back("password");
        if (std::getenv((std::string(kPrefix) + name + "_URL").c_str()))
            avail.push_back("url");
        if (!avail.empty())
            return {{"status", "error"},
                    {"error", "secret '" + name +
                                  "' is a login secret — call again with "
                                  "a 'field' argument"},
                    {"available_fields", avail}};
    }
    return {{"status", "error"},
            {"error", "secret '" + name +
                          (suffix.empty() ? "" : "' field '" + field) +
                          "' is not granted to you"}};
}

}  // namespace hive
