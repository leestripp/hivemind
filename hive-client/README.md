# HiveMind Electron client

`hive-client/` is the primary desktop client: Electron, strict TypeScript,
semantic HTML/CSS, and no UI framework. The previous GTK4/gtkmm4 client is
removed; its history remains reachable in git (archived under `hive-client-gdk/` up to tag 1.4.0).

## Run

Use Node.js 22 or newer:

```bash
cd hive-client
npm install
npm start
```

`npm start` compiles TypeScript, copies the renderer assets, and launches the
real Electron application. The legacy hub and voice preferences are imported
on first run from:

- `~/.config/hivemind/config.json`
- `~/.config/hive-client-voice/prefs.json`
- `~/.config/hive-client-voice/chimpflow.key`

Electron stores ordinary preferences in its own `settings.json` and the
ChimpFlow credential separately in `chimpflow.key` (mode 0600). Existing legacy
files are never modified. Set `HIVE_CLIENT_USER_DATA` to isolate a profile;
set `HIVE_CLIENT_LEGACY_CONFIG_HOME` to override the import source for tests.
Thread-group collapse and assignment state is imported from the GTK client
on first run; `HIVE_CLIENT_LEGACY_DATA_HOME` overrides that import source.

## UI and automation contract

Every in-app popup is an HTML `<dialog>` in the Electron document. Agent
desktops open in separate sandboxed Chromium windows loaded by the main
process; they receive no preload bridge. There are no GTK/GNOME dialogs and
no calls to Electron's `dialog` module. Dropdowns are Chromium `<select>`
elements. File upload uses an HTML
`<input type="file">`; automation calls Playwright's `setInputFiles` directly
instead of operating an OS file chooser. The new client registers no global
keyboard shortcuts.

The renderer has `contextIsolation`, sandboxing, and a restrictive CSP enabled;
Node integration is disabled. Only the typed `window.hive` preload API crosses
the IPC boundary. Network access to the hub and loopback ChimpFlow service is
implemented in the main process, not by renderer HTML.

## Verify

```bash
npm run typecheck
npm run test:unit
npm run test:e2e
# On a headless host:
xvfb-run -a npm run test:e2e
```

The Electron tests launch the real executable against isolated mock hub and
ChimpFlow HTTP services. They cover roster/DM navigation, light/dark `nativeTheme` propagation,
Settings tabs and dropdowns, model-scoped voice persistence and preview, text
and attachment posting, media preview/download, all routine schedule types,
advanced agent creation/editing and lifecycle, inspector details, cancellation,
safe deletion and task reassignment, sandboxed agent desktop windows, exec
logs, shared and learned skills, custom/smart thread groups, thread
clear/delete, microphone-to-transcription-to-message flow, stale-history
suppression, new-message narration, stop confirmation, and renderer isolation. They do not use AT-SPI, Dogtail, desktop screenshots, or
your production credentials.

## Linux package

Build a self-contained x86-64 Linux tarball without adding a packaging
framework or runtime npm dependencies:

```bash
npm run package:linux
npm run test:package
```

The package is written to `release/hivemind-client-<version>-linux-x64.tar.gz`.
It contains the project-pinned Electron runtime, compiled application, a
freedesktop entry, and a sandbox-aware `hivemind-client` launcher.
`test:package` launches the exact packaged executable with an isolated empty
profile and checks the Chromium UI and sandbox boundary.

**First run on Ubuntu 23.10+:** Electron's Chromium sandbox needs its
`chrome-sandbox` helper installed setuid-root (Ubuntu restricts unprivileged
user namespaces via AppArmor, so no fallback sandbox exists). After unpacking
the tarball, run once:

```bash
./install.sh   # sudo chown/chmod the sandbox helper + install desktop entry
```

Without it the launcher prints that same repair recipe instead of letting
Chromium abort silently (the packaged "won't open" symptom). `test:package`
installs the helper itself when non-interactive sudo is available and skips
with the recipe otherwise.

Also unpack the tarball onto a filesystem that allows setuid binaries — some
`/data`/`/projects` volumes are mounted `nosuid`, which silently breaks the
setuid helper (the launcher detects this and says so; `findmnt -T <dir>`
shows the mount options). Your home directory always works.

## Source map

- `src/main/` — settings migration/persistence, hub/ChimpFlow services, IPC,
  BrowserWindow security.
- `src/preload/` — minimal typed bridge.
- `src/shared/` — contracts, defensive payload normalization, speech text.
- `src/renderer/` — roster, grouped threads, messages, voice, Settings, audio.
- `src/tests/` — Node unit tests.
- `tests/` — Playwright Electron tests.

The client covers the GTK window-level workflow: worker-grouped roster,
inbox/direct/group threads, DM reuse, history/live activity, message send and
cancel, attachments with inline image/audio/video playback, agent/thread
creation and management, custom and smart thread folders, all routine schedule
types, shared/learned skills, exec logs, agent inspector details, sandboxed
agent desktop windows, master volume, model-scoped voices, message narration,
and Talk/Send voice turns with live/final deduplication and stop confirmation.

Appearance follows Electron's `nativeTheme.shouldUseDarkColors` in the main
process and updates on `nativeTheme.updated`; renderer CSS does not depend on a
desktop-specific `prefers-color-scheme` implementation. There is deliberately
no automatic OS-wide keyboard grab. Voice control is available through the
focused, accessible HTML buttons; a global shortcut must be separately and
explicitly approved before it is introduced.
