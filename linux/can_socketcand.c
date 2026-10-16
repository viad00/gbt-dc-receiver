/*
 * can_socketcand.c
 *
 * Linux transport for the shared CAN interface (can.h) over the socketcand
 * TCP protocol, so the GB/T 27930 logic can reach a CAN bus through the
 * network instead of a local SocketCAN interface.
 *
 * Protocol (https://github.com/linux-can/socketcand doc/protocol.md):
 * ASCII commands and replies framed with '<' and '>'.  After connecting the
 * server greets with "< hi >", the client selects a bus ("< open can0 >"),
 * switches to RAW mode ("< rawmode >") and then exchanges frames as
 *   TX: "< send 1826F456 2 5b 0f >"        (8 hex digits = extended id)
 *   RX: "< frame 1826F456 23.424242 5B0F >"
 * LICENSE: MIT
 */

#include "can.h"
#include "can_backends.h"
#include "helpers.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_DEFAULT_HOST   "127.0.0.1"
#define SC_DEFAULT_PORT   29536
#define SC_REPLY_TIMEOUT_MS 3000u
#define SC_RECONNECT_MS     2000u
#define SC_IN_BUF         4096
#define SC_MSG_MAX        512
#define SC_CMD_MAX        160

static int sc_fd = -1;
static char sc_host[256] = SC_DEFAULT_HOST;
static int sc_port = SC_DEFAULT_PORT;
static char sc_bus[24] = "can0";
static uint32_t sc_last_connect_try = 0;

/* raw byte stream from the server; complete '<'...'>' messages are cut out */
static char sc_in[SC_IN_BUF];
static size_t sc_in_len = 0;

void CanSocketcand_SetServer(const char *host, int port)
{
	if (host != NULL && host[0] != '\0') {
		strncpy(sc_host, host, sizeof(sc_host) - 1);
		sc_host[sizeof(sc_host) - 1] = '\0';
	}
	if (port > 0 && port <= 65535) {
		sc_port = port;
	}
}

void CanSocketcand_SetBus(const char *bus)
{
	if (bus != NULL && bus[0] != '\0') {
		strncpy(sc_bus, bus, sizeof(sc_bus) - 1);
		sc_bus[sizeof(sc_bus) - 1] = '\0';
	}
}

static void sc_disconnect(const char *why)
{
	if (sc_fd >= 0) {
		close(sc_fd);
		sc_fd = -1;
	}
	sc_in_len = 0;
	if (why != NULL) {
		fprintf(stderr, "gbt-linux: socketcand connection closed (%s)\n", why);
	}
}

static bool sc_send_all(const char *s)
{
	size_t len = strlen(s);
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = write(sc_fd, s + sent, len - sent);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

static void sc_feed(const char *data, size_t n)
{
	if (sc_in_len + n > sizeof(sc_in)) {
		sc_in_len = 0; /* protocol garbage or oversized message - resync */
		return;
	}
	memcpy(sc_in + sc_in_len, data, n);
	sc_in_len += n;
}

/* Cut the next complete "< ... >" message out of the input buffer. */
static bool sc_take_message(char *out, size_t outlen)
{
	size_t start = 0;

	while (start < sc_in_len && sc_in[start] != '<') {
		start++;
	}
	if (start == sc_in_len) {
		sc_in_len = 0;
		return false;
	}
	if (start > 0) {
		memmove(sc_in, sc_in + start, sc_in_len - start);
		sc_in_len -= start;
	}

	size_t end = 0;
	while (end < sc_in_len && sc_in[end] != '>') {
		end++;
	}
	if (end == sc_in_len) {
		if (sc_in_len == sizeof(sc_in)) {
			sc_in_len = 0; /* oversized message without terminator */
		}
		return false;
	}

	size_t n = (end + 1 < outlen) ? end + 1 : outlen - 1;
	memcpy(out, sc_in, n);
	out[n] = '\0';
	memmove(sc_in, sc_in + end + 1, sc_in_len - end - 1);
	sc_in_len -= end + 1;
	return true;
}

/* Blocking-with-deadline wait for "< ok >"; any error reply text is copied to
 * out.  "< hi >" and stray frames are skipped. */
static bool sc_wait_reply(char *out, size_t outlen)
{
	uint32_t deadline = GetTS() + SC_REPLY_TIMEOUT_MS;
	char msg[SC_MSG_MAX];

	if (out != NULL && outlen > 0) {
		out[0] = '\0';
	}

	for (;;) {
		while (sc_take_message(msg, sizeof(msg))) {
			if (strcmp(msg, "< ok >") == 0) {
				return true;
			}
			if (strncmp(msg, "< error", 7) == 0) {
				if (out != NULL) {
					strncpy(out, msg, outlen - 1);
					out[outlen - 1] = '\0';
				}
				return false;
			}
			/* "< hi >" or anything else: keep waiting */
		}

		if ((int32_t)(GetTS() - deadline) >= 0) {
			if (out != NULL) {
				snprintf(out, outlen, "no reply (timeout)");
			}
			return false;
		}

		struct pollfd pfd;
		pfd.fd = sc_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
			char tmp[512];
			ssize_t n = recv(sc_fd, tmp, sizeof(tmp), 0);
			if (n > 0) {
				sc_feed(tmp, (size_t)n);
			} else if (n == 0) {
				if (out != NULL) {
					snprintf(out, outlen, "connection closed");
				}
				return false;
			} else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
				if (out != NULL) {
					snprintf(out, outlen, "%s", strerror(errno));
				}
				return false;
			}
		}
	}
}

