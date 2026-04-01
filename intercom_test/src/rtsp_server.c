/*
 * rtsp_server.c — Lightweight RTSP server for intercom streaming
 *
 * Implements minimal RTSP 1.0 (RFC 2326) over TCP:
 *   OPTIONS   → supported methods
 *   DESCRIBE  → SDP with H.264 video + G.711 audio
 *   SETUP     → allocate RTP/RTCP UDP port pairs
 *   PLAY      → start RTP media send
 *   TEARDOWN  → stop and release
 *
 * Single-client design (one RTSP session at a time).
 * RTP is sent as plain UDP (RTP/AVP) — no interleaved TCP mode.
 */
#include <string.h>
#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <re.h>
#include <rem.h>
#include "intercom.h"
#include "camera.h"
#include "rtsp_server.h"
#include "audio_out.h"
#include "audio_in.h"

#define PT_PCMU       0
#define PT_H264       107
#define AUDIO_SRATE   8000
#define AUDIO_PTIME   40
#define AUDIO_SAMPLES (AUDIO_SRATE * AUDIO_PTIME / 1000)
#define VIDEO_WIDTH   320
#define VIDEO_HEIGHT  240
#define VIDEO_FPS     15
#define VIDEO_PTIME   (1000 / VIDEO_FPS)
#define VIDEO_CLOCK   90000
#define RTP_MTU       1200

/* RTSP session state */
enum rtsp_state {
	RTSP_IDLE,
	RTSP_READY,     /* SETUP done, not playing */
	RTSP_PLAYING,   /* PLAY active */
};

/* Single RTSP client session */
struct rtsp_client {
	struct le le;              /* linked list element */
	struct tcp_conn *tc;       /* TCP connection */
	struct mbuf *rbuf;         /* receive buffer (line accumulator) */

	enum rtsp_state state;
	char session_id[16];

	/* Transport mode */
	bool interleaved;          /* true = RTP over TCP (interleaved) */

	/* Video RTP */
	struct udp_sock *rtp_video;
	struct sa video_dest;      /* client's video RTP addr (UDP) */
	uint16_t video_srv_port;   /* our video RTP port (UDP) */
	uint8_t video_interleaved; /* TCP interleaved channel (RTP) */
	uint8_t video_interleaved_rtcp; /* TCP interleaved channel (RTCP) */
	uint32_t video_ssrc;
	uint32_t video_ts;
	uint16_t video_seq;

	/* Audio RTP */
	struct udp_sock *rtp_audio;
	struct sa audio_dest;      /* client's audio RTP addr (UDP) */
	uint16_t audio_srv_port;   /* our audio RTP port (UDP) */
	uint8_t audio_interleaved; /* TCP interleaved channel (RTP) */
	uint8_t audio_interleaved_rtcp; /* TCP interleaved channel (RTCP) */
	uint32_t audio_ssrc;
	uint32_t audio_ts;
	uint16_t audio_seq;

	bool video_setup;
	bool audio_setup;

	/* Backchannel audio (client → device, recvonly) */
	bool backchannel_setup;
	uint8_t backchannel_interleaved;      /* TCP interleaved channel (RTP) */
	uint8_t backchannel_interleaved_rtcp; /* TCP interleaved channel (RTCP) */
	bool audio_out_opened;

	struct tmr tmr_timeout;    /* session inactivity timeout */
};

/* Server globals */
static struct {
	struct tcp_sock *ts;       /* RTSP listen socket */
	struct list clients;       /* list of rtsp_client */

	/* Media timers (shared for all playing clients) */
	struct tmr tmr_audio;
	struct tmr tmr_video;
	bool camera_opened;
	uint32_t video_frame_count;

	uint16_t port;             /* RTSP listen port */
	char local_ip[64];         /* our IP for SDP */
	bool mic_opened;           /* audio input device open */
} g_rtsp;


/* Forward declarations */
static void rtsp_recv_handler(struct mbuf *mb, void *arg);
static void rtsp_estab_handler(void *arg);
static void rtsp_close_handler(int err, void *arg);
static void audio_send_timer(void *arg);
static void video_send_timer(void *arg);
static void send_h264_nalu_to_client(struct rtsp_client *cli,
				     const uint8_t *nalu, size_t nalu_len,
				     bool is_last_nalu);
static void maybe_stop_media(void);


static void session_timeout_handler(void *arg)
{
	struct rtsp_client *cli = arg;

	re_printf("[RTSP] Session timeout, closing client\n");

	if (cli->state == RTSP_PLAYING) {
		cli->state = RTSP_IDLE;
		maybe_stop_media();
	}

	mem_deref(cli);
}


static void client_destructor(void *data)
{
	struct rtsp_client *cli = data;

	tmr_cancel(&cli->tmr_timeout);

	if (cli->audio_out_opened) {
		audio_out_close();
		cli->audio_out_opened = false;
	}

	list_unlink(&cli->le);
	mem_deref(cli->rtp_video);
	mem_deref(cli->rtp_audio);
	mem_deref(cli->rbuf);
	mem_deref(cli->tc);
}


static bool has_playing_clients(void)
{
	struct le *le;
	for (le = g_rtsp.clients.head; le; le = le->next) {
		struct rtsp_client *cli = le->data;
		if (cli->state == RTSP_PLAYING)
			return true;
	}
	return false;
}


