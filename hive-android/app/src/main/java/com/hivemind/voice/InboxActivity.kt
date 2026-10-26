package com.hivemind.voice

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.ImageView
import android.widget.ListView
import android.widget.TextView
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.util.concurrent.Executors

/** The hub's one-way agent-to-user inbox, with per-sender tap-to-read. */
class InboxActivity : Activity() {

    companion object {
        private const val INBOX_THREAD_ID = "user"
        private const val POLL_MS = 5_000L
        private const val MAX_UTTERANCE_CHARS = 3_000
        private const val SENTENCE_ENDINGS = ".!?;:\n"
    }

    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var api: ApiClient
    private lateinit var status: TextView
    private lateinit var list: ListView
    private lateinit var tts: TtsEngine
    private var adapter: InboxAdapter? = null
    private var agents = mapOf<String, ApiClient.Agent>()
    private var latestMessageId = ""
    private var readMarked = false
    private var loading = false
    @Volatile private var foreground = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (!Prefs.isConfigured(this)) {
            startActivity(Intent(this, SettingsActivity::class.java))
            finish()
            return
        }
        setContentView(R.layout.activity_inbox)
        status = findViewById(R.id.inboxStatus)
        list = findViewById(R.id.inboxList)
        tts = TtsEngine(
            this,
            onQueueEmpty = {
                ui.post { if (foreground) showIdleStatus() }
            },
            onInitFailed = {
                ui.post {
                    status.text = getString(R.string.no_tts_engine)
                }
            },
        )
        Thread { tts.initLocal(applicationContext) }.start()

