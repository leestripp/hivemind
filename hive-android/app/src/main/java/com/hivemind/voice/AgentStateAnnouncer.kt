package com.hivemind.voice

import android.util.Log

/**
 * Edge-triggered agent-state announcer, shared by MainActivity (the
 * roster/selection page) and VoiceActivity (in-chat).
 *
 * [diff] collapses each agent's hub state to a coarse bucket and emits
 * an [Announcement] only when the bucket CHANGES and the new state has
 * held [STATE_DWELL_MS] across polls — one utterance per real edge, so
 * a user who is not looking at the screen is never spammed:
 *
 *  - first sight of an agent is a SILENT baseline (opening the app
 *    never announces the whole roster);
 *  - a transition must dwell before it speaks — flicker back to the
 *    previous state cancels the pending note;
 *  - [suppressedAgentId]'s routine working/finished edges are skipped
 *    (in-chat, the reply stream already narrates that agent) while its
 *    actionable states (error/offline) always speak.
 *
 * All state lives in this class; activities own timing, TTS and voices.
 */
class AgentStateAnnouncer(
    private val enabled: () -> Boolean,
    private val suppressedAgentId: () -> String?,
) {

    data class Announcement(val agentId: String, val text: String)

    companion object {
        private const val TAG = "HiveVoice"

        /** A new state must hold this long (~2 polls) before it speaks. */
        const val STATE_DWELL_MS = 5_000L
    }

    private val lastAnnounced = HashMap<String, String>()
    private val pending = HashMap<String, Pair<String, Long>>()

    /** Forget every baseline — the next [diff] is silent again. */
    fun reset() {
        lastAnnounced.clear()
        pending.clear()
    }

    /** Collapse the hub's agent states to the buckets worth speaking.
     *  null = not trackable ("starting" flicker, no state yet). */
    private fun bucket(a: ApiClient.Agent): String? = when {
        a.busy || a.state == "acting" || a.state == "working" -> "working"
        a.state == "error"          -> "error"
        a.state == "down"           -> "down"
        a.state == "starting" || a.state.isEmpty() -> null
        else                        -> "idle"   // ready, idle, …
    }

    /** Spoken text for a dwelled transition, or null = not worth a note. */
    private fun announceText(name: String, from: String, to: String): String? =
        when {
            to == "working" && from != "working" -> "$name started working."
            to == "idle" && from == "working"    -> "$name finished."
            to == "error"    -> "$name hit an error."
            to == "down"     -> "$name went offline."
            else             -> null
        }

    /**
     * Diff one roster snapshot. Returns the announcements that completed
     * their dwell this tick (usually empty). Safe to call from any
     * single thread.
     */
    fun diff(agents: List<ApiClient.Agent>,
             now: Long = System.currentTimeMillis()): List<Announcement> {
        val out = mutableListOf<Announcement>()
        for (a in agents) {
            val cur = bucket(a) ?: continue
            val prev = lastAnnounced[a.id]
            if (prev == null) {           // first sight = silent baseline
                lastAnnounced[a.id] = cur
                continue
            }
            if (prev == cur) {            // stable: cancel any pending note
                pending.remove(a.id)
                continue
            }
            val pend = pending[a.id]
            if (pend == null || pend.first != cur) {
                pending[a.id] = cur to now   // new edge: start dwelling
                continue
            }
            if (now - pend.second < STATE_DWELL_MS) continue
            // Dwelled: this edge is real — emit it once.
            pending.remove(a.id)
            lastAnnounced[a.id] = cur
            if (!enabled()) continue
            val routine = cur == "working" || cur == "idle"
            if (a.id == suppressedAgentId() && routine) continue
            announceText(a.name, prev, cur)?.let {
                Log.d(TAG, "state announce: ${a.name} $prev -> $cur")
                out += Announcement(a.id, it)
            }
        }
        return out
    }
}
