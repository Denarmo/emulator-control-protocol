package io.github.ecp

import android.app.Activity
import android.app.ActivityOptions
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicLong

/**
 * The engine side of a session.
 *
 * ```
 * val session = EcpEngine.Session.from(intent) ?: return startLevel0()
 * engine = EcpEngine(descriptor, callbacks).also { it.connect(session) }
 * ```
 *
 * Callbacks run on [executor], the main thread by default. Every [Request] is answered exactly
 * once with [Request.ok] or [Request.error], from any thread and at any time (a save state can
 * answer when the emulation thread has written it). Event methods may be called from any thread.
 */
class EcpEngine(
    descriptor: JSONObject,
    private val callbacks: Callbacks,
    private val executor: Executor = MainExecutor,
) {
    /** What the frontend passed in the PLAY intent (section 3, Android). */
    data class Session(
        val port: Int,
        val token: String,
        val id: String?,
        val menuIntent: PendingIntent?,
        val fullscreen: Boolean,
        val content: Uri?,
        val path: String?,
    ) {
        companion object {
            /** The session in [intent], or null when it has no PORT: run as at level 0. */
            fun from(intent: Intent?): Session? {
                if (intent == null) return null
                val port = intent.getIntExtra(Ecp.EXTRA_PORT, 0)
                val token = intent.getStringExtra(Ecp.EXTRA_TOKEN)
                if (port <= 0 || token.isNullOrEmpty()) return null
                @Suppress("DEPRECATION")
                val menu: PendingIntent? = if (Build.VERSION.SDK_INT >= 33)
                    intent.getParcelableExtra(Ecp.EXTRA_MENU_INTENT, PendingIntent::class.java)
                else intent.getParcelableExtra(Ecp.EXTRA_MENU_INTENT)
                return Session(
                    port, token, intent.getStringExtra(Ecp.EXTRA_SESSION), menu,
                    intent.getBooleanExtra(Ecp.EXTRA_FULLSCREEN, true), intent.data,
                    intent.getStringExtra(Ecp.EXTRA_PATH),
                )
            }
        }
    }

    /** A request from the frontend. */
    class Request internal constructor(
        private val engine: EcpEngine,
        val id: Long,
        val type: String,
        val msg: JSONObject,
    ) {
        fun int(key: String, fallback: Int = -1): Int = if (msg.has(key)) msg.optInt(key, fallback) else fallback
        fun bool(key: String, fallback: Boolean = false): Boolean = msg.optBoolean(key, fallback)
        fun double(key: String, fallback: Double = Double.NaN): Double = msg.optDouble(key, fallback)
        fun string(key: String): String? = if (msg.has(key) && !msg.isNull(key)) msg.optString(key) else null

        /** Answers ok, with the result fields in [fields]. */
        fun ok(fields: JSONObject? = null) {
            val res = fields ?: JSONObject()
            res.put("type", "result").put("id", id).put("ok", true)
            engine.send(res)
        }

        fun error(code: String, message: String? = null, detail: String? = null) {
            engine.send(
                JSONObject().put("type", "result").put("id", id).put("ok", false)
                    .put("error", Ecp.error(code, message, detail)),
            )
        }

        fun unsupported() = error("unsupported", null, type)
    }

    /**
     * One handler per request, named after it. Those left at their default answer `unsupported`;
     * the library also answers `unsupported` for any request whose capability the descriptor does
     * not list. The names stay clear of Activity.onPause/onResume/onStop, so an activity can
     * implement this interface.
     */
    interface Callbacks {
        fun handleLaunch(r: Request)
        fun handleStop(r: Request)
        fun handlePause(r: Request) = r.unsupported()
        fun handleResume(r: Request) = r.unsupported()
        fun handleSaveState(r: Request) = r.unsupported()
        fun handleLoadState(r: Request) = r.unsupported()
        fun handleListStates(r: Request) = r.unsupported()
        fun handleDeleteState(r: Request) = r.unsupported()
        fun handleGetSchema(r: Request) = r.unsupported()
        fun handleGetSettings(r: Request) = r.unsupported()
        fun handleSetSetting(r: Request) = r.unsupported()
        fun handleReset(r: Request) = r.unsupported()
        fun handleSetSpeed(r: Request) = r.unsupported()
        fun handleSetRewind(r: Request) = r.unsupported()
        fun handleScreenshot(r: Request) = r.unsupported()
        fun handleSetVolume(r: Request) = r.unsupported()
        fun handleHudLevel(r: Request) = r.unsupported()
        fun handleNotify(r: Request) = r.unsupported()
        fun handleSetCheats(r: Request) = r.unsupported()
        fun handleListDiscs(r: Request) = r.unsupported()
        fun handleSetDisc(r: Request) = r.unsupported()

        /** The frontend's hello: its name, level and the capabilities it will use. */
        fun onHello(frontend: JSONObject) {}

        /** A result to a request the engine sent with [EcpEngine.request] (applet_request). */
        fun onResult(id: Long, msg: JSONObject) {}

        /**
         * The channel closed or could not be opened ([reason] says why). The engine saves and
         * finishes (section 4, End); it never keeps running headless.
         */
        fun onClosed(reason: String?) {}
    }

    private val descriptor = JSONObject(descriptor.toString())
    private val capabilities: Set<String> = descriptor.optJSONArray("capabilities").strings().toSet()
    private val nextId = AtomicLong()
    @Volatile private var channel: EcpChannel? = null
    @Volatile private var frontendCaps: Set<String> = emptySet()
    @Volatile var session: Session? = null
        private set

    val isOpen: Boolean get() = channel?.isOpen == true

    /** True if the frontend listed [capability] in its hello. */
    fun frontendHas(capability: String) = capability in frontendCaps

    /** Opens the channel on a background thread: connect within 10 s, auth, hello. */
    fun connect(session: Session) {
        this.session = session
        Thread({
            try {
                val socket = Socket()
                socket.connect(InetSocketAddress(InetAddress.getLoopbackAddress(), session.port), 10_000)
                val ch = EcpChannel(socket)
                channel = ch
                ch.send(JSONObject().put("type", "auth").put("token", session.token))
                val hello = JSONObject(descriptor.toString()).put("type", "hello").put("protocol", Ecp.PROTOCOL)
                if (!ch.send(hello)) throw java.io.IOException("the channel closed during hello")
                if (session.id != null) Log.i(TAG, "session ${session.id}")
                ch.startReader("ecp-engine", ::dispatch) {
                    executor.execute { callbacks.onClosed("the frontend closed the channel") }
                }
            } catch (e: Exception) {
                Log.w(TAG, "cannot connect to the frontend: $e")
                executor.execute { callbacks.onClosed(e.toString()) }
            }
        }, "ecp-connect").start()
    }

    fun close() {
        channel?.close()
    }

    // ---- events (sections 5 and 6)

    fun loading(progress: Double? = null, text: String? = null) = event("loading") {
        if (progress != null) put("progress", progress.coerceIn(0.0, 1.0))
        if (text != null) put("text", text)
    }

    fun firstFrame() = event("first_frame")
    fun running() = event("running")
    fun paused(reason: String) = event("paused") { put("reason", reason) }
    fun resumed() = event("resumed")

    fun stopped(reason: String, errorCode: String? = null, message: String? = null) = event("stopped") {
        put("reason", reason)
        if (errorCode != null) put("error", Ecp.error(errorCode, message))
    }

    fun perf(
        fps: Double, targetFps: Double, speed: Double, frameMs: Double,
        cpuMs: Double? = null, gpuMs: Double? = null, shadersBuilding: Boolean? = null,
    ) = event("perf") {
        put("fps", fps).put("target_fps", targetFps).put("speed", speed).put("frame_ms", frameMs)
        if (cpuMs != null) put("cpu_ms", cpuMs)
        if (gpuMs != null) put("gpu_ms", gpuMs)
        if (shadersBuilding != null) put("shaders_building", shadersBuilding)
    }

    /**
     * The menu hand-over (section 6.2): the engine has paused; this sends `paused` (menu) and
     * `menu_request` with [frame], then fires MENU_INTENT. Call it while the engine's activity is
     * in the foreground, which is what lets Android start the frontend's activity.
     * Returns false if the frontend did not take the `menu` capability.
     */
    fun requestMenu(context: Context, frame: Bitmap): Boolean {
        if (!frontendHas("menu")) return false
        paused("menu")
        event("menu_request") { put("frame", Ecp.picture(frame)) }
        session?.menuIntent?.let { sendMenuIntent(context, it) }
        return true
    }

    /** Sends a request to the frontend (applet_request); the answer goes to [Callbacks.onResult]. */
    fun request(type: String, fields: JSONObject = JSONObject()): Long {
        val id = nextId.incrementAndGet()
        send(fields.put("type", type).put("id", id))
        return id
    }

    fun send(msg: JSONObject): Boolean = channel?.send(msg) == true

    private inline fun event(type: String, fill: JSONObject.() -> Unit = {}): Boolean =
        send(JSONObject().put("type", type).apply(fill))

    // ---- dispatch

    private fun dispatch(msg: JSONObject) {
        val type = msg.optString("type")
        when {
            type == "hello" -> {
                if (msg.optInt("protocol") != Ecp.PROTOCOL) {
                    Log.w(TAG, "the frontend speaks protocol ${msg.opt("protocol")}; closing")
                    close()
                    return
                }
                frontendCaps = msg.optJSONArray("capabilities").strings().filter { it in capabilities }.toSet()
                executor.execute { callbacks.onHello(msg) }
            }
            type == "result" -> executor.execute { callbacks.onResult(msg.optLong("id", -1), msg) }
            msg.has("id") -> {
                val r = Request(this, msg.optLong("id"), type, msg)
                val allowed = when (type) {
                    "set_speed" -> "fast_forward" in capabilities || "slow_motion" in capabilities
                    else -> Ecp.REQUEST_CAPABILITY.containsKey(type) &&
                        Ecp.REQUEST_CAPABILITY[type].let { it == null || it in capabilities }
                }
                if (!allowed) r.unsupported() else executor.execute { route(r) }
            }
        }
    }

    private fun route(r: Request) = with(callbacks) {
        when (r.type) {
            "launch" -> handleLaunch(r)
            "stop" -> handleStop(r)
            "pause" -> handlePause(r)
            "resume" -> handleResume(r)
            "save_state" -> handleSaveState(r)
            "load_state" -> handleLoadState(r)
            "list_states" -> handleListStates(r)
            "delete_state" -> handleDeleteState(r)
            "get_schema" -> handleGetSchema(r)
            "get_settings" -> handleGetSettings(r)
            "set_setting" -> handleSetSetting(r)
            "reset" -> handleReset(r)
            "set_speed" -> handleSetSpeed(r)
            "set_rewind" -> handleSetRewind(r)
            "screenshot" -> handleScreenshot(r)
            "set_volume" -> handleSetVolume(r)
            "hud_level" -> handleHudLevel(r)
            "notify" -> handleNotify(r)
            "set_cheats" -> handleSetCheats(r)
            "list_discs" -> handleListDiscs(r)
            "set_disc" -> handleSetDisc(r)
            else -> r.unsupported()
        }
    }

    companion object {
        private const val TAG = "ECP"

        /** True if [intent] is the frontend bringing the running game back (no new session). */
        fun isBringBack(intent: Intent?, current: Session?): Boolean =
            intent != null && current != null &&
                (intent.getIntExtra(Ecp.EXTRA_PORT, 0) == 0 || intent.getStringExtra(Ecp.EXTRA_TOKEN) == current.token)

        /** Reads a descriptor from a raw resource (the `io.github.ecp.descriptor` meta-data). */
        fun descriptor(context: Context, rawRes: Int): JSONObject =
            context.resources.openRawResource(rawRes).bufferedReader().use { JSONObject(it.readText()) }

        /**
         * Fires MENU_INTENT with the sender's opt-in to start an activity from the background on
         * Android 14+, which the platform otherwise refuses for PendingIntents.
         */
        fun sendMenuIntent(context: Context, menu: PendingIntent) {
            val options = ActivityOptions.makeBasic()
            if (Build.VERSION.SDK_INT >= 36) {
                options.setPendingIntentBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS)
            } else if (Build.VERSION.SDK_INT >= 34) {
                @Suppress("DEPRECATION")
                options.setPendingIntentBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED)
            }
            try {
                menu.send(context, 0, null, null, null, null, options.toBundle())
            } catch (e: PendingIntent.CanceledException) {
                Log.w(TAG, "the frontend's menu intent was cancelled")
            }
        }

        /** Immersive full screen, for sessions that ask for it (EXTRA_FULLSCREEN). */
        fun applyFullscreen(activity: Activity, fullscreen: Boolean) {
            if (!fullscreen) return
            val window = activity.window
            if (Build.VERSION.SDK_INT >= 30) {
                @Suppress("DEPRECATION") // edge-to-edge is the default from Android 15
                window.setDecorFitsSystemWindows(false)
                window.insetsController?.let {
                    it.hide(android.view.WindowInsets.Type.systemBars())
                    it.systemBarsBehavior = android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                }
            } else {
                @Suppress("DEPRECATION")
                window.decorView.systemUiVisibility = (android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or android.view.View.SYSTEM_UI_FLAG_FULLSCREEN or android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
            }
        }
    }
}

internal object MainExecutor : Executor {
    private val handler by lazy { Handler(Looper.getMainLooper()) }
    override fun execute(command: Runnable) {
        handler.post(command)
    }
}

internal fun JSONArray?.strings(): List<String> =
    if (this == null) emptyList() else (0 until length()).mapNotNull { optString(it, null) }
