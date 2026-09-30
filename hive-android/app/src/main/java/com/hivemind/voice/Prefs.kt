package com.hivemind.voice

import android.content.Context

object Prefs {
    private const val FILE = "hive_voice"
    private const val KEY_URL = "hub_url"
    private const val KEY_API = "api_key"
    const val DEFAULT_URL = "https://leestrippai.com"

    fun hubUrl(ctx: Context): String =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getString(KEY_URL, DEFAULT_URL) ?: DEFAULT_URL

    fun apiKey(ctx: Context): String =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getString(KEY_API, "") ?: ""

    fun save(ctx: Context, url: String, key: String) {
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putString(KEY_URL, url.trim().trimEnd('/'))
            .putString(KEY_API, key.trim())
            .apply()
    }

    fun isConfigured(ctx: Context): Boolean = apiKey(ctx).isNotEmpty()

    // Per-agent TTS voice ("en-us-x-tpf-local"); "" = engine default.
    private const val KEY_VOICE_PREFIX = "voice_"

    fun voiceFor(ctx: Context, agentId: String): String =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getString(KEY_VOICE_PREFIX + agentId, "") ?: ""

    fun setVoice(ctx: Context, agentId: String, voiceName: String) {
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putString(KEY_VOICE_PREFIX + agentId, voiceName)
            .apply()
    }

    // Agent state-change announcements ("Forge started working.") — the
    // voice-first ambient awareness toggle. Default ON (2026-10-01).
    private const val KEY_STATE_ANNOUNCE = "state_announce"

    fun stateAnnounce(ctx: Context): Boolean =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getBoolean(KEY_STATE_ANNOUNCE, true)

    fun setStateAnnounce(ctx: Context, on: Boolean) {
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_STATE_ANNOUNCE, on)
            .apply()
    }
}
