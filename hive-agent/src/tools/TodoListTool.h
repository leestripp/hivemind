#pragma once

#include "BaseTool.h"
#include <string>
#include <nlohmann/json.hpp>

class FileIO;

using json = nlohmann::json;

/**
 * @file TodoListTool.h
 * @brief Per-project, per-agent todo list tools.
 *
 * Each agent namespace gets its own persisted todo list under
 * <project_root>/.daedalus/hive-brain[-<agent_name>]/todos.json.
 */

/**
 * todo_add — add a new task to the list.
 * Usage: todo_add(text="...", done=false)
 */
class TodoAddTool : public BaseTool {
public:
    std::string get_name() const override { return "todo_add"; }
    std::string get_description() const override {
        return "Add a task to the agent's per-project todo list.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * todo_edit — update an existing task's text and/or done state.
 * Usage: todo_edit(id=..., text="...", done=true)
 */
class TodoEditTool : public BaseTool {
public:
    std::string get_name() const override { return "todo_edit"; }
    std::string get_description() const override {
        return "Edit a todo item by id: change its text or mark it done/pending.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * todo_delete — remove a task by id.
 * Usage: todo_delete(id=...)
 */
class TodoDeleteTool : public BaseTool {
public:
    std::string get_name() const override { return "todo_delete"; }
    std::string get_description() const override {
        return "Delete a todo item by id.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * todo_list — show the current todo list.
 * Usage: todo_list(filter="all|pending|done")
 */
class TodoListTool : public BaseTool {
public:
    std::string get_name() const override { return "todo_list"; }
    std::string get_description() const override {
        return "List the agent's todos, grouped into pending and done sections.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * set_max_todos — change the maximum number of allowed todos.
 * Usage: set_max_todos(max=10)
 */
class TodoSetMaxTool : public BaseTool {
public:
    std::string get_name() const override { return "set_max_todos"; }
    std::string get_description() const override {
        return "Set the maximum number of todos allowed in the list (default 10).";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * todo_count — quick count of total/pending/done items.
 * Usage: todo_count
 */
class TodoCountTool : public BaseTool {
public:
    std::string get_name() const override { return "todo_count"; }
    std::string get_description() const override {
        return "Return the number of total, pending, and done todos.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * todo_clear — remove all todos and reset the list.
 * Usage: todo_clear
 */
class TodoClearTool : public BaseTool {
public:
    std::string get_name() const override { return "todo_clear"; }
    std::string get_description() const override {
        return "Clear the entire todo list. Use this when the list becomes stale.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
