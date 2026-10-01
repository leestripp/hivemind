package com.hivemind.voice

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import java.util.concurrent.Executors

/** Hub URL + API key entry. The hub is CONTACTED on save — a typo'd
 *  URL or key used to save silently and strand every other screen on a
 *  dead connection. Saved to SharedPreferences only on success. */
class SettingsActivity : Activity() {

    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        val url = findViewById<EditText>(R.id.urlEdit)
        val key = findViewById<EditText>(R.id.keyEdit)
        val save = findViewById<Button>(R.id.saveButton)
        val announce = findViewById<CheckBox>(R.id.announceCheck)
        announce.isChecked = Prefs.stateAnnounce(this)
        announce.setOnCheckedChangeListener { _, on ->
            Prefs.setStateAnnounce(this, on)
        }
        val notify = findViewById<CheckBox>(R.id.notifyCheck)
        notify.isChecked = Prefs.notifyAnnounce(this)
        notify.setOnCheckedChangeListener { _, on ->
            Prefs.setNotifyAnnounce(this, on)
            // Built-in grant popup: enabling without the permission
            // explains + routes to the system toggle screen.
            if (on) NotifyListenerService.ensureGranted(this)
        }
        // Which agent receives the relayed notifications (picker is
        // roster-populated; "(not shared)" = local announce only).
        val spinner = findViewById<Spinner>(R.id.notifyAgentSpinner)
        io.execute {
            val agents = try {
                ApiClient(Prefs.hubUrl(this), Prefs.apiKey(this)).listAgents()
            } catch (e: Exception) {
                emptyList()
            }
            ui.post {
                val names = mutableListOf("(not shared)")
                val ids = mutableListOf("")
                for (a in agents) { names.add(a.name); ids.add(a.id) }
                spinner.adapter = ArrayAdapter(
                    this, android.R.layout.simple_spinner_dropdown_item, names)
                val sel = ids.indexOf(Prefs.notifyAgent(this))
                spinner.setSelection(if (sel >= 0) sel else 0)
                spinner.onItemSelectedListener =
                    object : AdapterView.OnItemSelectedListener {
                        override fun onItemSelected(
                            parent: AdapterView<*>?, view: View?, pos: Int, rowId: Long) {
                            Prefs.setNotifyAgent(this@SettingsActivity, ids[pos])
                        }
                        override fun onNothingSelected(parent: AdapterView<*>?) {}
                    }
            }
        }
        val status = findViewById<TextView>(R.id.statusText)
        url.setText(Prefs.hubUrl(this))
        key.setText(Prefs.apiKey(this))

        save.setOnClickListener {
            val u = url.text.toString().trim().trimEnd('/')
            val k = key.text.toString().trim()
            if (u.isEmpty() || k.isEmpty()) {
                status.text = "Both the hub URL and an API key are required"
                return@setOnClickListener
            }
            if (!u.startsWith("https://") && !u.startsWith("http://")) {
                status.text = "The URL must start with https:// (or http:// for a LAN hub)"
                return@setOnClickListener
            }
            save.isEnabled = false
            status.text = "Testing the connection…"
            io.execute {
                val result = runCatching {
                    ApiClient(u, k).listAgents().size
                }
                ui.post {
                    save.isEnabled = true
                    result.onSuccess { n ->
                        Prefs.save(this, u, k)
                        status.text = "✓ Connected — $n agents"
                        ui.postDelayed({ finish() }, 700)
                    }.onFailure { e ->
                        status.text = "Couldn't reach the hub: ${e.message}\n(not saved)"
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        io.shutdownNow()
        super.onDestroy()
    }
}
