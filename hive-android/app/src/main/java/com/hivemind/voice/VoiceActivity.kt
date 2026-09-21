package com.hivemind.voice

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.TextView
import java.util.concurrent.Executors

/**
 * Voice session with one agent.
 *
 * Big round button: tap once to talk, tap again to send; while the agent
 * speaks it becomes an interrupt (cut off → straight back to recording).
 * Small side button: cancel / do not send.
 *
 * The reply is STREAMED to TTS: while the turn runs, the hub's live
 * partial (task_delta stream, same source as the desktop client's live
 * chunks) is sentence-chunked and queued to TextToSpeech, so the agent
 * starts speaking from its first sentence.
 *
 * IDLE ──tap──▶ LISTENING ──tap──▶ THINKING ──first sentence──▶ STREAMING ──speech done──▶ IDLE
 *                  │                    │                          │ tap: cut off
 *                  └────── ✕ cancel anywhere ──────────────────────┴──▶ IDLE
 */
class VoiceActivity : Activity() {

    companion object {
        const val EXTRA_AGENT_ID = "agent_id"
        const val EXTRA_AGENT_NAME = "agent_name"
        const val EXTRA_AGENT_AVATAR = "agent_avatar"
        private const val TAG = "HiveVoice"
        private const val REQ_AUDIO = 42
        private const val POLL_MS = 700L
        private const val IDLE_POLL_MS = 2_500L
        private const val STALL_TIMEOUT_MS = 240_000L  // no text growth
        private const val NET_FAIL_TIMEOUT_MS = 45_000L  // consecutive poll failures
        private const val MAX_UTTER_CHARS = 2000  // TTS rejects very long utterances
        private const val INBOX_THREAD = "user"   // hub's User Inbox (kUserThreadId)
    }

