/*
 * relay_server.c — TCP Relay / Rendezvous Server
 *
 * Runs on a cloud VPS as a bridge for NAT traversal.
 * Devices register with a device_id; clients connect and get bridged.
 *
 * Protocol (text-based control, then raw TCP tunnel):
 *
 *   Device  → Relay:  REGISTER <device_id> <auth_token>\r\n
 *   Relay   → Device: OK\r\n
 *   (Device keeps connection open, relay holds it for bridging)
 *
 *   Client  → Relay:  CONNECT <device_id>\r\n
 *   Relay   → Client: OK\r\n
 *   (Relay bridges client ↔ device TCP streams bidirectionally)
 *
 *   If device not found: Relay → Client: ERROR device_not_found\r\n
 *
 * Once bridged, raw bytes flow transparently (RTSP+RTP over TCP).
 *
 * Build: gcc -o relay_server relay_server.c -lws2_32 (Windows)
 *        gcc -o relay_server relay_server.c (Linux)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define close closesocket
#define SHUT_RDWR SD_BOTH
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#endif

#define RELAY_PORT       9100
#define MAX_DEVICES      64
#define MAX_LINE         256
#define BRIDGE_BUF_SIZE  16384
#define AUTH_TOKEN        "smartlock2026"   /* shared secret */

struct device_slot {
	bool    in_use;
	bool    bridging;  /* being bridged, don't reuse */
	char    device_id[64];
#ifdef _WIN32
	SOCKET  sock;
#else
	int     sock;
#endif
};

static struct device_slot g_devices[MAX_DEVICES];

#ifdef _WIN32
static CRITICAL_SECTION g_lock;
#define LOCK()   EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif


static int find_device(const char *device_id)
{
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (g_devices[i].in_use && !g_devices[i].bridging &&
		    strcmp(g_devices[i].device_id, device_id) == 0)
			return i;
	}
	return -1;
}


static int alloc_device_slot(void)
{
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (!g_devices[i].in_use)
			return i;
	}
	return -1;
}


static int readline(
#ifdef _WIN32
	SOCKET sock,
#else
	int sock,
#endif
	char *buf, int bufsz)
{
	int pos = 0;
	while (pos < bufsz - 1) {
		char c;
		int n = recv(sock, &c, 1, 0);
		if (n <= 0) return -1;
		if (c == '\n') {
			if (pos > 0 && buf[pos - 1] == '\r')
				pos--;
			break;
		}
		buf[pos++] = c;
	}
	buf[pos] = '\0';
	return pos;
}


/*
 * Bridge two sockets bidirectionally.
 * Runs in a thread; exits when either side closes.
 */
struct bridge_ctx {
#ifdef _WIN32
	SOCKET s1, s2;
#else
	int s1, s2;
#endif
};


#ifdef _WIN32
static DWORD WINAPI bridge_one_dir(LPVOID arg)
#else
static void *bridge_one_dir(void *arg)
#endif
{
	struct bridge_ctx *ctx = (struct bridge_ctx *)arg;
	char buf[BRIDGE_BUF_SIZE];

	for (;;) {
		int n = recv(ctx->s1, buf, sizeof(buf), 0);
		if (n <= 0) break;

		int sent = 0;
		while (sent < n) {
			int w = send(ctx->s2, buf + sent, n - sent, 0);
			if (w <= 0) goto done;
			sent += w;
		}
	}
done:
	shutdown(ctx->s2, SHUT_RDWR);
	free(ctx);
	return 0;
}


static void bridge_sockets(
#ifdef _WIN32
	SOCKET s1, SOCKET s2
#else
	int s1, int s2
#endif
)
{
	struct bridge_ctx *ctx1 = malloc(sizeof(*ctx1));
	struct bridge_ctx *ctx2 = malloc(sizeof(*ctx2));
	if (!ctx1 || !ctx2) {
		free(ctx1);
		free(ctx2);
		return;
	}

	ctx1->s1 = s1;  ctx1->s2 = s2;
	ctx2->s1 = s2;  ctx2->s2 = s1;

#ifdef _WIN32
	HANDLE h1 = CreateThread(NULL, 0, bridge_one_dir, ctx1, 0, NULL);
	HANDLE h2 = CreateThread(NULL, 0, bridge_one_dir, ctx2, 0, NULL);
	if (h1) {
		WaitForSingleObject(h1, INFINITE);
		CloseHandle(h1);
	}
	if (h2) {
		WaitForSingleObject(h2, INFINITE);
		CloseHandle(h2);
	}
#else
	pthread_t t1, t2;
	pthread_create(&t1, NULL, bridge_one_dir, ctx1);
	pthread_create(&t2, NULL, bridge_one_dir, ctx2);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
#endif
}


/*
 * Handle a newly accepted connection.
 * First line determines if this is REGISTER or CONNECT.
 */
