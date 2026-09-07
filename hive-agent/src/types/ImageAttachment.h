/**
 * @file hive-brain/ImageAttachment.h
 * @brief Image attached to a user message (multi-modal input).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hive {

struct ImageAttachment {
    enum class Source { Path, Url, Bytes };
    Source source = Source::Path;
    std::string path_or_url;             ///< For Path / Url
    std::vector<std::uint8_t> bytes;     ///< For Bytes
    std::string mime_type;               ///< "image/png", "image/jpeg", ...
};

}  // namespace hive