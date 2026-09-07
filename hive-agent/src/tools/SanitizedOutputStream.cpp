#include "tools/SanitizedOutputStream.h"

#include "core/Utf8Utils.h"

namespace {
constexpr unsigned char ESC = 0x1B;
constexpr unsigned char BEL = 0x07;
}

SanitizedOutputStream::SanitizedOutputStream(ChunkCallback cb,
                                             std::size_t max_line_buffer,
                                             int min_interval_ms)
    : callback_(std::move(cb)),
      max_line_buffer_(max_line_buffer),
      min_interval_ms_(min_interval_ms),
      last_emit_(std::chrono::steady_clock::now()) {}

void SanitizedOutputStream::append(std::string_view raw) {
    if (!callback_) return;
    pending_.append(raw);
    sanitize_feed();
    try_emit(false);
}

void SanitizedOutputStream::flush() {
    if (!callback_) return;
    // Resolve held bytes. A trailing CR becomes LF (same as one-shot
    // sanitisation of a buffer ending in CR). A partial UTF-8 prefix or
    // an incomplete escape sequence is dropped — the historical
    // documented behaviour for truncated output.
    if (!pending_.empty() && pending_.size() == 1 && pending_[0] == '\r') {
        sanitized_.push_back('\n');
    }
    pending_.clear();
    state_ = State::Normal;
    csi_buffer_.clear();
    csi_has_intermediate_ = false;
    try_emit(true);
}

std::string SanitizedOutputStream::sanitize(const std::string& input) {
    if (input.empty()) return input;
    std::string out;
    out.reserve(input.size());
    SanitizedOutputStream stream(
        [&out](std::string_view chunk) { out.append(chunk); });
    stream.append(input);
    stream.flush();
    return out;
}

