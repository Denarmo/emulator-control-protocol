package io.github.ecp

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/** EcpEngine and EcpSession talking to each other over loopback, on the JVM. */
class LoopbackTest {
    private val direct = Executor { it.run() }
    private val descriptor = JSONObject(
        """{"ecp":1,"id":"io.github.ecp.test","name":"Test","version":"1.0","level":2,
           "systems":["gba"],"capabilities":["pause","states","menu"]}""",
    )

    private class Events : EcpSession.Listener {
        val ready = CountDownLatch(1)
        val firstFrame = CountDownLatch(1)
        val paused = CountDownLatch(1)
        val stopped = CountDownLatch(1)
        val closed = CountDownLatch(1)
        var closeReason = ""

        override fun onReady(session: EcpSession, engine: JSONObject) = ready.countDown()
        override fun onEvent(session: EcpSession, type: String, msg: JSONObject) {
            when (type) {
                "first_frame" -> firstFrame.countDown()
                "paused" -> if (msg.optString("reason") == "frontend") paused.countDown()
                "stopped" -> stopped.countDown()
            }
        }
        override fun onClosed(session: EcpSession, reason: String) {
            closeReason = reason
            closed.countDown()
        }
    }

    private fun CountDownLatch.waitOk() = assertTrue(await(5, TimeUnit.SECONDS))

    @Test
    fun session() {
        val events = Events()
        val session = EcpSession(events, direct).also { it.wanted = setOf("pause", "states") }
        session.acceptInBackground()

        lateinit var engine: EcpEngine
        val callbacks = object : EcpEngine.Callbacks {
            override fun handleLaunch(r: EcpEngine.Request) {
                assertEquals("game.gba", r.string("content"))
                r.ok()
                engine.firstFrame()
            }
            override fun handleStop(r: EcpEngine.Request) {
                r.ok()
                engine.stopped("frontend")
            }
            override fun handlePause(r: EcpEngine.Request) {
                r.ok()
                engine.paused("frontend")
            }
            override fun handleSaveState(r: EcpEngine.Request) =
                r.ok(JSONObject().put("slot", r.int("slot")).put("time", 1_700_000_000L))
        }
        engine = EcpEngine(descriptor, callbacks, direct)
        engine.connect(EcpEngine.Session(session.port, session.token, session.id, null, true, null, null))

        events.ready.waitOk()
        assertEquals(setOf("pause", "states"), session.capabilities)
        session.request("launch", JSONObject().put("content", "game.gba").put("game_id", "g"))
        events.firstFrame.waitOk()
        assertTrue(engine.frontendHas("pause"))
        assertTrue("menu was not taken by the frontend", !engine.frontendHas("menu"))

        session.request("pause")
        events.paused.waitOk()
        assertEquals(3, session.request("save_state", JSONObject().put("slot", 3)).getInt("slot"))

        expectError("unsupported") { session.request("load_state", JSONObject().put("slot", 3)) } // no callback
        expectError("unsupported") { session.request("reset") }                                    // not listed
        expectError("unsupported") { session.request("no_such_request") }

        session.request("stop")
        events.stopped.waitOk()
        session.close()
        events.closed.waitOk()
    }

    @Test
    fun wrongTokenIsRefused() {
        val events = Events()
        val session = EcpSession(events, direct)
        session.acceptInBackground()
        val closed = CountDownLatch(1)
        val engine = EcpEngine(descriptor, object : EcpEngine.Callbacks {
            override fun handleLaunch(r: EcpEngine.Request) = r.ok()
            override fun handleStop(r: EcpEngine.Request) = r.ok()
            override fun onClosed(reason: String?) = closed.countDown()
        }, direct)
        engine.connect(EcpEngine.Session(session.port, "00".repeat(32), null, null, true, null, null))
        events.closed.waitOk()
        assertTrue(events.closeReason, events.closeReason.contains("token"))
        closed.waitOk()
    }

    @Test
    fun stopIsNotQueuedBehindASlowRequest() {
        val events = Events()
        val session = EcpSession(events, direct)
        session.acceptInBackground()
        val saving = CountDownLatch(1)
        val release = CountDownLatch(1)
        val menuOnly = JSONObject(descriptor.toString())
            .put("capabilities", org.json.JSONArray(listOf("menu", "states")))
        val engine = EcpEngine(menuOnly, object : EcpEngine.Callbacks {   // default threads
            override fun handleLaunch(r: EcpEngine.Request) = r.ok()
            override fun handleStop(r: EcpEngine.Request) = r.ok()
            override fun handleResume(r: EcpEngine.Request) = r.ok()
            override fun handleSaveState(r: EcpEngine.Request) {
                saving.countDown()
                release.await(10, TimeUnit.SECONDS) // a state save that blocks on the emulator
                r.ok(JSONObject().put("slot", 1).put("time", 1L))
            }
        })
        engine.connect(EcpEngine.Session(session.port, session.token, null, null, true, null, null))
        events.ready.waitOk()
        session.request("launch", JSONObject().put("content", "x"))
        session.request("resume") // valid with menu alone (SPEC 6.2)

        val saved = CountDownLatch(1)
        session.requestAsync("save_state", JSONObject().put("slot", 1), 10_000) { saved.countDown() }
        saving.waitOk()
        session.request("stop", timeoutMs = 1000) // answered while save_state still runs
        release.countDown()
        saved.waitOk()
        session.close()
    }

    private fun expectError(code: String, block: () -> Unit) {
        try {
            block()
            fail("expected $code")
        } catch (e: EcpException) {
            assertEquals(code, e.code)
        }
    }
}
