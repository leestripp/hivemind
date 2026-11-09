#include "BrainTool.h"

#include "core/FileIO.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::shared_ptr<hive::SecondBrain> brain_for_root(const fs::path& requested) {
    static std::mutex mutex;
    static std::map<fs::path, std::shared_ptr<hive::SecondBrain>> engines;

    std::error_code ec;
    fs::path root = fs::weakly_canonical(requested, ec);
    if (ec) root = requested.lexically_normal();
    std::lock_guard lock(mutex);
    auto& engine = engines[root];
    if (!engine) engine = std::make_shared<hive::SecondBrain>(root);
    return engine;
}

std::string require_string(const json& args, const char* key,
                           const char* action) {
    const auto value = args.value(key, "");
    if (value.empty())
        throw std::invalid_argument(std::string("brain: '") + key +
                                    "' is required for action '" + action + "'");
    return value;
}

std::vector<std::string> string_array(const json& args, const char* key) {
    std::vector<std::string> out;
    if (!args.contains(key) || !args.at(key).is_array()) return out;
    for (const auto& item : args.at(key)) {
        if (!item.is_string()) continue;
        const auto value = item.get<std::string>();
        if (!value.empty()) out.push_back(value);
    }
    return out;
}

json note_json(const hive::BrainNote& note) {
    return json::object({
        {"id", note.id}, {"folder", note.folder}, {"title", note.title},
        {"tags", note.tags}, {"created", note.created}, {"updated", note.updated},
        {"links", note.links}, {"body", note.body},
    });
}

}  // namespace

BrainTool::BrainTool() {
    description_ =
        "The shared second brain for every agent on this worker — a "
        "plain-Markdown knowledge base at /workspace/second-brain (the shared "
        "project root's second-brain directory). Use it to capture, organise, "
        "find and connect long-term knowledge: research, decisions, how-tos "
        "and reference material. Use 'remember' for private session-scoped "
        "facts instead. Notes live in PARA folders: inbox (quick captures), "
        "projects (active goals), areas (ongoing responsibilities), resources "
        "(reference), archives (done/inactive). One idea per note; link notes "
        "with [[note-id]] wiki-links. Actions: capture, search, read, list, "
        "update, append, file, backlinks, delete, reindex.";
}

json BrainTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"action", {
                        {"type", "string"},
                        {"enum", {"capture", "search", "read", "list",
                                  "update", "append", "file", "backlinks",
                                  "delete", "reindex"}},
                        {"description", "capture: new note (body required). search: full-text (query required). read: full note (id required). list: metadata rows. update: replace given fields (id required). append: add Markdown (id+markdown required). file: move folder (id+folder required). backlinks: notes linking to id. delete: remove note (id required). reindex: reconcile the index from Markdown."}
                    }},
                    {"folder", {
                        {"type", "string"},
                        {"enum", {"inbox", "projects", "areas", "resources", "archives"}},
                        {"description", "PARA folder. capture destination (default inbox), list filter, or file destination."}
                    }},
                    {"title", {{"type", "string"},
                        {"description", "Note title for capture/update; capture falls back to the body's first heading."}}},
                    {"tags", {
                        {"type", "array"}, {"items", {{"type", "string"}}},
                        {"description", "Tags for capture/update, preferably short lowercase slugs."}
                    }},
                    {"body", {{"type", "string"},
                        {"description", "Markdown body (required for capture, replacement for update)."}}},
                    {"markdown", {{"type", "string"},
                        {"description", "Markdown appended to the body (required for append)."}}},
                    {"id", {{"type", "string"},
                        {"description", "Note id returned by capture/list/search; required by read/update/append/file/backlinks/delete."}}},
                    {"query", {{"type", "string"},
                        {"description", "Plain-language search text matching title/tags/body with prefix and stemming."}}},
                    {"tag", {{"type", "string"},
                        {"description", "Single tag filter for list."}}},
                    {"limit", {{"type", "integer"},
                        {"description", "Maximum results (search default 20, list default 100, maximum 1000)."}}}
                }},
                {"required", {"action"}}
            }}
        }}
    };
}

json BrainTool::execute(const json& args, FileIO& file_io) {
    try {
        const std::string action = args.value("action", "");
        auto brain = brain_for_root(file_io.get_project_path() / "second-brain");
        if (action == "capture") return do_capture(*brain, args);
        if (action == "search") return do_search(*brain, args);
        if (action == "read") return do_read(*brain, args);
        if (action == "list") return do_list(*brain, args);
        if (action == "update") return do_update(*brain, args);
        if (action == "append") return do_append(*brain, args);
        if (action == "file") return do_file(*brain, args);
        if (action == "backlinks") return do_backlinks(*brain, args);
        if (action == "delete") return do_delete(*brain, args);
        if (action == "reindex") return do_reindex(*brain);
        return json::object({
            {"status", "error"},
            {"message", "brain: unknown or missing action '" + action +
                        "' — expected capture | search | read | list | update | "
                        "append | file | backlinks | delete | reindex"},
        });
    } catch (const std::exception& error) {
        return json::object({{"status", "error"}, {"message", error.what()}});
    }
}

