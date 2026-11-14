// Emulator Control Protocol 1 for Android. MIT licence.
package io.github.ecp

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Base64
import org.json.JSONObject
import java.io.ByteArrayOutputStream

/** Names from the specification (sections 2, 3 and 4). */
object Ecp {
    const val PROTOCOL = 1

    const val ACTION_PLAY = "io.github.ecp.action.PLAY"
    const val META_DESCRIPTOR = "io.github.ecp.descriptor"

    const val EXTRA_PORT = "io.github.ecp.extra.PORT"
    const val EXTRA_TOKEN = "io.github.ecp.extra.TOKEN"
    const val EXTRA_SESSION = "io.github.ecp.extra.SESSION"
    const val EXTRA_MENU_INTENT = "io.github.ecp.extra.MENU_INTENT"
    const val EXTRA_FULLSCREEN = "io.github.ecp.extra.FULLSCREEN"
    const val EXTRA_PATH = "io.github.ecp.extra.PATH"

    const val MAX_FRAME = 16 * 1024 * 1024

    /** 127.0.0.1, as SPEC section 3 says. Not getLoopbackAddress(), which is ::1 on Android. */
    @JvmField
    val LOOPBACK: java.net.InetAddress = java.net.InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))

    /**
     * The capabilities that allow each request (section 6): any one of them will do, and an empty
     * list means every engine handles it. `resume` also ends the menu hand-over (6.2).
     */
    internal val REQUEST_CAPABILITY: Map<String, List<String>> = mapOf(
        "launch" to emptyList(), "stop" to emptyList(),
        "pause" to listOf("pause"), "resume" to listOf("pause", "menu"),
        "save_state" to listOf("states"), "load_state" to listOf("states"),
        "list_states" to listOf("states"), "delete_state" to listOf("states"),
        "get_schema" to listOf("settings"), "get_settings" to listOf("settings"),
        "set_setting" to listOf("settings"), "reset" to listOf("reset"),
        "set_speed" to listOf("fast_forward", "slow_motion"), "set_rewind" to listOf("rewind"),
        "screenshot" to listOf("screenshot"), "set_volume" to listOf("volume"), "hud_level" to listOf("hud"),
        "notify" to listOf("notify"), "set_cheats" to listOf("cheats"),
        "list_discs" to listOf("discs"), "set_disc" to listOf("discs"),
    )

    /** An error object: `{"code":..., "message":..., "detail":...}`. */
    @JvmStatic
    @JvmOverloads
    fun error(code: String, message: String? = null, detail: String? = null): JSONObject =
        JSONObject().put("code", code).apply {
            if (message != null) put("message", message)
            if (detail != null) put("detail", detail)
        }

    /**
     * A picture value, `{"png":..., "width":..., "height":...}`. Bitmaps over 4 megapixels are
     * halved until they fit, which keeps the frame well under the 16 MiB limit.
     */
    @JvmStatic
    fun picture(bitmap: Bitmap): JSONObject {
        var bmp = bitmap
        while (bmp.width.toLong() * bmp.height > 4_000_000L)
            bmp = Bitmap.createScaledBitmap(bmp, bmp.width / 2, bmp.height / 2, true)
        val out = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
        return picture(out.toByteArray(), bmp.width, bmp.height)
    }

    @JvmStatic
    fun picture(png: ByteArray, width: Int, height: Int): JSONObject = JSONObject()
        .put("png", Base64.encodeToString(png, Base64.NO_WRAP))
        .put("width", width)
        .put("height", height)

    /** Decodes a picture value, or null. */
    @JvmStatic
    fun bitmap(picture: JSONObject?): Bitmap? {
        val b64 = picture?.optString("png").orEmpty()
        if (b64.isEmpty()) return null
        val png = Base64.decode(b64, Base64.DEFAULT)
        return BitmapFactory.decodeByteArray(png, 0, png.size)
    }
}

/** A typed protocol error (section 4): `code` is one of the spec's codes, or `timeout`/`closed`. */
class EcpException(val code: String, message: String? = null, val detail: String? = null) :
    Exception(message ?: code)
