#include "tools/TodoListService.h"

#include "core/Logger.h"
#include "library/default_paths.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kDefaultMaxTodos = 100;
constexpr int kAbsoluteMaxTodos = 1000;

namespace {

constexpr std::string_view kStatusPending = "pending";
constexpr std::string_view kStatusInProgress = "in_progress";
constexpr std::string_view kStatusDone = "done";
constexpr std::string_view kStatusBlocked = "blocked";
constexpr std::string_view kGroupDefault = "General";

bool is_valid_status(std::string_view s) {
    return s == kStatusPending || s == kStatusInProgress || s == kStatusDone ||
           s == kStatusBlocked;
}

std::string normalise_status(std::string_view s) {
    if (is_valid_status(s)) return std::string(s);
    return std::string(kStatusPending);
}

}  // namespace

struct TodoItem {
    int id = 0;
    std::string text;
    std::string status = std::string(kStatusPending);
    std::string group = std::string(kGroupDefault);
    std::string created_at;
    std::string updated_at;

    json to_json() const {
        return json::object({
            {"id", id},
            {"text", text},
            {"status", status},
            {"group", group},
            {"created_at", created_at},
            {"updated_at", updated_at}
        });
    }

    static TodoItem from_json(const json& j) {
        TodoItem item;
        item.id = j.value("id", 0);
        item.text = j.value("text", "");
        item.status = normalise_status(j.value("status", ""));
        // Backward compatibility: legacy files stored boolean "done".
        if (j.contains("done") && j["done"].is_boolean() && j["done"].get<bool>()) {
            item.status = std::string(kStatusDone);
        }
        item.group = j.value("group", std::string(kGroupDefault));
        item.created_at = j.value("created_at", "");
        item.updated_at = j.value("updated_at", "");
        return item;
    }
};

struct TodoListData {
    int max_todos = kDefaultMaxTodos;
    int next_id = 1;
    std::vector<TodoItem> todos;

    json to_json() const {
        json arr = json::array();
        for (const auto& t : todos) {
            arr.push_back(t.to_json());
        }
        return json::object({
            {"max_todos", max_todos},
            {"next_id", next_id},
            {"todos", arr}
        });
    }

    static TodoListData from_json(const json& j) {
        TodoListData data;
        data.max_todos = j.value("max_todos", kDefaultMaxTodos);
        if (data.max_todos < 1) data.max_todos = kDefaultMaxTodos;
        data.next_id = j.value("next_id", 1);
        if (data.next_id < 1) data.next_id = 1;
        if (j.contains("todos") && j["todos"].is_array()) {
            for (const auto& item : j["todos"]) {
                data.todos.push_back(TodoItem::from_json(item));
            }
        }
        return data;
    }
};

static std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%d %H:%M");
    return ss.str();
}

static fs::path get_todo_file_path(const fs::path& project_root,
                                   std::string_view agent_name) {
    return hive::resolve_agent_dir(project_root, agent_name) / "todos.json";
}

static json success(const std::string& message, const json& extra = json::object()) {
    json r = json::object({
        {"status", "success"},
        {"message", message}
    });
    r.update(extra);
    return r;
}

static json error(const std::string& message) {
    return json::object({
        {"status", "error"},
        {"message", message}
    });
}

}  // namespace

// ============================================================================
// TodoListHandle::Impl
// ============================================================================

class TodoListHandle::Impl {
public:
    explicit Impl(fs::path file_path) : file_path_(std::move(file_path)) {}

    json add(const std::string& text, const std::optional<std::string>& status,
             const std::optional<std::string>& group) {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        if (text.empty()) {
            return error("Todo text cannot be empty");
        }
        if (static_cast<int>(data_.todos.size()) >= data_.max_todos) {
            return error("Todo list is full (" + std::to_string(data_.max_todos) +
                         "). Delete or complete items before adding new ones.");
        }

        TodoItem item;
        item.id = data_.next_id++;
        item.text = text;
        item.status = status.has_value() ? normalise_status(*status)
                                         : std::string(kStatusPending);
        item.group = group.value_or(std::string(kGroupDefault));
        item.created_at = current_timestamp();
        item.updated_at = item.created_at;
        data_.todos.push_back(item);

        save_nolock();
        return success("Todo added", json::object({{"todo", item.to_json()}}));
    }

