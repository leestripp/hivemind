package com.hivemind.voice

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer

/**
 * Continuous-listening SpeechRecognizer wrapper.
 *
 * The platform recognizer auto-ends on silence — often mid-sentence.
 * For a push-to-talk voice session the USER is the only stop signal:
 * [start] keeps re-arming the recognizer on every end/error until
 * [stop] (send) or [discard] (cancel) is called. Transcripts from each
 * segment accumulate into [transcript] and stream to [onTranscript].
 *
 * All callbacks arrive on the main thread.
 */
class SttEngine(
    context: Context,
    private val onLevel: (Float) -> Unit,
    private val onTranscript: (String) -> Unit,
    private val onFatal: (String) -> Unit,
) : RecognitionListener {

    private val recognizer: SpeechRecognizer = SpeechRecognizer.createSpeechRecognizer(context)
    private val ui = Handler(Looper.getMainLooper())

    var transcript: String = ""
        private set
    var listening: Boolean = false
        private set

    private var finals = StringBuilder()
    private var currentPartial = ""
    private var consecutiveErrors = 0

    private val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
        putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL,
                 RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
        putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
        putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
        // Ask for very long silence tolerance — hints only, most
        // recognizers cap these; the restart loop is the real mechanism.
        putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS, 60_000L)
        putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_POSSIBLY_COMPLETE_SILENCE_LENGTH_MILLIS, 60_000L)
        putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_MINIMUM_LENGTH_MILLIS, 2_000L)
    }

    init {
        recognizer.setRecognitionListener(this)
    }

    fun start() {
        transcript = ""
        finals = StringBuilder()
        currentPartial = ""
        consecutiveErrors = 0
        listening = true
        recognizer.startListening(intent)
    }

    /** User tapped SEND: stop capturing, keep the transcript. */
    fun stop() {
        if (!listening) return
        listening = false
        ui.removeCallbacksAndMessages(null)
        recognizer.stopListening()
    }

    /** User tapped CANCEL: abort and discard everything. */
    fun discard() {
        listening = false
        transcript = ""
        finals = StringBuilder()
        currentPartial = ""
        ui.removeCallbacksAndMessages(null)
        recognizer.cancel()
    }

    fun destroy() {
        ui.removeCallbacksAndMessages(null)
        recognizer.destroy()
    }

    /** Re-arm after an end/error while the user hasn't stopped us. */
    private fun rearm() {
        if (!listening) return
        ui.postDelayed({
            if (listening) recognizer.startListening(intent)
        }, 350)
    }

    private fun publish() {
        val base = finals.toString()
        transcript = when {
            base.isEmpty() -> currentPartial
            currentPartial.isEmpty() -> base
            else -> "$base $currentPartial"
        }
        onTranscript(transcript)
    }

    // -- RecognitionListener ---------------------------------------------

    override fun onReadyForSpeech(params: Bundle?) { consecutiveErrors = 0 }
    override fun onBeginningOfSpeech() {}
    override fun onBufferReceived(buffer: ByteArray?) {}
    override fun onEvent(eventType: Int, params: Bundle?) {}
    override fun onEndOfSpeech() {}

    override fun onRmsChanged(rmsdB: Float) {
        // SpeechRecognizer RMS is roughly -2..10 dB.
        onLevel(((rmsdB + 2f) / 12f).coerceIn(0f, 1f))
    }

    override fun onPartialResults(partialResults: Bundle?) {
        currentPartial = partialResults
            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            ?.firstOrNull().orEmpty()
        publish()
    }

    override fun onResults(results: Bundle?) {
        val text = results
            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            ?.firstOrNull()
        if (!text.isNullOrBlank()) {
            if (finals.isNotEmpty()) finals.append(' ')
            finals.append(text.trim())
        }
        currentPartial = ""
        publish()
        rearm()  // silence auto-ended this segment; keep capturing
    }

    override fun onError(error: Int) {
        currentPartial = ""
        consecutiveErrors++
        if (listening && consecutiveErrors < MAX_CONSECUTIVE_ERRORS) {
            rearm()  // transient (timeout/no-match): keep the session alive
            return
        }
        listening = false
        val msg = when (error) {
            SpeechRecognizer.ERROR_AUDIO -> "Audio recording error"
            SpeechRecognizer.ERROR_CLIENT -> "Speech service error"
            SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "Microphone permission missing"
            SpeechRecognizer.ERROR_NETWORK,
            SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "Speech network error"
            SpeechRecognizer.ERROR_NO_MATCH -> "Didn't catch that"
            SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> "Speech service busy"
            SpeechRecognizer.ERROR_SERVER -> "Speech service error"
            SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "No speech heard"
            else -> "Speech error $error"
        }
        if (transcript.isEmpty()) onFatal(msg)
    }

    companion object {
        /** Give up after this many back-to-back failures (dead service). */
        private const val MAX_CONSECUTIVE_ERRORS = 6
    }
}
