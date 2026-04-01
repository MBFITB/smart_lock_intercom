/*
 * relay_client.c — Device-side relay client for NAT traversal
 *
 * Connects to the relay server, sends REGISTER, and waits for
 * BRIDGE commands. When bridged, creates a libre tcp_conn wrapper
 * so the RTSP server can handle it normally.
 *
 * Auto-reconnects if connection drops.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <re.h>
#include "relay_client.h"

#define RECONNECT_INTERVAL_MS  5000
#define MAX_RECONNECT_MS       300000  /* 5 min max backoff */
#define MAX_LINE               256
#define MAX_RBUF_SIZE          8192    /* prevent unbounded buffer growth */

static struct {
	struct tcp_conn *tc;       /* TCP connection to relay */
	struct tmr tmr_reconnect;  /* reconnect timer */
	struct mbuf *rbuf;         /* receive buffer */

	char relay_host[128];
	uint16_t relay_port;
	char device_id[64];
	char auth_token[64];
	char relay_addr[256];      /* "host:port" for clients */

	relay_bridge_h *bridge_cb;
	void *bridge_arg;

	bool registered;
	bool running;
	bool bridging;             /* bridge in progress */
	uint32_t backoff_count;    /* exponential backoff counter */
} g_relay;


/* Forward declarations */
static void relay_reconnect(void *arg);
static void relay_do_connect(void);

static uint32_t relay_backoff_delay(void)
{
	uint32_t delay = RECONNECT_INTERVAL_MS
		<< (g_relay.backoff_count < 6
		    ? g_relay.backoff_count : 6);
	if (delay > MAX_RECONNECT_MS)
		delay = MAX_RECONNECT_MS;
	g_relay.backoff_count++;
	return delay;
}

static void relay_schedule_reconnect(void)
{
	if (!g_relay.running)
		return;
	uint32_t delay = relay_backoff_delay();
	re_printf("[RELAY-CLIENT] Reconnecting in %ums\n", delay);
	tmr_start(&g_relay.tmr_reconnect, delay,
		  relay_reconnect, NULL);
}


static void relay_close_handler(int err, void *arg)
{
	(void)arg;

	if (err)
		re_printf("[RELAY-CLIENT] Connection closed: %m\n", err);
	else
		re_printf("[RELAY-CLIENT] Connection closed by server\n");

	g_relay.tc = mem_deref(g_relay.tc);
	g_relay.rbuf = mem_deref(g_relay.rbuf);
	g_relay.registered = false;

	if (g_relay.running && !g_relay.bridging) {
		relay_schedule_reconnect();
	}
}


static void relay_recv_handler(struct mbuf *mb, void *arg)
{
	(void)arg;
	const char *data;
	size_t len;

	if (!g_relay.rbuf)
		g_relay.rbuf = mbuf_alloc(1024);
	if (!g_relay.rbuf) return;

	/* Prevent unbounded buffer growth (DoS protection) */
	if (g_relay.rbuf->end + mbuf_get_left(mb) > MAX_RBUF_SIZE) {
		re_fprintf(stderr,
			   "[RELAY-CLIENT] Buffer overflow, dropping\n");
		relay_close_handler(EOVERFLOW, NULL);
		return;
	}

	/* Append data */
	mbuf_set_pos(g_relay.rbuf, g_relay.rbuf->end);
	mbuf_write_mem(g_relay.rbuf, mbuf_buf(mb), mbuf_get_left(mb));

	/* Check for complete lines */
	data = (const char *)g_relay.rbuf->buf;
	len = g_relay.rbuf->end;

	const char *nl = memchr(data, '\n', len);
	while (nl) {
		size_t line_len = (size_t)(nl - data);

		/* Remove trailing \r */
		size_t actual = line_len;
		if (actual > 0 && data[actual - 1] == '\r')
			actual--;

		char line[MAX_LINE];
		if (actual >= sizeof(line))
			actual = sizeof(line) - 1;
		memcpy(line, data, actual);
		line[actual] = '\0';

		/* Process the line */
		if (strcmp(line, "OK") == 0 && !g_relay.registered) {
			g_relay.registered = true;
			re_printf("[RELAY-CLIENT] Registered as '%s'\n",
				  g_relay.device_id);
		}
		else if (strcmp(line, "BRIDGE") == 0) {
			re_printf("[RELAY-CLIENT] Bridge request received!\n");
			g_relay.bridging = true;

			/* Hand off tcp_conn to RTSP server.
			 * The RTSP server will take ownership. */
			struct tcp_conn *tc = g_relay.tc;
			g_relay.tc = NULL;
			g_relay.rbuf = mem_deref(g_relay.rbuf);
			g_relay.registered = false;

			if (g_relay.bridge_cb)
				g_relay.bridge_cb(tc, g_relay.bridge_arg);

			/* Reconnect for next client */
			g_relay.bridging = false;
			g_relay.backoff_count = 0;  /* reset after bridge */
			if (g_relay.running) {
				tmr_start(&g_relay.tmr_reconnect,
					  RECONNECT_INTERVAL_MS,
					  relay_reconnect, NULL);
			}

			return;  /* buffer consumed by bridge */
		}
		else if (strncmp(line, "ERROR", 5) == 0) {
			re_fprintf(stderr, "[RELAY-CLIENT] Server error: %s\n",
				   line);
		}

		/* Consume processed data */
		size_t consumed = line_len + 1;  /* include \n */
		memmove(g_relay.rbuf->buf,
			g_relay.rbuf->buf + consumed,
			g_relay.rbuf->end - consumed);
		g_relay.rbuf->end -= consumed;

		/* Look for next line */
		data = (const char *)g_relay.rbuf->buf;
		len = g_relay.rbuf->end;
		nl = memchr(data, '\n', len);
	}
}


