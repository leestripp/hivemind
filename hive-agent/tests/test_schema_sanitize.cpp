/**
 * @file test_schema_sanitize.cpp
 * @brief Unit tests for mcp::sanitize_tool_schema (2026-09-30, 4th schema
 *        incident — comfy-mcp's internal-$ref schema silently muted Bob:
 *        Moonshot 400'd every request, deliveries recorded done:true with
 *        empty replies). Uses the REAL captured comfy-mcp set_workflow_slot
 *        schema as the central fixture.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mcp/SchemaSanitize.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace {

bool dump_contains(const json& j, const std::string& needle) {
    return j.dump().find(needle) != std::string::npos;
}

// The exact schema comfy-mcp advertised for set_workflow_slot on
// 2026-09-30 (captured live from Bob's container). Moonshot's verdict on
// the original: "At path 'properties.overrides.items.anyOf': detected
// infinite recursion without termination condition".
const char* kRealComfySlotSchema = R"JSON({
 "properties": {
  "workflow_path": {"title": "Workflow Path", "type": "string"},
  "overrides": {
   "items": {"anyOf": [{"type": "string"}, {"$ref": "#/$defs/SlotOverride"}]},
   "title": "Overrides", "type": "array"
  },
  "stdout": {"default": true, "title": "Stdout", "type": "boolean"}
 },
 "required": ["workflow_path", "overrides"],
 "type": "object",
 "$defs": {
  "SlotOverride": {
   "description": "One set_workflow_slot override, as structured data.",
   "properties": {
    "address": {"description": "Slot address, e.g. '6.text'.", "title": "Address", "type": "string"},
    "value": {"description": "The value to set, as JSON data.", "title": "Value"}
   },
   "required": ["address", "value"],
   "title": "SlotOverride",
   "type": "object"
  }
 },
 "title": "set_workflow_slotArguments"
})JSON";

} // namespace

TEST_SUITE("schema sanitize") {

    TEST_CASE("real comfy-mcp schema: internal $ref is inlined, $defs stripped") {
        json schema = json::parse(kRealComfySlotSchema);
        CHECK(mcp::sanitize_tool_schema(schema));

        CHECK(!dump_contains(schema, "$ref"));
        CHECK(!dump_contains(schema, "$defs"));
        CHECK(!dump_contains(schema, "definitions"));

        // The inlined SlotOverride keeps its guidance: the second anyOf
        // option is now a concrete object schema, not a bare {}.
        const json& opt = schema["properties"]["overrides"]["items"]["anyOf"][1];
        CHECK(opt.value("type", "") == "object");
        CHECK(opt["properties"].contains("address"));
        CHECK(opt["properties"].contains("value"));
        CHECK(opt["properties"]["address"].value("type", "") == "string");

        // Untouched parts survive intact.
        CHECK(schema["properties"]["workflow_path"].value("type", "") == "string");
        CHECK(schema["properties"]["stdout"].value("default", false) == true);
        CHECK(schema["required"].size() == 2);
    }

    TEST_CASE("clean schema is left untouched") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {
                "prompt": {"type": "string"},
                "count": {"type": "integer", "default": 1}
            },
            "required": ["prompt"]
        })");
        const json before = schema;
        CHECK(!mcp::sanitize_tool_schema(schema));
        CHECK(schema == before);
    }

    TEST_CASE("true internal ref cycle degrades to permissive object") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {"node": {"$ref": "#/$defs/Node"}},
            "$defs": {
                "Node": {
                    "type": "object",
                    "properties": {"next": {"$ref": "#/$defs/Node"}}
                }
            }
        })");
        CHECK(mcp::sanitize_tool_schema(schema));
        CHECK(!dump_contains(schema, "$ref"));
        CHECK(!dump_contains(schema, "$defs"));
        // The cycle point is degraded; the first inline level survives.
        const json& node = schema["properties"]["node"];
        CHECK(node.value("type", "") == "object");
        CHECK(node["properties"]["next"].empty());
    }

    TEST_CASE("external ref degrades") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {"x": {"$ref": "https://example.com/schemas/x.json"}}
        })");
        CHECK(mcp::sanitize_tool_schema(schema));
        CHECK(schema["properties"]["x"].empty());
    }

    TEST_CASE("ref with sibling keys degrades (merge semantics refused)") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {"x": {"$ref": "#/definitions/Y", "description": "sibling"}},
            "definitions": {"Y": {"type": "string"}}
        })");
        CHECK(mcp::sanitize_tool_schema(schema));
        CHECK(schema["properties"]["x"].empty());
        CHECK(!dump_contains(schema, "definitions"));
    }

    TEST_CASE("dangling ref degrades") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {"x": {"$ref": "#/$defs/Missing"}}
        })");
        CHECK(mcp::sanitize_tool_schema(schema));
        CHECK(schema["properties"]["x"].empty());
    }

    TEST_CASE("over-deep nesting is cut") {
        // Build a 40-deep properties chain programmatically.
        json leaf = {{"type", "string"}};
        json schema = leaf;
        for (int i = 0; i < 40; ++i)
            schema = {{"type", "object"},
                      {"properties", {{"child", schema}}}};
        CHECK(mcp::sanitize_tool_schema(schema));
        // Walk down: the chain must have been cut well below 40 levels
        // (resolve cap first, structural cap second — either proves the
        // schema cannot nest unboundedly).
        const json* cur = &schema;
        int depth = 0;
        while (cur->is_object()) {
            auto pit = cur->find("properties");
            if (pit == cur->end() || !pit->is_object()) break;
            auto cit = pit->find("child");
            if (cit == pit->end()) break;
            cur = &*cit;
            ++depth;
        }
        CHECK(depth < 40);
        CHECK(depth <= 17);  // cut at kMaxSchemaDepth (16) + final {}
    }

    TEST_CASE("definitions-style refs resolve too") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {"y": {"$ref": "#/definitions/Y"}},
            "definitions": {"Y": {"type": "integer", "minimum": 0}}
        })");
        CHECK(mcp::sanitize_tool_schema(schema));
        CHECK(schema["properties"]["y"].value("type", "") == "integer");
        CHECK(schema["properties"]["y"].value("minimum", -1) == 0);
        CHECK(!dump_contains(schema, "definitions"));
    }

    TEST_CASE("escaped json-pointer segments resolve") {
        json schema = json::parse(R"({
            "type": "object",
            "properties": {"z": {"$ref": "#/$defs/a~1b"}},
            "$defs": {"a/b": {"type": "boolean"}}
        })");
        CHECK(mcp::sanitize_tool_schema(schema));
        CHECK(schema["properties"]["z"].value("type", "") == "boolean");
    }

    TEST_CASE("non-object schemas are ignored") {
        json s = "not a schema";
        CHECK(!mcp::sanitize_tool_schema(s));
        CHECK(s == "not a schema");
    }
}
