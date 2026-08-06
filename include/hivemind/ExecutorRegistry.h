#pragma once

#include "NodeExecutor.h"
#include <string>
#include <memory>
#include <unordered_map>

/**
 * ExecutorRegistry — maps node type string → concrete NodeExecutor instance.
 *
 * Thread-safe: the registry itself is read-only after construction.
 * Each executor instance is owned by the registry.
 */
class ExecutorRegistry {
public:
    ExecutorRegistry();
    ~ExecutorRegistry();

    // No copy/move
    ExecutorRegistry(const ExecutorRegistry&) = delete;
    ExecutorRegistry& operator=(const ExecutorRegistry&) = delete;

    /**
     * Get the executor for a node type.
     * @param type  e.g. "prompt", "agent", "loop_start", ...
     * @return      nullptr if unknown type
     */
    NodeExecutor* get(std::string_view type) const;

    /**
     * All registered type names.
     */
    std::vector<std::string_view> all_types() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl_;
};
