/**
 * @file test_bash_tool_helpers.cpp
 * @brief Unit tests for BashTool helper functions (standalone, no external deps)
 * 
 * This file contains only the pure static helper functions duplicated from BashTool
 * for independent testing. This avoids the complexity of linking all BashTool dependencies.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

// ============================================================================
// STATIC HELPER FUNCTIONS (legacy copies of BashTool helpers)
// These functions are kept for independent unit testing of the helper logic.
// BashTool itself now delegates to BashPathChecker and BackgroundProcessManager.
// ============================================================================

// Check if a command is considered dangerous
static bool is_dangerous_command(const std::string& command) {
    size_t start = command.find_first_not_of(" \t\n");
    if (start == std::string::npos) {
        return false;
    }

    std::string trimmed = command.substr(start);
    
    std::istringstream iss(trimmed);
    std::string cmd;
    iss >> cmd;

    const std::vector<std::string> dangerous_commands = {
        "rm", "del", "rmdir", "shred", "mkfs", "dd", "fdformat",
        "mkboot", "parted", "fdisk", "sfdisk", "cfdisk"
    };

    for (const auto& dangerous : dangerous_commands) {
        if (cmd == dangerous) {
            return true;
        }
    }

    if (cmd == "rm" || cmd == "del") {
        std::string flags;
        while (iss >> flags) {
            if (flags.find('-') == 0) {
                std::string flag = flags.substr(1);
                if (flag.find('r') != std::string::npos || 
                    flag.find('f') != std::string::npos) {
                    return true;
                }
            }
        }
    }

    return false;
}

// Extract path argument from command
static std::string extract_path_from_command(const std::string& command) {
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;
    
    std::string arg;
    while (iss >> arg) {
        if (arg.empty()) continue;
        if (arg[0] == '-') continue;
        return arg;
    }
    return "";
}

// Check if a path is inside the project directory
static bool is_path_in_project(const std::string& path, const std::string& project_path) {
    if (path.empty()) return false;
    
    if (path[0] != '/') {
        return true;  // Relative paths are within project context
    }
    
    std::string normalized_project = project_path;
    if (normalized_project.back() != '/') {
        normalized_project += '/';
    }
    
    if (path.compare(0, normalized_project.size(), normalized_project) == 0) {
        return true;
    }
    
    return false;
}

// Check if a dangerous command needs approval
static bool needs_approval_for_dangerous(const std::string& command, const std::string& project_path) {
    std::string target_path = extract_path_from_command(command);
    
    if (target_path.empty()) {
        return true;
    }
    
    if (is_path_in_project(target_path, project_path)) {
        return false;
    }
    
    return true;
}

// Check if command contains chaining operators (&&, ||, or background &)
static bool contains_command_chaining(const std::string& command) {
    // Only && and || are command chaining operators
    // Note: | (pipe) and ; (semicolon) are shell syntax, NOT command chaining
    if (command.find("&&") != std::string::npos || command.find("||") != std::string::npos) {
        return true;
    }

    // Check for standalone & (background operator) but NOT & in 2>&1 or similar
    size_t amp_pos = command.find('&');
    if (amp_pos != std::string::npos) {
        size_t next_pos = amp_pos + 1;
        
        // If & is at end of string (or followed only by whitespace to end), it's background &
        if (next_pos >= command.length()) {
            return true;
        }
        
        // If followed by digit or >, it's a redirect (like 2>&1), not background &
        char next_char = command[next_pos];
        if (next_char == '1' || next_char == '2' || next_char == '>') {
            return false;
        }
        
        // If followed by whitespace, it's background &
        if (next_char == ' ' || next_char == '\t' || next_char == '\n') {
            return true;
        }
    }
    return false;
}

// Parse chained commands
static std::vector<std::pair<std::string, std::string>> parse_chained_commands(const std::string& command) {
    std::vector<std::pair<std::string, std::string>> result;
    
    if (command.empty()) {
        return result;
    }
    
    std::string current_cmd;
    std::string current_operator;
    
    size_t i = 0;
    while (i < command.length()) {
        size_t operator_pos = command.length();
        std::string found_op;
        
        size_t and_pos = command.find("&&", i);
        size_t or_pos = command.find("||", i);
        
        if (and_pos != std::string::npos && (or_pos == std::string::npos || and_pos < or_pos)) {
            operator_pos = and_pos;
            found_op = "&&";
        } else if (or_pos != std::string::npos) {
            operator_pos = or_pos;
            found_op = "||";
        }
        
        // NOTE: We only split by && and ||.
        // Single | (pipe) and ; (semicolon) are shell syntax, NOT command chaining.
        // They should be passed to the shell as-is.
        
        if (operator_pos < command.length()) {
            std::string cmd = command.substr(i, operator_pos - i);
            cmd.erase(0, cmd.find_first_not_of(" \t"));
            cmd.erase(cmd.find_last_not_of(" \t") + 1);
            result.push_back({found_op, cmd});
            i = operator_pos + found_op.length();
        } else {
            std::string cmd = command.substr(i);
            cmd.erase(0, cmd.find_first_not_of(" \t"));
            cmd.erase(cmd.find_last_not_of(" \t") + 1);
            result.push_back({"", cmd});
            break;
        }
    }
    
    return result;
}

// Convert command to approval pattern
static std::string command_to_pattern(const std::string& command) {
    std::istringstream iss(command);
    std::string cmd;
    std::vector<std::string> args;
    iss >> cmd;
    std::string arg;
    while (iss >> arg) {
        args.push_back(arg);
    }

    if (args.empty()) {
        return command;
    }

    std::ostringstream pattern;
    pattern << cmd;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].find('-') == 0 && args[i].find('.') == std::string::npos) {
            pattern << " " << args[i];
        } else {
            pattern << " *";
        }
    }
    return pattern.str();
}

// ============================================================================
// TEST SUITES
// ============================================================================

TEST_CASE("Dangerous Command Detection") {
    SUBCASE("Dangerous commands detected") {
        CHECK(is_dangerous_command("rm file.txt") == true);
        CHECK(is_dangerous_command("rm -rf /tmp/test") == true);
        CHECK(is_dangerous_command("rmdir build") == true);
        CHECK(is_dangerous_command("dd if=/dev/zero of=test.img") == true);
        CHECK(is_dangerous_command("del file.txt") == true);
    }
    
    SUBCASE("Safe commands not flagged as dangerous") {
        CHECK(is_dangerous_command("ls") == false);
        CHECK(is_dangerous_command("ls -la") == false);
        CHECK(is_dangerous_command("cmake -S . -B build") == false);
        CHECK(is_dangerous_command("make -j8") == false);
        CHECK(is_dangerous_command("nproc") == false);
        CHECK(is_dangerous_command("pwd") == false);
        CHECK(is_dangerous_command("whoami") == false);
        CHECK(is_dangerous_command("uname -a") == false);
        CHECK(is_dangerous_command("cat file.txt") == false);
        CHECK(is_dangerous_command("grep pattern file") == false);
        CHECK(is_dangerous_command("find . -name '*.cpp'") == false);
        CHECK(is_dangerous_command("git status") == false);
        CHECK(is_dangerous_command("git commit -m 'test'") == false);
    }
    
    SUBCASE("Whitespace handling") {
        CHECK(is_dangerous_command("   rm file") == true);
        CHECK(is_dangerous_command("\n\trm file") == true);
        CHECK(is_dangerous_command("\t  ls") == false);
    }
    
    SUBCASE("Empty command not dangerous") {
        CHECK(is_dangerous_command("") == false);
        CHECK(is_dangerous_command("   ") == false);
    }
}

TEST_CASE("Path Extraction") {
    SUBCASE("Extract simple path") {
        CHECK(extract_path_from_command("ls /home/user") == "/home/user");
        CHECK(extract_path_from_command("cat file.txt") == "file.txt");
    }
    
    SUBCASE("Skip flags") {
        CHECK(extract_path_from_command("ls -la /home/user") == "/home/user");
        CHECK(extract_path_from_command("cat -n file.txt") == "file.txt");
        CHECK(extract_path_from_command("rm -rf build/") == "build/");
    }
    
    SUBCASE("Command with no path argument") {
        CHECK(extract_path_from_command("nproc") == "");
        CHECK(extract_path_from_command("pwd") == "");
        CHECK(extract_path_from_command("whoami") == "");
    }
    
    SUBCASE("Relative paths preserved") {
        CHECK(extract_path_from_command("cd build") == "build");
        CHECK(extract_path_from_command("ls ../src") == "../src");
    }
}

TEST_CASE("Project Path Detection") {
    std::string project = "/home/user/myproject";
    
    SUBCASE("Relative paths are in project") {
        CHECK(is_path_in_project("build", project) == true);
        CHECK(is_path_in_project("src/main.cpp", project) == true);
        CHECK(is_path_in_project("../other", project) == true);
    }
    
    SUBCASE("Absolute paths inside project") {
        CHECK(is_path_in_project("/home/user/myproject/build", project) == true);
        CHECK(is_path_in_project("/home/user/myproject/src", project) == true);
    }
    
    SUBCASE("Absolute paths outside project") {
        CHECK(is_path_in_project("/tmp", project) == false);
        CHECK(is_path_in_project("/home/user/otherproject", project) == false);
        CHECK(is_path_in_project("/usr/bin", project) == false);
        CHECK(is_path_in_project("/", project) == false);
    }
    
    SUBCASE("Empty path handling") {
        CHECK(is_path_in_project("", project) == false);
    }
}

TEST_CASE("Approval Requirements") {
    std::string project = "/home/user/myproject";
    
    SUBCASE("Empty path requires approval") {
        CHECK(needs_approval_for_dangerous("rm", project) == true);
    }
    
    SUBCASE("Relative paths are considered in project") {
        // "file" is a relative path, so it's considered "in project" (safe)
        CHECK(needs_approval_for_dangerous("rm file", project) == false);
    }
    
    SUBCASE("Path inside project doesn't need approval") {
        CHECK(needs_approval_for_dangerous("rm build/", project) == false);
        CHECK(needs_approval_for_dangerous("rm /home/user/myproject/build/", project) == false);
    }
    
    SUBCASE("Path outside project needs approval") {
        CHECK(needs_approval_for_dangerous("rm /tmp", project) == true);
        CHECK(needs_approval_for_dangerous("rm /home/user/other", project) == true);
    }
}

TEST_CASE("Command Chaining Detection") {
    SUBCASE("Commands with && are detected") {
        CHECK(contains_command_chaining("cmake .. && make") == true);
        CHECK(contains_command_chaining("cmd1 && cmd2") == true);
    }
    
    SUBCASE("Commands with || are detected") {
        CHECK(contains_command_chaining("cmake .. || make") == true);
    }
    
    SUBCASE("Semicolon is NOT command chaining (shell syntax)") {
        // Semicolons should NOT trigger command chaining detection
        // They are passed to the shell as-is
        CHECK(contains_command_chaining("cmd1 ; cmd2") == false);
    }
    
    SUBCASE("Pipes are NOT command chaining") {
        CHECK(contains_command_chaining("ls | grep test") == false);
        CHECK(contains_command_chaining("cat file | head") == false);
    }
    
    SUBCASE("Pipes with grep patterns work correctly") {
        // This is the bug case: grep with | inside quoted string
        // Should NOT be detected as command chaining
        CHECK(contains_command_chaining(R"(grep -E "pattern|word" file)") == false);
        CHECK(contains_command_chaining("grep -E 'error|warning' log.txt") == false);
    }
    
    SUBCASE("Redirects are NOT command chaining") {
        CHECK(contains_command_chaining("echo hello > file.txt") == false);
        CHECK(contains_command_chaining("echo hello >> file.txt") == false);
    }
    
    SUBCASE("2>&1 is NOT command chaining") {
        CHECK(contains_command_chaining("make 2>&1") == false);
        CHECK(contains_command_chaining("cmake .. 2>&1 | tee build.log") == false);
    }
    
    SUBCASE("Simple commands not flagged") {
        CHECK(contains_command_chaining("ls") == false);
        CHECK(contains_command_chaining("cmake -S . -B build") == false);
        CHECK(contains_command_chaining("nproc") == false);
    }
    
    SUBCASE("Background operator (&) detected") {
        CHECK(contains_command_chaining("make &") == true);
        CHECK(contains_command_chaining("make -j8 &") == true);
    }
}

TEST_CASE("Chained Command Parsing") {
    SUBCASE("Simple && chain") {
        auto result = parse_chained_commands("cmd1 && cmd2");
        CHECK(result.size() == 2);
        CHECK(result[0].second == "cmd1");
        CHECK(result[0].first == "&&");
        CHECK(result[1].second == "cmd2");
    }
    
    SUBCASE("Complex chain with && and ||") {
        auto result = parse_chained_commands("cmake .. && make || echo 'failed'");
        CHECK(result.size() == 3);
        CHECK(result[0].first == "&&");
        CHECK(result[1].first == "||");
    }
    
    SUBCASE("Semicolon separated commands are NOT split") {
        // Semicolons are shell syntax, NOT command chaining operators
        // The whole command should be treated as a single command
        auto result = parse_chained_commands("cmd1 ; cmd2 ; cmd3");
        CHECK(result.size() == 1);
        CHECK(result[0].second == "cmd1 ; cmd2 ; cmd3");
    }
    
    SUBCASE("Pipes are NOT split") {
        // Pipes are shell syntax, NOT command chaining operators
        // The whole command should be treated as a single command
        auto result = parse_chained_commands("ls | grep test");
        CHECK(result.size() == 1);
        CHECK(result[0].second == "ls | grep test");
    }
    
    SUBCASE("Grep with pipe in pattern works correctly") {
        // This is the bug case: grep pattern with | inside quotes
        // Should NOT be split
        auto result = parse_chained_commands(R"(grep -E "Starting|Finished" log.txt)");
        CHECK(result.size() == 1);
        CHECK(result[0].second == R"(grep -E "Starting|Finished" log.txt)");
    }
    
    SUBCASE("Simple command returns single element") {
        auto result = parse_chained_commands("ls -la");
        CHECK(result.size() == 1);
        CHECK(result[0].second == "ls -la");
        CHECK(result[0].first == "");
    }
    
    SUBCASE("Empty command returns empty vector") {
        auto result = parse_chained_commands("");
        CHECK(result.empty());
    }
}

TEST_CASE("Pattern Generation") {
    SUBCASE("Simple command pattern") {
        CHECK(command_to_pattern("ls") == "ls");
        CHECK(command_to_pattern("pwd") == "pwd");
    }

    SUBCASE("Pattern with arguments becomes wildcard") {
        auto pattern = command_to_pattern("ls -la /home");
        CHECK(pattern.find("ls") != std::string::npos);
    }

    SUBCASE("Trailing ampersand is preserved in pattern") {
        // BashTool strips the '&' before execution, but the approval pattern
        // is generated from the raw command the LLM sent.
        auto pattern = command_to_pattern("python3 -m http.server 8000 &");
        CHECK(pattern.find("python3") != std::string::npos);
    }
}

TEST_CASE("Full Approval Logic Integration") {
    std::string project = "/home/user/myproject";
    
    SUBCASE("Safe commands never need approval") {
        // nproc - safe, doesn't need approval
        bool is_dangerous = is_dangerous_command("nproc");
        bool needs_approval = is_dangerous && needs_approval_for_dangerous("nproc", project);
        CHECK(needs_approval == false);
        
        // cmake build - safe, doesn't need approval
        is_dangerous = is_dangerous_command("cmake -S . -B build");
        needs_approval = is_dangerous && needs_approval_for_dangerous("cmake -S . -B build", project);
        CHECK(needs_approval == false);
        
        // make - safe, doesn't need approval
        is_dangerous = is_dangerous_command("make -j8");
        needs_approval = is_dangerous && needs_approval_for_dangerous("make -j8", project);
        CHECK(needs_approval == false);
    }
    
    SUBCASE("Dangerous commands inside project auto-approved") {
        // rm build/ - dangerous but inside project, auto-approved
        bool is_dangerous = is_dangerous_command("rm -rf build/");
        bool needs_approval = is_dangerous && needs_approval_for_dangerous("rm -rf build/", project);
        CHECK(needs_approval == false);
    }
    
    SUBCASE("Dangerous commands outside project need approval") {
        // rm /tmp - dangerous and outside project, needs approval
        bool is_dangerous = is_dangerous_command("rm -rf /tmp");
        bool needs_approval = is_dangerous && needs_approval_for_dangerous("rm -rf /tmp", project);
        CHECK(needs_approval == true);
    }
    
    SUBCASE("Common agent commands are safe") {
        std::vector<std::string> safe_commands = {
            "nproc",
            "uname -a",
            "whoami",
            "pwd",
            "ls --help",
            "grep -r pattern .",
            "find . -name '*.cpp'",
            "git status",
            "git branch",
            "ctest --output-on-failure"
        };
        
        for (const auto& cmd : safe_commands) {
            bool is_dangerous = is_dangerous_command(cmd);
            std::string msg = "Command should not be dangerous: " + cmd;
            CHECK_MESSAGE(is_dangerous == false, msg.c_str());
        }
    }
}