static void ensure_media_started(void)
{
	if (!g_rtsp.camera_opened) {
		int err = camera_open(VIDEO_WIDTH, VIDEO_HEIGHT,
				      VIDEO_FPS, 300);
		if (err && err != EALREADY) {
			re_fprintf(stderr, "[RTSP] camera_open: %d\n", err);
			return;
		}
		g_rtsp.camera_opened = true;
	}

	/* Start microphone capture */
	if (!g_rtsp.mic_opened) {
		int err = audio_in_open(AUDIO_SRATE, 1, AUDIO_SAMPLES);
		if (err) {
			re_fprintf(stderr,
				   "[RTSP] audio_in_open: %d (using silence)\n",
				   err);
		} else {
			g_rtsp.mic_opened = true;
			re_printf("[RTSP] Microphone capture started\n");
		}
	}

	/*
	 * Always (re)start timers unconditionally.
	 *
	 * libre 4.6.0 bug: tmr_cancel() doesn't clear tmr->th,
	 * so tmr_isrunning() returns true even after cancel.
	 * tmr_start() is safe to call on running timers (re-links).
	 */
	tmr_start(&g_rtsp.tmr_video, VIDEO_PTIME,
		  video_send_timer, NULL);
	tmr_start(&g_rtsp.tmr_audio, AUDIO_PTIME,
		  audio_send_timer, NULL);
}


static void maybe_stop_media(void)
{
	if (!has_playing_clients()) {
		tmr_cancel(&g_rtsp.tmr_video);
		tmr_cancel(&g_rtsp.tmr_audio);
		g_rtsp.video_frame_count = 0;
		re_printf("[RTSP] No playing clients, media stopped\n");
	}
}


/*------------------------------------------------------------------------
 * Bind a UDP socket to an ephemeral port, return the port number
 *----------------------------------------------------------------------*/
static int alloc_rtp_socket(struct udp_sock **usp, uint16_t *portp)
{
	struct sa laddr;
	int err;

	sa_set_str(&laddr, "0.0.0.0", 0);
	err = udp_listen(usp, &laddr, NULL, NULL);
	if (err)
		return err;

	{
		struct sa bound;
		udp_local_get(*usp, &bound);
		*portp = sa_port(&bound);
	}
	return 0;
}


/*------------------------------------------------------------------------
 * Send RTSP response over TCP
 *----------------------------------------------------------------------*/
static int rtsp_reply(struct rtsp_client *cli, int cseq,
		      int code, const char *reason,
		      const char *extra_hdrs,
		      const char *body, size_t body_len)
{
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(512 + body_len);
	if (!mb)
		return ENOMEM;

	err = mbuf_printf(mb, "RTSP/1.0 %d %s\r\n", code, reason);
	err |= mbuf_printf(mb, "CSeq: %d\r\n", cseq);

	/* Only include Session header in success responses */
	if (cli->session_id[0] && code >= 200 && code < 300)
		err |= mbuf_printf(mb, "Session: %s;timeout=60\r\n",
				   cli->session_id);

	if (extra_hdrs)
		err |= mbuf_printf(mb, "%s", extra_hdrs);

	if (body && body_len > 0) {
		err |= mbuf_printf(mb, "Content-Length: %zu\r\n", body_len);
		err |= mbuf_printf(mb, "\r\n");
		err |= mbuf_write_mem(mb, (const uint8_t *)body, body_len);
	} else {
		err |= mbuf_printf(mb, "\r\n");
	}

	if (err) {
		mem_deref(mb);
		return err;
	}

	mbuf_set_pos(mb, 0);

	err = tcp_send(cli->tc, mb);
	if (err)
		re_fprintf(stderr, "[RTSP] tcp_send reply %d: %m\n",
			   code, err);
	mem_deref(mb);
	return err;
}


/*------------------------------------------------------------------------
 * Parse RTSP request line and headers from text
 *----------------------------------------------------------------------*/
struct rtsp_req {
	char method[16];
	char uri[256];
	int cseq;
	char transport[256];
	char session[32];
};

static void parse_rtsp_request(const char *buf, size_t len,
			       struct rtsp_req *req)
{
	const char *p = buf;
	const char *end = buf + len;

	memset(req, 0, sizeof(*req));

	/* Request line: METHOD URI RTSP/1.0 */
	{
		const char *sp1, *sp2;
		sp1 = memchr(p, ' ', (size_t)(end - p));
		if (!sp1) return;
		size_t mlen = (size_t)(sp1 - p);
		if (mlen >= sizeof(req->method)) mlen = sizeof(req->method) - 1;
		memcpy(req->method, p, mlen);

		sp1++;
		sp2 = memchr(sp1, ' ', (size_t)(end - sp1));
		if (!sp2) return;
		size_t ulen = (size_t)(sp2 - sp1);
		if (ulen >= sizeof(req->uri)) ulen = sizeof(req->uri) - 1;
		memcpy(req->uri, sp1, ulen);
	}

	/* Headers */
	{
		const char *line = strstr(p, "\r\n");
		while (line && line + 2 < end) {
			line += 2;  /* skip \r\n */
			const char *eol = strstr(line, "\r\n");
			if (!eol) break;

			if (strncasecmp(line, "CSeq:", 5) == 0) {
				req->cseq = atoi(line + 5);
			}
			else if (strncasecmp(line, "Transport:", 10) == 0) {
				const char *val = line + 10;
				while (*val == ' ') val++;
				size_t tlen = (size_t)(eol - val);
				if (tlen >= sizeof(req->transport))
					tlen = sizeof(req->transport) - 1;
				memcpy(req->transport, val, tlen);
			}
			else if (strncasecmp(line, "Session:", 8) == 0) {
				const char *val = line + 8;
				while (*val == ' ') val++;
				size_t slen = (size_t)(eol - val);
				if (slen >= sizeof(req->session))
					slen = sizeof(req->session) - 1;
				memcpy(req->session, val, slen);
			}

			line = eol;
		}
	}
}


/*------------------------------------------------------------------------
 * Parse "client_port=XXXX-YYYY" from Transport header
 *----------------------------------------------------------------------*/
