package com.hivemind.voice

import android.animation.Keyframe
import android.animation.ObjectAnimator
import android.animation.PropertyValuesHolder
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
import java.util.concurrent.Executors

/** One worker group in the roster: header info plus its agents. */
private data class Group(
    val worker: String,
    val online: Boolean,
    val agents: List<ApiClient.Agent>,
)

/** Agent picker — agents grouped by hive-worker; offline workers collapse. */
class MainActivity : Activity() {

    companion object {
        private const val TAG = "HiveVoice"
        private const val POLL_MS = 2_500L   // roster/state refresh
    }

    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var list: ListView
    private lateinit var tts: TtsEngine
    private var adapter: RosterAdapter? = null
    private lateinit var api: ApiClient

    /** Edge-triggered state announcements (see AgentStateAnnouncer) — on
     *  the selection page there is no session agent, so every agent's
     *  edges speak, each in ITS OWN configured voice. */
    private val stateAnnouncer = AgentStateAnnouncer(
        enabled = { Prefs.stateAnnounce(this) },
        suppressedAgentId = { null },
    )

    /** One flattened list entry: a worker group header or an agent card. */
    sealed interface Row {
        data class Header(
            val worker: String, val online: Boolean,
            val expanded: Boolean, val count: Int,
        ) : Row

