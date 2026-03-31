#ifndef INTERCOM_H
#define INTERCOM_H

#include <re.h>
#include <rem.h>

/*
 * Intercom session — lightweight pure RTP/UDP
 *
 * Architecture (RTOS-aligned):
 *   Device sends H.264 video (PT 107) + G.711 audio (PT 0) over plain RTP
 *   Device receives G.711 audio (PT 0) from remote peer
 *   No DTLS/SRTP/ICE — pure UDP/RTP for LAN P2P
 */
struct intercom_session {
	/* Network — plain UDP socket */
	struct udp_sock *us;          /* RTP UDP socket */
	struct sa peer_addr;          /* remote peer address */
	uint16_t local_port;          /* bound local RTP port */

	/* Audio send timer */
	struct tmr tmr_audio;

	/* Video (H.264 via camera abstraction) */
	struct tmr tmr_video;
	uint32_t video_ssrc;
	uint32_t video_ts;
	uint16_t video_seq;
	uint32_t video_frame_count;

	/* Audio RTP state */
	uint32_t ssrc;
	uint32_t audio_ts;
	uint16_t audio_seq;

	/* State */
	bool active;                  /* session alive, media flowing */
};


/*
 * HTTP signaling server
 */
int http_sig_init(struct http_sock **sockp, const struct sa *laddr,
		  const char *www_path);
void http_sig_close(void);
void http_sig_set_session(struct intercom_session *sess);

/*
 * Session management — pure RTP
 */
int session_alloc(struct intercom_session **sessp,
		  const struct sa *peer);
void session_close(struct intercom_session *sess);

#endif /* INTERCOM_H */
