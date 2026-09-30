package com.hivemind.voice

/** JNI bridge to the C++ OpenGL ES sphere renderer (libsphere.so). */
object SphereNative {
    init {
        System.loadLibrary("sphere")
    }

    external fun nativeInit()
    external fun nativeResize(width: Int, height: Int)
    external fun nativeRender(timeSec: Float, level: Float, state: Float)
}
