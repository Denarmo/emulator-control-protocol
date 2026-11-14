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

    /** Capabilities whose requests the engine side routes (section 6). */
    internal val REQUEST_CAPABILITY = mapOf(
        "launch" to null, "stop" to null,
        "pause" to "pause", "resume" to "pause",
        "save_state" to "states", "load_state" to "states",
        "list_states" to "states", "delete_state" to "states",
        "get_schema" to "settings", "get_settings" to "settings", "set_setting" to "settings",
        "reset" to "reset", "set_speed" to "fast_forward", "set_rewind" to "rewind",
        "screenshot" to "screenshot", "set_volume" to "volume", "hud_level" to "hud",
        "notify" to "notify", "set_cheats" to "cheats", "list_discs" to "discs", "set_disc" to "discs",
    )

    /** An error object: `{"code":..., "message":..., "detail":...}`. */
    fun error(code: String, message: String? = null, detail: String? = null): JSONObject =
        JSONObject().put("code", code).apply {
            if (message != null) put("message", message)
            if (detail != null) put("detail", detail)
        }

    /**
     * A picture value, `{"png":..., "width":..., "height":...}`. Bitmaps over 4 megapixels are
     * halved until they fit, which keeps the frame well under the 16 MiB limit.
     */
    fun picture(bitmap: Bitmap): JSONObject {
        var bmp = bitmap
        while (bmp.width.toLong() * bmp.height > 4_000_000L)
            bmp = Bitmap.createScaledBitmap(bmp, bmp.width / 2, bmp.height / 2, true)
        val out = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
        return picture(out.toByteArray(), bmp.width, bmp.height)
    }

    fun picture(png: ByteArray, width: Int, height: Int): JSONObject = JSONObject()
        .put("png", Base64.encodeToString(png, Base64.NO_WRAP))
        .put("width", width)
        .put("height", height)

    /** Decodes a picture value, or null. */
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
