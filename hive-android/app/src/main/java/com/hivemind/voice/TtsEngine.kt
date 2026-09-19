package com.hivemind.voice

import android.content.Context
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import android.util.Log

/**
 * Queued-utterance TextToSpeech wrapper for streamed replies.
 *
 * VoiceActivity feeds it sentence chunks AS THE AGENT STREAMS THEM
 * ([speakQueued] → QUEUE_ADD), so the user hears the reply from its
 * first sentence instead of waiting for the full turn. [onQueueEmpty]
 * fires once when every queued chunk has finished playing — the signal
 * to return to idle. Chunks queued before the engine is ready are held
 * and flushed on init.
 */
class TtsEngine(
    context: Context,
    private val onQueueEmpty: () -> Unit,
    private val onReady: (() -> Unit)? = null,
) : UtteranceProgressListener() {

    /** Applied once the engine is ready (per-agent voice selection). */
    var preferredVoiceName: String? = null

    private var tts: TextToSpeech? = null
    @Volatile private var ready = false
    @Volatile var speaking = false
        private set
    @Volatile var outstanding = 0
        private set
    private var seq = 0
    private val pending = mutableListOf<String>()

    init {
        TextToSpeech(context) { status ->
            if (status == TextToSpeech.SUCCESS) {
                tts?.setOnUtteranceProgressListener(this)
                Log.i(TAG, "voices available: ${tts?.voices?.size}")
                ready = true
                preferredVoiceName?.let { setVoiceByName(it) }
                val p = pending.toList()
                pending.clear()
                p.forEach { speakQueued(it) }
                onReady?.invoke()
            }
        }.also { tts = it }
    }

    /** Queue a chunk for speech. Call on the main thread. */
    fun speakQueued(text: String) {
        val engine = tts
        if (!ready || engine == null) { pending += text; return }
        outstanding++
        Log.d(TAG, "queue +${text.length} chars (out=$outstanding)")
        engine.speak(text, TextToSpeech.QUEUE_ADD, null, "$UTTER_PREFIX${seq++}")
    }

    data class VoiceOption(val name: String, val label: String)

    /** English EMBEDDED voices for the picker — network twins excluded. */
    fun englishVoiceOptions(): List<VoiceOption> {
        val engine = tts ?: return emptyList()
        return (engine.voices ?: emptySet())
            .filter {
                it.locale.language == "en" &&
                it.name.endsWith("-local") &&
                !it.isNetworkConnectionRequired
            }
            .sortedWith(compareBy({ it.locale.toString() }, { it.name }))
            .map { v ->
                // "en-us-x-tpf-local" -> "TPF"
                val code = v.name.split('-').dropLast(1).last().uppercase()
                VoiceOption(v.name, "${v.locale.displayCountry} · $code")
            }
    }

    /** Select a voice by engine name ("en-us-x-tpf-local"). */
    fun setVoiceByName(name: String): Boolean {
        val engine = tts ?: return false
        val match = engine.voices?.firstOrNull { it.name == name } ?: return false
        engine.voice = match
        Log.i(TAG, "voice set to $name")
        return true
    }

    /** One-off sample playback (voice picker); interrupts any prior sample. */
    fun preview(text: String) {
        val engine = tts
        if (!ready || engine == null) return
        engine.speak(text, TextToSpeech.QUEUE_FLUSH, null, "${UTTER_PREFIX}preview")
    }

    /** Interrupt: drop everything queued + playing. */
    fun stop() {
        pending.clear()
        outstanding = 0
        speaking = false
        tts?.stop()
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
        if (utteranceId?.startsWith(UTTER_PREFIX) == true && outstanding > 0) {
            outstanding--
            Log.d(TAG, "done (out=$outstanding)")
        }
        if (outstanding == 0) {
            speaking = false
            onQueueEmpty()
        }
    }

    @Deprecated("deprecated in TTS API")
    override fun onError(utteranceId: String?) {
        if (utteranceId?.startsWith(UTTER_PREFIX) == true && outstanding > 0) {
            outstanding--
        }
        if (outstanding == 0) {
            speaking = false
            onQueueEmpty()
        }
    }

    companion object {
        private const val TAG = "HiveTts"
        private const val UTTER_PREFIX = "hive-"
    }
}
