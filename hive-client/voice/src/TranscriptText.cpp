#include "TranscriptText.h"

#include <cctype>

namespace hv {
namespace {

std::string trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() &&
           std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    std::size_t last = text.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return std::string(text.substr(first, last - first));
}

bool isSilenceArtifact(std::string_view part) {
    std::string norm;
    norm.reserve(part.size());
    for (const unsigned char c : part) {
        if (std::isalnum(c)) norm += static_cast<char>(std::tolower(c));
    }
    return norm.empty() || norm == "blankaudio" || norm == "silence";
}

}  // namespace

std::string sanitizeSttTranscript(std::string_view text) {
    const std::string original = trim(text);
    if (original.empty()) return {};

    std::string cleaned;
    bool removed_artifact = false;
    std::size_t i = 0;
    while (i < original.size()) {
        while (i < original.size() &&
               std::isspace(static_cast<unsigned char>(original[i]))) ++i;
        if (i >= original.size()) break;
        std::size_t end = i;
        while (end < original.size()) {
            const char c = original[end++];
            if (c == '.' || c == '!' || c == '?' || c == '\n') break;
        }
        const std::string part = trim(
            std::string_view(original).substr(i, end - i));
        if (!part.empty()) {
            if (isSilenceArtifact(part)) {
                removed_artifact = true;
            } else {
                if (!cleaned.empty()) cleaned += ' ';
                cleaned += part;
            }
        }
        i = end;
    }

    // Preserve the transcript byte-for-byte when no artifact was removed.
    // Multiple sentences and newlines still travel in this ONE message.
    return removed_artifact ? cleaned : original;
}

}  // namespace hv
