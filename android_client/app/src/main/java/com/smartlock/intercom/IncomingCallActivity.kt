package com.smartlock.intercom

import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Bundle
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.WindowManager
import android.os.Build
import androidx.appcompat.app.AppCompatActivity
import com.smartlock.intercom.databinding.ActivityIncomingCallBinding

/**
 * Full-screen incoming call activity shown when doorbell rings.
 * Plays ringtone + vibrates. User can accept (connect RTSP) or reject.
 */
class IncomingCallActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "IncomingCall"
        const val EXTRA_HOST = "host"
        private const val RING_TIMEOUT_MS = 30_000L
    }

    private lateinit var binding: ActivityIncomingCallBinding
    private var ringtone: Ringtone? = null
    private var vibrator: Vibrator? = null
    private val timeoutRunnable = Runnable { onTimeout() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Show over lock screen
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(
                WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
                WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD or
                WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
            )
        }
        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        )

        binding = ActivityIncomingCallBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.acceptBtn.setOnClickListener { onAccept() }
        binding.rejectBtn.setOnClickListener { onReject() }

        startRinging()

        // Auto-dismiss after 30 seconds
        binding.root.postDelayed(timeoutRunnable, RING_TIMEOUT_MS)
    }

    override fun onDestroy() {
        binding.root.removeCallbacks(timeoutRunnable)
        stopRinging()
        super.onDestroy()
    }

    private fun onAccept() {
        Log.i(TAG, "Call accepted")
        stopRinging()

        val host = intent.getStringExtra(EXTRA_HOST)
        if (host.isNullOrEmpty()) {
            finish()
            return
        }
        val mainIntent = android.content.Intent(this, MainActivity::class.java).apply {
            flags = android.content.Intent.FLAG_ACTIVITY_CLEAR_TOP or
                    android.content.Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra("auto_connect_host", host)
        }
        startActivity(mainIntent)
        finish()
    }

    private fun onReject() {
        Log.i(TAG, "Call rejected")
        stopRinging()
        finish()
    }

    private fun onTimeout() {
        Log.i(TAG, "Call timeout (30s)")
        stopRinging()
        finish()
    }

    private fun startRinging() {
        // Play default ringtone
        try {
            val ringtoneUri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
            ringtone = RingtoneManager.getRingtone(this, ringtoneUri)
            ringtone?.audioAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
            ringtone?.play()
        } catch (e: Exception) {
            Log.w(TAG, "Could not play ringtone", e)
        }

        // Vibrate
        try {
            vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val vm = getSystemService(VibratorManager::class.java)
                vm?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                getSystemService(VIBRATOR_SERVICE) as? Vibrator
            }
            val pattern = longArrayOf(0, 500, 300, 500, 300, 500)
            vibrator?.vibrate(VibrationEffect.createWaveform(pattern, 0))
        } catch (e: Exception) {
            Log.w(TAG, "Could not vibrate", e)
        }
    }

    private fun stopRinging() {
        try { ringtone?.stop() } catch (_: Exception) {}
        ringtone = null
        try { vibrator?.cancel() } catch (_: Exception) {}
        vibrator = null
    }
}
