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
import java.util.concurrent.Executors

/** One worker group in the roster: header info plus its agents. */
private data class Group(
    val worker: String,
    val online: Boolean,
    val agents: List<ApiClient.Agent>,
)

/** Agent picker — agents grouped by hive-worker; offline workers collapse. */
class MainActivity : Activity() {

    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var list: ListView
    private var adapter: RosterAdapter? = null

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
        findViewById<Button>(R.id.settingsButton).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
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
        if (Prefs.isConfigured(this)) loadAgents()
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

        private val groups: List<Group>
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
            v.findViewById<ImageView>(R.id.avatarImage)
                .setImageBitmap(Avatars.bitmap(this@MainActivity, a.name, a.avatar))
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

    override fun onDestroy() {
        io.shutdownNow()
        super.onDestroy()
    }
}
