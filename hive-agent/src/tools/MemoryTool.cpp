#include "tools/MemoryTool.h"
#include "core/Utf8Utils.h"  // §BUG-UTF8-TRUNC
#include "core/SessionMemory.h"
#include "core/FileIO.h"

#include <sstream>
#include <iomanip>

using json = nlohmann::json;

// Static member definitions
SessionMemory* RememberTool::session_memory_ = nullptr;
SessionMemory* ForgetTool::session_memory_ = nullptr;
SessionMemory* MemoriesTool::session_memory_ = nullptr;
SessionMemory* MemoryStatsTool::session_memory_ = nullptr;

// Helper to format timestamp
static std::string format_time(const std::chrono::system_clock::time_point& tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%d %H:%M");
    return ss.str();
}

// ============================================================================
// RememberTool Implementation
// ============================================================================

json RememberTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Explicitly saves a memory for future sessions. Use this when you learn something important about the project that should be remembered."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"category", json::object({
                        {"type", "string"},
                        {"description", "Category: builds, errors, preferences, patterns, or custom"},
                        {"enum", {"builds", "errors", "preferences", "patterns", "sessions", "custom"}}
                    })},
                    {"content", json::object({
                        {"type", "string"},
                        {"description", "The content to remember"}
                    })},
                    {"confidence", json::object({
                        {"type", "integer"},
                        {"description", "Confidence level 1-5 (default: 3)"},
                        {"minimum", 1},
                        {"maximum", 5}
                    })},
                    {"tags", json::object({
                        {"type", "array"},
                        {"description", "Optional tags for categorization"},
                        {"items", json::object({
                            {"type", "string"}
                        })}
                    })}
                })},
                {"required", {"category", "content"}}
            })}
        })}
    });
}

json RememberTool::execute(const json& args, FileIO& /*file_io*/) {
    if (!session_memory_) {
        return json::object({
            {"status", "error"},
            {"message", "Session memory not available"}
        });
    }
    
    try {
        std::string category = args.value("category", "custom");
        std::string content = args.value("content", "");
        int confidence = args.value("confidence", 3);
        std::vector<std::string> tags;
        
        if (args.contains("tags") && args["tags"].is_array()) {
            for (const auto& tag : args["tags"]) {
                if (tag.is_string()) {
                    tags.push_back(tag.get<std::string>());
                }
            }
        }
        
        if (content.empty()) {
            return json::object({
                {"status", "error"},
                {"message", "Content cannot be empty"}
            });
        }
        
        session_memory_->remember(category, content, confidence, tags);
        
        return json::object({
            {"status", "success"},
            {"message", "Memory saved successfully"},
            {"memory", json::object({
                {"category", category},
                {"content", content},
                {"confidence", confidence},
                {"tags", tags}
            })}
        });
        
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Error saving memory: ") + e.what()}
        });
    }
}

// ============================================================================
// ForgetTool Implementation
// ============================================================================

json ForgetTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Removes a specific memory by its ID. Use 'memories' tool to find memory IDs first."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"id", json::object({
                        {"type", "string"},
                        {"description", "The memory ID to forget (e.g., 'err_001', 'bld_abc123')"}
                    })}
                })},
                {"required", {"id"}}
            })}
        })}
    });
}

json ForgetTool::execute(const json& args, FileIO& /*file_io*/) {
    if (!session_memory_) {
        return json::object({
            {"status", "error"},
            {"message", "Session memory not available"}
        });
    }
    
    try {
        std::string id = args.value("id", "");
        
        if (id.empty()) {
            return json::object({
                {"status", "error"},
                {"message", "Memory ID cannot be empty"}
            });
        }
        
        bool success = session_memory_->forget(id);
        
        if (success) {
            return json::object({
                {"status", "success"},
                {"message", "Memory forgotten: " + id}
            });
        } else {
            return json::object({
                {"status", "error"},
                {"message", "Memory not found: " + id}
            });
        }
        
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Error forgetting memory: ") + e.what()}
        });
    }
}

// ============================================================================
// MemoriesTool Implementation
// ============================================================================

json MemoriesTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Lists all memories, optionally filtered by category. Shows memory IDs, content, and confidence levels."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"category", json::object({
                        {"type", "string"},
                        {"description", "Filter by category: builds, errors, preferences, patterns, sessions, or 'all'"},
                        {"enum", {"all", "builds", "errors", "preferences", "patterns", "sessions"}}
                    })},
                    {"limit", json::object({
                        {"type", "integer"},
                        {"description", "Maximum number of memories to show (default: 20)"}
                    })},
                    {"show_unresolved_only", json::object({
                        {"type", "boolean"},
                        {"description", "If true, only show unresolved errors"}
                    })}
                })}
            })}
        })}
    });
}

