/**
 * @file hive-brain/types/ApprovalSource.h
 * @brief Identifies why a bash command was approved without prompting the host.
 *
 * Part of the public C++ API of the the hive-agent engine static library.
 * Stable across minor versions.
 *
 * §BUG-008: ToolHandler can approve a command without ever calling
 * AgentSink::request_command_approval() — via the process-wide auto-approve
 * mode, the in-memory session-approved cache, or the persisted
 * approvals.json cache. Hosts receive AgentSink::on_command_auto_approved()
 * with one of these values so they can render the bypass in their UI.
 */
#pragma once

#include <cstdint>

namespace hive {

enum class ApprovalSource : std::uint8_t {
    AutoApprove,      ///< Process-wide auto-approve mode was ON.
    SessionCache,     ///< Exact/pattern match in the in-memory session-approved set.
    PersistedCache,   ///< Exact/pattern match in the persisted approvals.json set.
};

}  // namespace hive
