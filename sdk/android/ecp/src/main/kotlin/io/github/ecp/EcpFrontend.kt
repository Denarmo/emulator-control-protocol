package io.github.ecp

import android.app.ActivityOptions
import android.app.PendingIntent
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.util.Log
import org.json.JSONObject
import java.io.IOException
import java.net.ServerSocket
import java.net.SocketTimeoutException
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

/**
 * The frontend side on Android: finds engines and starts sessions.
 *
 * ```
 * val fe = EcpFrontend(context)
 * val engine = fe.findEngines().first { "ps2" in it.systems }
 * val session = fe.start(engine, gameUri, Intent(context, MenuActivity::class.java), listener = myListener)
 * // listener.onReady -> session.request("launch", ...) off the main thread, or requestAsync
 * ```
 */
class EcpFrontend(private val context: Context) {

    /** An installed engine: its PLAY activity and descriptor. */
    data class Engine(
        val packageName: String,
        val activity: String,
        val label: String,
        val descriptor: JSONObject?,
    ) {
        val systems: List<String> get() = descriptor?.optJSONArray("systems").strings()
        val capabilities: List<String> get() = descriptor?.optJSONArray("capabilities").strings()
        val component: ComponentName get() = ComponentName(packageName, activity)
    }

    data class LaunchOptions @JvmOverloads constructor(
        val displayId: Int? = null,     // ActivityOptions.setLaunchDisplayId
        val fullscreen: Boolean = true,
        val path: String? = null,       // EXTRA_PATH, for engines with all-files access
        val mimeType: String? = null,
    )

    /** Every activity that handles io.github.ecp.action.PLAY, with its descriptor if it has one. */
    fun findEngines(): List<Engine> {
        val pm = context.packageManager
        val query = Intent(Ecp.ACTION_PLAY).addCategory(Intent.CATEGORY_DEFAULT)
        @Suppress("DEPRECATION")
        val found = if (Build.VERSION.SDK_INT >= 33)
            pm.queryIntentActivities(query, PackageManager.ResolveInfoFlags.of(PackageManager.GET_META_DATA.toLong()))
        else pm.queryIntentActivities(query, PackageManager.GET_META_DATA)
        return found.map { ri ->
            val ai = ri.activityInfo
            val res = ai.metaData?.getInt(Ecp.META_DESCRIPTOR, 0) ?: 0
            val descriptor = if (res == 0) null else try {
                pm.getResourcesForApplication(ai.applicationInfo).openRawResource(res)
                    .bufferedReader().use { JSONObject(it.readText()) }
            } catch (e: Exception) {
                Log.w(TAG, "${ai.packageName}: unreadable ECP descriptor: $e")
                null
            }
            Engine(ai.packageName, ai.name, ri.loadLabel(pm).toString(), descriptor)
        }
    }

