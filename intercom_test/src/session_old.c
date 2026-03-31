/*
 * session.c — Full WebRTC session: ICE-lite + DTLS-SRTP + RTP audio
 *
 * Single UDP port multiplexes:
 *   - STUN binding (ICE connectivity checks)
 *   - DTLS handshake (key exchange)
 *   - SRTP (encrypted audio media)
 *
 * Flow:
 *   1. Create offer with UDP/TLS/RTP/SAVPF + ice-lite + fingerprint
 *   2. Browser sends STUN binding request → we respond → ICE done
 *   3. Browser starts DTLS ClientHello → we accept → handshake completes
 *   4. Extract SRTP keys from DTLS → create SRTP sessions
 *   5. Send/receive SRTP-encrypted G.711 audio
 */
#include <string.h>
#include <math.h>
#include <re.h>
#include <rem.h>
#include "intercom.h"
#include "camera.h"

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

/* SRTP_AES128_CM_HMAC_SHA1_80: 16-byte key + 14-byte salt = 30 bytes */
#define SRTP_KEY_LEN  30


/* Forward declarations */
static void udp_recv_handler(const struct sa *src, struct mbuf *mb, void *arg);
static void dtls_conn_handler(const struct sa *peer, void *arg);
static void dtls_estab_handler(void *arg);
static void dtls_recv_handler(struct mbuf *mb, void *arg);
static void dtls_close_handler(int err, void *arg);
static void audio_send_timer(void *arg);
static void video_send_timer(void *arg);


static void session_destructor(void *data)
{
	struct intercom_session *sess = data;

	tmr_cancel(&sess->tmr_audio);
	tmr_cancel(&sess->tmr_video);
	camera_close();
	mem_deref(sess->srtp_tx);
	mem_deref(sess->srtp_rx);
	mem_deref(sess->dtls_conn);
	mem_deref(sess->dtls_sock);
	mem_deref(sess->tls);
	mem_deref(sess->us);
}


static void format_fingerprint(char *dst, size_t dstsz,
			       const uint8_t *fp, size_t fplen)
{
	size_t pos = 0;
	for (size_t i = 0; i < fplen && pos + 3 < dstsz; i++) {
		if (i > 0)
			dst[pos++] = ':';
		re_snprintf(dst + pos, dstsz - pos, "%02X", fp[i]);
		pos += 2;
	}
	dst[pos] = '\0';
}


/* Extract "a=<name>:<value>" from SDP text */
static int sdp_extract_attr(const char *sdp, size_t sdplen,
			    const char *name, char *val, size_t valsz)
{
	char pat[64];
	const char *p, *end, *sdp_end;
	size_t vlen;

	re_snprintf(pat, sizeof(pat), "a=%s:", name);
	sdp_end = sdp + sdplen;

	p = strstr(sdp, pat);
	if (!p || p >= sdp_end)
		return ENOENT;

	p += strlen(pat);

	end = strstr(p, "\r\n");
	if (!end || end > sdp_end)
		end = sdp_end;

	vlen = (size_t)(end - p);
	if (vlen >= valsz)
		vlen = valsz - 1;

	memcpy(val, p, vlen);
	val[vlen] = '\0';
	return 0;
}


/* Callback for net_if_apply — add host candidates to SDP mbuf */
struct cand_ctx {
	struct mbuf *mb;
	uint16_t port;
	int count;
};

static bool add_host_candidate(const char *ifname, const struct sa *sa,
			       void *arg)
{
	struct cand_ctx *ctx = arg;

	(void)ifname;

	if (sa_af(sa) != AF_INET)
		return false;   /* IPv4 only */
	if (sa_is_loopback(sa))
		return false;
	if (sa_is_linklocal(sa))
		return false;

	ctx->count++;

	mbuf_printf(ctx->mb,
		    "a=candidate:%d 1 UDP %u %j %u typ host\r\n",
		    ctx->count,
		    (uint32_t)(2130706431u - (uint32_t)(ctx->count - 1)),
		    sa, ctx->port);

	re_printf("[SESSION]   candidate: %j:%u (%s)\n",
		  sa, ctx->port, ifname);

	return false;   /* continue */
}


