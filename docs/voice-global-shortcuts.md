# Voice global shortcuts on GNOME/Wayland

This is the permanent runbook for HiveMind's embedded voice push-to-talk key.
It records the November 2026 failure, the verified implementation, and the
checks to use before changing this code again.

The implementation is
[`hive-client/voice/src/GlobalShortcuts.cpp`](../hive-client/voice/src/GlobalShortcuts.cpp).
The voice-page lifecycle and callback guard are in
[`hive-client/src/HiveWindow.cpp`](../hive-client/src/HiveWindow.cpp).

## Current behavior and safety boundary

- **Super+Alt+Backspace** toggles the same Talk/Send action as the voice page's
  button.
- The binding exists only while an embedded voice page is open. Back, thread
  navigation, and window close unsubscribe and close the portal session.
- GNOME Shell delivers an XDG portal `Activated` D-Bus signal; it does **not**
  deliver the physical key as a GTK/GDK key event.
- Do not replace this with a compositor grab, X11 key grab, GNOME Shell restart,
  or a second global-input mechanism.
- Never choose a key used by lee's magnifier, zoom, media keys, NumLock, or
  other accessibility bindings. Any new global/input binding requires explicit
  user approval first. The Backspace combination was explicitly requested and
  tested; it does not reuse the dangerous KP_Insert binding.

## The four bugs that made the key do nothing

The integrated implementation inherited several independent problems. Fixing
only one still left a silent failure:

1. **Wrong option and syntax:** the code supplied `trigger` with a GTK
   accelerator string. XDG requires `preferred_trigger` with freedesktop XKB
   syntax. The portal silently discarded the unsupported `trigger` option.
2. **Wrong portal application identity:** the shared `g_bus_get_sync()`
   connection could already be associated with the host/launcher identity.
   Explicit `Registry.Register` on that connection failed with `Connection
   already associated with an application ID.` Without explicit registration,
   the portal could select another application's saved binding.
3. **Wrong signal object:** `Activated` is emitted on the portal root object,
   `/org/freedesktop/portal/desktop`, not on the Session object. The Session
   handle is parameter 0 of the signal.
4. **Wrong giomm lambda argument:** the callback treated `interface_name` as
   `object_path`. It therefore compared
   `org.freedesktop.portal.GlobalShortcuts` with
   `/org/freedesktop/portal/desktop` and rejected every real activation.

A fifth problem affected diagnosis rather than the product: multiple leftover
probe processes competed for the same app ID and key. Killing background
wrapper PIDs did not kill their child probes. Later tests were invalid until
all actual probe processes were removed and exactly one owner remained.

## GTK shortcuts and XDG global shortcuts are different APIs

GTK accelerator syntax such as:

```text
<Super><Alt>BackSpace
```

belongs to GTK actions, `GtkShortcutTrigger`, and application/window input.
Even `GTK_SHORTCUT_SCOPE_GLOBAL` means global to the **root widget**, not
system-wide. It is not an OS-wide Wayland key grab.

The XDG Shortcuts Specification uses XKB modifier names joined by `+`:

```text
LOGO+ALT+BackSpace
```

`LOGO` means Super. Other modifiers include `CTRL`, `ALT`, `SHIFT`, and `NUM`.
The key is an xkbcommon keysym name without `XKB_KEY_`.

Keep the two strings separate. HiveMind passes `LOGO+ALT+BackSpace` over D-Bus;
its UiApi displays the equivalent GTK-style `<Super><Alt>BackSpace` only as a
human-readable field.

Changing GTK widgets, focus propagation, `EventControllerKey`, or GDK settings
cannot repair a missing portal `Activated` callback. The delivery path is:

```text
physical key
  -> GNOME Shell accelerator
  -> xdg-desktop-portal-gnome
  -> xdg-desktop-portal
  -> owning client's D-Bus Activated callback
  -> VoiceView::toggleTalk()
```

## Exact portal contract

Use:

- destination: `org.freedesktop.portal.Desktop`
- root object: `/org/freedesktop/portal/desktop`
- interface: `org.freedesktop.portal.GlobalShortcuts`

The sequence is:

1. Open a dedicated session-bus connection for the portal client.
2. Call `org.freedesktop.host.portal.Registry.Register` with
   `org.hivemind.Client`. This makes the permission and saved-shortcut
   namespace explicit. Keep a non-fatal fallback for portals without Registry.
3. Call `CreateSession` with **both** `handle_token` and
   `session_handle_token`.
