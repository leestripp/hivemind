package com.hivemind.voice

import android.content.Context
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.view.Surface
import android.view.TextureView
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.MediaController

/**
 * TextureView-backed fullscreen video player (2026-10-14). VideoView's
 * SurfaceView punches its own layer through the window: it ghosted frame
 * counters at the screen's top-left after close, mispositioned vs its
 * view bounds, and measured MATCH_PARENT as content-height. TextureView
 * renders in the normal view hierarchy — none of those bugs exist here.
 * Fit-center letterboxing is done with a texture transform matrix.
 */
class TexturePlayer(ctx: Context) : FrameLayout(ctx),
    TextureView.SurfaceTextureListener, MediaController.MediaPlayerControl {

    private val tv = TextureView(ctx)
    private var mp: MediaPlayer? = null
    private var path: String? = null
    var onDone: (() -> Unit)? = null
    // NOTE: no BACK handling here — VoiceActivity.onBackPressed owns it
    // (closeMedia). A view-level dispatchKeyEvent override double-fired
    // with the activity dispatcher: one BACK closed the player AND exited
    // chat mode (2026-10-14).

    init {
        addView(tv, LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT))
        tv.surfaceTextureListener = this
    }

    fun play(p: String) {
        path = p
        if (tv.isAvailable) startPlayer()
    }

    private fun startPlayer() {
        val p = path ?: return
        release()
        mp = MediaPlayer().apply {
            setDataSource(p)
            setSurface(Surface(tv.surfaceTexture))
            setOnPreparedListener { m ->
                fit(m.videoWidth, m.videoHeight)
                m.start()
            }
            setOnCompletionListener { onDone?.invoke() }
            prepareAsync()
        }
    }

    /** Letterbox the video centered in the view (fit-center). */
    private fun fit(iw: Int, ih: Int) {
        if (iw <= 0 || ih <= 0 || width == 0 || height == 0) return
        val fitScale = minOf(width.toFloat() / iw, height.toFloat() / ih)
        val sx = iw * fitScale / width
        val sy = ih * fitScale / height
        val m = Matrix()
        m.setScale(sx, sy, width / 2f, height / 2f)
        tv.setTransform(m)
    }

    fun release() {
        mp?.let { runCatching { it.stop() }; it.release() }
        mp = null
    }

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        if (path != null && mp == null) startPlayer()
    }
    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        release()
        return true
    }
    override fun onDetachedFromWindow() {
        release()
        super.onDetachedFromWindow()
    }
    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    // MediaController.MediaPlayerControl
    override fun start() { mp?.start() }
    override fun pause() { mp?.pause() }
    override fun getDuration() = mp?.duration ?: 0
    override fun getCurrentPosition() = mp?.currentPosition ?: 0
    override fun seekTo(pos: Int) { mp?.seekTo(pos) }
    override fun isPlaying() = mp?.isPlaying == true
    override fun getBufferPercentage() = 100
    override fun canPause() = true
    override fun canSeekBackward() = true
    override fun canSeekForward() = true
    override fun getAudioSessionId() = mp?.audioSessionId ?: 0
}
