#ifndef RELAY_CLIENT_H
#define RELAY_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Relay Client — connects to a relay server for NAT traversal.
 *
 * When enabled, the device registers with the relay server using
 * its device_id and auth_token. When a client connects to the relay
 * requesting this device_id, the relay bridges the TCP streams.
 *
 * The relay client receives the BRIDGE command and then hands off
 * the socket to the RTSP server for normal request handling.
 */

/* Callback when relay receives a bridged client connection.
 * The tcp_conn is ready for RTSP communication. */
typedef void (relay_bridge_h)(void *tc, void *arg);

/*
 * Start relay client.
 *   relay_host  : relay server hostname or IP
 *   relay_port  : relay server port (default 9100)
 *   device_id   : unique device identifier
 *   auth_token  : shared authentication token
 *   bridge_cb   : called when a client is bridged through relay
 *   arg         : callback argument
 * Returns 0 on success.
 */
int relay_client_start(const char *relay_host, uint16_t relay_port,
		       const char *device_id, const char *auth_token,
		       relay_bridge_h *bridge_cb, void *arg);

/* Stop relay client and close connection */
void relay_client_stop(void);

/* Check if relay is connected and registered */
bool relay_client_is_connected(void);

/* Get relay server address (for telling clients where to connect) */
const char *relay_client_get_server(void);

#endif /* RELAY_CLIENT_H */
