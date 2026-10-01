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
        private const val STOP_CONFIRM_MS = 12_000L  // bounded wait for a stop to confirm
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
    private var agentName = ""
    private var threadId = ""
    /** Edge-triggered working chrome (sphere THINKING + visible cancel)
     *  for the session agent being busy in ANOTHER thread — the DM's own
     *  activity endpoint stays quiet there, so without the roster's
     *  agent-global busy flag the screen would show idle mid-task. */
    private var showingAgentBusy = false
    /** A stop was requested and the agent has NOT confirmed free yet.
     *  An unkillable command (long bash exec) keeps the turn "busy"
     *  for minutes after the hub accepts the cancel, so input stays
     *  locked until the polls below see the agent actually idle.
     *  Written on the io thread (confirm loops), read on the UI thread. */
    @Volatile private var stopping = false
    /** A message was sent while the agent was still busy/cancelling.
     *  The hub durably queues the wake; the UI must neither stream the
     *  dying turn's partial as the answer nor give up in the dead zone
     *  between the old turn dying and the queued wake dispatching. */
    private var queuedBehindStop = false
    private var lastMessageId = ""
    private val finalReply = StringBuilder()

    // -- inbox watch ----------------------------------------------------------
    /** Agent id -> display name, for "PA says: …" inbox attribution. */
    private var agentNames = mapOf<String, String>()
    private var inboxLastId = ""
    /** A whole message awaiting speech: the text plus WHO sent it, so
     *  it can be read in the sender's own voice (Phase 68). */
    /** Edge-triggered state-change announcements (see AgentStateAnnouncer).
     *  The session agent's routine working/finished edges are skipped —
     *  the reply stream itself narrates it. */
    private val stateAnnouncer = AgentStateAnnouncer(
        enabled = { Prefs.stateAnnounce(this) },
        suppressedAgentId = { agentId },
    )

    private data class PendingNote(val senderId: String, val text: String)
    /** Whole messages awaiting speech — only ever drained at IDLE, so an
     *  arriving note can never interleave with recording or a live reply. */
    private val pendingSpeech = ArrayDeque<PendingNote>()

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
    /** Partial length a stalled narration gave up at — inboxWatch must not
     *  re-narrate the same stuck turn until the partial GROWS past this
     *  (reset when the turn ends / busy goes false). */
    private var narrationGiveUpLen = 0
    /** Text already narrated live from the partial feed. When the turn's
     *  stored message arrives later (stall-abort recovery), only the tail
     *  past this prefix is spoken — never the whole reply twice. */
    private var narratedPrefix = ""
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
        agentName = intent.getStringExtra(EXTRA_AGENT_NAME).orEmpty()
        val agentName = this.agentName
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
                // stopping: input is locked until the stop confirms
                // (talk.isEnabled is false too — this is the backstop).
                Phase.IDLE -> if (!stopping) maybeStartListening()
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
                // 2026-10-01: opening the voice chat == viewing the
                // thread — clear the hub's unread mark (best effort).
                runCatching { api.markRead(threadId) }
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
        stopping = false
        queuedBehindStop = false
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
        // The streamed reply comes from the SESSION agent — re-apply its
        // voice (a just-spoken inbox note may have left another agent's
        // voice active on the engine).
        tts.applyVoice(voiceNameFor(agentId))
        sphere.setSphereState(SphereState.PROCESSING)
        talk.text = "···"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = false
        cancel.visibility = View.VISIBLE
        status.text = "Transcribing…"
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
            ui.post { status.text = "Sending…" }
            io.execute {
                try {
                    sendAndEnterWait(text)
                } catch (e: Exception) {
                    // A vanished DM thread is recoverable: re-resolve and
                    // retry the send ONCE before surfacing a failure.
                    if (reresolveIfThreadGone(e)) {
                        try {
                            sendAndEnterWait(text)
                            return@execute
                        } catch (_: Exception) {
                            // fall through to the visible failure below
                        }
                    }
                    ui.post {
                        toIdle("Send failed: ${e.message}")
                        speakNotice("Your message didn't send. " +
                                    "Check the connection and try again.")
                    }
                }
            }
        }
    }

    /**
     * Post the transcript and enter the matching wait state. The agent
     * is POLLED FIRST: if it is still tied up — a just-cancelled bash
     * command can't die mid-flight, a task on another thread keeps it
     * busy — the hub durably queues the message's wake, so say "queued"
     * and wait for the REAL reply turn instead of streaming the dying
     * turn's leftovers as if they answered (the "old message read out"
     * bug).
     */
    private fun sendAndEnterWait(text: String) {
        val preAct = runCatching { api.activity(threadId) }.getOrNull()
        val agentBusy = runCatching {
            api.listAgents().firstOrNull { it.id == agentId }?.busy == true
        }.getOrDefault(false)
        val queued = preAct?.cancelling == true || agentBusy
        api.postMessage(threadId, SpeechText.OUTGOING_PREFIX + text)
        ui.post {
            if (phase != Phase.THINKING) return@post
            queuedBehindStop = queued
            sphere.setSphereState(SphereState.THINKING)
            status.text = if (queued)
                "${agentName.ifEmpty { "Agent" }} is busy — " +
                    "your message is queued\u2026"
            else "Thinking\u2026"
            ui.postDelayed(pollTick, POLL_MS)
        }
    }

    /**
     * The cached DM thread id can dangle: the thread may be deleted from
     * another client while we sit on it (2026-09-30 incident — the app
     * 404-looped forever and voice messages posted into the void). When a
     * call fails with HTTP 404 thread-not-found, re-resolve the canonical
     * DM via openDm and let the caller retry/continue. Returns true when
     * the thread was re-resolved.
     */
    private fun reresolveIfThreadGone(e: Exception): Boolean {
        if (e !is ApiException || e.code != 404) return false
        return try {
            threadId = api.openDm(agentId)
            lastMessageId = api.latestMessageId(threadId)
            runCatching { api.markRead(threadId) }
            narratedPrefix = ""
            Log.w(TAG, "DM thread vanished - re-resolved to $threadId")
            true
        } catch (e2: Exception) {
            Log.w(TAG, "DM re-resolve failed: ${e2.message}")
            false
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
                    val fresh = mutableListOf<PendingNote>()
                    var dmConsumed = false
                    for (m in api.messagesAfter(threadId, lastMessageId)) {
                        lastMessageId = m.id
                        dmConsumed = true
                        if (m.senderKind == "agent" && m.body.isNotBlank()) {
                            var body = m.body.trim()
                            // Already narrated live from the partial feed?
                            // Speak only the tail that was never heard.
                            val lcp = body.commonPrefixWith(narratedPrefix).length
                            if (narratedPrefix.isNotEmpty() && lcp >= 40) {
                                narratedPrefix = ""
                                body = body.substring(lcp).trim()
                            }
                            if (body.isNotEmpty())
                                fresh.add(PendingNote(m.senderId, body))
                        }
                    }
                    // Live-turn watch: a reply STARTED ELSEWHERE (desktop /
                    // web / routine) streams into the hub's partial feed —
                    // narrate it as it grows instead of waiting for the
                    // stored message (2026-09-29: "chunks sit in the live
                    // feed while the user waits").
                    val act = runCatching { api.activity(threadId) }.getOrNull()
                    val inbox = runCatching {
                        api.messagesAfter(INBOX_THREAD, inboxLastId)
                    }.getOrDefault(emptyList())
                    var inboxConsumed = false
                    for (m in inbox) {
                        inboxLastId = m.id
                        inboxConsumed = true
                        if (m.senderKind == "agent" && m.body.isNotBlank()) {
                            val who = agentNames[m.senderId] ?: "An agent"
                            fresh.add(PendingNote(m.senderId,
                                "$who says: ${m.body.trim()}"))
                        }
                    }
                    // Agent state edge watch (2026-10-01): read state
                    // transitions aloud, edge-triggered + dwelled. Also
                    // refresh agentNames so inbox notes name new agents.
                    val roster = runCatching { api.listAgents() }
                        .getOrDefault(emptyList())
                    // Agent-global busy (any thread) — the roster is the
                    // only source; the thread activity endpoint below is
                    // blind to turns running outside this DM.
                    val me = roster.firstOrNull { it.id == agentId }
                    if (roster.isNotEmpty()) {
                        agentNames = roster.associate { it.id to it.name }
                        // Announcement notes carry the transitioning
                        // agent's id, so speakPending reads them in THAT
                        // agent's configured voice (falls back to the
                        // session voice) — you can tell who it is by ear.
                        for (ann in stateAnnouncer.diff(roster))
                            fresh.add(PendingNote(ann.agentId, ann.text))
                    }
                    // Clear the persisted unread marks (66c) for anything
                    // this session consumed, or desktop/web show phantom
                    // unread from voice sessions.
                    if (dmConsumed) runCatching { api.markRead(threadId) }
                    if (inboxConsumed) runCatching { api.markRead(INBOX_THREAD) }
                    ui.post {
                        // Stop confirmation (agent level): the agent was
                        // told to stop but an unkillable command kept it
                        // busy past the bounded wait — stand down only
                        // now that the roster confirms it is free.
                        if (stopping && me != null && !me.busy) {
                            stopping = false
                            showingAgentBusy = false
                            toIdle("Stopped")
                        }
                        if (act != null) {
                            if (!act.busy) narrationGiveUpLen = 0
                            // Busy alone is enough to start: the empty-
                            // partial guard used to block entry while the
                            // agent was mid-tool-call (0 > 0). After a
                            // stall-abort the give-up mark still demands
                            // partial GROWTH before re-narrating.
                            else if (narrationGiveUpLen == 0 ||
                                     act.partial.length > narrationGiveUpLen)
                                startIdleNarration()
                        }
                        if (fresh.isNotEmpty()) {
                            pendingSpeech.addAll(fresh)
                            speakPending()
                        }
                        // Working chrome, edge-triggered on the roster's
                        // agent-global busy flag: busy in ANOTHER thread
                        // shows the sphere working + a Stop button, which
                        // agent-cancel (not the thread cancel) stops.
                        // Only transitions AT IDLE are recorded: chrome/
                        // narration/recording phases own the screen, and
                        // recording a busy edge mid-turn would suppress
                        // the chrome when the turn hands back to IDLE
                        // with the agent still working. A vanished roster
                        // fetch (me == null) keeps the last state.
                        if (me != null && phase == Phase.IDLE &&
                                me.busy != showingAgentBusy) {
                            showingAgentBusy = me.busy
                            if (me.busy) {
                                sphere.setSphereState(SphereState.THINKING)
                                status.text =
                                    "${agentName.ifEmpty { "Agent" }} is working\u2026"
                                cancel.visibility = View.VISIBLE
                            } else {
                                sphere.setSphereState(SphereState.IDLE)
                                cancel.visibility = View.INVISIBLE
                                // Clear only labels WE set — never
                                // clobber status text from other paths
                                // ("Stopping\u2026" = our own cancel).
                                val was = "${agentName.ifEmpty { "Agent" }} is working\u2026"
                                if (status.text.toString() == was ||
                                        status.text.toString() == "Stopping\u2026")
                                    status.text = ""
                            }
                        }
                        ui.postDelayed(this, IDLE_POLL_MS)
                    }
                } catch (e: Exception) {
                    if (!reresolveIfThreadGone(e))
                        Log.w(TAG, "inbox poll failed: ${e.message}")
                    ui.postDelayed(this, IDLE_POLL_MS)
                }
            }
        }
    }

    /**
     * Narrate a live turn the USER did not start here (desktop/web/routine
     * reply on this DM thread): mirror sendTranscript's THINKING setup and
     * let pollTick/handleTick drive the same sentence-by-sentence streaming
     * as a voice-started reply. pollTick consumes the final stored message
     * itself, so inboxWatch never speaks it twice.
     */
    private fun startIdleNarration() {
        if (phase != Phase.IDLE) return
        turnGen++
        finalReply.clear()
        streamText = ""
        spokenLen = 0
        lastPartialLen = 0
        lastProgressMs = System.currentTimeMillis()
        netFailSince = 0L
        phase = Phase.THINKING
        tts.applyVoice(voiceNameFor(agentId))
        sphere.setSphereState(SphereState.THINKING)
        talk.text = "\u00b7\u00b7\u00b7"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = false
        cancel.visibility = View.VISIBLE
        status.text = "Replying\u2026"
        ui.postDelayed(pollTick, POLL_MS)
    }

    /**
     * Speak a short status notice aloud — voice-first UX: the user is not
     * watching the screen, so abnormal endings must be HEARD. Call AFTER
     * toIdle; re-enters STREAMING for the notice and returns to IDLE via
     * onQueueEmpty when it has played.
     */
    private fun speakNotice(text: String) {
        tts.applyVoice(voiceNameFor(agentId))
        turnDone = true            // standalone utterance: nothing more coming
        streamText = text
        spokenLen = 0
        drainSpeech(flush = true)
    }

    /**
     * The voice a message should be READ in: the sender's configured
     * voice when they have one (VoicePickerActivity), else the session
     * agent's, else null = engine default. This is what makes an inbox
     * note from Honey sound like Honey while chatting with PA.
     */
    private fun voiceNameFor(senderId: String): String? {
        Prefs.voiceFor(this, senderId).ifEmpty { null }?.let { return it }
        if (senderId != agentId)
            Prefs.voiceFor(this, agentId).ifEmpty { null }?.let { return it }
        return null
    }

    /** Speak the next pending note — IDLE only. */
    private fun speakPending() {
        if (phase != Phase.IDLE || pendingSpeech.isEmpty()) return
        val note = pendingSpeech.removeFirst()
        tts.applyVoice(voiceNameFor(note.senderId))
        turnDone = true            // standalone note: nothing more coming
        streamText = note.text
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
                narratedPrefix = streamText
                narrationGiveUpLen = lastPartialLen
                toIdle("Connection lost")
                speakNotice("I lost the connection to the hub. " +
                            "Check your network, then ask again.")
                return
            }
            if (now - lastProgressMs > STALL_TIMEOUT_MS) {
                tts.stop()
                narratedPrefix = streamText
                narrationGiveUpLen = lastPartialLen
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
                    val cancelling = act.cancelling
                    val grew = appended || partial.length > lastPartialLen
                    if (partial.isNotEmpty()) lastPartialLen = partial.length
                    ui.post { handleTick(busy, error, partial, cancelling,
                                         gen, grew) }
                } catch (e: Exception) {
                    // A vanished DM thread is not a network blip — re-resolve
                    // onto the canonical DM and keep polling.
                    if (reresolveIfThreadGone(e)) {
                        netFailSince = 0L
                    } else if (netFailSince == 0L) {
                        // Transient hub/network blip — keep polling until the
                        // failure streak itself times out (checked above).
                        netFailSince = System.currentTimeMillis()
                    }
                    ui.postDelayed(this, POLL_MS)
                }
            }
        }
    }

    private fun handleTick(busy: Boolean, error: String, partial: String,
                           cancelling: Boolean, gen: Int, grew: Boolean) {
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
        // Stop confirmation: only stand down once the turn actually died.
        if (!busy && stopping) {
            toIdle("Stopped")
            return
        }
        // A cancelling turn's partial is the OLD reply on its way out —
        // never stream it as the answer; just wait for the turn to
        // clear. Suppresses the stall watchdog too: an unkillable bash
        // command can hold a cancelling turn live for minutes, by design.
        if (busy && cancelling) {
            lastProgressMs = System.currentTimeMillis()
            ui.postDelayed(pollTick, POLL_MS)
            return
        }
        if (busy && queuedBehindStop) queuedBehindStop = false
        if (!busy && queuedBehindStop) {
            // Dead zone: the old turn died and the hub hasn't dispatched
            // our queued message's wake yet. Not done — the reply is
            // still coming. lastProgressMs is deliberately NOT refreshed
            // here: the stall watchdog stays armed as the safety net if
            // the wake is never dispatched (a normal dispatch lands in
            // ~1 tick, far below STALL_TIMEOUT).
            ui.postDelayed(pollTick, POLL_MS)
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

    /** Cut the agent's reply short and immediately start recording. The
     *  user is about to queue a NEW message — cancel the old turn in the
     *  background (no idle-wait: the send's pre-poll handles the agent
     *  being slow to actually die). */
    private fun interruptAndTalk() {
        tts.stop()
        issueCancelAndResync()
        toIdle("")
        maybeStartListening()
    }

    private fun cancelCurrent() {
        // Stop means STOP: queued inbox notes must not start playing the
        // moment the cancel lands (toIdle would otherwise drain them).
        pendingSpeech.clear()
        when (phase) {
            Phase.LISTENING -> {
                stt.discard()
                toIdle("Cancelled")
            }
            // The stop must CONFIRM before input unlocks: an unkillable
            // command keeps the turn busy after the hub accepts the
            // cancel, and accepting a new message then reads the old
            // turn's leftovers out as the answer.
            Phase.THINKING, Phase.STREAMING -> {
                tts.stop()
                confirmStopThread()   // toIdles itself once the stop confirms
            }
            Phase.IDLE -> if (showingAgentBusy) {
                // Agent mid-task in ANOTHER thread: only an agent-level
                // cancel can stop it — and it too must confirm.
                io.execute { stopAgentAndConfirm() }
            } else toIdle("Cancelled")
            else -> {}
        }
    }

    /**
     * Hub-side cancel + baseline resync, fire-and-forget (interrupt path:
     * the user is queuing a new message whose pre-poll copes with a still-
     * busy agent). Anything the cancelled turn still STORES is swallowed
     * (the poll baseline moves past it) so its ghost can't be picked up by
     * inboxWatch and spoken as a "note" later.
     */
    private fun issueCancelAndResync() {
        io.execute {
            api.cancel(threadId)
            val deadline = System.currentTimeMillis() + 15_000
            while (System.currentTimeMillis() < deadline) {
                val act = runCatching { api.activity(threadId) }.getOrNull()
                    ?: break
                if (!act.busy) break
                Thread.sleep(500)
            }
            runCatching { api.latestMessageId(threadId) }.getOrNull()
                ?.let { if (it.isNotEmpty()) lastMessageId = it }
        }
    }

    /**
     * Thread-level stop that CONFIRMS. Polls the thread's activity
     * (bounded) until the turn actually dies before standing down. On
     * timeout the stop stays pending: [stopping] locks input, and
     * pollTick — which skips a cancelling turn's partial — toIdles on
     * the first not-busy tick, whenever the command finally returns.
     */
    private fun confirmStopThread() {
        io.execute {
          try {
            api.cancel(threadId)
            val deadline = System.currentTimeMillis() + STOP_CONFIRM_MS
            var free = false
            while (System.currentTimeMillis() < deadline) {
                Thread.sleep(500)
                val act = runCatching { api.activity(threadId) }.getOrNull()
                    ?: break
                if (!act.busy) { free = true; break }
            }
            if (free) runCatching { api.latestMessageId(threadId) }.getOrNull()
                ?.let { if (it.isNotEmpty()) lastMessageId = it }
            ui.post {
                if (free) {
                    toIdle("Cancelled")
                } else if (phase == Phase.THINKING || phase == Phase.STREAMING) {
                    stopping = true
                    talk.text = "\u00b7\u00b7\u00b7"
                    talk.isEnabled = false
                    status.text = "Still busy — the current command " +
                                  "must finish first\u2026"
                }
            }
          } catch (e: Exception) {
            // Cancel call itself failed — stand back down rather than
            // wedging the UI with input locked.
            Log.w(TAG, "confirmStopThread failed: ${e.message}")
            ui.post { toIdle("Stop failed — try again") }
          }
        }
    }

    /**
     * Agent-level stop that CONFIRMS (the busy turn may live in another
     * thread, where a thread cancel is a no-op). Polls the roster
     * (bounded) for the agent to go truly idle; on timeout [stopping]
     * locks input and the idle watch stands the UI down when the agent
     * actually finishes.
     */
    private fun stopAgentAndConfirm() {
      try {
        api.cancelAgent(agentId)
        val deadline = System.currentTimeMillis() + STOP_CONFIRM_MS
        var free = false
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(500)
            free = runCatching {
                api.listAgents().firstOrNull { it.id == agentId }?.busy != true
            }.getOrDefault(false)
            if (free) break
        }
        ui.post {
            if (free) {
                showingAgentBusy = false
                toIdle("Stopped")
            } else if (phase == Phase.IDLE) {
                stopping = true
                talk.isEnabled = false
                status.text = "Still busy — the current command " +
                              "must finish first\u2026"
            }
        }
      } catch (e: Exception) {
        Log.w(TAG, "stopAgentAndConfirm failed: ${e.message}")
        ui.post { toIdle("Stop failed — try again") }
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
        // Re-arm the idle watch (removeCallbacks first — onResume also
        // fires at creation, before openThread's own first post).
        ui.removeCallbacks(inboxWatch)
        ui.postDelayed(inboxWatch, IDLE_POLL_MS)
    }

    override fun onPause() {
        sphere.onPause()
        // Stop the idle watch entirely: no pocket audio from queued
        // announcements or inbox narration while backgrounded.
        ui.removeCallbacks(inboxWatch)
        // Backgrounding cancels ANY active turn: LISTENING/STREAMING were
        // always cancelled, but a THINKING turn's reply would arrive and
        // start SPEAKING while the app is in the background (pocket audio).
        if (phase == Phase.LISTENING || phase == Phase.STREAMING ||
            phase == Phase.THINKING) cancelCurrent()
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