void SanitizedOutputStream::sanitize_feed() {
    std::size_t i = 0;
    const std::size_t n = pending_.size();

    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(pending_[i]);

        switch (state_) {
        case State::Normal:
            if (c == ESC) {
                state_ = State::Escape;
                ++i;
            } else if (c == '\r') {
                // Normalise CR and CR/LF to a single LF. A CR at the very
                // end of the pending bytes is undecidable — the next feed
                // might start with LF — so hold it back.
                if (i + 1 >= n) {
                    goto done;
                }
                if (pending_[i + 1] == '\n') {
                    ++i; // skip the following LF
                }
                sanitized_.push_back('\n');
                ++i;
            } else if (c == '\t' || c == '\n') {
                sanitized_.push_back(static_cast<char>(c));
                ++i;
            } else if (c < 0x20 || c == 0x7F) {
                // Drop other C0 controls (bell, backspace, vertical tab,
                // form feed) and DEL.
                ++i;
            } else if ((c & 0x80) != 0) {
                // Start of a multi-byte UTF-8 sequence.
                const std::size_t char_len = utf8_char_length_from_first_byte(c);
                if (char_len > 1) {
                    if (i + char_len <= n) {
                        if (is_valid_utf8_char(pending_.data(), i, n)) {
                            sanitized_.append(pending_, i, char_len);
                            i += char_len;
                        } else {
                            ++i; // invalid lead byte: drop it
                        }
                    } else {
                        // Not enough bytes yet. Hold the prefix only if it
                        // could still complete (all available bytes are
                        // continuations); otherwise the lead byte is
                        // provably invalid — drop it and re-examine the
                        // rest as fresh starts, matching one-shot.
                        bool could_complete = true;
                        for (std::size_t j = i + 1; j < n; ++j) {
                            if (!is_utf8_continuation_byte(
                                    static_cast<unsigned char>(pending_[j]))) {
                                could_complete = false;
                                break;
                            }
                        }
                        if (could_complete) {
                            goto done;
                        }
                        ++i;
                    }
                } else {
                    ++i; // invalid high byte: drop it
                }
            } else {
                // Printable ASCII.
                sanitized_.push_back(static_cast<char>(c));
                ++i;
            }
            break;

        case State::Escape:
            if (c == '[') {
                state_ = State::CSI;
                csi_buffer_.clear();
                csi_buffer_.push_back(static_cast<char>(ESC));
                csi_buffer_.push_back('[');
                csi_has_intermediate_ = false;
            } else if (c == ']') {
                state_ = State::OSC;
            } else if (c == 'P' || c == '^' || c == '_' || c == 'X') {
                state_ = State::String;
            } else if (c >= 0x30 && c <= 0x7E) {
                // ESC + final byte (e.g. ESC c, ESC 7, ESC 8). Drop.
                state_ = State::Normal;
            } else if (c >= 0x20 && c <= 0x2F) {
                // ESC + intermediate byte; wait for final byte.
                state_ = State::EscIntermediate;
            } else {
                // Unrecognised ESC sequence: drop ESC and this byte.
                state_ = State::Normal;
            }
            ++i;
            break;

        case State::EscIntermediate:
            if (c >= 0x30 && c <= 0x7E) {
                // Final byte reached; drop the whole ESC sequence.
                state_ = State::Normal;
            } else if (c < 0x20 || c > 0x2F) {
                // Invalid byte in the middle of the sequence; recover.
                state_ = State::Normal;
            }
            // Otherwise remain in EscIntermediate waiting for the final byte.
            ++i;
            break;

        case State::CSI:
            csi_buffer_.push_back(static_cast<char>(c));
            ++i;
            if (c >= 0x30 && c <= 0x3F) {
                // Parameter byte: remain in CSI.
            } else if (c >= 0x20 && c <= 0x2F) {
                csi_has_intermediate_ = true;
                state_ = State::CSIIntermediate;
            } else if (c >= 0x40 && c <= 0x7E) {
                // Final byte. Keep only Select Graphic Rendition (SGR),
                // which uses final byte 'm' with no intermediate bytes.
                if (c == 'm' && !csi_has_intermediate_) {
                    sanitized_.append(csi_buffer_);
                }
                state_ = State::Normal;
                csi_buffer_.clear();
                csi_has_intermediate_ = false;
            } else {
                // Malformed CSI; recover to normal state.
                state_ = State::Normal;
                csi_buffer_.clear();
                csi_has_intermediate_ = false;
            }
            break;

        case State::CSIIntermediate:
            csi_buffer_.push_back(static_cast<char>(c));
            ++i;
            if (c >= 0x20 && c <= 0x2F) {
                // Another intermediate byte.
            } else if (c >= 0x40 && c <= 0x7E) {
                // Final byte reached. Not SGR (SGR has no intermediates),
                // so drop the sequence.
                state_ = State::Normal;
                csi_buffer_.clear();
                csi_has_intermediate_ = false;
            } else {
                // Malformed; recover to normal state.
                state_ = State::Normal;
                csi_buffer_.clear();
                csi_has_intermediate_ = false;
            }
            break;

        case State::OSC:
            if (c == ESC) {
                state_ = State::OSC_ST;
            } else if (c == BEL) {
                // BEL-terminated OSC (xterm legacy). Drop.
                state_ = State::Normal;
            }
            // Otherwise remain in OSC, dropping bytes.
            ++i;
            break;

        case State::OSC_ST:
            if (c == '\\') {
                // String terminator (ESC \). Drop.
                state_ = State::Normal;
            } else {
                // Not a terminator; continue OSC.
                state_ = State::OSC;
            }
            ++i;
            break;

        case State::String:
            if (c == ESC) {
                state_ = State::String_ST;
            }
            // Otherwise remain in String, dropping bytes.
            ++i;
            break;

        case State::String_ST:
            if (c == '\\') {
                // String terminator. Drop.
                state_ = State::Normal;
            } else {
                state_ = State::String;
            }
            ++i;
            break;
        }
    }

done:
    pending_.erase(0, i);
}

void SanitizedOutputStream::try_emit(bool force) {
    if (sanitized_.empty()) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_emit_).count();

    // Prefer to emit on line boundaries so the streaming display is
    // coherent. The buffer holds only sanitised bytes (printable text,
    // complete UTF-8, complete SGR sequences), so ANY cut point is safe
    // and concatenation of chunks is exact.
    std::size_t emit_end = sanitized_.find_last_of('\n');
    bool has_newline = emit_end != std::string::npos;
    if (has_newline) {
        emit_end += 1; // include the newline
    }

    bool threshold_reached = sanitized_.size() >= max_line_buffer_;
    bool time_elapsed = elapsed_ms >= min_interval_ms_;

    if (!force && !has_newline && !threshold_reached && !time_elapsed) {
        return;
    }

    // If we have no newline but are emitting for threshold/time reasons,
    // emit the whole current buffer. This is rare (very long single line).
    if (!has_newline) {
        emit_end = sanitized_.size();
    }

    if (emit_end == 0) return;

    callback_(std::string_view(sanitized_.data(), emit_end));
    sanitized_.erase(0, emit_end);
    last_emit_ = now;
}