    /**
     * Listens on loopback, starts the engine's PLAY activity with the session extras and waits
     * (in the background) for it to connect. [menuIntent] is the frontend's own activity that
     * shows its menu over the game; it is wrapped in the MENU_INTENT PendingIntent.
     */
    @JvmOverloads
    fun start(
        engine: Engine,
        content: Uri,
        menuIntent: Intent,
        options: LaunchOptions = LaunchOptions(),
        listener: EcpSession.Listener,
        executor: Executor = MainExecutor,
    ): EcpSession {
        val session = EcpSession(listener, executor)
        val intent = Intent(Ecp.ACTION_PLAY)
            .setComponent(engine.component)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION)
            .putExtra(Ecp.EXTRA_PORT, session.port)
            .putExtra(Ecp.EXTRA_TOKEN, session.token)
            .putExtra(Ecp.EXTRA_SESSION, session.id)
            .putExtra(Ecp.EXTRA_MENU_INTENT, menuPendingIntent(menuIntent))
            .putExtra(Ecp.EXTRA_FULLSCREEN, options.fullscreen)
        if (options.mimeType != null) intent.setDataAndType(content, options.mimeType) else intent.data = content
        if (options.path != null) intent.putExtra(Ecp.EXTRA_PATH, options.path)
        val launch = ActivityOptions.makeBasic()
        if (options.displayId != null) launch.launchDisplayId = options.displayId
        session.engineComponent = engine.component
        session.acceptInBackground()
        try {
            context.startActivity(intent, launch.toBundle())
        } catch (e: Exception) {
            session.close()
            throw EcpException("internal", "The emulator could not be started.", e.toString())
        }
        return session
    }

    /**
     * Puts the engine back in front, then sends `resume` (section 6.2, step 5). Call it from the
     * frontend's foreground activity, which is what allows the activity start.
     */
    @JvmOverloads
    fun resume(session: EcpSession, onResult: ResultCallback = ResultCallback {}) {
        bringToFront(session)
        session.requestAsync("resume", JSONObject(), 5000, onResult)
    }

    fun bringToFront(session: EcpSession) {
        val component = session.engineComponent ?: return
        val intent = Intent(Ecp.ACTION_PLAY).setComponent(component)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
        context.startActivity(intent)
    }

    private fun menuPendingIntent(menu: Intent): PendingIntent {
        val creator = ActivityOptions.makeBasic()
        if (Build.VERSION.SDK_INT >= 36) {
            creator.setPendingIntentCreatorBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS)
        } else if (Build.VERSION.SDK_INT >= 34) {
            @Suppress("DEPRECATION")
            creator.setPendingIntentCreatorBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED)
        }
        val intent = Intent(menu).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
        return PendingIntent.getActivity(
            context, 0, intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            creator.toBundle(),
        )
    }

    private companion object {
        const val TAG = "ECP"
    }
}

/**
 * One session with an engine, from the frontend's side: a loopback listener, the token check, the
 * hellos, requests matched to results by id, and events. Plain Java networking, so it also runs on
 * the JVM (the unit tests use it).
 */
/** Receives a result, or null when none arrived (timeout, channel closed). */
fun interface ResultCallback {
    fun onResult(result: JSONObject?)
}

