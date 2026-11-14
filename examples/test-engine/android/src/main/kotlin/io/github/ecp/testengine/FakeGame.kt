package io.github.ecp.testengine

import android.graphics.Bitmap
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * The "emulator": a frame counter (the whole game state, shown in the picture), the settings
 * the desktop test engine has, and save states stored as files under the app's own storage.
 */
class FakeGame(private val statesRoot: File) {
    var frame = 0L
    var gameId = "game"

    private class Setting(
        val key: String, val label: String, val group: String, val applies: String,
        val type: String, val min: Double = 0.0, val max: Double = 0.0,
        val choices: List<Pair<String, String>> = emptyList(), val default: Any,
    )

    private val settings = listOf(
        Setting("video.internal_scale", "Internal resolution", "Video", "now", "range", 1.0, 8.0, default = 1),
        Setting("video.aspect", "Aspect ratio", "Video", "now", "choice",
            choices = listOf("auto" to "Automatic", "4:3" to "4:3", "16:9" to "16:9", "stretch" to "Stretch"), default = "auto"),
        Setting("audio.volume", "Volume", "Audio", "now", "range", 0.0, 100.0, default = 100),
        Setting("speed.limit", "Frame limiter", "Speed", "now", "toggle", default = true),
        Setting("speed.fast_forward_factor", "Fast-forward speed", "Speed", "now", "range", 2.0, 10.0, default = 4),
        Setting("test.pattern", "Pattern", "Test", "now", "choice",
            choices = listOf("bars" to "Bars", "checker" to "Checkerboard"), default = "bars"),
        Setting("test.region", "Region", "Test", "restart", "choice",
            choices = listOf("ntsc" to "NTSC (60 Hz)", "pal" to "PAL (50 Hz)"), default = "ntsc"),
    )
    private val values = settings.associate { it.key to it.default }.toMutableMap()

    fun number(key: String) = (values[key] as Number).toDouble()
    fun text(key: String) = values[key].toString()
    fun flag(key: String) = values[key] as Boolean

    /** Applies one value; returns why it was refused, or null. */
    fun apply(key: String, value: Any?): String? {
        val s = settings.firstOrNull { it.key == key } ?: return "unknown key"
        when (s.type) {
            "toggle" -> values[key] = value as? Boolean ?: return "expected a boolean"
            "range" -> {
                val n = (value as? Number)?.toDouble() ?: return "expected a number"
                if (n < s.min || n > s.max) return "out of range"
                values[key] = n
            }
            "choice" -> {
                if (s.choices.none { it.first == value }) return "not one of the choices"
                values[key] = value as String
            }
        }
        return null
    }

    fun applies(key: String) = settings.first { it.key == key }.applies

    fun schema(): JSONArray = JSONArray().apply {
        for (s in settings) put(JSONObject().apply {
            put("key", s.key).put("label", s.label).put("group", s.group).put("applies", s.applies)
            put("type", s.type).put("default", s.default)
            if (s.type == "range") put("min", s.min).put("max", s.max).put("step", 1)
            if (s.type == "choice") put("choices", JSONArray(s.choices.map { JSONArray(listOf(it.first, it.second)) }))
        })
    }

    fun valuesJson(): JSONObject = JSONObject().apply { for ((k, v) in values) put(k, v) }

    val fps get() = if (text("test.region") == "pal") 50.0 else 60.0

    // ---- states

    private fun stateFile(slot: Int) = File(File(statesRoot, gameId.replace(Regex("[^A-Za-z0-9.-]"), "_")), "slot%02d.state".format(slot))

    fun save(slot: Int): Long {
        val f = stateFile(slot)
        f.parentFile?.mkdirs()
        val now = System.currentTimeMillis() / 1000
        f.writeText("ecp-test-state 1 $frame $now\n")
        return now
    }

    /** (frame, time) of a slot, or null if it is empty; throws IllegalStateException if unreadable. */
    fun read(slot: Int): Pair<Long, Long>? {
        val f = stateFile(slot)
        if (!f.exists()) return null
        val parts = f.readText().trim().split(" ")
        check(parts.size == 4 && parts[0] == "ecp-test-state") { "incompatible" }
        return parts[2].toLong() to parts[3].toLong()
    }

    fun delete(slot: Int) = stateFile(slot).delete()

    // ---- picture: the same pattern as the desktop test engine

    fun render(frame: Long = this.frame, scale: Int = 1): Bitmap {
        val w = 320 * scale
        val h = 240 * scale
        val px = IntArray(w * h)
        val t = frame.toInt()
        val checker = text("test.pattern") == "checker"
        for (y in 0 until h) for (x in 0 until w) {
            val bx = x / scale
            val by = y / scale
            val (r, g, b) = if (checker) {
                val on = (((bx + t) / 16 + by / 16) and 1) == 1
                Triple(if (on) 230 else 30, if (on) 200 else 40, (by + t) and 255)
            } else Triple((bx + t * 2) and 255, (by + t) and 255, ((bx xor by) + t * 3) and 255)
            px[y * w + x] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        }
        val digits = "%08d".format(frame % 100_000_000)
        val cell = 3 * scale
        for (d in 0 until 8) for (gy in 0 until 5) for (gx in 0 until 3) {
            val on = (DIGITS[digits[d] - '0'] shr ((4 - gy) * 3 + (2 - gx))) and 1 == 1
            val color = if (on) -1 else 0xFF000000.toInt()
            for (yy in 0 until cell) for (xx in 0 until cell) {
                val x = (4 + d * 4 + gx) * cell + xx
                val y = (4 + gy) * cell + yy
                if (x < w && y < h) px[y * w + x] = color
            }
        }
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
    }

    private companion object {
        // 3x5 digits, one bit per pixel, top row first.
        val DIGITS = intArrayOf(0x7B6F, 0x2492, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF)
    }
}
