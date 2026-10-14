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

    fun voiceFor(ctx: Context, agentId: String): String {
        val stored = ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getString(KEY_VOICE_PREFIX + agentId, "") ?: ""
        // Desktop hive-client-voice mapping (~/.config/hive-client-voice/
        // prefs.json, 2026-10-14) — pocket-tts voice names, so the phone
        // matches each agent's desktop voice out of the box. An explicit
        // picker selection (stored) always wins over the seed.
        return stored.ifEmpty { DESKTOP_VOICE_SEED[agentId] ?: "" }
    }

    private val DESKTOP_VOICE_SEED = mapOf(
        "1fd19a3c-367a-43da-a78e-8195277b9055" to "jean",
        "226eec73-adca-4fa5-a4b4-30579c323c01" to "eponine",
        "2c6644df-89db-4005-8837-57bc28325f00" to "charles",
        "40c8b597-ddc4-46f7-bc6e-56d424b141e7" to "peter_yearsley",
        "4557d008-f09e-416f-b8a1-2a47fbaa846e" to "bill_boerst",
        "4901ee3c-47e1-413d-86dc-44bfe02d3776" to "paul",
        "8e9d3ad9-8a59-41bc-9b5e-ef20f71cfb21" to "bill_boerst",
        "a6d8e41b-86cc-4b5e-ba14-a7ee1f6b2b0c" to "vera",
        "c47aeeaa-d6fb-4eeb-a211-21b852e6c4e8" to "anna",
        "c58516f8-ce2c-41be-b147-20ec687ef548" to "peter_yearsley",
        "da77d170-e569-44dc-a118-f74d4210001c" to "vera",
    )

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

    // Read other apps' notifications aloud (SMS, missed calls) — needs
    // the system Notification Access grant; the app prompts for it.
    // Default ON (2026-10-01): silent until the user grants, and the
    // Settings toggle / startup prompt route them there.
    private const val KEY_NOTIFY_ANNOUNCE = "notify_announce"

    fun notifyAnnounce(ctx: Context): Boolean =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getBoolean(KEY_NOTIFY_ANNOUNCE, true)

    fun setNotifyAnnounce(ctx: Context, on: Boolean) {
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_NOTIFY_ANNOUNCE, on)
            .apply()
    }

    // Which agent receives relayed phone notifications (Settings
    // picker). THREE states: "" = never chosen → the service resolves
    // the default agent (Scout, the PA) once and caches it; a real
    // agent id = explicit choice; OPT_OUT = the user picked
    // "(not shared)" — local announce only, never push.
    const val OPT_OUT = "-"
    private const val KEY_NOTIFY_AGENT = "notify_agent"

    fun notifyAgent(ctx: Context): String =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            .getString(KEY_NOTIFY_AGENT, "") ?: ""

    fun setNotifyAgent(ctx: Context, agentId: String) {
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putString(KEY_NOTIFY_AGENT, agentId)
            .apply()
    }
}
