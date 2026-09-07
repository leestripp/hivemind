#include "core/utils.h"
#include "core/Utf8Utils.h"
#include "core/Logger.h"  // §Phase 5.1: LOG_DEBUG / LOG_WARNING for diagnostic output
#include <iostream>
#include <sstream>
#include <iomanip>
#include <set>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>
#include <cstring>
#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>


// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
// Base64 encoding table
static const char* base64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Base64 decode table
static const int base64_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

std::string base64_encode(const std::string& input) {
    return base64_encode(reinterpret_cast<const unsigned char*>(input.data()), input.size());
}

std::string base64_encode(const unsigned char* data, size_t size) {
    std::string result;
    result.reserve(((size + 2) / 3) * 4);
    
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) n |= static_cast<uint32_t>(data[i + 2]);
        
        result.push_back(base64_chars[(n >> 18) & 0x3F]);
        result.push_back(base64_chars[(n >> 12) & 0x3F]);
        
        if (i + 1 < size) {
            result.push_back(base64_chars[(n >> 6) & 0x3F]);
        } else {
            result.push_back('=');
        }
        
        if (i + 2 < size) {
            result.push_back(base64_chars[n & 0x3F]);
        } else {
            result.push_back('=');
        }
    }
    
    return result;
}

std::string base64_decode(const std::string& input) {
    std::vector<unsigned char> decoded = base64_decode_binary(input);
    return std::string(decoded.begin(), decoded.end());
}

std::vector<unsigned char> base64_decode_binary(const std::string& input) {
    std::vector<unsigned char> result;
    
    // Remove padding characters
    // §fix (2026-07-18): size()-2 underflows when input.size() == 1.
    size_t pad_count = 0;
    if (!input.empty() && input[input.size() - 1] == '=') pad_count++;
    if (input.size() >= 2 && input[input.size() - 2] == '=') pad_count++;
    
    size_t len = input.size() - pad_count;
    result.reserve((len * 3) / 4);
    
    for (size_t i = 0; i < len; i += 4) {
        // §fix (2026-07-18): bounds-check every character — a truncated
        // input (length not a multiple of 4) used to read past the end.
        int n0 = base64_decode_table[static_cast<unsigned char>(input[i])];
        int n1 = (i + 1 < len) ? base64_decode_table[static_cast<unsigned char>(input[i + 1])] : 0;
        int n2 = (i + 2 < len) ? base64_decode_table[static_cast<unsigned char>(input[i + 2])] : 0;
        int n3 = (i + 3 < len) ? base64_decode_table[static_cast<unsigned char>(input[i + 3])] : 0;
        
        uint32_t n = (static_cast<uint32_t>(n0) << 18) |
                     (static_cast<uint32_t>(n1) << 12) |
                     (static_cast<uint32_t>(n2) << 6)  |
                     static_cast<uint32_t>(n3);
        
        result.push_back(static_cast<unsigned char>((n >> 16) & 0xFF));
        if (i + 2 < len) result.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
        if (i + 3 < len) result.push_back(static_cast<unsigned char>(n & 0xFF));
    }
    
    return result;
}

// Supported image extensions
static const std::set<std::string> supported_image_extensions = {
    "jpg", "jpeg", "png", "gif", "webp", "bmp"
};

std::string get_image_extension(const std::string& url_or_path) {
    // Find the last dot
    size_t dot_pos = url_or_path.rfind('.');
    if (dot_pos == std::string::npos || dot_pos == url_or_path.size() - 1) {
        return "";
    }
    
    // Extract extension and convert to lowercase
    std::string ext = url_or_path.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    return ext;
}

bool is_valid_image_url(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    
    // Check for http or https scheme
    bool has_valid_scheme = (url.substr(0, 7) == "http://" || 
                             url.substr(0, 8) == "https://");
    if (!has_valid_scheme) {
        // Also check for data URLs (base64 encoded images)
        if (url.substr(0, 5) == "data:") {
            return true;
        }
        return false;
    }
    
    // Check for valid image extension
    std::string ext = get_image_extension(url);
    if (ext.empty()) {
        return false;
    }
    
    return supported_image_extensions.count(ext) > 0;
}