static int parse_client_port(const char *transport,
			     uint16_t *rtp_port, uint16_t *rtcp_port)
{
	const char *p = strstr(transport, "client_port=");
	if (!p) return ENOENT;
	p += 12;

	*rtp_port = (uint16_t)atoi(p);
	const char *dash = strchr(p, '-');
	if (dash)
		*rtcp_port = (uint16_t)atoi(dash + 1);
	else
		*rtcp_port = *rtp_port + 1;

	return 0;
}


/*------------------------------------------------------------------------
 * Parse "interleaved=X-Y" from Transport header
 *----------------------------------------------------------------------*/
static int parse_interleaved(const char *transport,
			     uint8_t *rtp_ch, uint8_t *rtcp_ch)
{
	const char *p = strstr(transport, "interleaved=");
	if (!p) return ENOENT;
	p += 12;

	*rtp_ch = (uint8_t)atoi(p);
	const char *dash = strchr(p, '-');
	if (dash)
		*rtcp_ch = (uint8_t)atoi(dash + 1);
	else
		*rtcp_ch = *rtp_ch + 1;

	return 0;
}


/*------------------------------------------------------------------------
 * Check if Transport header requests TCP interleaved
 *----------------------------------------------------------------------*/
static bool is_tcp_transport(const char *transport)
{
	return (strstr(transport, "RTP/AVP/TCP") != NULL ||
	        strstr(transport, "interleaved=") != NULL);
}


/*------------------------------------------------------------------------
 * Send RTP packet over TCP interleaved (RFC 2326 Section 10.12)
 * Format: '$' + channel(1) + length(2 big-endian) + RTP data
 *----------------------------------------------------------------------*/
static int tcp_interleaved_send(struct rtsp_client *cli,
			       uint8_t channel,
			       struct mbuf *rtp)
{
	size_t rtp_len = mbuf_get_left(rtp);
	struct mbuf *mb = mbuf_alloc(4 + rtp_len);
	if (!mb) return ENOMEM;

	mbuf_write_u8(mb, '$');
	mbuf_write_u8(mb, channel);
	mbuf_write_u8(mb, (uint8_t)(rtp_len >> 8));
	mbuf_write_u8(mb, (uint8_t)(rtp_len & 0xFF));
	mbuf_write_mem(mb, mbuf_buf(rtp), rtp_len);
	mbuf_set_pos(mb, 0);

	int err = tcp_send(cli->tc, mb);
	if (err)
		re_printf("[RTSP] tcp_interleaved_send ch=%u len=%zu: %m\n",
			  channel, rtp_len, err);
	mem_deref(mb);
	return err;
}


/*------------------------------------------------------------------------
 * RTSP method handlers
 *----------------------------------------------------------------------*/
static void handle_options(struct rtsp_client *cli, struct rtsp_req *req)
{
	rtsp_reply(cli, req->cseq, 200, "OK",
		   "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
		   NULL, 0);
}


static void handle_describe(struct rtsp_client *cli, struct rtsp_req *req)
{
	char sdp[2048];
	char sprop[256] = "";
	int n;

	/*
	 * Get SPS/PPS from camera to build sprop-parameter-sets.
	 * If camera isn't open yet, open it to get the params.
	 */
	{
		const uint8_t *sps, *pps;
		size_t sps_len, pps_len;
		bool opened_here = false;

		if (camera_get_sps_pps(&sps, &sps_len,
				       &pps, &pps_len) != 0) {
			/* Camera not open or no params yet — open and
			   encode one frame to extract SPS/PPS */
			int err = camera_open(VIDEO_WIDTH, VIDEO_HEIGHT,
					      VIDEO_FPS, 300);
			if (err == 0 || err == EALREADY) {
				if (err == 0)
					opened_here = true;
				struct camera_frame frm;
				camera_request_keyframe();
				if (camera_get_frame(&frm) == 0) {
					camera_get_sps_pps(&sps, &sps_len,
							   &pps, &pps_len);
				}
			}
			if (!g_rtsp.camera_opened && opened_here) {
				g_rtsp.camera_opened = true;
			}
		}

		if (camera_get_sps_pps(&sps, &sps_len,
				       &pps, &pps_len) == 0) {
			char sps_b64[128], pps_b64[128];
			size_t sps_b64_len = sizeof(sps_b64) - 1;
			size_t pps_b64_len = sizeof(pps_b64) - 1;

			if (base64_encode(sps, sps_len,
					  sps_b64, &sps_b64_len) == 0 &&
			    base64_encode(pps, pps_len,
					  pps_b64, &pps_b64_len) == 0) {
				sps_b64[sps_b64_len] = '\0';
				pps_b64[pps_b64_len] = '\0';
				re_snprintf(sprop, sizeof(sprop),
					    ";sprop-parameter-sets=%s,%s",
					    sps_b64, pps_b64);
			}
		}
	}

	n = re_snprintf(sdp, sizeof(sdp),
		"v=0\r\n"
		"o=- 0 0 IN IP4 %s\r\n"
		"s=Smart Lock Intercom\r\n"
		"c=IN IP4 0.0.0.0\r\n"
		"t=0 0\r\n"
		"m=video 0 RTP/AVP %d\r\n"
		"a=rtpmap:%d H264/90000\r\n"
		"a=fmtp:%d profile-level-id=42e01f;packetization-mode=1%s\r\n"
		"a=sendonly\r\n"
		"a=control:trackID=0\r\n"
		"m=audio 0 RTP/AVP %d\r\n"
		"a=rtpmap:%d PCMU/8000\r\n"
		"a=ptime:%d\r\n"
		"a=sendonly\r\n"
		"a=control:trackID=1\r\n"
		"m=audio 0 RTP/AVP %d\r\n"
		"a=rtpmap:%d PCMU/8000\r\n"
		"a=ptime:%d\r\n"
		"a=recvonly\r\n"
		"a=control:trackID=2\r\n",

		g_rtsp.local_ip,
		PT_H264, PT_H264, PT_H264, sprop,
		PT_PCMU, PT_PCMU, AUDIO_PTIME,
		PT_PCMU, PT_PCMU, AUDIO_PTIME);

	{
		char desc_hdrs[512];
		re_snprintf(desc_hdrs, sizeof(desc_hdrs),
			    "Content-Type: application/sdp\r\n"
			    "Content-Base: %s/\r\n",
			    req->uri);
		rtsp_reply(cli, req->cseq, 200, "OK",
			   desc_hdrs, sdp, (size_t)n);
	}
}


