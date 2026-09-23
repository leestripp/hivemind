package com.hivemind.voice

/** JNI bridge to whisper.cpp (libwhisper_jni.so). All calls are blocking —
 *  invoke from the SttEngine background thread only. Two model slots:
 *  WHICH_PARTIAL (cheap, live preview) and WHICH_FINAL (accurate, on SEND). */
object WhisperNative {
    init {
        System.loadLibrary("whisper_jni")
    }

    const val WHICH_PARTIAL = 0
    const val WHICH_FINAL = 1

    /** Load (or swap) the ggml model for a slot. Returns false on failure. */
    external fun nativeInit(modelPath: String, which: Int): Boolean

    /** Transcribe 16kHz mono PCM16. finalPass=false → partial slot, fast
     *  single-segment decode; true → final slot, full multi-segment. */
    external fun nativeTranscribe(
        pcm: ShortArray, nSamples: Int, nThreads: Int, finalPass: Boolean
    ): String

    external fun nativeFree()
}