/*------------------------------------------------------------------------
 * Session allocation — TLS context + self-signed cert + SRTP config
 *----------------------------------------------------------------------*/
int session_alloc(struct intercom_session **sessp)
{
	struct intercom_session *sess;
	int err;

	sess = mem_zalloc(sizeof(*sess), session_destructor);
	if (!sess)
		return ENOMEM;

	tmr_init(&sess->tmr_audio);

	/* ICE credentials */
	rand_str(sess->lufrag, sizeof(sess->lufrag));
	rand_str(sess->lpwd, sizeof(sess->lpwd));

	/* Random SSRC for RTP */
	sess->ssrc = rand_u32();
	sess->video_ssrc = rand_u32();

	/* H.264 camera/encoder init */
	{
		int cerr = camera_open(VIDEO_WIDTH, VIDEO_HEIGHT,
				       VIDEO_FPS, 300);
		if (cerr) {
			re_fprintf(stderr, "[H264] camera_open failed: %d\n",
				   cerr);
			err = cerr;
			goto fail;
		}
		re_printf("[H264] Encoder ready: %ux%u @ %u fps, %u kbps\n",
			  VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS, 300);
	}

	tmr_init(&sess->tmr_video);

	/* DTLS context */
	err = tls_alloc(&sess->tls, TLS_METHOD_DTLS, NULL, NULL);
	if (err) {
		re_fprintf(stderr, "[SESSION] tls_alloc: %m\n", err);
		goto fail;
	}

	/* Self-signed EC certificate */
	err = tls_set_selfsigned_ec(sess->tls, "intercom", "prime256v1");
	if (err) {
		re_fprintf(stderr, "[SESSION] selfsigned_ec: %m\n", err);
		goto fail;
	}

	/* DTLS-SRTP cipher suite negotiation */
	err = tls_set_srtp(sess->tls,
			   "SRTP_AES128_CM_SHA1_80:"
			   "SRTP_AES128_CM_SHA1_32");
	if (err) {
		re_fprintf(stderr, "[SESSION] tls_set_srtp: %m\n", err);
		goto fail;
	}

	/* Don't verify peer cert (self-signed WebRTC) */
	tls_set_verify_client_trust_all(sess->tls);

	/* Certificate fingerprint for SDP a=fingerprint */
	err = tls_fingerprint(sess->tls, TLS_FINGERPRINT_SHA256,
			      sess->dtls_fp, sizeof(sess->dtls_fp));
	if (err) {
		re_fprintf(stderr, "[SESSION] fingerprint: %m\n", err);
		goto fail;
	}
	format_fingerprint(sess->dtls_fp_str, sizeof(sess->dtls_fp_str),
			   sess->dtls_fp, 32);

	sess->active = true;
	*sessp = sess;

	re_printf("[SESSION] created  ufrag=%s\n", sess->lufrag);
	re_printf("[SESSION] fingerprint=%s\n", sess->dtls_fp_str);
	return 0;

fail:
	mem_deref(sess);
	return err;
}


/*------------------------------------------------------------------------
 * Create SDP offer — bind UDP + attach DTLS + build WebRTC SDP
 *----------------------------------------------------------------------*/
int session_create_offer(struct intercom_session *sess,
			 struct mbuf **sdp_offer)
{
	struct sa laddr;
	struct mbuf *mb = NULL;
	struct cand_ctx ctx;
	uint16_t port;
	int err;