static void handle_setup(struct rtsp_client *cli, struct rtsp_req *req)
{
	char transport_resp[256];
	int err;

	/* Determine which track (exact match to avoid trackID=10 etc.) */
	bool is_video = false, is_audio = false, is_backchannel = false;
	const char *p;

	if ((p = strstr(req->uri, "trackID=0")) != NULL) {
		char c = p[9];
		if (c == '\0' || c == '/' || c == '&' || c == ';' || c == ' ')
			is_video = true;
	}
	if ((p = strstr(req->uri, "trackID=1")) != NULL) {
		char c = p[9];
		if (c == '\0' || c == '/' || c == '&' || c == ';' || c == ' ')
			is_audio = true;
	}
	if ((p = strstr(req->uri, "trackID=2")) != NULL) {
		char c = p[9];
		if (c == '\0' || c == '/' || c == '&' || c == ';' || c == ' ')
			is_backchannel = true;
	}

	/* If no trackID, check if it's the base URL (default to video) */
	if (!is_video && !is_audio && !is_backchannel) {
		if (!cli->video_setup)
			is_video = true;
		else
			is_audio = true;
	}

	/* Check if client wants TCP interleaved transport */
	bool tcp_mode = is_tcp_transport(req->transport);

	if (tcp_mode) {
		/* TCP interleaved mode (RTP over RTSP connection) */
		uint8_t rtp_ch, rtcp_ch;
		err = parse_interleaved(req->transport, &rtp_ch, &rtcp_ch);
		if (err) {
			/* Default interleaved channels */
			rtp_ch  = is_video ? 0 : 2;
			rtcp_ch = is_video ? 1 : 3;
		}

		cli->interleaved = true;

		if (is_video) {
			cli->video_interleaved = rtp_ch;
			cli->video_interleaved_rtcp = rtcp_ch;
			cli->video_ssrc = rand_u32();
			cli->video_setup = true;

			re_snprintf(transport_resp, sizeof(transport_resp),
				    "Transport: RTP/AVP/TCP;unicast;"
				    "interleaved=%u-%u;"
				    "ssrc=%08X\r\n",
				    rtp_ch, rtcp_ch, cli->video_ssrc);

			re_printf("[RTSP] SETUP video TCP interleaved=%u-%u\n",
				  rtp_ch, rtcp_ch);
		}
		else if (is_backchannel) {
			cli->backchannel_interleaved = rtp_ch;
			cli->backchannel_interleaved_rtcp = rtcp_ch;
			cli->backchannel_setup = true;

			re_snprintf(transport_resp, sizeof(transport_resp),
				    "Transport: RTP/AVP/TCP;unicast;"
				    "interleaved=%u-%u\r\n",
				    rtp_ch, rtcp_ch);

			re_printf("[RTSP] SETUP backchannel TCP "
				  "interleaved=%u-%u\n",
				  rtp_ch, rtcp_ch);
		}
		else {
			cli->audio_interleaved = rtp_ch;
			cli->audio_interleaved_rtcp = rtcp_ch;
			cli->audio_ssrc = rand_u32();
			cli->audio_setup = true;

			re_snprintf(transport_resp, sizeof(transport_resp),
				    "Transport: RTP/AVP/TCP;unicast;"
				    "interleaved=%u-%u;"
				    "ssrc=%08X\r\n",
				    rtp_ch, rtcp_ch, cli->audio_ssrc);

			re_printf("[RTSP] SETUP audio TCP interleaved=%u-%u\n",
				  rtp_ch, rtcp_ch);
		}
	}
	else {
		/* UDP mode */
		uint16_t client_rtp, client_rtcp;
		struct sa peer_addr;

		/* Backchannel requires TCP interleaved */
		if (is_backchannel) {
			re_printf("[RTSP] Rejecting UDP for backchannel "
				  "-> use TCP\n");
			rtsp_reply(cli, req->cseq, 461,
				   "Unsupported Transport",
				   NULL, NULL, 0);
			return;
		}

		tcp_conn_peer_get(cli->tc, &peer_addr);

		/*
		 * Reject UDP for remote (non-loopback) clients.
		 * WiFi/NAT/firewall often blocks UDP delivery.
		 * VLC will automatically retry with RTP/AVP/TCP.
		 */
		if (!sa_is_loopback(&peer_addr)) {
			re_printf("[RTSP] Rejecting UDP for remote "
				  "client %J -> use TCP\n",
				  &peer_addr);
			rtsp_reply(cli, req->cseq, 461,
				   "Unsupported Transport",
				   NULL, NULL, 0);
			return;
		}

		err = parse_client_port(req->transport,
					&client_rtp, &client_rtcp);
		if (err) {
			rtsp_reply(cli, req->cseq, 461,
				   "Unsupported Transport",
				   NULL, NULL, 0);
			return;
		}

		if (is_video) {
			if (!cli->rtp_video) {
				err = alloc_rtp_socket(&cli->rtp_video,
						       &cli->video_srv_port);
				if (err) {
					rtsp_reply(cli, req->cseq, 500,
						   "Internal Error",
						   NULL, NULL, 0);
					return;
				}
				cli->video_ssrc = rand_u32();
			}

			sa_set_port(&peer_addr, client_rtp);
			cli->video_dest = peer_addr;
			cli->video_setup = true;

			re_snprintf(transport_resp, sizeof(transport_resp),
				    "Transport: RTP/AVP;unicast;"
				    "client_port=%u-%u;"
				    "server_port=%u-%u;"
				    "ssrc=%08X\r\n",
				    client_rtp, client_rtcp,
				    cli->video_srv_port,
				    cli->video_srv_port + 1,
				    cli->video_ssrc);

			re_printf("[RTSP] SETUP video → %J "
				  "(client_port=%u)\n",
				  &peer_addr, client_rtp);
		}
		else {
			if (!cli->rtp_audio) {
				err = alloc_rtp_socket(&cli->rtp_audio,
						       &cli->audio_srv_port);
				if (err) {
					rtsp_reply(cli, req->cseq, 500,
						   "Internal Error",
						   NULL, NULL, 0);
					return;
				}
				cli->audio_ssrc = rand_u32();
			}

			sa_set_port(&peer_addr, client_rtp);
			cli->audio_dest = peer_addr;
			cli->audio_setup = true;

			re_snprintf(transport_resp, sizeof(transport_resp),
				    "Transport: RTP/AVP;unicast;"
				    "client_port=%u-%u;"
				    "server_port=%u-%u;"
				    "ssrc=%08X\r\n",
				    client_rtp, client_rtcp,
				    cli->audio_srv_port,
				    cli->audio_srv_port + 1,
				    cli->audio_ssrc);

			re_printf("[RTSP] SETUP audio → %J "
				  "(client_port=%u)\n",
				  &peer_addr, client_rtp);
		}
	}

	/* Generate session ID on first successful SETUP */
	if (!cli->session_id[0]) {
		re_snprintf(cli->session_id, sizeof(cli->session_id),
			    "%08x", rand_u32());
	}

	cli->state = RTSP_READY;

	rtsp_reply(cli, req->cseq, 200, "OK",
		   transport_resp, NULL, 0);
}


