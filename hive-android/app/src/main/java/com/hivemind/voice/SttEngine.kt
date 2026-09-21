package com.hivemind.voice

import android.content.Context
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import com.k2fsa.sherpa.onnx.FeatureConfig
import com.k2fsa.sherpa.onnx.OnlineModelConfig
import com.k2fsa.sherpa.onnx.OnlineRecognizer
import com.k2fsa.sherpa.onnx.OnlineRecognizerConfig
import com.k2fsa.sherpa.onnx.OnlineStream
import com.k2fsa.sherpa.onnx.OnlineTransducerModelConfig
import java.io.File
import kotlin.math.sqrt
import kotlin.concurrent.thread

/**
 * On-device streaming STT (sherpa-onnx Zipformer2 transducer, int8).
 *
 * Unlike the platform SpeechRecognizer there is NO endpointer: the
 * recognizer never stops on silence — recording runs until the user
 * taps SEND ([stop]) or CANCEL ([discard]). No sessions, no throttling,
 * no audio cues, works offline. Mic RMS feeds the sphere; partial
 * results stream to [onTranscript] ~5x/second.
 *
 * Same contract as the platform-based engine it replaced.
 * Diagnostics: adb logcat -d | grep HiveStt
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

    private val ui = Handler(Looper.getMainLooper())
    private val worker = HandlerThread("hive-stt").apply { start() }
    private val bg = Handler(worker.looper)

    @Volatile private var ready = false
    @Volatile private var wantListen = false
    /** Bumped on every start/stop/discard — stale queued starts no-op. */
    @Volatile private var session = 0

    private var recognizer: OnlineRecognizer? = null
    private var stream: OnlineStream? = null
    private var record: AudioRecord? = null
    private var captureThread: Thread? = null

    init {
        bg.post { initEngine() }
    }

    // -- Lifecycle ---------------------------------------------------------

    private fun initEngine() {
        try {
            val dir = File(context.filesDir, "stt")
            extractModels(dir)
            val config = OnlineRecognizerConfig(
                featConfig = FeatureConfig(sampleRate = SAMPLE_RATE, featureDim = 80),
                modelConfig = OnlineModelConfig(
                    transducer = OnlineTransducerModelConfig(
                        encoder = File(dir, "encoder.onnx").absolutePath,
                        decoder = File(dir, "decoder.onnx").absolutePath,
                        joiner = File(dir, "joiner.onnx").absolutePath,
                    ),
                    tokens = File(dir, "tokens.txt").absolutePath,
                    numThreads = 2,
                    debug = false,
                    provider = "cpu",
                    modelType = "zipformer2",
                ),
                enableEndpoint = false,   // silence NEVER stops a recording
                decodingMethod = "greedy_search",
            )
            recognizer = OnlineRecognizer(config = config)
            ready = true
            Log.i(TAG, "model ready (${dir})")
            if (wantListen) arm()
        } catch (e: Exception) {
            Log.e(TAG, "model init failed", e)
            ui.post { onStopped("Speech model failed to load: ${e.message}") }
        }
    }

    /** Copy bundled models out of assets on first run (native needs paths).
     *  Extraction is ATOMIC (.tmp + rename): a process killed mid-copy used
     *  to leave a truncated .onnx that every later launch trusted and the
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

    /** User tapped SEND: stop capturing, finalize, keep the transcript.
     *  ASYNC — teardown + tail decode run on the bg thread (joining the
     *  capture thread on the UI could stall it for ~2s worst case);
     *  [onFinalized] fires on the UI thread once [transcript] is final. */
    fun stop(onFinalized: (() -> Unit)? = null) {
        wantListen = false
        session++
        bg.post {
            if (listening) teardownCapture()
            // Decode whatever is still buffered, then take the final result.
            val rec = recognizer; val st = stream
            if (rec != null && st != null) {
                runCatching {
                    st.inputFinished()
                    while (rec.isReady(st)) rec.decode(st)
                    val t = rec.getResult(st).text.trim()
                    if (t.isNotEmpty()) transcript = t
                }
            }
            Log.i(TAG, "stop (user) — transcript ${transcript.length} chars")
            if (onFinalized != null) ui.post { onFinalized() }
        }
    }

    /** Kill the capture thread + mic. Idempotent. */
    private fun teardownCapture() {
        listening = false
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
        bg.post { if (listening) teardownCapture() }
        Log.i(TAG, "discard (user)")
    }

    fun destroy() {
        discard()
        // quitSafely() drops anything posted AFTER it, so the release must
        // be queued FIRST and the quit issued from inside that same block —
        // the old order leaked the native recognizer on every destroy.
        bg.post {
            stream?.release(); stream = null
            recognizer?.release(); recognizer = null
            worker.quitSafely()
        }
    }

    // -- Capture -------------------------------------------------------------

    private fun startCapture() {
        val rec = recognizer ?: return
        if (listening) return
        transcript = ""
        stream?.release()
        val st = rec.createStream()
        stream = st

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
        Log.i(TAG, "capture start")
        ar.startRecording()

        captureThread = thread(name = "hive-stt-capture") {
            val shorts = ShortArray(SAMPLE_RATE / 10)   // 100 ms chunks
            val floats = FloatArray(SAMPLE_RATE / 10)
            var lastPublish = 0L
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
                        floats[i] = v / 32768f
                    }
                    val rms = sqrt(sum / n)
                    rmsRef = if (rms > rmsRef) rms
                    else rmsRef * 0.999 + rms * 0.001
                    onLevel((rms / (rmsRef * 1.3)).coerceIn(0.0, 1.0).toFloat())
                    st.acceptWaveform(
                        if (n == floats.size) floats else floats.copyOf(n),
                        SAMPLE_RATE)
                    while (rec.isReady(st)) rec.decode(st)
                    val now = System.currentTimeMillis()
                    if (now - lastPublish > 200) {
                        lastPublish = now
                        val t = rec.getResult(st).text.trim()
                        if (t != transcript) {
                            transcript = t
                            ui.post { onTranscript(t) }
                        }
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
        private val MODEL_FILES = listOf(
            "encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt")
    }
}