	/* Bind UDP socket to 0.0.0.0:<random> */
	sa_set_str(&laddr, "0.0.0.0", 0);
	err = udp_listen(&sess->us, &laddr, udp_recv_handler, sess);
	if (err) {
		re_fprintf(stderr, "[SESSION] udp_listen: %m\n", err);
		return err;
	}

#ifdef _WIN32
	/*
	 * Windows reports ICMP "port unreachable" as WSAECONNRESET on
	 * subsequent recvfrom(), which spins libre's event loop.
	 * Disable SIO_UDP_CONNRESET to prevent this.
	 */
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
		port = sa_port(&bound);
	}
	re_printf("[SESSION] UDP port: %u\n", port);

	/* Attach DTLS layer on top of UDP socket */
	err = dtls_listen(&sess->dtls_sock, NULL, sess->us,
			  4, 10, dtls_conn_handler, sess);
	if (err) {
		re_fprintf(stderr, "[SESSION] dtls_listen: %m\n", err);
		return err;
	}

	/* dtls_accept will be called from dtls_conn_handler
	 * when the browser sends a DTLS ClientHello */

	/*
	 * Build SDP offer manually for full control.
	 *
	 * Key WebRTC attributes:
	 *   - UDP/TLS/RTP/SAVPF  (DTLS-SRTP with RTCP feedback)
	 *   - a=ice-lite         (we only respond to STUN, don't initiate)
	 *   - a=fingerprint      (DTLS cert hash for verification)
	 *   - a=setup:actpass    (we can be either DTLS client or server)
	 *   - a=rtcp-mux         (RTP and RTCP on same port)
	 *   - a=group:BUNDLE     (all media on one transport)
	 */
	mb = mbuf_alloc(4096);
	if (!mb)
		return ENOMEM;

	err = mbuf_printf(mb,
		"v=0\r\n"
		"o=- %u %u IN IP4 0.0.0.0\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"a=ice-lite\r\n"
		"a=group:BUNDLE 0 1\r\n"
		"m=audio %u UDP/TLS/RTP/SAVPF 0\r\n"
		"c=IN IP4 0.0.0.0\r\n"
		"a=mid:0\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=sendrecv\r\n"
		"a=rtcp-mux\r\n"
		"a=ice-ufrag:%s\r\n"
		"a=ice-pwd:%s\r\n"
		"a=fingerprint:sha-256 %s\r\n"
		"a=setup:actpass\r\n",
		rand_u32(), rand_u32(),
		port,
		sess->lufrag,
		sess->lpwd,
		sess->dtls_fp_str);
	if (err) {
		mem_deref(mb);
		return err;
	}

	/* Add host candidates for audio */
	re_printf("[SESSION] Host candidates:\n");
	ctx.mb = mb;
	ctx.port = port;
	ctx.count = 0;
	net_if_apply(add_host_candidate, &ctx);

	if (ctx.count == 0) {
		re_fprintf(stderr, "[SESSION] WARNING: no network interfaces!\n");
	}

	/* Video m-line (sendonly — camera to phone, H.264 Constrained Baseline) */
	err = mbuf_printf(mb,
		"m=video %u UDP/TLS/RTP/SAVPF %d\r\n"
		"c=IN IP4 0.0.0.0\r\n"
		"a=mid:1\r\n"
		"a=rtpmap:%d H264/90000\r\n"
		"a=fmtp:%d profile-level-id=42e01f;packetization-mode=1\r\n"
		"a=sendonly\r\n"
		"a=rtcp-mux\r\n"
		"a=ice-ufrag:%s\r\n"
		"a=ice-pwd:%s\r\n"
		"a=fingerprint:sha-256 %s\r\n"
		"a=setup:actpass\r\n",
		port, PT_H264,
		PT_H264,
		PT_H264,
		sess->lufrag,
		sess->lpwd,
		sess->dtls_fp_str);
	if (err) {
		mem_deref(mb);
		return err;
	}

	/* Add host candidates for video (same port, BUNDLE) */
	ctx.count = 0;
	net_if_apply(add_host_candidate, &ctx);

	mbuf_set_pos(mb, 0);

	re_printf("[SESSION] SDP Offer (%zu bytes):\n%b\n",
		  mbuf_get_left(mb), mbuf_buf(mb), mbuf_get_left(mb));

	*sdp_offer = mb;
	return 0;
}


/*------------------------------------------------------------------------
 * Handle SDP answer — extract remote ICE credentials
 *----------------------------------------------------------------------*/
