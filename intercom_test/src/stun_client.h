#ifndef STUN_CLIENT_H
#define STUN_CLIENT_H

#include <stdbool.h>

/*
 * STUN client for NAT traversal — discovers public IP:port
 *
 * Sends STUN Binding Request to a public STUN server,
 * receives XOR-MAPPED-ADDRESS to learn our external address.
 */

/* Result callback:
 *   public_addr : "IP:port" string (e.g. "203.0.113.5:8555")
 *   arg         : user callback data
 */
typedef void (stun_result_h)(const char *public_addr, void *arg);

/*
 * Start STUN discovery.
 *   stun_server : STUN server address (e.g. "stun.l.google.com")
 *   stun_port   : STUN server port (usually 19302)
 *   local_port  : our local port to bind (match RTSP port for NAT mapping)
 *   cb          : callback when result is available (or NULL on failure)
 *   arg         : callback argument
 * Returns 0 on success (async), error code on immediate failure.
 */
int stun_discover(const char *stun_server, uint16_t stun_port,
		  uint16_t local_port,
		  stun_result_h *cb, void *arg);

/* Get last discovered public address (NULL if not yet discovered) */
const char *stun_get_public_addr(void);

/* Close STUN resources */
void stun_close(void);

#endif /* STUN_CLIENT_H */
