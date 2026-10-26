package com.hivemind.voice

import android.content.Context
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import android.util.Log
import java.util.concurrent.Executors

/**
 * Queued-utterance TextToSpeech wrapper for streamed replies.
 *
 * VoiceActivity feeds it sentence chunks AS THE AGENT STREAMS THEM
 * ([speakQueued] → QUEUE_ADD), so the user hears the reply from its
 * first sentence instead of waiting for the full turn. [onQueueEmpty]
 * fires once when every queued chunk has finished playing — the signal
 * to return to idle. Chunks queued before the engine is ready are held
 * and flushed on init.
 */
class TtsEngine(
    context: Context,
    private val onQueueEmpty: () -> Unit,
    private val onReady: (() -> Unit)? = null,
    private val onInitFailed: ((Int) -> Unit)? = null,
) : UtteranceProgressListener() {

    /** Applied once the engine is ready (per-agent voice selection). */
    var preferredVoiceName: String? = null

    private var tts: TextToSpeech? = null
    @Volatile private var ready = false
    @Volatile var speaking = false
        private set
    @Volatile var outstanding = 0
        private set
    private var seq = 0

    /** Push-to-talk hold (2026-10-14): while held, chunks queue in
     *  [heldChunks], NOT the OS engine — agents never talk over the
     *  user's recording. Engaging mid-utterance re-queues the playing
     *  sentence at the front (Android TTS has no pause/resume, and its
     *  stop() discards the WHOLE engine queue, so every live
     *  utterance's text is kept in [texts] for the rebuild). */
    @Volatile private var held = false
    private val heldChunks = ArrayDeque<String>()
    private var currentId: String? = null
    private val texts = mutableMapOf<String, String>()
    /** Utterances with seq below this were killed by [stop] — their late
     *  callbacks must not touch speaking/outstanding (already accounted),
     *  or they flicker the sphere mid-next-reply and double-fire
     *  onQueueEmpty. */
    @Volatile private var epochBase = 0
    private val pending = mutableListOf<String>()

    /** On-device pocket-tts backend (audio.cpp — the SAME engine and
     *  voices as desktop hive-client-voice, so agents sound identical
     *  on both). When [pocketOk], speakQueued synthesizes chunks on
     *  [synthExec] and plays PCM through PcmPlayer, which gives TRUE
     *  mid-word hold (AudioTrack.pause keeps position) — the system-TTS
     *  path below stays as the fallback when the model can't load. */
    private val synthExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "hive-tts-synth")
    }
    private var player: PcmPlayer? = null
    @Volatile private var pocketOk = false
    @Volatile private var pocketVoice: String? = null
    @Volatile private var rms = 0f
    private val ui = android.os.Handler(android.os.Looper.getMainLooper())

    init {
        TextToSpeech(context) { status ->
            if (status == TextToSpeech.SUCCESS) {
                tts?.setOnUtteranceProgressListener(this)
                Log.i(TAG, "voices available: ${tts?.voices?.size}")
                // Name list for diagnosing per-agent voice selection on a
                // given device (names differ across engines/OEMs).
                tts?.voices
                    ?.filter { it.locale.language == "en" && !it.isNetworkConnectionRequired }
                    ?.forEach { Log.i(TAG, "en voice: ${it.name}") }
                ready = true
                preferredVoiceName?.let { setVoiceByName(it) }
                val p = pending.toList()
                pending.clear()
                p.forEach { speakQueued(it) }
                onReady?.invoke()
            } else {
                // No usable engine on this device — without this the
                // session would swallow every reply into `pending` and
                // stay SILENT forever with no explanation.
                Log.e(TAG, "TTS init failed, status=$status")
                onInitFailed?.invoke(status)
            }
        }.also { tts = it }
    }

    /** Load the pocket-tts model. BLOCKING (~1-4s) — bg thread only.
     *  Safe to call once at startup; failure leaves the system-TTS
     *  fallback active. */
    fun initLocal(context: Context) {
        if (!PocketTts.init(context)) return
        pocketOk = true
        (preferredVoiceName ?: pocketVoice)?.let {
            if (it in PocketTts.VOICES) pocketVoice = it
        }
        player = PcmPlayer(PocketTts.sampleRate,
            onChunkPlayed = { counted -> if (counted) chunkDone() },
            onLevel = { rms = it })
        Log.i(TAG, "pocket-tts active (voice=${pocketVoice ?: "alba"})")
    }

    /** Queue a chunk for speech. Call on the main thread. */
    fun speakQueued(text: String) {
        if (pocketOk) { pocketSpeak(text); return }
        val engine = tts
        if (!ready || engine == null) {
            if (pending.size < 20) pending += text  // never-ready cap
            return
        }
        outstanding++
        if (held) {
            heldChunks += text
            Log.d(TAG, "queue +${text.length} chars HELD (out=$outstanding)")
            return
        }
        speakNow(engine, text)
    }

    /** Hand one chunk to the OS engine (no accounting — callers own
     *  outstanding; the flush path re-speaks already-counted chunks). */
    private fun speakNow(engine: TextToSpeech, text: String) {
        val id = "$UTTER_PREFIX${seq++}"
        texts[id] = text
        Log.d(TAG, "queue +${text.length} chars (out=$outstanding)")
        engine.speak(text, TextToSpeech.QUEUE_ADD, null, id)
    }

    /** Pocket path: synthesize on the synth thread, hand PCM to the
     *  player. Chunks synthesized DURING a hold still queue (player is
     *  paused) — no rebuild dance needed. Synth failure falls back to
     *  the system engine for that chunk rather than dropping speech. */
    private fun pocketSpeak(text: String) {
        outstanding++
        val epoch = epochBase
        speaking = true
        Log.d(TAG, "synth +${text.length} chars (out=$outstanding)")
        synthExec.execute {
            if (epoch != epochBase) return@execute   // stop() already zeroed
            val pcm = PocketTts.synthesize(text, pocketVoice ?: "alba")
            if (pcm != null && epoch == epochBase) {
                player?.enqueue(pcm, counted = true)
            } else if (pcm != null) {
                // stop() raced the synth — drop the PCM, count stays zeroed
            } else {
                Log.w(TAG, "pocket synth failed — system TTS speaks this chunk")
                ui.post {
                    if (epoch != epochBase) return@post
                    outstanding--                 // undo pocket count…
                    val engine = tts
                    if (ready && engine != null) {
                        outstanding++             // …system count instead
                        speakNow(engine, text)    // its callbacks decrement
                    } else {
                        outstanding = 0
                        speaking = false
                        onQueueEmpty()
                    }
                }
            }
        }
    }

    /** A pocket chunk finished PLAYING (PcmPlayer writer thread). */
    private fun chunkDone() {
        if (outstanding > 0) outstanding--
        Log.d(TAG, "chunk played (out=$outstanding)")
        if (outstanding == 0) {
            speaking = false
            onQueueEmpty()
        }
    }

    /** Hold/resume playback for push-to-talk. Call on the main thread. */
    fun setHeld(h: Boolean) {
        if (pocketOk) {
            if (held == h) return
            held = h
            if (h) player?.pause() else player?.resume()
            Log.i(TAG, "pocket setHeld($h) — playback ${if (h) "paused mid-word" else "resumed"}")
            return
        }
        Log.i(TAG, "setHeld($h) — held=$held live=${texts.size}")
        if (held == h) return
        held = h
        if (h) {
            // Rebuild everything the OS engine is holding (playing +
            // queued) into heldChunks, in seq order, playing first —
            // engine.stop() below discards it all.
            val live = texts.entries
                .sortedBy { it.key.removePrefix(UTTER_PREFIX).toIntOrNull() ?: 0 }
            val playing = currentId
            for (e in live) {
                if (e.key == playing) heldChunks.addFirst(e.value)
                else heldChunks.addLast(e.value)
            }
            texts.clear()
            currentId = null
            speaking = false
            epochBase = seq  // late callbacks for the discarded utterances are stale
            tts?.stop()
            if (live.isNotEmpty())
                Log.i(TAG, "held: ${live.size} utterance(s) parked for recording")
        } else if (heldChunks.isNotEmpty()) {
            val engine = tts
            val chunks = heldChunks.toList()
            heldChunks.clear()
            Log.i(TAG, "released: ${chunks.size} held chunk(s)")
            if (engine != null && ready) chunks.forEach { speakNow(engine, it) }
            else pending.addAll(chunks)  // engine died mid-hold: pre-ready list
        }
    }

    data class VoiceOption(val name: String, val label: String)

    /** English EMBEDDED voices for the picker — network twins excluded. */
    fun englishVoiceOptions(): List<VoiceOption> {
        val engine = tts ?: return emptyList()
        return (engine.voices ?: emptySet())
            .filter {
                it.locale.language == "en" &&
                it.name.endsWith("-local") &&
                !it.isNetworkConnectionRequired
            }
            .sortedWith(compareBy({ it.locale.toString() }, { it.name }))
            .map { v ->
                // "en-us-x-tpf-local" -> "TPF"
                val code = v.name.split('-').dropLast(1).last().uppercase()
                VoiceOption(v.name, "${v.locale.displayCountry} · $code")
            }
    }

    /**
     * Switch voices BETWEEN utterances (per-sender speech): null/empty
     * resets to the engine default. Pre-ready the name is stored and
     * applied on init; ready it applies immediately and only affects
     * utterances QUEUED AFTER this call — the one playing is untouched.
     */
    fun applyVoice(name: String?) {
        if (name != null && name in PocketTts.VOICES) {
            pocketVoice = name
            if (pocketOk) { Log.i(TAG, "pocket voice set to $name"); return }
        }
        if (!ready) { preferredVoiceName = name; return }
        if (name.isNullOrEmpty()) {
            tts?.let { it.voice = it.defaultVoice }
            return
        }
        setVoiceByName(name)
    }

    /** Select a voice by engine name ("en-us-x-tpf-local"). */
    fun setVoiceByName(name: String): Boolean {
        val engine = tts ?: return false
        val match = engine.voices?.firstOrNull { it.name == name } ?: return false
        engine.voice = match
        Log.i(TAG, "voice set to $name")
        return true
    }

    /** One-off sample playback (voice picker); interrupts any prior sample. */
    fun preview(text: String) {
        if (pocketOk) {
            val v = pocketVoice ?: "alba"
            synthExec.execute {
                val pcm = PocketTts.synthesize(text, v) ?: return@execute
                val p = player ?: return@execute
                p.stop()                       // cut the prior sample
                p.enqueue(pcm, counted = false)
            }
            return
        }
        val engine = tts
        if (!ready || engine == null) return
        engine.speak(text, TextToSpeech.QUEUE_FLUSH, null, "${UTTER_PREFIX}preview")
    }

    /** Interrupt: drop everything queued + playing. */
    fun stop() {
        pending.clear()
        heldChunks.clear()
        texts.clear()
        currentId = null
        outstanding = 0
        speaking = false
        // Advance BOTH counters: pocket synth does not consume utterance
        // ids, so epochBase = seq left its in-flight jobs valid after stop.
        // Advancing seq keeps new system utterances at/above the stale cutoff.
        epochBase = ++seq
        player?.stop()
        tts?.stop()
    }

    fun destroy() {
        player?.destroy()
        player = null
        synthExec.shutdown()
        tts?.stop()
        tts?.shutdown()
        tts = null
    }

    /** Animated 0..1 envelope for the sphere while the agent talks. */
    fun level(nowSec: Float): Float {
        if (pocketOk) return (rms * 2.2f).coerceIn(0f, 0.9f)
        if (!speaking) return 0f
        val base = 0.38f + 0.22f * kotlin.math.sin(nowSec * 7.3f) *
                kotlin.math.sin(nowSec * 2.1f + 1.3f)
        return base.coerceIn(0.08f, 0.75f)
    }

    override fun onStart(utteranceId: String?) {
        speaking = true
        currentId = utteranceId
    }

    override fun onDone(utteranceId: String?) {
        if (isPreStop(utteranceId)) return
        utteranceId?.let { texts.remove(it) }
        if (utteranceId == currentId) currentId = null
        if (utteranceId?.startsWith(UTTER_PREFIX) == true && outstanding > 0) {
            outstanding--
            Log.d(TAG, "done (out=$outstanding)")
        }
        if (outstanding == 0) {
            speaking = false
            onQueueEmpty()
        }
    }

    @Deprecated("deprecated in TTS API")
    override fun onError(utteranceId: String?) {
        if (isPreStop(utteranceId)) return
        utteranceId?.let { texts.remove(it) }
        if (utteranceId == currentId) currentId = null
        if (utteranceId?.startsWith(UTTER_PREFIX) == true && outstanding > 0) {
            outstanding--
        }
        if (outstanding == 0) {
            speaking = false
            onQueueEmpty()
        }
    }

    /** True for callbacks of utterances killed by a [stop] call. */
    private fun isPreStop(utteranceId: String?): Boolean {
        if (utteranceId?.startsWith(UTTER_PREFIX) != true) return false
        val n = utteranceId.removePrefix(UTTER_PREFIX).toIntOrNull()
            ?: return false   // e.g. "preview" — not sequenced
        return n < epochBase
    }

    companion object {
        private const val TAG = "HiveTts"
        private const val UTTER_PREFIX = "hive-"
    }
}