static void handle_play(struct rtsp_client *cli, struct rtsp_req *req)
{
	if (cli->state != RTSP_READY && cli->state != RTSP_PLAYING) {
		rtsp_reply(cli, req->cseq, 455,
			   "Method Not Valid in This State",
			   NULL, NULL, 0);
		return;
	}

	cli->state = RTSP_PLAYING;

	re_printf("[RTSP] PLAY (interleaved=%d v=%d a=%d)\n",
		  cli->interleaved, cli->video_setup, cli->audio_setup);

	/* Ensure camera + timers are running */
	ensure_media_started();

	/* Force IDR so client gets SPS+PPS+keyframe immediately */
	camera_request_keyframe();

	/* Build RTP-Info header with absolute URLs (RFC 2326) */
	{
		char rtp_info[512];
		/* Strip trailing slash to avoid double-slash in URLs */
		char base_uri[256];
		re_snprintf(base_uri, sizeof(base_uri), "%s", req->uri);
		size_t uri_len = strlen(base_uri);
		while (uri_len > 0 && base_uri[uri_len - 1] == '/')
			base_uri[--uri_len] = '\0';

		if (cli->backchannel_setup)
			re_snprintf(rtp_info, sizeof(rtp_info),
				    "Range: npt=0.000-\r\n"
				    "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%u,"
				    "url=%s/trackID=1;seq=%u;rtptime=%u,"
				    "url=%s/trackID=2;seq=0;rtptime=0\r\n",
				    base_uri, cli->video_seq, cli->video_ts,
				    base_uri, cli->audio_seq, cli->audio_ts,
				    base_uri);
		else
			re_snprintf(rtp_info, sizeof(rtp_info),
				    "Range: npt=0.000-\r\n"
				    "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%u,"
				    "url=%s/trackID=1;seq=%u;rtptime=%u\r\n",
				    base_uri, cli->video_seq, cli->video_ts,
				    base_uri, cli->audio_seq, cli->audio_ts);

		/* Open audio output for backchannel playback */
		if (cli->backchannel_setup && !cli->audio_out_opened) {
			if (audio_out_open(AUDIO_SRATE, 1, AUDIO_SAMPLES) == 0)
				cli->audio_out_opened = true;
			else
				re_fprintf(stderr,
					   "[RTSP] audio_out_open failed\n");
		}

		int err = rtsp_reply(cli, req->cseq, 200, "OK",
				     rtp_info, NULL, 0);
		if (err)
			re_printf("[RTSP] PLAY reply send error: %m\n",
				  err);
	}

	/*
	 * Send first video + audio frame IMMEDIATELY after PLAY response.
	 * Don't wait for the timer — clients need data right away for probe.
	 */
	{
		struct camera_frame frame;
		int err = camera_get_frame(&frame);
		if (err == 0) {
			re_printf("[RTSP] Sending immediate IDR: %d NALs\n",
				  frame.count);
			for (int i = 0; i < frame.count; i++) {
				send_h264_nalu_to_client(cli,
							 frame.nalus[i].data,
							 frame.nalus[i].len,
							 i == frame.count - 1);
			}
			cli->video_ts += VIDEO_CLOCK / VIDEO_FPS;
			g_rtsp.video_frame_count++;
		}
	}

	/* Immediate audio packet */
	if (cli->audio_setup) {
		int16_t pcm[AUDIO_SAMPLES];
		uint8_t pcmu[AUDIO_SAMPLES];

		/* Read mic if available, else silence */
		int nread = 0;
		if (g_rtsp.mic_opened)
			nread = audio_in_read(pcm, AUDIO_SAMPLES);
		if (nread < AUDIO_SAMPLES)
			memset(&pcm[nread], 0,
			       (size_t)(AUDIO_SAMPLES - nread) * sizeof(int16_t));
		for (int i = 0; i < AUDIO_SAMPLES; i++)
			pcmu[i] = g711_pcm2ulaw(pcm[i]);

		struct mbuf *mb = mbuf_alloc(12 + AUDIO_SAMPLES);
		if (mb) {
			struct rtp_header hdr;
			memset(&hdr, 0, sizeof(hdr));
			hdr.ver  = 2;
			hdr.pt   = PT_PCMU;
			hdr.seq  = cli->audio_seq++;
			hdr.ts   = cli->audio_ts;
			hdr.ssrc = cli->audio_ssrc;
			rtp_hdr_encode(mb, &hdr);
			mbuf_write_mem(mb, pcmu, sizeof(pcmu));
			mbuf_set_pos(mb, 0);
			if (cli->interleaved)
				tcp_interleaved_send(cli,
						     cli->audio_interleaved,
						     mb);
			else
				udp_send(cli->rtp_audio,
					 &cli->audio_dest, mb);
			mem_deref(mb);
			cli->audio_ts += AUDIO_SAMPLES;
		}
	}
}


