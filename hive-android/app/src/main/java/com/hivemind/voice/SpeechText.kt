package com.hivemind.voice

/**
 * Text hygiene for the voice channel.
 *
 * [OUTGOING_PREFIX] rides on every user message from this app so the
 * agent knows the reply will be spoken aloud and should stay in plain,
 * flowing, TTS-friendly prose.
 *
 * [forSpeech] is the safety net: whatever the agent still emits that
 * reads badly aloud gets cleaned before hitting TextToSpeech.
 */
object SpeechText {

    const val OUTGOING_PREFIX: String =
        "(Voice message via the HiveMind Voice app — your reply is read " +
        "aloud by text-to-speech, so answer in plain flowing prose: no " +
        "markdown, lists, tables, code blocks, links, or emoji; write " +
        "numbers, dates and abbreviations the way they should be spoken; " +
        "keep it conversational and concise unless I ask for detail.)\n\n"

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
