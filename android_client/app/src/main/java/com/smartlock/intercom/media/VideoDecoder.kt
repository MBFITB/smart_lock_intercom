package com.smartlock.intercom.media

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * H.264 video decoder using Android MediaCodec.
 * Accepts raw NAL units from RTP and renders to a Surface.
 *
 * Handles FU-A fragmented NALs and STAP-A aggregated NALs.
 */
class VideoDecoder {

    companion object {
        private const val TAG = "VideoDecoder"
        private const val MIME_H264 = "video/avc"
        private const val DEQUEUE_TIMEOUT_US = 10000L
    }

    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    @Volatile private var running = false
    private var feedThread: Thread? = null
    private var framePtsUs = 0L

    // NAL unit queue: each entry is a complete NAL unit (with start code prefix)
    private val nalQueue = LinkedBlockingQueue<ByteArray>(120)

    // FU-A reassembly state
    private var fuBuffer: ByteArray? = null
    private var fuOffset = 0

    fun start(surface: Surface, sps: ByteArray?, pps: ByteArray?) {
        if (running) return  // already started
        this.surface = surface

        val format = MediaFormat.createVideoFormat(MIME_H264, 320, 240)
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 100000)

        // Configure with SPS/PPS from SDP
        if (sps != null && pps != null) {
            format.setByteBuffer("csd-0", ByteBuffer.wrap(addStartCode(sps)))
            format.setByteBuffer("csd-1", ByteBuffer.wrap(addStartCode(pps)))
        }

        val mc = try {
            val c = MediaCodec.createDecoderByType(MIME_H264)
            c.configure(format, surface, null, 0)
            c.start()
            c
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create H.264 decoder", e)
            return
        }
        codec = mc
        running = true
        framePtsUs = 0L

        feedThread = Thread({
            decodingLoop(mc)
        }, "VideoDecoder")
        feedThread!!.start()
    }

    fun stop() {
        running = false
        nalQueue.clear()
        feedThread?.interrupt()
        feedThread?.join(2000)
        feedThread = null
        fuBuffer = null
        fuOffset = 0

        try {
            codec?.flush()
            codec?.stop()
            codec?.release()
        } catch (_: Exception) {}
        codec = null
        surface = null
    }

    /**
     * Called from RTP receive thread with the RTP payload (after header).
     * Handles NAL unit types: single NAL, STAP-A (24), FU-A (28).
     */
    fun feedRtpPayload(data: ByteArray, offset: Int, length: Int, timestamp: Long, marker: Boolean) {
        if (length < 1) return

        val nalHeader = data[offset].toInt() and 0xFF
        val nalType = nalHeader and 0x1F

        when (nalType) {
            28 -> handleFuA(data, offset, length, marker)  // FU-A
            24 -> handleStapA(data, offset, length)         // STAP-A
            else -> {
                // Single NAL unit
                val nal = ByteArray(length)
                System.arraycopy(data, offset, nal, 0, length)
                enqueueNal(nal)
            }
        }
    }

    private fun handleFuA(data: ByteArray, offset: Int, length: Int, marker: Boolean) {
        if (length < 2) return

        val fuIndicator = data[offset].toInt() and 0xFF
        val fuHeader = data[offset + 1].toInt() and 0xFF
        val startBit = (fuHeader and 0x80) != 0
        val endBit = (fuHeader and 0x40) != 0
        val nalType = fuHeader and 0x1F
        val nri = fuIndicator and 0x60

        if (startBit) {
            // Start of fragmented NAL — allocate buffer
            fuBuffer = ByteArray(65536)
            // Reconstruct NAL header: NRI + NAL type
            fuBuffer!![0] = (nri or nalType).toByte()
            System.arraycopy(data, offset + 2, fuBuffer!!, 1, length - 2)
            fuOffset = 1 + (length - 2)
        } else if (fuBuffer != null) {
            // Middle or end fragment
            val payloadLen = length - 2
            if (fuOffset + payloadLen <= fuBuffer!!.size) {
                System.arraycopy(data, offset + 2, fuBuffer!!, fuOffset, payloadLen)
                fuOffset += payloadLen
            }

            if (endBit || marker) {
                val nal = ByteArray(fuOffset)
                System.arraycopy(fuBuffer!!, 0, nal, 0, fuOffset)
                enqueueNal(nal)
                fuBuffer = null
                fuOffset = 0
            }
        }
    }

    private fun handleStapA(data: ByteArray, offset: Int, length: Int) {
        var pos = offset + 1  // skip STAP-A header byte
        val end = offset + length

        while (pos + 2 <= end) {
            val nalSize = ((data[pos].toInt() and 0xFF) shl 8) or
                    (data[pos + 1].toInt() and 0xFF)
            pos += 2
            if (nalSize <= 0 || pos + nalSize > end) break

            val nal = ByteArray(nalSize)
            System.arraycopy(data, pos, nal, 0, nalSize)
            enqueueNal(nal)
            pos += nalSize
        }
    }

    private fun enqueueNal(nal: ByteArray) {
        // Drop frames if queue is full (better to drop than block RTP thread)
        if (!nalQueue.offer(nal)) {
            nalQueue.poll()   // drop oldest
            nalQueue.offer(nal)
        }
    }

    private fun decodingLoop(mc: MediaCodec) {
        val info = MediaCodec.BufferInfo()

        while (running) {
            // Feed input
            val nal = try {
                nalQueue.poll(50, TimeUnit.MILLISECONDS)
            } catch (_: InterruptedException) { break }

            if (nal != null) {
                val inputIdx = mc.dequeueInputBuffer(DEQUEUE_TIMEOUT_US)
                if (inputIdx >= 0) {
                    val inputBuf = mc.getInputBuffer(inputIdx) ?: continue
                    val nalWithStartCode = addStartCode(nal)
                    inputBuf.clear()
                    inputBuf.put(nalWithStartCode)
                    mc.queueInputBuffer(inputIdx, 0, nalWithStartCode.size, framePtsUs, 0)
                    framePtsUs += 1_000_000L / 15  // ~66ms per frame at 15fps
                }
            }

            // Drain output
            while (running) {
                val outputIdx = mc.dequeueOutputBuffer(info, 0)
                if (outputIdx >= 0) {
                    mc.releaseOutputBuffer(outputIdx, true)  // render to surface
                } else if (outputIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    Log.i(TAG, "Decoder output format changed: ${mc.outputFormat}")
                } else {
                    break
                }
            }
        }
    }

    private fun addStartCode(nal: ByteArray): ByteArray {
        val result = ByteArray(4 + nal.size)
        result[0] = 0; result[1] = 0; result[2] = 0; result[3] = 1
        System.arraycopy(nal, 0, result, 4, nal.size)
        return result
    }
}
