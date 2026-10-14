package com.hivemind.voice

import android.app.AlertDialog
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
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
        list.setOnItemClickListener { _, _, pos, _ ->
            val m = messages.getOrNull(pos) ?: return@setOnItemClickListener
            if (m.attachments.length() > 0)
                openAttachment(m.attachments.getJSONObject(0))
        }
    }

    private fun openAttachment(att: JSONObject) {
        val name = att.optString("name", "file")
        Log.i(TAG, "open attachment: $name")
        Thread {
            try {
                val cache = cacheFile(name)
                if (!cache.exists() || cache.length() == 0L) {
                    cache.parentFile?.mkdirs()
                    cache.writeBytes(api.downloadFile(att))
                }
                val ext = name.substringAfterLast('.', "").lowercase()
                ui.post {
                    when (ext) {
                        "mp4", "webm", "mov", "mkv", "3gp" -> playVideo(cache)
                        else -> showImage(cache)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "attachment download failed: ${e.message}")
            }
        }.start()
    }

    /** Fullscreen image viewer (tap anywhere to dismiss). */
    private fun showImage(file: File) {
        val iv = ImageView(activity).apply {
            setImageBitmap(BitmapFactory.decodeFile(file.absolutePath))
            adjustViewBounds = true
            scaleType = ImageView.ScaleType.FIT_CENTER
        }
        val dlg = AlertDialog.Builder(activity,
            android.R.style.Theme_Black_NoTitleBar_Fullscreen).setView(iv).create()
        iv.setOnClickListener { dlg.dismiss() }
        dlg.show()
    }

    private fun playVideo(file: File) {
        val vv = VideoView(activity)
        val dlg = AlertDialog.Builder(activity,
            android.R.style.Theme_Black_NoTitleBar_Fullscreen).setView(vv).create()
        vv.setVideoPath(file.absolutePath)
        val mc = MediaController(activity)
        mc.setAnchorView(vv)
        vv.setMediaController(mc)
        vv.setOnCompletionListener { dlg.dismiss() }
        dlg.setOnDismissListener { vv.stopPlayback() }
        dlg.show()
        vv.start()
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
            val row = (convert as? LinearLayout) ?: LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = android.widget.AbsListView.LayoutParams(
                    android.widget.AbsListView.LayoutParams.MATCH_PARENT,
                    android.widget.AbsListView.LayoutParams.WRAP_CONTENT)
            }
            row.removeAllViews()
            if (m.senderKind != "user" && m.senderKind != "agent") {
                row += centered(m.body, Color.parseColor("#7A8A99"))
                return row
            }
            val isUser = m.senderKind == "user"
            row.gravity = if (isUser) Gravity.END else Gravity.START
            val who = if (isUser) "You" else agentName
            row += centered("$who · ${shortTime(m.createdAt)}", Color.parseColor("#7A8A99"), 11f)
            if (m.body.isNotBlank()) row += bubble(m.body, isUser)
            for (i in 0 until m.attachments.length())
                row += attachmentView(m.attachments.getJSONObject(i), isUser)
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
                "mp4", "webm", "mov", "mkv", "3gp" -> videoChip(att, name, isUser)
                else -> fileChip(att, name, isUser)
            }
        }

        private fun imageView(att: JSONObject, name: String, isUser: Boolean): ImageView {
            val iv = ImageView(activity).apply {
                adjustViewBounds = true
                val max = (activity.resources.displayMetrics.widthPixels * 0.72f).toInt()
                layoutParams = LinearLayout.LayoutParams(max, LinearLayout.LayoutParams.WRAP_CONTENT)
                    .apply { gravity = if (isUser) Gravity.END else Gravity.START }
                scaleType = ImageView.ScaleType.FIT_CENTER
            }
            val cache = cacheFile(name)
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

        private fun videoChip(att: JSONObject, name: String, isUser: Boolean): View =
            chip("▶ $name", isUser)

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

    private fun cacheFile(name: String): File {
        val safe = name.replace(Regex("[^A-Za-z0-9._-]"), "_")
        return File(activity.cacheDir, "attachments/$threadId/$safe")
    }

    private operator fun LinearLayout.plusAssign(v: View) { addView(v) }

    companion object { private const val TAG = "HiveChat" }
}
