#include "tools/ReadImageTool.h"

#include "core/ImageProcessor.h"
#include "core/FileIO.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {

// Unscaled passthrough cap. base64 inflates ~4/3x, and the default
// max_request_size_ is 4MB — a 2MB file becomes ~2.7MB on the wire,
// leaving room for the rest of the conversation. PNG takes the
// libpng scaled path (<=512px) so it is unaffected by this cap.
constexpr size_t kMaxPassthroughBytes = 2 * 1024 * 1024;  // 2MB

std::string lower_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

json error_result(const std::string& message) {
    return json::object({{"status", "error"}, {"message", message}});
}

} // namespace

json ReadImageTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description",
             "Read a local image file and attach it to the next request so a "
             "vision-capable model (e.g. Kimi K3) can see it. Supports png and "
             "jpeg (downscaled to 512px) plus gif, bmp and webp (passed "
             "through, 2MB max). SVG is not accepted as an image — read the "
             "SVG source as text instead."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the image file (relative to the project root or absolute)"}
                    })},
                    {"prompt", json::object({
                        {"type", "string"},
                        {"description", "Optional question or instruction about the image, attached alongside it"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json ReadImageTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    std::string prompt;

    try {
        if (args.is_object()) {
            path = args.value("path", "");
            prompt = args.value("prompt", "");
        } else if (args.is_string()) {
            json parsed = json::parse(args.get<std::string>());
            path = parsed.value("path", "");
            prompt = parsed.value("prompt", "");
        } else {
            return error_result("Invalid arguments format for read_image");
        }
    } catch (const std::exception& e) {
        return error_result(std::string("Error parsing arguments: ") + e.what());
    }

    if (path.empty()) {
        return error_result("Missing or empty 'path' argument");
    }

    const std::string ext = lower_extension(path);

    // Kimi rejects SVG image parts (platform.kimi.ai vision docs) — the
    // model can still read SVG markup when it is sent as plain text.
    if (ext == "svg") {
        return error_result(
            "SVG is not supported as image input. Read the SVG file as text "
            "and include its XML source in the message instead.");
    }

    // §JPEG-SCALE (2026-08-10): PNG and JPEG both decode + downscale in
    // ImageProcessor (libpng / libjpeg); gif/bmp/webp have no in-process
    // codec and pass through as-is.
    static const std::vector<std::string> kScaled      = {"png", "jpg", "jpeg"};
    static const std::vector<std::string> kPassthrough = {"gif", "bmp", "webp"};

    const bool is_scaled = std::find(kScaled.begin(), kScaled.end(), ext) != kScaled.end();
    const bool is_passthrough =
        std::find(kPassthrough.begin(), kPassthrough.end(), ext) != kPassthrough.end();

    if (!is_scaled && !is_passthrough) {
        return error_result("Unsupported image format '." + ext +
                            "'. Supported: png, jpeg, gif, bmp, webp.");
    }

    // Resolve against the project root (same rule as ImageProcessor).
    std::filesystem::path resolved = std::filesystem::path(path);
    if (resolved.is_relative()) {
        resolved = file_io.get_project_path() / resolved;
    }

    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) ||
        !std::filesystem::is_regular_file(resolved, ec)) {
        return error_result("Image file not found: " + path);
    }

    const size_t file_size = std::filesystem::file_size(resolved, ec);
    if (ec) {
        return error_result("Cannot stat image file: " + path);
    }

    std::string data_uri;

    if (is_scaled) {
        // libpng/libjpeg decode + downscale to <=512px longest edge (Kimi
        // accepts up to 4K, but 512px keeps vision-token cost low; the docs
        // note higher resolution does not improve understanding).
        if (file_size > 10 * 1024 * 1024) {
            return error_result("Image file too large (max 10MB): " + path);
        }
        data_uri = ImageProcessor::load_local_image(path,
                                                    file_io.get_project_path().string());
        if (data_uri.empty()) {
            return error_result("Failed to decode " + ext + " image: " + path);
        }
    } else {
        if (file_size > kMaxPassthroughBytes) {
            return error_result(
                "Image too large for passthrough (max 2MB for " + ext +
                "; convert to PNG or JPEG for automatic downscaling): " + path);
        }
        data_uri = ImageProcessor::read_file_as_base64(resolved.string());
        if (data_uri.empty()) {
            return error_result("Failed to read image file: " + path);
        }
    }

    json result = json::object({
        {"status", "success"},
        {"path", path},
        {"data_uri", data_uri},
        {"mime_type", data_uri.substr(5, data_uri.find(';') - 5)},
        {"bytes", file_size}
    });
    if (!prompt.empty()) {
        result["prompt"] = prompt;
    }
    result["note"] =
        "The image is attached to the next model request automatically. "
        "The base64 payload is not kept in conversation history.";
    return result;
}