static bool sc_tcp_connect(void)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL, *rp;
	char portstr[16];
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof(portstr), "%d", sc_port);

	if (getaddrinfo(sc_host, portstr, &hints, &result) != 0) {
		fprintf(stderr, "gbt-linux: socketcand host '%s' not resolvable\n", sc_host);
		return false;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0) {
			continue;
		}
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(result);

	if (fd < 0) {
		return false;
	}
	sc_fd = fd;
	sc_in_len = 0;
	return true;
}

static bool sc_connect(void)
{
	char cmd[SC_CMD_MAX];
	char reply[128];

	if (sc_fd >= 0) {
		return true;
	}
	if (!sc_tcp_connect()) {
		fprintf(stderr, "gbt-linux: cannot reach socketcand at %s:%d\n", sc_host, sc_port);
		return false;
	}

	snprintf(cmd, sizeof(cmd), "< open %s >", sc_bus);
	if (!sc_send_all(cmd) || !sc_wait_reply(reply, sizeof(reply))) {
		fprintf(stderr, "gbt-linux: socketcand open '%s' failed: %s\n", sc_bus,
		        reply[0] ? reply : "send error");
		sc_disconnect(NULL);
		return false;
	}

	if (!sc_send_all("< rawmode >") || !sc_wait_reply(reply, sizeof(reply))) {
		fprintf(stderr, "gbt-linux: socketcand rawmode failed: %s\n",
		        reply[0] ? reply : "send error");
		sc_disconnect(NULL);
		return false;
	}

	fprintf(stderr, "gbt-linux: socketcand %s:%d bus '%s' connected (RAW mode)\n",
	        sc_host, sc_port, sc_bus);
	return true;
}

static void sc_init(void)
{
	if (sc_fd < 0 && !sc_connect()) {
		fprintf(stderr, "gbt-linux: socketcand not reachable yet, will keep retrying\n");
	}
}

/* Parse one "< frame ID TS HEXDATA >" message into the shared RX ring. */
static void sc_handle_message(const char *msg)
{
	unsigned long id = 0;
	const char *p = msg;

	if (strncmp(msg, "< frame ", 8) != 0) {
		return; /* "< error ... >", "< stat ... >", "< hi >", ... */
	}
	p += 8;

	char *endp = NULL;
	id = strtoul(p, &endp, 16);
	if (endp == p) {
		return;
	}
	p = endp;

	/* skip the timestamp token */
	while (*p == ' ') p++;
	while (*p != '\0' && *p != ' ' && *p != '>') p++;

	/* remaining hex until ' ' or '>' is the data */
	while (*p == ' ') p++;
	const char *hex = p;
	size_t hexlen = 0;
	while (p[hexlen] != '\0' && p[hexlen] != ' ' && p[hexlen] != '>') {
		hexlen++;
	}
	if (hexlen > 16u) {
		hexlen = 16u;
	}

	uint8_t data[8] = {0};
	uint8_t dlc = (uint8_t)(hexlen / 2u);
	for (uint8_t i = 0; i < dlc; ++i) {
		unsigned v = 0;
		sscanf(&hex[i * 2u], "%2x", &v);
		data[i] = (uint8_t)v;
	}
	CanRingBufferPush((uint32_t)(id & 0x1FFFFFFFu), data, dlc);
}

static void sc_drain_rx(void)
{
	char msg[SC_MSG_MAX];

	while (sc_take_message(msg, sizeof(msg))) {
		sc_handle_message(msg);
	}
}

/* Drain the TCP stream into the RX ring.  Called from CanRingBufferPop() so
 * busy-wait consumers (J1939 TP) see incoming frames. */
static void sc_pump(void)
{
	if (sc_fd < 0) {
		uint32_t now = GetTS();
		if ((uint32_t)(now - sc_last_connect_try) >= SC_RECONNECT_MS) {
			sc_last_connect_try = now;
			(void)sc_connect();
		}
		return;
	}

	for (;;) {
		char tmp[1024];
		ssize_t n = recv(sc_fd, tmp, sizeof(tmp), MSG_DONTWAIT);
		if (n > 0) {
			sc_feed(tmp, (size_t)n);
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n == 0) {
			sc_disconnect("peer closed");
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			sc_disconnect(strerror(errno));
		}
		break;
	}
	sc_drain_rx();
}

static void sc_send_frame(uint32_t id, uint8_t *buf, uint8_t len)
{
	char cmd[SC_CMD_MAX];
	int pos;

	if (sc_fd < 0) {
		return;
	}
	if (len > 8u) {
		len = 8u;
	}

	/* socketcand derives the frame format from the hex digit count of the id:
	 * 8 digits -> extended, 3 digits -> standard */
	pos = snprintf(cmd, sizeof(cmd), "< send %0*X %u",
	               (id > 0x7FFu) ? 8 : 3, (unsigned int)id, (unsigned int)len);
	for (uint8_t i = 0; i < len && pos < (int)sizeof(cmd); ++i) {
		pos += snprintf(&cmd[pos], sizeof(cmd) - (size_t)pos, " %02x", buf[i]);
	}
	if (pos < (int)sizeof(cmd)) {
		snprintf(&cmd[pos], sizeof(cmd) - (size_t)pos, " >");
	}

	if (!sc_send_all(cmd)) {
		sc_disconnect("send failed");
		return;
	}
	CanFrameObserve(id, buf, len, true);
}

static void sc_close(void)
{
	sc_disconnect(NULL);
}

static const CanBackendOps socketcand_ops = {
	.init = sc_init,
	.send = sc_send_frame,
	.pump = sc_pump,
	.close = sc_close,
};

const CanBackendOps *CanSocketcand_Backend(void)
{
	return &socketcand_ops;
}
