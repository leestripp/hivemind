package com.hivemind.voice

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.EditText
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
