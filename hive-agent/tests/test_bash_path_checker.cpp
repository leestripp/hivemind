#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vector>
#include <string>
#include <stdlib.h>
#include "tools/BashPathChecker.h"

TEST_CASE("BashPathChecker - Privilege Escalation Detection") {
    SUBCASE("sudo commands blocked") {
        CHECK(BashPathChecker::contains_privilege_escalation("sudo apt update") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("sudo -i") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("sudo su -") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("SUDO apt install") == true);  // case insensitive
    }
    
    SUBCASE("su commands blocked") {
        CHECK(BashPathChecker::contains_privilege_escalation("su - root") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("su -") == true);
    }
    
    SUBCASE("pkexec blocked") {
        CHECK(BashPathChecker::contains_privilege_escalation("pkexec some-command") == true);
    }
    
    SUBCASE("doas blocked") {
        CHECK(BashPathChecker::contains_privilege_escalation("doas apt update") == true);
    }
    
    SUBCASE("dzdo blocked") {
        CHECK(BashPathChecker::contains_privilege_escalation("dzdo rm -rf /") == true);
    }
    
    SUBCASE("run0 blocked") {
        CHECK(BashPathChecker::contains_privilege_escalation("run0 bash") == true);
    }
    
    SUBCASE("Normal commands allowed") {
        CHECK(BashPathChecker::contains_privilege_escalation("ls -la") == false);
        CHECK(BashPathChecker::contains_privilege_escalation("gcc -o main main.c") == false);
        // §Phase 9.10: daedalus itself builds with clang++ (see CMakeLists.txt),
        // but the BashTool approval logic must recognise EVERY common
        // C/C++ compiler name — the agent compiles USER code, which may
        // be using whatever toolchain the user has installed. Lock in
        // that none of these trigger the privilege-escalation detector.
        CHECK(BashPathChecker::contains_privilege_escalation("g++ -std=c++23 -o main main.cpp") == false);
        CHECK(BashPathChecker::contains_privilege_escalation("clang -o main main.c") == false);
        CHECK(BashPathChecker::contains_privilege_escalation("clang++ -fsyntax-only -std=c++23 main.cpp") == false);
        CHECK(BashPathChecker::contains_privilege_escalation("rm -rf build") == false);
        CHECK(BashPathChecker::contains_privilege_escalation("cd /tmp && ls") == false);
        CHECK(BashPathChecker::contains_privilege_escalation("cat /etc/hostname") == false);
    }
    
    SUBCASE("Commands that start with sudo but aren't privilege escalation") {
        // Just "sudo" without a space is not blocked (edge case)
        CHECK(BashPathChecker::contains_privilege_escalation("sudo-test") == false);
    }

    SUBCASE("sudo/su as the last token or bare (§FIX-sudo-last-token)") {
        CHECK(BashPathChecker::contains_privilege_escalation("sudo") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("cd /tmp && sudo") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("su") == true);
        CHECK(BashPathChecker::contains_privilege_escalation("su root") == true);
    }
}

TEST_CASE("BashPathChecker - contains_standalone_ampersand (B3)") {
    SUBCASE("Detects bare backgrounding `&`") {
        CHECK(BashPathChecker::contains_standalone_ampersand("sleep 60 &") == true);
        CHECK(BashPathChecker::contains_standalone_ampersand("cmd &") == true);
        CHECK(BashPathChecker::contains_standalone_ampersand("cmd1 & cmd2") == true);
        CHECK(BashPathChecker::contains_standalone_ampersand("&") == true);
        CHECK(BashPathChecker::contains_standalone_ampersand("ls &\n") == true);
    }
    SUBCASE("Allows `&&` (logical AND)") {
        CHECK(BashPathChecker::contains_standalone_ampersand("a && b") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("a&&b") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("a && b && c") == false);
    }
    SUBCASE("Allows `&` inside double quotes") {
        CHECK(BashPathChecker::contains_standalone_ampersand("echo \"hello & world\"") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("printf \"a&b\"") == false);
    }
    SUBCASE("Allows `&` inside single quotes") {
        CHECK(BashPathChecker::contains_standalone_ampersand("echo 'hello & world'") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("printf 'a&b'") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("echo $'a&b'") == false);
    }
    SUBCASE("Allows fd redirection `>&N` and `&>`") {
        CHECK(BashPathChecker::contains_standalone_ampersand("cmd 2>&1") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("cmd 1>&2") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("cmd &> /dev/null") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand("cmd >file 2>&1") == false);
    }
    SUBCASE("Allows `&` inside heredocs") {
        CHECK(BashPathChecker::contains_standalone_ampersand(
            "cat <<EOF\nfoo & bar\nEOF") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand(
            "cat <<-EOF\n\tfoo & bar\n\tEOF") == false);
        CHECK(BashPathChecker::contains_standalone_ampersand(
            "cat <<'EOF'\nfoo & bar\nEOF") == false);
    }
    SUBCASE("Rejects mixed: quoted `&` followed by real `&`") {
        CHECK(BashPathChecker::contains_standalone_ampersand(
            "echo \"x &\" &") == true);
    }
    SUBCASE("Detects `&` after the heredoc marker (§FIX-heredoc-line-skip)") {
        CHECK(BashPathChecker::contains_standalone_ampersand(
            "cat <<EOF\nhello\nEOF\nsleep 100 &") == true);
        // ...but `&` inside the body is still data, and a clean command
        // after the marker is not backgrounded.
        CHECK(BashPathChecker::contains_standalone_ampersand(
            "cat <<EOF\nfoo & bar\nEOF\necho done") == false);
    }
}