class EcpSession @JvmOverloads constructor(
    private val listener: Listener,
    private val executor: Executor = MainExecutor,
    frontendName: String = "android-frontend",
    frontendVersion: String = "1.0.0",
) {
    interface Listener {
        /** Hellos exchanged; [engine] is the engine's hello (its descriptor). Send `launch` now. */
        fun onReady(session: EcpSession, engine: JSONObject) {}

        /** Any event: loading, first_frame, running, paused, resumed, menu_request, perf, stopped. */
        fun onEvent(session: EcpSession, type: String, msg: JSONObject) {}

        /** applet_request; answer with [EcpSession.reply]. */
        fun onAppletRequest(session: EcpSession, id: Long, msg: JSONObject) {
            session.reply(id, JSONObject(), ok = false)
        }

        /** The engine never connected, failed auth or hello, or the channel closed. */
        fun onClosed(session: EcpSession, reason: String) {}
    }

    private val server = ServerSocket(0, 1, Ecp.LOOPBACK)
    private val hello = JSONObject().put("type", "hello").put("protocol", Ecp.PROTOCOL)
        .put("frontend", frontendName).put("version", frontendVersion)
    private val pending = ConcurrentHashMap<Long, (JSONObject?) -> Unit>()
    private val nextId = AtomicLong()
    @Volatile private var channel: EcpChannel? = null
    @Volatile var engine: JSONObject? = null
        private set
    @Volatile var capabilities: Set<String> = emptySet()
        private set
    internal var engineComponent: ComponentName? = null

    val port: Int = server.localPort
    val token: String = ByteArray(32).also { SecureRandom().nextBytes(it) }.joinToString("") { "%02x".format(it) }
    val id: String = UUID.randomUUID().toString()

    /** Capabilities to use; null: all the engine lists. Set before the engine connects. */
    var wanted: Set<String>? = null

    fun has(capability: String) = capability in capabilities

    /** Accepts the engine's connection (10 s), checks its token and hello (10 s). */
    fun acceptInBackground() {
        Thread({
            val reason = try {
                server.soTimeout = 10_000
                val socket = server.accept()
                server.close()
                val ch = EcpChannel(socket)
                socket.soTimeout = 10_000
                val auth = ch.read()
                if (auth?.optString("type") != "auth" ||
                    !MessageDigest.isEqual(auth.optString("token").toByteArray(), token.toByteArray())
                ) {
                    ch.close()
                    "the engine's first frame was not the right token"
                } else {
                    val h = ch.read()
                    socket.soTimeout = 0
                    if (h?.optString("type") != "hello" || h.optInt("protocol") != Ecp.PROTOCOL) {
                        ch.close()
                        "no usable hello from the engine"
                    } else {
                        start(ch, h)
                        null
                    }
                }
            } catch (_: SocketTimeoutException) {
                "the engine did not connect within 10 s"
            } catch (e: IOException) {
                e.toString()
            } finally {
                try { server.close() } catch (_: IOException) {}
            }
            if (reason != null) executor.execute { listener.onClosed(this, reason) }
        }, "ecp-accept").apply { isDaemon = true }.start()
    }

    private fun start(ch: EcpChannel, engineHello: JSONObject) {
        engine = engineHello
        val listed = engineHello.optJSONArray("capabilities").strings()
        capabilities = listed.filter { wanted?.contains(it) ?: true }.toSet()
        channel = ch
        ch.send(JSONObject(hello.toString()).put("level", engineHello.optInt("level", 1))
            .put("capabilities", org.json.JSONArray(capabilities.toList())))
        executor.execute { listener.onReady(this, engineHello) }
        ch.startReader("ecp-frontend", ::dispatch) {
            pending.keys.toList().forEach { id -> pending.remove(id)?.invoke(null) }
            executor.execute { listener.onClosed(this, "the channel closed") }
        }
    }

    private fun dispatch(msg: JSONObject) {
        val type = msg.optString("type")
        when {
            type == "result" -> pending.remove(msg.optLong("id", -1))?.invoke(msg)
            type == "applet_request" && msg.has("id") ->
                executor.execute { listener.onAppletRequest(this, msg.optLong("id"), msg) }
            else -> executor.execute { listener.onEvent(this, type, msg) }
        }
    }

    /**
     * Sends a request and blocks until its result (not on the main thread). Throws
     * [EcpException] with the engine's error, or `timeout` / `closed`.
     */
    @JvmOverloads
    fun request(type: String, fields: JSONObject = JSONObject(), timeoutMs: Long = 5000): JSONObject {
        val latch = CountDownLatch(1)
        var result: JSONObject? = null
        val id = send(type, fields) { result = it; latch.countDown() }
        if (!latch.await(timeoutMs, TimeUnit.MILLISECONDS)) {
            pending.remove(id)
            throw EcpException("timeout", "The emulator did not answer.", type)
        }
        val r = result ?: throw EcpException("closed", "The emulator closed.", type)
        if (!r.optBoolean("ok")) {
            val e = r.optJSONObject("error") ?: JSONObject()
            throw EcpException(e.optString("code", "internal"), e.optString("message").ifEmpty { null }, e.optString("detail").ifEmpty { null })
        }
        return r
    }

    /** Sends a request; [onResult] runs on the executor with the result, or null on timeout/close. */
    @JvmOverloads
    fun requestAsync(type: String, fields: JSONObject = JSONObject(), timeoutMs: Long = 5000, onResult: ResultCallback) {
        Thread({
            val r = try { request(type, fields, timeoutMs) } catch (e: EcpException) {
                if (e.code == "timeout" || e.code == "closed") null
                else JSONObject().put("type", "result").put("ok", false).put("error", Ecp.error(e.code, e.message, e.detail))
            }
            executor.execute { onResult.onResult(r) }
        }, "ecp-request").apply { isDaemon = true }.start()
    }

    /** Answers a request the engine sent. */
    @JvmOverloads
    fun reply(id: Long, fields: JSONObject = JSONObject(), ok: Boolean = true) {
        channel?.send(fields.put("type", "result").put("id", id).put("ok", ok))
    }

    /** Closes the channel; the engine then saves and exits (section 4, End). */
    fun close() {
        try { server.close() } catch (_: IOException) {}
        channel?.close()
    }

    private fun send(type: String, fields: JSONObject, onResult: (JSONObject?) -> Unit): Long {
        val id = nextId.incrementAndGet()
        pending[id] = onResult
        val ch = channel
        if (ch == null || !ch.send(fields.put("type", type).put("id", id))) pending.remove(id)?.invoke(null)
        return id
    }
}
