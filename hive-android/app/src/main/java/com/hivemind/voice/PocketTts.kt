package com.hivemind.voice

import android.content.Context
import android.util.Log
import java.io.File

/**
 * On-device pocket-tts via audio.cpp (same engine + voices as desktop
 * hive-client-voice). Loads the 122MB q8_0 model ONCE per process;
 * [synthesize] runs offline per text chunk and returns 24 kHz mono
 * float PCM. Everything native is serialized inside the JNI layer.
 *
 * Falls back gracefully: [ready] false (model missing / load failed)
 * means callers use the Android system TTS path instead.
 */
object PocketTts {

    private const val TAG = "PocketTts"
    private const val MODEL = "pocket-tts-english-q8_0.gguf"

    /** Voices bundled in assets/models/tts/embeddings — the desktop
     *  voice names, so per-agent mappings match hive-client-voice. */
    val VOICES = listOf("alba", "anna", "bill_boerst", "charles", "eponine",
                        "jean", "mary", "paul", "peter_yearsley", "vera")

    @Volatile var ready = false
        private set
    @Volatile var sampleRate = 24000
        private set

    private var initAttempted = false

    private external fun nativeInit(modelPath: String, threads: Int): Boolean
    private external fun nativeSynthesize(text: String, voiceId: String): FloatArray?
    private external fun nativeSampleRate(): Int
    private external fun nativeShutdown()

    /** Extract assets + load the model. BLOCKING (~2-4s first run,
     *  ~1s warm) — call from a background thread. Idempotent. */
    @Synchronized
    fun init(context: Context): Boolean {
        if (ready) return true
        if (initAttempted) return false      // failed once — don't retry per utterance
        initAttempted = true
        return try {
            extractAssets(context)
            System.loadLibrary("audiocpp")
            System.loadLibrary("pocket_tts_jni")
            val model = File(context.filesDir, "models/tts/$MODEL")
            ready = nativeInit(model.absolutePath, 4)
            if (ready) {
                sampleRate = nativeSampleRate()
                Log.i(TAG, "engine ready @ ${sampleRate}Hz")
            }
            ready
        } catch (e: Throwable) {
            Log.e(TAG, "init failed — system TTS fallback stays", e)
            false
        }
    }

    /** Blocking synthesis. Null = failure (caller falls back). */
    fun synthesize(text: String, voiceId: String): FloatArray? {
        if (!ready) return null
        return try {
            val t0 = System.currentTimeMillis()
            val pcm = nativeSynthesize(text, voiceId)
            if (pcm != null) {
                val ms = System.currentTimeMillis() - t0
                Log.d(TAG, "synth ${text.length} chars → " +
                      "%.1fs audio in ${ms}ms".format(pcm.size / 24000.0, ms))
            }
            pcm
        } catch (e: Throwable) {
            Log.e(TAG, "synthesize failed", e)
            null
        }
    }

    /** Same tmp+rename discipline as SttEngine.extractModels: a crash
     *  mid-copy must not leave a truncated model every launch trusts. */
    private fun extractAssets(context: Context) {
        val dir = File(context.filesDir, "models/tts").apply { mkdirs() }
        val embDir = File(dir, "embeddings").apply { mkdirs() }
        val files = listOf(MODEL) + VOICES.map { "embeddings/$it.safetensors" }
        for (rel in files) {
            val out = File(dir, rel)
            val tmp = File(dir, "$rel.tmp")
            if (tmp.exists()) tmp.delete()
            if (out.exists() && out.length() > 0) continue
            Log.i(TAG, "extracting $rel")
            context.assets.open("models/tts/$rel").use { input ->
                tmp.outputStream().use { input.copyTo(it) }
            }
            if (!tmp.renameTo(out)) {
                tmp.delete()
                throw java.io.IOException("could not install $rel")
            }
        }
    }
}