TEST_CASE("BashPathChecker - privilege_escalation_message") {
    SUBCASE("Returns error message") {
        std::string msg = BashPathChecker::privilege_escalation_message("sudo rm -rf /");
        CHECK(msg.find("sudo") != std::string::npos);
        CHECK(msg.find("Privilege") != std::string::npos);
        CHECK(msg.find("normal user") != std::string::npos);
    }
}

TEST_CASE("BashPathChecker - is_dangerous_command") {
    SUBCASE("rm commands") {
        CHECK(BashPathChecker::is_dangerous_command("rm file.txt") == true);
        CHECK(BashPathChecker::is_dangerous_command("rm -rf build") == true);
        CHECK(BashPathChecker::is_dangerous_command("rm -r directory") == true);
        CHECK(BashPathChecker::is_dangerous_command("rm -f file") == true);
    }
    
    SUBCASE("Other dangerous commands") {
        CHECK(BashPathChecker::is_dangerous_command("del file.txt") == true);
        CHECK(BashPathChecker::is_dangerous_command("rmdir empty_dir") == true);
        CHECK(BashPathChecker::is_dangerous_command("shred -u file") == true);
        CHECK(BashPathChecker::is_dangerous_command("fdisk -l") == true);
    }
    
    SUBCASE("Non-dangerous commands") {
        CHECK(BashPathChecker::is_dangerous_command("ls -la") == false);
        CHECK(BashPathChecker::is_dangerous_command("cat file.txt") == false);
        CHECK(BashPathChecker::is_dangerous_command("gcc -o main main.c") == false);
        // §Phase 9.10: lock in that the agent's safety checker doesn't
        // false-positive on any of the common C/C++ compiler names.
        // See the matching "Normal commands allowed" subcase above for
        // the rationale (daedalus builds with clang++, but user code
        // may use any of these).
        CHECK(BashPathChecker::is_dangerous_command("g++ -std=c++23 -o main main.cpp") == false);
        CHECK(BashPathChecker::is_dangerous_command("clang -o main main.c") == false);
        CHECK(BashPathChecker::is_dangerous_command("clang++ -fsyntax-only -std=c++23 main.cpp") == false);
        CHECK(BashPathChecker::is_dangerous_command("echo hello") == false);
    }
}

TEST_CASE("BashPathChecker - extract_paths") {
    SUBCASE("Simple command") {
        auto paths = BashPathChecker::extract_paths("rm -rf build");
        CHECK(paths.size() == 1);
        CHECK(paths[0] == "build");
    }
    
    SUBCASE("Command with multiple paths") {
        auto paths = BashPathChecker::extract_paths("rm -f file1.cpp file2.cpp file3.cpp");
        CHECK(paths.size() == 3);
        CHECK(paths[0] == "file1.cpp");
        CHECK(paths[1] == "file2.cpp");
        CHECK(paths[2] == "file3.cpp");
    }
    
    SUBCASE("Command with flags but no paths") {
        auto paths = BashPathChecker::extract_paths("ls -la");
        CHECK(paths.size() == 0);
    }
    
    SUBCASE("Command with flags and paths") {
        auto paths = BashPathChecker::extract_paths("g++ -std=c++17 -o main main.cpp");
        CHECK(paths.size() == 2);
        CHECK(paths[0] == "main");  // -o main is an output flag, but "main" looks like a path arg
        CHECK(paths[1] == "main.cpp");
    }
    
    SUBCASE("Empty command") {
        auto paths = BashPathChecker::extract_paths("");
        CHECK(paths.size() == 0);
    }
}

TEST_CASE("BashPathChecker - extract_first_path") {
    SUBCASE("With paths") {
        CHECK(BashPathChecker::extract_first_path("rm -rf build") == "build");
        CHECK(BashPathChecker::extract_first_path("g++ -o main main.cpp") == "main");
    }
    
    SUBCASE("Without paths") {
        CHECK(BashPathChecker::extract_first_path("ls -la") == "");
    }
}

TEST_CASE("BashPathChecker - extract_path_from_command") {
    SUBCASE("With paths") {
        CHECK(BashPathChecker::extract_path_from_command("rm -rf build") == "build");
        CHECK(BashPathChecker::extract_path_from_command("cat /etc/passwd") == "/etc/passwd");
    }
    
    SUBCASE("Without paths") {
        CHECK(BashPathChecker::extract_path_from_command("ls -la") == "");
        CHECK(BashPathChecker::extract_path_from_command("pwd") == "");
    }
}