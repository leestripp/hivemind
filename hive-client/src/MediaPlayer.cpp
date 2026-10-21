#include "MediaPlayer.h"

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <csignal>
#include <stdexcept>
#include <vector>

#include <glibmm.h>

namespace media_player {

GPid play(const std::string& file, ExitCallback on_exit) {
    std::vector<std::string> argv = {"gst-play-1.0", file};
    GPid pid = 0;
    Glib::spawn_async(
        "", argv,
        Glib::SpawnFlags::SEARCH_PATH | Glib::SpawnFlags::DO_NOT_REAP_CHILD |
            Glib::SpawnFlags::STDOUT_TO_DEV_NULL |
            Glib::SpawnFlags::STDERR_TO_DEV_NULL,
        {}, &pid);
    if (pid == 0)
        throw std::runtime_error("media_player: spawn returned pid 0");

    // Fires on the main context when the child exits. We own the reaping.
    const auto started = std::chrono::steady_clock::now();
    Glib::signal_child_watch().connect(
        [on_exit = std::move(on_exit), started](GPid done_pid, int status) {
            const double elapsed =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            std::fprintf(stderr,
                         "[media_player] exit pid=%d ok=%d after=%.1fs "
                         "(exit=%d sig=%d)\n",
                         (int)done_pid, (int)ok, elapsed,
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                         WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            Glib::spawn_close_pid(done_pid);
            on_exit(done_pid, ok, elapsed);
        },
        pid);
    return pid;
}

void stop(GPid pid) {
    if (pid != 0)
        ::kill(pid, SIGTERM);   // gst-play exits cleanly on SIGTERM
}

bool make_poster(const std::string& video, const std::string& png_out) {
    std::vector<std::string> argv = {
        "ffmpeg", "-y", "-loglevel", "error", "-i", video,
        "-frames:v", "1", "-vf", "scale=640:-2", png_out,
    };
    try {
        int status = 0;
        // SEARCH_PATH: without it glib execs "ffmpeg" relative to cwd —
        // ENOENT despite /usr/bin/ffmpeg existing.
        Glib::spawn_sync("", argv,
                         Glib::SpawnFlags::SEARCH_PATH |
                             Glib::SpawnFlags::STDOUT_TO_DEV_NULL |
                             Glib::SpawnFlags::STDERR_TO_DEV_NULL,
                         {}, nullptr, nullptr, &status);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!ok)
            std::fprintf(stderr, "[media_player] poster ffmpeg rc=%d: %s\n",
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                         video.c_str());
        return ok;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[media_player] poster spawn failed: %s\n",
                     e.what());
        return false;   // ffmpeg missing, unspawnable — no poster
    }
}

}   // namespace media_player
