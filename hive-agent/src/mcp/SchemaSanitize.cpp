#include "mcp/SchemaSanitize.h"

#include <algorithm>
#include <string>
#include <vector>

namespace mcp {
namespace {

constexpr int kMaxSchemaDepth  = 16;
constexpr int kMaxResolveDepth = 24;

// Walk one segment of a JSON pointer: "~1" -> '/', "~0" -> '~'.
std::string ptr_unescape(std::string seg) {
    std::string out;
    out.reserve(seg.size());
    for (size_t i = 0; i < seg.size(); ++i) {
        if (seg[i] == '~' && i + 1 < seg.size()) {
            if (seg[i + 1] == '1') { out += '/'; ++i; continue; }
            if (seg[i + 1] == '0') { out += '~'; ++i; continue; }
        }
        out += seg[i];
    }
    return out;
}

// Phase 1: eliminate "$ref" entirely (2026-09-30, 4th schema incident —
// see SchemaSanitize.h). comfy-mcp's poison was an INTERNAL ref
// (#/$defs/SlotOverride) which the first cut of this file ignored because
// it only degraded EXTERNAL refs; Moonshot rejected the whole surface.
// Internal refs are INLINED (guidance preserved — verified accepted by the
// live provider 2026-09-30); unresolvable shapes degrade to {}:
//   - external refs (not "#/...")      — providers cannot fetch them
//   - refs with sibling keys           — merge semantics, not worth it
//   - ref cycles (ref already on the active resolution stack)
//   - refs pointing at missing targets
//   - resolution deeper than kMaxResolveDepth
bool resolve_refs(nlohmann::json& node, const nlohmann::json& root,
                  std::vector<std::string>& active, int depth) {
    if (depth > kMaxResolveDepth) {
        node = nlohmann::json::object();
        return true;
    }
    if (node.is_array()) {
        bool modified = false;
        for (auto& el : node)
            modified |= resolve_refs(el, root, active, depth + 1);
        return modified;
    }
    if (!node.is_object()) return false;

    if (auto it = node.find("$ref");
        it != node.end() && it->is_string()) {
        const std::string ref = it->get<std::string>();
        const bool cycle = std::find(active.begin(), active.end(), ref)
                           != active.end();
        if (ref.rfind("#/", 0) != 0 || node.size() > 1 || cycle) {
            node = nlohmann::json::object();
            return true;
        }
        const nlohmann::json* tgt = &root;
        bool ok = true;
        size_t pos = 2;  // skip "#/"
        while (ok && pos <= ref.size()) {
            const size_t slash = ref.find('/', pos);
            const std::string seg = ptr_unescape(ref.substr(
                pos, slash == std::string::npos ? std::string::npos
                                                : slash - pos));
            if (tgt->is_object())
                if (auto tit = tgt->find(seg); tit != tgt->end()) {
                    tgt = &*tit;
                } else {
                    ok = false;
                }
            else
                ok = false;
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (!ok) {
            node = nlohmann::json::object();
            return true;
        }
        active.push_back(ref);
        nlohmann::json inlined = *tgt;  // deep copy — defs stay shared-source
        resolve_refs(inlined, root, active, depth + 1);
        active.pop_back();
        node = std::move(inlined);
        return true;
    }

    bool modified = false;
    for (auto& [key, sub] : node.items()) {
        // Def tables are stripped at the root by sanitize_tool_schema()
        // once every ref is inlined — descending into them is dead work.
        if (key == "$defs" || key == "definitions") continue;
        modified |= resolve_refs(sub, root, active, depth + 1);
    }
    return modified;
}

// Phase 2: structural walk. Ancestry holds the serialized combinator
// nodes on the path from the root to `node`. If a combinator node's own
// serialization already appears in its ancestry, the shape is a
// structural fixpoint: a validator that unrolls it (Moonshot's "moonshot
// flavored json schema" check) never terminates.
bool walk(nlohmann::json& node, std::vector<std::string>& ancestry,
          int depth) {
    bool modified = false;

    if (depth > kMaxSchemaDepth) {
        node = nlohmann::json::object();
        return true;
    }
    if (!node.is_object()) return false;

    static constexpr const char* kCombinators[] = {"anyOf", "oneOf", "allOf"};
    for (const char* key : kCombinators) {
        auto it = node.find(key);
        if (it == node.end() || !it->is_array()) continue;

        const std::string self = node.dump();
        if (std::find(ancestry.begin(), ancestry.end(), self)
            != ancestry.end()) {
            node = nlohmann::json::object();   // recursive shape
            return true;
        }
        ancestry.push_back(self);
        for (auto& sub : *it)
            modified |= walk(sub, ancestry, depth + 1);
        ancestry.pop_back();
        if (!node.is_object()) return true;   // replaced mid-walk
    }

    if (auto it = node.find("properties");
        it != node.end() && it->is_object()) {
        for (auto& [_, sub] : it->items())
            modified |= walk(sub, ancestry, depth + 1);
    }
    if (auto it = node.find("items"); it != node.end())
        modified |= walk(*it, ancestry, depth + 1);
    if (auto it = node.find("additionalProperties"); it != node.end())
        modified |= walk(*it, ancestry, depth + 1);

    return modified;
}

} // namespace

bool sanitize_tool_schema(nlohmann::json& schema) {
    if (!schema.is_object()) return false;
    bool modified = false;

    std::vector<std::string> active;
    modified |= resolve_refs(schema, schema, active, 0);
    // Every ref is inlined (or degraded) by now — the def tables are dead
    // weight and some validators trip on their mere presence.
    modified |= schema.erase("$defs") > 0;
    modified |= schema.erase("definitions") > 0;

    std::vector<std::string> ancestry;
    modified |= walk(schema, ancestry, 0);
    return modified;
}

} // namespace mcp
