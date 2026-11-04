# hive-android — voice-first Android client

Voice-first Android client for the hub: pick an agent, talk, hear the
answer, with roster, voice, text-chat/media, User Inbox, voice-picker,
and settings surfaces. It talks ONLY to hive-server over HTTPS (Bearer
key), the same REST surface as hive-client (`docs/api.md`). Source lives
top-level in `hive-android/`.

## What it does

- **Roster** — agent info cards: 64 dp creature avatar, large name,
  two-line role, and a live-state pill (ready/busy) on a rounded dark
  card. Avatar resolution is identical to the other clients: the stored `avatar` field when it
  is in the set, else a stable MurmurHash64A of the agent name into the
  20-name list (bit-for-bit parity with the client's
  `std::hash<string>`), else the user silhouette. PNGs are 192 px
  rasters of `hive-client/data/avatars/` bundled in assets.
- **Voice session** — a Jarvis-style point-cloud sphere (OpenGL ES 2.0,
  C++/NDK, `app/src/main/cpp/sphere_renderer.cpp`) that moves with the
  voice signal, plus two buttons: big round TALK/SEND, small ✕ cancel.
  Tap once to talk, tap again to send; while the agent speaks, the big
  button is an interrupt — cut it off and drop straight back to
  recording. ✕ discards the take or cancels the turn
  (`POST /api/threads/:id/cancel`).
- **Continuous recording** — STT is on-device whisper.cpp fed by our own
  AudioRecord pipeline: quantized tiny.en supplies live sliding-window
  partials and base.en performs the final pass on SEND. There is NO
  endpointer: silence never stops a take — only SEND or ✕ does. No
  platform SpeechRecognizer is used (its silence auto-end, restart
  throttling, and start/stop cues are why the app went on-device).
- **Streaming TTS** — the hub's live turn partial (the same task_delta
  stream the desktop client's chunks come from, via
  `GET /api/threads/:id/activity`) is sentence-chunked every 700 ms and
  synthesized on-device by pocket-tts/audio.cpp through JNI, with PCM
  playback through AudioTrack. Android platform TextToSpeech `QUEUE_ADD`
  is the fallback if pocket-tts cannot initialize or a chunk fails. The
  agent therefore starts speaking from its first sentence instead of
  after the full reply. Chunks are cut
  only at safe sentence boundaries (the still-growing tail is never
  read mid-word); the remainder flushes at turn end. Every outgoing
  message carries a voice-style prefix (plain flowing prose, no
  markdown/lists/code, spoken-form numbers; consult via message_agent,
  report back via message_user), and replies pass a speech sanitizer
  (`SpeechText.forSpeech`) before TTS.
- **Visible User Inbox** — the roster's bottom icon-and-label Agents / Inbox bar opens
  hub thread `"user"`. The latest 300 messages render newest-first with
  sender avatar/name, local timestamps, bodies, and attachment names.
  The screen refreshes every 5 seconds and clears the persisted user
  unread mark. Tapping a message reads it in the sender's configured
  voice, streamed sentence-by-sentence so playback starts while later
  sentences synthesize; tapping another switches playback, and ■ stops
  it. Inbox notes
  are not auto-narrated during voice sessions (DM narration covers the
  reply), avoiding duplicate speech while keeping them available on
  demand.
