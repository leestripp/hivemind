package com.hivemind.voice

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.TextView
import java.util.concurrent.Executors

/**
 * Voice session with one agent.
 *
 * Big round button: tap once to talk, tap again to send.
 * Small side button: cancel (discard the recording / stop the turn).
 *
 * IDLE ──tap──▶ LISTENING ──tap──▶ THINKING ──reply──▶ SPEAKING ──done──▶ IDLE
 *                  │                  │                  │ tap: cut off
 *                  └─────── cancel (✕) anywhere ───────┴───▶ IDLE   │
 *                                                                   ▼
 *                                                              LISTENING
 */
class VoiceActivity : Activity() {

    companion object {
        const val EXTRA_AGENT_ID = "agent_id"
        const val EXTRA_AGENT_NAME = "agent_name"
        private const val REQ_AUDIO = 42
        private const val POLL_MS = 700L
        private const val TURN_TIMEOUT_MS = 180_000L
    }

    private enum class Phase { SETUP, IDLE, LISTENING, THINKING, SPEAKING }

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
    private val replyParts = StringBuilder()
    private var turnDeadline = 0L
    private var speakAnimator: Runnable? = null
    private var lastStatusUpdate = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_voice)

        // A voice session must never let the screen time out: locking
        // pauses us, which cancels recording / kills the audio reply.
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        agentId = intent.getStringExtra(EXTRA_AGENT_ID).orEmpty()
        findViewById<TextView>(R.id.agentNameText).text =
            intent.getStringExtra(EXTRA_AGENT_NAME).orEmpty()

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
                    // alive — throttled: rapid TextView churn feeds
                    // accessibility services (TalkBack) event storms.
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
        tts = TtsEngine(this) { ui.post { if (phase == Phase.SPEAKING) toIdle("") } }

        talk.isEnabled = false
        status.text = "Connecting…"

        talk.setOnClickListener {
            when (phase) {
                Phase.IDLE -> maybeStartListening()
                Phase.LISTENING -> sendTranscript()
                Phase.SPEAKING -> interruptAndTalk()
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
                ui.post { toIdle("Tap TALK and speak") }
            } catch (e: Exception) {
                ui.post {
                    status.text = "Couldn't open conversation:\n${e.message}"
                }
            }
        }
    }

    // -- Phase transitions --------------------------------------------------

    private fun toIdle(message: String) {
        phase = Phase.IDLE
        replyParts.clear()
        sphere.setSphereState(SphereState.IDLE)
        sphere.setLevel(0f)
        stopSpeakAnimator()
        talk.text = "TALK"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = true
        cancel.visibility = View.INVISIBLE
        if (message.isNotEmpty()) status.text = message
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
        status.text = "Listening…"
        stt.start()
    }

    private fun sendTranscript() {
        stt.stop()
        sphere.setLevel(0f)
        val text = stt.transcript.trim()
        if (text.isEmpty()) {
            toIdle("Didn't catch that — tap TALK to retry")
            return
        }
        phase = Phase.THINKING
        sphere.setSphereState(SphereState.THINKING)
        talk.text = "···"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = false
        cancel.visibility = View.VISIBLE
        status.text = "Sending…"
        turnDeadline = System.currentTimeMillis() + TURN_TIMEOUT_MS

        io.execute {
            try {
                api.postMessage(threadId, SpeechText.OUTGOING_PREFIX + text)
                ui.post {
                    status.text = "Thinking…"
                    ui.postDelayed(pollTick, POLL_MS)
                }
            } catch (e: Exception) {
                ui.post { toIdle("Send failed: ${e.message}") }
            }
        }
    }

    private val pollTick = object : Runnable {
        override fun run() {
            if (phase != Phase.THINKING) return
            if (System.currentTimeMillis() > turnDeadline) {
                toIdle("Timed out waiting for the agent")
                return
            }
            io.execute {
                try {
                    val act = api.activity(threadId)
                    for (m in api.messagesAfter(threadId, lastMessageId)) {
                        lastMessageId = m.id
                        if (m.senderKind == "agent" && m.body.isNotBlank()) {
                            if (replyParts.isNotEmpty()) replyParts.append(' ')
                            replyParts.append(m.body.trim())
                        }
                    }
                    val err = act.error
                    when {
                        err.isNotEmpty() ->
                            ui.post { toIdle("Agent error: $err") }
                        !act.busy && replyParts.isNotEmpty() ->
                            ui.post { speakReply() }
                        else ->
                            ui.postDelayed(this, POLL_MS)
                    }
                } catch (e: Exception) {
                    // Transient hub/network blip — keep polling until deadline.
                    ui.postDelayed(this, POLL_MS)
                }
            }
        }
    }

    private fun speakReply() {
        val text = SpeechText.forSpeech(replyParts.toString())
        replyParts.clear()
        if (text.isEmpty()) { toIdle(""); return }
        phase = Phase.SPEAKING
        sphere.setSphereState(SphereState.SPEAKING)
        status.text = ""
        // Big button is live during the reply: tap to cut the agent off
        // and go straight back to recording.
        talk.text = "TALK"
        talk.setBackgroundResource(R.drawable.btn_talk)
        talk.isEnabled = true
        cancel.visibility = View.VISIBLE
        startSpeakAnimator()
        tts.speak(text)
    }

    /** While the agent talks, feed the sphere a synthetic voice envelope. */
    private fun startSpeakAnimator() {
        stopSpeakAnimator()
        val start = System.nanoTime()
        val r = object : Runnable {
            override fun run() {
                if (phase != Phase.SPEAKING) return
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

    /**
     * The engine stopped itself (hard error). Never strand the user:
     * a captured transcript stays sendable; with nothing captured we
     * just return to idle with the reason on screen.
     */
    private fun onEngineStopped(msg: String) {
        if (phase != Phase.LISTENING) return
        if (stt.transcript.isNotEmpty()) {
            status.text = "⚠ $msg — SEND keeps what you have, TALK restarts"
            // Phase stays LISTENING: SEND posts stt.transcript as-is.
        } else {
            toIdle(msg)
        }
    }

    /** Cut the agent's reply short and immediately start recording. */
    private fun interruptAndTalk() {
        tts.stop()
        io.execute { api.cancel(threadId) }
        toIdle("")
        maybeStartListening()
    }

    private fun cancelCurrent() {
        when (phase) {
            Phase.LISTENING -> stt.discard()
            Phase.THINKING -> io.execute { api.cancel(threadId) }
            Phase.SPEAKING -> {
                tts.stop()
                io.execute { api.cancel(threadId) }
            }
            else -> {}
        }
        if (phase != Phase.SETUP) toIdle("Cancelled")
    }

    // -- Permission ---------------------------------------------------------

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

    // -- Lifecycle ----------------------------------------------------------

    override fun onResume() {
        super.onResume()
        sphere.onResume()
    }

    override fun onPause() {
        sphere.onPause()
        if (phase == Phase.LISTENING || phase == Phase.SPEAKING) cancelCurrent()
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
