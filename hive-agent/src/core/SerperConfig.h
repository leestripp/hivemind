#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace hive {

/**
 * @brief Result of looking up the Serper.dev API key from settings.json.
 *
 * This struct is the single source of truth for the agent's view of whether
 * the Serper search tool is properly configured. It is populated by
 * load_serper_api_key() and consumed by:
 *   - SerperSearchTool::execute() (to fetch the key at request time)
 *   - tests (to assert configuration correctness)
 *
 * Status values are deliberately explicit (not just bool) so the caller can
 * produce a precise, actionable error message and so the unit test can
 * distinguish "file missing" from "key empty" from "key is a placeholder".
 */
struct SerperApiKeyResult {
    enum class Status {
        OK,                      //!< Key found and looks valid
        MISSING_FILE,            //!< No settings.json in any of the search paths
        MISSING_BLOCK,           //!< File present but no "serper-search"/"serper" key
        MISSING_API_KEY_FIELD,   //!< Block present but no "api_key" inside it
        EMPTY_API_KEY,           //!< "api_key" present but is the empty string
        PLACEHOLDER_API_KEY,     //!< "api_key" is the default placeholder (e.g. YOUR_SERPER_API_KEY)
        INVALID_API_KEY_FORMAT,  //!< "api_key" present but doesn't look like a valid Serper key
        JSON_PARSE_ERROR,        //!< settings.json exists but is not valid JSON
        IO_ERROR,                //!< File exists but couldn't be read
    };

    Status status = Status::MISSING_FILE;
    std::string api_key;                              //!< Populated only when status == OK
    std::filesystem::path settings_path;              //!< The path that produced the result (existing or first-tried)
    std::string error_detail;                         //!< Human-readable detail for non-OK statuses

    bool ok() const { return status == Status::OK; }

    //! Short status tag, suitable for logs and assertion messages.
    const char* status_name() const;
};

//! Stream output for SerperApiKeyResult::Status - prints the status_name()
//! so test failures and logs show the enum value rather than an integer.
inline std::ostream& operator<<(std::ostream& os,
                                SerperApiKeyResult::Status s) {
    SerperApiKeyResult r;
    r.status = s;
    return os << r.status_name();
}

/**
 * @brief Load and validate the Serper API key from the SettingsStore.
 *
 * §v0.5.0+: the SQLite SettingsStore (`serper_config` table, singleton row
 * id=1, in `$XDG_DATA_HOME/hive-brain/daedalus.db`) is the sole source of
 * truth for the key. Legacy settings.json / config.json files are no
 * longer consulted.
 *
 * The returned result has `settings_path` set to the SettingsStore path
 * that produced the result.
 *
 * @return A SerperApiKeyResult with status set appropriately.
 */
SerperApiKeyResult load_serper_api_key();

/**
 * @brief Print a one-line, human-readable startup status for the Serper key.
 *
 * Designed to be called once at agent startup. Behaviour:
 *   - On OK:           "[SERPER] API key loaded (serper-search, prefix e44f07...)"
 *   - On error:        "[SERPER] WARNING: <status_name>: <error_detail>"
 *                      and a follow-up line telling the user exactly which
 *                      file to edit and what JSON to add.
 *
 * @return true iff the key is configured and valid.
 */
bool check_serper_api_key_at_startup();

//! Returns true if `key` looks like a plausible Serper.dev API key.
//! Serper keys are 40 lowercase hex characters. We accept any string of
//! 32+ hex characters to be forgiving of future format changes; we reject
//! anything with whitespace, quotes, or placeholder-shaped values.
bool looks_like_valid_serper_key(const std::string& key);

}  // namespace hive
