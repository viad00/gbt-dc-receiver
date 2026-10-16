/*
 * can_socketcan.c
 *
 * Linux transport for the shared CAN interface (can.h) on top of SocketCAN.
 * The RX ring buffer and debug toggle live in Core/Src/can_buffer.c.
 * LICENSE: MIT
 */

#include "can.h"
#include "can_backends.h"
#include "helpers.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int can_fd = -1;
static char can_ifname[IFNAMSIZ] = "can0";

void CanSocketcan_SetInterface(const char *ifname)
{
	if (ifname != NULL && ifname[0] != '\0') {
		strncpy(can_ifname, ifname, sizeof(can_ifname) - 1);
		can_ifname[sizeof(can_ifname) - 1] = '\0';
	}
}

static void sock_init(void)
{
	struct ifreq ifr;
	struct sockaddr_can addr;

	can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (can_fd < 0) {
		perror("socket(PF_CAN)");
		exit(1);
	}

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", can_ifname);
	if (ioctl(can_fd, SIOCGIFINDEX, &ifr) < 0) {
		fprintf(stderr, "CAN interface '%s' not found (create it first, e.g.\n"
		                "  sudo modprobe vcan && sudo ip link add dev %s type vcan && sudo ip link set up %s)\n",
		        can_ifname, can_ifname, can_ifname);
		exit(1);
	}

	memset(&addr, 0, sizeof(addr));
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(can_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind(PF_CAN)");
		exit(1);
	}

	int flags = fcntl(can_fd, F_GETFL, 0);
	if (flags < 0 || fcntl(can_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl(O_NONBLOCK)");
		exit(1);
	}

	fprintf(stderr, "gbt-linux: SocketCAN bound to '%s'\n", can_ifname);
}

/* Drain the socket into the shared RX ring buffer.  Called from
 * CanRingBufferPop() so busy-wait consumers see incoming frames. */
static void sock_pump(void)
{
	struct can_frame fr;

	while (can_fd >= 0) {
		ssize_t n = recv(can_fd, &fr, sizeof(fr), 0);
		if (n < (ssize_t)sizeof(fr)) {
			break; /* empty or error (e.g. ENETDOWN) */
		}
		uint32_t id = (fr.can_id & CAN_EFF_FLAG)
		                  ? (fr.can_id & CAN_EFF_MASK)
		                  : (fr.can_id & CAN_SFF_MASK);
		uint8_t len = (fr.can_id & CAN_RTR_FLAG) ? 0u : (uint8_t)fr.can_dlc;
		if (len > 8u) {
			len = 8u;
		}
		CanRingBufferPush(id, fr.data, len);
	}
}

static void sock_send(uint32_t id, uint8_t *buf, uint8_t len)
{
	struct can_frame fr;

	if (can_fd < 0) {
		return;
	}

	memset(&fr, 0, sizeof(fr));
	/* GB/T 27930 uses 29-bit J1939 identifiers only. */
	fr.can_id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
	fr.can_dlc = (len > 8u) ? 8u : len;
	memcpy(fr.data, buf, fr.can_dlc);

	if (write(can_fd, &fr, sizeof(fr)) != (ssize_t)sizeof(fr)) {
		perror("can write");
		return;
	}
	CanFrameObserve(id, buf, fr.can_dlc, true);
}

static void sock_close(void)
{
	if (can_fd >= 0) {
		close(can_fd);
		can_fd = -1;
	}
}

static const CanBackendOps sock_ops = {
	.init = sock_init,
	.send = sock_send,
	.pump = sock_pump,
	.close = sock_close,
};

const CanBackendOps *CanSocketcan_Backend(void)
{
	return &sock_ops;
}
