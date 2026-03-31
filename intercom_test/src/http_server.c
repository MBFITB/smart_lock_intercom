/*
 * http_server.c — HTTP signaling server + static file server
 *
 * Serves:
 *   GET  /            → www/index.html (control page)
 *   GET  /js/*        → static JS files
 *   POST /api/call    → create RTP session, return port info
 *   DELETE /api/call  → hang up
 *   GET  /api/status  → session status
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include "intercom.h"


static struct http_sock *g_httpsock;
static struct intercom_session *g_sess;
static char g_www_path[256];


/* Content-type lookup */
static const char *mime_type(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot)
		return "application/octet-stream";
	if (strcasecmp(dot, ".html") == 0) return "text/html; charset=utf-8";
	if (strcasecmp(dot, ".js")   == 0) return "application/javascript; charset=utf-8";
	if (strcasecmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
	if (strcasecmp(dot, ".json") == 0) return "application/json";
	if (strcasecmp(dot, ".ico")  == 0) return "image/x-icon";
	if (strcasecmp(dot, ".png")  == 0) return "image/png";
	return "application/octet-stream";
}


/* Serve a static file from www/ directory */
static int serve_file(struct http_conn *conn, const char *filename)
{
	char filepath[512];
	FILE *fp;
	uint8_t *buf;
	long fsize;
	int err;

	/* Prevent path traversal */
	if (strstr(filename, "..") || strstr(filename, "\\") ||
	    strchr(filename, '\0') != filename + strlen(filename)) {
		http_ereply(conn, 403, "Forbidden");
		return EPERM;
	}

	if (strlen(g_www_path) + strlen(filename) + 2 >= sizeof(filepath)) {
		http_ereply(conn, 414, "URI Too Long");
		return EINVAL;
	}

	re_snprintf(filepath, sizeof(filepath), "%s/%s",
		    g_www_path, filename);

	fp = fopen(filepath, "rb");
	if (!fp) {
		http_ereply(conn, 404, "Not Found");
		return ENOENT;
	}

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (fsize <= 0 || fsize > 1024 * 1024) {
		fclose(fp);
		http_ereply(conn, 500, "Internal Error");
		return EINVAL;
	}

	buf = mem_alloc((size_t)fsize, NULL);
	if (!buf) {
		fclose(fp);
		http_ereply(conn, 500, "Internal Error");
		return ENOMEM;
	}

	if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
		fclose(fp);
		mem_deref(buf);
		http_ereply(conn, 500, "Read Error");
		return EIO;
	}
	fclose(fp);

	err = http_reply(conn, 200, "OK",
			 "Content-Type: %s\r\n"
			 "Content-Length: %ld\r\n"
			 "Access-Control-Allow-Origin: *\r\n"
			 "Connection: close\r\n"
			 "\r\n"
			 "%b",
			 mime_type(filename),
			 fsize,
			 buf, (size_t)fsize);

	mem_deref(buf);
	return err;
}


/* Handle POST /api/call — create RTP session to specified peer */
static void handle_call_create(struct http_conn *conn,
			       const struct http_msg *msg)
{
	struct sa peer;
	char peer_str[64] = "";
	int err;

	/* Tear down any existing session */
	if (g_sess) {
		session_close(g_sess);
		g_sess = mem_deref(g_sess);
	}

	/*
	 * Parse peer address from JSON body: {"peer":"IP:port"}
	 * If no body or no peer, use a default for testing.
	 */
	if (msg->mb && mbuf_get_left(msg->mb) > 0) {
		const char *body = (const char *)mbuf_buf(msg->mb);
		size_t body_len = mbuf_get_left(msg->mb);

		/* Simple JSON extraction: find "peer":"..." */
		const char *key = "\"peer\"";
		const char *p = strstr(body, key);
		if (p) {
			p += strlen(key);
			/* skip whitespace and colon */
			while (p < body + body_len &&
			       (*p == ' ' || *p == ':' || *p == '"'))
				p++;
			const char *end = strchr(p, '"');
			if (end && (size_t)(end - p) < sizeof(peer_str)) {
				memcpy(peer_str, p, (size_t)(end - p));
				peer_str[end - p] = '\0';
			}
		}
	}

	/* Default peer for testing: localhost:5006 */
	if (peer_str[0] == '\0')
		str_ncpy(peer_str, "127.0.0.1:5006", sizeof(peer_str));

	err = sa_decode(&peer, peer_str, strlen(peer_str));
	if (err) {
		http_creply(conn, 400, "Bad Request",
			    "application/json",
			    "{\"error\":\"invalid peer address\"}");
		return;
	}

	err = session_alloc(&g_sess, &peer);
	if (err) {
		http_ereply(conn, 500, "Session alloc failed");
		return;
	}

	/* Return session info as JSON */
	http_reply(conn, 201, "Created",
		   "Content-Type: application/json\r\n"
		   "Access-Control-Allow-Origin: *\r\n"
		   "Connection: close\r\n"
		   "\r\n"
		   "{\"status\":\"ok\","
		   "\"rtp_port\":%u,"
		   "\"peer\":\"%s\"}",
		   g_sess->local_port,
		   peer_str);
}


