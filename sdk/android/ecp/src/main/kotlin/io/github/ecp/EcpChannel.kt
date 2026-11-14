package io.github.ecp

import org.json.JSONException
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.Socket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Framing over a socket: a 4-byte little-endian length, then UTF-8 JSON (section 4).
 * Frames are written by a thread of the channel's own, so [send] may be called from any thread,
 * the main thread included (Android forbids network writes there).
 */
internal class EcpChannel(private val socket: Socket) {
    private val input = DataInputStream(BufferedInputStream(socket.getInputStream()))
    private val output = BufferedOutputStream(socket.getOutputStream())
    private val open = AtomicBoolean(true)
    private val queue = LinkedBlockingQueue<ByteArray>()

    val isOpen: Boolean get() = open.get()

    init {
        socket.tcpNoDelay = true
    }

    /** The next object, or null once the channel is closed or broken. Frames that are not JSON objects are skipped. */
    fun read(): JSONObject? {
        while (open.get()) {
            try {
                val b = ByteArray(4)
                input.readFully(b)
                val len = (b[0].toInt() and 0xFF) or ((b[1].toInt() and 0xFF) shl 8) or
                    ((b[2].toInt() and 0xFF) shl 16) or ((b[3].toInt() and 0xFF) shl 24)
                if (len < 0 || len > Ecp.MAX_FRAME) break
                val body = ByteArray(len)
                input.readFully(body)
                return try {
                    JSONObject(String(body, Charsets.UTF_8))
                } catch (_: JSONException) {
                    continue
                }
            } catch (_: IOException) {
                break
            }
        }
        close()
        return null
    }

    /** Queues a frame; false once the channel is closed or the frame is too big. */
    fun send(msg: JSONObject): Boolean {
        val body = msg.toString().toByteArray(Charsets.UTF_8)
        if (body.size > Ecp.MAX_FRAME || !open.get()) return false
        val n = body.size
        val frame = ByteArray(4 + n)
        frame[0] = n.toByte(); frame[1] = (n shr 8).toByte(); frame[2] = (n shr 16).toByte(); frame[3] = (n shr 24).toByte()
        System.arraycopy(body, 0, frame, 4, n)
        queue.put(frame)
        return true
    }

    private val writer = Thread({
        try {
            while (true) {
                val frame = queue.take()
                if (frame.isEmpty()) break // close(): what was queued before it is written
                output.write(frame)
                if (queue.isEmpty()) output.flush()
            }
            output.flush()
        } catch (_: IOException) {
        } catch (_: InterruptedException) {
        }
        open.set(false)
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }, "ecp-writer").apply { isDaemon = true; start() }

    /** Reads on a daemon thread until the channel closes. */
    fun startReader(name: String, onMessage: (JSONObject) -> Unit, onClosed: () -> Unit) {
        Thread({
            while (true) onMessage(read() ?: break)
            onClosed()
        }, name).apply { isDaemon = true }.start()
    }

    /** Writes what was already sent, then closes the socket. */
    fun close() {
        if (open.getAndSet(false)) queue.put(ByteArray(0))
    }
}
