#include "tools/BashPathChecker.h"
#include <algorithm>
#include <cstring>
#include <sstream>

// ========== Privilege Escalation Detection ==========

bool BashPathChecker::contains_privilege_escalation(const std::string& command) {
    // Check for privilege escalation patterns
    const char* patterns[] = {
        "sudo ", "sudo -", "sudo\n", "sudo\t",
        "su ", "su -", "su\n", "su\t",
        "pkexec",
        "doas",
        "dzdo",
        "run0"
    };

    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);

    for (const auto& pattern : patterns) {
        if (cmd_lower.find(pattern) != std::string::npos) {
            return true;
        }
    }

    // §FIX-sudo-last-token: catch `sudo`/`su` as the final token of the
    // command — the patterns above all require a trailing
    // space/dash/newline/tab, so `sudo`, `su` and `... && su` at the end
    // of the string slipped through.
    if (cmd_lower.size() >= 4 &&
        cmd_lower.compare(cmd_lower.size() - 4, 4, "sudo") == 0) {
        return true;
    }
    if (cmd_lower.size() >= 2 &&
        cmd_lower.compare(cmd_lower.size() - 2, 2, "su") == 0) {
        return true;
    }

    return false;
}

std::string BashPathChecker::privilege_escalation_message([[maybe_unused]] const std::string& command) {
    return "Security: Privilege escalation commands (sudo, su, pkexec, doas, etc.) are not allowed. "
           "The Agent runs as a normal user and cannot use sudo.";
}

// ========== Standalone Ampersand (§B3 fix) ==========

