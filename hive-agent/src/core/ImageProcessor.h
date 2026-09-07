#pragma once

#include <string>
#include <vector>

/**
 * @brief Image processing utilities for hive-brain
 * 
 * Provides functions for:
 * - Loading local images
 * - Converting images to base64 data URIs
 * - Basic image format detection
 */
class ImageProcessor {
public:
    /**
     * @brief Load a local image file and convert to base64 data URI
     * @param path Path to the image file (relative to project root)
     * @param project_root Project root directory for resolving paths
     * @return Base64 data URI string (e.g., "data:image/jpeg;base64,..."), empty on failure
     */
    static std::string load_local_image(const std::string& path, const std::string& project_root);
    
    /**
     * @brief Load a local image file from absolute path
     * @param absolute_path Absolute path to the image file
     * @return Base64 data URI string, empty on failure
     */
    static std::string load_local_image_absolute(const std::string& absolute_path);
    
    /**
     * @brief Detect image format from file content
     * @param data Pointer to image data
     * @param size Size of data in bytes
     * @return MIME type string (e.g., "image/jpeg", "image/png")
     */
    static std::string detect_format(const unsigned char* data, size_t size);
    
    /**
     * @brief Get MIME type from file extension
     * @param extension File extension (with or without dot)
     * @return MIME type string
     */
    static std::string get_mime_from_extension(const std::string& extension);
    
    /**
     * @brief Check if a file is a supported image
     * @param path Path to the file
     * @return true if supported image format
     */
    static bool is_supported_image(const std::string& path);
    
    /**
     * @brief Read file as binary and return base64 encoded
     * @param path Path to file
     * @return Base64 encoded string, empty on failure
     */
    static std::string read_file_as_base64(const std::string& path);
};
