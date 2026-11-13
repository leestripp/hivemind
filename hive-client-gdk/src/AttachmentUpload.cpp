#include "AttachmentUpload.h"

#include <gdkmm/pixbuf.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace attachment_upload {
namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read: " + path);
    std::string bytes{std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>()};
    if (bytes.empty())
        throw std::runtime_error("cannot attach an empty file: " + path);
    return bytes;
}

std::string lowercase_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string image_encoder(const std::string& ext) {
    if (ext == ".png") return "png";
    if (ext == ".jpg" || ext == ".jpeg") return "jpeg";
    if (ext == ".gif") return "gif";
    if (ext == ".webp") return "webp";
    if (ext == ".bmp") return "bmp";
    return {};
}

} // namespace

PreparedAttachment prepare(const std::string& path) {
    PreparedAttachment result;
    result.filename = std::filesystem::path(path).filename().string();
    const std::string encoder = image_encoder(lowercase_extension(path));
    if (encoder.empty()) {
        result.bytes = read_file(path);
        return result;
    }

    try {
        auto image = Gdk::Pixbuf::create_from_file(path);
        if (!image) throw std::runtime_error("no decoded image");
        result.width = image->get_width();
        result.height = image->get_height();
        if (result.width <= 0 || result.height <= 0)
            throw std::runtime_error("invalid image dimensions");

        if (std::max(result.width, result.height) <= kMaxImageEdge) {
            result.bytes = read_file(path);
            return result;
        }

        const double scale =
            static_cast<double>(kMaxImageEdge) /
            std::max(result.width, result.height);
        const int width = std::max(1, static_cast<int>(std::lround(
            result.width * scale)));
        const int height = std::max(1, static_cast<int>(std::lround(
            result.height * scale)));
        auto scaled = image->scale_simple(width, height,
                                          Gdk::InterpType::BILINEAR);
        if (!scaled) throw std::runtime_error("image scaling failed");

        gchar* buffer = nullptr;
        gsize buffer_size = 0;
        if (encoder == "jpeg")
            scaled->save_to_buffer(buffer, buffer_size, encoder,
                                   {"quality"}, {"90"});
        else
            scaled->save_to_buffer(buffer, buffer_size, encoder);
        std::unique_ptr<gchar, decltype(&g_free)> owner(buffer, &g_free);
        result.bytes.assign(buffer, buffer_size);
        result.resized = true;
        result.width = width;
        result.height = height;
        return result;
    } catch (const Glib::Error& e) {
        throw std::runtime_error("cannot prepare image " + path + ": " +
                                 e.what());
    }
}

} // namespace attachment_upload
