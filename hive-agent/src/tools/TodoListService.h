#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

/**
 * @file TodoListService.h
 * @brief Internal service backing the todo_list tools and the public Agent API.
 *
 * Each agent namespace (project_root + agent_name) gets its own persisted list
 * under <project_root>/.daedalus/hive-brain[-<agent_name>]/todos.json.
 */

class TodoListHandle {
public:
    explicit TodoListHandle(std::filesystem::path file_path);
    ~TodoListHandle();

    nlohmann::json add(const std::string& text,
                        const std::optional<std::string>& status,
                        const std::optional<std::string>& group);
    nlohmann::json edit(int id,
                        const std::optional<std::string>& text,
                        const std::optional<std::string>& status,
                        const std::optional<std::string>& group);
    nlohmann::json remove(int id);
    nlohmann::json list(std::string_view filter,
                        const std::optional<std::string>& group) const;
    nlohmann::json count() const;
    nlohmann::json set_max(int max);
    nlohmann::json clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TodoListService {
public:
    static TodoListService& instance();

    TodoListHandle& get(const std::filesystem::path& project_root,
                        std::string_view agent_name);

private:
    TodoListService();
    ~TodoListService();
    TodoListService(const TodoListService&) = delete;
    TodoListService& operator=(const TodoListService&) = delete;

    class Impl;
    std::unique_ptr<Impl> impl_;
};
