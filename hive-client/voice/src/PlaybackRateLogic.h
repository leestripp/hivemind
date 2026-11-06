#pragma once

namespace hv::playback {

// A native-rate stream cannot accept PCM at another rate. Wait for the old
// stream's queued audio to drain before closing it; closing early discards
// both audible speech and the completion tags for that speech.
inline bool rateSwitchBlocked(int openRate, int nextRate, bool audioPending) {
    return openRate != nextRate && audioPending;
}

}  // namespace hv::playback