        findViewById<Button>(R.id.stopSpeechButton).setOnClickListener {
            tts.stop()
            showIdleStatus()
        }
        findViewById<Button>(R.id.agentsButton).apply {
            setTextColor(getColor(R.color.dim))
            setOnClickListener { finish() }
        }
        findViewById<Button>(R.id.inboxButton).setTextColor(getColor(R.color.cyan))
        list.setOnItemClickListener { _, _, position, _ ->
            adapter?.getItem(position)?.let(::readMessage)
        }
    }

    override fun onResume() {
        super.onResume()
        if (!Prefs.isConfigured(this)) return
        foreground = true
        api = ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
        ui.removeCallbacks(inboxPoll)
        refreshInbox()
    }

    override fun onPause() {
        foreground = false
        ui.removeCallbacks(inboxPoll)
        if (::tts.isInitialized) tts.stop()
        super.onPause()
    }

    override fun onDestroy() {
        foreground = false
        ui.removeCallbacksAndMessages(null)
        if (::tts.isInitialized) tts.destroy()
        io.shutdownNow()
        super.onDestroy()
    }

    private val inboxPoll = Runnable { refreshInbox() }

    /** Refresh history and sender metadata without letting a roster
     *  failure hide otherwise-readable inbox messages. */
    private fun refreshInbox() {
        if (loading || !foreground) return
        loading = true
        io.execute {
            try {
                val roster = runCatching { api.listAgents() }.getOrDefault(emptyList())
                val messages = api.history(INBOX_THREAD_ID)
                val newestId = messages.lastOrNull()?.id.orEmpty()
                if (!readMarked || newestId != latestMessageId) {
                    runCatching { api.markRead(INBOX_THREAD_ID) }
                        .onSuccess { readMarked = true }
                }
                latestMessageId = newestId
                ui.post {
                    loading = false
                    if (roster.isNotEmpty()) agents = roster.associateBy { it.id }
                    if (adapter == null) {
                        adapter = InboxAdapter(messages, agents)
                        list.adapter = adapter
                    } else {
                        adapter?.update(messages, agents)
                    }
                    if (!tts.speaking) showIdleStatus()
                    scheduleNextRefresh()
                }
            } catch (e: Exception) {
                ui.post {
                    loading = false
                    status.text = getString(R.string.hub_error, e.message ?: "request failed")
                    scheduleNextRefresh()
                }
            }
        }
    }

    private fun scheduleNextRefresh() {
        if (!foreground) return
        ui.removeCallbacks(inboxPoll)
        ui.postDelayed(inboxPoll, POLL_MS)
    }

    private fun showIdleStatus() {
        if (!::status.isInitialized) return
        val count = adapter?.count ?: 0
        status.text = when (count) {
            0 -> getString(R.string.empty_inbox)
            else -> resources.getQuantityString(
                R.plurals.inbox_message_count, count, count,
                getString(R.string.inbox_listen_hint))
        }
    }

    /** Switch immediately to the tapped message, in the SENDER's voice. */
    private fun readMessage(message: ApiClient.Message) {
        val clean = SpeechText.forSpeech(message.body)
        if (clean.isEmpty()) {
            tts.stop()
            status.text = getString(R.string.no_readable_text)
            return
        }
        tts.stop()
        tts.applyVoice(Prefs.voiceFor(this, message.senderId).ifEmpty { null })
        status.text = getString(R.string.reading_sender, senderName(message))
        queueSpeech(clean)
    }

    /** Queue sentences separately so the first one can play while later
     *  sentences synthesize in the background. Oversized sentences are
     *  split at word boundaries, never through a UTF-16 surrogate pair. */
    private fun queueSpeech(text: String) {
        for (sentence in sentenceChunks(text)) {
            var rest = sentence
            while (rest.length > MAX_UTTERANCE_CHARS) {
                var cut = rest.lastIndexOf(' ', MAX_UTTERANCE_CHARS)
                if (cut < MAX_UTTERANCE_CHARS / 2) cut = MAX_UTTERANCE_CHARS
                if (cut > 0 && rest[cut - 1].isHighSurrogate()) cut--
                tts.speakQueued(rest.substring(0, cut).trim())
                rest = rest.substring(cut).trimStart()
            }
            if (rest.isNotEmpty()) tts.speakQueued(rest)
        }
    }

    private fun sentenceChunks(text: String): List<String> = buildList {
        var start = 0
        for (i in text.indices) {
            if (text[i] !in SENTENCE_ENDINGS) continue
            if (i + 1 < text.length && !text[i + 1].isWhitespace()) continue
            val sentence = text.substring(start, i + 1).trim()
            if (sentence.isNotEmpty()) add(sentence)
            start = i + 1
        }
        val tail = text.substring(start).trim()
        if (tail.isNotEmpty()) add(tail)
    }

    private fun senderName(message: ApiClient.Message): String =
        agents[message.senderId]?.name
            ?: message.senderLabel.ifEmpty {
                when (message.senderKind) {
                    "user" -> "You"
                    "agent" -> "Agent"
                    else -> "System"
                }
            }

    private inner class InboxAdapter(
        messages: List<ApiClient.Message>,
        private var agents: Map<String, ApiClient.Agent>,
    ) : BaseAdapter() {
        private var rows = messages.asReversed()

        fun update(messages: List<ApiClient.Message>,
                   agents: Map<String, ApiClient.Agent>) {
            rows = messages.asReversed()
            this.agents = agents
            notifyDataSetChanged()
        }

        override fun getCount() = rows.size
        override fun getItem(position: Int) = rows[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: LayoutInflater.from(this@InboxActivity)
                .inflate(R.layout.inbox_row, parent, false)
            val message = rows[position]
            val agent = agents[message.senderId]
            val name = agent?.name ?: senderName(message)
            view.findViewById<ImageView>(R.id.inboxAvatar).setImageBitmap(
                Avatars.bitmap(this@InboxActivity, name,
                    agent?.avatar ?: if (message.senderKind == "agent") "" else "user"))
            view.findViewById<TextView>(R.id.inboxSender).text = name
            view.findViewById<TextView>(R.id.inboxTime).text =
                formatMessageTime(message.createdAt)
            view.findViewById<TextView>(R.id.inboxBody).text = displayBody(message)
            return view
        }

        private fun displayBody(message: ApiClient.Message): String {
            val attachments = buildList {
                for (i in 0 until message.attachments.length()) {
                    add(message.attachments.getJSONObject(i).optString("name", "attachment"))
                }
            }
            if (attachments.isEmpty()) return message.body
            val attachmentText = attachments.joinToString(prefix = "Attachments: ")
            return if (message.body.isBlank()) attachmentText
            else "${message.body}\n\n$attachmentText"
        }
    }
}

private fun formatMessageTime(value: String): String {
    val instant = runCatching { Instant.parse(value) }.getOrNull() ?: return value
    return DateTimeFormatter.ofLocalizedDateTime(FormatStyle.SHORT)
        .withZone(ZoneId.systemDefault())
        .format(instant)
}
