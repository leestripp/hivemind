/**
 * @file test_read_image_tool.cpp
 * @brief Tests for the core read_image tool (§READ-IMAGE-CORE, 2026-08-10).
 *
 * The tool loads a local image and returns a base64 data URI in the
 * result's "data_uri" field; LLMClient attaches it to the next chat
 * request as an image_url content part (Kimi/OpenAI vision format) and
 * strips the payload from stored history.
 *
 * Format rules (platform.kimi.ai/docs/guide/use-kimi-vision-model):
 *   png            -> libpng decode + downscale to <=512px longest edge
 *   jpeg/gif/bmp/webp -> raw base64 passthrough (2MB cap)
 *   svg            -> rejected (Kimi refuses SVG image parts)
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// libjpeg: the test generates real JPEG fixtures and decodes the tool's
// output to verify the downscaled dimensions (§JPEG-SCALE, 2026-08-10).
#include <cstdio>
#include <jpeglib.h>

#include "core/FileIO.h"
#include "core/ImageProcessor.h"
#include "tools/ToolFactory.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("hive_agent_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// 1x1 transparent PNG (valid, decodable by libpng).
const char* kTinyPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk"
    "YPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

std::vector<unsigned char> base64_decode(const std::string& in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        auto pos = chars.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

void write_file(const fs::path& p, const std::vector<unsigned char>& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Encode a solid-color RGB image as a JPEG file (quality 90).
void write_jpeg(const fs::path& p, int w, int h) {
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = 200; rgb[i + 1] = 120; rgb[i + 2] = 40;
    }
    FILE* fp = fopen(p.string().c_str(), "wb");
    REQUIRE(fp != nullptr);
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = static_cast<JDIMENSION>(w);
    cinfo.image_height = static_cast<JDIMENSION>(h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = rgb.data() + cinfo.next_scanline * w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
}

// Decode a base64 payload (after the "data:...;base64," prefix) and
// return the JPEG dimensions via libjpeg.
std::pair<int, int> jpeg_dimensions(const std::string& data_uri) {
    const std::string prefix = "data:image/jpeg;base64,";
    REQUIRE(data_uri.rfind(prefix, 0) == 0);
    std::vector<unsigned char> bytes = base64_decode(data_uri.substr(prefix.size()));
    REQUIRE(!bytes.empty());
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
    jpeg_read_header(&cinfo, TRUE);
    int w = static_cast<int>(cinfo.image_width);
    int h = static_cast<int>(cinfo.image_height);
    jpeg_destroy_decompress(&cinfo);
    return {w, h};
}

} // namespace

TEST_SUITE("ReadImageTool") {

TEST_CASE("read_image is registered in the default ToolFactory") {
    ToolFactory factory;
    CHECK(factory.has_tool("read_image"));
}

TEST_CASE("read_image definition shape") {
    ToolFactory factory;
    auto defs = factory.get_all_definitions();
    bool found = false;
    for (const auto& d : defs) {
        if (d["function"]["name"] == "read_image") {
            found = true;
            CHECK(d["type"] == "function");
            const auto& params = d["function"]["parameters"];
            CHECK(params["type"] == "object");
            CHECK(params["properties"].contains("path"));
            CHECK(params["properties"].contains("prompt"));
            REQUIRE(params.contains("required"));
            CHECK(params["required"] == json::array({"path"}));
        }
    }
    CHECK(found);
}

TEST_CASE("read_image: missing path argument is an error") {
    ScratchDir dir("read_image_args");
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool("read_image", json::object(), io);
    CHECK(result.value("status", "") == "error");
}

TEST_CASE("read_image: nonexistent file is an error") {
    ScratchDir dir("read_image_missing");
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "nope.png"}}), io);
    CHECK(result.value("status", "") == "error");
    CHECK(result.value("message", "").find("not found") != std::string::npos);
}

TEST_CASE("read_image: SVG is rejected with guidance") {
    ScratchDir dir("read_image_svg");
    write_file(dir.path / "icon.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "icon.svg"}}), io);
    CHECK(result.value("status", "") == "error");
    CHECK(result.value("message", "").find("SVG") != std::string::npos);
}

TEST_CASE("read_image: unsupported extension is an error") {
    ScratchDir dir("read_image_ext");
    write_file(dir.path / "notes.txt", "hello");
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "notes.txt"}}), io);
    CHECK(result.value("status", "") == "error");
}

TEST_CASE("read_image: valid PNG returns a png data URI") {
    ScratchDir dir("read_image_png");
    write_file(dir.path / "dot.png", base64_decode(kTinyPngB64));
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image",
        json::object({{"path", "dot.png"}, {"prompt", "what is this?"}}), io);

    REQUIRE(result.value("status", "") == "success");
    const std::string uri = result.value("data_uri", "");
    CHECK(uri.rfind("data:image/png;base64,", 0) == 0);
    CHECK(result.value("mime_type", "") == "image/png");
    CHECK(result.value("path", "") == "dot.png");
    CHECK(result.value("prompt", "") == "what is this?");
    CHECK(result.contains("bytes"));
}

TEST_CASE("read_image: small jpeg round-trips without scaling") {
    ScratchDir dir("read_image_jpeg_small");
    write_jpeg(dir.path / "small.jpg", 100, 80);  // below the 512px cap
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "small.jpg"}}), io);

    REQUIRE(result.value("status", "") == "success");
    CHECK(result.value("mime_type", "") == "image/jpeg");
    auto [w, h] = jpeg_dimensions(result.value("data_uri", ""));
    CHECK(w == 100);
    CHECK(h == 80);
}

TEST_CASE("read_image: large jpeg is downscaled to 512px longest edge") {
    ScratchDir dir("read_image_jpeg_large");
    write_jpeg(dir.path / "big.jpg", 2000, 1000);  // 2:1 aspect, exceeds cap
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "big.jpg"}}), io);

    REQUIRE(result.value("status", "") == "success");
    auto [w, h] = jpeg_dimensions(result.value("data_uri", ""));
    CHECK(w == 512);   // longest edge clamped
    CHECK(h == 256);   // aspect ratio preserved
}

TEST_CASE("read_image: corrupt jpeg returns a decode error") {
    ScratchDir dir("read_image_jpeg_bad");
    std::vector<unsigned char> junk = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10,
                                       'J', 'F', 'I', 'F', 0x00, 0x01};
    junk.insert(junk.end(), 64, 0x11);  // header only, no scan data
    write_file(dir.path / "broken.jpg", junk);
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "broken.jpg"}}), io);
    CHECK(result.value("status", "") == "error");
}

TEST_CASE("read_image: webp passes through with detected MIME") {
    ScratchDir dir("read_image_webp");
    // RIFF header with WEBP form type (magic bytes only; no decode needed).
    std::vector<unsigned char> webp = {'R', 'I', 'F', 'F', 0x1A, 0x00,
                                       0x00, 0x00, 'W', 'E', 'B', 'P'};
    webp.insert(webp.end(), 16, 0x00);
    write_file(dir.path / "pic.webp", webp);
    FileIO io(dir.path);
    ToolFactory factory;

    json result = factory.execute_tool(
        "read_image", json::object({{"path", "pic.webp"}}), io);

    REQUIRE(result.value("status", "") == "success");
    CHECK(result.value("data_uri", "").rfind("data:image/webp;base64,", 0) == 0);
}

TEST_CASE("ImageProcessor: webp magic-byte and extension detection") {
    std::vector<unsigned char> webp = {'R', 'I', 'F', 'F', 0x1A, 0x00,
                                       0x00, 0x00, 'W', 'E', 'B', 'P'};
    CHECK(ImageProcessor::detect_format(webp.data(), webp.size()) == "image/webp");
    CHECK(ImageProcessor::get_mime_from_extension(".webp") == "image/webp");
    CHECK(ImageProcessor::is_supported_image("a/b/c.webp"));
}

} // TEST_SUITE
