# hive-android — HiveMind Voice

Voice-only Android client for the HiveMind hub. Pick an agent, get a
Jarvis-style sphere that moves with the voice signal, talk to it, hear
the reply.

- **STT**: on-device streaming sherpa-onnx (Zipformer2 transducer,
  int8, ~70 MB bundled model) fed by our own AudioRecord capture —
  NO endpointer, so silence never stops a recording: the user is the
  only stop signal. No sessions, no throttling, no audio cues, works
  fully offline. Kotlin API + native libs vendored from
  [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) v1.13.8
  (Apache-2.0), fetched by `./fetch-deps.sh`.
- **TTS**: Android platform `TextToSpeech`.
- **Sphere**: OpenGL ES 2.0 point-cloud renderer in C++ (NDK,
  `app/src/main/cpp/sphere_renderer.cpp`) — Fibonacci lattice displaced
  by simplex noise; amplitude follows mic RMS while listening and a
  synthetic envelope while the agent speaks. States: idle / listening /
  thinking / speaking.
- **Hub**: plain REST (same as hive-client) — `GET /api/agents`,
  `POST /api/threads` (`reuse_dm:true` reopens the 1:1),
  `POST/GET …/messages`, `GET …/activity`, `POST …/cancel`.
  Auth: `Authorization: Bearer <key>` (mint one via the hub's
  `/api/keys` — `agent` scope is enough).

## UI flow

```
agent list → tap agent → voice screen
IDLE ──tap TALK──▶ LISTENING ──tap SEND──▶ THINKING ──reply──▶ SPEAKING ──▶ IDLE
                     │                │             │
                     └──── ✕ cancel anywhere ────────┴──▶ IDLE
```

- LISTENING: one continuous capture with no silence detection at all —
  pause to think as long as you like; it ends only when YOU tap SEND
  (send it) or ✕ (discard it). Live transcript tail in the status line.
- THINKING: polls activity+messages every 700 ms (3 min watchdog).
- Every message carries a voice-style instruction (plain flowing prose,
  no markdown/lists/code/links — it is read aloud); replies pass a
  speech sanitizer (SpeechText.forSpeech) as a safety net before TTS.
- SPEAKING: big button = interrupt — cuts the reply, cancels the turn,
  and drops straight back into recording; ✕ stops to idle.
- The voice screen holds FLAG_KEEP_SCREEN_ON for the whole session, so
  the phone can't lock mid-rant, mid-think, or mid-reply.

## Build

Requires JDK 21 (AGP), Android SDK platform 35 + NDK (side-by-side).
On this rig both live in `~/Android/Sdk`; JDK 21 at
`/usr/lib/jvm/java-21-openjdk-amd64`.

```bash
./fetch-deps.sh   # one-time: sherpa-onnx natives + STT model (~130 MB)
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

- **Waydroid**: STT is self-contained (sherpa-onnx, x86_64 libs
  included) so dictation works without GMS; TTS still needs a system
  TTS engine, which stock LineageOS images lack — replies would be
  text-status only there unless you install one.
- cleartext HTTP is permitted in the manifest for LAN hubs; the default
  URL is TLS.
- Third-party code: sherpa-onnx Kotlin API + native libs (Apache-2.0,
  (c) k2-fsa) vendored under app/src/main/java/com/k2fsa/ and
  app/src/main/jniLibs/; everything else is framework Views + org.json
  + kotlin-stdlib.
