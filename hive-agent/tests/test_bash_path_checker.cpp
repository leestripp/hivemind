#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vector>
#include <string>
#include <stdlib.h>
#include "tools/BashPathChecker.h"

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