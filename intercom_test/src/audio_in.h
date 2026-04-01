#ifndef AUDIO_IN_H
#define AUDIO_IN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Platform-agnostic audio input (microphone capture) interface
 *
 * Captures PCM audio from system microphone and accumulates
 * in an internal ring buffer. Caller reads via audio_in_read().
 *
 * Platform implementations:
 *   - audio_in_win.c  : Windows waveIn API
 *   - audio_in_bk7258.c : BK7258 I2S/ADC input (target HW)
 */

/*
 * Initialize audio input device.
 *   srate: sample rate in Hz (e.g. 8000)
 *   channels: 1 = mono
 *   samples_per_frame: expected read size (e.g. 320 for 40ms @ 8kHz)
 * Returns 0 on success, errno on failure.
 */
int audio_in_open(int srate, int channels, int samples_per_frame);

/*
 * Close audio input, release resources.
 */
void audio_in_close(void);

/*
 * Read captured PCM samples.
 *   pcm: output buffer for 16-bit signed PCM samples
 *   count: number of samples to read
 * Returns number of samples actually read (may be < count if
 * not enough data captured yet). Returns 0 if no data available.
 */
int audio_in_read(int16_t *pcm, int count);

/*
 * Return number of samples currently buffered in the ring buffer.
 */
int audio_in_available(void);

/*
 * Check if audio input is open and capturing.
 */
bool audio_in_is_open(void);

#endif /* AUDIO_IN_H */