#ifdef _WIN32
static DWORD WINAPI handle_connection(LPVOID arg)
#else
static void *handle_connection(void *arg)
#endif
{
#ifdef _WIN32
	SOCKET sock = (SOCKET)(uintptr_t)arg;
#else
	int sock = (int)(intptr_t)arg;
#endif
	char line[MAX_LINE];

	if (readline(sock, line, sizeof(line)) <= 0) {
		close(sock);
		return 0;
	}

	if (strncmp(line, "REGISTER ", 9) == 0) {
		/* REGISTER <device_id> <auth_token> */
		char *device_id = line + 9;
		char *token = strchr(device_id, ' ');
		if (token) {
			*token++ = '\0';
		}

		/* Validate token */
		if (!token || strcmp(token, AUTH_TOKEN) != 0) {
			send(sock, "ERROR auth_failed\r\n", 19, 0);
			close(sock);
			return 0;
		}

		/* Validate device_id (alphanumeric + dash/underscore only) */
		for (char *p = device_id; *p; p++) {
			if (!((*p >= 'a' && *p <= 'z') ||
			      (*p >= 'A' && *p <= 'Z') ||
			      (*p >= '0' && *p <= '9') ||
			      *p == '-' || *p == '_')) {
				send(sock, "ERROR invalid_id\r\n", 18, 0);
				close(sock);
				return 0;
			}
		}

		LOCK();
		/* Remove existing registration for this device */
		int existing = find_device(device_id);
		if (existing >= 0) {
			close(g_devices[existing].sock);
			g_devices[existing].in_use = false;
		}

		int slot = alloc_device_slot();
		if (slot < 0) {
			UNLOCK();
			send(sock, "ERROR server_full\r\n", 19, 0);
			close(sock);
			return 0;
		}

		g_devices[slot].in_use = true;
		strncpy(g_devices[slot].device_id, device_id,
			sizeof(g_devices[slot].device_id) - 1);
		g_devices[slot].device_id[sizeof(g_devices[slot].device_id) - 1] = '\0';
		g_devices[slot].sock = sock;
		UNLOCK();

		send(sock, "OK\r\n", 4, 0);
		printf("[RELAY] Device '%s' registered (slot %d)\n",
		       device_id, slot);

		/* Keep connection alive — block until disconnect.
		 * We detect disconnect by trying to recv. */
		char dummy;
		while (recv(sock, &dummy, 1, MSG_PEEK) > 0) {
#ifdef _WIN32
			Sleep(1000);
#else
			sleep(1);
#endif
		}

		/* Device disconnected — remove from registry */
		LOCK();
		if (g_devices[slot].in_use &&
		    g_devices[slot].sock == sock) {
			g_devices[slot].in_use = false;
			printf("[RELAY] Device '%s' disconnected\n", device_id);
		}
		UNLOCK();
		close(sock);

	} else if (strncmp(line, "CONNECT ", 8) == 0) {
		/* CONNECT <device_id> */
		char *device_id = line + 8;

		/* Validate device_id */
		for (char *p = device_id; *p; p++) {
			if (!((*p >= 'a' && *p <= 'z') ||
			      (*p >= 'A' && *p <= 'Z') ||
			      (*p >= '0' && *p <= '9') ||
			      *p == '-' || *p == '_')) {
				send(sock, "ERROR invalid_id\r\n", 18, 0);
				close(sock);
				return 0;
			}
		}

		LOCK();
		int idx = find_device(device_id);
		if (idx < 0) {
			UNLOCK();
			send(sock, "ERROR device_not_found\r\n", 24, 0);
			close(sock);
			return 0;
		}

		/* Take device socket out of registry for bridging */
#ifdef _WIN32
		SOCKET dev_sock = g_devices[idx].sock;
#else
		int dev_sock = g_devices[idx].sock;
#endif
		g_devices[idx].bridging = true;  /* prevent reuse during bridge */
		UNLOCK();

		/* Notify both sides */
		send(sock, "OK\r\n", 4, 0);
		send(dev_sock, "BRIDGE\r\n", 8, 0);

		printf("[RELAY] Bridging client → device '%s'\n", device_id);

		/* Bridge bidirectionally (blocks until one side closes) */
		bridge_sockets(sock, dev_sock);

		printf("[RELAY] Bridge ended for device '%s'\n", device_id);

		/* Now fully release the slot */
		LOCK();
		g_devices[idx].in_use = false;
		g_devices[idx].bridging = false;
		UNLOCK();

		close(sock);
		close(dev_sock);

	} else {
		send(sock, "ERROR unknown_command\r\n", 22, 0);
		close(sock);
	}

	return 0;
}


int main(int argc, char *argv[])
{
	int port = RELAY_PORT;

	if (argc > 1)
		port = atoi(argv[1]);

#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	InitializeCriticalSection(&g_lock);
#endif

	memset(g_devices, 0, sizeof(g_devices));

#ifdef _WIN32
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
#else
	int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
#endif
	if (listen_sock < 0) {
		perror("socket");
		return 1;
	}

	int opt = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
		   (const char *)&opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((uint16_t)port);

	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(listen_sock);
		return 1;
	}

	if (listen(listen_sock, 16) < 0) {
		perror("listen");
		close(listen_sock);
		return 1;
	}

	printf("[RELAY] Listening on port %d\n", port);
	printf("[RELAY] Auth token: %s\n", AUTH_TOKEN);

	for (;;) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
#ifdef _WIN32
		SOCKET client_sock = accept(listen_sock,
			(struct sockaddr *)&client_addr, &client_len);
		if (client_sock == INVALID_SOCKET) continue;
#else
		int client_sock = accept(listen_sock,
			(struct sockaddr *)&client_addr, &client_len);
		if (client_sock < 0) continue;
#endif

		/* Disable Nagle for low latency */
		int nodelay = 1;
		setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY,
			   (const char *)&nodelay, sizeof(nodelay));

		printf("[RELAY] Connection from %s:%d\n",
		       inet_ntoa(client_addr.sin_addr),
		       ntohs(client_addr.sin_port));

#ifdef _WIN32
		HANDLE h = CreateThread(NULL, 0, handle_connection,
			(LPVOID)(uintptr_t)client_sock, 0, NULL);
		if (h) CloseHandle(h);
#else
		pthread_t t;
		pthread_create(&t, NULL, handle_connection,
			       (void *)(intptr_t)client_sock);
		pthread_detach(t);
#endif
	}

	close(listen_sock);
	return 0;
}