        data class AgentRow(val agent: ApiClient.Agent, val online: Boolean) : Row
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (!Prefs.isConfigured(this)) {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.statusText)
        list = findViewById(R.id.agentList)
        tts = TtsEngine(this, onQueueEmpty = { /* nothing to resume */ })
        // Warm the on-device pocket-tts engine at app start (process-wide
        // singleton): VoiceActivity/picker then get instant local voices;
        // state announcements on this screen use it too.
        Thread { tts.initLocal(applicationContext) }.start()
        // Feature on but permission missing → the grant popup, once per
        // app start (the Settings toggle also triggers it).
        if (Prefs.notifyAnnounce(this))
            NotifyListenerService.ensureGranted(this)
        findViewById<Button>(R.id.settingsButton).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.agentsButton).apply {
            setTextColor(getColor(R.color.cyan))
            compoundDrawables[2]?.mutate()?.setTint(getColor(R.color.cyan))
        }
        findViewById<Button>(R.id.inboxButton).apply {
            setTextColor(getColor(R.color.dim))
            compoundDrawables[2]?.mutate()?.setTint(getColor(R.color.dim))
            setOnClickListener {
                startActivity(Intent(this@MainActivity, InboxActivity::class.java))
            }
        }
        list.setOnItemClickListener { _, _, position, _ ->
            when (val row = adapter?.getItem(position) ?: return@setOnItemClickListener) {
                is Row.Header -> adapter?.toggle(row.worker)
                is Row.AgentRow -> {
                    // Agents on an offline worker can never answer — dead tap.
                    if (!row.online) return@setOnItemClickListener
                    startActivity(Intent(this, VoiceActivity::class.java).apply {
                        putExtra(VoiceActivity.EXTRA_AGENT_ID, row.agent.id)
                        putExtra(VoiceActivity.EXTRA_AGENT_NAME, row.agent.name)
                        putExtra(VoiceActivity.EXTRA_AGENT_AVATAR, row.agent.avatar)
                    })
                }
            }
        }
        // Long-press a card: per-agent voice picker.
        list.setOnItemLongClickListener { _, _, position, _ ->
            val row = adapter?.getItem(position) as? Row.AgentRow
                ?: return@setOnItemLongClickListener false
            if (!row.online) return@setOnItemLongClickListener false
            startActivity(Intent(this, VoicePickerActivity::class.java).apply {
                putExtra(VoicePickerActivity.EXTRA_AGENT_ID, row.agent.id)
                putExtra(VoicePickerActivity.EXTRA_AGENT_NAME, row.agent.name)
            })
            true
        }
    }

    override fun onResume() {
        super.onResume()
        if (!Prefs.isConfigured(this)) return
        api = ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
        if (adapter == null) loadAgents()
        // removeCallbacks first: onResume also fires at creation.
        ui.removeCallbacks(rosterPoll)
        ui.postDelayed(rosterPoll, POLL_MS)
    }

    override fun onPause() {
        // No pocket audio from state announcements while backgrounded.
        ui.removeCallbacks(rosterPoll)
        tts.stop()
        super.onPause()
    }

    /** Live roster + state watch: refresh the list in place and speak
     *  edge-triggered state transitions (dwell-filtered, silent
     *  baseline — see AgentStateAnnouncer). Runs only in the
     *  foreground; failures retry quietly at the next tick. */
    private val rosterPoll = object : Runnable {
        override fun run() {
            io.execute {
                try {
                    val workers = api.listWorkers()
                    val roster = api.listAgents()
                    val anns = stateAnnouncer.diff(roster)
                    ui.post {
                        adapter?.update(roster, workers)
                        if (adapter == null) loadAgents()
                        for (ann in anns) {
                            // The agent's OWN voice where one is picked —
                            // you can tell WHO changed state without
                            // looking at the screen.
                            tts.applyVoice(
                                Prefs.voiceFor(this@MainActivity, ann.agentId)
                                    .ifEmpty { null })
                            tts.speakQueued(ann.text)
                        }
                        ui.postDelayed(this, POLL_MS)
                    }
                } catch (e: Exception) {
                    ui.post {
                        status.text = "Hub error: ${e.message}"
                        ui.postDelayed(this, POLL_MS)
                    }
                }
            }
        }
    }

    private fun loadAgents() {
        status.text = "Loading agents…"
        val api = ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this))
        io.execute {
            try {
                val workers = api.listWorkers()
                val roster = api.listAgents()
                ui.post {
                    adapter = RosterAdapter(roster, workers)
                    list.adapter = adapter
                    status.text = if (roster.isEmpty()) "No agents on the hub"
                    else "Tap to talk · long-press for voice"
                }
            } catch (e: Exception) {
                ui.post {
                    status.text = "Hub error: ${e.message}\n(check URL/key in ⚙ settings)"
                }
            }
        }
    }

    /**
     * Grouped roster: one header per worker, agent cards beneath it.
     * Online groups sort first and start expanded; offline groups sort
     * last, start collapsed, and render greyed out.
     */
    private inner class RosterAdapter(
        agents: List<ApiClient.Agent>,
        workers: List<ApiClient.Worker>,
    ) : BaseAdapter() {

        private var groups: List<Group>
        private val collapsed = mutableSetOf<String>()
        private var rows: List<Row> = emptyList()

        init {
            val onlineByName = workers.associate { it.name to it.online }
            groups = agents.groupBy { it.worker.ifEmpty { "hub" } }
                .map { (w, list) ->
                    // An agent homed on a worker that isn't enrolled any
                    // more is treated as offline — it can't be running.
                    Group(w, onlineByName[w] ?: false, list)
                }
                .sortedWith(compareByDescending<Group> { it.online }
                    .thenBy { it.worker })
            collapsed += groups.filter { !it.online }.map { it.worker }
            rebuildRows()
        }

        fun toggle(worker: String) {
            if (!collapsed.remove(worker)) collapsed += worker
            rebuildRows()
            notifyDataSetChanged()
        }

        /** In-place refresh from a fresh poll: rebuild groups/rows but
         *  keep the user's collapse choices and scroll position. */
        fun update(agents: List<ApiClient.Agent>, workers: List<ApiClient.Worker>) {
            val onlineByName = workers.associate { it.name to it.online }
            groups = agents.groupBy { it.worker.ifEmpty { "hub" } }
                .map { (w, list) -> Group(w, onlineByName[w] ?: false, list) }
                .sortedWith(compareByDescending<Group> { it.online }
                    .thenBy { it.worker })
            // A worker that came back online re-expands; gone workers
            // stay collapsed.
            collapsed += groups.filter { !it.online }.map { it.worker }
            rebuildRows()
            notifyDataSetChanged()
        }

        private fun rebuildRows() {
            rows = buildList {
                for (g in groups) {
                    val expanded = g.worker !in collapsed
                    add(Row.Header(g.worker, g.online, expanded, g.agents.size))
                    if (expanded) {
                        for (a in g.agents) add(Row.AgentRow(a, g.online))
                    }
                }
            }
        }

        override fun getCount() = rows.size
        override fun getItem(position: Int) = rows[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getViewTypeCount() = 2
        override fun getItemViewType(position: Int) =
            if (rows[position] is Row.Header) 0 else 1
        // Headers intercept taps for expand/collapse; offline agent cards
        // are visible but inert.
        override fun isEnabled(position: Int) =
            when (val r = rows[position]) {
                is Row.Header -> true
                is Row.AgentRow -> r.online
            }

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            return when (val row = rows[position]) {
                is Row.Header -> headerView(row, convertView, parent)
                is Row.AgentRow -> agentView(row, convertView, parent)
            }
        }

        private fun headerView(row: Row.Header, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: LayoutInflater.from(this@MainActivity)
                .inflate(R.layout.worker_header, parent, false)
            val chevron = v.findViewById<TextView>(R.id.workerChevron)
            val name = v.findViewById<TextView>(R.id.workerName)
            val state = v.findViewById<TextView>(R.id.workerState)
            chevron.text = if (row.expanded) "▾" else "▸"
            name.text = "${row.worker} · ${row.count}"
            if (row.online) {
                state.text = "● online"
                state.setTextColor(0xFF3DDC84.toInt())
                name.setTextColor(0xFF33D6E8.toInt())
                v.alpha = 1.0f
            } else {
                state.text = "○ offline"
                state.setTextColor(0xFF7A8A99.toInt())
                name.setTextColor(0xFF7A8A99.toInt())
                v.alpha = 0.55f
            }
            return v
        }

        private fun agentView(row: Row.AgentRow, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: LayoutInflater.from(this@MainActivity)
                .inflate(R.layout.agent_row, parent, false)
            val a = row.agent
            val avatar = v.findViewById<ImageView>(R.id.avatarImage)
            avatar.setImageBitmap(Avatars.bitmap(this@MainActivity, a.name, a.avatar))
            // Heartbeat: a working agent's avatar visibly pulses — the
            // state pill alone was too subtle to notice at a glance.
            pulseAvatar(avatar, row.online && a.busy)
            v.findViewById<TextView>(R.id.agentName).text = a.name
            v.findViewById<TextView>(R.id.agentRole).text =
                a.role.lineSequence().firstOrNull().orEmpty()
            val state = v.findViewById<TextView>(R.id.agentState)
            when {
                !row.online -> {
                    state.text = "○ offline"
                    state.setTextColor(0xFF7A8A99.toInt())
                }
                a.busy -> {
                    state.text = "● busy"
                    state.setTextColor(0xFFFFB74D.toInt())
                }
                a.state == "ready" -> {
                    state.text = "● ready"
                    state.setTextColor(0xFF3DDC84.toInt())
                }
                a.state.isNotEmpty() -> {
                    state.text = "● ${a.state}"
                    state.setTextColor(0xFF7A8A99.toInt())
                }
                else -> {
                    state.text = ""
                    state.visibility = View.GONE
                }
            }
            if (state.text.isNotEmpty()) state.visibility = View.VISIBLE
            v.alpha = if (row.online) 1.0f else 0.45f
            return v
        }
    }

    /**
     * Heartbeat double-beat (lub-dub, then rest) on a busy agent's avatar.
     * The animator is tag-keyed on the ImageView so convertView reuse
     * always cancels the previous row's pulse before rebinding, and any
     * row that stops being busy lands back at scale 1 — a recycled view
     * can never carry a stale animation into a different agent.
     */
    private fun pulseAvatar(img: ImageView, on: Boolean) {
        val running = img.getTag(R.id.avatarImage) as? ObjectAnimator
        if (on == (running != null)) return
        if (running != null) {
            running.cancel()
            img.setTag(R.id.avatarImage, null)
            img.scaleX = 1f
            img.scaleY = 1f
            return
        }
        // Shared keyframes: quick strong beat, quick weak beat, rest.
        val beats = arrayOf(
            Keyframe.ofFloat(0.00f, 1.00f),
            Keyframe.ofFloat(0.12f, 1.14f),
            Keyframe.ofFloat(0.26f, 1.00f),
            Keyframe.ofFloat(0.38f, 1.09f),
            Keyframe.ofFloat(0.52f, 1.00f),
            Keyframe.ofFloat(1.00f, 1.00f),
        )
        val pulse = ObjectAnimator.ofPropertyValuesHolder(img,
            PropertyValuesHolder.ofKeyframe("scaleX", *beats),
            PropertyValuesHolder.ofKeyframe("scaleY", *beats),
        ).apply {
            duration = 1_100
            repeatCount = ObjectAnimator.INFINITE
            img.setTag(R.id.avatarImage, this)
            start()
        }
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        tts.destroy()
        io.shutdownNow()
        super.onDestroy()
    }
}