static void handle_teardown(struct rtsp_client *cli, struct rtsp_req *req)
{
	re_printf("[RTSP] TEARDOWN\n");

	rtsp_reply(cli, req->cseq, 200, "OK", NULL, NULL, 0);

	cli->state = RTSP_IDLE;
	maybe_stop_media();

	/* Close the TCP connection after teardown */
	mem_deref(cli);
}


/*------------------------------------------------------------------------
 * Dispatch RTSP request — returns true if client was freed (TEARDOWN)
 *----------------------------------------------------------------------*/
static bool dispatch_rtsp(struct rtsp_client *cli,
			  const char *buf, size_t len)
{
	struct rtsp_req req;

	parse_rtsp_request(buf, len, &req);

	re_printf("[RTSP] %s %s CSeq=%d\n", req.method, req.uri, req.cseq);

	if (strcmp(req.method, "OPTIONS") == 0)
		handle_options(cli, &req);
	else if (strcmp(req.method, "DESCRIBE") == 0)
		handle_describe(cli, &req);
	else if (strcmp(req.method, "SETUP") == 0)
		handle_setup(cli, &req);
	else if (strcmp(req.method, "PLAY") == 0)
		handle_play(cli, &req);
	else if (strcmp(req.method, "TEARDOWN") == 0) {
		handle_teardown(cli, &req);
		return true;
	}
	else if (strcmp(req.method, "GET_PARAMETER") == 0) {
		/* Keepalive — just reply 200 OK */
		rtsp_reply(cli, req.cseq, 200, "OK", NULL, NULL, 0);
	}
	else {
		rtsp_reply(cli, req.cseq, 405, "Method Not Allowed",
			   NULL, NULL, 0);
	}

	/* Reset session inactivity timeout on recognized methods */
	if (strcmp(req.method, "OPTIONS") == 0 ||
	    strcmp(req.method, "DESCRIBE") == 0 ||
	    strcmp(req.method, "SETUP") == 0 ||
	    strcmp(req.method, "PLAY") == 0 ||
	    strcmp(req.method, "GET_PARAMETER") == 0) {
		tmr_start(&cli->tmr_timeout, 60000,
			  session_timeout_handler, cli);
	}

	return false;
}


/*------------------------------------------------------------------------
 * TCP callbacks
 *----------------------------------------------------------------------*/
static void rtsp_recv_handler(struct mbuf *mb, void *arg)
{
	struct rtsp_client *cli = arg;
	const uint8_t *data;
	size_t len;

	if (!cli->rbuf) {
		cli->rbuf = mbuf_alloc(4096);
		if (!cli->rbuf) return;
	}

	/* Append received data */
	mbuf_set_pos(cli->rbuf, cli->rbuf->end);
	mbuf_write_mem(cli->rbuf, mbuf_buf(mb), mbuf_get_left(mb));

	/* Reject if buffer exceeds sane limit (16KB) */
	if (cli->rbuf->end > 16384) {
		re_printf("[RTSP] Client buffer overflow, closing\n");
		mem_deref(cli);
		return;
	}

	/* Process complete requests / interleaved frames */
	for (;;) {
		data = cli->rbuf->buf;
		len = cli->rbuf->end;

		if (len == 0)
			break;

		/*
		 * Skip interleaved RTP/RTCP frames from client.
		 * Format: '$' + channel(1) + length(2 BE) + data
		 */
		if (data[0] == '$') {
			if (len < 4)
				break;  /* need more data */
			uint8_t channel = data[1];
			uint16_t frame_len = (uint16_t)(data[2] << 8 | data[3]);
			size_t total = 4 + frame_len;
			if (len < total)
				break;  /* need more data */

			/* Backchannel audio: decode RTP and play */
			if (cli->backchannel_setup &&
			    channel == cli->backchannel_interleaved &&
			    frame_len >= 12) {
				const uint8_t *rtp = data + 4;

				/* Parse variable-length RTP header */
				uint8_t cc = rtp[0] & 0x0F;
				bool ext = (rtp[0] >> 4) & 0x01;
				size_t hdr_len = 12 + cc * 4;

				if (ext && frame_len >= hdr_len + 4) {
					uint16_t ext_len =
					    (uint16_t)(rtp[hdr_len + 2] << 8
						     | rtp[hdr_len + 3]);
					hdr_len += 4 + ext_len * 4;
				}

				if (frame_len > hdr_len && cli->audio_out_opened) {
					const uint8_t *pcmu = rtp + hdr_len;
					size_t pcmu_len = frame_len - hdr_len;
					audio_out_play(pcmu, pcmu_len);
				}
			}
			/* else: discard RTCP or unknown channel */

			size_t remain = len - total;
			if (remain > 0)
				memmove(cli->rbuf->buf,
					data + total, remain);
			cli->rbuf->end = remain;
			continue;
		}

		/* Ensure space for null terminator */
		if (len >= cli->rbuf->size)
			mbuf_resize(cli->rbuf, len + 1);
		data = cli->rbuf->buf;  /* may have moved */

		/* Null-terminate for strstr */
		cli->rbuf->buf[len] = '\0';

		const char *eor = strstr((const char *)data, "\r\n\r\n");
		if (!eor)
			break;

		size_t req_len = (size_t)(eor - (const char *)data) + 4;

		if (dispatch_rtsp(cli, (const char *)data, req_len))
			return;  /* client freed by TEARDOWN */

		/* Shift remaining data */
		size_t remain = len - req_len;
		if (remain > 0)
			memmove(cli->rbuf->buf, data + req_len, remain);
		cli->rbuf->end = remain;
	}
}


