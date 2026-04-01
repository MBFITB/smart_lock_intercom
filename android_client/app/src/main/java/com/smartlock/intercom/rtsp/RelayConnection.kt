package com.smartlock.intercom.rtsp

import android.util.Log
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Connects to the relay server and issues CONNECT <device_id>.
 * Returns the raw Socket that is now bridged to the device.
 *
 * After calling connect(), the returned socket speaks RTSP directly
 * to the device as if it were a direct TCP connection.
 */
class RelayConnection(
    private val relayHost: String,
    private val relayPort: Int = 9100,
    private val deviceId: String
) {
    companion object {
        private const val TAG = "RelayConnection"
        private const val CONNECT_TIMEOUT_MS = 10000
    }

    /**
     * Connect to relay and request bridge to device.
     * Returns the connected Socket on success.
     * Throws IOException on failure.
     */
    fun connect(): Socket {
        val socket = Socket()
        try {
            socket.connect(InetSocketAddress(relayHost, relayPort), CONNECT_TIMEOUT_MS)
            socket.tcpNoDelay = true

            val output = socket.getOutputStream()
            val input = socket.getInputStream()

            // Send CONNECT command
            val cmd = "CONNECT $deviceId\r\n"
            output.write(cmd.toByteArray(Charsets.US_ASCII))
            output.flush()

            // Read response line
            val response = readLine(input)
            Log.d(TAG, "Relay response: $response")

            if (response.startsWith("OK")) {
                Log.i(TAG, "Relay bridge established to device '$deviceId'")
                return socket
            }

            val errorMsg = if (response.startsWith("ERROR "))
                response.substring(6)
            else
                "Unknown relay error: $response"

            socket.close()
            throw IOException("Relay: $errorMsg")

        } catch (e: Exception) {
            try { socket.close() } catch (_: Exception) {}
            throw if (e is IOException) e
            else IOException("Relay connection failed: ${e.message}", e)
        }
    }

    private fun readLine(input: InputStream): String {
        val sb = StringBuilder()
        while (true) {
            val b = input.read()
            if (b < 0) throw IOException("Relay closed connection")
            if (b == '\n'.code) {
                if (sb.isNotEmpty() && sb.last() == '\r')
                    sb.deleteCharAt(sb.length - 1)
                break
            }
            sb.append(b.toChar())
            if (sb.length > 256) throw IOException("Relay response too long")
        }
        return sb.toString()
    }
}