    private enum class Phase { SETUP, IDLE, LISTENING, THINKING, STREAMING }

    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())

    private lateinit var sphere: SphereView
    private lateinit var status: TextView
    private lateinit var talk: Button
    private lateinit var cancel: Button
    private lateinit var stt: SttEngine
    private lateinit var tts: TtsEngine
    private lateinit var api: ApiClient

    private var phase = Phase.SETUP
    private var agentId = ""
    private var threadId = ""
    private var lastMessageId = ""
    private val finalReply = StringBuilder()

    // -- inbox watch ----------------------------------------------------------
    /** Agent id -> display name, for "PA says: …" inbox attribution. */
    private var agentNames = mapOf<String, String>()
    private var inboxLastId = ""
    /** Whole messages awaiting speech — only ever drained at IDLE, so an
     *  arriving note can never interleave with recording or a live reply. */
    private val pendingSpeech = ArrayDeque<String>()

    // -- streaming-speech state ---------------------------------------------
    /** The reply text [spokenLen] refers to (live partial while busy). */
    private var streamText = ""
    /** Chars of [streamText] already queued to TTS. */
    private var spokenLen = 0
    private var turnDone = false
    /** Last time the reply visibly grew (partial or stored messages) —
     *  a turn only times out when it STALLS, never for being long. */
    private var lastProgressMs = 0L
    private var lastPartialLen = 0
    /** First consecutive poll-failure time (0 = polls healthy). Written on
     *  the io thread, read on the UI thread. */
    @Volatile private var netFailSince = 0L
    /** Invalidates in-flight poll ticks: a tick fetched under an older
     *  generation is dropped in [handleTick], so a cancelled turn's stale
     *  partial can never resurrect speech inside the NEXT turn. */
    private var turnGen = 0

    private var speakAnimator: Runnable? = null
    private var lastStatusUpdate = 0L
    private var lastTalkClick = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_voice)

        // A voice session must never let the screen time out: locking
        // pauses us, which cancels recording / kills the audio reply.
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        agentId = intent.getStringExtra(EXTRA_AGENT_ID).orEmpty()
        val agentName = intent.getStringExtra(EXTRA_AGENT_NAME).orEmpty()
        findViewById<TextView>(R.id.agentNameText).text = agentName
        findViewById<android.widget.ImageView>(R.id.agentAvatar).setImageBitmap(
            Avatars.bitmap(this, agentName,
                intent.getStringExtra(EXTRA_AGENT_AVATAR).orEmpty()))

        sphere = findViewById(R.id.sphereView)
        status = findViewById(R.id.statusText)
        talk = findViewById(R.id.talkButton)
        cancel = findViewById(R.id.cancelButton)

        api = ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
        stt = SttEngine(
            this,
            onLevel = { if (phase == Phase.LISTENING) sphere.setLevel(it) },
            onTranscript = { text ->
                if (phase == Phase.LISTENING) {
                    // Live transcript (tail) so the user sees capture is
                    // alive — throttled to cut accessibility event churn.
                    val now = System.currentTimeMillis()
                    if (now - lastStatusUpdate > 300) {
                        lastStatusUpdate = now
                        status.text = if (text.isEmpty()) "Listening…"
                        else "…" + text.takeLast(160)
                    }
                }
            },
            onStopped = { msg -> onEngineStopped(msg) },
        )
        tts = TtsEngine(this, onQueueEmpty = {
            ui.post {
                if (phase == Phase.STREAMING && turnDone) toIdle("")
            }
        }, onInitFailed = {
            status.text = "No text-to-speech engine on this device — " +
                          "replies will be silent"
        })
        // Per-agent voice (chosen in VoicePickerActivity); "" = default.
        Prefs.voiceFor(this, agentId).ifEmpty { null }?.let {
            tts.preferredVoiceName = it
        }

        talk.isEnabled = false
        status.text = "Connecting…"

        talk.setOnClickListener {
            // Debounce: a double-fired click would TALK->SEND an empty take.
            val now = System.currentTimeMillis()
            if (now - lastTalkClick < 500) return@setOnClickListener
            lastTalkClick = now
            when (phase) {
                Phase.IDLE -> maybeStartListening()
                Phase.LISTENING -> sendTranscript()
                Phase.STREAMING -> interruptAndTalk()
                else -> Unit  // SETUP/THINKING: disabled
            }
        }
        cancel.setOnClickListener { cancelCurrent() }

        openThread()
    }

    // -- Setup ------------------------------------------------------------

    private fun openThread() {
        io.execute {
            try {
                threadId = api.openDm(agentId)
                lastMessageId = api.latestMessageId(threadId)
                agentNames = runCatching {
                    api.listAgents().associate { it.id to it.name }
                }.getOrDefault(emptyMap())
                // Inbox may not exist yet (no agent has ever posted) — the
                // hub 404s; treat as empty rather than failing setup.
                inboxLastId = runCatching { api.latestMessageId(INBOX_THREAD) }
                    .getOrDefault("")
                ui.post {
                    toIdle("Tap TALK and speak")
                    ui.postDelayed(inboxWatch, IDLE_POLL_MS)
                }
            } catch (e: Exception) {
                ui.post {
                    status.text = "Couldn't open conversation:\n${e.message}\n(tap to retry)"
                    status.setOnClickListener {
                        if (phase == Phase.SETUP) {
                            status.text = "Connecting…"
                            openThread()
                        }
                    }
                }
            }
        }
    }

    // -- Phase transitions --------------------------------------------------

    private fun toIdle(message: String) {
        turnGen++
        phase = Phase.IDLE
        finalReply.clear()
        streamText = ""
        spokenLen = 0
        turnDone = false
        sphere.setSphereState(SphereState.IDLE)
        sphere.setLevel(0f)
        stopSpeakAnimator()
        talk.text = "TALK"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = true
        cancel.visibility = View.INVISIBLE
        if (message.isNotEmpty()) status.text = message
        // Notes that arrived mid-turn wait for exactly this moment.
        if (pendingSpeech.isNotEmpty()) speakPending()
    }

    private fun maybeStartListening() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO)
            return
        }
        phase = Phase.LISTENING
        sphere.setSphereState(SphereState.LISTENING)
        talk.text = "SEND"
        talk.setBackgroundResource(R.drawable.btn_talk_active)
        cancel.visibility = View.VISIBLE
        status.text = if (stt.engineReady) "Listening…"
        else "Loading speech model…"
        stt.start()
    }

    private fun sendTranscript() {
        sphere.setLevel(0f)
        phase = Phase.THINKING
        turnGen++
        sphere.setSphereState(SphereState.THINKING)
        talk.text = "···"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = false
        cancel.visibility = View.VISIBLE
        status.text = "Sending…"
        lastProgressMs = System.currentTimeMillis()
        lastPartialLen = 0
        netFailSince = 0L

        // Capture teardown + tail decode finish off the UI thread; the
        // message is posted once the transcript is FINAL (the alternative —
        // reading it here — would drop the last fraction of a second of
        // speech, or block the UI on the capture-thread join).
        stt.stop {
            if (phase != Phase.THINKING) return@stop  // cancelled meanwhile
            val text = stt.transcript.trim()
            if (text.isEmpty()) {
                toIdle("Didn't catch that — tap TALK to retry")
                return@stop
            }
            io.execute {
                try {
                    api.postMessage(threadId, SpeechText.OUTGOING_PREFIX + text)
                    ui.post {
                        if (phase != Phase.THINKING) return@post
                        status.text = "Thinking…"
                        ui.postDelayed(pollTick, POLL_MS)
                    }
                } catch (e: Exception) {
                    ui.post {
                        toIdle("Send failed: ${e.message}")
                        speakNotice("Your message didn't send. " +
                                    "Check the connection and try again.")
                    }
                }
            }
        }
    }

    // -- Inbox + DM idle watch -------------------------------------------------

    /**
     * At IDLE, poll the DM and the User Inbox for anything new. Agent
     * messages become pending speech — spoken ONLY at idle boundaries,
     * in arrival order, one after another (TTS is itself a serial
     * queue). While the user records or a reply streams, arriving notes
     * wait silently: voices never mix.
     */
    private val inboxWatch = object : Runnable {
        override fun run() {
            if (phase != Phase.IDLE) {
                ui.postDelayed(this, IDLE_POLL_MS)
                return
            }
            io.execute {
                try {
                    val fresh = mutableListOf<String>()
                    for (m in api.messagesAfter(threadId, lastMessageId)) {
                        lastMessageId = m.id
                        if (m.senderKind == "agent" && m.body.isNotBlank())
                            fresh.add(m.body.trim())
                    }
                    val inbox = runCatching {
                        api.messagesAfter(INBOX_THREAD, inboxLastId)
                    }.getOrDefault(emptyList())
                    for (m in inbox) {
                        inboxLastId = m.id
                        if (m.senderKind == "agent" && m.body.isNotBlank()) {
                            val who = agentNames[m.senderId] ?: "An agent"
                            fresh.add("$who says: ${m.body.trim()}")
                        }
                    }
                    ui.post {
                        if (fresh.isNotEmpty()) {
                            pendingSpeech.addAll(fresh)
                            speakPending()
                        }
                        ui.postDelayed(this, IDLE_POLL_MS)
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "inbox poll failed: ${e.message}")
                    ui.postDelayed(this, IDLE_POLL_MS)
                }
            }
        }
    }

    /**
     * Speak a short status notice aloud — voice-first UX: the user is not
     * watching the screen, so abnormal endings must be HEARD. Call AFTER
     * toIdle; re-enters STREAMING for the notice and returns to IDLE via
     * onQueueEmpty when it has played.
     */
    private fun speakNotice(text: String) {
        turnDone = true            // standalone utterance: nothing more coming
        streamText = text
        spokenLen = 0
        drainSpeech(flush = true)
    }

    /** Speak the next pending note — IDLE only. */
    private fun speakPending() {
        if (phase != Phase.IDLE || pendingSpeech.isEmpty()) return
        val text = pendingSpeech.removeFirst()
        turnDone = true            // standalone note: nothing more coming
        streamText = text
        spokenLen = 0
        drainSpeech(flush = true)  // queues it whole → STREAMING via queueSpeech
    }

    /** First chunk of the reply is being spoken: go green, stay receiving. */
    private fun enterStreaming() {
        phase = Phase.STREAMING
        sphere.setSphereState(SphereState.SPEAKING)
        talk.text = "TALK"          // interrupt-into-recording
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = true
        cancel.visibility = View.VISIBLE
        status.text = ""
        startSpeakAnimator()
    }

    // -- Turn polling + streamed speech ---------------------------------------

    private val pollTick = object : Runnable {
        override fun run() {
            if (phase != Phase.THINKING && phase != Phase.STREAMING) return
            val now = System.currentTimeMillis()
            if (netFailSince != 0L && now - netFailSince > NET_FAIL_TIMEOUT_MS) {
                tts.stop()
                toIdle("Connection lost")
                speakNotice("I lost the connection to the hub. " +
                            "Check your network, then ask again.")
                return
            }
            if (now - lastProgressMs > STALL_TIMEOUT_MS) {
                tts.stop()
                toIdle("Agent stopped responding")
                speakNotice("The agent stopped responding partway through. " +
                            "I'll read the full reply out when it arrives.")
                return
            }
            val gen = turnGen
            io.execute {
                try {
                    val act = api.activity(threadId)
                    var appended = false
                    for (m in api.messagesAfter(threadId, lastMessageId)) {
                        lastMessageId = m.id
                        if (m.senderKind == "agent" && m.body.isNotBlank()) {
                            if (finalReply.isNotEmpty()) finalReply.append(' ')
                            finalReply.append(m.body.trim())
                            appended = true
                        }
                    }
                    netFailSince = 0L
                    // finalReply + activity state converge on this thread;
                    // hand a snapshot to the UI side.
                    val busy = act.busy
                    val error = act.error
                    val partial = act.partial
                    val grew = appended || partial.length > lastPartialLen
                    if (partial.isNotEmpty()) lastPartialLen = partial.length
                    ui.post { handleTick(busy, error, partial, gen, grew) }
                } catch (e: Exception) {
                    // Transient hub/network blip — keep polling until the
                    // failure streak itself times out (checked above).
                    if (netFailSince == 0L)
                        netFailSince = System.currentTimeMillis()
                    ui.postDelayed(this, POLL_MS)
                }
            }
        }
    }

    private fun handleTick(busy: Boolean, error: String, partial: String,
                           gen: Int, grew: Boolean) {
        if (gen != turnGen) return  // stale tick from a cancelled turn
        if (phase != Phase.THINKING && phase != Phase.STREAMING) return
        if (grew) lastProgressMs = System.currentTimeMillis()
        if (error.isNotEmpty()) {
            tts.stop()
            toIdle("Agent error: $error")
            speakNotice("Sorry, the agent hit an error and stopped. " +
                        "Please try again.")
            return
        }
        // The text the spoken length refers to: the live partial while the
        // turn runs, the final message body once it's stored.
        val next = if (busy) partial
        else finalReply.toString().ifEmpty { partial }
        if (busy && next.isEmpty()) {
            // Transient empty partial (tool call / turn hand-over): keep
            // the speech cursor — an empty source must NOT reset it.
            ui.postDelayed(pollTick, POLL_MS)
            return
        }
        if (!next.startsWith(streamText)) {
            // Source diverged (the stored reply is normalized vs the live
            // partial, or a genuinely new reply): re-speak only from the
            // first differing character, never the whole message again.
            val lcp = next.commonPrefixWith(streamText).length
            if (streamText.isNotEmpty() && lcp < spokenLen) {
                Log.d(TAG, "stream diverged at $lcp (spoken=$spokenLen)")
            }
            spokenLen = minOf(spokenLen, lcp)
            streamText = next
        } else {
            streamText = next
        }
        drainSpeech(flush = !busy)
        if (busy) {
            ui.postDelayed(pollTick, POLL_MS)
        } else {
            turnDone = true
            if (tts.outstanding == 0) toIdle("")
        }
    }

    /**
     * Queue newly-completed sentences of [streamText] to TTS. While the
     * turn is still running only text up to a SAFE sentence boundary is
     * spoken (the tail may be mid-sentence and still change); [flush]
     * speaks the remainder once the turn is done.
     */
    private fun drainSpeech(flush: Boolean) {
        if (flush) {
            val tail = streamText.substring(spokenLen).trim()
            if (tail.isNotEmpty()) queueSpeech(tail)
            spokenLen = streamText.length
            return
        }
        var end = -1
        for (i in streamText.length - 1 downTo spokenLen + 1) {
            val ch = streamText[i - 1]
            if ((ch == '.' || ch == '!' || ch == '?' || ch == ':' ||
                        ch == ';' || ch == '\n') && streamText[i].isWhitespace()) {
                end = i
                break
            }
        }
        if (end > spokenLen) {
            val chunk = streamText.substring(spokenLen, end).trim()
            if (chunk.isNotEmpty()) queueSpeech(chunk)
            spokenLen = end
        }
    }

    private fun queueSpeech(text: String) {
        val clean = SpeechText.forSpeech(text)
        if (clean.isEmpty()) return
        // TextToSpeech silently REJECTS very long utterances (typical cap
        // ~4000 chars) — a single oversized sentence would vanish from the
        // spoken reply. Split on word boundaries so nothing is dropped.
        var rest = clean
        while (rest.length > MAX_UTTER_CHARS) {
            var cut = rest.lastIndexOf(' ', MAX_UTTER_CHARS)
            if (cut < MAX_UTTER_CHARS / 2) cut = MAX_UTTER_CHARS
            queueSpeechChunk(rest.substring(0, cut))
            rest = rest.substring(cut).trimStart()
        }
        queueSpeechChunk(rest)
    }

    private fun queueSpeechChunk(text: String) {
        if (text.isEmpty()) return
        Log.d(TAG, "speak chunk: ${text.take(40)}…")
        tts.speakQueued(text)
        if (phase == Phase.THINKING || phase == Phase.IDLE) enterStreaming()
    }

    // -- Interrupt / cancel ---------------------------------------------------

    private fun onEngineStopped(msg: String) {
        when {
            phase != Phase.LISTENING ->
                status.text = msg  // never swallow an idle-phase failure
            stt.transcript.isNotEmpty() -> {
                status.text = "⚠ $msg — SEND keeps what you have, TALK restarts"
                // Phase stays LISTENING: SEND posts stt.transcript as-is.
            }
            else -> toIdle(msg)
        }
    }

    /** Cut the agent's reply short and immediately start recording. */
    private fun interruptAndTalk() {
        tts.stop()
        cancelTurnRemote()
        toIdle("")
        maybeStartListening()
    }

    private fun cancelCurrent() {
        // Stop means STOP: queued inbox notes must not start playing the
        // moment the cancel lands (toIdle would otherwise drain them).
        pendingSpeech.clear()
        when (phase) {
            Phase.LISTENING -> stt.discard()
            Phase.THINKING, Phase.STREAMING -> {
                tts.stop()
                cancelTurnRemote()
            }
            else -> {}
        }
        if (phase != Phase.SETUP) toIdle("Cancelled")
    }

    /**
     * Hub-side cancel + baseline resync. Anything the cancelled turn still
     * STORES after the cancel is swallowed (the poll baseline moves past
     * it), so its ghost can't be picked up by inboxWatch and spoken as a
     * "note" later. Bounded wait: a queued postMessage for the user's next
     * utterance sits behind this on the single io thread.
     */
    private fun cancelTurnRemote() {
        io.execute {
            api.cancel(threadId)
            val deadline = System.currentTimeMillis() + 3_000
            while (System.currentTimeMillis() < deadline) {
                val act = runCatching { api.activity(threadId) }.getOrNull()
                    ?: break
                if (!act.busy) break
                Thread.sleep(300)
            }
            runCatching { api.latestMessageId(threadId) }.getOrNull()
                ?.let { if (it.isNotEmpty()) lastMessageId = it }
        }
    }

    // -- Sphere level while speaking ------------------------------------------

    private fun startSpeakAnimator() {
        stopSpeakAnimator()
        val start = System.nanoTime()
        val r = object : Runnable {
            override fun run() {
                if (phase != Phase.STREAMING) return
                val t = (System.nanoTime() - start) / 1_000_000_000f
                sphere.setLevel(tts.level(t))
                ui.postDelayed(this, 50)
            }
        }
        speakAnimator = r
        ui.post(r)
    }

    private fun stopSpeakAnimator() {
        speakAnimator?.let { ui.removeCallbacks(it) }
        speakAnimator = null
    }

    // -- Permission -----------------------------------------------------------

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_AUDIO) {
            if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
                maybeStartListening()
            } else {
                status.text = "Microphone permission is required"
            }
        }
    }

    // -- Lifecycle --------------------------------------------------------------

    override fun onResume() {
        super.onResume()
        sphere.onResume()
    }

    override fun onPause() {
        sphere.onPause()
        if (phase == Phase.LISTENING || phase == Phase.STREAMING) cancelCurrent()
        super.onPause()
    }

    override fun onDestroy() {
        stopSpeakAnimator()
        ui.removeCallbacksAndMessages(null)
        stt.destroy()
        tts.destroy()
        io.shutdownNow()
        super.onDestroy()
    }
}
