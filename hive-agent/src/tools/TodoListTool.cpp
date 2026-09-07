#include "tools/TodoListTool.h"
#include "tools/TodoListService.h"
#include "core/FileIO.h"
#include "core/Logger.h"

#include <optional>
#include <string>
#include <string_view>

using json = nlohmann::json;

namespace {

// ============================================================================
// Argument helpers
// ============================================================================

static std::optional<int> parse_id(const json& args) {
    if (!args.contains("id")) return std::nullopt;
    const auto& v = args["id"];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static std::string normalise_filter(const json& args) {
    std::string f = args.value("filter", "all");
    if (f == "pending" || f == "in_progress" || f == "done" || f == "blocked") {
        return f;
    }
    return "all";
}

static std::optional<std::string> opt_string(const json& args,
                                              const std::string& key) {
    if (!args.contains(key)) return std::nullopt;
    const auto& v = args[key];
    if (v.is_string()) return v.get<std::string>();
    return std::nullopt;
}

static json make_tool_def(const std::string& name,
                          const std::string& description,
                          const json& properties,
                          const json& required = json::array()) {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", name},
            {"description", description},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", properties},
                {"required", required}
            })}
        })}
    });
}

}  // namespace

// ============================================================================
// Tool definitions
// ============================================================================

static TodoListHandle& get_list(FileIO& file_io) {
    return TodoListService::instance().get(
        file_io.get_project_path(), file_io.get_agent_name());
}

json TodoAddTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "Add a task to the per-project todo list. The list persists across turns "
        "and sessions in .daedalus/hive-brain[-<name>]/todos.json.",
        json::object({
            {"text", json::object({
                {"type", "string"},
                {"description", "The task description."}
            })},
            {"status", json::object({
                {"type", "string"},
                {"description", "Task status: 'pending' (default), 'in_progress', 'done', or 'blocked'."},
                {"enum", json::array({"pending", "in_progress", "done", "blocked"})}
            })},
            {"group", json::object({
                {"type", "string"},
                {"description", "Phase or category, e.g. 'Understand', 'Plan', 'Implement', 'Verify' (default: 'General')."}
            })}
        }),
        json::array({"text"})
    );
}

json TodoAddTool::execute(const json& args, FileIO& file_io) {
    try {
        std::string text = args.value("text", "");
        return get_list(file_io).add(text, opt_string(args, "status"),
                                     opt_string(args, "group"));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] todo_add failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json TodoEditTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "Edit an existing todo by id. Change the text, status, and/or group.",
        json::object({
            {"id", json::object({
                {"type", "integer"},
                {"description", "The todo id to edit."}
            })},
            {"text", json::object({
                {"type", "string"},
                {"description", "New task description (optional)."}
            })},
            {"status", json::object({
                {"type", "string"},
                {"description", "New status: 'pending', 'in_progress', 'done', or 'blocked' (optional)."},
                {"enum", json::array({"pending", "in_progress", "done", "blocked"})}
            })},
            {"group", json::object({
                {"type", "string"},
                {"description", "New phase or category (optional)."}
            })}
        }),
        json::array({"id"})
    );
}

json TodoEditTool::execute(const json& args, FileIO& file_io) {
    try {
        auto id_opt = parse_id(args);
        if (!id_opt) {
            return json::object(
                {{"status", "error"}, {"message", "Missing or invalid 'id'"}});
        }
        std::optional<std::string> text;
        if (args.contains("text")) {
            if (!args["text"].is_string()) {
                return json::object(
                    {{"status", "error"}, {"message", "'text' must be a string"}});
            }
            text = args["text"].get<std::string>();
        }
        return get_list(file_io).edit(*id_opt, text, opt_string(args, "status"),
                                      opt_string(args, "group"));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] todo_edit failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json TodoDeleteTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "Delete a todo item by id.",
        json::object({
            {"id", json::object({
                {"type", "integer"},
                {"description", "The todo id to delete."}
            })}
        }),
        json::array({"id"})
    );
}

json TodoDeleteTool::execute(const json& args, FileIO& file_io) {
    try {
        auto id_opt = parse_id(args);
        if (!id_opt) {
            return json::object(
                {{"status", "error"}, {"message", "Missing or invalid 'id'"}});
        }
        return get_list(file_io).remove(*id_opt);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] todo_delete failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json TodoListTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "List todos grouped by phase/category and status. Use filter and group to "
        "narrow the view.",
        json::object({
            {"filter", json::object({
                {"type", "string"},
                {"description", "Status filter: 'all' (default), 'pending', 'in_progress', 'done', or 'blocked'."},
                {"enum", json::array({"all", "pending", "in_progress", "done", "blocked"})}
            })},
            {"group", json::object({
                {"type", "string"},
                {"description", "Optional phase/category filter, e.g. 'Implement' or 'Verify'."}
            })}
        })
    );
}

json TodoListTool::execute(const json& args, FileIO& file_io) {
    try {
        std::string filter = normalise_filter(args);
        return get_list(file_io).list(filter, opt_string(args, "group"));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] todo_list failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json TodoSetMaxTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "Set the maximum number of todos allowed. Default is 100. Cannot be set lower "
        "than the current list size.",
        json::object({
            {"max", json::object({
                {"type", "integer"},
                {"description", "Maximum todos (1-1000)."},
                {"minimum", 1},
                {"maximum", 1000}
            })}
        }),
        json::array({"max"})
    );
}

json TodoSetMaxTool::execute(const json& args, FileIO& file_io) {
    try {
        if (!args.contains("max") || !args["max"].is_number_integer()) {
            return json::object(
                {{"status", "error"}, {"message", "Missing or invalid 'max'"}});
        }
        int max = args["max"].get<int>();
        return get_list(file_io).set_max(max);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] set_max_todos failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json TodoCountTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "Return the total, pending, and done todo counts plus the current maximum.",
        json::object()
    );
}

json TodoCountTool::execute(const json& /*args*/, FileIO& file_io) {
    try {
        return get_list(file_io).count();
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] todo_count failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json TodoClearTool::get_definition() const {
    return make_tool_def(
        get_name(),
        "Clear the entire todo list. Use this when the list becomes stale or the "
        "current task changes completely.",
        json::object()
    );
}

json TodoClearTool::execute(const json& /*args*/, FileIO& file_io) {
    try {
        return get_list(file_io).clear();
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[TODO] todo_clear failed: ") + e.what());
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}
