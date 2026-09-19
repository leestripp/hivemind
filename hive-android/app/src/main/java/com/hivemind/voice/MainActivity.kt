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

/** Agent picker — tap an agent to open the voice session. */
class MainActivity : Activity() {

    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var list: ListView
    private var agents: List<ApiClient.Agent> = emptyList()

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
            val agent = agents.getOrNull(position) ?: return@setOnItemClickListener
            startActivity(Intent(this, VoiceActivity::class.java).apply {
                putExtra(VoiceActivity.EXTRA_AGENT_ID, agent.id)
                putExtra(VoiceActivity.EXTRA_AGENT_NAME, agent.name)
                putExtra(VoiceActivity.EXTRA_AGENT_AVATAR, agent.avatar)
            })
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
                val roster = api.listAgents()
                agents = roster
                ui.post {
                    list.adapter = AgentAdapter(roster)
                    status.text = if (roster.isEmpty()) "No agents on the hub"
                    else "Tap an agent to talk"
                }
            } catch (e: Exception) {
                ui.post {
                    status.text = "Hub error: ${e.message}\n(check URL/key in ⚙ settings)"
                }
            }
        }
    }

    /** Roster info cards: creature avatar, name, role, live-state pill. */
    private inner class AgentAdapter(
        private val agents: List<ApiClient.Agent>,
    ) : BaseAdapter() {
        override fun getCount() = agents.size
        override fun getItem(position: Int) = agents[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: LayoutInflater.from(this@MainActivity)
                .inflate(R.layout.agent_row, parent, false)
            val a = agents[position]
            v.findViewById<ImageView>(R.id.avatarImage)
                .setImageBitmap(Avatars.bitmap(this@MainActivity, a.name, a.avatar))
            v.findViewById<TextView>(R.id.agentName).text = a.name
            v.findViewById<TextView>(R.id.agentRole).text =
                a.role.lineSequence().firstOrNull().orEmpty()
            val state = v.findViewById<TextView>(R.id.agentState)
            when {
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
            return v
        }
    }

    override fun onDestroy() {
        io.shutdownNow()
        super.onDestroy()
    }
}
