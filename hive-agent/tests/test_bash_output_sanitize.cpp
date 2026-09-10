/**
 * @file test_bash_output_sanitize.cpp
 * @brief Hermetic tests for BashCommandExecutor TTY output sanitisation
 *
 * Verifies that terminal-control sequences (cursor movement, screen erase,
 * alternate buffer, OSC, etc.) are stripped from captured shell output while
 * UTF-8 text and ANSI SGR colour/style codes are preserved.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tools/BashCommandExecutor.h"
#include "tools/SanitizedOutputStream.h"

#include <random>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

static std::string esc(const std::string& seq) {
    return "\x1B" + seq;
}

// ============================================================================
// Tests for sanitize_tty_sequences
// ============================================================================

TEST_CASE("Plain text passes through unchanged") {
    std::string input = "Hello, world!\nThis is a test.\n";
    CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
}

TEST_CASE("UTF-8 text is preserved") {
    std::string input =
        "English: hello\n"
        "Chinese: 你好世界\n"
        "Japanese: こんにちは\n"
        "Korean: 안녕하세요\n"
        "Russian: Привет мир\n"
        "Emoji: 👋🌍🚀🎉\n"
        "Accented: café, naïve, résumé\n";
    CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
}

TEST_CASE("Tabs and newlines are preserved") {
    std::string input = "col1\tcol2\tcol3\nline2\n";
    CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
}

TEST_CASE("SGR colour and style codes are preserved") {
    SUBCASE("Basic colours") {
        std::string input = esc("[31mred") + esc("[0m") + " normal";
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
    }

    SUBCASE("Bold, dim, italic, underline") {
        std::string input =
            esc("[1mbold") + esc("[0m ") +
            esc("[2mdim") + esc("[0m ") +
            esc("[3mitalic") + esc("[0m ") +
            esc("[4munderline") + esc("[0m");
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
    }

    SUBCASE("256-colour and true-colour") {
        std::string input =
            esc("[38;5;196m256-red") + esc("[0m ") +
            esc("[38;2;255;128;0mtruecolor") + esc("[0m");
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
    }

    SUBCASE("Empty SGR reset") {
        std::string input = esc("[mplain");
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == input);
    }
}

TEST_CASE("Cursor movement CSI sequences are stripped") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[2A")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[5B")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[10C")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[3D")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[2E")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[4F")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[20G")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[5;10H")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[5;10f")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[s")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[u")) == "");
}

TEST_CASE("Screen and line erase sequences are stripped") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[2J")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[3J")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[0J")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[K")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[2K")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[1K")) == "");
}

TEST_CASE("Alternate screen buffer and cursor visibility are stripped") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[?1049h")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[?1049l")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[?25l")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("[?25h")) == "");
}

TEST_CASE("OSC sequences are stripped") {
    SUBCASE("Window title") {
        std::string input = esc("]0;My Window Title\x07");
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == "");
    }

    SUBCASE("Hyperlink") {
        std::string input =
            esc("]8;;https://example.com\x07") +
            "link text" +
            esc("]8;;\x07");
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == "link text");
    }

    SUBCASE("OSC string terminator variant") {
        std::string input = esc("]2;Title\x1B\\");
        CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == "");
    }
}

TEST_CASE("DCS, APC, PM, SOS string sequences are stripped") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("P1;1;2{data\x1B\\")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("_app data\x1B\\")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("^privacy\x1B\\")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("Xstring\x1B\\")) == "");
}

TEST_CASE("Standalone ESC sequences are stripped") {
    // Save/restore cursor.
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("7")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("8")) == "");
    // Reset.
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("c")) == "");
    // Keypad modes.
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("=")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc(">")) == "");
    // Charset selection.
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc("(B")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(esc(")0")) == "");
}

TEST_CASE("C0 control characters are stripped or normalised") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences(std::string("\x07")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(std::string("\x08")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(std::string("\x0B")) == "");
    CHECK(BashCommandExecutor::sanitize_tty_sequences(std::string("\x0C")) == "");
}

TEST_CASE("Carriage returns are normalised to LF") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences("line1\r\nline2\r\n") == "line1\nline2\n");
    CHECK(BashCommandExecutor::sanitize_tty_sequences("line1\rline2\r") == "line1\nline2\n");
    CHECK(BashCommandExecutor::sanitize_tty_sequences("a\r\nb") == "a\nb");
}

TEST_CASE("Mixed TUI-like output is rendered as readable text") {
    std::string input =
        esc("[?1049h") +               // enter alternate screen
        esc("[2J") + esc("[H") +       // clear and home cursor
        esc("[1m") + "Process List" + esc("[0m") + "\n" +
        esc("[31m") + "cpu: 12%" + esc("[0m") + "\n" +
        esc("[2;10H") + "at row 2 col 10" + "\n" +
        esc("[?1049l");                // leave alternate screen

    // SGR colour/style codes are preserved; all cursor/screen control is dropped.
    std::string expected =
        esc("[1m") + "Process List" + esc("[0m") + "\n" +
        esc("[31m") + "cpu: 12%" + esc("[0m") + "\n" +
        "at row 2 col 10\n";
    CHECK(BashCommandExecutor::sanitize_tty_sequences(input) == expected);
}

TEST_CASE("SGR colour is preserved around stripped cursor movement") {
    std::string input =
        esc("[32m") + "green" +
        esc("[10D") +                   // move cursor back
        esc("[0m") + "done";
    CHECK(BashCommandExecutor::sanitize_tty_sequences(input) ==
          esc("[32m") + "green" + esc("[0m") + "done");
}

TEST_CASE("Incomplete escape sequence at end of output is dropped") {
    CHECK(BashCommandExecutor::sanitize_tty_sequences("hello\x1B") == "hello");
    CHECK(BashCommandExecutor::sanitize_tty_sequences("hello\x1B[") == "hello");
    CHECK(BashCommandExecutor::sanitize_tty_sequences("hello\x1B[31") == "hello");
    CHECK(BashCommandExecutor::sanitize_tty_sequences("hello\x1B]0;title") == "hello");
}

TEST_CASE("sanitize_output still detects binary content") {
    // 20% non-printable bytes should trigger the binary path.
    std::string input(100, '\x00');
    std::string result = BashCommandExecutor::sanitize_output(input);
    CHECK(result.find("[Binary output truncated") != std::string::npos);
}

TEST_CASE("sanitize_output still truncates oversized output") {
    std::string input(25001, 'x');
    std::string result = BashCommandExecutor::sanitize_output(input);
    CHECK(result.find("[Output truncated") != std::string::npos);
    CHECK(result.size() < input.size());
}

TEST_CASE("sanitize_output strips TTY sequences after binary check") {
    // Keep the ANSI escape density below the binary-detection threshold
    // (10% non-printable bytes) by including enough printable text.
    std::string input =
        "Status report from the build system:\n"
        "- compilation started at 10:00\n"
        "- all targets up to date\n" +
        esc("[2J") +                    // clear screen (stripped)
        esc("[31m") + "error" + esc("[0m") + "\n" +
        esc("[?25l");                   // hide cursor (stripped)
    std::string result = BashCommandExecutor::sanitize_output(input);
    CHECK(result ==
          "Status report from the build system:\n"
          "- compilation started at 10:00\n"
          "- all targets up to date\n" +
          esc("[31m") + "error" + esc("[0m") + "\n");
}

// ============================================================================
// Tests for streaming output chunks
// ============================================================================

TEST_CASE("Streaming execute forwards sanitised output chunks") {
    std::vector<std::string> chunks;
    auto chunk_cb = [&chunks](std::string_view chunk) {
        chunks.emplace_back(chunk);
    };

    auto result = BashCommandExecutor::execute(
        "printf 'line1\\nline2\\nline3\\n'", ".", 10, chunk_cb);

    CHECK(result.success);
    CHECK(result.output == "line1\nline2\nline3\n");

    // Chunks may be coalesced depending on timing, but the concatenation
    // of all received chunks must equal the full sanitised output.
    std::string joined;
    for (const auto& c : chunks) {
        joined += c;
    }
    CHECK(joined == result.output);
}

TEST_CASE("Streaming chunks are sanitised in real time") {
    std::vector<std::string> chunks;
    auto chunk_cb = [&chunks](std::string_view chunk) {
        chunks.emplace_back(chunk);
    };

    // Cursor movement and screen-clear sequences should be stripped before
    // the host ever sees the chunk, while the SGR colour is preserved.
    // Include enough plain text to keep the ANSI escape density below the
    // binary-detection threshold.
    std::string command =
        std::string("printf '") +
        "Status report from the build:\n" +
        "- step one completed\n" +
        esc("[2J") +                    // clear screen (stripped)
        esc("[31m") + "error" + esc("[0m") + "\n" +
        esc("[10D") +                   // cursor back (stripped)
        "- step two completed\n" +
        "- all done\n'";

    auto result = BashCommandExecutor::execute(command, ".", 10, chunk_cb);

    CHECK(result.success);
    CHECK(result.output ==
          "Status report from the build:\n"
          "- step one completed\n" +
          esc("[31m") + "error" + esc("[0m") + "\n"
          "- step two completed\n"
          "- all done\n");

    for (const auto& chunk : chunks) {
        CHECK(chunk.find(esc("[2J")) == std::string::npos);
        CHECK(chunk.find(esc("[10D")) == std::string::npos);
    }
}

TEST_CASE("No chunks emitted for command with no output") {
    std::vector<std::string> chunks;
    auto chunk_cb = [&chunks](std::string_view chunk) {
        chunks.emplace_back(chunk);
    };

    auto result = BashCommandExecutor::execute("true", ".", 10, chunk_cb);

    CHECK(result.success);
    CHECK(result.output.empty());
    CHECK(chunks.empty());
}

// ============================================================================
// Tests for SanitizedOutputStream — split-invariance / reassembly guarantee
//
// The core streaming guarantee: for ANY input and ANY append() split
// pattern, concatenating all emitted chunks equals one-shot sanitisation
// of the whole input. A chunk boundary must never split an ANSI escape
// sequence, a multi-byte UTF-8 character, or a CR/LF pair.
// ============================================================================

// Feed `input` through a stream in `piece_size` pieces and return the
// concatenation of all emitted chunks.
static std::string streamed_in_pieces(const std::string& input, size_t piece_size) {
    std::string joined;
    SanitizedOutputStream stream(
        [&joined](std::string_view chunk) { joined.append(chunk); });
    for (size_t pos = 0; pos < input.size(); pos += piece_size) {
        stream.append(input.substr(pos, piece_size));
    }
    stream.flush();
    return joined;
}

TEST_CASE("Every two-part split matches one-shot sanitisation") {
    // Nasty input: SGR, cursor movement, OSC, multi-byte UTF-8 (2/3/4-byte),
    // CR/LF pairs, a lone CR, and control characters.
    const std::string input =
        "start\n" +
        esc("[31m") + "red" + esc("[0m") + "\n" +
        esc("[2J") + esc("[10D") +
        esc("]8;;http://example.com") + "\x07" + "link\n" +
        "h\xC3\xA9llo " +              // é (2-byte)
        "\xE4\xBD\xA0\xE5\xA5\xBD " +  // 你好 (3-byte)
        "\xF0\x9F\x9A\x80 " +          // 🚀 (4-byte)
        "win\r\nline\rlone\n" +
        "bell\x07 end\n";

    const std::string expected =
        BashCommandExecutor::sanitize_tty_sequences(input);

    for (size_t cut = 0; cut <= input.size(); ++cut) {
        std::string joined;
        SanitizedOutputStream stream(
            [&joined](std::string_view chunk) { joined.append(chunk); });
        stream.append(input.substr(0, cut));
        stream.append(input.substr(cut));
        stream.flush();
        CHECK(joined == expected);
    }
}

TEST_CASE("Split-invariance across a battery of inputs and piece sizes") {
    const std::string long_line =
        std::string(3000, 'a') + esc("[32m") + std::string(3000, 'b') +
        esc("[0m");  // newline-less, crosses the 4096 emission threshold

    const std::vector<std::string> inputs = {
        "plain text\nwith lines\n",
        esc("[31m") + "colour" + esc("[0m") + esc("[2J") + "cleared\n",
        "h\xC3\xA9llo \xE4\xBD\xA0\xE5\xA5\xBD \xF0\x9F\x9A\x80\n",
        "win\r\nline\rlone\rend\r",
        "truncated escape at end" + esc("[31"),
        "invalid \x80\x80 utf-\xff\xFE text\n",
        esc("]0;window title") + "\x07" + "after osc\n",
        esc("P1;2|payload") + esc("\\") + "after dcs\n",
        long_line,
    };

    for (const auto& input : inputs) {
        const std::string expected =
            BashCommandExecutor::sanitize_tty_sequences(input);

        SUBCASE("byte-at-a-time") {
            CHECK(streamed_in_pieces(input, 1) == expected);
        }
        SUBCASE("small fixed pieces") {
            CHECK(streamed_in_pieces(input, 3) == expected);
            CHECK(streamed_in_pieces(input, 7) == expected);
        }
        SUBCASE("random pieces (fixed seed)") {
            std::mt19937 rng(42);
            std::uniform_int_distribution<size_t> dist(1, 11);
            std::string joined;
            SanitizedOutputStream stream(
                [&joined](std::string_view chunk) { joined.append(chunk); });
            size_t pos = 0;
            while (pos < input.size()) {
                const size_t n = std::min(dist(rng), input.size() - pos);
                stream.append(input.substr(pos, n));
                pos += n;
            }
            stream.flush();
            CHECK(joined == expected);
        }
    }
}

TEST_CASE("Streamed chunks reassemble exactly for long newline-less output") {
    // Regression: the old per-slice sanitiser corrupted output when the
    // 4096-byte / 100 ms fallback cut mid-UTF-8 or mid-escape. A single
    // >4096-byte line forces that path repeatedly.
    std::vector<std::string> chunks;
    auto chunk_cb = [&chunks](std::string_view chunk) {
        chunks.emplace_back(chunk);
    };

    std::string command =
        std::string("printf '\\033[31m'; ") +
        "head -c 5000 </dev/zero | tr '\\000' 'a'; " +
        "printf '\\033[0m\\n'";

    auto result = BashCommandExecutor::execute(command, ".", 10, chunk_cb);

    const std::string expected =
        esc("[31m") + std::string(5000, 'a') + esc("[0m") + "\n";
    CHECK(result.success);
    CHECK(result.output == expected);

    // The concatenation of all emitted chunks must equal the full output.
    // (Chunk count is timing-dependent — the subprocess may deliver the
    // whole line in one read; the deterministic threshold-path coverage
    // is the `long_line` case in the split-invariance battery above.)
    std::string joined;
    for (const auto& c : chunks) {
        joined += c;
    }
    CHECK(joined == result.output);
}