json MemoriesTool::execute(const json& args, FileIO& /*file_io*/) {
    if (!session_memory_) {
        return json::object({
            {"status", "error"},
            {"message", "Session memory not available"}
        });
    }
    
    try {
        std::string category = args.value("category", "all");
        int limit = args.value("limit", 20);
        bool unresolved_only = args.value("show_unresolved_only", false);
        
        std::stringstream output;
        output << "\n## Session Memories\n";
        
        // Show builds
        if (category == "all" || category == "builds") {
            auto builds = session_memory_->get_builds();
            if (!builds.empty()) {
                output << "\n### Builds (" << builds.size() << " entries)\n";
                int count = 0;
                for (const auto* b : builds) {
                    if (count >= limit) break;
                    // §BUG-UTF8-TRUNC: entries stored before utf8-safe
                    // truncation may hold split codepoints — never let them
                    // into a tool result raw (next payload dump would 316).
                    output << "- [" << b->id << "] "
                           << utf8_sanitize_replace_invalid(b->command) << "\n";
                    output << "  Used " << b->use_count << " times, last: " << format_time(b->last_used) << "\n";
                    count++;
                }
            }
        }
        
        // Show errors
        if (category == "all" || category == "errors") {
            auto errors = unresolved_only ? 
                session_memory_->get_unresolved_errors() : 
                session_memory_->get_errors();
            
            if (!errors.empty()) {
                output << "\n### Errors (" << errors.size() << " entries";
                if (unresolved_only) output << " (unresolved only)";
                output << ")\n";
                
                int count = 0;
                for (const auto* e : errors) {
                    if (count >= limit) break;
                    output << "- [" << e->id << "] \"" << e->pattern << "\"\n";
                    if (!e->solution.empty()) {
                        output << "  → " << e->solution << "\n";
                    }
                    output << "  Seen " << e->occurrences << " times, ";
                    output << (e->resolved ? "RESOLVED ✓" : "UNRESOLVED") << "\n";
                    count++;
                }
            }
        }
        
        // Show preferences
        if (category == "all" || category == "preferences") {
            auto prefs = session_memory_->get_preferences();
            if (!prefs.empty()) {
                output << "\n### Preferences (" << prefs.size() << " entries)\n";
                int count = 0;
                for (const auto* p : prefs) {
                    if (count >= limit) break;
                    output << "- [" << p->id << "] " << p->value << "\n";
                    if (!p->inferred_from.empty()) {
                        output << "  Inferred from: " << p->inferred_from << "\n";
                    }
                    count++;
                }
            }
        }
        
        // Show patterns
        if (category == "all" || category == "patterns") {
            auto patterns = session_memory_->get_patterns();
            if (!patterns.empty()) {
                output << "\n### Patterns (" << patterns.size() << " entries)\n";
                int count = 0;
                for (const auto* p : patterns) {
                    if (count >= limit) break;
                    output << "- [" << p->id << "] " << p->description << "\n";
                    if (!p->example.empty()) {
                        output << "  Example: " << p->example << "\n";
                    }
                    count++;
                }
            }
        }
        
        // Show recent sessions
        if (category == "all" || category == "sessions") {
            auto sessions = session_memory_->get_recent_sessions(limit);
            if (!sessions.empty()) {
                output << "\n### Recent Sessions (" << sessions.size() << " entries)\n";
                for (const auto* s : sessions) {
                    output << "- [" << s->id << "] " << format_time(s->timestamp) << "\n";
                    output << "  " << s->summary << "\n";
                    if (!s->files_touched.empty()) {
                        output << "  Files: " << s->files_touched.size() << " touched\n";
                    }
                }
            }
        }
        
        return json::object({
            {"status", "success"},
            {"output", output.str()}
        });
        
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Error listing memories: ") + e.what()}
        });
    }
}

// ============================================================================
// MemoryStatsTool Implementation
// ============================================================================

json MemoryStatsTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Shows statistics about session memory usage including entry counts, storage size, and memory health."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({})}
            })}
        })}
    });
}

json MemoryStatsTool::execute(const json& /*args*/, FileIO& /*file_io*/) {
    if (!session_memory_) {
        return json::object({
            {"status", "error"},
            {"message", "Session memory not available"}
        });
    }
    
    try {
        auto stats = session_memory_->get_stats();
        
        std::stringstream output;
        output << "\n## Session Memory Statistics\n\n";
        output << "| Metric | Value |\n";
        output << "|--------|-------|\n";
        output << "| Total Builds | " << stats.total_builds << " |\n";
        output << "| Total Errors | " << stats.total_errors << " |\n";
        output << "| Resolved Errors | " << stats.resolved_errors << " |\n";
        output << "| Preferences | " << stats.total_preferences << " |\n";
        output << "| Patterns | " << stats.total_patterns << " |\n";
        output << "| Sessions | " << stats.total_sessions << " |\n";
        output << "| Storage Size | " << (stats.total_size_bytes / 1024.0) << " KB |\n";
        output << "| Last Updated | " << format_time(stats.last_updated) << " |\n";
        
        output << "\n### Resolution Rate\n";
        if (stats.total_errors > 0) {
            int rate = (stats.resolved_errors * 100) / stats.total_errors;
            output << "Errors resolved: " << rate << "% (" << stats.resolved_errors << "/" << stats.total_errors << ")\n";
        } else {
            output << "No errors recorded yet.\n";
        }
        
        return json::object({
            {"status", "success"},
            {"output", output.str()},
            {"stats", json::object({
                {"total_builds", stats.total_builds},
                {"total_errors", stats.total_errors},
                {"resolved_errors", stats.resolved_errors},
                {"total_preferences", stats.total_preferences},
                {"total_patterns", stats.total_patterns},
                {"total_sessions", stats.total_sessions},
                {"total_size_bytes", stats.total_size_bytes}
            })}
        });
        
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Error getting memory stats: ") + e.what()}
        });
    }
}
