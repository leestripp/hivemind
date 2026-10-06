# hive-client-voice

GTK4 (gtkmm-4.0) voice client for the HiveMind hub — a 1:1 port of the
Android voice app (`hive-android`). Talk to an agent hands-free: tap
TALK, speak, tap SEND; the reply streams back sentence-by-sentence as
speech while the Jarvis swarm sphere reacts to mic level and agent state.

## Build

Built as part of the top-level Hive-Mind CMake tree
(`-DHIVEMIND_BUILD_CLIENT=ON`). Standalone:

```sh
cmake -B build -S . && cmake --build build -j$(nproc)
```

## Runtime dependencies

The binary **builds without** these and says so at runtime, but for a
functional client you need:

1. **Audio I/O** — `libpipewire-0.3-dev` (Debian/Ubuntu). Without it the
   app compiles a null backend: playback completes by wall-clock (TTS
   flow works silently) and the mic reports unavailable.
2. **ChimpFlow voice service** — local TTS+STT gateway at
   `http://127.0.0.1:8090` (audio.cpp pocket-tts + whisper.cpp behind
   an OpenAI-compatible API; nothing leaves the machine). All models
   live inside the service — the app downloads and loads nothing.
   - URL: `CHIMPFLOW_URL` env var (default `http://127.0.0.1:8090`)
   - API key: `CHIMPFLOW_KEY` env var, or a
     `~/.config/hive-client-voice/chimpflow.key` file (mode 600)
   - Voices: whatever `GET /v1/voices` reports; picked per-agent in the
     voice picker

## First run

Start on the roster: pick an agent (saved as the default). The hub URL
and API key live in Settings; they're persisted to
`~/.config/hive-client-voice/prefs.json`.

## Layout

| file | role |
|------|------|
| `src/VoiceView.*` | the voice session — full VoiceActivity.kt port (poll ticks, cancel-confirm, inbox watch, interrupt-and-talk) |
| `src/SttEngine.*` | mic capture + SEND-time transcription via ChimpFlow |
| `src/TtsEngine.*` | ChimpFlow sentence queue, epoch stop model |
| `src/ChimpFlowClient.*` | ChimpFlow gateway HTTP client (TTS WAV, STT multipart) |
| `src/WavCodec.h` | WAV decode/encode + resample to the 16 kHz mono contract |
| `src/SphereArea.*` | swarm + honeycomb shaders (GLSL 330, epoxy) |
| `src/Audio*.*` | PipeWire 16 kHz mono capture/playback + null fallback |
| `src/ApiClient.*` | hub REST (`/api/agents`, threads, activity, cancel) |
| `src/SpeechText.*` | TTS hygiene (clause split, fillers, CJK punctuation) |
| `src/AgentStateAnnouncer.*` | roster edge announcements ("Scout is online") |
| `data/voice.ui` | GTK builder UI for the session window (gresource) |

## Tests

`test_speech_text`, `test_announcer`, `test_prefs`, `test_avatars`,
`test_wav_codec` (pure-logic units; run via ctest from the top-level
build).