4. Read the new Session handle from the CreateSession Request's `Response`.
5. Call `BindShortcuts` with `a(sa{sv})` entries containing `description` and
   `preferred_trigger`.
6. Inspect the Bind Request's returned `shortcuts` subset. Response code 0
   alone is not proof that `push-to-talk` was bound. HiveMind also reads the
   returned `trigger_description` for its log.
7. Subscribe to `Activated` on the portal root object and filter parameter 0
   by the current Session handle and parameter 1 by `push-to-talk`.
8. On teardown, unsubscribe first and close the Session. Keep the weak UI
   lifetime token and exact voice-view identity check so an already queued
   callback cannot retarget a later agent's page.

### Tokens are not the saved permission

`handle_token` names a Request; `session_handle_token` names a Session. Supply
both. xdg-desktop-portal 1.21.1 has an assertion when the session token is
missing; deliberately omitting it is not a valid probe.

HiveMind creates a fresh token for each live Session. GNOME persists the
approved shortcut by **registered application ID and shortcut ID**, so a new
live Session can restore it without another permission prompt. Saving an old
D-Bus Session token in `Prefs` is not what preserves the grant.

GNOME can reuse an existing saved binding instead of a later
`preferred_trigger`. If an old key unexpectedly returns, verify the actual
portal app ID and saved setting rather than assuming the new preferred trigger
was sent incorrectly.

## The glibmm callback order — read this before editing

`Gio::DBus::Connection::SlotSignal` receives:

```text
connection, sender_name, object_path, interface_name, signal_name, parameters
```

The correct HiveMind lambda deliberately labels all positional parameters:

```cpp
bus_->signal_subscribe(
    [this](
        const Glib::RefPtr<Gio::DBus::Connection>& /*connection*/,
        const Glib::ustring& /*senderName*/,
        const Glib::ustring& objectPath,
        const Glib::ustring& /*interfaceName*/,
        const Glib::ustring& /*signalName*/,
        const Glib::VariantContainerBase& parameters) {
        if (objectPath != "/org/freedesktop/portal/desktop") return;
        // parameters[0] = Session handle (o)
        // parameters[1] = shortcut ID (s)
        // parameters[2] = timestamp (t)
        // parameters[3] = options (a{sv})
    },
    "org.freedesktop.portal.Desktop",
    "org.freedesktop.portal.GlobalShortcuts",
    "Activated",
    "/org/freedesktop/portal/desktop");
```

Do not copy a callback signature from another language binding without checking
the C++/giomm header. Do not leave a parameter named `path` unless it is truly
the second string after `connection`.

GLib dispatches the callback in the thread-default main context of the thread
that subscribed. HiveMind subscribes on the GTK thread and defers widget work
through idle; teardown and weak/identity guards still remain necessary.

## Safe diagnostic procedure

### 1. Check the actual stack, not assumptions

```bash
pkg-config --modversion gtkmm-4.0 giomm-2.68 glibmm-2.68 gtk4
dpkg-query -W -f='${Package} ${Version}\n' \
  xdg-desktop-portal xdg-desktop-portal-gnome gnome-shell
```

The verified incident environment was gtkmm 4.20.0, GTK 4.22.4,
giomm/glibmm 2.86.0, xdg-desktop-portal 1.21.1,
xdg-desktop-portal-gnome 50.0, and GNOME Shell 50.1. These are observations,
not minimum requirements.

### 2. Ensure exactly one binding owner

Do not run the client, Python probe, and C++ probe simultaneously against the
same app ID and key. Before a probe:

```bash
pgrep -a -x hive-client || true
pgrep -a -f '^/tmp/hive-global-shortcut-cpp-probe$' || true
```

For the temporary C++ probe, kill the **actual exact command PIDs**, then prove
zero remain:

```bash
pids=$(pgrep -f '^/tmp/hive-global-shortcut-cpp-probe$' || true)
if [ -n "$pids" ]; then kill -KILL $pids; fi
sleep 1
test -z "$(pgrep -f '^/tmp/hive-global-shortcut-cpp-probe$' || true)"
```

Do not assume a background job's wrapper PID owns its child process. After
starting the desired owner, verify exactly one actual process exists.

### 3. Reproduce with the exact C++ class

A standalone probe must link the production `GlobalShortcuts.cpp`; a Python
wire probe alone cannot catch a giomm lambda-order bug. Build the temporary
probe from the repository root:

```bash
g++ -std=c++23 -Wall -Wextra -Wpedantic \
  -Ihive-client/voice/src \
  /tmp/hive-global-shortcut-cpp-probe.cpp \
  hive-client/voice/src/GlobalShortcuts.cpp \
  -o /tmp/hive-global-shortcut-cpp-probe \
  $(pkg-config --cflags --libs giomm-2.68)
```

The probe should initialize Glib/Gio, run a `Glib::MainLoop`, construct
`hv::GlobalShortcuts` with a callback that prints `CPP PROBE: ACTIVATED`, and
call `bind("LOGO+ALT+BackSpace")`. Only after its log reports the real bound
trigger should the user perform one approved physical-key test.

### 4. Verify in the owning real client

A generic `gdbus monitor` is supporting evidence only. The portal can direct
the signal to the owning unique D-Bus connection, so a separate monitor may
not receive it. The authority is the owning client's callback, session/ID
checks, and resulting voice action.

Run the installed client with its UiApi, open one agent's voice page, and read:

```text
voice_agent
voice_phase
voice_status
voice_shortcut_active
voice_shortcut
voice_shortcut_activations
```

`voice_shortcut_active=true` means the returned Bind subset contained the
shortcut; it does not by itself prove physical delivery. After one approved
key press, require all of the following:

- the log shows `Activated callback` with matching actual/expected Session;
- the log shows `global shortcut activated`;
- `voice_shortcut_activations` increases;
- Talk/Send behavior follows (recording, STT, then Thinking or playback as
  appropriate).

Test Back/thread/window teardown separately: the Session must close, the global
binding must disappear, and a queued callback must not affect another voice
page.

## Symptom checklist

| Symptom | Check first |
|---|---|
| Bind response is 0 but no physical key was grabbed | `preferred_trigger`, XDG syntax, returned `shortcuts` subset, saved app-ID binding |
| Binding reports active but the app never activates | root-object subscription, giomm argument order, Session/ID filters, competing owners |
| `Connection already associated with an application ID` | Registry.Register was attempted on GTK's shared bus; use the dedicated connection |
| An old application/key unexpectedly binds | actual portal host identity and GNOME's per-app saved setting |
| CreateSession fails or the old portal asserts | both request and Session tokens must be present |
| Callback affects a page after Back/navigation | close/unsubscribe order plus weak UI and exact view guards |
| A separate monitor sees nothing | not conclusive; inspect the owning directed connection/client |
| Repeated contradictory probe results | enumerate actual probe/client PIDs; ensure exactly one binding owner |

## Verification recorded for the fix

On 2026-11-06:

- the exact standalone C++ class received `Activated` and printed its callback
  marker;
- the installed real client received the same signal and incremented
  `voice_shortcut_activations`;
- STT processed audio and the voice page reached `Thinking…`;
- Back/lifecycle code retained explicit Session close and stale-callback
  protection;
- GCC build was warning-clean and cppcheck was clean;
- `git diff --check` was clean;
- full ctest reported 143/143 tests passed, with 4 marked skipped;
- `~/.local/bin/hive-client` matched the tested build.

## Official references

- [XDG GlobalShortcuts API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)
- [XDG Shortcuts Specification](https://specifications.freedesktop.org/shortcuts-spec/latest/)
- [Portal window identifiers](https://flatpak.github.io/xdg-desktop-portal/docs/window-identifiers.html)
- [GLib DBusConnection.signal_subscribe](https://docs.gtk.org/gio/method.DBusConnection.signal_subscribe.html)
- [GLib DBusSignalCallback order](https://docs.gtk.org/gio/callback.DBusSignalCallback.html)
- [GTK4 accelerator parser](https://docs.gtk.org/gtk4/func.accelerator_parse.html)
- [GTK4 ShortcutScope](https://docs.gtk.org/gtk4/enum.ShortcutScope.html)
- [xdg-desktop-portal 1.21.1 GlobalShortcuts XML](https://raw.githubusercontent.com/flatpak/xdg-desktop-portal/1.21.1/data/org.freedesktop.portal.GlobalShortcuts.xml)
- [xdg-desktop-portal-gnome 50.0 backend](https://gitlab.gnome.org/GNOME/xdg-desktop-portal-gnome/-/raw/50.0/src/globalshortcuts.c)
- [GNOME Control Center 50.0 settings provider](https://raw.githubusercontent.com/GNOME/gnome-control-center/50.0/global-shortcuts-provider/cc-global-shortcuts-provider.c)
