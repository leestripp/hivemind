// SPDX-License-Identifier: BSL-1.0
// src/library/net/SseParser.h
//
// §NC-2 (2026-07-16): client-side Server-Sent Events (SSE) parser.
//
// OmniNet has SSEHandler for server-side *emission* (SSEHandler.cpp),
// but no parser for client-side consumption. This header provides one.
//
// Spec: WHATWG HTML "Server-Sent Events"
//   https://html.spec.whatwg.org/multipage/server-sent-events.html
//
// Wire format:
//
//   field: value\n
//   field: value\n
//   \n                         <- empty line ends the event
//   field: value\n
//   \n
//
// Fields:  id, event, data, retry
// A line beginning with ':' is a comment and ignored.
// Lines are CRLF or LF terminated.
// `data` may span multiple lines; each is joined with '\n'.
// A blank line dispatches the accumulated event (if any data fields were seen).
//
// Usage:
//
//   SseParser parser;
//   parser.feed(chunk_from_socket);          // call repeatedly with bytes
//   while (auto ev = parser.try_pop()) {     // drain events
//       handle(*ev);
//   }
//
// The parser is stateless across chunks — partial lines at chunk
// boundaries are buffered until the next chunk completes them.
// The caller is responsible for the wire (HTTP::ClientResponseStream
// or any other byte source); this parser only knows bytes → events.
//
// §Thread-safety: not thread-safe. One parser per stream.
// §Library rule: no stdio. Lives in src/library so it can be linked
//   into libdaedalus.a without violating the no-stdio rule.

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace hive::net {

/// One parsed SSE event. `data` is the joined data lines (separator '\n').
struct SseEvent {
    std::string id;        // last `id:` line; empty if never seen
    std::string event;     // last `event:` line; empty → defaults to "message"
    std::string data;      // joined data lines; empty if no `data:` seen
    int         retry = 0; // last `retry:` line in ms; 0 if never seen
};

class SseParser {
public:
    /// Push more bytes into the parser. May emit zero or more events
    /// via try_pop(). Call repeatedly until your stream ends.
    void feed(std::string_view chunk) {
        buffer_.append(chunk);
        // Each line is terminated by \n (we normalise \r\n → \n).
        while (true) {
            const auto nl = buffer_.find('\n');
            if (nl == std::string::npos) break;
            // Strip optional trailing '\r' (CRLF).
            std::string line(buffer_, 0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            handle_line(line);
            buffer_.erase(0, nl + 1);
        }
    }

    /// Pop the next fully-parsed event, or nullopt if none ready.
    std::optional<SseEvent> try_pop() {
        if (events_.empty()) return std::nullopt;
        SseEvent ev = std::move(events_.front());
        events_.pop_front();
        return ev;
    }

    /// True if no events are buffered and the input buffer is empty.
    bool idle() const noexcept {
        return events_.empty() && buffer_.empty();
    }

    /// Finish parsing. Flushes any in-flight event whose terminator
    /// was missing at end-of-stream. Call once when the underlying
    /// stream reports EOF.
    void finish() {
        if (!buffer_.empty()) {
            // Stream ended mid-line; treat the partial line as a final line.
            handle_line(buffer_);
            buffer_.clear();
        }
        flush_event();
    }

private:
    void handle_line(const std::string& line) {
        // Blank line — flush the in-flight event (if it had any data).
        if (line.empty()) { flush_event(); return; }

        // Comment line — ignore.
        if (line[0] == ':') return;

        // Split on first ':'.
        const auto colon = line.find(':');
        std::string field, value;
        if (colon == std::string::npos) {
            field = line;
            value = "";
        } else {
            field = line.substr(0, colon);
            value = line.substr(colon + 1);
            // Per spec: a single leading space after the colon is stripped.
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
        }

        if (field == "id") {
            current_.id = value;
        } else if (field == "event") {
            current_.event = value;
        } else if (field == "data") {
            // Per spec, an explicit `data:` (even with empty value) IS a
            // data field. Track this so flush_event() can distinguish
            // "data: \n\n" (dispatch, data=="") from "no data field at
            // all" (skip).
            if (had_data_field_) current_.data.push_back('\n');
            else                  had_data_field_ = true;
            current_.data.append(value);
        } else if (field == "retry") {
            // Per spec: only parse if all chars are ASCII digits.
            int v = 0;
            for (char c : value) {
                if (c < '0' || c > '9') return;
                v = v * 10 + (c - '0');
            }
            current_.retry = v;
        }
        // Unknown fields are silently ignored.
    }

    void flush_event() {
        if (!had_data_field_) {
            // Per spec: events without any data field are not dispatched.
            current_ = SseEvent{};
            had_data_field_ = false;
            return;
        }
        events_.push_back(std::move(current_));
        current_ = SseEvent{};
        had_data_field_ = false;
    }

    std::string         buffer_;    // partial line accumulator
    std::deque<SseEvent> events_;
    SseEvent            current_;   // accumulator for the in-flight event
    bool                had_data_field_ = false;
};

} // namespace hive::net