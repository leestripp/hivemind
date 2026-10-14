package com.hivemind.voice

import android.content.Context
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.io.File
import kotlin.math.min
import kotlin.math.sqrt
import kotlin.concurrent.thread

/**
 * On-device STT via whisper.cpp (ggml-base.en, CPU+NEON).
 *
 * Whisper is non-streaming, so partials are sliding-window
 * re-transcription: every [PARTIAL_MS] the trailing [WINDOW_MS] of audio
 * gets a fast single-segment decode; SEND runs the full multi-segment
 * pass over everything. Same no-endpointer contract as the sherpa engine
 * it replaced: recording stops only on SEND ([stop]) or CANCEL
 * ([discard]). Mic RMS feeds the sphere; ASR never touches the UI thread.
 *
 * Diagnostics: adb logcat -d | grep -E "HiveStt|HiveWhisper"
 */
class SttEngine(
    private val context: Context,
    private val onLevel: (Float) -> Unit,
    private val onTranscript: (String) -> Unit,
    private val onStopped: (String) -> Unit,
) {

    @Volatile var transcript: String = ""
        private set
    val engineReady: Boolean get() = ready
    @Volatile var listening: Boolean = false
        private set

    /** Push-to-talk playback hold (2026-10-14): fires on every capture
     *  transition (true = recording, false = stopped) so the owner can
     *  hold TTS — agents must not talk over the user's recording.
     *  Fires on the bg thread; the owner must post to the main thread
     *  before touching UI/TTS. */
    var onRecordingChanged: ((Boolean) -> Unit)? = null

    private val ui = Handler(Looper.getMainLooper())
    private val worker = HandlerThread("hive-stt").apply { start() }
    private val bg = Handler(worker.looper)

    @Volatile private var ready = false
    @Volatile private var wantListen = false
    /** Bumped on every start/stop/discard — stale queued starts no-op. */
    @Volatile private var session = 0

    private var record: AudioRecord? = null
    private var captureThread: Thread? = null

    // Growing PCM buffer (16kHz mono shorts), capped at MAX_SECONDS.
    // Written by the capture thread, snapshotted by the partial ticker —
    // all access under pcmLock.
    private val pcmLock = Any()
    private var pcm = ShortArray(SAMPLE_RATE * 30)
    private var pcmLen = 0

    @Volatile private var decodeInFlight = false
    private val partialTick = object : Runnable {
        override fun run() {
            if (listening) {
                decodePartial()
                bg.postDelayed(this, PARTIAL_MS)
            }
        }
    }

    init {
        bg.post { initEngine() }
    }

    // -- Lifecycle ---------------------------------------------------------

    private fun initEngine() {
        try {
            val dir = File(context.filesDir, "stt")
            extractModels(dir)
            if (!WhisperNative.nativeInit(
                    File(dir, MODEL_PARTIAL).absolutePath,
                    WhisperNative.WHICH_PARTIAL))
                throw java.io.IOException("whisper.cpp rejected $MODEL_PARTIAL")
            if (!WhisperNative.nativeInit(
                    File(dir, MODEL_FINAL).absolutePath,
                    WhisperNative.WHICH_FINAL))
                throw java.io.IOException("whisper.cpp rejected $MODEL_FINAL")
            ready = true
            Log.i(TAG, "models ready (partial=$MODEL_PARTIAL final=$MODEL_FINAL)")
            if (wantListen) arm()
        } catch (e: Exception) {
            Log.e(TAG, "model init failed", e)
            ui.post { onStopped("Speech model failed to load: ${e.message}") }
        }
    }

    /** Copy bundled models out of assets on first run (native needs paths).
     *  Extraction is ATOMIC (.tmp + rename): a process killed mid-copy used
     *  to leave a truncated model that every later launch trusted and the
     *  native loader then rejected — the app stayed broken forever. */
    private fun extractModels(dir: File) {
        dir.mkdirs()
        for (name in MODEL_FILES) {
            val out = File(dir, name)
            val tmp = File(dir, "$name.tmp")
            if (tmp.exists()) tmp.delete()   // stale partial from a crash
            if (out.exists() && out.length() > 0) continue
            Log.i(TAG, "extracting $name")
            context.assets.open("models/stt/$name").use { input ->
                tmp.outputStream().use { input.copyTo(it) }
            }
            if (!tmp.renameTo(out)) {
                tmp.delete()
                throw java.io.IOException("could not install model $name")
            }
        }
    }

    /** User tapped TALK. NEVER a silent no-op: a stale/zombie capture
     *  gets reset, a not-yet-ready engine (re)loads and arms when done.
     *  Everything happens on the bg thread — teardownCapture() joins the
     *  capture thread, which must never block the UI. */
    fun start() {
        wantListen = true
        val s = ++session
        bg.post {
            if (!wantListen || s != session) return@post
            if (listening) {
                Log.w(TAG, "start() found stale capture — resetting")
                teardownCapture()
            }
            if (!ready) { initEngine(); return@post }  // arms itself on success
            if (!listening) startCapture()
        }
    }

    private fun arm() {
        val s = ++session
        bg.post { if (wantListen && s == session && !listening) startCapture() }
    }

    /** User tapped SEND: stop capturing, run the full final decode, keep
     *  the transcript. ASYNC — teardown + final pass run on the bg thread
     *  (joining the capture thread on the UI could stall it for ~2s worst
     *  case); [onFinalized] fires on the UI thread once [transcript] is
     *  final. */
    fun stop(onFinalized: (() -> Unit)? = null) {
        wantListen = false
        session++
        bg.post {
            if (listening) teardownCapture()
            if (ready) {
                val snap = synchronized(pcmLock) { pcm.copyOf(pcmLen) }
                if (snap.isNotEmpty()) {
                    val t0 = SystemClock.elapsedRealtime()
                    runCatching {
                        val t = WhisperNative.nativeTranscribe(
                            snap, snap.size, THREADS, true).trim()
                        if (t.isNotEmpty()) transcript = t
                    }.onFailure { Log.e(TAG, "final decode failed", it) }
                    Log.i(TAG, "final decode ${SystemClock.elapsedRealtime() - t0}ms " +
                        "for ${snap.size / SAMPLE_RATE}s audio")
                }
            }
            Log.i(TAG, "stop (user) — transcript ${transcript.length} chars")
            if (onFinalized != null) ui.post { onFinalized() }
        }
    }

    /** Kill the capture thread + mic. Idempotent. */
    private fun teardownCapture() {
        listening = false
        onRecordingChanged?.invoke(false)
        try {
            captureThread?.join(2_000)
        } catch (_: InterruptedException) { Thread.currentThread().interrupt() }
        captureThread = null
        record?.let { r -> runCatching { r.stop() }; r.release() }
        record = null
    }

    /** User tapped CANCEL: abort and discard. Teardown runs on bg —
     *  same no-UI-blocking rule as [stop]. */
    fun discard() {
        wantListen = false
        session++
        transcript = ""
        synchronized(pcmLock) { pcmLen = 0 }
        bg.post { if (listening) teardownCapture() }
        Log.i(TAG, "discard (user)")
    }

    fun destroy() {
        discard()
        bg.post {
            WhisperNative.nativeFree()
            worker.quitSafely()
        }
    }

    // -- Partial decode ------------------------------------------------------

    /** Sliding-window partial: transcribe the trailing WINDOW_MS of audio
     *  (single-segment, fast). Skips a tick if the previous decode is
     *  still running — decode slower than PARTIAL_MS must never pile up. */
    private fun decodePartial() {
        if (!ready || decodeInFlight) return
        val snap = synchronized(pcmLock) {
            val n = min(pcmLen, WINDOW_SAMPLES)
            if (n < SAMPLE_RATE / 2) return   // ignore sub-500ms blips
            pcm.copyOfRange(pcmLen - n, pcmLen)
        }
        // VAD gate: a silent window decodes to nothing useful — skip the
        // CPU/battery cost. Threshold sits well under normal speech RMS.
        var peak = 0
        for (v in snap) { val a = kotlin.math.abs(v.toInt()); if (a > peak) peak = a }
        if (peak < VAD_PEAK_MIN) {
            Log.d(TAG, "partial skipped (silence, peak=$peak)")
            return
        }
        decodeInFlight = true
        thread(name = "hive-stt-partial") {
            try {
                val t0 = SystemClock.elapsedRealtime()
                val t = WhisperNative.nativeTranscribe(
                    snap, snap.size, THREADS, false).trim()
                Log.i(TAG, "partial decode ${SystemClock.elapsedRealtime() - t0}ms")
                if (t.isNotEmpty() && listening) {
                    transcript = t
                    ui.post { onTranscript(t) }
                }
            } catch (e: Exception) {
                Log.e(TAG, "partial decode failed", e)
            } finally {
                decodeInFlight = false
            }
        }
    }

    // -- Capture -------------------------------------------------------------

    private fun startCapture() {
        if (!ready || listening) return
        transcript = ""
        synchronized(pcmLock) { pcmLen = 0 }

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, android.media.AudioFormat.CHANNEL_IN_MONO,
            android.media.AudioFormat.ENCODING_PCM_16BIT)
        val ar = try {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_RECOGNITION, SAMPLE_RATE,
                android.media.AudioFormat.CHANNEL_IN_MONO,
                android.media.AudioFormat.ENCODING_PCM_16BIT,
                maxOf(minBuf, 8192))
        } catch (e: SecurityException) {
            Log.e(TAG, "AudioRecord denied", e)
            ui.post { onStopped("Microphone permission missing") }
            return
        }
        if (ar.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord init failed")
            ar.release()
            ui.post { onStopped("Microphone unavailable") }
            return
        }
        record = ar
        listening = true
        onRecordingChanged?.invoke(true)
        Log.i(TAG, "capture start")
        ar.startRecording()
        bg.postDelayed(partialTick, PARTIAL_MS)

        captureThread = thread(name = "hive-stt-capture") {
            val shorts = ShortArray(SAMPLE_RATE / 10)   // 100 ms chunks
            // Adaptive level reference (fast attack, slow release): mic
            // gain varies wildly across devices/sources, so a fixed RMS
            // divisor leaves the sphere dead on quiet mics. Normal
            // speech sits ~0.7; the loudest recent sound ~1.0.
            var rmsRef = 400.0
            try {
                while (listening) {
                    val n = ar.read(shorts, 0, shorts.size)
                    if (n <= 0) continue
                    var sum = 0.0
                    for (i in 0 until n) {
                        val v = shorts[i].toInt()
                        sum += (v * v).toDouble()
                    }
                    val rms = sqrt(sum / n)
                    rmsRef = if (rms > rmsRef) rms
                    else rmsRef * 0.999 + rms * 0.001
                    onLevel((rms / (rmsRef * 1.3)).coerceIn(0.0, 1.0).toFloat())
                    synchronized(pcmLock) {
                        if (pcmLen + n > MAX_SAMPLES) {
                            // Cap reached: drop the oldest half, keep going.
                            val keep = MAX_SAMPLES / 2
                            System.arraycopy(pcm, pcmLen - keep, pcm, 0, keep)
                            pcmLen = keep
                        }
                        if (pcmLen + n > pcm.size) {
                            pcm = pcm.copyOf((pcm.size * 2)
                                .coerceAtMost(MAX_SAMPLES))
                        }
                        System.arraycopy(shorts, 0, pcm, pcmLen, n)
                        pcmLen += n
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "capture loop failed", e)
                if (listening) ui.post { onStopped("Recording failed: ${e.message}") }
            }
        }
    }

    companion object {
        private const val TAG = "HiveStt"
        private const val SAMPLE_RATE = 16_000
        private const val THREADS = 4
        private const val PARTIAL_MS = 2_000L
        private const val WINDOW_SAMPLES = SAMPLE_RATE * 5   // 5 s window
        private const val MAX_SECONDS = 120
        private const val MAX_SAMPLES = SAMPLE_RATE * MAX_SECONDS
        private const val VAD_PEAK_MIN = 500   // PCM16 peak; speech ~3-20k
        private const val MODEL_PARTIAL = "ggml-tiny.en-q8_0.bin"
        private const val MODEL_FINAL = "ggml-base.en-q8_0.bin"
        private val MODEL_FILES = listOf(MODEL_PARTIAL, MODEL_FINAL)
    }
}
