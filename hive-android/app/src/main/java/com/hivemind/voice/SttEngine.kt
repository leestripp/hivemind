package com.hivemind.voice

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.util.Log

/**
 * Continuous-listening SpeechRecognizer wrapper.
 *
 * The platform recognizer auto-ends on silence and the Google service
 * throttles rapid restarts, so a naive restart loop eventually dies.
 * This engine treats the USER as the only real stop signal:
 *
 *  - every end/error re-arms with exponential backoff (0.4 s → 4 s cap),
 *  - benign errors (silence timeout, no-match, busy, throttled) never
 *    stop the session — they are normal during pauses in a long rant,
 *  - only repeated HARD errors (service dead, audio failure, …) stop it,
 *    and that stop is ALWAYS reported via [onStopped] with the
 *    transcript preserved.
 *
 * Everything is logged under the HiveStt tag for field diagnosis:
 *   adb logcat -d | grep HiveStt
 *
 * All callbacks arrive on the main thread.
 */
class SttEngine(
    context: Context,
    private val onLevel: (Float) -> Unit,
    private val onTranscript: (String) -> Unit,
    private val onStopped: (String) -> Unit,
) : RecognitionListener {

    private val recognizer: SpeechRecognizer = SpeechRecognizer.createSpeechRecognizer(context)
    private val ui = Handler(Looper.getMainLooper())

    var transcript: String = ""
        private set
    var listening: Boolean = false
        private set

    private var finals = StringBuilder()
    private var currentPartial = ""
    private var hardErrors = 0
    private var rearmDelay = REARM_MIN_MS

    private val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
        putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL,
                 RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
        putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
        putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
        // Ask for very long silence tolerance — hints only, most
        // recognizers cap these; the re-arm loop is the real mechanism.
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
        hardErrors = 0
        rearmDelay = REARM_MIN_MS
        listening = true
        Log.i(TAG, "start")
        arm()
    }

    /** User tapped SEND: stop capturing, keep the transcript. */
    fun stop() {
        if (!listening) return
        listening = false
        ui.removeCallbacksAndMessages(null)
        Log.i(TAG, "stop (user) — transcript ${transcript.length} chars")
        recognizer.stopListening()
    }

    /** User tapped CANCEL: abort and discard everything. */
    fun discard() {
        listening = false
        transcript = ""
        finals = StringBuilder()
        currentPartial = ""
        ui.removeCallbacksAndMessages(null)
        Log.i(TAG, "discard (user)")
        recognizer.cancel()
    }

    fun destroy() {
        ui.removeCallbacksAndMessages(null)
        recognizer.destroy()
    }

    // -- (re)arming ---------------------------------------------------------

    private fun arm() {
        if (!listening) return
        try {
            recognizer.startListening(intent)
        } catch (e: Exception) {
            Log.e(TAG, "startListening threw", e)
            fatal("Couldn't start speech service: ${e.message}")
        }
    }

    private fun rearm(reason: String) {
        if (!listening) return
        val delay = rearmDelay
        rearmDelay = (rearmDelay * 2).coerceAtMost(REARM_MAX_MS)
        Log.d(TAG, "rearm in ${delay}ms ($reason)")
        ui.postDelayed({ arm() }, delay)
    }

    private fun fatal(msg: String) {
        Log.w(TAG, "fatal stop: $msg — transcript ${transcript.length} chars kept")
        listening = false
        ui.removeCallbacksAndMessages(null)
        onStopped(msg)
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

    override fun onReadyForSpeech(params: Bundle?) {
        hardErrors = 0
        rearmDelay = REARM_MIN_MS
    }
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
        rearmDelay = REARM_MIN_MS
        rearm("results")  // silence auto-ended this segment; keep capturing
    }

    override fun onError(error: Int) {
        currentPartial = ""
        val benign = error == SpeechRecognizer.ERROR_NO_MATCH ||
                error == SpeechRecognizer.ERROR_SPEECH_TIMEOUT ||
                error == SpeechRecognizer.ERROR_TOO_MANY_REQUESTS ||
                error == SpeechRecognizer.ERROR_RECOGNIZER_BUSY
        Log.d(TAG, "onError $error (${errorName(error)}) benign=$benign hard=$hardErrors")
        if (benign) {
            rearm(errorName(error))
            return
        }
        hardErrors++
        if (hardErrors < MAX_HARD_ERRORS) {
            rearm(errorName(error))
            return
        }
        val msg = when (error) {
            SpeechRecognizer.ERROR_AUDIO -> "Audio recording error"
            SpeechRecognizer.ERROR_CLIENT -> "Speech service error"
            SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "Microphone permission missing"
            SpeechRecognizer.ERROR_NETWORK,
            SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "Speech network error"
            SpeechRecognizer.ERROR_SERVER -> "Speech service error"
            else -> "Speech error $error"
        }
        fatal(msg)
    }

    private fun errorName(error: Int): String = when (error) {
        SpeechRecognizer.ERROR_AUDIO -> "AUDIO"
        SpeechRecognizer.ERROR_CLIENT -> "CLIENT"
        SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "PERMISSION"
        SpeechRecognizer.ERROR_NETWORK -> "NETWORK"
        SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "NETWORK_TIMEOUT"
        SpeechRecognizer.ERROR_NO_MATCH -> "NO_MATCH"
        SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> "BUSY"
        SpeechRecognizer.ERROR_SERVER -> "SERVER"
        SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "SPEECH_TIMEOUT"
        SpeechRecognizer.ERROR_TOO_MANY_REQUESTS -> "TOO_MANY_REQUESTS"
        SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED -> "LANG_NOT_SUPPORTED"
        SpeechRecognizer.ERROR_LANGUAGE_UNAVAILABLE -> "LANG_UNAVAILABLE"
        SpeechRecognizer.ERROR_CANNOT_CHECK_SUPPORT -> "CANNOT_CHECK_SUPPORT"
        else -> "E$error"
    }

    companion object {
        private const val TAG = "HiveStt"
        private const val REARM_MIN_MS = 400L
        private const val REARM_MAX_MS = 4_000L
        private const val MAX_HARD_ERRORS = 4
    }
}
