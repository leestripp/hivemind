# hive-client-voice

GTK4 (gtkmm-4.0) voice client for the HiveMind hub — a 1:1 port of the
Android voice app (`hive-android`). Talk to an agent hands-free: tap
TALK, speak, tap SEND; the reply streams back sentence-by-sentence as
speech while the Jarvis swarm sphere reacts to mic level and agent state.
The complete STT transcript is posted as exactly one user message;
sentence splitting is used only for outbound TTS narration.

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

`test_speech_text`, `test_announcer`, `test_voice_turn_logic`,
`test_tts_tag_ledger`, `test_prefs`, `test_avatars`,
`test_wav_codec` (pure-logic units; run via ctest from the top-level
build).


## Turn activity scopes

A voice session streams reply text from the selected agent's DM, but its
lifetime follows the agent-wide activity signal (`GET /api/agents/activity`).
During agent-to-agent relays the DM can be idle while the agent is still
working in another thread. In that state the app keeps listening and speaks
new DM messages, then returns to idle only when the agent is idle across all
threads. `test_voice_turn_logic` covers the scope transitions.

TTS completion is tracked by exact utterance tag, so late playback callbacks
from a stopped turn cannot consume a newer turn's outstanding count. Once
the hub is idle, a silent audio drain is bounded to 75 seconds (longer than
the 60-second TTS request timeout); an orphaned drain logs and returns to
idle instead of leaving the Stop control up forever. The debug shell's
`status` includes `turn_done`, `tts_outstanding`, and `tts_speaking`.

PipeWire playback uses each WAV's native sample rate. When the next
utterance has a different rate, old-rate audio now drains before the stream
is reopened; closing early used to discard queued PCM and its completion
tags. Queued items also carry a stop/clear generation so cancelled audio
cannot reappear while a rate change waits. A 24 kHz to 16 kHz null-sink
regression test verifies that both utterance-completion tags arrive.
