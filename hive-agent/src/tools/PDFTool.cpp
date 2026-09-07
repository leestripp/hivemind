#include "PDFTool.h"
#include "core/FileIO.h"

#include <poppler-document.h>
#include <poppler-page.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Check if cwd has been initialized (defined in ToolHandler.cpp)
namespace ToolHandler {
    bool is_cwd_initialized();
    std::filesystem::path get_current_working_directory();
}

// Helper function to convert byte_array to string
static std::string byte_array_to_string(const std::vector<char>& ba) {
    return std::string(ba.begin(), ba.end());
}

// ========== ReadPDFTool ==========

json ReadPDFTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Read a PDF file and extract its text content. "
                           "Returns the text extracted from all pages or a specific page range."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the PDF file (relative to project root or absolute). "
                                       "Example: 'documents/report.pdf'"}
                    })},
                    {"start_page", json::object({
                        {"type", "integer"},
                        {"description", "Starting page number (1-indexed). Default: 1"}
                    })},
                    {"max_pages", json::object({
                        {"type", "integer"},
                        {"description", "Maximum number of pages to extract. Default: 10"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json ReadPDFTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    int start_page = 1;
    int max_pages = 10;
    
    try {
        // Parse arguments
        if (args.is_object() && args.contains("path")) {
            path = args["path"].get<std::string>();
            start_page = args.value("start_page", 1);
            max_pages = args.value("max_pages", 10);
        } else if (args.is_string()) {
            // Handle string arguments (some LLMs send args as string)
            try {
                json parsed_args = json::parse(args.get<std::string>());
                if (parsed_args.contains("path")) {
                    path = parsed_args["path"].get<std::string>();
                    start_page = parsed_args.value("start_page", 1);
                    max_pages = parsed_args.value("max_pages", 10);
                } else {
                    return json::object({{"status", "error"}, {"message", "Missing 'path' in arguments"}});
                }
            } catch (...) {
                // If not JSON, treat as path directly
                path = args.get<std::string>();
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for read_pdf"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        // Resolve the full path
        std::filesystem::path full_path;
        
        // Check if absolute path
        if (std::filesystem::path(path).is_absolute()) {
            full_path = path;
        } else {
            // Relative path - resolve from project root
            full_path = file_io.get_project_path() / path;
        }

        // Check if file exists
        if (!std::filesystem::exists(full_path)) {
            return json::object({
                {"status", "error"},
                {"message", "PDF file not found: " + full_path.string()}
            });
        }

        // Load the PDF document.
        // §FIX-pdf-doc-leak: unique_ptr so the document is also freed on
        // the exception path below — the raw pointer leaked when the
        // catch returned.
        std::unique_ptr<poppler::document> doc(
            poppler::document::load_from_file(full_path.string()));

        if (!doc) {
            return json::object({
                {"status", "error"},
                {"message", "Failed to load PDF file: " + full_path.string()}
            });
        }

        // Check if document is encrypted
        if (doc->is_encrypted()) {
            return json::object({
                {"status", "error"},
                {"path", full_path.string()},
                {"message", "PDF file is encrypted and cannot be read: " + full_path.string()}
            });
        }

        int num_pages = doc->pages();
        if (num_pages == 0) {
            return json::object({
                {"status", "error"},
                {"message", "PDF file has no pages"}
            });
        }

        // Validate and clamp page range
        if (start_page < 1) start_page = 1;
        if (start_page > num_pages) start_page = num_pages;
        
        int end_page = start_page + max_pages - 1;
        if (end_page > num_pages) end_page = num_pages;

        // Extract text from pages using text_list() API
        std::stringstream extracted_text;
        int pages_extracted = 0;
        
        for (int page_num = start_page; page_num <= end_page; ++page_num) {
            poppler::page* page = doc->create_page(page_num - 1);  // Poppler uses 0-based
            
            if (!page) {
                continue;
            }
            
            // Use text_list() to extract text
            std::vector<poppler::text_box> text_boxes = page->text_list();
            
            std::string page_text;
            for (const auto& tb : text_boxes) {
                // to_utf8() returns byte_array (vector<char>)
                page_text += byte_array_to_string(tb.text().to_utf8());
            }
            
            if (!page_text.empty()) {
                extracted_text << "\n--- Page " << page_num << " ---\n";
                extracted_text << page_text;
                pages_extracted++;
            }
            
            delete page;  // Clean up page pointer
        }

        std::string result_text = extracted_text.str();

        if (result_text.empty()) {
            return json::object({
                {"status", "success"},
                {"path", path},
                {"num_pages", num_pages},
                {"pages_extracted", 0},
                {"text", ""},
                {"message", "No text content found in PDF (may be image-based or empty)"}
            });
        }

        return json::object({
            {"status", "success"},
            {"path", path},
            {"num_pages", num_pages},
            {"start_page", start_page},
            {"end_page", end_page},
            {"pages_extracted", pages_extracted},
            {"text", result_text},
            {"message", "Successfully extracted text from " + std::to_string(pages_extracted) + " page(s)"}
        });
        
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"path", path},
            {"message", std::string("Error processing PDF: ") + e.what()}
        });
    }
}
