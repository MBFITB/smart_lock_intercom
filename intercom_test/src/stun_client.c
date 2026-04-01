/*
 * stun_client.c — STUN Binding Request to discover public address
 *
 * Uses libre's STUN module to send a Binding Request to a
 * public STUN server and parse the XOR-MAPPED-ADDRESS response.
 */
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include <re.h>
#include "stun_client.h"

#define STUN_DEFAULT_SERVER "stun.l.google.com"
#define STUN_DEFAULT_PORT   19302

static struct {
	struct stun *stun;
	struct stun_ctrans *ct;
	struct udp_sock *us;

	char public_addr[64];   /* "IP:PORT" result */
	bool discovered;

	stun_result_h *cb;
	void *arg;
} g_stun;


static void stun_resp_handler(int err, uint16_t scode,
			      const char *reason,
			      const struct stun_msg *msg,
			      void *arg)
{
	(void)arg;

	g_stun.ct = NULL;

	if (err) {
		re_fprintf(stderr, "[STUN] Request failed: %m\n", err);
		if (g_stun.cb)
			g_stun.cb(NULL, g_stun.arg);
		return;
	}

	if (scode) {
		re_fprintf(stderr, "[STUN] Error %u %s\n", scode, reason);
		if (g_stun.cb)
			g_stun.cb(NULL, g_stun.arg);
		return;
	}

	/* Extract XOR-MAPPED-ADDRESS */
	const struct stun_attr *attr;

	attr = stun_msg_attr(msg, STUN_ATTR_XOR_MAPPED_ADDR);
	if (!attr) {
		attr = stun_msg_attr(msg, STUN_ATTR_MAPPED_ADDR);
	}

	if (attr) {
		re_snprintf(g_stun.public_addr, sizeof(g_stun.public_addr),
			    "%J", &attr->v.sa);
		g_stun.discovered = true;
		re_printf("[STUN] Public address: %s\n", g_stun.public_addr);

		if (g_stun.cb)
			g_stun.cb(g_stun.public_addr, g_stun.arg);
	} else {
		re_fprintf(stderr, "[STUN] No MAPPED-ADDRESS in response\n");
		if (g_stun.cb)
			g_stun.cb(NULL, g_stun.arg);
	}
}


int stun_discover(const char *stun_server, uint16_t stun_port,
		  uint16_t local_port,
		  stun_result_h *cb, void *arg)
{
	struct sa srv, local;
	int err;

	if (!stun_server)
		stun_server = STUN_DEFAULT_SERVER;
	if (!stun_port)
		stun_port = STUN_DEFAULT_PORT;

	g_stun.cb = cb;
	g_stun.arg = arg;
	g_stun.discovered = false;
	memset(g_stun.public_addr, 0, sizeof(g_stun.public_addr));

	/* Resolve STUN server address */
	err = sa_set_str(&srv, stun_server, stun_port);
	if (err) {
		/* Try DNS resolution */
		struct sa dns_result;
		struct dnsc *dnsc = NULL;

		/* Use simple numeric check first */
		re_fprintf(stderr, "[STUN] Resolving %s...\n", stun_server);

		/* For numeric IPs, sa_set_str works directly.
		 * For hostnames, we need DNS. Use a simpler approach:
		 * resolve synchronously via getaddrinfo. */
		struct addrinfo hints, *res;
		char port_str[8];

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;

		re_snprintf(port_str, sizeof(port_str), "%u", stun_port);

		if (getaddrinfo(stun_server, port_str, &hints, &res) != 0) {
			re_fprintf(stderr, "[STUN] DNS resolution failed\n");
			return ENOENT;
		}

		sa_set_sa(&srv, res->ai_addr);
		freeaddrinfo(res);
	}

	/* Bind local UDP socket */
	sa_set_str(&local, "0.0.0.0", local_port);
	err = udp_listen(&g_stun.us, &local, NULL, NULL);
	if (err) {
		re_fprintf(stderr, "[STUN] UDP bind failed: %m\n", err);
		return err;
	}

	/* Create STUN instance */
	err = stun_alloc(&g_stun.stun, NULL, NULL, NULL);
	if (err) {
		re_fprintf(stderr, "[STUN] stun_alloc failed: %m\n", err);
		g_stun.us = mem_deref(g_stun.us);
		return err;
	}

	/* Send STUN Binding Request */
	err = stun_request(&g_stun.ct, g_stun.stun, IPPROTO_UDP,
			   g_stun.us, &srv, 0,
			   STUN_METHOD_BINDING, NULL, 0, false,
			   stun_resp_handler, NULL, 0);
	if (err) {
		re_fprintf(stderr, "[STUN] Request send failed: %m\n", err);
		g_stun.us = mem_deref(g_stun.us);
		g_stun.stun = mem_deref(g_stun.stun);
		return err;
	}

	re_printf("[STUN] Binding request sent to %s:%u\n",
		  stun_server, stun_port);
	return 0;
}


const char *stun_get_public_addr(void)
{
	if (!g_stun.discovered)
		return NULL;
	return g_stun.public_addr;
}


void stun_close(void)
{
	g_stun.ct = mem_deref(g_stun.ct);
	g_stun.us = mem_deref(g_stun.us);
	g_stun.stun = mem_deref(g_stun.stun);
	g_stun.discovered = false;
}
