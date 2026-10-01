package com.hivemind.voice

import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

class ApiException(val code: Int, message: String) : Exception("HTTP $code: $message")

/**
 * Minimal REST client for the hive-server hub. Blocking — call from a
 * background thread. Mirrors the endpoints documented in docs/api.md.
 */
class ApiClient(baseUrl: String, private val apiKey: String) {

    private val base = baseUrl.trimEnd('/')

    data class Agent(
        val id: String, val name: String, val role: String,
        val state: String, val busy: Boolean, val avatar: String,
        val worker: String
    )

    data class Worker(val name: String, val online: Boolean)

    data class Message(
        val id: String, val senderKind: String, val body: String,
        val senderId: String
    )

    data class Activity(val busy: Boolean, val error: String, val partial: String)

    private fun request(method: String, path: String, body: JSONObject? = null): String {
        val conn = (URL(base + path).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 10_000
            readTimeout = 20_000
            setRequestProperty("Authorization", "Bearer $apiKey")
            setRequestProperty("Accept", "application/json")
            if (body != null) {
                doOutput = true
                setRequestProperty("Content-Type", "application/json")
            }
        }
        try {
            if (body != null) {
                // Explicit UTF-8: the platform default is UTF-8 on Android
                // today, but a voice transcript is user speech — never let
                // a charset default corrupt it.
                OutputStreamWriter(conn.outputStream, Charsets.UTF_8)
                    .use { it.write(body.toString()) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader()?.use(BufferedReader::readText) ?: ""
            if (code !in 200..299) {
                val err = runCatching { JSONObject(text).optString("error", text) }
                    .getOrDefault(text)
                throw ApiException(code, err.ifEmpty { "request failed" })
            }
            return text
        } finally {
            conn.disconnect()
        }
    }

    /** GET /api/agents → roster with live state. */
    fun listAgents(): List<Agent> {
        val arr = JSONObject(request("GET", "/api/agents")).getJSONArray("agents")
        return buildList {
            for (i in 0 until arr.length()) {
                val a = arr.getJSONObject(i)
                add(Agent(
                    id = a.getString("id"),
                    name = a.getString("name"),
                    role = a.optString("role", ""),
                    state = a.optString("state", ""),
                    busy = a.optBoolean("busy", false),
                    avatar = a.optString("avatar", ""),
                    worker = a.optString("worker", ""),
                ))
            }
        }
    }

    /** GET /api/workers → enrolled workers with live online flag. */
    fun listWorkers(): List<Worker> {
        val arr = JSONObject(request("GET", "/api/workers")).getJSONArray("workers")
        return buildList {
            for (i in 0 until arr.length()) {
                val w = arr.getJSONObject(i)
                add(Worker(
                    name = w.getString("name"),
                    online = w.optBoolean("online", false),
                ))
            }
        }
    }

    /** POST /api/threads with reuse_dm — opens (or reopens) the 1:1. */
    fun openDm(agentId: String): String {
        val body = JSONObject()
            .put("agent_ids", JSONArray().put(agentId))
            .put("title", "Voice")
            .put("kind", "dm")
            .put("reuse_dm", true)
        return JSONObject(request("POST", "/api/threads", body)).getString("id")
    }

    /** POST /api/threads/:id/messages → message id (202 Accepted). */
    fun postMessage(threadId: String, text: String): String {
        val body = JSONObject().put("body", text)
        return JSONObject(request("POST", "/api/threads/$threadId/messages", body))
            .getString("message_id")
    }

    /** Phone notification → agent DM on the hub: silent system
     *  message, folded into the agent's context on the next user
     *  wake. Call from a worker thread (request() is blocking). */
    fun postNotification(agentId: String, app: String, title: String, text: String) {
        val body = JSONObject()
            .put("agent_id", agentId)
            .put("app", app)
            .put("title", title)
            .put("text", text)
        request("POST", "/api/notifications", body)
    }

    /** GET /api/threads/:id/messages?after= — incremental poll. */
    fun messagesAfter(threadId: String, afterId: String): List<Message> {
        val after = URLEncoder.encode(afterId, "UTF-8")
        val arr = JSONObject(request("GET", "/api/threads/$threadId/messages?after=$after"))
            .getJSONArray("messages")
        return parseMessages(arr)
    }

    /** Latest message id in the thread ("" when empty) — poll baseline. */
    fun latestMessageId(threadId: String): String {
        val obj = JSONObject(request("GET", "/api/threads/$threadId/messages?limit=1"))
        val arr = obj.getJSONArray("messages")
        if (arr.length() == 0) return ""
        return arr.getJSONObject(arr.length() - 1).getString("id")
    }

    /** GET /api/threads/:id/activity → busy + first error, if any. */
    fun activity(threadId: String): Activity {
        val obj = JSONObject(request("GET", "/api/threads/$threadId/activity"))
        var error = ""
        var partial = ""
        val turns = obj.optJSONArray("turns")
        if (turns != null) {
            for (i in 0 until turns.length()) {
                val t = turns.getJSONObject(i)
                val e = t.optString("error", "")
                if (e.isNotEmpty()) { error = e; break }
                if (partial.isEmpty()) partial = t.optString("partial", "")
            }
        }
        return Activity(busy = obj.optBoolean("busy", false), error = error,
                        partial = partial)
    }

    /** POST /api/threads/:id/cancel — best effort. */
    fun cancel(threadId: String) {
        runCatching { request("POST", "/api/threads/$threadId/cancel", JSONObject()) }
    }

    /**
     * POST /api/threads/:id/read — clear the hub's PERSISTED unread
     * mark (Phase 66c) once this app has consumed (spoken) the new
     * messages, or the desktop/web clients show phantom unread from
     * voice sessions. Best effort at the call site.
     */
    fun markRead(threadId: String) {
        request("POST", "/api/threads/$threadId/read", JSONObject())
    }

    private fun parseMessages(arr: JSONArray): List<Message> = buildList {
        for (i in 0 until arr.length()) {
            val m = arr.getJSONObject(i)
            add(Message(
                id = m.getString("id"),
                senderKind = m.optString("sender_kind", ""),
                body = m.optString("body", ""),
                senderId = m.optString("sender_id", ""),
            ))
        }
    }
}
