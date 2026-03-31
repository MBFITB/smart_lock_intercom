/*
 * audio_out_win.c — Windows audio output via waveOut API
 *
 * Receives G.711 µ-law samples, decodes to 16-bit PCM,
 * and plays through the default audio output device.
 *
 * Uses double-buffering for smooth playback.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include "audio_out.h"

/* G.711 µ-law decode table */
static int16_t ulaw_to_pcm(uint8_t u)
{
	u = ~u;
	int sign = (u & 0x80) ? -1 : 1;
	int exponent = (u >> 4) & 0x07;
	int mantissa = u & 0x0F;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign * magnitude);
}

#ifdef _WIN32

#define NUM_BUFFERS  4
#define MAX_SAMPLES  960  /* max samples per buffer (120ms @ 8kHz) */

static struct {
	HWAVEOUT     hwo;
	WAVEHDR      hdrs[NUM_BUFFERS];
	int16_t      bufs[NUM_BUFFERS][MAX_SAMPLES];
	int          next_buf;
	int          srate;
	int          channels;
	bool         opened;
} g_aout;


static void CALLBACK wave_out_proc(HWAVEOUT hwo, UINT uMsg,
				   DWORD_PTR dwInstance,
				   DWORD_PTR dwParam1,
				   DWORD_PTR dwParam2)
{
	(void)hwo;
	(void)dwInstance;
	(void)dwParam1;
	(void)dwParam2;

	if (uMsg == WOM_DONE) {
		/* Buffer completed playback — available for reuse */
	}
}


int audio_out_open(int srate, int channels, int samples_per_frame)
{
	WAVEFORMATEX wfx;
	MMRESULT mr;

	(void)samples_per_frame;

	if (g_aout.opened)
		return 0;  /* already open */

	memset(&g_aout, 0, sizeof(g_aout));

	wfx.wFormatTag      = WAVE_FORMAT_PCM;
	wfx.nChannels        = (WORD)channels;
	wfx.nSamplesPerSec   = (DWORD)srate;
	wfx.wBitsPerSample   = 16;
	wfx.nBlockAlign      = (WORD)(channels * 2);
	wfx.nAvgBytesPerSec  = (DWORD)(srate * channels * 2);
	wfx.cbSize           = 0;

	mr = waveOutOpen(&g_aout.hwo, WAVE_MAPPER, &wfx,
			 (DWORD_PTR)wave_out_proc, 0,
			 CALLBACK_FUNCTION);
	if (mr != MMSYSERR_NOERROR)
		return ENODEV;

	/* Prepare all buffers */
	for (int i = 0; i < NUM_BUFFERS; i++) {
		WAVEHDR *wh = &g_aout.hdrs[i];
		memset(wh, 0, sizeof(*wh));
		wh->lpData         = (LPSTR)g_aout.bufs[i];
		wh->dwBufferLength = MAX_SAMPLES * sizeof(int16_t);
		wh->dwFlags        = 0;
		waveOutPrepareHeader(g_aout.hwo, wh, sizeof(*wh));
		/* Mark as done so it's available immediately */
		wh->dwFlags |= WHDR_DONE;
	}

	g_aout.srate    = srate;
	g_aout.channels = channels;
	g_aout.opened   = true;

	return 0;
}


void audio_out_close(void)
{
	if (!g_aout.opened)
		return;

	waveOutReset(g_aout.hwo);

	for (int i = 0; i < NUM_BUFFERS; i++) {
		waveOutUnprepareHeader(g_aout.hwo, &g_aout.hdrs[i],
				       sizeof(WAVEHDR));
	}

	waveOutClose(g_aout.hwo);
	memset(&g_aout, 0, sizeof(g_aout));
}


void audio_out_play(const uint8_t *pcmu, size_t len)
{
	WAVEHDR *wh;

	if (!g_aout.opened || len == 0)
		return;

	if (len > MAX_SAMPLES)
		len = MAX_SAMPLES;

	/* Find next available buffer */
	wh = &g_aout.hdrs[g_aout.next_buf];

	if (!(wh->dwFlags & WHDR_DONE)) {
		/* All buffers busy — drop this frame */
		return;
	}

	/* Decode G.711 µ-law → 16-bit PCM */
	int16_t *dst = g_aout.bufs[g_aout.next_buf];
	for (size_t i = 0; i < len; i++)
		dst[i] = ulaw_to_pcm(pcmu[i]);

	wh->dwBufferLength = (DWORD)(len * sizeof(int16_t));
	waveOutWrite(g_aout.hwo, wh, sizeof(WAVEHDR));

	g_aout.next_buf = (g_aout.next_buf + 1) % NUM_BUFFERS;
}

#else
/* Stub for non-Windows — will be replaced per platform */

int audio_out_open(int srate, int channels, int samples_per_frame)
{
	(void)srate;
	(void)channels;
	(void)samples_per_frame;
	return 0;
}

void audio_out_close(void)
{
}

void audio_out_play(const uint8_t *pcmu, size_t len)
{
	(void)pcmu;
	(void)len;
}

#endif /* _WIN32 */
