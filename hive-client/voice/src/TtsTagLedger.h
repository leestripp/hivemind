#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace hv {

// Exact set of utterances awaiting synthesis/playback completion. A bare
// outstanding-count lets a late callback from a stopped epoch consume a
// NEWER turn's count; tracking tags makes stale completions no-ops.
// Callers serialize access (TtsEngine::m_).
class TtsTagLedger {
 public:
    void add(std::uint64_t tag) { pending_.insert(tag); }
    void clear() { pending_.clear(); }

    // Returns true only for a known tag that completes the final pending
    // utterance. Unknown/duplicate tags must not fire onQueueEmpty.
    bool complete(std::uint64_t tag) {
        if (pending_.erase(tag) == 0) return false;
        return pending_.empty();
    }

    bool contains(std::uint64_t tag) const { return pending_.contains(tag); }
    std::size_t size() const { return pending_.size(); }

 private:
    std::unordered_set<std::uint64_t> pending_;
};

}  // namespace hv
