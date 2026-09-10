#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Logger.h"

// §logging: tests for Logger self-initialization and basic file output.
// NOTE: the Logger is a process-wide singleton, so test order matters —
// the lazy cwd-fallback case runs first (fresh state), the explicit
// initialize() cases afterwards re-point the logger at their own temp dirs.

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path first_log_file(const fs::path& dir) {
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".log") {
            return e.path();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("logger self-initializes on first use (cwd fallback)") {
    auto& logger = Logger::get_instance();
    REQUIRE(!logger.is_initialized());

    fs::path tmp = fs::temp_directory_path() / "hive_agent_logger_lazy_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const auto old_cwd = fs::current_path();
    fs::current_path(tmp);
    LOG_INFO("lazy-init-marker");
    fs::current_path(old_cwd);

    CHECK(logger.is_initialized());
    CHECK(logger.is_auto_initialized());

    fs::path log_file = first_log_file(tmp / ".daedalus" / "hive-brain" / "logs");
    REQUIRE(!log_file.empty());
    CHECK(read_file(log_file).find("lazy-init-marker") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("explicit initialize writes to the configured log dir") {
    auto& logger = Logger::get_instance();
    fs::path tmp = fs::temp_directory_path() / "hive_agent_logger_explicit_test";
    fs::remove_all(tmp);

    LoggerConfig cfg;
    cfg.log_path = "logs/";
    logger.initialize(tmp, cfg);
    CHECK(logger.is_initialized());
    CHECK(!logger.is_auto_initialized());  // explicit init wins over fallback

    LOG_INFO("explicit-marker");

    fs::path log_file = first_log_file(tmp / "logs");
    REQUIRE(!log_file.empty());
    CHECK(read_file(log_file).find("explicit-marker") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("log level filtering suppresses DEBUG at default INFO level") {
    auto& logger = Logger::get_instance();
    fs::path tmp = fs::temp_directory_path() / "hive_agent_logger_level_test";
    fs::remove_all(tmp);

    LoggerConfig cfg;  // default level = INFO
    cfg.log_path = "logs/";
    logger.initialize(tmp, cfg);

    LOG_DEBUG("debug-marker-hidden");
    logger.set_level(LogLevel::DEBUG);
    LOG_DEBUG("debug-marker-visible");
    logger.set_level(LogLevel::INFO);  // restore default

    fs::path log_file = first_log_file(tmp / "logs");
    REQUIRE(!log_file.empty());
    const std::string content = read_file(log_file);
    CHECK(content.find("debug-marker-hidden") == std::string::npos);
    CHECK(content.find("debug-marker-visible") != std::string::npos);

    fs::remove_all(tmp);
}
