/*
 * audio_in_win.c — Windows audio input via waveIn API
 *
 * Captures 8kHz mono 16-bit PCM from the default microphone.
 * Uses ring buffer with double-buffered waveIn headers.
 *
 * Architecture mirrors audio_out_win.c for consistency.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#endif

#include "audio_in.h"
#include <re.h>

#ifdef _WIN32

#define NUM_BUFFERS    4
#define MAX_SAMPLES    960    /* max samples per waveIn buffer (120ms @ 8kHz) */

/* Ring buffer for captured PCM: 1 second @ 8kHz */
#define RING_SIZE      8000

static struct {
	HWAVEIN      hwi;
	WAVEHDR      hdrs[NUM_BUFFERS];
	int16_t      bufs[NUM_BUFFERS][MAX_SAMPLES];
	int          samples_per_buf;
	bool         opened;

	/* Ring buffer (CriticalSection-protected) */
	int16_t      ring[RING_SIZE];
	LONG         wr;      /* write index (waveIn callback thread) */
	LONG         rd;      /* read index (main/timer thread) */
	uint32_t     drop_count;  /* dropped sample counter */
	DWORD        last_drop_log; /* tick of last drop warning */

	CRITICAL_SECTION cs;
} g_ain;


/* Software gain: amplify mic PCM by ~10x (20dB) */
#define MIC_GAIN  10

static void enqueue_to_ring(const int16_t *src, int count)
{
	EnterCriticalSection(&g_ain.cs);

	for (int i = 0; i < count; i++) {
		LONG wr = g_ain.wr;
		LONG next = (wr + 1) % RING_SIZE;

		if (next == g_ain.rd) {
			/* Ring full — drop oldest sample */
			g_ain.rd = (g_ain.rd + 1) % RING_SIZE;
			g_ain.drop_count++;
			DWORD now = GetTickCount();
			if (now - g_ain.last_drop_log > 10000) {
				g_ain.last_drop_log = now;
				re_printf("[AUDIO-IN] Ring buffer full, "
					  "dropped %u samples\n",
					  g_ain.drop_count);
			}
		}

		/* Apply software gain with clipping */
		int32_t s = (int32_t)src[i] * MIC_GAIN;
		if (s > 32767) s = 32767;
		else if (s < -32768) s = -32768;
		g_ain.ring[wr] = (int16_t)s;
		g_ain.wr = next;
	}

	LeaveCriticalSection(&g_ain.cs);
}


static void CALLBACK wave_in_proc(HWAVEIN hwi, UINT uMsg,
				  DWORD_PTR dwInstance,
				  DWORD_PTR dwParam1,
				  DWORD_PTR dwParam2)
{
	(void)dwInstance;
	(void)dwParam2;

	if (uMsg == WIM_DATA) {
		WAVEHDR *wh = (WAVEHDR *)dwParam1;
		int samples = (int)(wh->dwBytesRecorded / sizeof(int16_t));

		if (samples > 0) {
			enqueue_to_ring((const int16_t *)wh->lpData, samples);
		}

		/* Re-queue buffer if still open */
		if (g_ain.opened) {
			wh->dwBytesRecorded = 0;
			waveInAddBuffer(hwi, wh, sizeof(WAVEHDR));
		}
	}
}


int audio_in_open(int srate, int channels, int samples_per_frame)
{
	WAVEFORMATEX wfx;
	MMRESULT mr;

	if (g_ain.opened)
		return 0;  /* already open */

	memset(&g_ain, 0, sizeof(g_ain));
	InitializeCriticalSection(&g_ain.cs);

	g_ain.samples_per_buf = samples_per_frame;
	if (g_ain.samples_per_buf > MAX_SAMPLES)
		g_ain.samples_per_buf = MAX_SAMPLES;

	wfx.wFormatTag      = WAVE_FORMAT_PCM;
	wfx.nChannels        = (WORD)channels;
	wfx.nSamplesPerSec   = (DWORD)srate;
	wfx.wBitsPerSample   = 16;
	wfx.nBlockAlign      = (WORD)(channels * 2);
	wfx.nAvgBytesPerSec  = (DWORD)(srate * channels * 2);
	wfx.cbSize           = 0;

	mr = waveInOpen(&g_ain.hwi, WAVE_MAPPER, &wfx,
			(DWORD_PTR)wave_in_proc, 0,
			CALLBACK_FUNCTION);
	if (mr != MMSYSERR_NOERROR) {
		DeleteCriticalSection(&g_ain.cs);
		return ENODEV;
	}

	/* Prepare and queue all buffers */
	for (int i = 0; i < NUM_BUFFERS; i++) {
		WAVEHDR *wh = &g_ain.hdrs[i];
		memset(wh, 0, sizeof(*wh));
		wh->lpData         = (LPSTR)g_ain.bufs[i];
		wh->dwBufferLength = (DWORD)(g_ain.samples_per_buf *
					     sizeof(int16_t));
		waveInPrepareHeader(g_ain.hwi, wh, sizeof(*wh));
		waveInAddBuffer(g_ain.hwi, wh, sizeof(*wh));
	}

	g_ain.opened = true;

	mr = waveInStart(g_ain.hwi);
	if (mr != MMSYSERR_NOERROR) {
		audio_in_close();
		return ENODEV;
	}

	return 0;
}


void audio_in_close(void)
{
	if (!g_ain.opened)
		return;

	/* Clear flag BEFORE stop/reset so callback doesn't re-queue
	 * buffers flushed by waveInReset() */
	g_ain.opened = false;

	waveInStop(g_ain.hwi);
	waveInReset(g_ain.hwi);

	for (int i = 0; i < NUM_BUFFERS; i++) {
		waveInUnprepareHeader(g_ain.hwi, &g_ain.hdrs[i],
				      sizeof(WAVEHDR));
	}

	waveInClose(g_ain.hwi);
	DeleteCriticalSection(&g_ain.cs);
	memset(&g_ain, 0, sizeof(g_ain));
}


int audio_in_read(int16_t *pcm, int count)
{
	int n = 0;

	EnterCriticalSection(&g_ain.cs);

	while (n < count && g_ain.rd != g_ain.wr) {
		pcm[n++] = g_ain.ring[g_ain.rd];
		g_ain.rd = (g_ain.rd + 1) % RING_SIZE;
	}

	LeaveCriticalSection(&g_ain.cs);

	return n;
}


int audio_in_available(void)
{
	int avail;

	EnterCriticalSection(&g_ain.cs);
	avail = (g_ain.wr - g_ain.rd + RING_SIZE) % RING_SIZE;
	LeaveCriticalSection(&g_ain.cs);

	return avail;
}


bool audio_in_is_open(void)
{
	return g_ain.opened;
}

#else
/* Stub for non-Windows — will be replaced per platform */

int audio_in_open(int srate, int channels, int samples_per_frame)
{
	(void)srate;
	(void)channels;
	(void)samples_per_frame;
	return 0;
}

void audio_in_close(void)
{
}

int audio_in_read(int16_t *pcm, int count)
{
	(void)pcm;
	(void)count;
	return 0;
}

int audio_in_available(void)
{
	return 0;
}

bool audio_in_is_open(void)
{
	return false;
}

#endif /* _WIN32 */
