# Integrated voice module

ChimpFlow voice for the single native `hive-client` desktop app. The
`hive_voice_ui` library lives here and is linked directly into the main
client; there is no separate voice executable, application ID, desktop
launcher, roster, or debug shell.

Tap **TALK**, speak, then tap **SEND**. The reply streams back
sentence-by-sentence as speech while the sphere reacts to mic level and
agent state. The complete STT transcript is posted as exactly one user
message; sentence splitting is used only for outbound TTS narration.

## Build

Built as part of the top-level HiveMind CMake tree with
`-DHIVEMIND_BUILD_CLIENT=ON`:

```sh
cmake -S . -B build
cmake --build build --target hive-client -j$(nproc)
```

## Runtime dependencies

1. **Audio I/O** — `libpipewire-0.3-dev` (Debian/Ubuntu). Without it the
   module compiles a null backend and reports unavailable mic/speaker I/O.
2. **ChimpFlow voice service** — local TTS+STT gateway at
   `http://127.0.0.1:8090` (audio.cpp pocket-tts + whisper.cpp behind an
   OpenAI-compatible API; nothing leaves the machine).
   - URL: `CHIMPFLOW_URL` env var (default `http://127.0.0.1:8090`)
   - API key: `CHIMPFLOW_KEY` env var, or the existing
     `~/.config/hive-client-voice/chimpflow.key` file (mode 600)
   - Voices: whatever `GET /v1/voices` reports

The old configuration directory name is retained deliberately so existing
voice preferences and per-agent speaker choices continue to work.

## Integrated behavior

Selecting an agent opens or reuses its DM and, when ChimpFlow is healthy,
opens `VoiceView` in the left panel while keeping the DM visible. Back
restores Agents/Threads without losing the DM. Hub connection settings come
from the main client. Voice inherits the main GNOME dark/light/high-contrast
appearance. The page shows the scoped shortcut and live ChimpFlow health.

Readable agent chat bubbles also have a Play button. It uses an independent
narration engine owned by `HiveWindow`, reads the exact stored message with
the sender's configured voice, and interrupts previous bubble narration when
another Play button is clicked. This works in ordinary threads and Inbox;
it does not create another voice application or steal `VoiceView`'s sink.

**Super+Alt+Backspace** invokes the exact Talk button action through the XDG
GlobalShortcuts portal. The session exists only while voice is open: Back,
thread changes, and window close explicitly unbind it. Ordinary chat never
owns this global shortcut.

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
| `data/voice.ui` | GTK builder UI for the embedded voice page (gresource) |

## Tests

`test_speech_text`, `test_transcript_text`, `test_announcer`,
`test_voice_turn_logic`, `test_api_client_activity`, `test_tts_tag_ledger`,
`test_prefs`, `test_avatars`, `test_wav_codec`, and `test_audio_common`
(run via ctest from the top-level build). `test_speech_text` also covers
full-message chunking and UTF-8-safe splitting for bubble narration.


## Turn activity scopes

A voice session streams reply text from the selected agent's DM, but its
lifetime follows the agent-wide activity signal (`GET /api/agents/activity`).
During agent-to-agent relays the DM can be idle while the agent is still
working in another thread. In that state the app keeps listening and speaks
new DM messages, then returns to idle only when the agent is idle across all
threads. `test_voice_turn_logic` covers the scope transitions.
`test_api_client_activity` exercises the real HTTP response envelope: the
agent map is nested under `agents`. Looking up the id at the JSON root made
every agent appear idle, which ended live turns early and repeatedly
restarted their partial narration.

TTS completion is tracked by exact utterance tag, so late playback callbacks
from a stopped turn cannot consume a newer turn's outstanding count. Once
the hub is idle, a silent audio drain is bounded to 75 seconds (longer than
the 60-second TTS request timeout); an orphaned drain logs and returns to
idle instead of leaving the Stop control up forever. The main client's UiApi
(`GET /ui/state`) exposes the current `voice_phase`, `voice_status` and
related voice-page state for diagnostics; there is no separate debug shell.

PipeWire playback uses each WAV's native sample rate. When the next
utterance has a different rate, old-rate audio now drains before the stream
is reopened; closing early used to discard queued PCM and its completion
tags. Queued items also carry a stop/clear generation so cancelled audio
cannot reappear while a rate change waits. A 24 kHz to 16 kHz null-sink
regression test verifies that both utterance-completion tags arrive.
