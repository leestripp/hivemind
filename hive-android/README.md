# hive-android — HiveMind Voice

Voice-only Android client for the HiveMind hub. Pick an agent, get a
Jarvis-style sphere that moves with the voice signal, talk to it, hear
the reply.

- **STT/TTS**: Android platform services (`SpeechRecognizer` /
  `TextToSpeech`) — on-device, no bundled models.
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

- LISTENING: continuous capture — the recognizer re-arms itself on every
  silence auto-end, so recording runs until YOU tap SEND (send it) or ✕
  (discard it). Live transcript tail shows in the status line.
- THINKING: polls activity+messages every 700 ms (3 min watchdog).
- SPEAKING: big button = interrupt — cuts the reply, cancels the turn,
  and drops straight back into recording; ✕ stops to idle.
- The voice screen holds FLAG_KEEP_SCREEN_ON for the whole session, so
  the phone can't lock mid-rant, mid-think, or mid-reply.

## Build

Requires JDK 21 (AGP), Android SDK platform 35 + NDK (side-by-side).
On this rig both live in `~/Android/Sdk`; JDK 21 at
`/usr/lib/jvm/java-21-openjdk-amd64`.

```bash
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
export ANDROID_HOME=~/Android/Sdk
./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk  (~1.7 MB)
```

ABIs: `arm64-v8a` (phones), `armeabi-v7a`, `x86_64` (emulator/Waydroid).

## Install & run

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

First launch opens Settings: Hub URL (default `https://leestrippai.com`,
a LAN `http://<ip>:8421` works too) + API key. Grant the microphone
permission when prompted.

## Notes / caveats

- **Waydroid**: stock LineageOS images ship no GMS, so
  `SpeechRecognizer`/`TextToSpeech` services are absent — the app
  detects this and says so. UI + hub comms work; voice needs a gapps
  image or a real phone.
- cleartext HTTP is permitted in the manifest for LAN hubs; the default
  URL is TLS.
- No third-party deps: framework Views + org.json + kotlin-stdlib only.
