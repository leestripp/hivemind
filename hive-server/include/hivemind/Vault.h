#pragma once

#include <optional>
#include <string>

/**
 * Vault — Phase 22: external vault adapter for the secret broker.
 *
 * A secret can be backed by an EXTERNAL source instead of local AES-GCM
 * storage; the value is fetched from the user's vault CLI at delivery
 * time (env injection / browser fill) and never persists in secrets.db:
 *
 *   pass:work/github-token     → pass show -- work/github-token (1st line)
 *   op://Private/GitHub/token  → op read "op://Private/GitHub/token"
 *
 * References pass a strict charset allowlist AND single-quoting before
 * the shell (same rule as the skill installer). Binaries resolve from
 * PATH, overridable via HIVEMIND_PASS_BIN / HIVEMIND_OP_BIN (tests use
 * stub scripts). Fetches are capped with `timeout 15`.
 */
namespace hm::vault {

/// Validate a source reference. Returns "" if valid, else a reason.
std::string validate_source(const std::string& source);

/// Fetch the value for a valid source. nullopt when the scheme is
/// unknown, the CLI is missing, or the command fails/times out.
/// The value is caller-only — never logged.
std::optional<std::string> fetch(const std::string& source);

} // namespace hm::vault