int session_handle_answer(struct intercom_session *sess,
			  const char *sdp_answer, size_t len)
{
	char *sdp;

	/* Null-terminate for string operations */
	sdp = mem_alloc(len + 1, NULL);
	if (!sdp)
		return ENOMEM;
	memcpy(sdp, sdp_answer, len);
	sdp[len] = '\0';

	re_printf("[SESSION] SDP Answer (%zu bytes):\n%s\n", len, sdp);

	/* Extract remote ICE credentials (needed for STUN validation) */
	sdp_extract_attr(sdp, len, "ice-ufrag",
			 sess->rufrag, sizeof(sess->rufrag));
	sdp_extract_attr(sdp, len, "ice-pwd",
			 sess->rpwd, sizeof(sess->rpwd));

	re_printf("[SESSION] Remote ICE ufrag=%s\n", sess->rufrag);

	/* Extract remote setup direction (for DTLS role) */
	{
		char setup[16] = "";
		sdp_extract_attr(sdp, len, "setup",
				 setup, sizeof(setup));
		re_printf("[SESSION] Remote setup=%s\n", setup);
	}

	mem_deref(sdp);
	return 0;
}


/*------------------------------------------------------------------------
 * STUN binding request handler — respond to ICE connectivity checks
 *----------------------------------------------------------------------*/
static void handle_stun(struct intercom_session *sess,
			const struct sa *src, struct mbuf *mb)
{
	struct stun_msg *msg = NULL;
	struct stun_attr *attr;
	int err;

	err = stun_msg_decode(&msg, mb, NULL);
	if (err)
		return;

	/* Only handle binding requests */
	if (stun_msg_method(msg) != STUN_METHOD_BINDING ||
	    stun_msg_class(msg) != STUN_CLASS_REQUEST) {
		mem_deref(msg);
		return;
	}

	/* Validate USERNAME = "local_ufrag:remote_ufrag" */
	attr = stun_msg_attr(msg, STUN_ATTR_USERNAME);
	if (!attr) {
		re_printf("[STUN] no USERNAME\n");
		mem_deref(msg);
		return;
	}

	{
		size_t ufrag_len = strlen(sess->lufrag);
		if (strncmp(attr->v.str, sess->lufrag, ufrag_len) != 0 ||
		    attr->v.str[ufrag_len] != ':') {
			re_printf("[STUN] USERNAME mismatch: %s\n",
				  attr->v.str);
			mem_deref(msg);
			return;
		}
	}

	/* Verify MESSAGE-INTEGRITY with our ICE password */
	err = stun_msg_chk_mi(msg,
			      (const uint8_t *)sess->lpwd,
			      strlen(sess->lpwd));
	if (err) {
		re_printf("[STUN] MI check failed: %m\n", err);
		/* respond anyway for interop */
	}

	/* Send binding success response with XOR-MAPPED-ADDRESS */
	err = stun_reply(IPPROTO_UDP, sess->us, src, 0, msg,
			 (const uint8_t *)sess->lpwd, strlen(sess->lpwd),
			 true,   /* include FINGERPRINT */
			 1, STUN_ATTR_XOR_MAPPED_ADDR, src);
	if (err) {
		re_fprintf(stderr, "[STUN] reply error: %m\n", err);
		mem_deref(msg);
		return;
	}

	re_printf("[STUN] Binding response → %J\n", src);

	/* Update peer address on first check or nomination */
	{
		struct stun_attr *uc;
		uc = stun_msg_attr(msg, STUN_ATTR_USE_CAND);
		if (uc || !sess->ice_done) {
			sess->peer_addr = *src;
			sess->ice_done = true;
			re_printf("[ICE] Peer address: %J%s\n", src,
				  uc ? " (nominated)" : "");
		}
	}

	mem_deref(msg);
}


/*------------------------------------------------------------------------
 * SRTP receive handler — decrypt and process incoming audio
 *----------------------------------------------------------------------*/
static void handle_srtp(struct intercom_session *sess,
			const struct sa *src, struct mbuf *mb)
{
	struct rtp_header hdr;
	int err;
	(void)src;

	if (!sess->established || !sess->srtp_rx)
		return;

	err = srtp_decrypt(sess->srtp_rx, mb);
	if (err)
		return;  /* silently drop — likely RTCP/SRTCP */

	err = rtp_hdr_decode(&hdr, mb);
	if (err)
		return;

	/* TODO: Decode G.711 → PCM → speaker. For now just log. */
	re_printf("[RTP-RX] pt=%u seq=%u ts=%u len=%zu\n",
		  hdr.pt, hdr.seq, hdr.ts, mbuf_get_left(mb));
}