/* Handle GET /api/status — session info */
static void handle_status(struct http_conn *conn)
{
	if (g_sess && g_sess->active) {
		http_reply(conn, 200, "OK",
			   "Content-Type: application/json\r\n"
			   "Access-Control-Allow-Origin: *\r\n"
			   "Connection: close\r\n"
			   "\r\n"
			   "{\"active\":true,"
			   "\"rtp_port\":%u,"
			   "\"peer\":\"%J\"}",
			   g_sess->local_port,
			   &g_sess->peer_addr);
	}
	else {
		http_reply(conn, 200, "OK",
			   "Content-Type: application/json\r\n"
			   "Access-Control-Allow-Origin: *\r\n"
			   "Connection: close\r\n"
			   "\r\n"
			   "{\"active\":false}");
	}
}


/* Handle DELETE /api/call — hang up */
static void handle_call_delete(struct http_conn *conn,
			       const struct http_msg *msg)
{
	(void)msg;

	if (g_sess) {
		session_close(g_sess);
		g_sess = mem_deref(g_sess);
	}

	http_reply(conn, 200, "OK",
		   "Content-Type: application/json\r\n"
		   "Access-Control-Allow-Origin: *\r\n"
		   "Connection: close\r\n"
		   "\r\n"
		   "{\"status\":\"ok\"}");
}


/* Handle CORS preflight */
static void handle_options(struct http_conn *conn)
{
	http_reply(conn, 204, "No Content",
		   "Access-Control-Allow-Origin: *\r\n"
		   "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
		   "Access-Control-Allow-Headers: Content-Type\r\n"
		   "Access-Control-Max-Age: 86400\r\n"
		   "Connection: close\r\n"
		   "\r\n");
}


/* Main HTTP request handler */
static void http_req_handler(struct http_conn *conn,
			     const struct http_msg *msg, void *arg)
{
	(void)arg;

	re_printf("[HTTP] %r %r\n", &msg->met, &msg->path);

	/* CORS preflight */
	if (0 == pl_strcasecmp(&msg->met, "OPTIONS")) {
		handle_options(conn);
	}
	/* API endpoints — check before static files */
	else if (0 == pl_strcasecmp(&msg->met, "POST") &&
	         0 == pl_strcmp(&msg->path, "/api/call")) {
		handle_call_create(conn, msg);
	}
	else if (0 == pl_strcasecmp(&msg->met, "GET") &&
	         0 == pl_strcmp(&msg->path, "/api/status")) {
		handle_status(conn);
	}
	else if (0 == pl_strcasecmp(&msg->met, "DELETE") &&
	         0 == pl_strcmp(&msg->path, "/api/call")) {
		handle_call_delete(conn, msg);
	}
	/* Static files */
	else if (0 == pl_strcasecmp(&msg->met, "GET")) {

		if (0 == pl_strcmp(&msg->path, "/") ||
		    0 == pl_strcmp(&msg->path, "/index.html")) {
			serve_file(conn, "index.html");
		}
		else if (msg->path.l > 1) {
			char pathbuf[256];
			size_t plen = msg->path.l - 1;
			if (plen >= sizeof(pathbuf))
				plen = sizeof(pathbuf) - 1;
			memcpy(pathbuf, msg->path.p + 1, plen);
			pathbuf[plen] = '\0';
			serve_file(conn, pathbuf);
		}
		else {
			http_reply(conn, 404, "Not Found",
				   "Connection: close\r\n"
				   "\r\n");
		}
	}
	else {
		http_reply(conn, 404, "Not Found",
			   "Connection: close\r\n"
			   "\r\n");
	}

	/*
	 * Close TCP connection immediately after sending the response.
	 * libre's http_reply() does NOT close the connection (600s idle
	 * keep-alive). If we leave it open, the browser will RST it on
	 * WebRTC hangup, causing TCP 10054 errors that can break the
	 * event loop after several Call/Hangup cycles.
	 */
	http_conn_close(conn);
}


int http_sig_init(struct http_sock **sockp, const struct sa *laddr,
		  const char *www_path)
{
	int err;

	str_ncpy(g_www_path, www_path, sizeof(g_www_path));

	err = http_listen(&g_httpsock, laddr, http_req_handler, NULL);
	if (err) {
		re_fprintf(stderr, "http_listen error: %m\n", err);
		return err;
	}

	if (sockp)
		*sockp = g_httpsock;

	re_printf("HTTP server listening on %j:%d\n", laddr, sa_port(laddr));

	return 0;
}


void http_sig_close(void)
{
	g_httpsock = mem_deref(g_httpsock);
}


void http_sig_set_session(struct intercom_session *sess)
{
	g_sess = sess;
}
