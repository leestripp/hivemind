package com.hivemind.voice

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer

/**
 * Android platform SpeechRecognizer wrapper. All callbacks arrive on the
 * main thread. The engine accumulates partial + final transcripts so the
 * user can tap SEND even after the recognizer auto-ends on silence.
 */
class SttEngine(
    context: Context,
    private val onLevel: (Float) -> Unit,
    private val onStateChange: (Boolean) -> Unit,
    private val onError: (String) -> Unit,
) : RecognitionListener {

    private val recognizer: SpeechRecognizer = SpeechRecognizer.createSpeechRecognizer(context)

    var transcript: String = ""
        private set
    var listening: Boolean = false
        private set

    private var finals = StringBuilder()

    init {
        recognizer.setRecognitionListener(this)
    }

    fun start() {
        transcript = ""
        finals = StringBuilder()
        val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
            putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL,
                     RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
            // Generous silence windows — the user taps SEND explicitly,
            // so don't cut them off mid-thought.
            putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS, 4000L)
            putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_POSSIBLY_COMPLETE_SILENCE_LENGTH_MILLIS, 4000L)
            putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_MINIMUM_LENGTH_MILLIS, 1500L)
        }
        listening = true
        onStateChange(true)
        recognizer.startListening(intent)
    }

    /** Stop listening; transcript stays available for sending. */
    fun stop() {
        if (!listening) return
        listening = false
        recognizer.stopListening()
        onStateChange(false)
    }

    /** Abort and discard — the cancel path. */
    fun discard() {
        listening = false
        transcript = ""
        finals = StringBuilder()
        recognizer.cancel()
        onStateChange(false)
    }

    fun destroy() = recognizer.destroy()

    // -- RecognitionListener ---------------------------------------------

    override fun onReadyForSpeech(params: Bundle?) {}
    override fun onBeginningOfSpeech() {}
    override fun onBufferReceived(buffer: ByteArray?) {}
    override fun onEvent(eventType: Int, params: Bundle?) {}

    override fun onRmsChanged(rmsdB: Float) {
        // SpeechRecognizer RMS is roughly -2..10 dB.
        onLevel(((rmsdB + 2f) / 12f).coerceIn(0f, 1f))
    }

    override fun onPartialResults(partialResults: Bundle?) {
        val part = partialResults
            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            ?.firstOrNull() ?: return
        transcript = merge(part)
    }

    override fun onResults(results: Bundle?) {
        val text = results
            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            ?.firstOrNull()
        if (!text.isNullOrBlank()) {
            if (finals.isNotEmpty()) finals.append(' ')
            finals.append(text.trim())
        }
        transcript = finals.toString()
        if (listening) { listening = false; onStateChange(false) }
    }

    override fun onEndOfSpeech() {}

    override fun onError(error: Int) {
        if (listening) { listening = false; onStateChange(false) }
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
        // NO_MATCH / SPEECH_TIMEOUT with a partial transcript is fine —
        // the user can still tap SEND.
        if (transcript.isEmpty()) onError(msg)
    }

    private fun merge(partial: String): String {
        val base = finals.toString()
        return if (base.isEmpty()) partial else "$base $partial"
    }
}
