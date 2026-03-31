/*
 * session.c — Lightweight pure RTP session: H.264 video + G.711 audio
 *
 * Architecture (RTOS-aligned):
 *   - Plain UDP socket, no DTLS/SRTP/ICE
 *   - Sends H.264 video (PT 107) via RFC 6184 packetization
 *   - Sends G.711 µ-law audio (PT 0)
 *   - Receives G.711 audio from remote → audio_out playback
 *
 * LAN P2P: device ←→ peer over plain RTP/UDP
 */
#include <string.h>
#include <math.h>
#include <re.h>
#include <rem.h>
#include "intercom.h"
#include "camera.h"
#include "audio_out.h"

#define PT_PCMU       0
#define PT_H264       107
#define AUDIO_SRATE   8000
#define AUDIO_PTIME   20       /* ms per frame */
#define AUDIO_SAMPLES (AUDIO_SRATE * AUDIO_PTIME / 1000)  /* 160 */
#define VIDEO_WIDTH   320
#define VIDEO_HEIGHT  240
#define VIDEO_FPS     15
#define VIDEO_PTIME   (1000 / VIDEO_FPS)  /* ~67ms */
#define VIDEO_CLOCK   90000              /* RTP clock rate for video */
#define RTP_MTU       1200               /* max payload per RTP packet */

/* Default RTP port */
#define RTP_PORT      5004


/* Forward declarations */
static void udp_recv_handler(const struct sa *src, struct mbuf *mb, void *arg);
static void audio_send_timer(void *arg);
static void video_send_timer(void *arg);


static void session_destructor(void *data)
{
	struct intercom_session *sess = data;

	tmr_cancel(&sess->tmr_audio);
	tmr_cancel(&sess->tmr_video);
	camera_close();
	audio_out_close();
	mem_deref(sess->us);
}


/*------------------------------------------------------------------------
 * Session allocation — bind UDP + open camera + start media timers
 *----------------------------------------------------------------------*/
int session_alloc(struct intercom_session **sessp,
		  const struct sa *peer)
{
	struct intercom_session *sess;
	struct sa laddr;
	int err;

	sess = mem_zalloc(sizeof(*sess), session_destructor);
	if (!sess)
		return ENOMEM;

	tmr_init(&sess->tmr_audio);
	tmr_init(&sess->tmr_video);

	/* Random SSRCs for RTP */
	sess->ssrc = rand_u32();
	sess->video_ssrc = rand_u32();

	/* Store peer address */
	sess->peer_addr = *peer;

	/* H.264 camera/encoder init */
	err = camera_open(VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS, 300);
	if (err) {
		re_fprintf(stderr, "[SESSION] camera_open failed: %d\n", err);
		goto fail;
	}
	re_printf("[SESSION] Camera ready: %ux%u @ %u fps\n",
		  VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);

	/* Audio output init (for receiving remote G.711) */
	err = audio_out_open(AUDIO_SRATE, 1, AUDIO_SAMPLES);
	if (err) {
		re_fprintf(stderr, "[SESSION] audio_out_open failed: %d\n",
			   err);
		/* Non-fatal: continue without audio playback */
	}

	/* Bind UDP socket to 0.0.0.0:RTP_PORT */
	sa_set_str(&laddr, "0.0.0.0", RTP_PORT);
	err = udp_listen(&sess->us, &laddr, udp_recv_handler, sess);
	if (err) {
		re_fprintf(stderr, "[SESSION] udp_listen: %m\n", err);
		goto fail;
	}

#ifdef _WIN32
	/* Disable SIO_UDP_CONNRESET (Windows ICMP port unreachable fix) */
	{
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
		BOOL bFalse = FALSE;
		DWORD dwRet = 0;
		re_sock_t fd = udp_sock_fd(sess->us);
		WSAIoctl((SOCKET)fd, SIO_UDP_CONNRESET,
			 &bFalse, sizeof(bFalse),
			 NULL, 0, &dwRet, NULL, NULL);
	}
#endif

	{
		struct sa bound;
		udp_local_get(sess->us, &bound);
		sess->local_port = sa_port(&bound);
	}

	sess->active = true;

	re_printf("[SESSION] RTP port: %u → peer %J\n",
		  sess->local_port, &sess->peer_addr);

	/* Start media timers immediately */
	tmr_start(&sess->tmr_audio, AUDIO_PTIME, audio_send_timer, sess);
	tmr_start(&sess->tmr_video, VIDEO_PTIME, video_send_timer, sess);

	*sessp = sess;
	return 0;

fail:
	mem_deref(sess);
	return err;
}


