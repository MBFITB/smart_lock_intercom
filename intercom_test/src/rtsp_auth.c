/*
 * rtsp_auth.c — RTSP Digest Authentication (RFC 2617)
 *
 * Implements server-side Digest auth for RTSP.
 * Uses libre's MD5 routines for hash computation.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include <re.h>
#include "rtsp_auth.h"

#define REALM  "SmartLockIntercom"
#define NONCE_LEN  32

static struct {
	char username[64];
	char password[64];
	char nonce[NONCE_LEN + 1];
	bool enabled;
} g_auth;


/* Generate a random hex nonce */
static void generate_nonce(char *buf, size_t len)
{
	static const char hex[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++)
		buf[i] = hex[rand() % 16];
	buf[len] = '\0';
}


void rtsp_auth_init(const char *username, const char *password)
{
	if (!username || !password || !username[0] || !password[0]) {
		g_auth.enabled = false;
		return;
	}

	str_ncpy(g_auth.username, username, sizeof(g_auth.username));
	str_ncpy(g_auth.password, password, sizeof(g_auth.password));
	generate_nonce(g_auth.nonce, NONCE_LEN);
	g_auth.enabled = true;
}


bool rtsp_auth_enabled(void)
{
	return g_auth.enabled;
}


/*
 * Compute MD5 hex digest: md5(data) → 32-char hex string
 */
static void md5_hex(const char *data, size_t len, char *out)
{
	uint8_t digest[MD5_SIZE];

	md5((const uint8_t *)data, len, digest);

	for (int i = 0; i < MD5_SIZE; i++)
		sprintf(out + i * 2, "%02x", digest[i]);
	out[MD5_STR_SIZE - 1] = '\0';
}


/*
 * Parse a specific field from Digest authorization header.
 * e.g. extract "username" from:  Digest username="admin", realm="...", ...
 */
static int parse_digest_field(const char *hdr, const char *field,
			      char *out, size_t out_sz)
{
	char pattern[64];
	const char *p, *start, *end;

	re_snprintf(pattern, sizeof(pattern), "%s=\"", field);
	p = strstr(hdr, pattern);
	if (!p) {
		/* Try without quotes */
		re_snprintf(pattern, sizeof(pattern), "%s=", field);
		p = strstr(hdr, pattern);
		if (!p) return ENOENT;
		start = p + strlen(pattern);
		end = start;
		while (*end && *end != ',' && *end != ' ' && *end != '\r')
			end++;
	} else {
		start = p + strlen(pattern);
		end = strchr(start, '"');
		if (!end) return EINVAL;
	}

	size_t vlen = (size_t)(end - start);
	if (vlen >= out_sz) vlen = out_sz - 1;
	memcpy(out, start, vlen);
	out[vlen] = '\0';
	return 0;
}


int rtsp_auth_check(const char *method, const char *uri,
		    const char *auth_hdr)
{
	char username[64], realm[64], nonce[64], response[64], req_uri[256];
	char ha1_str[256], ha2_str[256], resp_str[512];
	char ha1[MD5_STR_SIZE], ha2[MD5_STR_SIZE], expected[MD5_STR_SIZE];

	if (!g_auth.enabled)
		return 0;  /* auth disabled, always pass */

	if (!auth_hdr || strncasecmp(auth_hdr, "Digest ", 7) != 0)
		return EACCES;

	/* Parse fields from Authorization header */
	if (parse_digest_field(auth_hdr, "username", username, sizeof(username)))
		return EACCES;
	if (parse_digest_field(auth_hdr, "realm", realm, sizeof(realm)))
		return EACCES;
	if (parse_digest_field(auth_hdr, "nonce", nonce, sizeof(nonce)))
		return EACCES;
	if (parse_digest_field(auth_hdr, "response", response, sizeof(response)))
		return EACCES;
	if (parse_digest_field(auth_hdr, "uri", req_uri, sizeof(req_uri)))
		return EACCES;

	/* Verify username */
	if (strcmp(username, g_auth.username) != 0)
		return EACCES;

	/* Verify nonce (prevents replay with old nonces) */
	if (strcmp(nonce, g_auth.nonce) != 0)
		return EACCES;

	/* HA1 = MD5(username:realm:password) */
	re_snprintf(ha1_str, sizeof(ha1_str), "%s:%s:%s",
		    g_auth.username, realm, g_auth.password);
	md5_hex(ha1_str, strlen(ha1_str), ha1);

	/* HA2 = MD5(method:uri) */
	re_snprintf(ha2_str, sizeof(ha2_str), "%s:%s", method, req_uri);
	md5_hex(ha2_str, strlen(ha2_str), ha2);

	/* expected = MD5(HA1:nonce:HA2) */
	re_snprintf(resp_str, sizeof(resp_str), "%s:%s:%s", ha1, nonce, ha2);
	md5_hex(resp_str, strlen(resp_str), expected);

	/* Constant-time comparison to prevent timing attacks */
	int cmp = 0;
	for (int i = 0; i < MD5_STR_SIZE - 1; i++)
		cmp |= response[i] ^ expected[i];
	if (cmp != 0)
		return EACCES;

	/* Rotate nonce after successful authentication for replay protection */
	generate_nonce(g_auth.nonce, NONCE_LEN);

	return 0;
}


int rtsp_auth_challenge(char *buf, size_t buf_sz)
{
	/* Nonce is generated once at init and kept stable.
	 * Only rotate after a successful auth or on explicit reset.
	 * This ensures the client can compute a valid response
	 * using the nonce received in the 401 challenge. */

	return re_snprintf(buf, buf_sz,
			   "WWW-Authenticate: Digest realm=\"%s\", "
			   "nonce=\"%s\"\r\n",
			   REALM, g_auth.nonce);
}
