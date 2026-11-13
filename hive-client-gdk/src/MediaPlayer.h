#pragma once

// Poster extraction for video attachment cards.
//
// 2026-10-23: playback left this module entirely — clicking a video or
// audio attachment now opens the file in the DESKTOP'S assigned app via
// Gio::AppInfo::launch_default_for_uri (xdg-open equivalent). Only the
// ffmpeg-child poster (first-frame thumbnail) remains here, and it is
// not a player: a decoder crash in the child can never touch the GUI
// process.

#include <string>

namespace media_player {

// Extract the first frame of `video` into `png_out` (scaled to 640 wide).
// SYNCHRONOUS — call from a worker thread, never the GTK thread.
// Returns true when the png was written.
bool make_poster(const std::string& video, const std::string& png_out);

}   // namespace media_player
