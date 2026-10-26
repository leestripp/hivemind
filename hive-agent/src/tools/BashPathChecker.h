#pragma once

#include <string>
#include <vector>

/**
 * BashPathChecker - Simplified path validation for bash commands
 *
 * Shell parsing helpers. Execution permissions belong to the OS/runtime.
 */

class BashPathChecker {
public:
    // ========== Standalone Background Operator ==========

    /**
     * §B3 fix: quote-aware check for a standalone `&` outside any
     * quoted context or heredoc body. Only the bare background
     * operator — the kind that hangs `popen()` forever — is flagged.
     *
     * Recognises and ALLOWS:
     *   - `&&` (logical AND)
     *   - `2>&1`, `2>&2`, `&>` etc. (file-descriptor redirection)
     *   - `echo "hello & world"` (inside double quotes)
     *   - `echo 'hello & world'` (inside single quotes)
     *   - `printf 'a&b\n'` (literal text inside single quotes)
     *   - Heredoc bodies (between `<<EOF` and the closing `EOF` marker
     *     on its own line)
     *
     * Recognises and REJECTS:
     *   - `sleep 60 &` (background, at end)
     *   - `cmd1 & cmd2` (background in the middle)
     *   - `cmd &` (background, trailing whitespace, then EOF)
     *
     * @param command The shell command to check
     * @return true if a backgrounding `&` is present (unquoted, not
     *         part of `&&` or fd-redirection), false otherwise.
     */
    static bool contains_standalone_ampersand(const std::string& command);

    /**
     * Extract path argument from command (e.g., "rm -rf build" -> "build")
     * Returns empty string if no path found
     */
    static std::string extract_path_from_command(const std::string& command);

    /**
     * Extract all paths from a command (arguments that don't start with -)
     *
     * @param command The shell command to parse
     * @return Vector of path arguments
     */
    static std::vector<std::string> extract_paths(const std::string& command);

    /**
     * Extract the first path from a command
     *
     * @param command The shell command to parse
     * @return First path argument, or empty string if none
     */
    static std::string extract_first_path(const std::string& command);
};