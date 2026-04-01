#ifndef RTSP_AUTH_H
#define RTSP_AUTH_H

#include <stdbool.h>

/*
 * RTSP Digest Authentication (RFC 2617)
 *
 * Server challenges with 401 + WWW-Authenticate,
 * client responds with Authorization header.
 */

/* Initialize auth module with username and password */
void rtsp_auth_init(const char *username, const char *password);

/* Check if authentication is enabled */
bool rtsp_auth_enabled(void);

/*
 * Verify an Authorization header value.
 *   method : RTSP method (e.g. "DESCRIBE")
 *   uri    : request URI
 *   auth_hdr : value of "Authorization:" header (NULL if absent)
 * Returns 0 if authorized, EACCES if not.
 */
int rtsp_auth_check(const char *method, const char *uri,
		    const char *auth_hdr);

/*
 * Generate a 401 response with WWW-Authenticate header.
 * Writes into buf (max buf_sz bytes).
 * Returns length written, or negative on error.
 */
int rtsp_auth_challenge(char *buf, size_t buf_sz);

#endif /* RTSP_AUTH_H */
