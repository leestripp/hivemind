// src/Vault.cpp — Phase 22 external vault adapter (pass / 1Password CLI).
#include "hivemind/Vault.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace hm::vault {
namespace {

bool valid_ref_chars(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                        c == '/' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string shell_single_quote(const std::string& s) {
    return "'" + s + "'";   // charset allowlist makes this airtight
}

// Run cmd, capture stdout (first max_bytes), "" on failure. No logging —
// the output IS a secret.
std::string run_capture(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string out;
    char buf[1024];
    while (fgets(buf, sizeof buf, pipe)) {
        out += buf;
        if (out.size() > 8192) break;
    }
    const int rc = pclose(pipe);
    if (rc != 0) return "";
    // trim trailing whitespace/newlines
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out;
}

const char* env_or(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return (v && *v) ? v : fallback;
}

} // namespace

std::string validate_source(const std::string& source) {
    if (source.rfind("pass:", 0) == 0) {
        const std::string ref = source.substr(5);
        if (!valid_ref_chars(ref) || ref.find("..") != std::string::npos)
            return "pass: path must be [A-Za-z0-9_./-] without '..'" ;
        return "";
    }
    if (source.rfind("op://", 0) == 0) {
        const std::string ref = source.substr(5);
        if (!valid_ref_chars(ref) || ref.find("..") != std::string::npos)
            return "op:// reference must be [A-Za-z0-9_./-] without '..'";
        return "";
    }
    return "unknown scheme — expected pass:<path> or op://<vault>/<item>/<field>";
}

std::optional<std::string> fetch(const std::string& source) {
    if (validate_source(source) != "") return std::nullopt;
    std::string cmd;
    bool first_line_only = false;
    if (source.rfind("pass:", 0) == 0) {
        cmd = std::string("timeout 15 ") + env_or("HIVEMIND_PASS_BIN", "pass") +
              " show -- " + shell_single_quote(source.substr(5)) + " 2>/dev/null";
        first_line_only = true;   // pass convention: line 1 is the secret
    } else {
        cmd = std::string("timeout 15 ") + env_or("HIVEMIND_OP_BIN", "op") +
              " read " + shell_single_quote(source) + " 2>/dev/null";
    }
    std::string out = run_capture(cmd);
    if (out.empty()) return std::nullopt;
    if (first_line_only) {
        const auto nl = out.find('\n');
        if (nl != std::string::npos) out = out.substr(0, nl);
        if (out.empty()) return std::nullopt;
    }
    return out;
}

} // namespace hm::vault
