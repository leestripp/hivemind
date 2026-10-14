package com.hivemind.voice

import android.app.Activity
import android.graphics.Color
import android.media.MediaPlayer
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import android.opengl.GLES20

/** Fullscreen audio player for music attachments (lee 2026-10-14: "proper
 *  audio players, Large Play/pause for fat fingers, OpenGL ES waveform
 *  visualizer"). Lives in the media overlay like TexturePlayer; BACK /
 *  closeMedia() releases it.
 *
 *  Visuals: android.media.audiofx.Visualizer taps the MediaPlayer's audio
 *  session for REAL waveform samples, rendered as 48 gradient bars by a
 *  tiny GL ES 2.0 renderer. If the device refuses the Visualizer (some
 *  do), a pseudo-animation runs while playing — never a dead screen. */
class AudioPlayerView(private val activity: Activity, private val onBack: () -> Unit) :
    LinearLayout(activity) {

    private var mp: MediaPlayer? = null
    private var viz: android.media.audiofx.Visualizer? = null
    private val renderer = WaveRenderer()
    private val gl: GLSurfaceView
    private val seek: SeekBar
    private val elapsed: TextView
    private val total: TextView
    private val playBtn: android.widget.Button
    private val handler = Handler(Looper.getMainLooper())
    private var seeking = false
    private var released = false

    init {
        orientation = VERTICAL
        setBackgroundColor(Color.parseColor("#0A0E14"))
        val dp = resources.displayMetrics.density

        // header: back arrow + song title (matches the chat app bar)
        addView(LinearLayout(activity).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding((8 * dp).toInt(), 0, (8 * dp).toInt(), 0)
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, (56 * dp).toInt())
            addView(ImageButton(activity).apply {
                setImageResource(R.drawable.ic_arrow_back)
                background = null
                contentDescription = "Close player"
                layoutParams = LayoutParams((48 * dp).toInt(), (48 * dp).toInt())
                setOnClickListener { onBack() }
            })
            addView(TextView(activity).apply {
                id = TITLE_ID
                setTextColor(Color.parseColor("#DCE8F0"))
                textSize = 18f
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
                layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
                    .apply { marginStart = (8 * dp).toInt() }
            })
        })

        gl = GLSurfaceView(activity).apply {
            setEGLContextClientVersion(2)
            setRenderer(renderer)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
        addView(gl, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))

        // seek row: elapsed | bar | total
        elapsed = timeLabel()
        total = timeLabel()
        seek = SeekBar(activity).apply {
            max = 1000
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                    if (fromUser) mp?.let {
                        val ms = p * it.duration / 1000
                        elapsed.text = fmt(ms)
                    }
                }
                override fun onStartTrackingTouch(s: SeekBar) { seeking = true }
                override fun onStopTrackingTouch(s: SeekBar) {
                    mp?.seekTo(s.progress * mp!!.duration / 1000)
                    seeking = false
                }
            })
        }
        addView(LinearLayout(activity).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            val p = (16 * dp).toInt()
            setPadding(p, 0, p, 0)
            addView(elapsed); addView(seek); addView(total)
        })

        // the fat-finger play/pause: 112dp circle, same style as TALK
        playBtn = android.widget.Button(activity).apply {
            text = "⏸"
            textSize = 40f
            setTextColor(Color.parseColor("#33D6E8"))
            setBackgroundResource(R.drawable.btn_talk)
            val sz = (112 * dp).toInt()
            layoutParams = LayoutParams(sz, sz).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                bottomMargin = (32 * dp).toInt()
                topMargin = (8 * dp).toInt()
            }
            setOnClickListener { toggle() }
        }
        addView(playBtn)
    }

    private fun timeLabel() = TextView(activity).apply {
        text = "0:00"
        setTextColor(Color.parseColor("#8FA8B8"))
        textSize = 14f
        setPadding((8 * resources.displayMetrics.density).toInt(), 0,
                   (8 * resources.displayMetrics.density).toInt(), 0)
    }

    /** Load + autoplay. Call once, on the UI thread. */
    fun open(file: File, name: String) {
        findViewById<TextView>(TITLE_ID).text = name
        val player = MediaPlayer()
        try {
            player.setDataSource(file.absolutePath)
            player.setOnCompletionListener { onEnded() }
            player.prepare()
            mp = player
            total.text = fmt(player.duration)
            player.start()
            renderer.active = true
            attachVisualizer(player.audioSessionId)
            tick()
        } catch (e: Exception) {
            Log.w(TAG, "audio open failed: ${e.message}")
            player.release()
            onBack()
        }
    }

    /** Visualizer needs RECORD_AUDIO (already granted for voice). Session-
     *  scoped capture: no MODIFY_AUDIO_SETTINGS needed. */
    private fun attachVisualizer(sessionId: Int) {
        try {
            viz = android.media.audiofx.Visualizer(sessionId).apply {
                captureSize = android.media.audiofx.Visualizer.getCaptureSizeRange()[1]
                    .coerceAtMost(1024)
                setDataCaptureListener(object :
                    android.media.audiofx.Visualizer.OnDataCaptureListener {
                    override fun onWaveFormDataCapture(
                        v: android.media.audiofx.Visualizer?,
                        bytes: ByteArray, rate: Int) {
                        renderer.wave = bytes.copyOf()
                    }
                    override fun onFftDataCapture(
                        v: android.media.audiofx.Visualizer?,
                        fft: ByteArray, rate: Int) {}
                }, android.media.audiofx.Visualizer.getMaxCaptureRate(), true, false)
                enabled = true
            }
        } catch (e: Exception) {
            Log.w(TAG, "visualizer unavailable, pseudo-wave fallback: ${e.message}")
            viz = null
        }
    }

    private fun toggle() {
        val p = mp ?: return
        if (p.isPlaying) {
            p.pause()
            renderer.active = false
            playBtn.text = "▶"
        } else {
            p.start()
            renderer.active = true
            playBtn.text = "⏸"
            tick()
        }
    }

    private fun onEnded() {
        playBtn.text = "▶"
        renderer.active = false
        renderer.wave = null
    }

    private val ticker = object : Runnable {
        override fun run() {
            val p = mp
            if (p != null && p.isPlaying && !seeking) {
                seek.progress = p.currentPosition * 1000 / p.duration.coerceAtLeast(1)
                elapsed.text = fmt(p.currentPosition)
                handler.postDelayed(this, 250)
            }
        }
    }
    private fun tick() { handler.post(ticker) }

    /** Idempotent teardown — closeMedia + onBack both land here. */
    fun release() {
        if (released) return
        released = true
        handler.removeCallbacks(ticker)
        try { viz?.enabled = false; viz?.release() } catch (_: Exception) {}
        viz = null
        try { mp?.stop(); mp?.release() } catch (_: Exception) {}
        mp = null
        renderer.active = false
        gl.onPause()
    }

    private fun fmt(ms: Int) = "%d:%02d".format(ms / 60000, (ms / 1000) % 60)

    private class WaveRenderer : GLSurfaceView.Renderer {
        private val n = 48
        private val bars = FloatArray(n)
        private val target = FloatArray(n)
        @Volatile var wave: ByteArray? = null
        @Volatile var active = false
        private var t = 0f
        private var prog = 0
        private var aPos = 0
        private var aColor = 0
        private var vb: java.nio.FloatBuffer? = null

        override fun onSurfaceCreated(unused: GL10?, config: EGLConfig?) {
            GLES20.glClearColor(0.039f, 0.055f, 0.078f, 1f)   // #0A0E14
            val vs = compile(GLES20.GL_VERTEX_SHADER,
                "attribute vec2 aPos; attribute vec4 aColor; varying vec4 vColor;" +
                "void main(){ vColor=aColor; gl_Position=vec4(aPos,0.,1.); }")
            val fs = compile(GLES20.GL_FRAGMENT_SHADER,
                "precision mediump float; varying vec4 vColor;" +
                "void main(){ gl_FragColor=vColor; }")
            prog = GLES20.glCreateProgram().also {
                GLES20.glAttachShader(it, vs); GLES20.glAttachShader(it, fs)
                GLES20.glLinkProgram(it)
            }
            aPos = GLES20.glGetAttribLocation(prog, "aPos")
            aColor = GLES20.glGetAttribLocation(prog, "aColor")
            vb = ByteBuffer.allocateDirect(n * 6 * 6 * 4)
                .order(ByteOrder.nativeOrder()).asFloatBuffer()
        }

        override fun onSurfaceChanged(unused: GL10?, w: Int, h: Int) {
            GLES20.glViewport(0, 0, w, h)
        }

        override fun onDrawFrame(unused: GL10?) {
            t += 0.016f
            val w = wave
            if (w != null && w.isNotEmpty()) {
                // waveform bytes are UNSIGNED 8-bit centred on 128
                val bucket = w.size / n
                for (i in 0 until n) {
                    var m = 0
                    val base = i * bucket
                    for (j in 0 until bucket) {
                        val v = kotlin.math.abs((w[base + j].toInt() and 0xFF) - 128)
                        if (v > m) m = v
                    }
                    target[i] = (m / 96f).coerceIn(0.02f, 1f)   // 96: boost quiet
                }
            } else if (active) {
                // fallback: layered sines while playing (no Visualizer)
                for (i in 0 until n)
                    target[i] = ((kotlin.math.sin(t * 2.1 + i * 0.55) +
                        kotlin.math.sin(t * 3.3 + i * 0.31)) * 0.25 + 0.5).toFloat() * 0.55f
            } else {
                for (i in 0 until n) target[i] = 0.03f
            }
            val buf = vb ?: return
            buf.clear()
            for (i in 0 until n) {
                bars[i] += (target[i] - bars[i]) * 0.25f
                val h = 0.04f + bars[i] * 1.5f
                val cx = -1f + (i + 0.5f) / n * 2f
                val bw = 0.6f / n
                // cyan gradient: dim at base, bright at top
                vert(buf, cx - bw, -0.8f, 0.03f, 0.20f, 0.24f)
                vert(buf, cx + bw, -0.8f, 0.03f, 0.20f, 0.24f)
                vert(buf, cx + bw, -0.8f + h, 0.20f, 0.84f, 0.91f)
                vert(buf, cx - bw, -0.8f, 0.03f, 0.20f, 0.24f)
                vert(buf, cx + bw, -0.8f + h, 0.20f, 0.84f, 0.91f)
                vert(buf, cx - bw, -0.8f + h, 0.20f, 0.84f, 0.91f)
            }
            buf.flip()
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glUseProgram(prog)
            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glEnableVertexAttribArray(aColor)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 24, buf)
            buf.position(2)
            GLES20.glVertexAttribPointer(aColor, 4, GLES20.GL_FLOAT, false, 24, buf)
            buf.position(0)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, n * 6)
            GLES20.glDisableVertexAttribArray(aPos)
            GLES20.glDisableVertexAttribArray(aColor)
        }

        private fun vert(buf: java.nio.FloatBuffer, x: Float, y: Float,
                         r: Float, g: Float, b: Float) {
            buf.put(x); buf.put(y); buf.put(r); buf.put(g); buf.put(b); buf.put(1f)
        }

        private fun compile(type: Int, src: String): Int {
            val sh = GLES20.glCreateShader(type)
            GLES20.glShaderSource(sh, src)
            GLES20.glCompileShader(sh)
            return sh
        }
    }

    companion object {
        private const val TAG = "HiveChat"
        private val TITLE_ID = View.generateViewId()
    }
}