bool BashPathChecker::contains_standalone_ampersand(const std::string& command) {
    // Quote-aware single-pass scan. Skips over characters inside:
    //   - Single quotes  (')
    //   - Double quotes  (")
    //   - ANSI-C strings ($'...')
    //   - Heredocs       (<<EOF ... EOF, <<-EOF ... EOF, <<'EOF' ... EOF)
    // `&` outside any quoted/heredoc context is the background operator.
    // Inside, it's just text and we ignore it.
    //
    // Then post-process each candidate to reject:
    //   - `&&`            (logical AND — not background)
    //   - fd redirections (`2>&1`, `&>file`, etc.)
    enum class State { Normal, InSingleQuote, InDoubleQuote,
                       InHeredoc, InHeredocIndented };
    State state = State::Normal;
    std::string heredoc_marker;

    auto is_word_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    };

    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];

        switch (state) {
        case State::Normal: {
            // Heredoc detection: << EOF or <<-EOF (or quoted marker <<'EOF').
            // Only recognised when `<<` appears at the start of a word
            // boundary (preceded by whitespace or start of string).
            // We treat the heredoc body as opaque text — no operator
            // checks inside it.
            if (c == '<' && i + 1 < command.size() && command[i + 1] == '<' &&
                (i == 0 || command[i - 1] == ' ' || command[i - 1] == '\t' ||
                 command[i - 1] == '\n' || command[i - 1] == '|')) {
                // Consume "<<" and skip whitespace.
                size_t j = i + 2;
                bool indented = false;
                if (j < command.size() && command[j] == '-') { indented = true; ++j; }
                while (j < command.size() && (command[j] == ' ' ||
                       command[j] == '\t')) ++j;
                // Parse the marker (handle optional quoting).
                if (j < command.size() && (command[j] == '\'' ||
                                          command[j] == '"')) {
                    char q = command[j++];
                    size_t end = command.find(q, j);
                    if (end == std::string::npos) return false; // malformed; bail safe
                    heredoc_marker = command.substr(j, end - j);
                    j = end + 1;
                } else {
                    size_t start = j;
                    while (j < command.size() && is_word_char(command[j])) ++j;
                    heredoc_marker = command.substr(start, j - start);
                }
                if (heredoc_marker.empty()) return false;
                i = j - 1; // outer for-loop will ++
                state = indented ? State::InHeredocIndented : State::InHeredoc;
                continue;
            }

            // Single quote.
            if (c == '\'') { state = State::InSingleQuote; continue; }
            // Double quote.
            if (c == '"')  { state = State::InDoubleQuote; continue; }
            // Dollar-quote ($'...').
            if (c == '$' && i + 1 < command.size() && command[i + 1] == '\'') {
                ++i; // skip the $, advance past the quote below
                state = State::InSingleQuote;
                continue;
            }

            // The interesting character.
            if (c == '&') {
                // Skip && — logical AND.
                if (i + 1 < command.size() && command[i + 1] == '&') {
                    ++i; continue;
                }

                // fd redirection:
                //   2>&1   -> &' preceded by a digit (the fd) and followed
                //            by '1' or '2' or another digit; or followed by
                //            '>' (&>file redirect).
                //   &>file -> '&' followed by '>' (no fd before).
                //   2&>1   -> '&' between fd and '>' (rare).
                bool preceded_by_fd = (i > 0 && command[i - 1] >= '0' &&
                                         command[i - 1] <= '9');
                bool followed_by_fd_or_redir =
                    (i + 1 < command.size() &&
                     (command[i + 1] >= '0' && command[i + 1] <= '9')) ||
                    (i + 1 < command.size() && command[i + 1] == '>') ||
                    (i > 0 && command[i - 1] == '>');

                if (preceded_by_fd || followed_by_fd_or_redir) continue;

                // Reject when followed by a `>` (already covered) or by
                // digit (already covered). Anything else — whitespace,
                // EOF, another letter — is the background operator.
                return true;
            }
            break;
        }
        case State::InSingleQuote:
            if (c == '\'') state = State::Normal;
            break;
        case State::InDoubleQuote:
            // `\&` doesn't escape & in double quotes — it stays a
            // literal character. We don't have to special-case it.
            if (c == '"') state = State::Normal;
            // Backslash-escapes \$, \", \\, \`. An escaped \" doesn't close
            // the string. This is enough for the bug at hand.
            else if (c == '\\' && i + 1 < command.size() &&
                     (command[i + 1] == '"' || command[i + 1] == '\\' ||
                      command[i + 1] == '$' || command[i + 1] == '`' ||
                      command[i + 1] == '\n')) {
                ++i; // skip the escaped character
            }
            break;
        case State::InHeredoc:
        case State::InHeredocIndented: {
            // Heredoc body. The marker line ends the body. For `<<-`,
            // leading tabs are stripped on the marker line before
            // comparison.
            if (c == '\n') {
                // §FIX-heredoc-line-skip (2026-07-18, retake): the line
                // extends only to the NEXT '\n', not to the end of the
                // whole command. We must KEEP the for-loop positioned ON
                // the '\n' at the end of the line we just inspected (so
                // its ++i lands on the marker line's '\n' for the next
                // round of inspection). Setting i = line_end jumped
                // PAST that '\n', hiding the "EOF\n" line — including
                // any trailing `&` command — from the rest of the
                // scanner. Use `line_end - 1` so ++i re-lands on
                // line_end itself, where the marker check runs again
                // with the FOLLOWING line as `cmp`.
                size_t line_start = i + 1;
                size_t line_end = command.find('\n', line_start);
                if (line_end == std::string::npos) line_end = command.size();
                std::string cmp = command.substr(line_start, line_end - line_start);
                if (state == State::InHeredocIndented) {
                    size_t k = 0;
                    while (k < cmp.size() && cmp[k] == '\t') ++k;
                    cmp = cmp.substr(k);
                }
                // Tolerate CRLF line endings on the marker line.
                if (!cmp.empty() && cmp.back() == '\r') cmp.pop_back();
                // Marker line must be exactly the marker (after the
                // indentation strip).
                if (cmp == heredoc_marker) {
                    heredoc_marker.clear();
                    state = State::Normal;
                }
                // Park i so the for-loop's ++ lands ON this '\n' at
                // line_end, which is the boundary between this line and
                // the next (potential marker) line.
                i = (line_end > 0) ? line_end - 1 : 0;
                continue;
            }
            // Inside the body (not at a newline) — just consume.
            break;
        }
        } // switch
    } // for

    // No unquoted `&` found.
    return false;
}

// ========== Dangerous Command Detection ==========

bool BashPathChecker::is_dangerous_command(const std::string& command) {
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

std::string BashPathChecker::extract_path_from_command(const std::string& command) {
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

std::vector<std::string> BashPathChecker::extract_paths(const std::string& command) {
    std::vector<std::string> paths;
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;  // skip command name

    std::string arg;
    while (iss >> arg) {
        if (arg.empty()) continue;
        if (arg[0] == '-') continue;  // skip flags
        paths.push_back(arg);
    }
    return paths;
}

std::string BashPathChecker::extract_first_path(const std::string& command) {
    std::vector<std::string> paths = extract_paths(command);
    if (paths.empty()) {
        return "";
    }
    return paths[0];
}