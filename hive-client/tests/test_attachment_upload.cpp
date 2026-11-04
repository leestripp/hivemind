// Image-upload preparation: 4K screenshots are capped at 1024 on their
// longest edge, while small images and non-image files keep their bytes.
#include "AttachmentUpload.h"

#include <gdkmm/pixbuf.h>
#include <gtkmm/application.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void check_dimensions(const std::filesystem::path& dir,
                      const attachment_upload::PreparedAttachment& prepared,
                      int width, int height) {
    const auto output = dir / "decoded.png";
    {
        std::ofstream out(output, std::ios::binary);
        out.write(prepared.bytes.data(),
                  static_cast<std::streamsize>(prepared.bytes.size()));
    }
    auto decoded = Gdk::Pixbuf::create_from_file(output.string());
    assert(decoded);
    assert(decoded->get_width() == width);
    assert(decoded->get_height() == height);
}

} // namespace

int main() {
    // gtkmm's GdkPixbuf wrappers are registered by Application construction;
    // a bare test binary otherwise trips Glib::wrap_create_new_wrapper().
    auto app = Gtk::Application::create("org.hivemind.AttachmentUploadTest");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hive-attachment-upload-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const auto large_path = dir / "4k.png";
    auto large = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8,
                                     3840, 2160);
    large->fill(0x336699ff);
    large->save(large_path.string(), "png");
    const auto large_result = attachment_upload::prepare(large_path.string());
    assert(large_result.resized);
    assert(large_result.filename == "4k.png");
    assert(large_result.width == 1024 && large_result.height == 576);
    check_dimensions(dir, large_result, 1024, 576);

    const auto portrait_path = dir / "portrait.png";
    auto portrait = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8,
                                        1000, 3000);
    portrait->fill(0x993366ff);
    portrait->save(portrait_path.string(), "png");
    const auto portrait_result = attachment_upload::prepare(portrait_path.string());
    assert(portrait_result.resized);
    assert(portrait_result.width == 341 && portrait_result.height == 1024);
    check_dimensions(dir, portrait_result, 341, 1024);

    const auto small_path = dir / "small.png";
    auto small = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 800, 400);
    small->fill(0x669933ff);
    small->save(small_path.string(), "png");
    const auto small_result = attachment_upload::prepare(small_path.string());
    assert(!small_result.resized);
    assert(small_result.width == 800 && small_result.height == 400);
    assert(small_result.bytes == read_bytes(small_path));

    const auto text_path = dir / "notes.txt";
    {
        std::ofstream out(text_path, std::ios::binary);
        out << "leave non-image bytes unchanged\n";
    }
    const auto text_result = attachment_upload::prepare(text_path.string());
    assert(!text_result.resized);
    assert(text_result.bytes == read_bytes(text_path));

    const auto empty_path = dir / "empty.bin";
    { std::ofstream out(empty_path); }
    bool empty_rejected = false;
    try {
        (void)attachment_upload::prepare(empty_path.string());
    } catch (const std::runtime_error&) {
        empty_rejected = true;
    }
    assert(empty_rejected);

    fs::remove_all(dir);
    std::cout << "test_attachment_upload OK\n";
    return 0;
}
