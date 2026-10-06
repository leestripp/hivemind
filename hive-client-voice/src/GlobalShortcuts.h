#pragma once

#include <giomm.h>

#include <functional>
#include <string>

namespace hv {

// GlobalShortcut: XDG portal client (org.freedesktop.portal.
// GlobalShortcuts) that binds a system-wide key for push-to-talk.
// On Wayland apps cannot see keys unless the compositor forwards
// them; the portal is the sanctioned path — the bind request shows
// a one-time permission dialog, afterwards Activated/Deactivated
// signals arrive over the session bus even when the window is on
// another workspace/activity. The session token is persisted so
// later launches restore silently (no new dialog).
//
// Usage: construct with an onActivated callback, call bind(). The
// callback fires on the GTK main thread (posted via idle), so it
// may touch widgets directly.
class GlobalShortcuts {
 public:
    explicit GlobalShortcuts(std::function<void()> onActivated);
    ~GlobalShortcuts();

    // Bind (or restore) the shortcut. trigger is a GTK accelerator
    // string, e.g. "<Control><Super>space". Safe to call repeatedly.
    void bind(const std::string& trigger);

 private:
    void createSession();
    void bindShortcuts();

    std::function<void()> onActivated_;
    Glib::RefPtr<Gio::DBus::Connection> bus_;
    std::string sessionHandle_;
    std::string sessionToken_;
    std::string pendingBindTrigger_;
    bool binding_ = false;
    guint subActivated_ = 0;
};

}  // namespace hv
