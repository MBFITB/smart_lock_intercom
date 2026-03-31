package com.smartlock.intercom.media

import android.Manifest
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import androidx.core.content.ContextCompat
import com.smartlock.intercom.rtsp.RtspClient

/**
 * Captures microphone audio, encodes to G.711 µ-law,
 * and sends via RTSP backchannel.
 */
class AudioCapture(private val rtspClient: RtspClient) {

    companion object {
        private const val TAG = "AudioCapture"
        private const val SAMPLE_RATE = 8000
        private const val SAMPLES_PER_FRAME = 320  // 40ms at 8kHz
    }

    private var audioRecord: AudioRecord? = null
    private var captureThread: Thread? = null
    @Volatile
    private var capturing = false

    fun start(context: android.content.Context): Boolean {
        if (capturing) return false

        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "RECORD_AUDIO permission not granted")
            return false
        }

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        val bufSize = maxOf(minBuf, SAMPLES_PER_FRAME * 2 * 4)

        val record = try {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufSize
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create AudioRecord", e)
            return false
        }

        if (record.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord init failed")
            record.release()
            return false
        }

        record.startRecording()
        audioRecord = record
        capturing = true

        captureThread = Thread({
            captureLoop(record)
        }, "AudioCapture")
        captureThread!!.start()

        Log.i(TAG, "Capture started (rate=$SAMPLE_RATE, frame=$SAMPLES_PER_FRAME)")
        return true
    }

    fun stop() {
        capturing = false
        captureThread?.join(2000)
        captureThread = null

        try {
            audioRecord?.stop()
            audioRecord?.release()
        } catch (_: Exception) {}
        audioRecord = null
    }

    val isCapturing: Boolean get() = capturing

    private fun captureLoop(record: AudioRecord) {
        val pcmBuf = ShortArray(SAMPLES_PER_FRAME)
        val ulawBuf = ByteArray(SAMPLES_PER_FRAME)
        var errorCount = 0

        while (capturing) {
            try {
                val read = record.read(pcmBuf, 0, SAMPLES_PER_FRAME)
                if (read <= 0) continue

                // Encode PCM → G.711 µ-law
                for (i in 0 until read) {
                    ulawBuf[i] = ulawEncode(pcmBuf[i])
                }

                // Send via backchannel
                rtspClient.sendBackchannelAudio(ulawBuf, 0, read)
                errorCount = 0
            } catch (e: Exception) {
                Log.e(TAG, "Capture loop error", e)
                if (++errorCount > 5) {
                    Log.e(TAG, "Too many errors, stopping capture")
                    capturing = false
                    break
                }
            }
        }
    }

    /**
     * G.711 µ-law encode: 16-bit PCM → µ-law byte.
     */
    private fun ulawEncode(sample: Short): Byte {
        val BIAS = 0x84
        val MAX = 0x7FFF

        var pcm = sample.toInt()
        val sign: Int
        if (pcm < 0) {
            sign = 0x80
            pcm = -pcm
        } else {
            sign = 0
        }
        if (pcm > MAX) pcm = MAX
        pcm += BIAS

        var exponent = 7
        var mask = 0x4000
        while (exponent > 0 && (pcm and mask) == 0) {
            exponent--
            mask = mask shr 1
        }

        val mantissa = (pcm shr (exponent + 3)) and 0x0F
        val ulawByte = (sign or (exponent shl 4) or mantissa) xor 0xFF

        return ulawByte.toByte()
    }
}
