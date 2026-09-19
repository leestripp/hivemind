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
}
