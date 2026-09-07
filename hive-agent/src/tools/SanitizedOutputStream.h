#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

/**
 * SanitizedOutputStream — incremental TTY-output sanitiser with
 * line-aware, rate-limited chunk emission.
 *
 * Tools that stream live stdout/stderr to a host (e.g. BashTool via
 * ToolProgressFn "output_chunk" events) feed raw subprocess bytes to
 * append() and receive safe-to-display chunks through the callback.
 *
 * Reassembly guarantee: for ANY input and ANY append() split pattern,
 * the concatenation of all emitted chunks is byte-identical to the
 * one-shot sanitisation of the whole input (see sanitize()). This holds
 * because sanitisation is INCREMENTAL — parser state (escape sequences,
 * UTF-8 validation, CR/LF normalisation) persists across feeds, and only
 * already-sanitised bytes are ever sliced for emission. A chunk boundary
 * can therefore never split an ANSI escape sequence, a multi-byte UTF-8
 * character, or a CR/LF pair.
 *
 * Sanitisation rules (same as the historical
 * BashCommandExecutor::sanitize_tty_sequences):
 *   - Preserved: printable ASCII, tab, LF, valid UTF-8, complete SGR
 *     colour/style sequences (ESC [ ... m).
 *   - Stripped:  cursor movement, screen erase, alternate buffer, OSC,
 *     DCS/APC/PM/SOS strings, other C0 controls, DEL, invalid UTF-8.
 *   - CR and CR/LF are normalised to a single LF.
 *   - An incomplete escape sequence or partial UTF-8 character at end of
 *     stream (flush()) is dropped.
 *
 * Threading: not internally synchronised — the caller must serialise
 * append()/flush() (Subprocess delivers output from a single reader
 * thread, so single-producer use needs no extra locking).
 */
class SanitizedOutputStream {
public:
    using ChunkCallback = std::function<void(std::string_view)>;

    explicit SanitizedOutputStream(ChunkCallback cb,
                                   std::size_t max_line_buffer = 4096,
                                   int min_interval_ms = 100);

    /// Feed raw subprocess output. Sanitises incrementally and may emit
    /// a chunk via the callback (on line boundaries, or when the buffered
    /// sanitised output exceeds max_line_buffer bytes / min_interval_ms).
    void append(std::string_view raw);

    /// End of stream: resolve any held bytes (a trailing CR is emitted as
    /// LF; an incomplete escape sequence or partial UTF-8 character is
    /// dropped) and emit whatever sanitised output remains.
    void flush();

    /// One-shot equivalent: sanitise a complete buffer. Identical to
    /// feeding `input` through append() + flush() and concatenating the
    /// emitted chunks.
    static std::string sanitize(const std::string& input);

private:
    // Incremental sanitiser state (persists across append() calls).
    enum class State {
        Normal,
        Escape,          // saw ESC
        EscIntermediate, // ESC + one or more intermediate bytes (0x20-0x2F)
        CSI,             // ESC [ collecting parameter bytes
        CSIIntermediate, // ESC [ params + intermediate bytes
        OSC,             // ESC ] collecting until BEL or ST
        OSC_ST,          // saw ESC inside OSC, expect '\' to terminate
        String,          // ESC P/^/_/X collecting until ST
        String_ST        // saw ESC inside String, expect '\'
    };

    /// Process pending_ as far as safely possible, appending sanitised
    /// bytes to sanitized_ and leaving any undecidable tail (a trailing
    /// CR, or a UTF-8 prefix awaiting continuation bytes) in pending_.
    void sanitize_feed();

    /// Emit a slice of the sanitised buffer per the rate/line policy.
    void try_emit(bool force);

    ChunkCallback callback_;

    // Sanitiser state.
    State state_ = State::Normal;
    std::string pending_;        // raw bytes not yet decidable (CR / UTF-8)
    std::string csi_buffer_;     // bytes collected for a CSI sequence
    bool csi_has_intermediate_ = false;

    // Sanitised output awaiting emission.
    std::string sanitized_;
    std::size_t max_line_buffer_;
    int min_interval_ms_;
    std::chrono::steady_clock::time_point last_emit_;
};
