// SPDX-License-Identifier: BSL-1.0
// src/core/SerperConfig.cpp
//
// v0.5.0+: Serper API key is stored in the SQLite SettingsStore
// (serper_config table). This file reads ONLY from the store —
// there is no JSON file fallback at runtime.

#include "core/SerperConfig.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include "library/paths.h"
#include "core/settings.h"
#include "core/Logger.h"

#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)

namespace hive {

namespace fs = std::filesystem;

bool looks_like_valid_serper_key(const std::string& key) {
    if (key.empty()) return false;
    // Disallow whitespace, quotes, and JSON structural characters - a
    // valid Serper key is just hex.
    for (char c : key) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
        if (c == '"' || c == '\'' || c == '{' || c == '}' ||
            c == '[' || c == ']' || c == ',' || c == ':') return false;
    }
    // Require at least 32 hex characters (Serper keys are 40, but be
    // forgiving so a future format change doesn't fail this test).
    size_t hex_count = 0;
    for (char c : key) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            ++hex_count;
        } else {
            return false;  // any non-hex char disqualifies
        }
    }
    return hex_count >= 32;
}

const char* SerperApiKeyResult::status_name() const {
    switch (status) {
        case Status::OK:                     return "OK";
        case Status::MISSING_FILE:          return "MISSING_FILE";
        case Status::MISSING_BLOCK:         return "MISSING_BLOCK";
        case Status::MISSING_API_KEY_FIELD: return "MISSING_API_KEY_FIELD";
        case Status::EMPTY_API_KEY:         return "EMPTY_API_KEY";
        case Status::PLACEHOLDER_API_KEY:   return "PLACEHOLDER_API_KEY";
        case Status::INVALID_API_KEY_FORMAT: return "INVALID_API_KEY_FORMAT";
        case Status::JSON_PARSE_ERROR:      return "JSON_PARSE_ERROR";
        case Status::IO_ERROR:              return "IO_ERROR";
    }
    return "UNKNOWN";
}

SerperApiKeyResult load_serper_api_key() {
    // §v0.5.0+: SettingsStore is the sole source of truth.
    // The serper api_key lives in the serper_config SQLite table.
    const auto& p = hive::Paths::global();
    fs::path db_dir = p.user_data_root();
    fs::path db_path = db_dir / "hive-agent.db";

    SerperApiKeyResult r;
    r.settings_path = db_path;

    std::error_code ec;
    if (!fs::exists(db_path, ec)) {
        r.status = SerperApiKeyResult::Status::MISSING_FILE;
        r.error_detail = "SettingsStore not found at " + db_path.string()
                       + " (serper_config table).";
        return r;
    }

    try {
        auto store = hive::SettingsStore::for_home(db_dir);
        std::string key = store.get_serper_api_key();

        if (key.empty()) {
            r.status = SerperApiKeyResult::Status::MISSING_BLOCK;
            r.error_detail = "No Serper API key in SettingsStore at "
                             + db_path.string();
            return r;
        }
        if (!looks_like_valid_serper_key(key)) {
            r.status = SerperApiKeyResult::Status::INVALID_API_KEY_FORMAT;
            r.error_detail = "Serper API key in SettingsStore does not look valid";
            return r;
        }
        r.status = SerperApiKeyResult::Status::OK;
        r.api_key = std::move(key);
        return r;
    } catch (const std::exception& e) {
        r.status = SerperApiKeyResult::Status::IO_ERROR;
        r.error_detail = std::string("SettingsStore read failed: ") + e.what();
        return r;
    }
}

bool check_serper_api_key_at_startup() {
    SerperApiKeyResult r = load_serper_api_key();

    if (r.ok()) {
        LOG_INFO("[SERPER] API key loaded from "
                 + r.settings_path.string());
        return true;
    }

    LOG_CERR(INFO, "\n[SERPER] WARNING: " << r.status_name() << ": "
              << r.error_detail);
    LOG_CERR(INFO, "[SERPER] The serper_search tool will fail at request time "
              << "until this is fixed.");
    LOG_CERR(INFO, "[SERPER] Add the key via: daedalus-cli --model-list "
              << "(or set it in your settings store directly).");
    LOG_CERR(INFO, '\n');
    return false;
}

}  // namespace hive