static void rtsp_estab_handler(void *arg)
{
	struct rtsp_client *cli = arg;
	(void)cli;
	re_printf("[RTSP] TCP connection established\n");
}


static void rtsp_close_handler(int err, void *arg)
{
	struct rtsp_client *cli = arg;

	re_printf("[RTSP] TCP connection closed: %m\n", err);

	if (cli->state == RTSP_PLAYING) {
		cli->state = RTSP_IDLE;
		maybe_stop_media();
	}

	mem_deref(cli);
}


/*------------------------------------------------------------------------
 * New RTSP connection
 *----------------------------------------------------------------------*/
static void rtsp_conn_handler(const struct sa *peer, void *arg)
{
	struct rtsp_client *cli;
	int err;

	(void)arg;

	re_printf("[RTSP] New connection from %J\n", peer);

	cli = mem_zalloc(sizeof(*cli), client_destructor);
	if (!cli) {
		tcp_reject(g_rtsp.ts);
		return;
	}

	cli->backchannel_interleaved = 0xFF;
	cli->backchannel_interleaved_rtcp = 0xFF;

	err = tcp_accept(&cli->tc, g_rtsp.ts,
			 rtsp_estab_handler,
			 rtsp_recv_handler,
			 rtsp_close_handler, cli);
	if (err) {
		re_fprintf(stderr, "[RTSP] tcp_accept: %m\n", err);
		mem_deref(cli);
		return;
	}

	/* Disable Nagle — send small audio RTP packets immediately */
	tcp_conn_set_nodelay(cli->tc, true);

	/* Start 60-second session inactivity timeout */
	tmr_init(&cli->tmr_timeout);
	tmr_start(&cli->tmr_timeout, 60000,
		  session_timeout_handler, cli);

	list_append(&g_rtsp.clients, &cli->le, cli);
}


/*------------------------------------------------------------------------
 * Audio send timer — G.711 µ-law to all playing RTSP clients
 *----------------------------------------------------------------------*/
static void audio_send_timer(void *arg)
{
	struct le *le;
	uint8_t pcmu[AUDIO_SAMPLES];
	int16_t pcm[AUDIO_SAMPLES];

	(void)arg;

	/* Always reschedule first */
	tmr_start(&g_rtsp.tmr_audio, AUDIO_PTIME, audio_send_timer, NULL);

	if (!has_playing_clients())
		return;

	/* Drain excess ring buffer to prevent accumulating latency.
	 * Keep at most 1 frame ahead — discard older data. */
	if (g_rtsp.mic_opened) {
		int avail = audio_in_available();
		if (avail > AUDIO_SAMPLES * 2) {
			int16_t discard[AUDIO_SAMPLES];
			int skip = avail - AUDIO_SAMPLES;
			while (skip > 0) {
				int n = audio_in_read(discard,
						     skip > AUDIO_SAMPLES
						     ? AUDIO_SAMPLES : skip);
				if (n == 0) break;
				skip -= n;
			}
		}
	}

	/* Read from microphone, fall back to silence */
	int nread = 0;
	if (g_rtsp.mic_opened)
		nread = audio_in_read(pcm, AUDIO_SAMPLES);

	if (nread < AUDIO_SAMPLES)
		memset(&pcm[nread], 0,
		       (size_t)(AUDIO_SAMPLES - nread) * sizeof(int16_t));

	/* PCM → G.711 µ-law */
	for (int i = 0; i < AUDIO_SAMPLES; i++)
		pcmu[i] = g711_pcm2ulaw(pcm[i]);

	/* Send to each playing client */
	for (le = g_rtsp.clients.head; le; le = le->next) {
		struct rtsp_client *cli = le->data;

		if (cli->state != RTSP_PLAYING || !cli->audio_setup)
			continue;

		struct mbuf *mb = mbuf_alloc(12 + AUDIO_SAMPLES);
		if (!mb) continue;

		struct rtp_header hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.ver  = 2;
		hdr.pt   = PT_PCMU;
		hdr.seq  = cli->audio_seq++;
		hdr.ts   = cli->audio_ts;
		hdr.ssrc = cli->audio_ssrc;

		rtp_hdr_encode(mb, &hdr);
		mbuf_write_mem(mb, pcmu, sizeof(pcmu));
		mbuf_set_pos(mb, 0);

		if (cli->interleaved)
			tcp_interleaved_send(cli,
					     cli->audio_interleaved, mb);
		else
			udp_send(cli->rtp_audio, &cli->audio_dest, mb);
		mem_deref(mb);

		cli->audio_ts += AUDIO_SAMPLES;
	}
}


/*------------------------------------------------------------------------
 * Send H.264 NAL over RTP to a single RTSP client
 *----------------------------------------------------------------------*/
