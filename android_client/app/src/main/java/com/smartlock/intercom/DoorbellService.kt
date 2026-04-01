package com.smartlock.intercom

import android.app.*
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import java.net.HttpURLConnection
import java.net.URL

/**
 * Foreground service that polls the device HTTP API for doorbell events.
 * When a doorbell ring is detected, launches IncomingCallActivity.
 */
class DoorbellService : Service() {

    companion object {
        private const val TAG = "DoorbellService"
        private const val CHANNEL_ID = "doorbell_service"
        private const val CHANNEL_RING_ID = "doorbell_ring"
        private const val NOTIF_SERVICE_ID = 1
        private const val NOTIF_RING_ID = 2
        private const val POLL_INTERVAL_MS = 2000L
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"

        fun start(context: Context, host: String, port: Int = 9000) {
            val intent = Intent(context, DoorbellService::class.java).apply {
                putExtra(EXTRA_HOST, host)
                putExtra(EXTRA_PORT, port)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, DoorbellService::class.java))
        }
    }

    @Volatile
    private var polling = false
    private var pollThread: Thread? = null
    private var host = ""
    private var httpPort = 9000

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannels()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val newHost = intent?.getStringExtra(EXTRA_HOST) ?: ""
        val newPort = intent?.getIntExtra(EXTRA_PORT, 9000) ?: 9000

        // Validate host — whitelist: alphanumeric, dots, dashes only
        if (newHost.isEmpty() || !newHost.matches(Regex("^[a-zA-Z0-9._-]+$"))) {
            stopSelf()
            return START_NOT_STICKY
        }

        // If host/port changed, restart polling
        if (newHost != host || newPort != httpPort) {
            polling = false
            pollThread?.interrupt()
            pollThread?.join(2000)
        }

        host = newHost
        httpPort = newPort

        startForeground(NOTIF_SERVICE_ID, buildServiceNotification())

        if (!polling) {
            polling = true
            pollThread = Thread({ pollLoop() }, "DoorbellPoll")
            pollThread?.start()
        }

        return START_NOT_STICKY
    }

    override fun onDestroy() {
        polling = false
        pollThread?.interrupt()
        pollThread?.join(3000)
        pollThread = null
        super.onDestroy()
    }

    private fun pollLoop() {
        while (polling) {
            try {
                val ringing = checkDoorbell()
                if (ringing) {
                    onDoorbellRing()
                }
                Thread.sleep(POLL_INTERVAL_MS)
            } catch (_: InterruptedException) {
                break
            } catch (e: Exception) {
                Log.w(TAG, "Poll error: ${e.message}")
                try { Thread.sleep(POLL_INTERVAL_MS * 2) } catch (_: InterruptedException) { break }
            }
        }
    }

    private fun checkDoorbell(): Boolean {
        val url = URL("http://$host:$httpPort/api/doorbell/status")
        val conn = url.openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 3000
            conn.readTimeout = 3000
            conn.requestMethod = "GET"

            if (conn.responseCode != 200) return false

            val body = conn.inputStream.bufferedReader().readText()
            return body.contains("\"ringing\":true")
        } finally {
            conn.disconnect()
        }
    }

    private fun onDoorbellRing() {
        Log.i(TAG, "Doorbell ring detected!")

        // If already streaming, don't disrupt — just show notification
        if (MainActivity.isStreaming) {
            Log.i(TAG, "Already streaming, skipping incoming call screen")
            return
        }

        // Launch incoming call screen
        val callIntent = Intent(this, IncomingCallActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra(IncomingCallActivity.EXTRA_HOST, host)
        }
        startActivity(callIntent)

        // Also show a high-priority notification (in case screen is off)
        val notifManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val pendingIntent = PendingIntent.getActivity(
            this, 0, callIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = NotificationCompat.Builder(this, CHANNEL_RING_ID)
            .setSmallIcon(android.R.drawable.ic_menu_call)
            .setContentTitle("门铃响了")
            .setContentText("有人按了门铃")
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setFullScreenIntent(pendingIntent, true)
            .setAutoCancel(true)
            .build()

        notifManager.notify(NOTIF_RING_ID, notification)
    }

    private fun createNotificationChannels() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

            val serviceChannel = NotificationChannel(
                CHANNEL_ID, "门铃监听服务",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "后台监听门铃事件"
            }
            manager.createNotificationChannel(serviceChannel)

            val ringChannel = NotificationChannel(
                CHANNEL_RING_ID, "门铃提醒",
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = "有人按门铃时通知"
                enableVibration(true)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            }
            manager.createNotificationChannel(ringChannel)
        }
    }

    private fun buildServiceNotification(): Notification {
        val intent = Intent(this, MainActivity::class.java)
        val pendingIntent = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_lock_idle_lock)
            .setContentTitle("智能门锁")
            .setContentText("正在监听门铃...")
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }
}
