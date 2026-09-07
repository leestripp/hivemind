#ifndef DAEDALUS_COMMON_UTILS_H
#define DAEDALUS_COMMON_UTILS_H

#include <string>
#include <vector>

/**
 * Common utility functions and constants shared across tools.
 */

namespace hive {

// List of safe read-only diagnostic tools that can be auto-approved
inline const std::vector<std::string>& get_safe_diagnostic_prefixes() {
    static const std::vector<std::string> prefixes = {
        "node --check", "node -c",
        "python -m py_compile", "python -m pyflakes", "python -m mypy",
        "python -m pylint", "python -m flake8", "python -m ruff",
        "python -m black --check", "python -m isort --check-only",
        "python -m pytest --collect-only", "python -m pytest --dry-run",
        "rustc --check", "cargo check", "cargo clippy",
        "cargo test --no-run", "cargo build --dry-run",
        "g++ -fsyntax-only", "gcc -fsyntax-only",
        "clang++ -fsyntax-only", "clang -fsyntax-only",
        "cmake -N", "cmake --build . --target help",
        "go build -n", "go vet", "go test -c", "go mod verify",
        "npx tsc --noEmit", "npx eslint --no-eslintrc --quiet",
        "ruby -c", "perl -c", "shellcheck",
    };
    return prefixes;
}

// Escape a string for safe use in shell commands (single-quote wrapping)
inline std::string escape_shell_arg(const std::string& input) {
    std::string result;
    result.reserve(input.size() + 16);
    result.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            result.push_back('\'');
            result.push_back('\\');
            result.push_back('\'');
            result.push_back('\'');
        } else {
            result.push_back(c);
        }
    }
    result.push_back('\'');
    return result;
}

// Extract command prefix for read-only syntax check commands
inline std::string extract_command_prefix(const std::string& command) {
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;
    std::string prefix = cmd;
    std::string arg;
    while (iss >> arg) {
        if (arg.empty()) continue;
        if (!arg.empty() && arg[0] != '-' && arg[0] != '+') {
            break;
        }
        prefix += " " + arg;
    }
    return prefix;
}

// Check if command already has stderr redirect (2>, 2>>, 2>&1, &>, etc.)
inline bool has_stderr_redirect(const std::string& command) {
    if (command.find("2>") != std::string::npos ||
        command.find("2>>") != std::string::npos ||
        command.find("2>&1") != std::string::npos ||
        command.find("&>") != std::string::npos ||
        command.find("&>>") != std::string::npos) {
        return true;
    }
    return false;
}

// Escape regex metacharacters for safe grep usage
inline std::string escape_grep_regex(const std::string& input) {
    // Characters that have special meaning in BRE/ERE regex
    static const char* meta_chars = ".^$*+?{}[]()|\\";
    
    std::string result;
    result.reserve(input.size() * 2);  // Worst case: all escaped
    
    for (char c : input) {
        if (c == '\n' || c == '\r' || c == '\0') {
            // Skip control characters that could be dangerous
            continue;
        }
        if (strchr(meta_chars, c) != nullptr) {
            result.push_back('\\');
        }
        result.push_back(c);
    }
    
    return result;
}


// Try to recover a truncated JSON object by completing unclosed braces
// Returns the original string if recovery fails or isn't possible
inline std::string try_recover_json_string(const std::string& json_str) {
    // Quick check: if it parses normally, return as-is
    try {
        auto __attribute__((unused)) _unused = json::parse(json_str);
        return json_str;  // Already valid
    } catch (...) {
        // Not valid JSON, try to recover
    }
    
    // Check if it looks like a truncated object (ends with , or : or { without closing })
    if (json_str.empty()) return json_str;
    
    char last_char = json_str.back();
    if (last_char != ',' && last_char != ':' && last_char != '{') {
        // Doesn't look like a truncation pattern, might be something else
        return json_str;
    }
    
    // Try to complete the object by counting braces
    int brace_count = 0;
    bool in_string = false;
    bool escaped = false;
    
    for (char c : json_str) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"' && !escaped) {
            in_string = !in_string;
            continue;
        }
        if (!in_string) {
            if (c == '{') brace_count++;
            else if (c == '}') brace_count--;
        }
    }
    
    // If we have unclosed braces, try to complete
    if (brace_count > 0) {
        std::string recovered = json_str;
        // Remove trailing commas or spaces before closing
        while (!recovered.empty() && (recovered.back() == ',' || recovered.back() == ' ')) {
            recovered.pop_back();
        }
        // Add closing braces
        for (int i = 0; i < brace_count; i++) {
            recovered += "}";
        }
        
        // Verify the fix worked
        try {
            json parsed_test = json::parse(recovered); (void)parsed_test;
            return recovered;
        } catch (...) {
            // Recovery didn't work, return original
            return json_str;
        }
    }
    
    return json_str;
}

} // namespace hive

#endif // DAEDALUS_COMMON_UTILS_H
