package com.hivemind.voice

import android.app.AlertDialog
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.MediaController
import android.widget.TextView
import android.widget.VideoView
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Full chat panel behind the bottom-bar chat icon (2026-10-14): whole
 * thread history with the attachments agents share (images inline,
 * videos tap-to-play, other files as name chips) — the hive-client
 * desktop feature, for on-the-go reading. Attachment contract mirrors
 * HiveWindow: {name, caption, path:"/workspace/<rel>", worker}, bytes
 * via /api/files/download, cached under cacheDir/attachments/.
 */
class ChatPanel(
    private val activity: VoiceActivity,
    private val api: ApiClient,
    private val threadId: String,
    private val agentName: String,
    private val list: ListView,
) {
    private val ui = Handler(Looper.getMainLooper())
    private val messages = mutableListOf<ApiClient.Message>()
    private val adapter = ChatAdapter()
    @Volatile private var refreshing = false

    init {
        list.adapter = adapter
        // Row-level tap is the contract (ListView child-click delivery is
        // unreliable — observed dead chips 2026-10-14): tapping a message
        // opens its first attachment fullscreen (image viewer / video
        // player). Chips stay visual affordances, not click targets.
        // Row-level tap is the contract (ListView child-click delivery is
        // unreliable — observed dead chips 2026-10-14). The row records the
        // tap's y so a multi-attachment message opens the attachment that
        // was actually tapped, not always the first (lee 2026-10-14: "tap
        // the second video, only the first plays").
        list.setOnItemClickListener { _, view, pos, _ ->
            val m = messages.getOrNull(pos) ?: return@setOnItemClickListener
            if (m.attachments.length() == 0) return@setOnItemClickListener
            val row = view as? AttRow
            val tapped = row?.attAt(row.lastDownY)
            openAttachment(tapped ?: m.attachments.getJSONObject(0))
        }
    }

    /** Message row that remembers where the tap landed and which view
     *  renders which attachment. */
    inner class AttRow(ctx: android.content.Context) : LinearLayout(ctx) {
        var lastDownY = Float.MAX_VALUE
        val attViews = mutableListOf<Pair<View, JSONObject>>()

        override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
            if (ev.actionMasked == MotionEvent.ACTION_DOWN) lastDownY = ev.y
            return super.onInterceptTouchEvent(ev)
        }
        override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
            if (ev.actionMasked == MotionEvent.ACTION_DOWN) lastDownY = ev.y
            return super.dispatchTouchEvent(ev)
        }

        fun attAt(y: Float): JSONObject? {
            for ((v, att) in attViews)
                if (y >= v.top && y <= v.bottom) return att
            return null
        }
    }

    /** Attachment opens in flight — a double-tap during a long download
     *  must not spawn two players (the second would orphan the first). */
    private val opening =
        java.util.Collections.synchronizedSet(mutableSetOf<String>())

    private fun openAttachment(att: JSONObject) {
        val name = att.optString("name", "file")
        val key = att.optString("path", name)
        if (!opening.add(key)) {
            Log.i(TAG, "open already in flight, ignored: $name")
            return
        }
        Log.i(TAG, "open attachment: $name")
        Thread {
            try {
                val cache = cacheFile(att)
                if (!cache.exists() || cache.length() == 0L) {
                    cache.parentFile?.mkdirs()
                    cache.writeBytes(api.downloadFile(att))
                }
                val ext = name.substringAfterLast('.', "").lowercase()
                ui.post {
                    opening.remove(key)
                    when (ext) {
                        "mp4", "webm", "mov", "mkv", "3gp" -> playVideo(cache)
                        "flac", "mp3", "wav", "ogg", "m4a", "aac", "opus" -> playAudio(cache, name)
                        else -> showImage(cache)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "attachment download failed: ${e.message}")
                ui.post { opening.remove(key) }
            }
        }.start()
    }

    /** Reload full history (bg). Safe to call from the poll loop on
     *  every new message — coalesced by [refreshing]. */
    fun refresh() {
        if (refreshing) return
        refreshing = true
        Thread {
            try {
                val hist = api.history(threadId)
                ui.post {
                    messages.clear()
                    messages.addAll(hist)
                    adapter.notifyDataSetChanged()
                    if (hist.isNotEmpty()) list.setSelection(hist.size - 1)
                }
            } catch (e: Exception) {
                Log.w(TAG, "history failed: ${e.message}")
            } finally {
                refreshing = false
            }
        }.start()
    }

    private inner class ChatAdapter : BaseAdapter() {
        override fun getCount() = messages.size
        override fun getItem(pos: Int) = messages[pos]
        override fun getItemId(pos: Int) = pos.toLong()

        override fun getView(pos: Int, convert: View?, parent: ViewGroup): View {
            val m = messages[pos]
            val row = (convert as? AttRow) ?: AttRow(activity).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = android.widget.AbsListView.LayoutParams(
                    android.widget.AbsListView.LayoutParams.MATCH_PARENT,
                    android.widget.AbsListView.LayoutParams.WRAP_CONTENT)
            }
            row.removeAllViews()
            row.attViews.clear()
            row.lastDownY = Float.MAX_VALUE
            if (m.senderKind != "user" && m.senderKind != "agent") {
                row.addView(centered(m.body, Color.parseColor("#7A8A99")))
                return row
            }
            val isUser = m.senderKind == "user"
            row.gravity = if (isUser) Gravity.END else Gravity.START
            val who = if (isUser) "You" else agentName
            row.addView(centered("$who · ${shortTime(m.createdAt)}",
                Color.parseColor("#7A8A99"), 11f))
            if (m.body.isNotBlank()) row.addView(bubble(m.body, isUser))
            for (i in 0 until m.attachments.length()) {
                val att = m.attachments.getJSONObject(i)
                val av = attachmentView(att, isUser)
                row.attViews += av to att
                row.addView(av)
            }
            return row
        }

        private fun bubble(text: String, isUser: Boolean): TextView {
            val bg = GradientDrawable().apply {
                cornerRadius = 18 * activity.resources.displayMetrics.density
                setColor(Color.parseColor(if (isUser) "#12303A" else "#101820"))
            }
            return TextView(activity).apply {
                this.text = text
                setTextColor(Color.parseColor("#DCE8F0"))
                textSize = 16f
                background = bg
                val p = (12 * activity.resources.displayMetrics.density).toInt()
                setPadding(p + 4, p, p + 4, p)
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    val m8 = (8 * activity.resources.displayMetrics.density).toInt()
                    setMargins(m8, 2, m8, 2)
                    gravity = if (isUser) Gravity.END else Gravity.START
                }
            }
        }

        private fun centered(text: String, color: Int, sp: Float = 12f): TextView =
            TextView(activity).apply {
                this.text = text
                setTextColor(color)
                textSize = sp
                gravity = Gravity.CENTER_HORIZONTAL
            }

        private fun attachmentView(att: JSONObject, isUser: Boolean): View {
            val name = att.optString("name", "file")
            val ext = name.substringAfterLast('.', "").lowercase()
            return when (ext) {
                "jpg", "jpeg", "png", "webp", "gif", "bmp" -> imageView(att, name, isUser)
                "mp4", "webm", "mov", "mkv", "3gp" -> inlineVideo(att, name, isUser)
                "flac", "mp3", "wav", "ogg", "m4a", "aac", "opus" -> chip("🎵 $name", isUser)
                else -> fileChip(att, name, isUser)
            }
        }

        private fun imageView(att: JSONObject, name: String, isUser: Boolean): ImageView {
            val iv = ImageView(activity).apply {
                adjustViewBounds = true
                val max = (activity.resources.displayMetrics.widthPixels * 0.72f).toInt()
                layoutParams = LinearLayout.LayoutParams(max, LinearLayout.LayoutParams.WRAP_CONTENT)
                    .apply { gravity = Gravity.CENTER_HORIZONTAL }   // lee: center media
                scaleType = ImageView.ScaleType.FIT_CENTER
            }
            val cache = cacheFile(att)
            if (cache.exists() && cache.length() > 0) {
                iv.setImageBitmap(BitmapFactory.decodeFile(cache.absolutePath))
            } else {
                Thread {
                    try {
                        val bytes = api.downloadFile(att)
                        cache.parentFile?.mkdirs()
                        cache.writeBytes(bytes)
                        val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                        ui.post { iv.setImageBitmap(bmp) }
                    } catch (e: Exception) {
                        Log.w(TAG, "image download failed: ${e.message}")
                    }
                }.start()
            }
            return iv
        }

        /** Embedded video preview = thumbnail bitmap + ▶ overlay
         *  (lee 2026-10-14: inline VideoViews showed black boxes —
         *  seekTo(1) lands on the fade-from-black frame; a thumbnail
         *  grabbed 1s in always has content, and a BitmapView recycles
         *  cleanly in the ListView where a SurfaceView doesn't). Tap the
         *  row for the fullscreen player. */
        private fun inlineVideo(att: JSONObject, name: String, isUser: Boolean): View {
            val max = (activity.resources.displayMetrics.widthPixels * 0.72f).toInt()
            val h = (max * 9f / 16f).toInt()
            val density = activity.resources.displayMetrics.density
            val frame = android.widget.FrameLayout(activity).apply {
                layoutParams = LinearLayout.LayoutParams(max, h)
                    .apply { gravity = Gravity.CENTER_HORIZONTAL }   // lee: center media
                setBackgroundColor(Color.parseColor("#0A0E14"))
            }
            val iv = ImageView(activity).apply {
                layoutParams = android.widget.FrameLayout.LayoutParams(max, h)
                scaleType = ImageView.ScaleType.CENTER_CROP
            }
            val play = TextView(activity).apply {
                text = "▶"
                textSize = 28f
                setTextColor(Color.WHITE)
                val r = (26 * density).toInt()
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(Color.parseColor("#80000000"))
                }
                gravity = Gravity.CENTER
                layoutParams = android.widget.FrameLayout.LayoutParams(r * 2, r * 2)
                    .apply { gravity = Gravity.CENTER }
            }
            frame.addView(iv)
            frame.addView(play)

            val thumb = File(cacheFile(att).absolutePath + ".thumb.png")
            if (thumb.exists() && thumb.length() > 0) {
                iv.setImageBitmap(BitmapFactory.decodeFile(thumb.absolutePath))
            } else {
                Thread {
                    try {
                        val cache = cacheFile(att)
                        if (!cache.exists() || cache.length() == 0L) {
                            cache.parentFile?.mkdirs()
                            cache.writeBytes(api.downloadFile(att))
                        }
                        val mmr = android.media.MediaMetadataRetriever()
                        try {
                            mmr.setDataSource(cache.absolutePath)
                            val bmp = mmr.getFrameAtTime(1_000_000)   // 1s in — past fades
                                ?: mmr.getFrameAtTime(0)
                            if (bmp != null) {
                                java.io.FileOutputStream(thumb).use { out ->
                                    bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 90, out)
                                }
                                ui.post { iv.setImageBitmap(bmp) }
                            }
                        } finally {
                            mmr.release()
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "video thumb failed: ${e.message}")
                    }
                }.start()
            }
            return frame
        }

        private fun fileChip(att: JSONObject, name: String, isUser: Boolean): View =
            chip("📎 $name", isUser)

        private fun chip(text: String, isUser: Boolean): TextView {
            val bg = GradientDrawable().apply {
                cornerRadius = 14 * activity.resources.displayMetrics.density
                setColor(Color.parseColor(if (isUser) "#12303A" else "#101820"))
            }
            return TextView(activity).apply {
                this.text = text
                setTextColor(Color.parseColor("#33D6E8"))
                textSize = 14f
                background = bg
                val p = (10 * activity.resources.displayMetrics.density).toInt()
                setPadding(p + 6, p, p + 6, p)
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply { gravity = if (isUser) Gravity.END else Gravity.START }
            }
        }

        private fun shortTime(iso: String): String {
            // Hub stamps are ISO-8601 UTC; show local HH:mm.
            if (iso.length < 16) return iso
            return try {
                val utc = SimpleDateFormat("yyyy-MM-dd'T'HH:mm", Locale.US)
                    .apply { timeZone = java.util.TimeZone.getTimeZone("UTC") }
                SimpleDateFormat("HH:mm", Locale.getDefault())
                    .format(utc.parse(iso.substring(0, 16)) ?: Date())
            } catch (_: Exception) { iso.takeLast(5) }
        }
    }

    /** Fullscreen media lives in the activity's mediaOverlay FrameLayout
     *  (2026-10-14: dialogs stayed floating top-third windows on this
     *  device despite setLayout — an in-activity view has no window
     *  manager between us and the screen). Returns false when nothing
     *  was open so BACK can fall through. */
    fun closeMedia(): Boolean {
        if (overlay.childCount == 0) return false
        activeController?.hide()      // a shown controller eats BACK presses
        activeController = null
        releaseOverlayChild()
        overlay.removeAllViews()
        overlay.visibility = View.GONE
        return true
    }

    /** Release whatever media view the overlay currently holds (if any).
     *  MUST run before any removeAllViews that swaps media views — a
     *  detached-but-unreleased AudioPlayerView/TexturePlayer keeps its
     *  MediaPlayer alive with no way to reach it (lee 2026-10-14: "audio
     *  playing with no way to stop it"). */
    private fun releaseOverlayChild() {
        if (overlay.childCount == 0) return
        when (val v = overlay.getChildAt(0)) {
            is TexturePlayer -> v.release()
            is AudioPlayerView -> v.release()
        }
    }

    /** Audio attachments open the fullscreen GL player (lee 2026-10-14:
     *  "proper audio players, big play/pause, visualizer"). The chip tap
     *  downloads as usual, then the overlay player autoplays. */
    private fun playAudio(cache: java.io.File, name: String) {
        val av = AudioPlayerView(activity) { closeMedia() }
        showMedia(av)
        av.open(cache, name)
    }

    private var activeController: MediaController? = null

    private val overlay: android.widget.FrameLayout
        get() = activity.findViewById(R.id.mediaOverlay)

    private fun showMedia(v: View) {
        releaseOverlayChild()         // never orphan a playing media view
        overlay.removeAllViews()
        overlay.addView(v, android.widget.FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT))
        overlay.visibility = View.VISIBLE
    }

    /** Fullscreen image viewer: pinch-zoom / pan / double-tap via
     *  ZoomImageView; long-press dismisses (a plain tap is zoom input). */
    private fun showImage(file: File) {
        val iv = ZoomImageView(activity)
        iv.setOnLongClickListener { closeMedia(); true }
        iv.setImageBitmap(BitmapFactory.decodeFile(file.absolutePath))
        showMedia(iv)
    }

    private fun playVideo(file: File) {
        val tp = TexturePlayer(activity)   // TextureView — no SurfaceView ghosts
        tp.onDone = { closeMedia() }
        val mc = MediaController(activity)
        mc.setAnchorView(tp)
        mc.setMediaPlayer(tp)
        activeController = mc
        // Controller on a TAP only (lee 2026-10-14: showing it on any touch
        // meant every back-swipe's edge-down summoned the controller, which
        // then ate the BACK itself — video never closed until it ended).
        val taps = android.view.GestureDetector(activity,
            object : android.view.GestureDetector.SimpleOnGestureListener() {
                override fun onSingleTapUp(e: MotionEvent): Boolean {
                    mc.show(); return true
                }
            })
        tp.setOnTouchListener { _, ev -> taps.onTouchEvent(ev) }
        showMedia(tp)
        tp.play(file.absolutePath)
    }

    /** Cache file for an attachment. Keyed by name + a short hash of the
     *  hub path: two different attachments can share a display name
     *  (lee 2026-10-14: second video "played the first" — name-keyed
     *  cache resolved both to the first download). */
    private fun cacheFile(att: JSONObject): File {
        val name = att.optString("name", "file")
        val safe = name.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val key = att.optString("path", "").hashCode().toString(16)
        return File(activity.cacheDir, "attachments/$threadId/$key-$safe")
    }

    private operator fun LinearLayout.plusAssign(v: View) { addView(v) }

    companion object { private const val TAG = "HiveChat" }
}
