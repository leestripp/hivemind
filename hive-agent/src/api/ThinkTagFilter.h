// SPDX-License-Identifier: BSL-1.0
// src/api/ThinkTagFilter.h
//
// Private header for `ThinkTagFilter` — a stream-safe stripper for inline
// `<think>...</think>` reasoning blocks that some models embed in the
// `content` channel (qwen3-family, MiniMax when `reasoning_split` is not
// honored, deepseek-style models). Providers that send reasoning as the
// `reasoning_content` *field* (Kimi/Moonshot) never touch this — that
// channel is accumulated separately.
//
// Why in the library and not the host: SSE deltas split tags across chunk
// boundaries ("</th" + "ink>"), so app-level string replacement misfires
// and leaks stray `</think>` into UIs. The filter holds back bytes that
// could be a tag prefix until the next delta disambiguates them.
//
// Behavior:
//   - Text outside a think block passes through unchanged.
//   - `<think>` ... `</think>` (including the tags) is dropped.
//   - A stray `</think>` outside a block strips just the tag — covers
//     providers that open reasoning implicitly but still emit the closer.
//   - Unterminated `<think>` at flush(): the held remainder is dropped
//     (it was reasoning; a never-closed think block has no visible tail).
//   - Held bytes that turn out NOT to be a tag are re-emitted verbatim —
//     including at flush(), where a partial tag prefix cut by end-of-stream
//     comes back as literal text (never drop unconfirmed text).
//
// Not part of the public libdaedalus API. Do not include from
// include/hive-brain/* or from any header that ends up in a host's
// translation unit.

#pragma once

#include <string>
#include <string_view>

class ThinkTagFilter {
public:
    /// Feed one content delta; returns the text visible to the user.
    std::string feed(std::string_view delta) {
        pending_ += delta;
        std::string out;
        size_t pos = 0;
        while (pos < pending_.size()) {
            if (pending_[pos] != '<') {
                const size_t next = pending_.find('<', pos);
                const size_t end  = (next == std::string::npos)
                                        ? pending_.size() : next;
                if (!in_think_) out.append(pending_, pos, end - pos);
                pos = end;
                continue;
            }
            // At '<': full tag, stray closer, possible partial tag, or
            // literal '<'.
            const std::string_view rest(pending_.data() + pos,
                                        pending_.size() - pos);
            const std::string_view tag = in_think_ ? kClose : kOpen;
            if (rest.starts_with(tag)) {
                in_think_ = !in_think_;
                pos += tag.size();
                continue;
            }
            if (!in_think_ && rest.starts_with(kClose)) {
                // Stray </think> with no matching opener — strip the tag.
                pos += kClose.size();
                continue;
            }
            const bool could_be_tag =
                tag.starts_with(rest) ||
                (!in_think_ && kClose.starts_with(rest));
            if (could_be_tag) break;  // hold pending_ for the next delta
            if (!in_think_) out += '<';
            pos += 1;
        }
        pending_.erase(0, pos);
        return out;
    }

    /// End of stream: re-emit held bytes when they were literal text;
    /// drop them when the stream ended inside a think block.
    std::string flush() {
        std::string out;
        if (!in_think_) out = std::move(pending_);
        pending_.clear();
        in_think_ = false;
        return out;
    }

    /// One-shot helper for buffered (non-streaming) content.
    static std::string strip(std::string_view text) {
        ThinkTagFilter f;
        std::string out = f.feed(text);
        out += f.flush();
        return out;
    }

private:
    static constexpr std::string_view kOpen  = "<think>";
    static constexpr std::string_view kClose = "</think>";

    bool        in_think_ = false;
    std::string pending_;
};
