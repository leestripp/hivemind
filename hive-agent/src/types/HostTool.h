/**
 * @file hive-brain/types/HostTool.h
 * @brief Deprecated alias for hive::BaseTool.
 *
 * @deprecated Use `hive::BaseTool` instead. This header and name are
 *             kept for backward compatibility and will be removed in a
 *             future release.
 */
#pragma once

#include "types/BaseTool.h"

namespace hive {

using HostTool [[deprecated("Use hive::BaseTool instead")]] = BaseTool;

}  // namespace hive
