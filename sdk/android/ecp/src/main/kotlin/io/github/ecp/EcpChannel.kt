package io.github.ecp

import org.json.JSONException
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

/** Framing over a socket: a 4-byte little-endian length, then UTF-8 JSON (section 4). */
internal class EcpChannel(private val socket: Socket) {
    private val input = DataInputStream(BufferedInputStream(socket.getInputStream()))
    private val output = BufferedOutputStream(socket.getOutputStream())
    private val open = AtomicBoolean(true)

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

    fun send(msg: JSONObject): Boolean {
        val body = msg.toString().toByteArray(Charsets.UTF_8)
        if (body.size > Ecp.MAX_FRAME) return false
        val n = body.size
        val hdr = byteArrayOf(n.toByte(), (n shr 8).toByte(), (n shr 16).toByte(), (n shr 24).toByte())
        synchronized(output) {
            if (!open.get()) return false
            return try {
                output.write(hdr)
                output.write(body)
                output.flush()
                true
            } catch (_: IOException) {
                close()
                false
            }
        }
    }

    /** Reads on a daemon thread until the channel closes. */
    fun startReader(name: String, onMessage: (JSONObject) -> Unit, onClosed: () -> Unit) {
        Thread({
            while (true) onMessage(read() ?: break)
            onClosed()
        }, name).apply { isDaemon = true }.start()
    }

    fun close() {
        if (open.getAndSet(false)) {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }
}