- **Per-agent voices** — long-press an agent card for the voice picker:
  single tap samples a voice ("Hi, I'm PA — this is my voice."), double
  tap selects it for that agent (Prefs `voice_<agentId>`; applied when
  the session's TtsEngine boots). English embedded voices only
  (offline, q400) — 23 on the test device.
- **State machine** — IDLE / LISTENING / THINKING / STREAMING. The
  sphere shows a per-state palette (dim cyan idle, cyan listening,
  amber fast-spin thinking, green speaking) and rides mic RMS while
  recording, a synthetic envelope while the agent talks. The voice
  screen holds `FLAG_KEEP_SCREEN_ON` for the whole session.

- **Text chat** — chat icon right of TALK opens the full thread over the
  sphere (voice session keeps running underneath). Bubbles with headers,
  history scrollback, BACK or the top-left arrow returns to voice. Agent
  attachments render inline: images center-cropped thumbnails, videos as
  thumbnail + ▶, audio as cards (large ▶ + filename + size). Tap for the
  fullscreen viewers.
- **Fullscreen media** — one in-activity overlay hosts the viewers:
  pinch-zoom/pan image viewer (fit-centre on open), a TextureView video
  player (letterboxed fit, tap for controls, BACK stops instantly), and
  an audio player with a 112dp play/pause, seek bar, and an OpenGL ES
  waveform visualizer fed real samples from android.media.audiofx
  Visualizer (pseudo-wave fallback). Audio autoplays; BACK closes any
  viewer and releases its player.

## Notification relay

With Android notification access enabled, `NotifyListenerService` relays
supported SMS/MMS and missed-call notifications to the configured agent's
DM through `POST /api/notifications`. Ongoing/low-priority/group-summary
notifications and the app's own notifications are filtered out. Dedupe
uses the Android notification instance plus a content fingerprint and
survives service restarts.

Delivery is deliberately **at-most-once**: a failed hub POST is dropped
and is not automatically retried, avoiding repeated announcements. The
persisted dedupe policy is not an end-to-end exactly-once guarantee.

## Build & install

- Toolchain: JDK 21 (AGP requirement), Android SDK platform 35 + NDK
  side-by-side; the Gradle wrapper is committed. One-time
  `./fetch-deps.sh` pulls the whisper.cpp/audio.cpp native dependencies
  and on-device STT/TTS models (large, gitignored).
- `./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`
  (includes native libraries and models for arm64-v8a and x86_64).
- `adb install -r …`; first launch opens Settings: hub URL (default
  `https://leestrippai.com`, LAN `http://<ip>:8421` works too) + API
  key (mint via `/api/keys`; `agent` scope suffices). RECORD_AUDIO is
  requested on the first TALK.

## Internals worth knowing

- **Media overlay discipline** — the overlay (mediaOverlay in
  activity_voice.xml) holds at most ONE viewer and always release()s the
  outgoing view BEFORE detach (an orphaned player keeps playing with no
  controls). Attachment opens are guarded against double-tap during the
  download (one in-flight open per attachment). MediaController shows on
  single-tap only — shown controllers eat BACK.
- **Attachment cache** — cacheDir/attachments/<thread>/<pathhash>-<name>;
  downloads buffer fully in memory before write (no partial files).

- **Kotlin shell** — framework Views, org.json, kotlin-stdlib, no
  AndroidX. Native code does the interesting work: `libsphere.so` (GL
  renderer), whisper.cpp STT, and audio.cpp/pocket-tts synthesis through
  JNI. Native libraries/models are bundled for arm64-v8a and x86_64.
- **SttEngine contract** — start/stop/discard/destroy + transcript +
  listening, with level/transcript/stopped callbacks. A session
  generation counter kills the zombie-capture class: a capture start
  queued on the engine worker can never fire after a cancel (that bug
  made TALK a dead button until fixed).
- **Threading** — mic capture on its own thread, model load on a
  HandlerThread, hub I/O on a single-thread executor, every view touch
  on the main Handler.
- **GL lifecycle** — the native renderer rebuilds on every
  `onSurfaceCreated`: GL objects belong to one context, so a second
  VoiceActivity after BACK would otherwise draw with the first
  activity's dead handles (the "sphere goes black on agent #2" bug).
- **DM + turn flow** — mirrors the client: `POST /api/threads` with
  `reuse_dm:true`; the turn loop advances a `lastMessageId` cursor over
  `messages?after=`; the idle watch resumes from the same cursor, so a
  message is never spoken twice.
- **Waydroid** — STT and primary pocket-tts work with the bundled x86_64
  libraries and need no GMS/system speech service. Only the platform-TTS
  fallback needs a system engine, which stock LineageOS images may lack.
- **Accessibility services** — TalkBack (or anything grabbing transient
  audio focus) fights the mic and makes single taps focus-only. adb
  `input tap` bypasses it, so automated taps pass while physical taps
  fail — when tap behaviour diverges between adb and fingers, check
  `settings get secure accessibility_enabled` FIRST.

See `hive-android/README.md` for the developer quick reference and
`docs/hive-worker.md` for the task-frame fields (`roster`, `user_inbox`)
the voice features build on.
