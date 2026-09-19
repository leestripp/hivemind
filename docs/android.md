# hive-android — voice-only Android client

Voice-only Android client for the hub: pick an agent, talk, hear the
answer. Three screens — roster, voice session, settings. It talks ONLY
to hive-server over HTTPS (Bearer key), the same REST surface as
hive-client (`docs/api.md`). Source lives top-level in `hive-android/`.

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
- **Continuous recording** — STT is an on-device streaming Zipformer2
  transducer (sherpa-onnx, int8) fed by our own AudioRecord pipeline
  with NO endpointer: silence never stops a take — only SEND or ✕ does.
  No platform SpeechRecognizer anywhere (its silence auto-end, restart
  throttling, and start/stop cues are why the app went on-device).
- **Streaming TTS** — the hub's live turn partial (the same task_delta
  stream the desktop client's chunks come from, via
  `GET /api/threads/:id/activity`) is sentence-chunked every 700 ms and
  fed to TextToSpeech `QUEUE_ADD`, so the agent starts speaking from
  its first sentence instead of after the full reply. Chunks are cut
  only at safe sentence boundaries (the still-growing tail is never
  read mid-word); the remainder flushes at turn end. Every outgoing
  message carries a voice-style prefix (plain flowing prose, no
  markdown/lists/code, spoken-form numbers; consult via message_agent,
  report back via message_user), and replies pass a speech sanitizer
  (`SpeechText.forSpeech`) before TTS.
- **Inbox watch** — at idle the app polls the DM AND the hub's User
  Inbox (thread `"user"`) for new agent messages and reads them out,
  attributed ("PA says: …") for inbox notes, which can come from any
  agent. Notes arriving mid-recording or mid-reply queue silently and
  play only at idle boundaries, in arrival order — voices never
  interleave. With the agents' `message_agent` / `message_user` /
  `read_user_inbox` tools this makes "ask PA to consult Bob and get
  back to me" work end-to-end: pair-thread consultation, inbox
  report-back, spoken delivery.
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

## Build & install

- Toolchain: JDK 21 (AGP requirement), Android SDK platform 35 + NDK
  side-by-side; the Gradle wrapper is committed. One-time
  `./fetch-deps.sh` pulls the sherpa-onnx natives + STT model
  (~130 MB, gitignored).
- `./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`
  (~114 MB — model + onnxruntime for arm64-v8a and x86_64).
- `adb install -r …`; first launch opens Settings: hub URL (default
  `https://leestrippai.com`, LAN `http://<ip>:8421` works too) + API
  key (mint via `/api/keys`; `agent` scope suffices). RECORD_AUDIO is
  requested on the first TALK.

## Internals worth knowing

- **Kotlin shell only** — framework Views, org.json, kotlin-stdlib, no
  AndroidX. Native code does the interesting work: `libsphere.so` (GL
  renderer) and the vendored sherpa-onnx JNI libs
  (`app/src/main/jniLibs/`, Apache-2.0 k2-fsa v1.13.8).
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
- **Waydroid** — STT works (x86_64 libs vendored, no GMS needed); TTS
  needs a system engine, which stock LineageOS images lack.
- **Accessibility services** — TalkBack (or anything grabbing transient
  audio focus) fights the mic and makes single taps focus-only. adb
  `input tap` bypasses it, so automated taps pass while physical taps
  fail — when tap behaviour diverges between adb and fingers, check
  `settings get secure accessibility_enabled` FIRST.

See `hive-android/README.md` for the developer quick reference and
`docs/hive-worker.md` for the task-frame fields (`roster`, `user_inbox`)
the voice features build on.
