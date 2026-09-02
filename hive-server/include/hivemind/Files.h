#pragma once

#include <filesystem>
#include <optional>
#include <string>

/**
 * Files — shared-workspace path helpers (Phase 18: chat attachments +
 * artifacts browser).
 *
 * Every agent container bind-mounts the company workspace at /workspace,
 * so a file saved under the per-worker workspace attachments/<thread_id>/ is
 * immediately readable by agents via their ordinary file tools — no
 * per-container copy step.
 *
 * All three helpers are pure (no globals) so tests exercise them directly.
 */
namespace hm::files {

/// Reduce an untrusted upload name to a safe bare filename: strips any
/// directory components, maps every char outside [A-Za-z0-9._-] to '_',
/// collapses leading dots (no hidden files / ".."), falls back to "file"
/// when nothing usable remains.
std::string sanitize_filename(const std::string& untrusted);

/// First non-existing path for `name` inside `dir`: "name", then
/// "name-1", "name-2" … (suffix inserted before the extension).
/// Does NOT create anything.
std::filesystem::path unique_path(const std::filesystem::path& dir,
                                  const std::string& name);

/// Resolve `rel` (from a query parameter) against `root`, rejecting
/// anything that escapes the root (absolute paths, "..", symlink hops).
/// Returns nullopt on escape or when the target does not exist.
/// `root` need not exist on disk for pure path math, but the returned
/// path is weakly_canonical against the real filesystem when it does.
std::optional<std::filesystem::path>
resolve_confined(const std::filesystem::path& root, const std::string& rel);

/// Same confinement as resolve_confined but for WRITES: the target itself
/// may not exist yet (it will be created); its PARENT must exist and be
/// confined. Rejects absolute paths, ".." escapes, and symlink hops.
std::optional<std::filesystem::path>
resolve_confined_create(const std::filesystem::path& root,
                        const std::string& rel);

} // namespace hm::files
