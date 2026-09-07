#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "tools/BaseTool.h"

class FileIO;

using json = nlohmann::json;

/**
 * PDFTool.h - Contains PDF processing tool classes using Poppler
 */

// ========== ReadPDFTool ==========
class ReadPDFTool : public BaseTool {
public:
    std::string get_name() const override { return "read_pdf"; }
    std::string get_description() const override { return "Extract text content from a PDF file."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
