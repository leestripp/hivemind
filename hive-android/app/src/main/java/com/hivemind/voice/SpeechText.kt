package com.hivemind.voice

/**
 * Text hygiene for the voice channel.
 *
 * 2026-10-04 (Plan.md §3.4): the outgoing TTS directive (formerly
 * OUTGOING_PREFIX) moved SERVER-SIDE — VoiceActivity now sends
 * `source: "voice"` and the hub attaches the header + channel hint to
 * the agent prompt. Nothing client-side prefixes message text anymore.
 *
 * [forSpeech] is the safety net: whatever the agent still emits that
 * reads badly aloud gets cleaned before hitting TextToSpeech.
 */
object SpeechText {

    /** Make an agent reply safe for TextToSpeech. */
    fun forSpeech(text: String): String {
        var t = text
        // Fenced code blocks: never read code aloud.
        t = t.replace(Regex("```[\\s\\S]*?```"), " … code omitted … ")
        // Inline code: keep the words, drop the backticks.
        t = t.replace(Regex("`([^`]*)`"), "$1")
        // Markdown links [text](url) → text; bare URLs → "a link".
        t = t.replace(Regex("\\[([^]]+)]\\([^)]*\\)"), "$1")
        t = t.replace(Regex("https?://\\S+"), " a link ")
        // Headings / bullet markers → sentence flow.
        t = t.replace(Regex("(?m)^\\s{0,3}#{1,6}\\s*"), "")
        t = t.replace(Regex("(?m)^\\s*[-*+]\\s+"), "")
        t = t.replace(Regex("(?m)^\\s*\\d+[.)]\\s+"), "")
        // Emphasis markers are meaningless in speech.
        t = t.replace(Regex("[*_~]{1,3}([^*_~]+)[*_~]{1,3}"), "$1")
        // Newlines become pauses, whitespace collapses.
        t = t.replace(Regex("\\n{2,}"), ". ")
        t = t.replace(Regex("\\n"), ", ")
        t = t.replace(Regex("[ \\t]{2,}"), " ")
        t = t.replace(Regex("([.,])\\s*([.,])"), "$1")
        return t.trim()
    }
}
