#pragma once

#include <giomm.h>

#include <functional>
#include <string>
#include <vector>

namespace hv {

// GlobalShortcut: XDG portal client (org.freedesktop.portal.
// GlobalShortcuts) that binds a system-wide key for push-to-talk.
// On Wayland apps cannot see keys unless the compositor forwards
// them; the portal is the sanctioned path — the bind request shows
// a one-time permission dialog, afterwards Activated/Deactivated
// signals arrive over the session bus even when the window is on
// another workspace/activity. Each run creates a fresh live session;
// GNOME persists the approved binding by app id, so later launches restore
// silently (no new dialog).
//
// Usage: construct with an onActivated callback, call bind(). The
// callback fires on the GTK main thread (posted via idle), so it
// may touch widgets directly.
class GlobalShortcuts {
 public:
    explicit GlobalShortcuts(std::function<void()> onActivated);
    ~GlobalShortcuts();

    // Unsubscribe every callback and close the portal session. Idempotent.
    // This is required for mode-scoped bindings: unsubscribing Activated
    // alone leaves the compositor forwarding the key after voice closes.
    void close();

    // Bind (or restore) the shortcut. preferredTrigger uses the freedesktop
    // Shortcuts Specification, e.g. "CTRL+ALT+Return" or
    // "LOGO+ALT+BackSpace" — not a GTK accelerator string. Call once.
    void bind(const std::string& preferredTrigger);

    // True only when BindShortcuts returned push-to-talk in its bound subset.
    bool bound() const { return bound_; }

 private:
    void createSession();
    void bindShortcuts();

    std::function<void()> onActivated_;
    Glib::RefPtr<Gio::DBus::Connection> bus_;
    std::string sessionHandle_;
    std::string sessionToken_;
    std::string pendingBindTrigger_;
    bool binding_ = false;
    bool bound_ = false;
    std::vector<guint> subscriptions_;
    bool closed_ = false;
};

}  // namespace hv
