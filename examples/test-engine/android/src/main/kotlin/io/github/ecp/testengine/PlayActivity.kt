package io.github.ecp.testengine

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Choreographer
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import io.github.ecp.Ecp
import io.github.ecp.EcpEngine
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * The test engine's one activity. Started by a frontend with an ECP session it speaks the
 * protocol; opened from the launcher it just plays (level 0). Tap the screen, or press L3+R3, to
 * ask for the frontend's menu.
 */
class PlayActivity : Activity(), EcpEngine.Callbacks, Choreographer.FrameCallback {
    private lateinit var game: FakeGame
    private lateinit var view: PatternView
    private val main = Handler(Looper.getMainLooper())
    private var engine: EcpEngine? = null

    private var running = false
    private var pausedBy: String? = null // frontend, menu, focus
    private var fastForward = false
    private var slowFactor = 1.0
    private var rewind = false
    private var hud = 0
    private var disc = 0
    private var lastNanos = 0L
    private var frameAcc = 0.0
    private var perfAt = 0L
    private var perfFrames = 0
    private var fps = 0.0
    private var toast: String? = null
    private var toastUntil = 0L
    private val held = mutableSetOf<Int>()
    private var chord = listOf(KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        game = FakeGame(File(filesDir, "states"))
        view = PatternView(this)
        setContentView(view)
        val session = EcpEngine.Session.from(intent)
        EcpEngine.applyFullscreen(this, session?.fullscreen ?: true)
        if (session == null) {
            game.gameId = intent.data?.lastPathSegment ?: "standalone"
            startPlaying() // level 0
        } else {
            // Every handler here is quick and touches the view's state, so all of them, stop
            // included, run on the main thread. An emulator whose calls block keeps the defaults.
            val desc = EcpEngine.descriptor(this, R.raw.ecp)
            engine = EcpEngine(desc, this, EcpEngine.MAIN_THREAD, EcpEngine.MAIN_THREAD).also { it.connect(session) }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // The frontend bringing the game back for `resume`: nothing to do, we are in front.
        if (!EcpEngine.isBringBack(intent, engine?.session)) setIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        if (pausedBy == "focus") {
            pausedBy = null
            engine?.resumed()
        }
        Choreographer.getInstance().postFrameCallback(this)
    }

    override fun onPause() {
        super.onPause()
        if (running && pausedBy == null) {
            pausedBy = "focus"
            engine?.paused("focus")
        }
        Choreographer.getInstance().removeFrameCallback(this)
    }

    override fun onDestroy() {
        engine?.close()
        super.onDestroy()
    }

    private fun startPlaying() {
        running = true
        lastNanos = 0
        perfAt = SystemClock.uptimeMillis()
    }

    // ---- frame loop

    override fun doFrame(frameTimeNanos: Long) {
        Choreographer.getInstance().postFrameCallback(this)
        if (lastNanos == 0L) lastNanos = frameTimeNanos
        val dt = (frameTimeNanos - lastNanos) / 1e9
        lastNanos = frameTimeNanos
        if (running && pausedBy == null) {
            val speed = when {
                fastForward -> game.number("speed.fast_forward_factor")
                !game.flag("speed.limit") -> 8.0
                else -> slowFactor
            }
            frameAcc += dt * game.fps * speed
            val n = frameAcc.toLong()
            frameAcc -= n
            game.frame = if (rewind) maxOf(0, game.frame - n) else game.frame + n
            perfFrames += n.toInt()
        }
        val now = SystemClock.uptimeMillis()
        if (now - perfAt >= 1000) {
            val secs = (now - perfAt) / 1000.0
            fps = perfFrames / secs
            if (engine?.frontendHas("perf") == true && running)
                engine?.perf(fps, if (pausedBy == null) game.fps else 0.0, fps / game.fps,
                    if (perfFrames > 0) secs * 1000 / perfFrames else 0.0)
            perfAt = now
            perfFrames = 0
        }
        view.invalidate()
    }

    // ---- the engine's own way into the frontend's menu

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_UP) askForMenu()
        return true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        held += keyCode
        if (chord.all { it in held }) askForMenu()
        return keyCode in chord || super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        held -= keyCode
        return super.onKeyUp(keyCode, event)
    }

    private fun askForMenu() {
        val e = engine
        if (!running || pausedBy != null) return
        if (e == null || !e.frontendHas("menu")) {
            showToast("No frontend menu attached")
            return
        }
        pausedBy = "menu"
        e.requestMenu(this, game.render())
    }

    private fun showToast(text: String, seconds: Double = 2.0) {
        toast = text
        toastUntil = SystemClock.uptimeMillis() + (seconds * 1000).toLong()
    }

    // ---- ECP requests

    override fun onClosed(reason: String?) {
        finish() // nothing to save: states are written when they are made
    }

    override fun handleLaunch(r: EcpEngine.Request) {
        if (running) return r.error("busy", "A game is already running.")
        val content = r.string("content")
        val uri = if (content == "intent") intent.data else null
        val readable = try {
            if (uri != null) contentResolver.openInputStream(uri)?.use { true } ?: false
            else content != null && File(content).canRead()
        } catch (e: Exception) {
            false
        }
        if (!readable) return r.error("content_missing", "The game file was not found.", content)
        game.gameId = r.string("game_id") ?: uri?.lastPathSegment ?: "game"
        r.msg.optJSONObject("settings")?.let { s -> s.keys().forEach { game.apply(it, s.get(it)) } }
        r.msg.optJSONArray("menu_chord")?.let { c ->
            chord = (0 until c.length()).mapNotNull { KEYS[c.optString(it)] }.ifEmpty { chord }
        }
        r.ok()
        engine?.loading(0.0, "Loading")
        main.postDelayed({ engine?.loading(1.0) }, 150)
        main.postDelayed({
            startPlaying()
            engine?.firstFrame()
            engine?.running()
        }, 300)
    }

    override fun handleStop(r: EcpEngine.Request) {
        r.ok()
        engine?.stopped("frontend")
        finish()
    }

    override fun handlePause(r: EcpEngine.Request) {
        r.ok()
        if (pausedBy == null) {
            pausedBy = "frontend"
            engine?.paused("frontend")
        }
    }

    override fun handleResume(r: EcpEngine.Request) {
        r.ok()
        if (pausedBy != null) {
            pausedBy = null
            held.clear() // buttons held now are ignored until released (section 6.1)
            engine?.resumed()
        }
    }

    private fun needsGame(r: EcpEngine.Request): Boolean {
        if (!running) r.error("busy", "The game is not running yet.")
        return running
    }

    override fun handleSaveState(r: EcpEngine.Request) {
        val slot = r.int("slot")
        if (!needsGame(r)) return
        if (slot !in 0..99) return r.error("protocol", null, "slot must be 0-99")
        val time = game.save(slot)
        r.ok(JSONObject().put("slot", slot).put("time", time).put("thumbnail", Ecp.picture(game.render())))
    }

    override fun handleLoadState(r: EcpEngine.Request) {
        if (!needsGame(r)) return
        val state = try {
            game.read(r.int("slot"))
        } catch (e: IllegalStateException) {
            return r.error("state_incompatible", "That state is from another version.")
        } ?: return r.error("slot_empty", "That slot is empty.")
        game.frame = state.first
        r.ok()
    }

    override fun handleListStates(r: EcpEngine.Request) {
        if (!needsGame(r)) return
        val list = JSONArray()
        for (slot in 0..99) {
            val s = try { game.read(slot) } catch (e: IllegalStateException) { null } ?: continue
            list.put(JSONObject().put("slot", slot).put("time", s.second).put("thumbnail", Ecp.picture(game.render(s.first))))
        }
        r.ok(JSONObject().put("states", list))
    }

    override fun handleDeleteState(r: EcpEngine.Request) {
        if (!needsGame(r)) return
        if (game.delete(r.int("slot"))) r.ok() else r.error("slot_empty", "That slot is empty.")
    }

    override fun handleGetSchema(r: EcpEngine.Request) = r.ok(JSONObject().put("settings", game.schema()))
    override fun handleGetSettings(r: EcpEngine.Request) = r.ok(JSONObject().put("values", game.valuesJson()))

    override fun handleSetSetting(r: EcpEngine.Request) {
        val key = r.string("key").orEmpty()
        val why = game.apply(key, r.msg.opt("value"))
        if (why != null) return r.error(if (why == "unknown key") "unsupported" else "protocol", null, why)
        r.ok(JSONObject().put("applied", game.applies(key)))
    }

    override fun handleReset(r: EcpEngine.Request) {
        if (!needsGame(r)) return
        game.frame = 0
        r.ok()
    }

    // set_speed sets the whole speed state: fast_forward false, factor 1, or neither is normal.
    override fun handleSetSpeed(r: EcpEngine.Request) {
        val factor = r.double("factor", 1.0)
        fastForward = r.bool("fast_forward")
        slowFactor = 1.0
        if (fastForward && factor > 1) game.apply("speed.fast_forward_factor", factor.coerceAtMost(10.0))
        else if (!fastForward && factor > 0 && factor < 1) slowFactor = factor.coerceAtLeast(0.1)
        r.ok()
    }

    override fun handleSetRewind(r: EcpEngine.Request) {
        rewind = r.bool("on")
        r.ok()
    }

    override fun handleScreenshot(r: EcpEngine.Request) {
        if (!needsGame(r)) return
        r.ok(JSONObject().put("image", Ecp.picture(game.render(scale = game.number("video.internal_scale").toInt()))))
    }

    override fun handleSetVolume(r: EcpEngine.Request) {
        if (r.msg.has("percent")) game.apply("audio.volume", r.double("percent").coerceIn(0.0, 100.0))
        r.ok()
    }

    override fun handleHudLevel(r: EcpEngine.Request) {
        val level = r.int("level")
        if (level !in 0..3) return r.error("protocol", null, "level must be 0-3")
        hud = level
        r.ok()
    }

    override fun handleNotify(r: EcpEngine.Request) {
        showToast(r.string("text").orEmpty(), r.double("seconds", 3.0))
        r.ok()
    }

    override fun handleSetCheats(r: EcpEngine.Request) {
        val n = r.msg.optJSONArray("cheats")?.length() ?: 0
        showToast("$n cheats")
        r.ok()
    }

    override fun handleListDiscs(r: EcpEngine.Request) {
        val discs = JSONArray()
        for (i in 0..1) discs.put(JSONObject().put("index", i).put("label", "Disc ${i + 1}"))
        r.ok(JSONObject().put("discs", discs).put("current", disc))
    }

    override fun handleSetDisc(r: EcpEngine.Request) {
        val index = r.int("index")
        if (index !in 0..1) return r.error("content_invalid", "There is no such disc.")
        disc = index
        r.ok()
    }

    // ---- drawing

    private inner class PatternView(context: Context) : View(context) {
        private val paint = Paint().apply { isFilterBitmap = false }
        private val text = Paint().apply {
            color = Color.WHITE
            textSize = 36f
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
        }

        override fun onDraw(canvas: Canvas) {
            canvas.drawColor(Color.BLACK)
            val bmp = game.render()
            val aspect = when (game.text("video.aspect")) {
                "16:9" -> 16f / 9f
                "stretch" -> width.toFloat() / height
                else -> 4f / 3f
            }
            val w = minOf(width.toFloat(), height * aspect)
            val h = w / aspect
            val left = (width - w) / 2
            val top = (height - h) / 2
            canvas.drawBitmap(bmp, null, Rect(left.toInt(), top.toInt(), (left + w).toInt(), (top + h).toInt()), paint)
            bmp.recycle()
            var y = 48f
            if (hud > 0) canvas.drawText("%.0f fps".format(fps), 24f, y.also { y += 44f }, text)
            if (pausedBy != null) canvas.drawText("Paused ($pausedBy)", 24f, y.also { y += 44f }, text)
            if (!running) canvas.drawText(if (engine != null) "Waiting for the frontend" else "", 24f, y, text)
            toast?.let { if (SystemClock.uptimeMillis() < toastUntil) canvas.drawText(it, 24f, height - 48f, text) }
        }
    }

    private companion object {
        val KEYS = mapOf(
            "a" to KeyEvent.KEYCODE_BUTTON_B, "b" to KeyEvent.KEYCODE_BUTTON_A,
            "x" to KeyEvent.KEYCODE_BUTTON_Y, "y" to KeyEvent.KEYCODE_BUTTON_X,
            "l" to KeyEvent.KEYCODE_BUTTON_L1, "r" to KeyEvent.KEYCODE_BUTTON_R1,
            "l2" to KeyEvent.KEYCODE_BUTTON_L2, "r2" to KeyEvent.KEYCODE_BUTTON_R2,
            "l3" to KeyEvent.KEYCODE_BUTTON_THUMBL, "r3" to KeyEvent.KEYCODE_BUTTON_THUMBR,
            "select" to KeyEvent.KEYCODE_BUTTON_SELECT, "start" to KeyEvent.KEYCODE_BUTTON_START,
            "up" to KeyEvent.KEYCODE_DPAD_UP, "down" to KeyEvent.KEYCODE_DPAD_DOWN,
            "left" to KeyEvent.KEYCODE_DPAD_LEFT, "right" to KeyEvent.KEYCODE_DPAD_RIGHT,
        )
    }
}
