package com.smartlock.intercom.rtsp

import android.util.Log
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.security.MessageDigest
import java.util.Timer
import java.util.TimerTask
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.random.Random

/**
 * RTSP/TCP client implementing ONVIF Profile T backchannel.
 *
 * Handles: OPTIONS, DESCRIBE, SETUP (3 tracks), PLAY, TEARDOWN,
 * TCP interleaved frame parsing, and backchannel RTP sending.
 */
class RtspClient(
    private val host: String,
    private val port: Int = 8555,
    private val path: String = "/live",
    private val username: String? = null,
    private val password: String? = null
) {
    companion object {
        private const val TAG = "RtspClient"
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val READ_TIMEOUT_MS = 10000
        private const val MAX_FRAME_SIZE = 100000  // 100KB max RTP frame
        private const val KEEPALIVE_INTERVAL_MS = 45000L  // 45s keepalive
        private const val PT_H264 = 107
        private const val PT_PCMU = 0
        private val CONTENT_LENGTH_RE = Regex("Content-Length:\\s*(\\d+)", RegexOption.IGNORE_CASE)
        private val CONTENT_BASE_RE = Regex("Content-Base:\\s*(\\S+)", RegexOption.IGNORE_CASE)
        private val SESSION_RE = Regex("Session:\\s*([^;\\r\\n]+)")
        private val CONTROL_RE = Regex("a=control:(\\S+)")
        private val RTPMAP_RE = Regex("a=rtpmap:\\d+\\s+\\S+/(\\d+)")
        private val SPROP_RE = Regex("sprop-parameter-sets=([^;\\s]+)")
        private val WWW_AUTH_RE = Regex("WWW-Authenticate:\\s*Digest\\s+(.*)", RegexOption.IGNORE_CASE)
        private val AUTH_FIELD_RE = Regex("(\\w+)=\"([^\"]*)\"")
    }

    interface Listener {
        /** Called with raw H.264 RTP payload (NAL units, after RTP header strip) */
        fun onVideoRtp(data: ByteArray, offset: Int, length: Int, timestamp: Long, marker: Boolean)
        /** Called with G.711 µ-law PCM data (after RTP header strip) */
        fun onAudioRtp(data: ByteArray, offset: Int, length: Int, timestamp: Long)
        /** Connection state changed */
        fun onStateChanged(state: State)
        /** Error occurred */
        fun onError(message: String)
    }

    enum class State {
        DISCONNECTED, CONNECTING, CONNECTED, PLAYING, ERROR
    }

    // SDP parsed info
    data class TrackInfo(
        val controlUrl: String,
        val payloadType: Int,
        val clockRate: Int,
        val direction: String,   // "sendonly" or "recvonly"
        val spropSps: ByteArray? = null,
        val spropPps: ByteArray? = null
    )

    private var socket: Socket? = null
    private var inputStream: InputStream? = null
    private var outputStream: OutputStream? = null

    private val cseq = AtomicInteger(0)
    private var sessionId: String? = null
    private var contentBase: String? = null
    private val running = AtomicBoolean(false)
    private var readThread: Thread? = null
    private var keepaliveTimer: Timer? = null

    var listener: Listener? = null
    @Volatile
    var state: State = State.DISCONNECTED
        private set(value) {
            field = value
            listener?.onStateChanged(value)
        }

    // Track info parsed from SDP
    private var videoTrack: TrackInfo? = null
    private var audioTrack: TrackInfo? = null
    private var backchannelTrack: TrackInfo? = null

    // Channel assignments
    private val videoRtpCh: Byte = 0
    private val videoRtcpCh: Byte = 1
    private val audioRtpCh: Byte = 2
    private val audioRtcpCh: Byte = 3
    private val backchRtpCh: Byte = 4
    private val backchRtcpCh: Byte = 5

    // Backchannel RTP state
    private var backchSeq: Short = 0
    private var backchTimestamp: Long = Random.nextLong(0, 0xFFFFFFFFL)
    private var backchSsrc: Int = Random.nextInt()
    private var backchFirstPacket: Boolean = true

    // Digest authentication state
    @Volatile private var authRealm: String? = null
    @Volatile private var authNonce: String? = null

    fun connect() {
        if (state != State.DISCONNECTED) return
        state = State.CONNECTING

        Thread({
            try {
                doConnect(null)
            } catch (e: Exception) {
                Log.e(TAG, "Connect failed", e)
                state = State.ERROR
                listener?.onError(e.message ?: "Unknown error")
                disconnect()
            }
        }, "RtspConnect").start()
    }

    /**
     * Connect via a pre-established Socket (e.g. from relay bridge).
     * The socket must already be connected and ready for RTSP.
     */
    fun connectViaSocket(relaySocket: Socket) {
        if (state != State.DISCONNECTED) return
        state = State.CONNECTING

        Thread({
            try {
                doConnect(relaySocket)
            } catch (e: Exception) {
                Log.e(TAG, "Relay connect failed", e)
                state = State.ERROR
                listener?.onError(e.message ?: "Unknown error")
                disconnect()
            }
        }, "RtspRelayConnect").start()
    }

    private fun doConnect(existingSocket: Socket?) {
        // 1. TCP connect (or use existing relay socket)
        val sock: Socket
        if (existingSocket != null) {
            sock = existingSocket
            sock.tcpNoDelay = true
        } else {
            sock = Socket()
            try {
                sock.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
                sock.soTimeout = READ_TIMEOUT_MS
                sock.tcpNoDelay = true
            } catch (e: Exception) {
                try { sock.close() } catch (_: Exception) {}
                throw e
            }
        }

        socket = sock
        inputStream = sock.getInputStream()
        outputStream = sock.getOutputStream()

        // 2. OPTIONS
        sendRequest("OPTIONS", "$path", null)
        readResponse()  // just check 200 OK (OPTIONS doesn't require auth)

        // 3. DESCRIBE (may trigger 401 challenge)
        val descResp = sendAuthRequest("DESCRIBE", "$path", "Accept: application/sdp\r\n")
        parseSdp(descResp.body)

        // 4. SETUP video (trackID=0, ch 0-1)
        val videoUrl = resolveTrackUrl(videoTrack?.controlUrl ?: "trackID=0")
        val setupResp = sendAuthRequest("SETUP", videoUrl,
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
        if (sessionId == null) {
            sessionId = parseSessionId(setupResp.headers)
            if (sessionId.isNullOrEmpty()) {
                throw IOException("Server did not return Session header")
            }
        }

        // 5. SETUP audio (trackID=1, ch 2-3)
        val audioUrl = resolveTrackUrl(audioTrack?.controlUrl ?: "trackID=1")
        sendAuthRequest("SETUP", audioUrl,
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n" +
            "Session: $sessionId\r\n")

        // 6. SETUP backchannel (trackID=2, ch 4-5)
        if (backchannelTrack != null) {
            val backchUrl = resolveTrackUrl(backchannelTrack?.controlUrl ?: "trackID=2")
            sendAuthRequest("SETUP", backchUrl,
                "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n" +
                "Session: $sessionId\r\n")
        }

        state = State.CONNECTED

        // 7. PLAY
        sendAuthRequest("PLAY", "$path",
            "Session: $sessionId\r\n" +
            "Range: npt=0.000-\r\n")

        state = State.PLAYING

        // 8. Start keepalive timer (prevent NAT mapping timeout)
        startKeepalive()

        // 9. Start reading interleaved frames
        running.set(true)
        readThread = Thread({
            readLoop()
        }, "RtspRead")
        readThread!!.start()
    }

    private fun readLoop() {
        val buf = ByteArray(65536)
        val input = inputStream ?: return

        try {
            // Keep timeout to detect stale connections
            socket?.soTimeout = 30000

            while (running.get()) {
                val first = input.read()
                if (first < 0) {
                    if (running.get()) listener?.onError("Server closed connection")
                    break
                }

                if (first == 0x24) {  // '$' interleaved frame
                    val channel = readByte(input)
                    val lenHi = readByte(input)
                    val lenLo = readByte(input)
                    val frameLen = (lenHi shl 8) or lenLo

                    if (frameLen > MAX_FRAME_SIZE || frameLen < 12) {
                        // Skip oversized or too-small frames
                        Log.w(TAG, "Skipping frame: len=$frameLen")
                        skipBytes(input, frameLen)
                        continue
                    }

                    if (frameLen > buf.size) {
                        skipBytes(input, frameLen)
                        continue
                    }

                    readFully(input, buf, 0, frameLen)
                    dispatchRtpFrame(channel, buf, frameLen)

                } else if (first == 0x52) {  // 'R' — possible RTSP response
                    // Unexpected RTSP response during play — skip it
                    // Read until \r\n\r\n
                    skipRtspMessage(input, first)
                }
            }
        } catch (e: IOException) {
            if (running.get()) {
                Log.e(TAG, "Read loop error", e)
                listener?.onError("Connection lost: ${e.message}")
            }
        } finally {
            if (running.get()) {
                running.set(false)
                state = State.ERROR
            }
        }
    }

    private fun dispatchRtpFrame(channel: Int, buf: ByteArray, frameLen: Int) {
        // Parse RTP header
        val cc = buf[0].toInt() and 0x0F
        val ext = (buf[0].toInt() shr 4) and 0x01
        val marker = (buf[1].toInt() and 0x80) != 0
        val pt = buf[1].toInt() and 0x7F

        val timestamp = ((buf[4].toLong() and 0xFF) shl 24) or
                ((buf[5].toLong() and 0xFF) shl 16) or
                ((buf[6].toLong() and 0xFF) shl 8) or
                (buf[7].toLong() and 0xFF)

        // Calculate variable RTP header length
        var hdrLen = 12 + cc * 4
        if (ext != 0 && frameLen >= hdrLen + 4) {
            val extLen = ((buf[hdrLen + 2].toInt() and 0xFF) shl 8) or
                    (buf[hdrLen + 3].toInt() and 0xFF)
            hdrLen += 4 + extLen * 4
        }

        if (hdrLen >= frameLen) return

        val payloadOff = hdrLen
        val payloadLen = frameLen - hdrLen

        when (channel) {
            videoRtpCh.toInt() -> {
                listener?.onVideoRtp(buf, payloadOff, payloadLen, timestamp, marker)
            }
            audioRtpCh.toInt() -> {
                listener?.onAudioRtp(buf, payloadOff, payloadLen, timestamp)
            }
            // RTCP channels (1, 3, 5) — ignored
        }
    }

    /**
     * Send backchannel audio as TCP interleaved RTP.
     * @param pcmuData G.711 µ-law encoded samples
     */
    fun sendBackchannelAudio(pcmuData: ByteArray, offset: Int, length: Int) {
        if (state != State.PLAYING || backchannelTrack == null) return

        val rtpLen = 12 + length
        val frame = ByteArray(4 + rtpLen)

        // TCP interleaved header: $ + channel + length(BE)
        frame[0] = 0x24  // '$'
        frame[1] = backchRtpCh
        frame[2] = ((rtpLen shr 8) and 0xFF).toByte()
        frame[3] = (rtpLen and 0xFF).toByte()

        // RTP header
        frame[4] = 0x80.toByte()  // V=2, P=0, X=0, CC=0
        val markerBit = if (backchFirstPacket) { backchFirstPacket = false; 0x80 } else 0
        frame[5] = (markerBit or PT_PCMU).toByte()
        frame[6] = ((backchSeq.toInt() shr 8) and 0xFF).toByte()
        frame[7] = (backchSeq.toInt() and 0xFF).toByte()
        frame[8]  = ((backchTimestamp shr 24) and 0xFF).toByte()
        frame[9]  = ((backchTimestamp shr 16) and 0xFF).toByte()
        frame[10] = ((backchTimestamp shr 8) and 0xFF).toByte()
        frame[11] = (backchTimestamp and 0xFF).toByte()
        frame[12] = ((backchSsrc shr 24) and 0xFF).toByte()
        frame[13] = ((backchSsrc shr 16) and 0xFF).toByte()
        frame[14] = ((backchSsrc shr 8) and 0xFF).toByte()
        frame[15] = (backchSsrc and 0xFF).toByte()

        // Payload
        System.arraycopy(pcmuData, offset, frame, 16, length)

        backchSeq++
        backchTimestamp += length

        try {
            synchronized(this) {
                outputStream?.write(frame)
                outputStream?.flush()
            }
        } catch (e: IOException) {
            Log.e(TAG, "Backchannel send error", e)
        }
    }

    private fun startKeepalive() {
        stopKeepalive()
        keepaliveTimer = Timer("RtspKeepalive", true).apply {
            schedule(object : TimerTask() {
                override fun run() {
                    if (state != State.PLAYING) return
                    try {
                        Log.d(TAG, "Sending keepalive GET_PARAMETER")
                        sendRequest("GET_PARAMETER", "$path",
                            "Session: $sessionId\r\n")
                    } catch (e: Exception) {
                        Log.w(TAG, "Keepalive send failed: ${e.message}")
                    }
                }
            }, KEEPALIVE_INTERVAL_MS, KEEPALIVE_INTERVAL_MS)
        }
    }

    private fun stopKeepalive() {
        keepaliveTimer?.cancel()
        keepaliveTimer = null
    }

    fun disconnect() {
        running.set(false)
        stopKeepalive()

        try {
            if (sessionId != null && socket?.isClosed == false) {
                sendRequest("TEARDOWN", "$path",
                    "Session: $sessionId\r\n")
            }
        } catch (_: Exception) {}

        try { socket?.close() } catch (_: Exception) {}

        readThread?.join(2000)
        readThread = null
        socket = null
        inputStream = null
        outputStream = null
        sessionId = null
        contentBase = null
        videoTrack = null
        audioTrack = null
        backchannelTrack = null
        backchSeq = 0
        backchTimestamp = Random.nextLong(0, 0xFFFFFFFFL)
        backchSsrc = Random.nextInt()
        backchFirstPacket = true
        authRealm = null
        authNonce = null
        state = State.DISCONNECTED
    }

    /** Get SPS from SDP (for MediaCodec configuration) */
    fun getSps(): ByteArray? = videoTrack?.spropSps
    /** Get PPS from SDP (for MediaCodec configuration) */
    fun getPps(): ByteArray? = videoTrack?.spropPps

    // ---- Private helpers ----

    private fun sendRequest(method: String, uri: String, extraHeaders: String?) {
        val seq = cseq.incrementAndGet()
        val fullUri = if (uri.startsWith("rtsp://")) uri else "rtsp://$host:$port$uri"
        val sb = StringBuilder()
        sb.append("$method $fullUri RTSP/1.0\r\n")
        sb.append("CSeq: $seq\r\n")
        sb.append("User-Agent: SmartLockIntercom/1.0\r\n")

        // Add Digest Authorization if we have auth credentials + challenge
        val authHeader = buildAuthHeader(method, fullUri)
        if (authHeader != null) sb.append(authHeader)

        if (extraHeaders != null) sb.append(extraHeaders)
        sb.append("\r\n")

        val data = sb.toString().toByteArray(Charsets.US_ASCII)
        Log.d(TAG, ">>> $method $fullUri (CSeq=$seq)")
        synchronized(this) {
            outputStream?.write(data)
            outputStream?.flush()
        }
    }

    /**
     * Send a request and handle 401 Digest challenge transparently.
     * Retries once with credentials if server returns 401.
     */
    private fun sendAuthRequest(method: String, uri: String, extraHeaders: String?): RtspResponse {
        sendRequest(method, uri, extraHeaders)
        val resp = readResponse()

        if (resp.statusCode == 401 && username != null && password != null
            && authRealm != null && authNonce != null) {
            // Retry with digest credentials
            Log.d(TAG, "Retrying $method with digest auth")
            sendRequest(method, uri, extraHeaders)
            val retryResp = readResponse()
            if (retryResp.statusCode == 401) {
                throw IOException("Authentication failed for user $username")
            }
            return retryResp
        }

        if (resp.statusCode == 401) {
            throw IOException("Server requires authentication (use --user/--pass)")
        }

        return resp
    }

    private data class RtspResponse(
        val statusCode: Int,
        val headers: String,
        val body: String
    )

    private fun readResponse(): RtspResponse {
        val input = inputStream ?: throw IOException("Not connected")
        val sb = StringBuilder()

        // Read header portion until \r\n\r\n
        var prev = 0
        var crlfCount = 0
        while (true) {
            val b = input.read()
            if (b < 0) throw IOException("Connection closed")
            sb.append(b.toChar())

            if (b == '\n'.code && prev == '\r'.code) {
                crlfCount++
                if (crlfCount >= 2) break
            } else if (b != '\r'.code) {
                crlfCount = 0
            }
            prev = b
        }

        val header = sb.toString()
        val statusLine = header.lines().firstOrNull() ?: ""
        val statusCode = statusLine.split(" ").getOrNull(1)?.toIntOrNull() ?: 0
        Log.d(TAG, "<<< $statusLine")

        // Parse Content-Length for body
        var body = ""
        val clMatch = CONTENT_LENGTH_RE.find(header)
        if (clMatch != null) {
            val contentLen = clMatch.groupValues[1].toInt()
            if (contentLen > 0) {
                val bodyBuf = ByteArray(contentLen)
                readFully(input, bodyBuf, 0, contentLen)
                body = String(bodyBuf, Charsets.US_ASCII)
            }
        }

        // Extract Content-Base
        val cbMatch = CONTENT_BASE_RE.find(header)
        if (cbMatch != null) {
            contentBase = cbMatch.groupValues[1].trimEnd('/')
        }

        // Handle 401 Unauthorized — parse WWW-Authenticate for Digest
        if (statusCode == 401) {
            val authMatch = WWW_AUTH_RE.find(header)
            if (authMatch != null) {
                val params = authMatch.groupValues[1]
                AUTH_FIELD_RE.findAll(params).forEach { m ->
                    when (m.groupValues[1].lowercase()) {
                        "realm" -> authRealm = m.groupValues[2]
                        "nonce" -> authNonce = m.groupValues[2]
                    }
                }
                Log.d(TAG, "Got 401 challenge: realm=$authRealm")
            }
        }

        if (statusCode != 200 && statusCode != 401) {
            throw IOException("RTSP error $statusCode: $statusLine")
        }

        return RtspResponse(statusCode, header, body)
    }

    private fun parseSdp(sdp: String) {
        Log.d(TAG, "SDP:\n$sdp")

        val mediaBlocks = mutableListOf<String>()
        val lines = sdp.lines()
        var current = StringBuilder()

        for (line in lines) {
            if (line.startsWith("m=") && current.isNotEmpty()) {
                mediaBlocks.add(current.toString())
                current = StringBuilder()
            }
            current.appendLine(line)
        }
        if (current.isNotEmpty()) mediaBlocks.add(current.toString())

        for (block in mediaBlocks) {
            if (!block.startsWith("m=")) continue

            val mLine = block.lines().first()

            // Extract control URL
            val controlMatch = CONTROL_RE.find(block)
            val controlUrl = controlMatch?.groupValues?.get(1) ?: continue

            // Extract direction
            val direction = when {
                block.contains("a=recvonly") -> "recvonly"
                block.contains("a=sendonly") -> "sendonly"
                else -> "sendrecv"
            }

            // Extract payload type from m= line
            val mParts = mLine.split(" ")
            val pt = mParts.getOrNull(3)?.toIntOrNull() ?: 0

            // Extract clock rate from rtpmap
            val rtpmapMatch = RTPMAP_RE.find(block)
            val clockRate = rtpmapMatch?.groupValues?.get(1)?.toIntOrNull() ?: 0

            // Extract SPS/PPS for H.264
            var sps: ByteArray? = null
            var pps: ByteArray? = null
            val spropMatch = SPROP_RE.find(block)
            if (spropMatch != null) {
                val parts = spropMatch.groupValues[1].split(",")
                if (parts.size >= 2) {
                    sps = android.util.Base64.decode(parts[0], android.util.Base64.DEFAULT)
                    pps = android.util.Base64.decode(parts[1], android.util.Base64.DEFAULT)
                }
            }

            val track = TrackInfo(controlUrl, pt, clockRate, direction, sps, pps)

            when {
                mLine.startsWith("m=video") -> videoTrack = track
                mLine.startsWith("m=audio") && direction == "recvonly" -> backchannelTrack = track
                mLine.startsWith("m=audio") && direction == "sendonly" -> audioTrack = track
            }
        }

        Log.i(TAG, "SDP parsed: video=${videoTrack != null}, " +
                "audio=${audioTrack != null}, backchannel=${backchannelTrack != null}")
    }

    private fun resolveTrackUrl(control: String): String {
        // If control is an absolute URL, use it directly
        if (control.startsWith("rtsp://")) return control
        // Otherwise append to Content-Base or request URI
        val base = contentBase ?: "rtsp://$host:$port$path"
        return "$base/$control"
    }

    private fun parseSessionId(headers: String): String? {
        val match = SESSION_RE.find(headers)
        return match?.groupValues?.get(1)?.trim()
    }

    private fun readByte(input: InputStream): Int {
        val b = input.read()
        if (b < 0) throw IOException("Connection closed")
        return b
    }

    private fun readFully(input: InputStream, buf: ByteArray, off: Int, len: Int) {
        var remaining = len
        var pos = off
        while (remaining > 0) {
            val n = input.read(buf, pos, remaining)
            if (n < 0) throw IOException("Connection closed")
            pos += n
            remaining -= n
        }
    }

    private fun skipBytes(input: InputStream, count: Int) {
        var remaining = count
        val tmp = ByteArray(minOf(remaining, 4096))
        while (remaining > 0) {
            val n = input.read(tmp, 0, minOf(remaining, tmp.size))
            if (n < 0) break
            remaining -= n
        }
    }

    private fun skipRtspMessage(input: InputStream, firstByte: Int) {
        // Already read 'R', read until \r\n\r\n
        var crlfCount = 0
        var prev = firstByte
        while (true) {
            val b = input.read()
            if (b < 0) break
            if (b == '\n'.code && prev == '\r'.code) {
                crlfCount++
                if (crlfCount >= 2) break
            } else if (b != '\r'.code) {
                crlfCount = 0
            }
            prev = b
        }
    }

    // ---- Digest Authentication ----

    private fun buildAuthHeader(method: String, uri: String): String? {
        val user = username ?: return null
        val pass = password ?: return null
        val realm = authRealm ?: return null
        val nonce = authNonce ?: return null

        // HA1 = MD5(username:realm:password)
        val ha1 = md5Hex("$user:$realm:$pass")
        // HA2 = MD5(method:uri)
        val ha2 = md5Hex("$method:$uri")
        // response = MD5(HA1:nonce:HA2)
        val response = md5Hex("$ha1:$nonce:$ha2")

        return "Authorization: Digest username=\"$user\", " +
                "realm=\"$realm\", nonce=\"$nonce\", " +
                "uri=\"$uri\", response=\"$response\"\r\n"
    }

    private fun md5Hex(input: String): String {
        val md = MessageDigest.getInstance("MD5")
        val digest = md.digest(input.toByteArray(Charsets.US_ASCII))
        return digest.joinToString("") { "%02x".format(it) }
    }
}
