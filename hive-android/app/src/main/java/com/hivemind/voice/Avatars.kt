package com.hivemind.voice

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory

/**
 * Agent avatars — mirrors hive-client's resolution exactly
 * (hive-client/src/HiveWindow.cpp make_avatar):
 *
 *   stored `avatar` field if it's in the set → else stable name-hash →
 *   else "user". The name set and its order are the same 20 creatures;
 *   the hash is libstdc++ std::hash<string> = MurmurHash64A seed
 *   0xc70f6907, so an agent shows the same face here as in the GTK
 *   client and web UI. PNGs are rasterized from the client's SVGs
 *   (rsvg-convert 192px) into assets/avatars/.
 */
object Avatars {

    private val NAMES = listOf(
        "queen-bee", "worker-bee", "drone-bee", "honey-bee", "bumblebee",
        "ladybug", "butterfly", "blue-butterfly", "dragonfly", "firefly",
        "caterpillar", "snail", "ant", "beetle", "spider",
        "moth", "grasshopper", "worm", "mantis", "cricket",
    )

    private val cache = HashMap<String, Bitmap>()

    /** Resolve to a bundled avatar name. */
    fun resolve(agentName: String, avatarField: String): String {
        if (avatarField.isNotEmpty() &&
            (avatarField in NAMES || avatarField == "user")) return avatarField
        val auto = NAMES[(murmur64a(agentName) % NAMES.size.toULong()).toInt()]
        return auto.ifEmpty { "user" }
    }

    fun bitmap(ctx: Context, agentName: String, avatarField: String): Bitmap {
        val res = resolve(agentName, avatarField)
        return cache.getOrPut(res) {
            ctx.assets.open("avatars/$res.png").use(BitmapFactory::decodeStream)
        }
    }

    /** MurmurHash64A — verified bit-for-bit against C++ std::hash<string>. */
    private fun murmur64a(s: String, seed: ULong = 0xc70f6907uL): ULong {
        val data = s.toByteArray(Charsets.UTF_8)
        val m = 0xc6a4a7935bd1e995uL
        val r = 47
        var h = seed xor (data.size.toULong() * m)
        var i = 0
        while (i + 8 <= data.size) {
            var k = data[i].toULong() and 0xFFuL or
                (data[i + 1].toULong() and 0xFFuL shl 8) or
                (data[i + 2].toULong() and 0xFFuL shl 16) or
                (data[i + 3].toULong() and 0xFFuL shl 24) or
                (data[i + 4].toULong() and 0xFFuL shl 32) or
                (data[i + 5].toULong() and 0xFFuL shl 40) or
                (data[i + 6].toULong() and 0xFFuL shl 48) or
                (data[i + 7].toULong() and 0xFFuL shl 56)
            k *= m; k = k xor (k shr r); k *= m
            h = h xor k
            h *= m
            i += 8
        }
        var shift = 0
        while (i < data.size) {
            h = h xor ((data[i].toULong() and 0xFFuL) shl shift)
            shift += 8
            i++
        }
        if (shift > 0) h *= m
        h = h xor (h shr r); h *= m; h = h xor (h shr r)
        return h
    }
}
