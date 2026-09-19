package com.hivemind.voice

import android.content.Context
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener

/**
 * Android platform TextToSpeech wrapper. Speaks the agent reply in one
 * queued utterance; exposes a synthetic level envelope while speaking so
 * the sphere keeps moving with the agent's voice.
 */
class TtsEngine(
    context: Context,
    private val onDone: () -> Unit,
) : UtteranceProgressListener() {

    private var tts: TextToSpeech? = null
    @Volatile private var ready = false
    @Volatile var speaking = false
        private set
    private var pending: String? = null

    init {
        TextToSpeech(context) { status ->
            if (status == TextToSpeech.SUCCESS) {
                tts?.setOnUtteranceProgressListener(this)
                ready = true
                pending?.let { speak(it) }
                pending = null
            }
        }.also { tts = it }
    }

    fun speak(text: String) {
        val engine = tts
        if (!ready || engine == null) { pending = text; return }
        speaking = true
        engine.speak(text, TextToSpeech.QUEUE_FLUSH, null, UTTERANCE_ID)
    }

    fun stop() {
        pending = null
        if (speaking) {
            speaking = false
            tts?.stop()
        }
    }

    fun destroy() {
        tts?.stop()
        tts?.shutdown()
        tts = null
    }

    /** Animated 0..1 envelope for the sphere while the agent talks. */
    fun level(nowSec: Float): Float {
        if (!speaking) return 0f
        val base = 0.38f + 0.22f * kotlin.math.sin(nowSec * 7.3f) *
                kotlin.math.sin(nowSec * 2.1f + 1.3f)
        return base.coerceIn(0.08f, 0.75f)
    }

    override fun onStart(utteranceId: String?) { speaking = true }

    override fun onDone(utteranceId: String?) {
        if (utteranceId == UTTERANCE_ID) {
            speaking = false
            onDone()
        }
    }

    @Deprecated("deprecated in TTS API")
    override fun onError(utteranceId: String?) {
        if (utteranceId == UTTERANCE_ID) {
            speaking = false
            onDone()
        }
    }

    companion object {
        private const val UTTERANCE_ID = "hive-reply"
    }
}
