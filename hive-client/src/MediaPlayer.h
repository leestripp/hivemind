#pragma once

// Child-process media playback for attachment cards.
//
// 2026-10-21 — the "fix it right" rewrite. Gtk::MediaFile / Gtk::Video run
// GStreamer decodebin3 IN this process, and decodebin3 has a fatal race
// (mq_slot_handle_stream_start: assertion failed: (collection)) that is a
// g_error — uncatchable, kills the whole app. Two band-aids (lazy creation,
// one pipeline at a time) only lowered the odds. The real fix is that NO
// GStreamer pipeline ever runs in the GUI process: playback and poster
// extraction happen in short-lived child processes (gst-play-1.0, ffmpeg).
// If a media file crashes the decoder now, a child dies and the card shows
// its fallback chip — the client stays alive.

#include <functional>
#include <string>

#include <glib.h>   // GPid

namespace media_player {

// Called on the GTK main thread when a child started by play() exits.
// ok == false means a nonzero/signalled exit; elapsed is how long the
// child lived. gst-play's EOS exit code is unreliable in some
// environments, so the caller should treat  !ok && elapsed is short  as
// "never played" (decoder failure → fallback) and anything after a
// couple of seconds as a natural end.
using ExitCallback =
    std::function<void(GPid pid, bool ok, double elapsed_seconds)>;

// Spawn gst-play-1.0 on `file`. Audio plays on the default sink; video
// opens its own window. Returns the child pid (never 0 on success) and
// arranges on_exit to fire on the main thread. Throws std::runtime_error
// if the spawn itself fails (binary missing etc.).
GPid play(const std::string& file, ExitCallback on_exit);

// Stop a child started by play(). Safe on an already-exited pid as long
// as the caller serializes with the exit callback (main thread does).
void stop(GPid pid);

// Extract the first frame of `video` into `png_out` (scaled to 640 wide).
// SYNCHRONOUS — call from a worker thread, never the GTK thread.
// Returns true when the png was written.
bool make_poster(const std::string& video, const std::string& png_out);

}   // namespace media_player
