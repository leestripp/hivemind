package com.hivemind.voice

import android.content.Context
import android.graphics.Matrix
import android.graphics.RectF
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.widget.ImageView

/**
 * Pinch-to-zoom + drag-pan image view for the fullscreen attachment
 * viewer (lee, 2026-10-14: "images in fullscreen should support pinch
 * to zoom"). Matrix-based: pinch scales around the focal point, drag
 * pans when zoomed in, double-tap toggles 1x/3x. Scale clamped to
 * [fit, 6x]; pan clamped so the image can't be dragged off-screen.
 */
class ZoomImageView @JvmOverloads constructor(
    ctx: Context, attrs: AttributeSet? = null,
) : ImageView(ctx, attrs) {

    private val mat = Matrix()
    private val base = Matrix()          // fit-to-view matrix
    private var scale = 1f
    private var ready = false

    private val scaler = ScaleGestureDetector(ctx,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                val next = (scale * d.scaleFactor).coerceIn(1f, 6f)
                val f = next / scale
                scale = next
                mat.postScale(f, f, d.focusX, d.focusY)
                clampPan()
                apply()
                return true
            }
        })

    private val taps = GestureDetector(ctx,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                val f = if (scale > 1.5f) 1f / scale else 3f / scale
                scale *= f
                mat.postScale(f, f, e.x, e.y)
                clampPan()
                apply()
                return true
            }
        })

    private var lastX = 0f
    private var lastY = 0f
    private var dragging = false

    init {
        scaleType = ScaleType.MATRIX
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        super.onLayout(changed, l, t, r, b)
        resetFit()
    }

    override fun setImageBitmap(bm: android.graphics.Bitmap?) {
        super.setImageBitmap(bm)
        resetFit()
    }

    /** Fit image inside the view, centered. */
    private fun resetFit() {
        val d = drawable ?: return
        if (width == 0 || height == 0) return
        val vw = width.toFloat(); val vh = height.toFloat()
        val iw = d.intrinsicWidth.toFloat(); val ih = d.intrinsicHeight.toFloat()
        if (iw <= 0 || ih <= 0) return
        val fit = minOf(vw / iw, vh / ih)
        base.reset()
        base.setScale(fit, fit)
        base.postTranslate((vw - iw * fit) / 2f, (vh - ih * fit) / 2f)
        mat.set(base)
        scale = 1f
        ready = true
        apply()
    }

    override fun onTouchEvent(ev: MotionEvent): Boolean {
        if (!ready) return super.onTouchEvent(ev)
        scaler.onTouchEvent(ev)
        taps.onTouchEvent(ev)
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastX = ev.x; lastY = ev.y; dragging = true
            }
            MotionEvent.ACTION_MOVE -> if (dragging && !scaler.isInProgress) {
                mat.postTranslate(ev.x - lastX, ev.y - lastY)
                clampPan()
                apply()
                lastX = ev.x; lastY = ev.y
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> dragging = false
        }
        return true
    }

    /** Keep the image covering its fitted footprint — no panning past edges. */
    private fun clampPan() {
        val d = drawable ?: return
        val r = RectF(0f, 0f, d.intrinsicWidth.toFloat(), d.intrinsicHeight.toFloat())
        val m = Matrix(base); m.postConcat(mat)
        m.mapRect(r)
        val vw = width.toFloat(); val vh = height.toFloat()
        var dx = 0f; var dy = 0f
        if (r.width() <= vw) dx = (vw - r.width()) / 2f - r.left
        else if (r.left > 0) dx = -r.left
        else if (r.right < vw) dx = vw - r.right
        if (r.height() <= vh) dy = (vh - r.height()) / 2f - r.top
        else if (r.top > 0) dy = -r.top
        else if (r.bottom < vh) dy = vh - r.bottom
        if (dx != 0f || dy != 0f) mat.postTranslate(dx, dy)
    }

    private fun apply() {
        val m = Matrix(base); m.postConcat(mat)
        imageMatrix = m
    }
}
