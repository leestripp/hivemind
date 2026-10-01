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
import java.util.concurrent.Executors

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
 *
 * Catch-up (2026-10-01): on every connect the listener snapshots the
 * notifications still sitting in the shade (the unread ones) and
 * relays them to the hub WITHOUT speaking — the agent gets everything
 * the user hasn't seen yet, even notifications that arrived while the
 * app was dead. Dedupe is a persisted fingerprint set (package | key |
 * postTime | title | text), capped at [MAX_PUSHED]: the same instance
 * with the same content is never relayed twice across restarts, while
 * content updates (new text) and genuinely new notifications (new
 * key/time) always go through. The system's own re-post burst on
 * reconnect races the snapshot; both orders converge on exactly-one
 * push via the same set.
 */
class NotifyListenerService : NotificationListenerService() {

    companion object {
        private const val TAG = "HiveNotify"
        private const val MAX_SPEAK_CHARS = 200

        /** Default receiver for phone notifications — the PA. */
        private const val DEFAULT_AGENT = "Scout"
        private const val DEBOUNCE_MS = 4_000L

        /** After connect, live posts are not SPOKEN for this long —
         *  the system re-posts every active notification on reconnect;
         *  those are history, not news. They still relay to the hub. */
        private const val SPEECH_GRACE_MS = 3_000L

        /** Persisted relay dedupe, service-local prefs file. */
        private const val PUSHED_FILE = "hive_notify"
        private const val KEY_PUSHED = "pushed"
        private const val MAX_PUSHED = 120

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
    private var connectedAt = 0L

    /** Fingerprints of everything already relayed to the hub —
     *  survives restarts so reconnect catch-up never double-sends. */
    private val pushed = LinkedHashSet<String>()

    /** Single-file push queue: a catch-up burst must hit the hub one
     *  POST at a time (ordered, polite) — a thread-per-notification
     *  storm is what triggered the sqlite write-lock collisions. */
    private val pushExecutor = Executors.newSingleThreadExecutor()

    override fun onCreate() {
        super.onCreate()
        pushed.addAll(getSharedPreferences(PUSHED_FILE, Context.MODE_PRIVATE)
            .getStringSet(KEY_PUSHED, emptySet()) ?: emptySet())
    }

    override fun onListenerConnected() {
        connectedAt = System.currentTimeMillis()
        Log.d(TAG, "listener connected, ${pushed.size} fingerprints remembered")
        Thread { catchUp() }.start()
    }

    /** Relay everything still in the shade — the unread backlog. No
     *  speech: this is history, the user has (or could have) seen it.
     *  Duplicates are stopped by the persisted fingerprint set. */
    private fun catchUp() {
        val actives = try { getActiveNotifications() } catch (e: Exception) {
            Log.w(TAG, "catch-up snapshot failed: ${e.message}")
            return
        } ?: return
        Log.d(TAG, "catch-up: ${actives.size} active notification(s)")
        for (sbn in actives) relay(sbn, live = false)
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        relay(sbn, live = true)
    }

    private fun fingerprint(sbn: StatusBarNotification, title: String, text: String): String =
        sbn.packageName + "|" + sbn.key + "|" + sbn.postTime + "|" + title + "|" + text

    private fun relay(sbn: StatusBarNotification, live: Boolean) {
        if (!Prefs.notifyAnnounce(this)) return
        if (sbn.packageName == packageName) return
        val n = sbn.notification
        if (n.flags and Notification.FLAG_ONGOING_EVENT != 0) return
        if (n.flags and Notification.FLAG_GROUP_SUMMARY != 0) return

        val extras = n.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)
            ?.toString()?.trim().orEmpty()
        val big = extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()
        val text = (big ?: extras.getCharSequence(Notification.EXTRA_TEXT)?.toString())
            ?.trim().orEmpty()
        if (title.isEmpty() && text.isEmpty()) return

        // Persisted dedupe: same instance + same content is relayed
        // exactly once, ever (across restarts too). Content updates and
        // new notifications change the fingerprint and always relay.
        val fp = fingerprint(sbn, title, text)
        synchronized(pushed) {
            if (fp in pushed) {
                if (!live) Log.d(TAG, "catch-up skip (already pushed): $title")
                return
            }
            // Claim the fingerprint ATOMICALLY — the system keeps
            // multiple listener connections, so catch-up threads and
            // live posts run concurrently; check-and-mark-in-one-block
            // is what guarantees exactly-once. A failed hub push is
            // dropped, never retried: dedupe correctness beats
            // at-least-once delivery for notifications.
            if (pushed.size >= MAX_PUSHED) pushed.remove(pushed.first())
            pushed.add(fp)
            getSharedPreferences(PUSHED_FILE, Context.MODE_PRIVATE).edit()
                .putStringSet(KEY_PUSHED, HashSet(pushed))
                .apply()
        }

        // Update-burst debounce (live only): one notification reposted
        // with progress (downloads, timers) speaks at most once per
        // DEBOUNCE_MS.
        val now = System.currentTimeMillis()
        if (live) synchronized(this) {
            if (sbn.key == lastKey && now - lastAt < DEBOUNCE_MS) return
            lastKey = sbn.key
            lastAt = now
        }

        // Speech is for live news only — never during the post-connect
        // grace window, never for catch-up snapshots.
        if (live && now - connectedAt > SPEECH_GRACE_MS) {
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
        }

        pushToHub(sbn.packageName, title, text)
    }

    /** Relay to the hub so the user's chosen agent sees the
     *  notification in DM context (folded into the next wake).
     *  Fire-and-forget worker thread; failures are logged and
     *  dropped — the local announce already happened. */
    private fun pushToHub(pkg: String, title: String, text: String) {
        var agentId = Prefs.notifyAgent(this)
        if (agentId == Prefs.OPT_OUT) return          // explicit opt-out
        if (agentId.isEmpty()) {
            // Never chosen: default to the PA (Scout). One blocking
            // roster fetch on this worker thread, then cached in Prefs
            // — the Settings picker shows the resolved selection.
            agentId = try {
                ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
                    .listAgents()
                    .firstOrNull { it.name == DEFAULT_AGENT }?.id ?: ""
            } catch (e: Exception) {
                Log.w(TAG, "default agent lookup failed: ${e.message}")
                ""
            }
            if (agentId.isEmpty()) return
            Prefs.setNotifyAgent(this, agentId)
            Log.d(TAG, "default notify agent -> $DEFAULT_AGENT")
        }
        val label = try {
            val ai = packageManager.getApplicationInfo(pkg, 0)
            packageManager.getApplicationLabel(ai).toString()
        } catch (e: Exception) {
            pkg
        }
        val appLabel = if (label.length > 24) pkg else label
        pushExecutor.execute {
            try {
                ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
                    .postNotification(agentId, appLabel, title, text)
            } catch (e: Exception) {
                Log.w(TAG, "hub push failed: ${e.message}")
            }
        }
    }

    private fun engine(): TtsEngine =
        tts ?: TtsEngine(this, onQueueEmpty = { /* standalone */ }).also { tts = it }

    override fun onDestroy() {
        pushExecutor.shutdown()
        tts?.destroy()
        super.onDestroy()
    }
}
