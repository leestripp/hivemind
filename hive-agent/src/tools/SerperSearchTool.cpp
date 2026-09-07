#include "tools/SerperSearchTool.h"

#include "core/SerperConfig.h"
#include "core/FileIO.h"

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>
#include <utils/Utils.h>

#include <chrono>
#include <string>

// Helper function to get the Serper API key from settings.
// Delegates to the shared SerperConfig helper, which is the single source
// of truth for the lookup (SettingsStore serper_config table).
//
// IMPORTANT: do NOT reimplement the lookup logic here - the helper
// is also called by the startup check and by the unit tests,
// so any change to the lookup algorithm must happen in SerperConfig.cpp.
static std::string get_serper_api_key() {
    return hive::load_serper_api_key().api_key;
}

json SerperSearchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Perform a web search using Google via the Serper.dev API. Returns search results with titles, URLs, and snippets. Requires Serper.dev API key in settings."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"query", json::object({
                        {"type", "string"},
                        {"description", "The search query to perform on Google"}
                    })}
                })},
                {"required", json::array({"query"})}
            })}
        }}
    });
}

json SerperSearchTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;  // Not used for Serper search

    std::string query;

    try {
        if (args.is_object()) {
            query = args.value("query", "");
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            query = parsed_args.value("query", "");
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for serper_search"}});
        }

        if (query.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'query' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    // Get API key
    std::string api_key = get_serper_api_key();
    if (api_key.empty() || api_key == "YOUR_SERPER_API_KEY") {
        return json::object({
            {"status", "error"},
            {"message", "Serper.dev API key not configured. Set it in the SettingsStore "
                        "(daedalus-settings-tui demo or SettingsStore::set_serper_api_key())."}
        });
    }

    try {
        // §NC-4 (2026-07-16): Serper search migrated from libcurl to
        // OmniNet::HTTP::Client. URL encoding via OmniNet::urlEncode();
        // POST via OmniNet::HTTP::Client::send_blocking() with explicit
        // Content-Type + X-API-Key + User-Agent headers.
        std::string encoded_query = OmniNet::Utils::urlEncode(query);

        // Build the URL (the `q=…` parameter is informational; the
        // POST body's `q` field is what Serper actually uses).
        const std::string url = "https://google.serper.dev/search?q=" + encoded_query;

        // Set up the POST body
        json post_body = json::object({
            {"q", query},
            {"num", 10}
        });
        const std::string post_body_str = post_body.dump();

        // Build the request.
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method("POST");
        req.body(post_body_str);
        req.header("Content-Type", "application/json");
        req.header("X-API-Key",     api_key);
        req.header("User-Agent",    "hive-brain/1.0");

        // 30s timeout — matches the old curl setting.
        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(10);
        cfg.request_timeout = std::chrono::seconds(30);
        cfg.follow_redirects = true;
        cfg.verify_tls       = true;

        // Perform the request (sync).
        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

        if (!result.has_value()) {
            return json::object({
                {"status",  "error"},
                {"message", std::string("transport error: ") +
                                OmniNet::HTTP::clientErrorName(result.error())}});
        }

        const auto& resp = result.value();
        const int code = resp.statusCode();
        if (code != 200) {
            return json::object({
                {"status",  "error"},
                {"message", "Serper API returned HTTP " + std::to_string(code)}});
        }

        // Parse the JSON response
        json api_response = json::parse(resp.bodyAsString());

        // Extract results from Serper API format
        json results = json::array();

        if (api_response.contains("organic") && api_response["organic"].is_array()) {
            for (const auto& item : api_response["organic"]) {
                json result_item = json::object();
                result_item["title"] = item.value("title", "");
                result_item["url"] = item.value("link", "");
                result_item["description"] = item.value("snippet", "");

                // Add position if available
                if (item.contains("position")) {
                    result_item["position"] = item["position"];
                }

                results.push_back(result_item);
            }
        }

        // Also include knowledge graph if available
        json extra = json::object();
        if (api_response.contains("knowledgeGraph")) {
            extra["knowledge_graph"] = api_response["knowledgeGraph"];
        }
        if (api_response.contains("answerBox") && !api_response["answerBox"].is_null()) {
            extra["answer_box"] = api_response["answerBox"];
        }

        json return_obj = json::object({
            {"status", "success"},
            {"query", query},
            {"count", results.size()},
            {"results", results}
        });

        if (!extra.empty()) {
            return_obj["extra"] = extra;
        }

        return return_obj;

    } catch (const json::parse_error& e) {
        return json::object({{"status", "error"}, {"message", std::string("Failed to parse Serper response: ") + e.what()}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}