static void send_h264_nalu_to_client(struct rtsp_client *cli,
				     const uint8_t *nalu, size_t nalu_len,
				     bool is_last_nalu)
{
	if (nalu_len < 1)
		return;

	uint8_t nal_header = nalu[0];

	if (nalu_len <= RTP_MTU) {
		struct mbuf *mb = mbuf_alloc(12 + nalu_len);
		if (!mb) return;

		struct rtp_header hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.ver  = 2;
		hdr.m    = is_last_nalu ? 1 : 0;
		hdr.pt   = PT_H264;
		hdr.seq  = cli->video_seq++;
		hdr.ts   = cli->video_ts;
		hdr.ssrc = cli->video_ssrc;

		rtp_hdr_encode(mb, &hdr);
		mbuf_write_mem(mb, nalu, nalu_len);
		mbuf_set_pos(mb, 0);

		if (cli->interleaved)
			tcp_interleaved_send(cli,
					     cli->video_interleaved, mb);
		else
			udp_send(cli->rtp_video, &cli->video_dest, mb);
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

			struct mbuf *mb = mbuf_alloc(12 + 2 + chunk);
			if (!mb) return;

			struct rtp_header hdr;
			memset(&hdr, 0, sizeof(hdr));
			hdr.ver  = 2;
			hdr.m    = (last_frag && is_last_nalu) ? 1 : 0;
			hdr.pt   = PT_H264;
			hdr.seq  = cli->video_seq++;
			hdr.ts   = cli->video_ts;
			hdr.ssrc = cli->video_ssrc;

			rtp_hdr_encode(mb, &hdr);
			mbuf_write_u8(mb, fu_indicator);

			uint8_t fu_header = nal_type & 0x1F;
			if (first) fu_header |= 0x80;
			if (last_frag) fu_header |= 0x40;
			mbuf_write_u8(mb, fu_header);

			mbuf_write_mem(mb, payload + offset, chunk);
			mbuf_set_pos(mb, 0);

			if (cli->interleaved)
				tcp_interleaved_send(cli,
						     cli->video_interleaved,
						     mb);
			else
				udp_send(cli->rtp_video,
					 &cli->video_dest, mb);
			mem_deref(mb);

			offset += chunk;
			first = false;
		}
	}
}


/*------------------------------------------------------------------------
 * Video send timer — H.264 to all playing RTSP clients
 *----------------------------------------------------------------------*/
static void video_send_timer(void *arg)
{
	struct camera_frame frame;
	struct le *le;
	int err;

	(void)arg;

	/* Always reschedule first */
	tmr_start(&g_rtsp.tmr_video, VIDEO_PTIME, video_send_timer, NULL);

	if (!has_playing_clients())
		return;

	/* Force IDR every 2 seconds for robustness */
	if (g_rtsp.video_frame_count > 0 &&
	    g_rtsp.video_frame_count % (VIDEO_FPS * 2) == 0)
		camera_request_keyframe();

	err = camera_get_frame(&frame);
	if (err) {
		if (err != EAGAIN)
			re_fprintf(stderr, "[RTSP] camera_get_frame: %d\n",
				   err);
		return;
	}

	if (g_rtsp.video_frame_count < 10 ||
	    g_rtsp.video_frame_count % (VIDEO_FPS * 5) == 0) {
		re_printf("[RTSP] frame #%u %s %d NALs\n",
			  g_rtsp.video_frame_count,
			  frame.is_keyframe ? "IDR" : "P",
			  frame.count);
	}

	for (le = g_rtsp.clients.head; le; le = le->next) {
		struct rtsp_client *cli = le->data;

		if (cli->state != RTSP_PLAYING || !cli->video_setup)
			continue;

		for (int i = 0; i < frame.count; i++) {
			send_h264_nalu_to_client(cli,
						 frame.nalus[i].data,
						 frame.nalus[i].len,
						 i == frame.count - 1);
		}

		cli->video_ts += VIDEO_CLOCK / VIDEO_FPS;
	}

	g_rtsp.video_frame_count++;
}


/*------------------------------------------------------------------------
 * Public API
 *----------------------------------------------------------------------*/
int rtsp_server_init(const struct sa *laddr)
{
	struct sa addr;
	int err;

	memset(&g_rtsp, 0, sizeof(g_rtsp));
	list_init(&g_rtsp.clients);
	tmr_init(&g_rtsp.tmr_audio);
	tmr_init(&g_rtsp.tmr_video);

	/* Request 1ms timer resolution on Windows for accurate RTP pacing */
	timeBeginPeriod(1);

	err = tcp_listen(&g_rtsp.ts, laddr, rtsp_conn_handler, NULL);
	if (err) {
		re_fprintf(stderr, "[RTSP] tcp_listen: %m\n", err);
		return err;
	}

	tcp_local_get(g_rtsp.ts, &addr);
	g_rtsp.port = sa_port(&addr);

	/* Determine our LAN IP for SDP */
	{
		struct sa def;
		if (0 == net_default_source_addr_get(AF_INET, &def)) {
			re_snprintf(g_rtsp.local_ip,
				    sizeof(g_rtsp.local_ip),
				    "%j", &def);
		} else {
			str_ncpy(g_rtsp.local_ip, "0.0.0.0",
				 sizeof(g_rtsp.local_ip));
		}
	}

	re_printf("[RTSP] Server listening on %J (rtsp://%s:%u/live)\n",
		  &addr, g_rtsp.local_ip, g_rtsp.port);

	return 0;
}


void rtsp_server_close(void)
{
	/* Stop media timers */
	tmr_cancel(&g_rtsp.tmr_audio);
	tmr_cancel(&g_rtsp.tmr_video);

	/* Close all clients */
	list_flush(&g_rtsp.clients);

	/* Close listener */
	g_rtsp.ts = mem_deref(g_rtsp.ts);

	if (g_rtsp.camera_opened) {
		camera_close();
		g_rtsp.camera_opened = false;
	}

	if (g_rtsp.mic_opened) {
		audio_in_close();
		g_rtsp.mic_opened = false;
	}

	timeEndPeriod(1);
}