/*------------------------------------------------------------------------
 * Handle incoming RTP — demux by payload type
 *----------------------------------------------------------------------*/
static void udp_recv_handler(const struct sa *src, struct mbuf *mb,
			     void *arg)
{
	struct intercom_session *sess = arg;
	struct rtp_header hdr;
	int err;

	if (!sess->active)
		return;

	if (mbuf_get_left(mb) < 12)
		return;  /* too short for RTP header */

	err = rtp_hdr_decode(&hdr, mb);
	if (err)
		return;

	if (hdr.ver != 2)
		return;

	if (hdr.pt == PT_PCMU) {
		/* G.711 µ-law audio from remote */
		const uint8_t *pcmu = mbuf_buf(mb);
		size_t pcmu_len = mbuf_get_left(mb);

		if (pcmu_len > 0)
			audio_out_play(pcmu, pcmu_len);
	}
	/* PT_H264 from remote: not expected in door lock scenario */
}


/*------------------------------------------------------------------------
 * Audio send timer — G.711 µ-law test tone over plain RTP
 *----------------------------------------------------------------------*/
static void audio_send_timer(void *arg)
{
	struct intercom_session *sess = arg;
	struct mbuf *mb;
	struct rtp_header hdr;
	uint8_t pcmu[AUDIO_SAMPLES];
	int16_t pcm[AUDIO_SAMPLES];
	static uint32_t phase = 0;

	if (!sess->active)
		return;

	if (!sa_isset(&sess->peer_addr, SA_ALL))
		goto restart;

	/* Generate 400 Hz test tone */
	for (int i = 0; i < AUDIO_SAMPLES; i++) {
		double t = (double)(phase + (uint32_t)i) / (double)AUDIO_SRATE;
		pcm[i] = (int16_t)(16000.0 * sin(2.0 * 3.14159265 * 400.0 * t));
	}
	phase += AUDIO_SAMPLES;

	/* PCM → G.711 µ-law */
	for (int i = 0; i < AUDIO_SAMPLES; i++)
		pcmu[i] = g711_pcm2ulaw(pcm[i]);

	/* Build RTP packet: header (12 bytes) + payload */
	mb = mbuf_alloc(12 + AUDIO_SAMPLES);
	if (!mb)
		goto restart;

	memset(&hdr, 0, sizeof(hdr));
	hdr.ver  = 2;
	hdr.pt   = PT_PCMU;
	hdr.seq  = sess->audio_seq++;
	hdr.ts   = sess->audio_ts;
	hdr.ssrc = sess->ssrc;

	rtp_hdr_encode(mb, &hdr);
	mbuf_write_mem(mb, pcmu, sizeof(pcmu));

	/* Send plain RTP over UDP */
	mbuf_set_pos(mb, 0);
	udp_send(sess->us, &sess->peer_addr, mb);

	sess->audio_ts += AUDIO_SAMPLES;
	mem_deref(mb);

restart:
	tmr_start(&sess->tmr_audio, AUDIO_PTIME, audio_send_timer, sess);
}


/*------------------------------------------------------------------------
 * Send a single H.264 NAL unit over RTP (RFC 6184)
 *
 * Small NALs (≤ RTP_MTU): Single NAL Unit Packet
 *   [RTP header][NAL unit data]
 *
 * Large NALs (> RTP_MTU): FU-A (Fragmentation Unit type A)
 *   [RTP header][FU indicator][FU header][fragment data]
 *----------------------------------------------------------------------*/
