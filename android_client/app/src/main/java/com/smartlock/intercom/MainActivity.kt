package com.smartlock.intercom

import android.Manifest
import android.annotation.SuppressLint
import android.content.pm.PackageManager
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

class MainActivity : AppCompatActivity(), RtspClient.Listener {

    companion object {
        private const val TAG = "MainActivity"
        private const val REQ_AUDIO_PERM = 1001
        private const val DEFAULT_PORT = 8555
    }

    private lateinit var binding: ActivityMainBinding

    private var rtspClient: RtspClient? = null
    private var videoDecoder: VideoDecoder? = null
    private var audioPlayer: AudioPlayer? = null
    private var audioCapture: AudioCapture? = null
    private var surfaceReady = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
            }
            override fun surfaceChanged(holder: SurfaceHolder, fmt: Int, w: Int, h: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                videoDecoder?.stop()
            }
        })

        binding.connectBtn.setOnClickListener { onConnectClick() }
        setupTalkButton()

        // Request audio permission early
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO_PERM)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        audioCapture?.stop()
        audioCapture = null
        rtspClient?.disconnect()
        rtspClient = null
        videoDecoder?.stop()
        videoDecoder = null
        audioPlayer?.stop()
        audioPlayer = null
    }

    private fun onConnectClick() {
        if (rtspClient != null) {
            // Disconnect
            disconnectAll()
            return
        }

        val address = binding.addressInput.text?.toString()?.trim() ?: return
        if (address.isEmpty()) return

        // Parse host:port
        val parts = address.split(":")
        val host = parts[0]
        val port = if (parts.size > 1) parts[1].toIntOrNull() ?: DEFAULT_PORT else DEFAULT_PORT

        // Validate host — prevent header injection
        if (host.isEmpty() || host.contains("\n") || host.contains("\r")
            || host.contains("/") || host.contains(" ")) {
            binding.statusText.text = "地址格式无效"
            return
        }

        // Start media pipeline
        videoDecoder = VideoDecoder()
        audioPlayer = AudioPlayer()

        val client = RtspClient(host, port)
        client.listener = this
        rtspClient = client
        audioCapture = AudioCapture(client)

        client.connect()
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
        val capture = audioCapture
        val client = rtspClient
        val decoder = videoDecoder
        val player = audioPlayer
        audioCapture = null
        rtspClient = null
        videoDecoder = null
        audioPlayer = null

        Thread({
            capture?.stop()
            client?.disconnect()
            decoder?.stop()
            player?.stop()
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
                }
                RtspClient.State.PLAYING -> {
                    binding.statusText.text = "播放中"
                    binding.talkBtn.isEnabled = true

                    // Start media decoders now that we know SPS/PPS
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
                RtspClient.State.DISCONNECTED -> {
                    binding.statusText.text = "未连接"
                    binding.connectBtn.text = "连接"
                    binding.talkBtn.isEnabled = false
                    binding.addressInput.isEnabled = true
                }
                RtspClient.State.ERROR -> {
                    binding.statusText.text = "连接错误"
                    binding.connectBtn.text = "连接"
                    binding.talkBtn.isEnabled = false
                    binding.addressInput.isEnabled = true
                }
            }
        }
    }

    override fun onError(message: String) {
        Log.e(TAG, "Error: $message")
        runOnUiThread {
            binding.statusText.text = "错误: $message"
            disconnectAll()
        }
    }
}
