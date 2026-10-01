package com.hivemind.voice

import android.app.Activity
import android.app.AlertDialog
import android.app.Notification
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Reads notifications from other apps aloud via TTS — SMS/MMS (from the
 * SMS app), missed calls (from the dialer), anything the user wants to
 * hear without looking. Android's Notification Access API; the user
 * grants it once in Settings → Special access → Notification access
 * ([ensureGranted] routes them there — it cannot be granted
 * programmatically).
 *
 * Deliberately conservative about WHAT speaks: ongoing notifications
 * (music, navigation), group summaries, our own app, empty texts, and
 * update-bursts to the same notification are all skipped. Speak on the
 * main thread ([TtsEngine.speakQueued]'s contract), in the default
 * voice — notifications are system announcements, not an agent.
 */
class NotifyListenerService : NotificationListenerService() {

    companion object {
        private const val TAG = "HiveNotify"
        private const val MAX_SPEAK_CHARS = 200
        private const val DEBOUNCE_MS = 4_000L

        /** Is Notification Access granted for this app? Read straight
         *  from Settings.Secure — no androidx dependency. */
        fun granted(ctx: Context): Boolean {
            val flat = Settings.Secure.getString(
                ctx.contentResolver, "enabled_notification_listeners") ?: return false
            return flat.split(':').mapNotNull { ComponentName.unflattenFromString(it) }
                .any { it.packageName == ctx.packageName }
        }

        /**
         * The built-in grant popup: explains + routes to the system
         * toggle screen. Call when the feature is enabled without the
         * permission. Returns granted state (true = no dialog needed).
         */
        fun ensureGranted(activity: Activity): Boolean {
            if (granted(activity)) return true
            AlertDialog.Builder(activity)
                .setTitle("Notification access")
                .setMessage("To read your notifications aloud — SMS, missed " +
                            "calls — HiveMind needs Notification access.\n\n" +
                            "On the next screen, find HiveMind and switch it ON.")
                .setPositiveButton("Open settings") { _, _ ->
                    activity.startActivity(
                        Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                }
                .setNegativeButton("Not now", null)
                .show()
            return false
        }
    }

    private val ui = Handler(Looper.getMainLooper())
    private var tts: TtsEngine? = null
    private var lastKey = ""
    private var lastAt = 0L
    private var lastSpoken = ""

    override fun onListenerConnected() {
        Log.d(TAG, "listener connected")
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (!Prefs.notifyAnnounce(this)) return
        if (sbn.packageName == packageName) return
        val n = sbn.notification
        if (n.flags and Notification.FLAG_ONGOING_EVENT != 0) return
        if (n.flags and Notification.FLAG_GROUP_SUMMARY != 0) return

        // Update-burst debounce: one notification reposted with progress
        // (downloads, timers) must speak at most once per DEBOUNCE_MS.
        val now = System.currentTimeMillis()
        synchronized(this) {
            if (sbn.key == lastKey && now - lastAt < DEBOUNCE_MS) return
            lastKey = sbn.key
            lastAt = now
        }

        val extras = n.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)
            ?.toString()?.trim().orEmpty()
        val big = extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()
        val text = (big ?: extras.getCharSequence(Notification.EXTRA_TEXT)?.toString())
            ?.trim().orEmpty()
        if (title.isEmpty() && text.isEmpty()) return

        val spoken = if (text.isEmpty()) title
                       else if (title.isEmpty()) text
                       else if (text == title) title
                       else "$title. $text"
        synchronized(this) {
            if (spoken == lastSpoken) return   // identical repost
            lastSpoken = spoken
        }
        Log.d(TAG, "notify announce: ${sbn.packageName} -> $spoken")
        ui.post { engine().speakQueued(spoken.take(MAX_SPEAK_CHARS)) }
        pushToHub(sbn.packageName, title, text)
    }

    /** Relay to the hub so the user's chosen agent sees the
     *  notification in DM context (folded into the next wake).
     *  Fire-and-forget worker thread; failures are logged and
     *  dropped — the local announce already happened. */
    private fun pushToHub(pkg: String, title: String, text: String) {
        val agentId = Prefs.notifyAgent(this)
        if (agentId.isEmpty()) return
        val label = try {
            val ai = packageManager.getApplicationInfo(pkg, 0)
            packageManager.getApplicationLabel(ai).toString()
        } catch (e: Exception) {
            pkg
        }
        val appLabel = if (label.length > 24) pkg else label
        Thread {
            try {
                ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
                    .postNotification(agentId, appLabel, title, text)
            } catch (e: Exception) {
                Log.w(TAG, "hub push failed: ${e.message}")
            }
        }.start()
    }

    private fun engine(): TtsEngine =
        tts ?: TtsEngine(this, onQueueEmpty = { /* standalone */ }).also { tts = it }

    override fun onDestroy() {
        tts?.destroy()
        super.onDestroy()
    }
}
