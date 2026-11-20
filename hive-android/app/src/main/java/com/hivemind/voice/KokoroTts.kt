package com.hivemind.voice

import android.content.Context
import android.util.Log
import java.io.File

/**
 * On-device Kokoro 82M via audio.cpp (same engine + voices as ChimpFlow's
 * kokoro-82m on the desktop). Loads the 203MB BF16 GGUF ONCE per process;
 * [synthesize] runs offline per text chunk and returns 24 kHz mono
 * float PCM. Everything native is serialized inside the JNI layer.
 *
 * Falls back gracefully: [ready] false (model missing / load failed)
 * means callers use the Android system TTS path instead.
 */
object KokoroTts {

    private const val TAG = "KokoroTts"
    private const val MODEL = "kokoro-82m-bf16.gguf"

    /** English Kokoro voices — all voice packs ship INSIDE the GGUF, so
     *  no embeddings assets are needed. af/am = American, bf/bm = British
     *  (the JNI picks the matching text-frontend language per request).
     *  Ids match ChimpFlow's /v1/voices?model=kokoro-82m, so per-agent
     *  mappings match the desktop integrated voice. */
    val VOICES = listOf(
        "af_alloy", "af_aoede", "af_bella", "af_heart", "af_jessica",
        "af_kore", "af_nicole", "af_nova", "af_river", "af_sarah", "af_sky",
        "am_adam", "am_echo", "am_eric", "am_fenrir", "am_liam",
        "am_michael", "am_onyx", "am_puck", "am_santa",
        "bf_alice", "bf_emma", "bf_isabella", "bf_lily",
        "bm_daniel", "bm_fable", "bm_george", "bm_lewis")

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
            // Kokoro phonemizes through eSpeak-ng (same runtime the
            // desktop uses via the OS package; Android has none, so the
            // app bundles it). Load order matters: libc++_shared first,
            // then the empty pcaudio stub, then eSpeak, then the engine.
            System.loadLibrary("c++_shared")
            System.loadLibrary("pcaudio")
            System.loadLibrary("espeak-ng")
            System.loadLibrary("audiocpp")
            System.loadLibrary("kokoro_tts_jni")
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

    /** Install eSpeak-ng's data from the zipped asset. Self-healing:
     *  anything but a regular phontab FILE triggers a clean re-extract
     *  (an early recursive-assets build left empty DIRECTORIES named
     *  like the data files). */
    private fun extractEspeakData(context: Context, dir: File) {
        val dataDir = File(dir, "espeak-ng-data")
        if (File(dataDir, "phontab").isFile) return
        dataDir.deleteRecursively()
        dataDir.mkdirs()
        context.assets.open("models/tts/espeak-ng-data.zip").use { raw ->
            java.util.zip.ZipInputStream(raw).use { zip ->
                var entry = zip.nextEntry
                while (entry != null) {
                    val out = File(dataDir, entry.name)
                    if (entry.isDirectory) {
                        out.mkdirs()
                    } else {
                        out.parentFile?.mkdirs()
                        val tmp = File(out.path + ".tmp")
                        if (tmp.exists()) tmp.delete()
                        tmp.outputStream().use { zip.copyTo(it) }
                        if (!tmp.renameTo(out)) {
                            tmp.delete()
                            throw java.io.IOException(
                                "could not install ${entry.name}")
                        }
                    }
                    zip.closeEntry()
                    entry = zip.nextEntry
                }
            }
        }
    }

    /** Same tmp+rename discipline as SttEngine.extractModels: a crash
     *  mid-copy must not leave a truncated model every launch trusts. */
    private fun extractAssets(context: Context) {
        val dir = File(context.filesDir, "models/tts").apply { mkdirs() }
        // Drop the retired legacy TTS model + embeddings (128MB) left by
        // older installs — Kokoro voice packs live inside the GGUF.
        File(dir, "pocket-tts-english-q8_0.gguf").delete()
        File(dir, "embeddings").deleteRecursively()
        // eSpeak-ng's dictionaries + phoneme tables. audio.cpp finds
        // <model dir>/espeak-ng-data/phontab automatically.
        extractEspeakData(context, dir)

        val files = listOf(MODEL)
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