static void relay_estab_handler(void *arg)
{
	(void)arg;
	char reg_msg[MAX_LINE];

	re_printf("[RELAY-CLIENT] Connected to relay %s:%u\n",
		  g_relay.relay_host, g_relay.relay_port);

	/* Reset backoff on successful connection */
	g_relay.backoff_count = 0;

	/* Send REGISTER command */
	re_snprintf(reg_msg, sizeof(reg_msg),
		    "REGISTER %s %s\r\n",
		    g_relay.device_id, g_relay.auth_token);

	struct mbuf *mb = mbuf_alloc(strlen(reg_msg));
	if (mb) {
		mbuf_write_mem(mb, (const uint8_t *)reg_msg,
			       strlen(reg_msg));
		mbuf_set_pos(mb, 0);
		tcp_send(g_relay.tc, mb);
		mem_deref(mb);
	}
}


static void relay_do_connect(void)
{
	struct sa srv;
	int err;

	/* Resolve relay server address */
	struct addrinfo hints, *res;
	char port_str[8];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	re_snprintf(port_str, sizeof(port_str), "%u", g_relay.relay_port);

	if (getaddrinfo(g_relay.relay_host, port_str, &hints, &res) != 0) {
		re_fprintf(stderr, "[RELAY-CLIENT] DNS resolution failed\n");
		relay_schedule_reconnect();
		return;
	}

	sa_set_sa(&srv, res->ai_addr);
	freeaddrinfo(res);

	err = tcp_connect(&g_relay.tc, &srv,
			  relay_estab_handler,
			  relay_recv_handler,
			  relay_close_handler,
			  NULL);
	if (err) {
		re_fprintf(stderr,
			   "[RELAY-CLIENT] tcp_connect failed: %m\n", err);
		relay_schedule_reconnect();
	}
}


static void relay_reconnect(void *arg)
{
	(void)arg;

	if (!g_relay.running)
		return;

	g_relay.tc = mem_deref(g_relay.tc);
	g_relay.rbuf = mem_deref(g_relay.rbuf);
	g_relay.registered = false;

	relay_do_connect();
}


int relay_client_start(const char *relay_host, uint16_t relay_port,
		       const char *device_id, const char *auth_token,
		       relay_bridge_h *bridge_cb, void *arg)
{
	if (!relay_host || !device_id || !auth_token)
		return EINVAL;

	str_ncpy(g_relay.relay_host, relay_host,
		 sizeof(g_relay.relay_host));
	g_relay.relay_port = relay_port ? relay_port : 9100;
	str_ncpy(g_relay.device_id, device_id,
		 sizeof(g_relay.device_id));
	str_ncpy(g_relay.auth_token, auth_token,
		 sizeof(g_relay.auth_token));

	re_snprintf(g_relay.relay_addr, sizeof(g_relay.relay_addr),
		    "%s:%u", relay_host, g_relay.relay_port);

	g_relay.bridge_cb = bridge_cb;
	g_relay.bridge_arg = arg;
	g_relay.running = true;
	g_relay.bridging = false;

	tmr_init(&g_relay.tmr_reconnect);

	relay_do_connect();

	re_printf("[RELAY-CLIENT] Started (device_id=%s, relay=%s:%u)\n",
		  device_id, relay_host, g_relay.relay_port);
	return 0;
}


void relay_client_stop(void)
{
	g_relay.running = false;
	tmr_cancel(&g_relay.tmr_reconnect);
	g_relay.tc = mem_deref(g_relay.tc);
	g_relay.rbuf = mem_deref(g_relay.rbuf);
	g_relay.registered = false;
}


bool relay_client_is_connected(void)
{
	return g_relay.registered;
}


const char *relay_client_get_server(void)
{
	if (!g_relay.running)
		return NULL;
	return g_relay.relay_addr;
}
