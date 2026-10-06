#pragma once

#include <gtkmm.h>

#include <atomic>
#include <string>
#include <thread>

namespace hv {

// DebugShell: line-based command server on a unix socket so the running
// app can be driven and screenshotted live from scripts — agent
// self-testing on Wayland, where external screenshot tools are blocked.
//
// Protocol: client connects, sends one command line, half-closes, reads
// the text response until EOF.
//
//   help                     list commands
//   status                   JSON: windows, voice phase/agent, GL info
//   windows                  list app windows (title, size, visible)
//   shot <path.png> [title]  render a window to PNG via the Gsk renderer
//   agents                   dump the last roster poll
//   picker <Name>            open the voice picker for an agent
//   open <agent name>        open a voice session (substring, online only)
//   say <text>               inject text as if it were transcribed speech
//   phase <idle|listening|thinking|speaking|processing>
//   level <0..1>
//   glinfo                   GL renderer/version strings from the sphere
//   talk                     toggle the Talk button (global-shortcut hook)
//
// Socket: $HIVE_VOICE_CTL, else $XDG_RUNTIME_DIR/hive-voice-ctl.sock,
// else /tmp/hive-voice-ctl-<uid>.sock.
class DebugShell {
 public:
    explicit DebugShell(Gtk::Application& app);
    ~DebugShell();

    // Page registry (single-window app; latest registration wins).
    static void registerRoster(class RosterView* w);
    static void registerVoice(class VoiceView* w);  // nullptr on page close
    static class VoiceView* voiceView();  // current page, or nullptr

 private:
    void serveLoop();
    std::string handle(const std::string& line);
    std::string shot(const std::string& path, const std::string& titleSub);

    Gtk::Application& app_;
    int listenFd_ = -1;
    std::string path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace hv
