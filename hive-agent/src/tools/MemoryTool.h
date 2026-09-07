#pragma once

#include "BaseTool.h"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class SessionMemory;

using json = nlohmann::json;

/**
 * RememberTool - Explicitly saves a memory
 * Usage: remember(category, content, confidence, tags)
 */
class RememberTool : public BaseTool {
private:
    static SessionMemory* session_memory_;
public:
    static void set_session_memory(SessionMemory* memory) { session_memory_ = memory; }
    
    std::string get_name() const override { return "remember"; }
    std::string get_description() const override { return "Save a fact, preference, or note to long-term memory for future sessions."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * ForgetTool - Removes a memory by ID
 * Usage: forget(id)
 */
class ForgetTool : public BaseTool {
private:
    static SessionMemory* session_memory_;
public:
    static void set_session_memory(SessionMemory* memory) { session_memory_ = memory; }
    
    std::string get_name() const override { return "forget"; }
    std::string get_description() const override { return "Remove a specific memory by its ID"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * MemoriesTool - Lists all memories, optionally filtered
 * Usage: memories(category, limit)
 */
class MemoriesTool : public BaseTool {
private:
    static SessionMemory* session_memory_;
public:
    static void set_session_memory(SessionMemory* memory) { session_memory_ = memory; }
    
    std::string get_name() const override { return "memories"; }
    std::string get_description() const override { return "List saved memories, optionally filtered by category or tags."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * MemoryStatsTool - Shows memory statistics
 * Usage: memory_stats
 */
class MemoryStatsTool : public BaseTool {
private:
    static SessionMemory* session_memory_;
public:
    static void set_session_memory(SessionMemory* memory) { session_memory_ = memory; }
    
    std::string get_name() const override { return "memory_stats"; }
    std::string get_description() const override { return "Show statistics about session memory usage"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
