package com.hivemind.voice

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.EditText

/** Hub URL + API key entry. Saved to SharedPreferences. */
class SettingsActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        val url = findViewById<EditText>(R.id.urlEdit)
        val key = findViewById<EditText>(R.id.keyEdit)
        url.setText(Prefs.hubUrl(this))
        key.setText(Prefs.apiKey(this))

        findViewById<Button>(R.id.saveButton).setOnClickListener {
            Prefs.save(this, url.text.toString(), key.text.toString())
            finish()
        }
    }
}
