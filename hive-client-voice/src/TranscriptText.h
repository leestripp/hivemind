#pragma once

#include <string>
#include <string_view>

namespace hv {

// Prepare the user's complete utterance for ONE hub message. Silence-only
// artifacts are discarded; artifacts mixed into real speech are removed
// without splitting the resulting message. Sentence splitting belongs only
// to outbound TTS narration, never to STT input.
std::string sanitizeSttTranscript(std::string_view text);

}  // namespace hv
