#include "hivemind/LoopEngine.h"

void LoopEngine::register_loop(const std::string& node_id, int iteration_count) {
    counters_[node_id] = {iteration_count, 0, {}};
}

bool LoopEngine::loop_end_reached(const std::string& loop_start_id,
                                   const std::string& body_text) {
    auto it = counters_.find(loop_start_id);
    if (it == counters_.end()) return false;
    it->second.current++;
    it->second.feedback = body_text;
    return it->second.current < it->second.target;
}

const std::string& LoopEngine::feedback_text(const std::string& loop_start_id) const {
    auto it = counters_.find(loop_start_id);
    static const std::string empty;
    return it != counters_.end() ? it->second.feedback : empty;
}

int LoopEngine::current_iteration(const std::string& loop_start_id) const {
    auto it = counters_.find(loop_start_id);
    return it != counters_.end() ? it->second.current : 0;
}

int LoopEngine::target_iterations(const std::string& loop_start_id) const {
    auto it = counters_.find(loop_start_id);
    return it != counters_.end() ? it->second.target : 0;
}

bool LoopEngine::is_complete(const std::string& loop_start_id) const {
    auto it = counters_.find(loop_start_id);
    return it != counters_.end() && it->second.current >= it->second.target;
}

void LoopEngine::reset() {
    counters_.clear();
}