    json edit(int id, const std::optional<std::string>& text,
              const std::optional<std::string>& status,
              const std::optional<std::string>& group) {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        auto it = std::find_if(data_.todos.begin(), data_.todos.end(),
                               [id](const TodoItem& t) { return t.id == id; });
        if (it == data_.todos.end()) {
            return error("Todo not found: id " + std::to_string(id));
        }
        if (!text.has_value() && !status.has_value() && !group.has_value()) {
            return error("Provide at least one of 'text', 'status', or 'group' to edit");
        }

        if (text.has_value()) {
            if (text->empty()) {
                return error("Todo text cannot be empty");
            }
            it->text = *text;
        }
        if (status.has_value()) {
            it->status = normalise_status(*status);
        }
        if (group.has_value()) {
            it->group = group.value_or(std::string(kGroupDefault));
        }
        it->updated_at = current_timestamp();

        save_nolock();
        return success("Todo updated", json::object({{"todo", it->to_json()}}));
    }

    json remove(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        auto it = std::find_if(data_.todos.begin(), data_.todos.end(),
                               [id](const TodoItem& t) { return t.id == id; });
        if (it == data_.todos.end()) {
            return error("Todo not found: id " + std::to_string(id));
        }

        json removed = it->to_json();
        data_.todos.erase(it);
        save_nolock();
        return success("Todo deleted", json::object({{"todo", removed}}));
    }

    json list(std::string_view filter,
              const std::optional<std::string>& group_filter) const {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        std::vector<const TodoItem*> filtered;
        for (const auto& t : data_.todos) {
            if (filter != "all" && t.status != filter) continue;
            if (group_filter.has_value() && t.group != *group_filter) continue;
            filtered.push_back(&t);
        }

        int pending = 0;
        int in_progress = 0;
        int done_items = 0;
        int blocked = 0;
        for (const auto& t : data_.todos) {
            if (filter != "all" && t.status != filter) continue;
            if (group_filter.has_value() && t.group != *group_filter) continue;
            if (t.status == kStatusPending) ++pending;
            else if (t.status == kStatusInProgress) ++in_progress;
            else if (t.status == kStatusDone) ++done_items;
            else if (t.status == kStatusBlocked) ++blocked;
        }

        // Group filtered items by group name, preserving insertion order.
        std::vector<std::string> group_order;
        std::map<std::string, std::vector<const TodoItem*>> by_group;
        for (const auto* t : filtered) {
            if (!by_group.contains(t->group)) {
                group_order.push_back(t->group);
            }
            by_group[t->group].push_back(t);
        }

        std::ostringstream out;
        out << "\n## Todo List\n";
        out << "Max allowed: " << data_.max_todos
            << " | Total: " << data_.todos.size()
            << " | Pending: " << pending
            << " | In Progress: " << in_progress
            << " | Done: " << done_items
            << " | Blocked: " << blocked << "\n";

        auto status_badge = [](const TodoItem* t) -> std::string {
            if (t->status == kStatusDone) return "[x]";
            if (t->status == kStatusInProgress) return "[>]";
            if (t->status == kStatusBlocked) return "[!]";
            return "[ ]";
        };

        if (filtered.empty()) {
            out << "\nNo todos match the filter.\n";
        } else {
            for (const auto& g : group_order) {
                out << "\n### " << g << "\n";
                for (const auto* t : by_group[g]) {
                    out << "- " << status_badge(t) << " [" << t->id << "] "
                        << t->text << "\n";
                }
            }
        }

        int matching_total = pending + in_progress + done_items + blocked;
        return json::object({
            {"status", "success"},
            {"output", out.str()},
            {"total", matching_total},
            {"pending", pending},
            {"in_progress", in_progress},
            {"done", done_items},
            {"blocked", blocked}
        });
    }

    json count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        int pending = 0;
        int in_progress = 0;
        int done_items = 0;
        int blocked = 0;
        std::map<std::string, int> by_group;
        for (const auto& t : data_.todos) {
            if (t.status == kStatusPending) ++pending;
            else if (t.status == kStatusInProgress) ++in_progress;
            else if (t.status == kStatusDone) ++done_items;
            else if (t.status == kStatusBlocked) ++blocked;
            ++by_group[t.group];
        }