/*------------------------------------------------------------------------
 * Main UDP receive handler — demux STUN / DTLS / SRTP
 *
 * dtls_listen already intercepts DTLS records (first byte 20-63).
 * This handler receives everything else: STUN and SRTP.
 *----------------------------------------------------------------------*/
static void udp_recv_handler(const struct sa *src, struct mbuf *mb,
			     void *arg)
{
	struct intercom_session *sess = arg;
	uint8_t b;

	if (mbuf_get_left(mb) < 1)
		return;

	if (!sess->active)
		return;

	b = mbuf_buf(mb)[0];

	if (b <= 3) {
		/* STUN (0x00-0x03) */
		handle_stun(sess, src, mb);
	}
	else if (b >= 128 && b <= 191) {
		/* RTP / SRTP */
		handle_srtp(sess, src, mb);
	}
	else if (b >= 192) {
		/* RTCP / SRTCP — ignore for now */
	}
	/* 4-19, 64-127: unexpected; DTLS (20-63) handled by dtls_listen */
}


/*------------------------------------------------------------------------
 * DTLS callbacks
 *----------------------------------------------------------------------*/
static void dtls_conn_handler(const struct sa *peer, void *arg)
{
	struct intercom_session *sess = arg;
	int err;

	re_printf("[DTLS] ClientHello from %J\n", peer);

	if (sess->dtls_conn) {
		re_printf("[DTLS] already have connection, ignoring\n");
		return;
	}

	err = dtls_accept(&sess->dtls_conn, sess->tls,
			  sess->dtls_sock,
			  dtls_estab_handler,
			  dtls_recv_handler,
			  dtls_close_handler, sess);
	if (err)
		re_fprintf(stderr, "[DTLS] accept: %m\n", err);
	else
		re_printf("[DTLS] Accepted, handshaking...\n");
}


static void dtls_estab_handler(void *arg)
{
	struct intercom_session *sess = arg;
	enum srtp_suite suite;
	uint8_t cli_key[SRTP_KEY_LEN];
	uint8_t srv_key[SRTP_KEY_LEN];
	int err;

	re_printf("[DTLS] Established! cipher=%s\n",
		  tls_cipher_name(sess->dtls_conn));

	/* Extract SRTP keying material from DTLS */
	err = tls_srtp_keyinfo(sess->dtls_conn, &suite,
			       cli_key, sizeof(cli_key),
			       srv_key, sizeof(srv_key));
	if (err) {
		re_fprintf(stderr, "[DTLS] srtp_keyinfo: %m\n", err);
		return;
	}

	re_printf("[SRTP] Suite: %s\n", srtp_suite_name(suite));

	/*
	 * We are the DTLS server (dtls_accept):
	 *   srv_key → our sending key
	 *   cli_key → our receiving key (browser sends with this)
	 */
	err = srtp_alloc(&sess->srtp_tx, suite,
			 srv_key, sizeof(srv_key), 0);
	if (err) {
		re_fprintf(stderr, "[SRTP] alloc tx: %m\n", err);
		return;
	}

	err = srtp_alloc(&sess->srtp_rx, suite,
			 cli_key, sizeof(cli_key), 0);
	if (err) {
		re_fprintf(stderr, "[SRTP] alloc rx: %m\n", err);
		return;
	}

	sess->established = true;
	re_printf("[SESSION] *** ESTABLISHED — media ready ***\n");

	/* Start sending G.711 test tone + H.264 video */
	tmr_start(&sess->tmr_audio, AUDIO_PTIME, audio_send_timer, sess);
	tmr_start(&sess->tmr_video, VIDEO_PTIME, video_send_timer, sess);
}


static void dtls_recv_handler(struct mbuf *mb, void *arg)
{
	(void)mb;
	(void)arg;
	/* Application data over DTLS (e.g. DataChannels) — not used */
}


static void dtls_close_handler(int err, void *arg)
{
	struct intercom_session *sess = arg;
	re_printf("[DTLS] Closed: %m\n", err);
	sess->established = false;
	sess->active = false;
	tmr_cancel(&sess->tmr_audio);
	tmr_cancel(&sess->tmr_video);
}


/*------------------------------------------------------------------------
 * Audio send timer — G.711 µ-law test tone over SRTP
 *----------------------------------------------------------------------*/
