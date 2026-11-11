package com.hivemind.voice

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Build
import android.util.Log
import java.util.concurrent.ArrayBlockingQueue
import kotlin.math.sqrt

/**
 * Queued float-PCM player for PocketTts output (AudioTrack, 24 kHz
 * mono float). One writer thread drains the queue; [pause] is a TRUE
 * hold — AudioTrack.pause() freezes mid-word and [resume] continues
 * exactly where it stopped (the desktop push-to-talk semantics the
 * system-TTS path can only approximate by re-queueing sentences).
 *
 * [onChunkPlayed] fires as each enqueued chunk finishes PLAYING (not
 * when enqueued) — TtsEngine's outstanding accounting hangs off it.
 */
class PcmPlayer(
    context: Context,
    private val sampleRate: Int,
    private val onChunkPlayed: (counted: Boolean) -> Unit,
    private val onLevel: (Float) -> Unit,
) {
    private data class Item(val pcm: FloatArray?, val counted: Boolean)
    /** ArrayBlockingQueue rejects NULL elements (offer → NPE — the
     *  2026-10-14 destroy crash): poison is an explicit sentinel Item. */
    private val poison = Item(null, counted = false)
    private val queue = ArrayBlockingQueue<Item>(64)
    /** Bumped by [stop]: the chunk mid-write when a stop lands must
     *  abort (else audio continues past the interrupt and its
     *  onChunkPlayed decrements an already-zeroed count). */
    private val writeGen = java.util.concurrent.atomic.AtomicInteger(0)
    @Volatile private var running = true
    @Volatile var paused = false
        private set
    @Volatile var playing = false
        private set

    private val track: AudioTrack
    private val appContext = context.applicationContext
    private val audioManager = appContext.getSystemService(AudioManager::class.java)
    private var volumeReceiverRegistered = false
    private val volumeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val stream = intent?.getIntExtra(EXTRA_VOLUME_STREAM_TYPE, -1) ?: -1
            // MUSIC is included because ASSISTANT is aliased to it on some
            // devices; the gain is always read from the assistant stream.
            if (stream == STREAM_ASSISTANT || stream == AudioManager.STREAM_MUSIC)
                applyPlaybackVolume("key")
        }
    }

    private val writer = Thread {
        while (running) {
            val item = try { queue.take() } catch (_: InterruptedException) { null } ?: break
            if (item === poison) break
            val g = writeGen.get()
            item.pcm?.let { writeChunk(it, g) }
            if (g == writeGen.get()) onChunkPlayed(item.counted)
        }
    }.apply { name = "hive-pcm"; start() }

    init {
        val minBuf = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT)
        track = AudioTrack(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANT)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build(),
            AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build(),
            maxOf(minBuf, sampleRate),   // ~1s
            AudioTrack.MODE_STREAM,
            AudioManager.AUDIO_SESSION_ID_GENERATE)
        track.play()
        applyPlaybackVolume("initial")
        val filter = IntentFilter(VOLUME_CHANGED_ACTION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            appContext.registerReceiver(volumeReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else
            @Suppress("DEPRECATION") appContext.registerReceiver(volumeReceiver, filter)
        volumeReceiverRegistered = true
    }

    /** Extra low-volume headroom, separate from Android's device attenuation.
     *  Square-root curve: gentle through the middle, full gain at maximum,
     *  and no unexpected +12 dB voice boost at the bottom of the rocker. */
    private fun applyPlaybackVolume(reason: String) {
        try {
            val index = audioManager.getStreamVolume(STREAM_ASSISTANT)
            val max = audioManager.getStreamMaxVolume(STREAM_ASSISTANT)
            val gain = playbackGainForVolume(index, max)
            val result = track.setVolume(gain)
            if (result != AudioTrack.SUCCESS) Log.w(TAG, "setVolume returned $result")
            Log.d(TAG, "volume $reason: assistant=$index/$max gain=%.3f".format(gain))
        } catch (e: RuntimeException) {
            Log.w(TAG, "could not apply assistant volume", e)
        }
    }

    /** [counted]=false for picker previews — their completion must not
     *  touch TtsEngine's outstanding accounting. */
    fun enqueue(pcm: FloatArray, counted: Boolean = true) {
        if (running) queue.offer(Item(pcm, counted))
    }

    /** Hold/resume — position kept, nothing lost. */
    fun pause()  { if (!paused) { paused = true; runCatching { track.pause() } } }
    fun resume() { if (paused)  { paused = false; runCatching { track.play() } } }

    /** Drop everything queued + playing (user interrupt). */
    fun stop() {
        writeGen.incrementAndGet()   // abort the mid-write chunk
        queue.clear()
        paused = false               // unpause FIRST or the writer waits
        runCatching {
            track.pause()
            track.flush()
            track.play()
        }
        playing = false
        onLevel(0f)
    }

    fun destroy() {
        running = false
        if (volumeReceiverRegistered) {
            runCatching { appContext.unregisterReceiver(volumeReceiver) }
            volumeReceiverRegistered = false
        }
        queue.clear()
        queue.offer(poison)        // unblock the writer; it breaks on it
        runCatching { writer.join(2_000) }
        runCatching { track.stop(); track.release() }
    }

    private fun writeChunk(pcm: FloatArray, gen: Int) {
        playing = true
        var off = 0
        // Level metering slice: report RMS per ~100ms window while writing.
        val win = sampleRate / 10
        while (off < pcm.size && running && gen == writeGen.get()) {
            while (paused && running && gen == writeGen.get()) Thread.sleep(20)
            if (!running || gen != writeGen.get()) break
            val n = minOf(win, pcm.size - off)
            var sum = 0.0
            for (i in off until off + n) { val s = pcm[i]; sum += s * s }
            onLevel(kotlin.math.sqrt(sum / n).toFloat().coerceAtMost(1f))
            val w = track.write(pcm, off, n, AudioTrack.WRITE_BLOCKING)
            if (w <= 0) { Log.w(TAG, "track.write returned $w"); break }
            off += w
        }
        playing = queue.isNotEmpty()
        if (!playing) onLevel(0f)
    }

    companion object {
        private const val TAG = "PcmPlayer"
        // AudioManager.STREAM_ASSISTANT is @SystemApi; stable since API 26,
        // which is also this app's minSdk. Using the public int API is enough.
        private const val STREAM_ASSISTANT = 11
        private const val VOLUME_CHANGED_ACTION = "android.media.VOLUME_CHANGED_ACTION"
        private const val EXTRA_VOLUME_STREAM_TYPE = "android.media.EXTRA_VOLUME_STREAM_TYPE"
    }
}

internal fun playbackGainForVolume(index: Int, maxIndex: Int): Float {
    if (index <= 0 || maxIndex <= 0) return 0f
    return sqrt(index.toFloat() / maxIndex).coerceIn(0f, 1f)
}
