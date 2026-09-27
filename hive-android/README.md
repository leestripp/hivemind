# hive-android — HiveMind Voice

Voice-only Android client for the HiveMind hub. Pick an agent, get a
Jarvis-style sphere that moves with the voice signal, talk to it, hear
the reply.

- **STT**: on-device whisper.cpp (ggml q8_0, NEON+dotprod): tiny.en
  for live partials (5s sliding window, ~0.5s), base.en for the final
  pass on SEND (~1s/9s audio). Replaces the old sherpa-onnx zipformer —
  dramatically better accuracy.
  int8, ~70 MB bundled model) fed by our own AudioRecord capture —
  NO endpointer, so silence never stops a recording: the user is the
  only stop signal. No sessions, no throttling, no audio cues, works
  fully offline. Kotlin API + native libs vendored from
  [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp) v1.9.4
  (Apache-2.0), fetched by `./fetch-deps.sh`.
- **TTS**: Android platform `TextToSpeech`.
- **Sphere**: OpenGL ES 2.0 point-cloud renderer in C++ (NDK,
  `app/src/main/cpp/sphere_renderer.cpp`) — Fibonacci lattice displaced
  by simplex noise; amplitude follows mic RMS while listening and a
  synthetic envelope while the agent speaks. States: idle / listening /
  thinking / speaking.
- **Roster**: agents are grouped by their hive-worker; online workers
  sort first and expand, offline workers collapse to a greyed-out
  header (tap to peek — their agents are inert while the worker is down).
- **Hub**: plain REST (same as hive-client) — `GET /api/agents`,
  `GET /api/workers` (online flags drive the grouping above),
  `POST /api/threads` (`reuse_dm:true` reopens the 1:1),
  `POST/GET …/messages`, `GET …/activity`, `POST …/cancel`.
  Auth: `Authorization: Bearer <key>` (mint one via the hub's
  `/api/keys` — `agent` scope is enough).

## Per-agent voices

Long-press an agent card → voice picker: single tap samples a voice
("Hi, I'm PA — this is my voice."), double tap selects it for that
agent (Prefs `voice_<agentId>`). English embedded voices only
(offline, q400; 23 on the test device). Selections are per-agent —
every agent can sound different.

Voices follow the SENDER, not the session (Phase 68): an inbox note
from Honey is read in Honey's configured voice even mid-chat with PA —
the engine switches between utterances, never mid-sentence. A sender
with no configured voice falls back to the session agent's voice, then
the engine default. DM replies and system notices always use the
session agent's voice. The app also clears the hub's persisted unread
mark (`POST /api/threads/:id/read`) for every message it speaks, so
voice sessions no longer leave phantom unread on desktop/web (66c).

## Avatars

Roster rows and the voice screen show the same creature avatars as the
GTK client / web UI. Resolution mirrors hive-client exactly: the stored
`avatar` field when it's in the set, else a stable MurmurHash64A of the
agent name (bit-for-bit parity with the client's std::hash<string>),
else the user silhouette. PNGs are 192px rasters of the client's SVGs
in `assets/avatars/` (regenerate:
`for f in ../hive-client/data/avatars/*.svg; do rsvg-convert -w 192 -h 192 -o app/src/main/assets/avatars/$(basename $f .svg).png $f; done`).

## UI flow

```
agent list → tap agent → voice screen
IDLE ──tap TALK──▶ LISTENING ──tap SEND──▶ THINKING ──first sentence──▶ STREAMING ──▶ IDLE
                     │                   │                    │ tap: cut off → LISTENING
                     └────────── ✕ cancel anywhere ───────────┴──▶ IDLE
```

- LISTENING: one continuous capture with no silence detection at all —
  pause to think as long as you like; it ends only when YOU tap SEND
  (send it) or ✕ (discard it). Live transcript tail in the status line.
- THINKING → STREAMING: the hub's live turn partial (same task_delta
  stream the desktop client's live chunks come from) is sentence-chunked
  and queued to TextToSpeech every 700 ms, so the agent starts speaking
  from its first sentence; the remainder is flushed at turn end.
- Every message carries a voice-style instruction (plain flowing prose,
  no markdown/lists/code/links — it is read aloud); replies pass a
  speech sanitizer (SpeechText.forSpeech) as a safety net before TTS.
- STREAMING: big button = interrupt — cuts the reply, cancels the turn,
  and drops straight back into recording; ✕ stops to idle.
- The voice screen holds FLAG_KEEP_SCREEN_ON for the whole session, so
  the phone can't lock mid-rant, mid-think, or mid-reply.
- INBOX WATCH: at idle the app also polls the DM and the hub's User
  Inbox (thread "user") for new agent messages and reads them out —
  attributed ("PA says: …") for inbox notes, which can come from any
  agent. Notes that arrive mid-recording or mid-reply queue silently
  and play only at idle boundaries, in order — voices never interleave.
  This is the voice-side answer to "ask PA to consult Bob and get back
  to me": PA messages Bob (pair thread), then reports back with
  message_user → the app speaks it. The outgoing prefix tells agents
  exactly this flow.

## Build

Requires JDK 21 (AGP), Android SDK platform 35 + NDK (side-by-side).
On this rig both live in `~/Android/Sdk`; JDK 21 at
`/usr/lib/jvm/java-21-openjdk-amd64`.

```bash
./fetch-deps.sh   # one-time: whisper.cpp source + ggml models (~125 MB)
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
export ANDROID_HOME=~/Android/Sdk
./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk  (~114 MB)
```

ABIs: `arm64-v8a` (phones), `x86_64` (emulator/Waydroid).

## Install & run

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

First launch opens Settings: Hub URL (default `https://leestrippai.com`,
a LAN `http://<ip>:8421` works too) + API key. Grant the microphone
permission when prompted.

## Notes / caveats

- **Waydroid**: STT is self-contained (whisper.cpp, x86_64 libs
  included) so dictation works without GMS; TTS still needs a system
  TTS engine, which stock LineageOS images lack — replies would be
  text-status only there unless you install one.
- cleartext HTTP is permitted in the manifest for LAN hubs; the default
  URL is TLS.
- Third-party code: whisper.cpp (MIT,
  (c) k2-fsa) vendored under app/src/main/java/com/k2fsa/ and
  app/src/main/jniLibs/; everything else is framework Views + org.json
  + kotlin-stdlib.
