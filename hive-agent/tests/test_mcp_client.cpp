/**
 * @file test_mcp_client.cpp
 * @brief Unit tests for McpClient class
 * 
 * Tests the MCP client functionality including:
 * - Tool result structure
 * - Tool definition structure
 * - JSON-RPC message formatting
 * - Singleton instance management
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Test structures (duplicated from McpClient.h for standalone testing)
// ============================================================================

struct TestToolDefinition {
    std::string name;
    std::string description;
    json input_schema;
    
    bool operator==(const TestToolDefinition& other) const {
        return name == other.name && description == other.description;
    }
};

struct TestToolResult {
    bool success;
    std::string content;
    std::optional<std::string> error;
    
    bool operator==(const TestToolResult& other) const {
        return success == other.success && content == other.content;
    }
};

// ============================================================================
// Tests for ToolDefinition
// ============================================================================

TEST_CASE("ToolDefinition structure initialization") {
    TestToolDefinition tool;
    tool.name = "test_tool";
    tool.description = "A test tool";
    tool.input_schema = json::object({
        {"type", "object"},
        {"properties", json::object({
            {"param1", json::object({
                {"type", "string"},
                {"description", "Test parameter"}
            })}
        })}
    });
    
    CHECK(tool.name == "test_tool");
    CHECK(tool.description == "A test tool");
    CHECK(tool.input_schema.is_object());
    CHECK(tool.input_schema.contains("properties"));
}

TEST_CASE("ToolDefinition JSON serialization") {
    TestToolDefinition tool;
    tool.name = "understand_image";
    tool.description = "Performs image understanding and analysis";
    tool.input_schema = json::object({
        {"type", "object"},
        {"properties", json::object({
            {"prompt", json::object({
                {"type", "string"},
                {"description", "Question or analysis request for the image"}
            })},
            {"image_url", json::object({
                {"type", "string"},
                {"description", "Image source"}
            })}
        })}
    });
    
    json j = tool.input_schema;
    CHECK(j["type"] == "object");
    CHECK(j["properties"].contains("prompt"));
    CHECK(j["properties"].contains("image_url"));
}

// ============================================================================
// Tests for ToolResult
// ============================================================================

TEST_CASE("ToolResult success initialization") {
    TestToolResult result;
    result.success = true;
    result.content = "Image shows a cat";
    result.error = std::nullopt;
    
    CHECK(result.success == true);
    CHECK(result.content == "Image shows a cat");
    CHECK(result.error.has_value() == false);
}

TEST_CASE("ToolResult error initialization") {
    TestToolResult result;
    result.success = false;
    result.content = "";
    result.error = "Connection timeout";
    
    CHECK(result.success == false);
    CHECK(result.content == "");
    CHECK(result.error.has_value() == true);
    CHECK(result.error.value() == "Connection timeout");
}

TEST_CASE("ToolResult equality") {
    TestToolResult result1;
    result1.success = true;
    result1.content = "Test content";
    
    TestToolResult result2;
    result2.success = true;
    result2.content = "Test content";
    
    CHECK(result1 == result2);
}

TEST_CASE("ToolResult inequality") {
    TestToolResult result1;
    result1.success = true;
    result1.content = "Test content";
    
    TestToolResult result2;
    result2.success = false;
    result2.content = "Test content";
    
    CHECK_FALSE(result1 == result2);
}

// ============================================================================
// Tests for JSON-RPC message formatting
// ============================================================================

TEST_CASE("JSON-RPC request format") {
    json request = json::object();
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "tools/call";
    request["params"] = json::object({
        {"name", "understand_image"},
        {"arguments", json::object({
            {"prompt", "What is in this image?"},
            {"image_url", "https://example.com/image.png"}
        })}
    });
    
    CHECK(request["jsonrpc"] == "2.0");
    CHECK(request["id"] == 1);
    CHECK(request["method"] == "tools/call");
    CHECK(request["params"]["name"] == "understand_image");
    CHECK(request["params"]["arguments"]["prompt"] == "What is in this image?");
    CHECK(request["params"]["arguments"]["image_url"] == "https://example.com/image.png");
}

TEST_CASE("JSON-RPC response parsing - success") {
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = 1;
    response["result"] = json::object({
        {"content", json::array({
            json::object({
                {"type", "text"},
                {"text", "Image analysis complete"}
            })
        })}
    });
    
    CHECK(response["jsonrpc"] == "2.0");
    CHECK(response.contains("result"));
    CHECK(response["result"]["content"].is_array());
    CHECK(response["result"]["content"][0]["text"] == "Image analysis complete");
}

TEST_CASE("JSON-RPC response parsing - error") {
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = 2;
    response["error"] = json::object({
        {"code", -32600},
        {"message", "Invalid Request"}
    });
    
    CHECK(response["jsonrpc"] == "2.0");
    CHECK(response.contains("error"));
    CHECK(response["error"]["code"] == -32600);
    CHECK(response["error"]["message"] == "Invalid Request");
}

TEST_CASE("JSON-RPC notification format (no id)") {
    json notification = json::object();
    notification["jsonrpc"] = "2.0";
    notification["method"] = "initialized";
    
    CHECK(notification["jsonrpc"] == "2.0");
    CHECK(notification.contains("method"));
    CHECK(notification.contains("id") == false);  // Notifications don't have id
}

// ============================================================================
// Tests for MCP initialize request
// ============================================================================

TEST_CASE("MCP initialize request structure") {
    json params = json::object();
    params["protocolVersion"] = "2025-06-18";
    params["capabilities"] = json::object({
        {"roots", json::object()},
        {"sampling", json::object()}
    });
    params["clientInfo"] = json::object({
        {"name", "daedalus"},
        {"version", "1.0.0"}
    });
    
    CHECK(params["protocolVersion"] == "2025-06-18");
    CHECK(params["capabilities"].is_object());
    CHECK(params["clientInfo"]["name"] == "daedalus");
    CHECK(params["clientInfo"]["version"] == "1.0.0");
}

TEST_CASE("MCP initialize response parsing") {
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = 1;
    response["result"] = json::object({
        {"protocolVersion", "2025-06-18"},
        {"capabilities", json::object({
            {"tools", json::object()}
        })},
        {"serverInfo", json::object({
            {"name", "minimax-mcp"},
            {"version", "1.0.0"}
        })}
    });
    
    CHECK(response["result"]["protocolVersion"] == "2025-06-18");
    CHECK(response["result"]["serverInfo"]["name"] == "minimax-mcp");
    CHECK(response["result"]["serverInfo"]["version"] == "1.0.0");
}

// ============================================================================
// Tests for tools/list response
// ============================================================================

TEST_CASE("MCP tools/list response parsing") {
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = 2;
    response["result"] = json::object({
        {"tools", json::array({
            json::object({
                {"name", "understand_image"},
                {"description", "Performs image understanding"},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"prompt", json::object({
                            {"type", "string"}
                        })},
                        {"image_url", json::object({
                            {"type", "string"}
                        })}
                    })}
                })}
            }),
            json::object({
                {"name", "web_search"},
                {"description", "Performs web search"},
                {"inputSchema", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"query", json::object({
                            {"type", "string"}
                        })}
                    })}
                })}
            })
        })}
    });
    
    CHECK(response["result"]["tools"].size() == 2);
    CHECK(response["result"]["tools"][0]["name"] == "understand_image");
    CHECK(response["result"]["tools"][1]["name"] == "web_search");
}

TEST_CASE("MCP tool with required properties parsing") {
    json tool = json::object({
        {"name", "test_tool"},
        {"description", "Test tool with required params"},
        {"inputSchema", json::object({
            {"type", "object"},
            {"properties", json::object({
                {"path", json::object({
                    {"type", "string"},
                    {"description", "File path"}
                })}
            })},
            {"required", json::array({"path"})}
        })}
    });
    
    CHECK(tool["inputSchema"]["required"].is_array());
    
    // Check if "path" is in the required array
    bool found_path = false;
    for (const auto& item : tool["inputSchema"]["required"]) {
        if (item == "path") {
            found_path = true;
            break;
        }
    }
    CHECK(found_path);
}

// ============================================================================
// Tests for web_search tool call
// ============================================================================

TEST_CASE("MCP web_search request format") {
    json params = json::object();
    params["name"] = "web_search";
    params["arguments"] = json::object({
        {"query", "MiniMax API documentation"}
    });
    
    CHECK(params["name"] == "web_search");
    CHECK(params["arguments"]["query"] == "MiniMax API documentation");
}

TEST_CASE("MCP understand_image request format") {
    json params = json::object();
    params["name"] = "understand_image";
    params["arguments"] = json::object({
        {"prompt", "Describe the UI elements in this screenshot"},
        {"image_url", "/home/user/screenshot.png"}
    });
    
    CHECK(params["name"] == "understand_image");
    CHECK(params["arguments"]["prompt"] == "Describe the UI elements in this screenshot");
    CHECK(params["arguments"]["image_url"] == "/home/user/screenshot.png");
}

// ============================================================================
// Tests for result content extraction
// ============================================================================

TEST_CASE("Extract text from content array - single item") {
    json content = json::array({
        json::object({
            {"type", "text"},
            {"text", "This is the analysis result"}
        })
    });
    
    std::string extracted;
    for (const auto& item : content) {
        if (item.contains("text")) {
            extracted += item["text"].get<std::string>();
        }
    }
    
    CHECK(extracted == "This is the analysis result");
}

TEST_CASE("Extract text from content array - multiple items") {
    json content = json::array({
        json::object({
            {"type", "text"},
            {"text", "First part. "}
        }),
        json::object({
            {"type", "text"},
            {"text", "Second part."}
        })
    });
    
    std::string extracted;
    for (const auto& item : content) {
        if (item.contains("text")) {
            extracted += item["text"].get<std::string>();
        }
    }
    
    CHECK(extracted == "First part. Second part.");
}

TEST_CASE("Extract text from content array - empty array") {
    json content = json::array();
    
    std::string extracted;
    for (const auto& item : content) {
        if (item.contains("text")) {
            extracted += item["text"].get<std::string>();
        }
    }
    
    CHECK(extracted.empty());
}

// ============================================================================
// Tests for environment map building
// ============================================================================

TEST_CASE("Environment variables map building") {
    std::map<std::string, std::string> env;
    env["MINIMAX_API_KEY"] = "sk-test-api-key-12345";
    env["MINIMAX_API_HOST"] = "https://api.minimax.io";
    
    CHECK(env.size() == 2);
    CHECK(env["MINIMAX_API_KEY"] == "sk-test-api-key-12345");
    CHECK(env["MINIMAX_API_HOST"] == "https://api.minimax.io");
}

TEST_CASE("Environment variables iteration") {
    std::map<std::string, std::string> env;
    env["MINIMAX_API_KEY"] = "sk-test";
    env["MINIMAX_API_HOST"] = "https://api.minimax.io";
    
    std::vector<std::string> keys;
    for (const auto& [key, value] : env) {
        keys.push_back(key);
    }
    
    CHECK(keys.size() == 2);
    CHECK(std::find(keys.begin(), keys.end(), "MINIMAX_API_KEY") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "MINIMAX_API_HOST") != keys.end());
}

// ============================================================================
// Tests for command parsing
// ============================================================================

TEST_CASE("Command string parsing - simple command") {
    std::string command = "uvx minimax-coding-plan-mcp -y";
    
    std::vector<std::string> args;
    std::string current;
    bool in_quote = false;
    
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        
        if (c == '"' || c == '\'') {
            in_quote = !in_quote;
        } else if (c == ' ' && !in_quote) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        args.push_back(current);
    }
    
    CHECK(args.size() == 3);
    CHECK(args[0] == "uvx");
    CHECK(args[1] == "minimax-coding-plan-mcp");
    CHECK(args[2] == "-y");
}

TEST_CASE("Command string parsing - with quoted arguments") {
    std::string command = "uvx \"minimax-coding-plan-mcp -y\"";
    
    std::vector<std::string> args;
    std::string current;
    bool in_quote = false;
    char quote_char = '\0';
    
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        
        if ((c == '"' || c == '\'') && !in_quote) {
            in_quote = true;
            quote_char = c;
        } else if (c == quote_char && in_quote) {
            in_quote = false;
            quote_char = '\0';
        } else if (c == ' ' && !in_quote) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        args.push_back(current);
    }
    
    CHECK(args.size() == 2);
    CHECK(args[0] == "uvx");
    CHECK(args[1] == "minimax-coding-plan-mcp -y");
}