static void audio_send_timer(void *arg)
{
	struct intercom_session *sess = arg;
	struct mbuf *mb;
	struct rtp_header hdr;
	uint8_t pcmu[AUDIO_SAMPLES];
	int16_t pcm[AUDIO_SAMPLES];
	static uint32_t phase = 0;
	int err;

	if (!sess->active)
		return;  /* session closed, stop timer */

	if (!sess->established || !sa_isset(&sess->peer_addr, SA_ALL))
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

	/* Build RTP packet: header (12 bytes) + payload + room for SRTP tag */
	mb = mbuf_alloc(12 + AUDIO_SAMPLES + 16);
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
	mbuf_set_pos(mb, 0);

	/* Encrypt with SRTP */
	err = srtp_encrypt(sess->srtp_tx, mb);
	if (err) {
		re_fprintf(stderr, "[SRTP] encrypt: %m\n", err);
		mem_deref(mb);
		goto restart;
	}

	/* Send over UDP */
	mbuf_set_pos(mb, 0);
	udp_send(sess->us, &sess->peer_addr, mb);

	sess->audio_ts += AUDIO_SAMPLES;
	mem_deref(mb);

restart:
	tmr_start(&sess->tmr_audio, AUDIO_PTIME, audio_send_timer, sess);
}


/*------------------------------------------------------------------------
 * Send a single H.264 NAL unit over RTP/SRTP (RFC 6184)
 *
 * Small NALs (≤ RTP_MTU): Single NAL Unit Packet
 *   [RTP header][NAL unit data]
 *
 * Large NALs (> RTP_MTU): FU-A (Fragmentation Unit type A)
 *   [RTP header][FU indicator][FU header][fragment data]
 *   FU indicator: (F|NRI from original NAL) | Type=28
 *   FU header:    S|E|R|Type (original NAL type)
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

		mb = mbuf_alloc(12 + nalu_len + 16);
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

		if (srtp_encrypt(sess->srtp_tx, mb)) {
			mem_deref(mb);
			return;
		}

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

		uint8_t fu_indicator = (nal_header & 0xE0) | 28; /* F|NRI|Type=28 */
		uint8_t nal_type = nal_header & 0x1F;

		while (offset < payload_len) {
			size_t chunk = payload_len - offset;
			/* Reserve 2 bytes for FU indicator + FU header */
			if (chunk > (size_t)(RTP_MTU - 2))
				chunk = (size_t)(RTP_MTU - 2);

			bool last_frag = (offset + chunk >= payload_len);

			struct mbuf *mb;
			struct rtp_header hdr;

			mb = mbuf_alloc(12 + 2 + chunk + 16);
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

			/* FU header: S|E|R|Type (R=0 reserved) */
			uint8_t fu_header = nal_type & 0x1F;
			if (first)
				fu_header |= 0x80;  /* S=1 */
			if (last_frag)
				fu_header |= 0x40;  /* E=1 */
			mbuf_write_u8(mb, fu_header);

			mbuf_write_mem(mb, payload + offset, chunk);
			mbuf_set_pos(mb, 0);

			if (srtp_encrypt(sess->srtp_tx, mb)) {
				mem_deref(mb);
				return;
			}

			mbuf_set_pos(mb, 0);
			udp_send(sess->us, &sess->peer_addr, mb);
			mem_deref(mb);

			offset += chunk;
			first = false;
		}
	}
}


/*------------------------------------------------------------------------
 * Video send timer — H.264 encoded test pattern over SRTP
 *----------------------------------------------------------------------*/
static void video_send_timer(void *arg)
{
	struct intercom_session *sess = arg;
	struct camera_frame frame;
	int err;

	if (!sess->active)
		return;

	if (!sess->established || !sa_isset(&sess->peer_addr, SA_ALL))
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
 * Add remote ICE candidate (ice-lite: not strictly needed)
 *----------------------------------------------------------------------*/
int session_add_remote_candidate(struct intercom_session *sess,
				 const char *cand, size_t len)
{
	re_printf("[SESSION] Remote candidate: %b\n", cand, len);
	/* With ice-lite we don't initiate checks — browser does. */
	return 0;
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
	sess->established = false;
	sess->active = false;
}