json BrainTool::meta_json(const hive::BrainNoteMeta& meta) {
    return json::object({
        {"id", meta.id}, {"folder", meta.folder}, {"title", meta.title},
        {"tags", meta.tags}, {"created", meta.created}, {"updated", meta.updated},
    });
}

json BrainTool::do_capture(hive::SecondBrain& brain, const json& args) {
    const auto body = require_string(args, "body", "capture");
    const auto note = brain.capture(args.value("folder", "inbox"),
                                    args.value("title", ""),
                                    string_array(args, "tags"), body);
    return json::object({
        {"status", "ok"}, {"id", note.id}, {"folder", note.folder},
        {"title", note.title},
        {"message", "captured as " + note.folder + "/" + note.id},
    });
}

json BrainTool::do_search(hive::SecondBrain& brain, const json& args) {
    const auto query = require_string(args, "query", "search");
    const auto hits = brain.search(query, args.value("limit", 20));
    json results = json::array();
    for (const auto& hit : hits) {
        auto item = meta_json(hit.note);
        item["snippet"] = hit.snippet;
        results.push_back(std::move(item));
    }
    return json::object({{"status", "ok"}, {"count", results.size()},
                         {"results", std::move(results)}});
}

json BrainTool::do_read(hive::SecondBrain& brain, const json& args) {
    const auto id = require_string(args, "id", "read");
    const auto note = brain.read(id);
    if (!note)
        return json::object({{"status", "error"},
                             {"message", "brain: no note with id '" + id + "'"}});
    auto result = note_json(*note);
    result["status"] = "ok";
    result["backlinks"] = brain.backlinks(id);
    return result;
}

json BrainTool::do_list(hive::SecondBrain& brain, const json& args) {
    const auto rows = brain.list(args.value("folder", ""), args.value("tag", ""),
                                 args.value("limit", 100));
    json notes = json::array();
    for (const auto& row : rows) notes.push_back(meta_json(row));
    return json::object({{"status", "ok"}, {"count", notes.size()},
                         {"folders", hive::SecondBrain::folders()},
                         {"notes", std::move(notes)}});
}

json BrainTool::do_update(hive::SecondBrain& brain, const json& args) {
    const auto id = require_string(args, "id", "update");
    std::optional<std::string> title;
    if (args.contains("title") && args.at("title").is_string())
        title = args.at("title").get<std::string>();
    std::optional<std::vector<std::string>> tags;
    if (args.contains("tags") && args.at("tags").is_array())
        tags = string_array(args, "tags");
    std::optional<std::string> body;
    if (args.contains("body") && args.at("body").is_string())
        body = args.at("body").get<std::string>();
    if (!title && !tags && !body)
        return json::object({{"status", "error"},
            {"message", "brain: update needs at least one of title/tags/body"}});
    if (!brain.update(id, title, tags, body))
        return json::object({{"status", "error"},
                             {"message", "brain: no note with id '" + id + "'"}});
    return json::object({{"status", "ok"}, {"id", id}});
}

json BrainTool::do_append(hive::SecondBrain& brain, const json& args) {
    const auto id = require_string(args, "id", "append");
    const auto markdown = require_string(args, "markdown", "append");
    if (!brain.append(id, markdown))
        return json::object({{"status", "error"},
                             {"message", "brain: no note with id '" + id + "'"}});
    return json::object({{"status", "ok"}, {"id", id}});
}

json BrainTool::do_file(hive::SecondBrain& brain, const json& args) {
    const auto id = require_string(args, "id", "file");
    const auto folder = require_string(args, "folder", "file");
    if (!brain.move(id, folder))
        return json::object({{"status", "error"},
                             {"message", "brain: no note with id '" + id + "'"}});
    return json::object({{"status", "ok"}, {"id", id}, {"folder", folder},
                         {"message", "filed " + id + " under " + folder}});
}

json BrainTool::do_backlinks(hive::SecondBrain& brain, const json& args) {
    const auto id = require_string(args, "id", "backlinks");
    const auto refs = brain.backlinks(id);
    return json::object({{"status", "ok"}, {"id", id}, {"count", refs.size()},
                         {"backlinks", refs}});
}

json BrainTool::do_delete(hive::SecondBrain& brain, const json& args) {
    const auto id = require_string(args, "id", "delete");
    if (!brain.remove(id))
        return json::object({{"status", "error"},
                             {"message", "brain: no note with id '" + id + "'"}});
    return json::object({{"status", "ok"}, {"deleted", id}});
}

json BrainTool::do_reindex(hive::SecondBrain& brain) {
    const auto result = brain.reindex();
    return json::object({
        {"status", "ok"}, {"indexed", result.indexed}, {"removed", result.removed},
        {"message", "reindex complete: " + std::to_string(result.indexed) +
                    " (re)indexed, " + std::to_string(result.removed) + " removed"},
    });
}
