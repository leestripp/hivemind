// Unit tests for the STT model pre-flight probe (SttModelProbe). Every
// failure shape the engine can hit — missing dir, missing file,
// truncated download, one-of-two present — without whisper or real
// 40 MB+ models: minBytes is injectable.
#include "../src/SttModelProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("hv-stt-test-" + std::to_string(static_cast<long long>(::getpid())) +
                "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    std::string str() const { return path.string(); }
};

void writeFile(const fs::path& p, std::size_t bytes) {
    std::ofstream f(p, std::ios::binary);
    f << std::string(bytes, 'x');
}

constexpr std::uintmax_t kTestMin = 16;  // tiny threshold for fixture files

}  // namespace

TEST_CASE("probe: missing directory", "[stt][models]") {
    const auto st = hv::probeSttModels("/nonexistent/hv-stt-test", kTestMin);
    REQUIRE_FALSE(st.dirExists);
    REQUIRE_FALSE(st.partial.present);
    REQUIRE_FALSE(st.final.present);
    REQUIRE_FALSE(st.ready());
    const auto msg = st.userMessage();
    REQUIRE(msg.find("ggml-tiny.en-q8_0.bin") != std::string::npos);
    REQUIRE(msg.find("ggml-base.en-q8_0.bin") != std::string::npos);
    REQUIRE(msg.find("missing") != std::string::npos);
    // Actionable: the exact curl commands to fix it.
    REQUIRE(msg.find("curl -fSL") != std::string::npos);
    REQUIRE(msg.find(hv::kSttModelBaseUrl) != std::string::npos);
    const auto sum = st.summary();
    REQUIRE(sum.find("MISSING") != std::string::npos);
}

TEST_CASE("probe: empty directory", "[stt][models]") {
    TempDir d;
    const auto st = hv::probeSttModels(d.str(), kTestMin);
    REQUIRE(st.dirExists);
    REQUIRE_FALSE(st.partial.present);
    REQUIRE_FALSE(st.final.present);
    REQUIRE_FALSE(st.ready());
    REQUIRE(st.summary().find("(exists)") != std::string::npos);
}

TEST_CASE("probe: only the partial model present", "[stt][models]") {
    TempDir d;
    writeFile(d.path / hv::kSttModelPartial, kTestMin);
    const auto st = hv::probeSttModels(d.str(), kTestMin);
    REQUIRE(st.partial.plausible);
    REQUIRE_FALSE(st.final.present);
    REQUIRE_FALSE(st.ready());
    const auto msg = st.userMessage();
    // Names ONLY the missing one — no noise about the good file.
    REQUIRE(msg.find("ggml-base.en-q8_0.bin — missing") != std::string::npos);
    REQUIRE(msg.find("ggml-tiny.en-q8_0.bin — ") == std::string::npos);
    REQUIRE(msg.find(std::string("curl -fSL -o \"") + d.str() + "/" +
                     hv::kSttModelFinal) != std::string::npos);
}

TEST_CASE("probe: both models present and plausible", "[stt][models]") {
    TempDir d;
    writeFile(d.path / hv::kSttModelPartial, kTestMin);
    writeFile(d.path / hv::kSttModelFinal, kTestMin * 2);
    const auto st = hv::probeSttModels(d.str(), kTestMin);
    REQUIRE(st.ready());
    REQUIRE(st.userMessage().empty());
    REQUIRE(st.summary().find("ok") != std::string::npos);
    REQUIRE(st.final.sizeBytes == kTestMin * 2);
}

TEST_CASE("probe: truncated download counts as unusable", "[stt][models]") {
    TempDir d;
    writeFile(d.path / hv::kSttModelPartial, kTestMin);
    writeFile(d.path / hv::kSttModelFinal, kTestMin - 1);  // 1 byte short
    const auto st = hv::probeSttModels(d.str(), kTestMin);
    REQUIRE(st.final.present);
    REQUIRE_FALSE(st.final.plausible);
    REQUIRE_FALSE(st.ready());
    const auto msg = st.userMessage();
    REQUIRE(msg.find("truncated") != std::string::npos);
    REQUIRE(st.summary().find("TRUNCATED") != std::string::npos);
}

TEST_CASE("probe: zero-byte file counts as truncated", "[stt][models]") {
    TempDir d;
    writeFile(d.path / hv::kSttModelPartial, 0);
    writeFile(d.path / hv::kSttModelFinal, 0);
    const auto st = hv::probeSttModels(d.str(), kTestMin);
    REQUIRE(st.partial.present);
    REQUIRE_FALSE(st.partial.plausible);
    REQUIRE_FALSE(st.ready());
}

TEST_CASE("probe: a directory masquerading as a model is not present",
          "[stt][models]") {
    TempDir d;
    fs::create_directories(d.path / hv::kSttModelPartial);  // a DIR
    const auto st = hv::probeSttModels(d.str(), kTestMin);
    REQUIRE_FALSE(st.partial.present);
    REQUIRE_FALSE(st.ready());
}

TEST_CASE("sttModelDir follows $HOME", "[stt][models]") {
    const char* old = std::getenv("HOME");
    const std::string saved = old ? old : "";
    setenv("HOME", "/tmp/hv-fake-home", 1);
    REQUIRE(hv::sttModelDir() ==
            "/tmp/hv-fake-home/.local/share/hive-client-voice/models");
    if (saved.empty())
        unsetenv("HOME");
    else
        setenv("HOME", saved.c_str(), 1);
}

TEST_CASE("sttModelDir falls back to . when $HOME unset", "[stt][models]") {
    const char* old = std::getenv("HOME");
    const std::string saved = old ? old : "";
    unsetenv("HOME");
    REQUIRE(hv::sttModelDir() == "./.local/share/hive-client-voice/models");
    if (!saved.empty()) setenv("HOME", saved.c_str(), 1);
}
