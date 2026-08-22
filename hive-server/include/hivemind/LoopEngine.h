#pragma once

#include <string>
#include <unordered_map>
#include <cstddef>

/**
 * LoopEngine — manages loop iteration state and feedback text across a run.
 *
 * One LoopEngine per GraphRun (not shared).
 */
class LoopEngine {
public:
    /**
     * Register a loop_start node with a target iteration count.
     */
    void register_loop(const std::string& node_id, int iteration_count);

    /**
     * Record that loop_end was reached for this loop.
     * Increments the counter, stores the body text as feedback for the next
     * iteration, and returns true if the loop should jump back.
     */
    bool loop_end_reached(const std::string& loop_start_id,
                          const std::string& body_text);

    /**
     * Feedback text to feed into loop_start:in on the next iteration.
     */
    const std::string& feedback_text(const std::string& loop_start_id) const;

    /**
     * How many iterations have been completed for this loop.
     */
    int current_iteration(const std::string& loop_start_id) const;

    /**
     * Total target iterations for this loop.
     */
    int target_iterations(const std::string& loop_start_id) const;

    /**
     * Is the loop complete (current >= target)?
     */
    bool is_complete(const std::string& loop_start_id) const;

    /**
     * Reset all state (for a fresh run).
     */
    void reset();

    /**
     * How many loops are currently tracked?
     */
    std::size_t active_loop_count() const noexcept { return counters_.size(); }

private:
    struct LoopState {
        int target = 0;
        int current = 0;
        std::string feedback;
    };
    std::unordered_map<std::string, LoopState> counters_;
};
