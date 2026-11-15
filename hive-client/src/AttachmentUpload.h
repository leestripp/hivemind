#pragma once

// ============================================================================
// AttachmentUpload.h — prepare user files for upload; cap large image edges.
// ============================================================================

#include <string>

namespace attachment_upload {

inline constexpr int kMaxImageEdge = 1024;

struct PreparedAttachment {
    std::string filename;
    std::string bytes;
    bool resized = false;
    int width = 0;
    int height = 0;
};

/// Read an attachment off the GTK thread. Images wider or taller than
/// kMaxImageEdge are downscaled proportionally and re-encoded in their
/// original format; smaller images and non-images retain their exact bytes.
/// Throws std::runtime_error on read/decode/encode failure or an empty file.
PreparedAttachment prepare(const std::string& path);

} // namespace attachment_upload
