package com.smartlock.intercom

import android.Manifest
import android.annotation.SuppressLint
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.smartlock.intercom.databinding.ActivityMainBinding
import com.smartlock.intercom.media.AudioCapture
import com.smartlock.intercom.media.AudioPlayer
import com.smartlock.intercom.media.VideoDecoder
import com.smartlock.intercom.rtsp.RtspClient
import com.smartlock.intercom.rtsp.RelayConnection
import com.smartlock.intercom.webrtc.WebRtcClient
import org.webrtc.EglBase
import java.net.Socket

class MainActivity : AppCompatActivity(), RtspClient.Listener {

    companion object {
        private const val TAG = "MainActivity"
        private const val REQ_AUDIO_PERM = 1001
        private const val REQ_NOTIF_PERM = 1002
        private const val DEFAULT_PORT = 8555

        /** Checked by DoorbellService to avoid showing incoming call while streaming */
        @Volatile @JvmStatic
        var isStreaming = false
    }

    private lateinit var binding: ActivityMainBinding

    private var rtspClient: RtspClient? = null
    private var videoDecoder: VideoDecoder? = null
    private var audioPlayer: AudioPlayer? = null
    private var audioCapture: AudioCapture? = null
    private var surfaceReady = false
    private var pendingAutoConnect: String? = null
    @Volatile
    private var disconnecting = false
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    // WebRTC mode
    private var eglBase: EglBase? = null
    private var webRtcClient: WebRtcClient? = null
    private var isWebRtcMode = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                // Restart video decoder if RTSP is already playing (e.g. back from background)
                val client = rtspClient
                if (client != null && client.state == RtspClient.State.PLAYING) {
                    restartVideoDecoder(holder.surface)
                }
                // Handle deferred auto-connect (from incoming call)
                val host = pendingAutoConnect
                if (host != null) {
                    pendingAutoConnect = null
                    binding.addressInput.setText(host)
                    onConnectClick()
                }
            }
            override fun surfaceChanged(holder: SurfaceHolder, fmt: Int, w: Int, h: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                videoDecoder?.stop()
                videoDecoder = null
            }
        })

        binding.connectBtn.setOnClickListener { onConnectClick() }
        setupTalkButton()

        // Advanced settings toggle
        binding.advancedToggle.setOnClickListener {
            val panel = binding.advancedPanel
            if (panel.visibility == View.GONE) {
                panel.visibility = View.VISIBLE
                binding.advancedToggle.text = "▼ 高级设置"
            } else {
                panel.visibility = View.GONE
                binding.advancedToggle.text = "▶ 高级设置"
            }
        }

        // WebRTC renderer init
        eglBase = EglBase.create()
        binding.webrtcView.init(eglBase!!.eglBaseContext, null)
        binding.webrtcView.setScalingType(org.webrtc.RendererCommon.ScalingType.SCALE_ASPECT_FIT)

        // Mode switching
        setupModeListener()

        // Request audio permission early
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO_PERM)
        }

        // Request notification permission (Android 13+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                    arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIF_PERM)
            }
        }

        // Handle auto-connect from IncomingCallActivity
        handleAutoConnect(intent)
    }

    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleAutoConnect(intent)
    }

    private fun handleAutoConnect(intent: android.content.Intent?) {
        val host = intent?.getStringExtra("auto_connect_host") ?: return
        intent.removeExtra("auto_connect_host")

        if (rtspClient != null || webRtcClient != null) return  // already connected

        if (surfaceReady) {
            binding.addressInput.setText(host)
            onConnectClick()
        } else {
            // Defer until surfaceCreated callback
            pendingAutoConnect = host
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        isStreaming = false
        unregisterNetworkCallback()
        DoorbellService.stop(this)
        audioCapture?.stop()
        audioCapture = null
        rtspClient?.disconnect()
        rtspClient = null
        webRtcClient?.release()
        webRtcClient = null
        videoDecoder?.stop()
        videoDecoder = null
        audioPlayer?.stop()
        audioPlayer = null
        binding.webrtcView.release()
        eglBase?.release()
        eglBase = null
    }

    private fun onConnectClick() {
        if (disconnecting) return  // wait for previous disconnect to finish

        if (rtspClient != null || webRtcClient != null) {
            // Disconnect
            disconnectAll()
            return
        }

        if (isWebRtcMode) {
            onConnectWebRtc()
        } else {
            onConnectRtsp()
        }
    }

    private fun onConnectWebRtc() {
        val go2rtcAddr = binding.go2rtcInput.text?.toString()?.trim()
        if (go2rtcAddr.isNullOrEmpty()) {
            binding.statusText.text = "请输入 go2rtc 地址"
            return
        }

        // Validate address
        val addrHost = go2rtcAddr.split(":")[0]
        if (addrHost.isEmpty() || !addrHost.matches(Regex("^[a-zA-Z0-9._-]+$"))) {
            binding.statusText.text = "go2rtc 地址格式无效"
            return
        }

        val go2rtcUrl = if (go2rtcAddr.startsWith("http")) go2rtcAddr else "http://$go2rtcAddr"

        val egl = eglBase ?: return
        val client = WebRtcClient(egl, applicationContext)
        client.init()
        client.listener = webRtcListener
        webRtcClient = client

        binding.connectBtn.text = "断开"
        binding.addressInput.isEnabled = false
        binding.statusText.text = "连接中..."

        client.connect(go2rtcUrl, "smartlock", binding.webrtcView)
    }

    private fun onConnectRtsp() {

        val address = binding.addressInput.text?.toString()?.trim() ?: return
        if (address.isEmpty()) return

        // Parse host:port
        val parts = address.split(":")
        val host = parts[0]
        val port = if (parts.size > 1) parts[1].toIntOrNull() ?: DEFAULT_PORT else DEFAULT_PORT

        // Validate host — whitelist: alphanumeric, dots, dashes only
        if (host.isEmpty() || !host.matches(Regex("^[a-zA-Z0-9._-]+$"))) {
            binding.statusText.text = "地址格式无效"
            return
        }

        // Read auth credentials (optional)
        val username = binding.usernameInput.text?.toString()?.trim()?.ifEmpty { null }
        val password = binding.passwordInput.text?.toString()?.trim()?.ifEmpty { null }

        // Read relay settings (optional)
        val relayAddr = binding.relayInput.text?.toString()?.trim()?.ifEmpty { null }
        val deviceId = binding.deviceIdInput.text?.toString()?.trim()?.ifEmpty { null }

        // Start media pipeline
        videoDecoder = VideoDecoder()
        audioPlayer = AudioPlayer()

        val client = RtspClient(host, port, username = username, password = password)
        client.listener = this
        rtspClient = client
        audioCapture = AudioCapture(client)

        // Start doorbell monitoring service
        DoorbellService.start(this, host)

        // Register network monitoring for auto-reconnect
        registerNetworkCallback()

        // Connect via relay or directly
        if (relayAddr != null && deviceId != null) {
            val relayParts = relayAddr.split(":")
            val relayHost = relayParts[0]
            val relayPort = if (relayParts.size > 1) relayParts[1].toIntOrNull() ?: 443 else 443

            if (relayHost.isEmpty() || !relayHost.matches(Regex("^[a-zA-Z0-9._-]+$"))) {
                binding.statusText.text = "中继地址格式无效"
                return
            }

            // Connect via relay with factory for reconnect support
            val relayFactory: () -> Socket = {
                RelayConnection(relayHost, relayPort, deviceId).connect()
            }
            client.connectViaSocketFactory(relayFactory)
        } else {
            client.connect()
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupTalkButton() {
        binding.talkBtn.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    startTalk()
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    stopTalk()
                    true
                }
                else -> false
            }
        }
    }

    private fun setupModeListener() {
        binding.modeGroup.setOnCheckedChangeListener { _, checkedId ->
            if (rtspClient != null || webRtcClient != null) {
                // Revert selection without triggering listener recursion
                binding.modeGroup.setOnCheckedChangeListener(null)
                binding.modeGroup.check(if (isWebRtcMode) R.id.modeWebrtc else R.id.modeRtsp)
                setupModeListener()
                return@setOnCheckedChangeListener
            }
            isWebRtcMode = (checkedId == R.id.modeWebrtc)
            binding.go2rtcInput.visibility = if (isWebRtcMode) View.VISIBLE else View.GONE
            binding.surfaceView.visibility = if (isWebRtcMode) View.GONE else View.VISIBLE
            binding.webrtcView.visibility = if (isWebRtcMode) View.VISIBLE else View.GONE
        }
    }

    private fun startTalk() {
        val capture = audioCapture ?: return
        if (capture.isCapturing) return

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO_PERM)
            return
        }

        if (capture.start(this)) {
            runOnUiThread {
                binding.talkBtn.text = "正在通话..."
                binding.talkBtn.isActivated = true
            }
        }
    }

    private fun stopTalk() {
        audioCapture?.stop()
        runOnUiThread {
            binding.talkBtn.text = "按住说话"
            binding.talkBtn.isActivated = false
        }
    }

    private fun disconnectAll() {
        if (disconnecting) return
        disconnecting = true
        isStreaming = false
        unregisterNetworkCallback()

        // WebRTC cleanup
        val wrtc = webRtcClient
        webRtcClient = null
        wrtc?.listener = null

        val capture = audioCapture
        val client = rtspClient
        val decoder = videoDecoder
        val player = audioPlayer
        audioCapture = null
        rtspClient = null
        videoDecoder = null
        audioPlayer = null

        // Detach listener before background disconnect to prevent
        // stale callbacks from interfering with new connections
        client?.listener = null

        // Keep DoorbellService running so doorbell can be received while disconnected

        Thread({
            capture?.stop()
            client?.disconnect()
            wrtc?.disconnect()
            decoder?.stop()
            player?.stop()
            disconnecting = false
        }, "Disconnect").start()

        runOnUiThread {
            binding.connectBtn.text = "连接"
            binding.talkBtn.isEnabled = false
            binding.statusText.text = "未连接"
            binding.addressInput.isEnabled = true
        }
    }

    // ---- RtspClient.Listener ----

    override fun onVideoRtp(data: ByteArray, offset: Int, length: Int,
                            timestamp: Long, marker: Boolean) {
        videoDecoder?.feedRtpPayload(data, offset, length, timestamp, marker)
    }

    override fun onAudioRtp(data: ByteArray, offset: Int, length: Int, timestamp: Long) {
        audioPlayer?.feedPcmu(data, offset, length)
    }

    override fun onStateChanged(state: RtspClient.State) {
        Log.i(TAG, "State: $state")
        runOnUiThread {
            when (state) {
                RtspClient.State.CONNECTING -> {
                    binding.statusText.text = "连接中..."
                    binding.connectBtn.text = "断开"
                    binding.addressInput.isEnabled = false
                }
                RtspClient.State.CONNECTED -> {
                    binding.statusText.text = "已连接"
                    // Start the existing decoders (created in onConnectClick, reused across reconnects).
                    // NALs arriving between PLAY and this start() are buffered in nalQueue,
                    // so the IDR keyframe won't be lost even if this runs after readLoop begins.
                    if (surfaceReady && !isDestroyed) {
                        val surface = binding.surfaceView.holder.surface
                        if (surface.isValid) {
                            videoDecoder?.start(
                                surface,
                                rtspClient?.getSps(),
                                rtspClient?.getPps()
                            )
                        }
                    }
                    audioPlayer?.start()
                }
                RtspClient.State.PLAYING -> {
                    binding.statusText.text = "播放中"
                    binding.talkBtn.isEnabled = true
                    isStreaming = true
                }
                RtspClient.State.DISCONNECTED -> {
                    isStreaming = false
                    binding.statusText.text = "未连接"
                    binding.connectBtn.text = "连接"
                    binding.talkBtn.isEnabled = false
                    binding.addressInput.isEnabled = true
                }
                RtspClient.State.RECONNECTING -> {
                    isStreaming = false
                    binding.statusText.text = "正在重连..."
                    binding.connectBtn.text = "断开"
                    binding.talkBtn.isEnabled = false
                    // Stop decoders during reconnect
                    videoDecoder?.stop()
                    audioPlayer?.stop()
                }
                RtspClient.State.ERROR -> {
                    isStreaming = false
                    binding.statusText.text = "连接错误"
                    binding.connectBtn.text = "连接"
                    binding.talkBtn.isEnabled = false
                    binding.addressInput.isEnabled = true
                    unregisterNetworkCallback()
                }
            }
        }
    }

    override fun onError(message: String) {
        Log.e(TAG, "Error: $message")
        runOnUiThread {
            binding.statusText.text = "错误: $message"
            // Only fully disconnect if not reconnecting (reconnect is handled internally)
            val client = rtspClient
            if (client != null && client.state == RtspClient.State.RECONNECTING) {
                return@runOnUiThread
            }
            disconnectAll()
        }
    }

    private fun restartVideoDecoder(surface: android.view.Surface) {
        if (!surface.isValid) return
        videoDecoder?.stop()
        val decoder = VideoDecoder()
        videoDecoder = decoder
        decoder.start(surface, rtspClient?.getSps(), rtspClient?.getPps())
    }

    private fun registerNetworkCallback() {
        unregisterNetworkCallback()
        val cm = getSystemService(ConnectivityManager::class.java) ?: return
        val request = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onLost(network: Network) {
                // Check if another network is available
                val activeNet = cm.activeNetwork
                if (activeNet != null) return  // WiFi→cellular transition, still online

                Log.w(TAG, "All networks lost")
                val client = rtspClient ?: return
                if (client.state == RtspClient.State.PLAYING ||
                    client.state == RtspClient.State.CONNECTED ||
                    client.state == RtspClient.State.RECONNECTING) {
                    runOnUiThread {
                        binding.statusText.text = "网络断开，等待重连..."
                    }
                }
            }
            override fun onAvailable(network: Network) {
                Log.i(TAG, "Network available")
                // If we're already reconnecting, this is helpful info
                val client = rtspClient ?: return
                if (client.state == RtspClient.State.RECONNECTING) {
                    runOnUiThread {
                        binding.statusText.text = "网络恢复，正在重连..."
                    }
                }
            }
        }
        networkCallback = cb
        cm.registerNetworkCallback(request, cb)
    }

    private fun unregisterNetworkCallback() {
        val cb = networkCallback ?: return
        networkCallback = null
        try {
            val cm = getSystemService(ConnectivityManager::class.java)
            cm?.unregisterNetworkCallback(cb)
        } catch (_: Exception) {}
    }

    // ---- WebRtcClient.Listener ----

    private val webRtcListener = object : WebRtcClient.Listener {
        override fun onStateChanged(state: WebRtcClient.State) {
            Log.i(TAG, "WebRTC State: $state")
            runOnUiThread {
                when (state) {
                    WebRtcClient.State.CONNECTING -> {
                        binding.statusText.text = "WebRTC 连接中..."
                    }
                    WebRtcClient.State.CONNECTED -> {
                        binding.statusText.text = "信令完成，等待媒体..."
                    }
                    WebRtcClient.State.PLAYING -> {
                        binding.statusText.text = "WebRTC 播放中"
                        isStreaming = true
                    }
                    WebRtcClient.State.DISCONNECTED -> {
                        isStreaming = false
                        binding.statusText.text = "未连接"
                        binding.connectBtn.text = "连接"
                        binding.addressInput.isEnabled = true
                    }
                    WebRtcClient.State.ERROR -> {
                        isStreaming = false
                        binding.statusText.text = "WebRTC 连接错误"
                        binding.connectBtn.text = "连接"
                        binding.addressInput.isEnabled = true
                    }
                }
            }
        }

        override fun onError(message: String) {
            Log.e(TAG, "WebRTC Error: $message")
            runOnUiThread {
                binding.statusText.text = "错误: $message"
                disconnectAll()
            }
        }
    }
}
