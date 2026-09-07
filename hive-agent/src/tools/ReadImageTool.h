#pragma once

#include "BaseTool.h"
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * ReadImageTool - Load a local image so a vision-capable model can see it.
 * Usage: read_image(path, prompt?)
 *
 * The tool loads the image, converts it to a base64 data URI, and returns
 * it in the tool result's "data_uri" field. LLMClient then attaches the
 * image as an "image_url" content part on the next chat request (the
 * format vision APIs such as Kimi K3 / moonshot-v1-*-vision expect), and
 * strips the bulky data URI from the stored history so it does not
 * consume context on subsequent turns.
 *
 * Format support follows the Kimi vision docs
 * (platform.kimi.ai/docs/guide/use-kimi-vision-model):
 *   - png / jpeg : decoded (libpng / libjpeg) and scaled to <=512px
 *            longest edge, re-encoded (keeps token cost low; Kimi
 *            accepts up to 4K)
 *   - gif / bmp / webp : passed through as base64 (no codec available
 *            in-process), capped at 2MB so the request stays under the
 *            4MB default request-size limit
 *   - svg  : rejected (Kimi rejects SVG image parts; the XML must be
 *            sent as text instead)
 */
class ReadImageTool : public BaseTool {
public:
    std::string get_name() const override { return "read_image"; }
    std::string get_description() const override {
        return "Read a local image file so the model can see it (requires a vision-capable model)";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
