#pragma once

// =============================================================================
// DbUtil.h — shared helpers for the SQLite stores (HiveMind 2.0, Phase 10
// consolidation). Previously copy-pasted into every *Store.cpp.
// All inline — header-only on purpose.
// =============================================================================

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <sqlite3.h>

namespace hivemind::db {

/// RFC4122 v4 UUID from /dev/urandom.
inline std::string uuid_v4() {
    std::array<unsigned char, 16> buf{};
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f) { std::fread(buf.data(), 1, 16, f); std::fclose(f); }
    char out[37];
    std::snprintf(out, sizeof(out),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        buf[0],buf[1],buf[2],buf[3], buf[4],buf[5],
        (buf[6] & 0x0f) | 0x40, buf[7],
        (buf[8] & 0x3f) | 0x80, buf[9], buf[10],buf[11],
        buf[12],buf[13],buf[14],buf[15]);
    return out;
}

/// UTC ISO-8601 "YYYY-MM-DDTHH:MM:SSZ".
inline std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

/// UTC ISO-8601 with microsecond suffix — messages posted in the same
/// second still sort in insertion order (ThreadStore).
inline std::string iso_now_us() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto us = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
    char buf[80];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
    std::snprintf(buf + 19, sizeof(buf) - 19, ".%06lldZ",
                  static_cast<long long>(us));
    return buf;
}

/// Bind a std::string as TEXT.
inline void bind_text(sqlite3_stmt* st, int idx, const std::string& v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

/// Execute a statement that returns no rows; throws std::runtime_error.
/// `what` prefixes the error with the failing store's name.
inline void exec_sql(sqlite3* db, const char* sql, const char* what) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error(std::string{what} + " exec failed: " + msg);
    }
}

/// Column as C-string ("" for NULL).
inline const char* col_text(sqlite3_stmt* st, int i) {
    const auto* p = sqlite3_column_text(st, i);
    return p ? reinterpret_cast<const char*>(p) : "";
}

} // namespace hivemind::db
