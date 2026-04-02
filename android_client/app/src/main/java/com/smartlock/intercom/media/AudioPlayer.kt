package com.smartlock.intercom.media

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.util.Log

/**
 * G.711 µ-law audio player using AudioTrack.
 * Decodes µ-law to 16-bit PCM and plays via speaker.
 */
class AudioPlayer {

    companion object {
        private const val TAG = "AudioPlayer"
        private const val SAMPLE_RATE = 8000
    }

    @Volatile private var audioTrack: AudioTrack? = null
    private val decodeBuffer = ShortArray(960)  // Max 120ms at 8kHz

    fun start() {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        // Use at least 2 frames worth of buffer (40ms × 2 = 80ms)
        val bufSize = maxOf(minBuf, SAMPLE_RATE * 2 * 80 / 1000)

        val track = try {
            AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build()
                )
                .setBufferSizeInBytes(bufSize)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create AudioTrack", e)
            return
        }

        track.play()
        audioTrack = track
        Log.i(TAG, "AudioTrack started (rate=$SAMPLE_RATE, buf=$bufSize)")
    }

    fun stop() {
        try {
            audioTrack?.stop()
            audioTrack?.release()
        } catch (_: Exception) {}
        audioTrack = null
    }

    /**
     * Decode G.711 µ-law and write to AudioTrack.
     * Called from RTP receive thread.
     */
    fun feedPcmu(data: ByteArray, offset: Int, length: Int) {
        val track = audioTrack ?: return
        if (length > decodeBuffer.size) return

        try {
            for (i in 0 until length) {
                decodeBuffer[i] = ulawDecode(data[offset + i])
            }
            track.write(decodeBuffer, 0, length)
        } catch (e: IllegalStateException) {
            Log.w(TAG, "AudioTrack released, ignoring frame")
        }
    }

    /**
     * G.711 µ-law decode: single byte → 16-bit PCM sample.
     */
    private fun ulawDecode(ulawByte: Byte): Short {
        var ulaw = (ulawByte.toInt() and 0xFF) xor 0xFF
        val sign = ulaw and 0x80
        val exponent = (ulaw shr 4) and 0x07
        val mantissa = ulaw and 0x0F

        var sample = ((mantissa shl 3) + 0x84) shl exponent
        sample -= 0x84

        return if (sign != 0) (-sample).toShort() else sample.toShort()
    }
}
