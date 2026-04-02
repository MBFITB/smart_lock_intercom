package com.smartlock.intercom.webrtc

import android.util.Log
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import org.webrtc.*
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * WebRTC client that connects to go2rtc for cloud-based video/audio streaming.
 * Sends SDP offer to go2rtc HTTP API, receives answer, establishes peer connection.
 * Receive-only (video + audio), no send tracks.
 */
class WebRtcClient(
    private val eglBase: EglBase,
    private val appContext: android.content.Context
) {

    companion object {
        private const val TAG = "WebRtcClient"
    }

    enum class State { DISCONNECTED, CONNECTING, CONNECTED, PLAYING, ERROR }

    interface Listener {
        fun onStateChanged(state: State)
        fun onError(message: String)
    }

    var listener: Listener? = null

    @Volatile
    var state: State = State.DISCONNECTED
        private set(value) {
            field = value
            listener?.onStateChanged(value)
        }

    private var peerConnectionFactory: PeerConnectionFactory? = null
    private var peerConnection: PeerConnection? = null
    private var connectThread: Thread? = null

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .writeTimeout(10, TimeUnit.SECONDS)
        .build()

    fun init() {
        val initOptions = PeerConnectionFactory.InitializationOptions.builder(appContext)
            .setEnableInternalTracer(false)
            .createInitializationOptions()
        PeerConnectionFactory.initialize(initOptions)

        val encoderFactory = DefaultVideoEncoderFactory(eglBase.eglBaseContext, true, true)
        val decoderFactory = DefaultVideoDecoderFactory(eglBase.eglBaseContext)

        peerConnectionFactory = PeerConnectionFactory.builder()
            .setVideoEncoderFactory(encoderFactory)
            .setVideoDecoderFactory(decoderFactory)
            .createPeerConnectionFactory()
    }

    /**
     * Connect to go2rtc stream via WebRTC.
     * @param go2rtcUrl Base URL like "http://192.168.10.25:3100"
     * @param streamName Stream name configured in go2rtc.yaml, e.g. "smartlock"
     * @param renderer SurfaceViewRenderer to display video
     */
    fun connect(go2rtcUrl: String, streamName: String, renderer: SurfaceViewRenderer) {
        if (state != State.DISCONNECTED) return

        state = State.CONNECTING

        val factory = peerConnectionFactory ?: run {
            state = State.ERROR
            listener?.onError("PeerConnectionFactory not initialized")
            return
        }

        val iceServers = listOf(
            PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer()
        )
        val rtcConfig = PeerConnection.RTCConfiguration(iceServers).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        }

        val pc = factory.createPeerConnection(rtcConfig, object : PeerConnection.Observer {
            override fun onSignalingChange(state: PeerConnection.SignalingState?) {}
            override fun onIceConnectionChange(iceState: PeerConnection.IceConnectionState?) {
                Log.i(TAG, "ICE connection: $iceState")
                when (iceState) {
                    PeerConnection.IceConnectionState.CONNECTED,
                    PeerConnection.IceConnectionState.COMPLETED -> {
                        if (this@WebRtcClient.state == State.CONNECTED) {
                            this@WebRtcClient.state = State.PLAYING
                        }
                    }
                    PeerConnection.IceConnectionState.DISCONNECTED -> {
                        this@WebRtcClient.state = State.ERROR
                        listener?.onError("ICE disconnected")
                    }
                    PeerConnection.IceConnectionState.FAILED -> {
                        this@WebRtcClient.state = State.ERROR
                        listener?.onError("ICE connection failed")
                    }
                    else -> {}
                }
            }
            override fun onIceConnectionReceivingChange(receiving: Boolean) {}
            override fun onIceGatheringChange(state: PeerConnection.IceGatheringState?) {}
            override fun onIceCandidate(candidate: IceCandidate?) {}
            override fun onIceCandidatesRemoved(candidates: Array<out IceCandidate>?) {}
            override fun onAddStream(stream: MediaStream?) {}
            override fun onRemoveStream(stream: MediaStream?) {}
            override fun onDataChannel(dc: DataChannel?) {}
            override fun onRenegotiationNeeded() {}
            override fun onAddTrack(receiver: RtpReceiver?, streams: Array<out MediaStream>?) {
                val track = receiver?.track() ?: return
                Log.i(TAG, "onAddTrack: ${track.kind()}")
                if (track is VideoTrack) {
                    track.addSink(renderer)
                }
                // AudioTrack plays automatically via WebRTC's built-in audio device
            }
        }) ?: run {
            state = State.ERROR
            listener?.onError("Failed to create PeerConnection")
            return
        }
        peerConnection = pc

        // Add receive-only transceivers
        pc.addTransceiver(
            MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO
        )?.direction = RtpTransceiver.RtpTransceiverDirection.RECV_ONLY

        pc.addTransceiver(
            MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO
        )?.direction = RtpTransceiver.RtpTransceiverDirection.RECV_ONLY

        // Create offer and exchange SDP with go2rtc
        connectThread = Thread({
            try {
                exchangeSdp(pc, go2rtcUrl, streamName)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
                Log.i(TAG, "SDP exchange interrupted")
            } catch (e: Exception) {
                Log.e(TAG, "SDP exchange failed", e)
                state = State.ERROR
                listener?.onError("SDP exchange failed: ${e.message}")
            }
        }, "WebRtcSignaling")
        connectThread!!.start()
    }

    private fun exchangeSdp(pc: PeerConnection, go2rtcUrl: String, streamName: String) {
        // Create offer
        val offerLatch = java.util.concurrent.CountDownLatch(1)
        var offerSdp: SessionDescription? = null

        pc.createOffer(object : SdpObserver {
            override fun onCreateSuccess(sdp: SessionDescription?) {
                offerSdp = sdp
                offerLatch.countDown()
            }
            override fun onCreateFailure(error: String?) {
                Log.e(TAG, "Create offer failed: $error")
                offerLatch.countDown()
            }
            override fun onSetSuccess() {}
            override fun onSetFailure(error: String?) {}
        }, MediaConstraints())

        offerLatch.await(10, TimeUnit.SECONDS)
        val offer = offerSdp ?: throw IOException("Failed to create SDP offer")

        // Set local description
        val setLocalLatch = java.util.concurrent.CountDownLatch(1)
        pc.setLocalDescription(object : SdpObserver {
            override fun onSetSuccess() { setLocalLatch.countDown() }
            override fun onSetFailure(error: String?) {
                Log.e(TAG, "Set local description failed: $error")
                setLocalLatch.countDown()
            }
            override fun onCreateSuccess(sdp: SessionDescription?) {}
            override fun onCreateFailure(error: String?) {}
        }, offer)
        setLocalLatch.await(5, TimeUnit.SECONDS)

        // POST offer SDP to go2rtc
        val url = "${go2rtcUrl.trimEnd('/')}/api/webrtc?src=$streamName"
        val json = JSONObject().apply {
            put("type", "offer")
            put("sdp", offer.description)
        }

        val request = Request.Builder()
            .url(url)
            .post(json.toString().toRequestBody("application/json".toMediaType()))
            .build()

        val response = try {
            httpClient.newCall(request).execute()
        } catch (e: java.net.ConnectException) {
            throw IOException("无法连接 go2rtc — 请确保服务器正在运行", e)
        } catch (e: java.net.SocketTimeoutException) {
            throw IOException("go2rtc 连接超时", e)
        }

        val body: String
        try {
            body = response.body?.string() ?: throw IOException("go2rtc 返回空响应")
            if (!response.isSuccessful) {
                throw IOException("go2rtc returned ${response.code}: $body")
            }
        } finally {
            response.close()
        }

        val answerJson = try {
            JSONObject(body)
        } catch (e: org.json.JSONException) {
            throw IOException("go2rtc 返回无效 JSON: ${e.message}")
        }
        val sdpStr = if (answerJson.has("sdp")) answerJson.getString("sdp")
            else throw IOException("go2rtc 响应缺少 sdp 字段")

        val answerSdp = SessionDescription(SessionDescription.Type.ANSWER, sdpStr)

        state = State.CONNECTED

        // Set remote description
        val setRemoteLatch = java.util.concurrent.CountDownLatch(1)
        var setRemoteError: String? = null
        pc.setRemoteDescription(object : SdpObserver {
            override fun onSetSuccess() { setRemoteLatch.countDown() }
            override fun onSetFailure(error: String?) {
                setRemoteError = error
                setRemoteLatch.countDown()
            }
            override fun onCreateSuccess(sdp: SessionDescription?) {}
            override fun onCreateFailure(error: String?) {}
        }, answerSdp)
        setRemoteLatch.await(5, TimeUnit.SECONDS)

        if (setRemoteError != null) {
            throw IOException("Set remote description failed: $setRemoteError")
        }

        Log.i(TAG, "WebRTC signaling complete, waiting for ICE connection")
    }

    fun disconnect() {
        listener = null
        connectThread?.interrupt()
        connectThread = null

        peerConnection?.close()
        peerConnection = null

        state = State.DISCONNECTED
    }

    fun release() {
        disconnect()
        try {
            connectThread?.join(5000)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        }
        peerConnectionFactory?.dispose()
        peerConnectionFactory = null
    }
}