static void send_h264_nalu_rtp(struct intercom_session *sess,
			       const uint8_t *nalu, size_t nalu_len,
			       bool is_last_nalu)
{
	if (nalu_len < 1)
		return;

	uint8_t nal_header = nalu[0];

	if (nalu_len <= RTP_MTU) {
		/* Single NAL Unit Packet */
		struct mbuf *mb;
		struct rtp_header hdr;

		mb = mbuf_alloc(12 + nalu_len);
		if (!mb)
			return;

		memset(&hdr, 0, sizeof(hdr));
		hdr.ver  = 2;
		hdr.m    = is_last_nalu ? 1 : 0;
		hdr.pt   = PT_H264;
		hdr.seq  = sess->video_seq++;
		hdr.ts   = sess->video_ts;
		hdr.ssrc = sess->video_ssrc;

		rtp_hdr_encode(mb, &hdr);
		mbuf_write_mem(mb, nalu, nalu_len);

		mbuf_set_pos(mb, 0);
		udp_send(sess->us, &sess->peer_addr, mb);
		mem_deref(mb);
	}
	else {
		/* FU-A fragmentation */
		const uint8_t *payload = nalu + 1;
		size_t payload_len = nalu_len - 1;
		size_t offset = 0;
		bool first = true;

		uint8_t fu_indicator = (nal_header & 0xE0) | 28;
		uint8_t nal_type = nal_header & 0x1F;

		while (offset < payload_len) {
			size_t chunk = payload_len - offset;
			if (chunk > (size_t)(RTP_MTU - 2))
				chunk = (size_t)(RTP_MTU - 2);

			bool last_frag = (offset + chunk >= payload_len);

			struct mbuf *mb;
			struct rtp_header hdr;

			mb = mbuf_alloc(12 + 2 + chunk);
			if (!mb)
				return;

			memset(&hdr, 0, sizeof(hdr));
			hdr.ver  = 2;
			hdr.m    = (last_frag && is_last_nalu) ? 1 : 0;
			hdr.pt   = PT_H264;
			hdr.seq  = sess->video_seq++;
			hdr.ts   = sess->video_ts;
			hdr.ssrc = sess->video_ssrc;

			rtp_hdr_encode(mb, &hdr);

			/* FU indicator */
			mbuf_write_u8(mb, fu_indicator);

			/* FU header: S|E|R|Type */
			uint8_t fu_header = nal_type & 0x1F;
			if (first)
				fu_header |= 0x80;  /* S=1 */
			if (last_frag)
				fu_header |= 0x40;  /* E=1 */
			mbuf_write_u8(mb, fu_header);

			mbuf_write_mem(mb, payload + offset, chunk);

			mbuf_set_pos(mb, 0);
			udp_send(sess->us, &sess->peer_addr, mb);
			mem_deref(mb);

			offset += chunk;
			first = false;
		}
	}
}


/*------------------------------------------------------------------------
 * Video send timer — H.264 encoded frames over plain RTP
 *----------------------------------------------------------------------*/
static void video_send_timer(void *arg)
{
	struct intercom_session *sess = arg;
	struct camera_frame frame;
	int err;

	if (!sess->active)
		return;

	if (!sa_isset(&sess->peer_addr, SA_ALL))
		goto restart;

	/* Get encoded H.264 frame from camera */
	err = camera_get_frame(&frame);
	if (err) {
		if (err != EAGAIN) {
			re_fprintf(stderr, "[H264] camera_get_frame: %d\n",
				   err);
		}
		goto restart;
	}

	if (sess->video_frame_count % (VIDEO_FPS * 5) == 0) {
		re_printf("[H264] frame #%u %s %d NALs\n",
			  sess->video_frame_count,
			  frame.is_keyframe ? "IDR" : "P",
			  frame.count);
	}

	/* Send each NAL unit as RTP packet(s) */
	for (int i = 0; i < frame.count; i++) {
		bool is_last = (i == frame.count - 1);
		send_h264_nalu_rtp(sess,
				   frame.nalus[i].data,
				   frame.nalus[i].len,
				   is_last);
	}

	/* Advance RTP timestamp: 90000 Hz / FPS */
	sess->video_ts += VIDEO_CLOCK / VIDEO_FPS;
	sess->video_frame_count++;

restart:
	tmr_start(&sess->tmr_video, VIDEO_PTIME, video_send_timer, sess);
}


/*------------------------------------------------------------------------
 * Close session
 *----------------------------------------------------------------------*/
void session_close(struct intercom_session *sess)
{
	if (!sess)
		return;

	re_printf("[SESSION] closing\n");
	tmr_cancel(&sess->tmr_audio);
	tmr_cancel(&sess->tmr_video);
	sess->active = false;
}
