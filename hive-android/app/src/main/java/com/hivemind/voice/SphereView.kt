package com.hivemind.voice

import android.content.Context
import android.opengl.GLSurfaceView
import android.util.AttributeSet
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.abs
import kotlin.math.sin

/** Visual states for the sphere — mirror the shader's uState values. */
enum class SphereState(val v: Float) {
    IDLE(0f), LISTENING(1f), THINKING(2f), SPEAKING(3f), PROCESSING(4f)
}

class SphereView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : GLSurfaceView(context, attrs) {

    private val renderer = SphereRenderer()

    init {
        setEGLContextClientVersion(2)
        preserveEGLContextOnPause = true
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    override fun onPause() {
        // Free the GL objects while the (possibly preserved) context is
        // still current — otherwise every pause/resume cycle orphans a
        // program + VBO inside it. onSurfaceCreated rebuilds on return.
        queueEvent { SphereNative.nativeDestroy() }
        super.onPause()
    }

    /** Instantaneous voice level 0..1 (mic RMS or TTS envelope). */
    fun setLevel(level: Float) = renderer.setLevel(level)

    fun setSphereState(state: SphereState) = renderer.setState(state.v)

    private class SphereRenderer : Renderer {
        @Volatile private var targetLevel = 0f
        @Volatile private var level = 0f
        @Volatile private var state = SphereState.IDLE.v
        @Volatile private var stateTarget = SphereState.IDLE.v
        private var startNanos = 0L

        fun setLevel(l: Float) { targetLevel = l.coerceIn(0f, 1f) }
        fun setState(s: Float) { stateTarget = s }

        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            SphereNative.nativeInit()
            startNanos = System.nanoTime()
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            SphereNative.nativeResize(width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            val t = (System.nanoTime() - startNanos) / 1_000_000_000f
            // Smooth level + state so the sphere eases instead of snapping.
            level += (targetLevel - level) * 0.25f
            state += (stateTarget - state) * 0.08f
            if (abs(stateTarget - state) < 0.01f) state = stateTarget
            // Idle breathing so the sphere never looks dead.
            val breathing = if (targetLevel < 0.02f)
                0.06f + 0.05f * sin(t * 1.7f)
            else 0f
            SphereNative.nativeRender(t, (level + breathing).coerceIn(0f, 1f), state)
        }
    }
}
