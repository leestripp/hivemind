#pragma once

#include <mutex>

/// Global serialization for daedalus::Agent lifecycles.
///
/// daedalus v0.9.6 shares global state across Agent instances (settings
/// store, curl globals, MCP manager). Constructing a new Agent while a
/// previous one is still winding down crashes the process with
/// `pthread_mutex_lock ... assertion failed: e != ESRCH || !robust`
/// (observed repeatedly when an agent_loop_end judge started right after
/// the worker agent finished). We don't patch daedalus — we serialize
/// construct..run..destruct of every agent-node execution instead.
/// Parallel agent DAGs therefore run agents one at a time (correct,
/// just not concurrent).
inline std::mutex& agent_exec_mutex() {
    static std::mutex m;
    return m;
}
