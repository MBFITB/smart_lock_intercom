#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>
#include <stddef.h>

/*
 * Platform-agnostic audio output interface
 *
 * Plays decoded PCM audio through the system speaker.
 * Input: G.711 µ-law encoded samples → decoded internally → speaker.
 *
 * Platform implementations:
 *   - audio_out_win.c  : Windows waveOut API
 *   - audio_out_bk7258.c : BK7258 I2S/DAC output (target HW)
 */

/*
 * Initialize audio output device.
 *   srate: sample rate in Hz (e.g. 8000)
 *   channels: 1 = mono
 *   samples_per_frame: samples per callback (e.g. 160 for 20ms @ 8kHz)
 * Returns 0 on success, errno on failure.
 */
int audio_out_open(int srate, int channels, int samples_per_frame);

/*
 * Close audio output, release resources.
 */
void audio_out_close(void);

/*
 * Play G.711 µ-law encoded audio.
 * Decodes to PCM internally and queues for playback.
 *   pcmu: G.711 µ-law samples
 *   len:  number of samples
 */
void audio_out_play(const uint8_t *pcmu, size_t len);

#endif /* AUDIO_OUT_H */
