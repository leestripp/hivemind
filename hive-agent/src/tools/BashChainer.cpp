#include "tools/BashChainer.h"
#include <sstream>

#include "core/Logger.h"
bool BashChainer::contains_command_chaining(const std::string& command) {
    // Command chaining operators (&&, ||, ;) are now allowed - execute directly
    (void)command;  // Suppress unused parameter warning
    return false;
}

bool BashChainer::contains_background_operator(const std::string& command) {
    size_t amp_pos = command.find('&');
    if (amp_pos == std::string::npos) {
        return false;
    }
    
    while (amp_pos != std::string::npos) {
        if (amp_pos + 1 >= command.length()) {
            return true;
        }
        
        size_t next_pos = amp_pos + 1;
        char next_char = command[next_pos];
        
        if (next_char == '1' || next_char == '2') {
            if (amp_pos > 0 && command[amp_pos - 1] == '2') {
                amp_pos = command.find('&', next_pos);
                continue;
            }
        }
        
        if (next_char == '>') {
            amp_pos = command.find('&', next_pos);
            continue;
        }
        
        if (amp_pos > 0 && (command[amp_pos - 1] == '1' || command[amp_pos - 1] == '2')) {
            amp_pos = command.find('&', next_pos);
            continue;
        }
        
        bool preceded_by_space = (amp_pos == 0) || (command[amp_pos - 1] == ' ') || (command[amp_pos - 1] == '\t');
        bool followed_by_space = (next_pos >= command.length()) || 
                                  (next_char == ' ') || 
                                  (next_char == '\t') ||
                                  (next_char == '\n');
        
        if (preceded_by_space || followed_by_space) {
            return true;
        }
        
        amp_pos = command.find('&', next_pos);
    }
    
    return false;
}

std::vector<std::pair<std::string, std::string>> 
BashChainer::parse_chained_commands(const std::string& command) {
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
        
        if (operator_pos == i) {
            if (!current_cmd.empty()) {
                result.push_back({current_operator, current_cmd});
                current_cmd.clear();
            }
            current_operator = found_op;
            i += found_op.length();
            continue;
        }
        
        if (operator_pos < command.length()) {
            current_cmd += command.substr(i, operator_pos - i);
            
            if (!current_cmd.empty()) {
                size_t trim_start = current_cmd.find_first_not_of(" \t");
                if (trim_start != std::string::npos) {
                    current_cmd = current_cmd.substr(trim_start);
                }
                size_t trim_end = current_cmd.find_last_not_of(" \t");
                if (trim_end != std::string::npos) {
                    current_cmd = current_cmd.substr(0, trim_end + 1);
                }
                result.push_back({current_operator, current_cmd});
            }
            current_operator = found_op;
            current_cmd.clear();
            i = operator_pos + found_op.length();
        } else {
            size_t start = i;
            while (start < command.length() && (command[start] == ' ' || command[start] == '\t')) {
                start++;
            }
            if (start < command.length()) {
                current_cmd += command.substr(start);
            }
            break;
        }
    }
    
    if (!current_cmd.empty()) {
        result.push_back({current_operator, current_cmd});
    }
    
    return result;
}