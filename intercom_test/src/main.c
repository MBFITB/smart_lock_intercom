/*
 * main.c — Intercom test entry point
 *
 * Initializes libre, starts HTTP signaling server,
 * and runs the event loop.
 */
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif
#include <re.h>
#include <rem.h>
#include "intercom.h"
#include "rtsp_server.h"
#include "rtsp_auth.h"
#include "stun_client.h"
#include "relay_client.h"


static struct http_sock *httpsock;
static struct intercom_session *g_session;


static void signal_handler(int sig)
{
	(void)sig;
	re_fprintf(stderr, "\nShutting down...\n");
	re_cancel();
}


/* Resolve www path relative to the executable's parent's parent dir */
static void resolve_www_path(char *buf, size_t sz)
{
	char exepath[512];
	char *sep;

#ifdef _WIN32
	GetModuleFileNameA(NULL, exepath, sizeof(exepath));
#else
	ssize_t len = readlink("/proc/self/exe", exepath, sizeof(exepath) - 1);
	if (len > 0) exepath[len] = '\0';
	else str_ncpy(exepath, ".", sizeof(exepath));
#endif
	/* exepath = .../intercom_test/build/intercom_test.exe
	   We want .../intercom_test/www */
	sep = strrchr(exepath, '\\');
	if (!sep) sep = strrchr(exepath, '/');
	if (sep) *sep = '\0'; /* now: .../intercom_test/build */

	sep = strrchr(exepath, '\\');
	if (!sep) sep = strrchr(exepath, '/');
	if (sep) *sep = '\0'; /* now: .../intercom_test */

	re_snprintf(buf, sz, "%s/www", exepath);
}


int main(int argc, char *argv[])
{
	struct sa laddr;
	char www_path[512];
	int err;

	/* Configurable via command-line args */
	const char *auth_user = NULL;
	const char *auth_pass = NULL;
	const char *relay_host = NULL;
	uint16_t relay_port = 9100;
	const char *device_id = NULL;
	const char *relay_token = "smartlock2026";
	const char *stun_server = NULL;

	/* Parse arguments: --user X --pass X --relay H --relay-port P
	 *                  --device-id X --relay-token X --stun S */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--user") == 0 && i + 1 < argc)
			auth_user = argv[++i];
		else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc)
			auth_pass = argv[++i];
		else if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc)
			relay_host = argv[++i];
		else if (strcmp(argv[i], "--relay-port") == 0 && i + 1 < argc)
			relay_port = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--device-id") == 0 && i + 1 < argc)
			device_id = argv[++i];
		else if (strcmp(argv[i], "--relay-token") == 0 && i + 1 < argc)
			relay_token = argv[++i];
		else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc)
			stun_server = argv[++i];
	}

	/* Initialize libre */
	err = libre_init();
	if (err) {
		re_fprintf(stderr, "libre_init failed: %m\n", err);
		return err;
	}

	re_printf("libre %s initialized\n", sys_libre_version_get());

	/* Resolve www dir relative to exe location */
	resolve_www_path(www_path, sizeof(www_path));
	re_printf("Serving static files from: %s\n", www_path);

	/* Bind HTTP server to 0.0.0.0:9000 */
	sa_set_str(&laddr, "0.0.0.0", 9000);

	err = http_sig_init(&httpsock, &laddr, www_path);
	if (err) {
		re_fprintf(stderr, "HTTP server init failed: %m\n", err);
		goto out;
	}

	/* Start RTSP server on port 8555 */
	{
		struct sa rtsp_addr;
		sa_set_str(&rtsp_addr, "0.0.0.0", 8555);
		err = rtsp_server_init(&rtsp_addr);
		if (err) {
			re_fprintf(stderr, "RTSP server init failed: %m\n", err);
			goto out;
		}
	}

	/* Print local addresses */
	{
		struct sa addr;

		if (0 == net_default_source_addr_get(AF_INET, &addr)) {
			re_printf("\n  http://%j:9000/\n", &addr);
			re_printf("  rtsp://%j:8555/live\n", &addr);
		}
		else {
			re_printf("\n  http://0.0.0.0:9000/\n");
			re_printf("  rtsp://0.0.0.0:8555/live\n");
		}
	}

	/* Initialize RTSP Digest Authentication (optional) */
	if (auth_user && auth_pass) {
		rtsp_auth_init(auth_user, auth_pass);
		re_printf("  RTSP auth: enabled (user=%s)\n", auth_user);
	} else {
		re_printf("  RTSP auth: disabled "
			  "(use --user/--pass to enable)\n");
	}

	/* STUN discovery (optional — discover public address) */
	if (stun_server) {
		err = stun_discover(stun_server, 19302, 8555, NULL, NULL);
		if (err)
			re_fprintf(stderr, "STUN discovery failed: %m\n", err);
		else
			re_printf("  STUN: querying %s...\n", stun_server);
	}

	/* Relay client (optional — register with relay for NAT traversal) */
	if (relay_host && device_id) {
		err = relay_client_start(relay_host, relay_port,
					device_id, relay_token,
					rtsp_accept_relay_conn, NULL);
		if (err)
			re_fprintf(stderr, "Relay client failed: %m\n", err);
		else
			re_printf("  Relay: connecting to %s:%u "
				  "(device=%s)\n",
				  relay_host, relay_port, device_id);
	}

	re_printf("\nPress Ctrl+C to quit.\n\n");

	/* Run event loop */
	err = re_main(signal_handler);

out:
	/* Cleanup */
	if (g_session) {
		session_close(g_session);
		mem_deref(g_session);
	}
	relay_client_stop();
	stun_close();
	rtsp_server_close();
	http_sig_close();
	mem_deref(httpsock);
	libre_close();

	return err;
}