std::string download_image_as_base64(const std::string& url) {
    if (url.empty()) {
        LOG_DEBUG("[download_image] URL is empty");
        return "";
    }

    // Check if it's already a data URL
    if (url.substr(0, 5) == "data:") {
        // Already base64 encoded, just return as-is
        return url;
    }

    // §NC-5 (2026-07-16): migrated from HttpSession to OmniNet.
    // Sync GET via OmniNet::HTTP::Client::send_blocking. Content-Type
    // is pulled from the response headers map.
    LOG_DEBUG("[download_image] Fetching URL: " + url);

    OmniNet::HTTP::ClientRequest req;
    req.url(url);
    req.method("GET");

    OmniNet::HTTP::ClientConfig cfg;
    // §UA: set the User-Agent on the config, not as a request header —
    // OmniNet always emits its own User-Agent first on the wire, so a
    // request-level header would just be a duplicate second line.
    cfg.user_agent = "hive-brain/1.0";
    cfg.connect_timeout = std::chrono::seconds(10);
    cfg.request_timeout = std::chrono::seconds(30);
    cfg.follow_redirects = true;

    auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

    if (!result.has_value()) {
        LOG_WARNING("[download_image] Request failed for URL: " + url);
        return "";
    }
    const auto& resp = result.value();
    const int code = resp.statusCode();
    if (code < 200 || code >= 300) {
        LOG_WARNING("[download_image] HTTP error: response code " +
                    std::to_string(code) + " for URL: " + url);
        return "";
    }
    const std::string body = resp.bodyAsString();
    if (body.empty()) {
        LOG_WARNING("[download_image] No data received from URL: " + url);
        return "";
    }

    LOG_DEBUG("[download_image] HTTP response code: " + std::to_string(code));
    LOG_DEBUG("[download_image] Data size received: " +
              std::to_string(body.size()) + " bytes");

    // Detect MIME type from URL extension (or use Content-Type from response)
    std::string ext = get_image_extension(url);
    std::string mime_type;
    if (ext == "png") mime_type = "image/png";
    else if (ext == "gif") mime_type = "image/gif";
    else if (ext == "webp") mime_type = "image/webp";
    else if (ext == "bmp") mime_type = "image/bmp";
    else {
        // Use server-provided Content-Type when available; strip any
        // parameters (e.g. "; charset=utf-8") so the data URL stays valid.
        const auto ctype = resp.header("Content-Type");
        if (!ctype.empty()) {
            const auto semi = ctype.find(';');
            mime_type = (semi == std::string::npos) ? ctype
                                                   : ctype.substr(0, semi);
            // Trim trailing whitespace.
            while (!mime_type.empty() &&
                   (mime_type.back() == ' ' || mime_type.back() == '\t'))
                mime_type.pop_back();
        } else {
            mime_type = "image/jpeg"; // Default to JPEG
        }
    }

    // Encode to base64
    std::string base64_data = base64_encode(
        reinterpret_cast<const unsigned char*>(body.data()),
        body.size()
    );

    // Return as data URL
    return "data:" + mime_type + ";base64," + base64_data;
}

// §Phase 5: linenoise helpers removed from the library. The library
// has no concept of where user input comes from — the host owns that
// entirely (see Agent::submit_user_message).
// Hosts that want line-editing + history can bring their own (e.g. drop
// a copy of linenoise into their host's own source tree). The CLI demo
// uses std::getline as the minimal example.

std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.length() && (str[start] == ' ' || str[start] == '\t' || str[start] == '\n' || str[start] == '\r')) {
        start++;
    }
    
    size_t end = str.length();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\n' || str[end - 1] == '\r')) {
        end--;
    }
    
    return str.substr(start, end - start);
}
