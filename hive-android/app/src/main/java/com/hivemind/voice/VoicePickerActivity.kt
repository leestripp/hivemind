package com.hivemind.voice

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ListView
import android.widget.TextView

/**
 * Per-agent voice picker (long-press an agent card to open).
 *
 *   single tap  → sample the voice ("Hi, I'm PA — this is my voice.")
 *   double tap  → select it for that agent (Prefs; applied in sessions)
 *
 * English embedded voices only (offline, quality 400).
 */
class VoicePickerActivity : Activity() {

    companion object {
        const val EXTRA_AGENT_ID = "agent_id"
        const val EXTRA_AGENT_NAME = "agent_name"
    }

    private val ui = Handler(Looper.getMainLooper())
    private lateinit var tts: TtsEngine
    private lateinit var status: TextView
    private lateinit var list: ListView
    private var agentId = ""
    private var agentName = ""
    private var options: List<TtsEngine.VoiceOption> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_voice_picker)

        agentId = intent.getStringExtra(EXTRA_AGENT_ID).orEmpty()
        agentName = intent.getStringExtra(EXTRA_AGENT_NAME).orEmpty()
        findViewById<TextView>(R.id.titleText).text = "Voice for $agentName"
        status = findViewById(R.id.statusText)
        list = findViewById(R.id.voiceList)

        tts = TtsEngine(this, onQueueEmpty = {}, onReady = {
            ui.post { populate() }
        }, onInitFailed = {
            status.text = "No text-to-speech engine on this device"
        })
        status.text = "Loading voices…"
    }

    private fun populate() {
        options = tts.englishVoiceOptions()
        list.adapter = VoiceAdapter()
        status.text = "${options.size} voices — tap to sample, double-tap to select"
    }

    private fun select(opt: TtsEngine.VoiceOption) {
        Prefs.setVoice(this, agentId, opt.name)
        status.text = "✓ ${opt.label} — voice set for $agentName"
        ui.postDelayed({ finish() }, 900)
    }

    private fun sample(opt: TtsEngine.VoiceOption) {
        tts.setVoiceByName(opt.name)
        tts.preview("Hi, I'm $agentName — this is my voice.")
        status.text = "Sampling ${opt.label}…"
    }

    private inner class VoiceAdapter : BaseAdapter() {
        override fun getCount() = options.size
        override fun getItem(position: Int) = options[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: LayoutInflater.from(this@VoicePickerActivity)
                .inflate(R.layout.voice_row, parent, false)
            val opt = options[position]
            v.findViewById<TextView>(R.id.voiceLabel).text = opt.label
            // Current selection marker.
            v.findViewById<TextView>(R.id.voiceCheck).visibility =
                if (opt.name == Prefs.voiceFor(this@VoicePickerActivity, agentId))
                    View.VISIBLE else View.GONE

            val gd = GestureDetector(this@VoicePickerActivity,
                object : GestureDetector.SimpleOnGestureListener() {
                    override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                        sample(opt); return true
                    }
                    override fun onDoubleTap(e: MotionEvent): Boolean {
                        select(opt); return true
                    }
                })
            // Return the detector's verdict, NOT always true: consuming
            // every MotionEvent made the ListView unable to scroll.
            v.setOnTouchListener { _, ev -> gd.onTouchEvent(ev) }
            return v
        }
    }

    override fun onDestroy() {
        tts.destroy()
        super.onDestroy()
    }
}
