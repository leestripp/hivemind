#pragma once

#include <string>
#include <vector>
#include <utility>

/**
 * BashChainer - Command parsing for bash tool
 *
 * Lightweight functions for detecting and parsing command chaining operators.
 * Only what BashTool actually needs - no approval logic.
 */
class BashChainer {
public:
    /**
     * Check if command contains chaining operators (&&, ||)
     * Note: | (pipe) and ; (semicolon) are shell syntax, NOT command chaining.
     */
    static bool contains_command_chaining(const std::string& command);
    
    /**
     * Check if a command contains a standalone background operator (&)
     * that is NOT part of redirects like 2>&1, &>, &>>.
     */
    static bool contains_background_operator(const std::string& command);
    
    /**
     * Parse chained commands into (operator, command) pairs
     * e.g., "cmd1 && cmd2 || cmd3" -> [("&&", "cmd1"), ("||", "cmd2"), ("", "cmd3")]
     */
    static std::vector<std::pair<std::string, std::string>>
        parse_chained_commands(const std::string& command);
};
