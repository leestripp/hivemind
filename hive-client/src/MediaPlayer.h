#pragma once

// Poster extraction for video attachment cards.
//
// Playback is not in this module: WebKit builds embed video controls,
// while the non-WebKit video card and audio attachments hand off to the
// desktop's assigned app. Only the ffmpeg-child poster remains here; a
// decoder crash in the child can never touch the GUI process.

#include <string>

namespace media_player {

// Extract the first frame of `video` into `png_out` (scaled to 640 wide).
// SYNCHRONOUS — call from a worker thread, never the GTK thread.
// Returns true when the png was written.
bool make_poster(const std::string& video, const std::string& png_out);

}   // namespace media_player
