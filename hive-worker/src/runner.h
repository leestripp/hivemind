#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hwrunner {

struct ExecOutcome {
    int exit_code = -1;
    bool timed_out = false;
    int64_t duration_ms = 0;
    std::string error;      // "" on success; infra errors (no podman etc.)
};

/// Ensure the agent's container exists + runs on THIS worker, then
/// `podman exec` argv inside it, streaming combined stdout/stderr via
/// on_chunk. env_content (may be "") is written to a 0600 tmpfs file and
/// passed as --env-file; deleted when the exec returns.
///
/// Security: argv is executed VERBATIM (execvp, no shell) — the worker
/// never interprets a command string from the wire.
ExecOutcome run_container_exec(const std::string& agent,
                               const std::vector<std::string>& argv,
                               const std::string& env_content,
                               int timeout_s,
                               const std::function<void(std::string_view)>& on_chunk);

} // namespace hwrunner