        return json::object({
            {"status", "success"},
            {"total", data_.todos.size()},
            {"pending", pending},
            {"in_progress", in_progress},
            {"done", done_items},
            {"blocked", blocked},
            {"max_todos", data_.max_todos},
            {"by_group", by_group}
        });
    }

    json set_max(int max) {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        if (max < 1 || max > kAbsoluteMaxTodos) {
            return error("max_todos must be between 1 and " +
                         std::to_string(kAbsoluteMaxTodos));
        }
        if (static_cast<int>(data_.todos.size()) > max) {
            return error("Cannot set max below the current todo count (" +
                         std::to_string(data_.todos.size()) + "). Delete items first.");
        }
        data_.max_todos = max;
        save_nolock();
        return success("Maximum todo count set to " + std::to_string(max),
                       json::object({{"max_todos", max}}));
    }

    json clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        load_nolock();

        int removed = static_cast<int>(data_.todos.size());
        data_ = TodoListData{};
        data_.max_todos = kDefaultMaxTodos;
        save_nolock();
        return success("Todo list cleared",
                       json::object({{"removed", removed}}));
    }

private:
    mutable std::mutex mutex_;
    fs::path file_path_;
    mutable TodoListData data_;
    mutable bool loaded_ = false;

    void load_nolock() const {
        if (loaded_) return;
        loaded_ = true;

        if (!fs::exists(file_path_)) {
            data_ = TodoListData{};
            return;
        }

        try {
            std::ifstream in(file_path_);
            if (!in) {
                data_ = TodoListData{};
                return;
            }
            json j = json::parse(in, nullptr, false);
            if (j.is_discarded()) {
                data_ = TodoListData{};
            } else {
                data_ = TodoListData::from_json(j);
            }
        } catch (const std::exception& e) {
            LOG_WARNING(std::string("[TODO] Failed to load todo list: ") + e.what());
            data_ = TodoListData{};
        }
    }

    void save_nolock() const {
        try {
            fs::path parent = file_path_.parent_path();
            if (!parent.empty() && !fs::exists(parent)) {
                fs::create_directories(parent);
            }
            std::ofstream out(file_path_);
            if (!out) {
                LOG_ERROR(std::string("[TODO] Failed to open todo file for writing: ") +
                          file_path_.string());
                return;
            }
            out << data_.to_json().dump(2);
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("[TODO] Failed to save todo list: ") + e.what());
        }
    }
};

// ============================================================================
// TodoListHandle
// ============================================================================

TodoListHandle::TodoListHandle(std::filesystem::path file_path)
    : impl_(std::make_unique<Impl>(std::move(file_path))) {}

TodoListHandle::~TodoListHandle() = default;

json TodoListHandle::add(const std::string& text,
                          const std::optional<std::string>& status,
                          const std::optional<std::string>& group) {
    return impl_->add(text, status, group);
}

json TodoListHandle::edit(int id, const std::optional<std::string>& text,
                          const std::optional<std::string>& status,
                          const std::optional<std::string>& group) {
    return impl_->edit(id, text, status, group);
}

json TodoListHandle::remove(int id) {
    return impl_->remove(id);
}

json TodoListHandle::list(std::string_view filter,
                          const std::optional<std::string>& group) const {
    return impl_->list(filter, group);
}

json TodoListHandle::count() const {
    return impl_->count();
}

json TodoListHandle::set_max(int max) {
    return impl_->set_max(max);
}

json TodoListHandle::clear() {
    return impl_->clear();
}

// ============================================================================
// TodoListService::Impl
// ============================================================================

class TodoListService::Impl {
public:
    TodoListHandle& get(const fs::path& project_root,
                        std::string_view agent_name) {
        std::string key_root = project_root.string();
        std::string key_name(agent_name);
        std::lock_guard<std::mutex> lock(mutex_);
        auto& ptr = lists_[{key_root, key_name}];
        if (!ptr) {
            ptr = std::make_unique<TodoListHandle>(
                get_todo_file_path(project_root, agent_name));
        }
        return *ptr;
    }

private:
    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>,
             std::unique_ptr<TodoListHandle>> lists_;
};

// ============================================================================
// TodoListService
// ============================================================================

TodoListService& TodoListService::instance() {
    static TodoListService svc;
    return svc;
}

TodoListService::TodoListService() : impl_(std::make_unique<Impl>()) {}
TodoListService::~TodoListService() = default;

TodoListHandle& TodoListService::get(const fs::path& project_root,
                                     std::string_view agent_name) {
    return impl_->get(project_root, agent_name);
}
