/*
 * main.c
 *
 * Linux host program for the GB/T 27930 BMS logic in Core/Src/gbt_27930_bms.c.
 * Hardware is replaced by two platform halves:
 *   CAN  -> SocketCAN      (linux/can_socketcan.c)
 *   CLI  -> MQTT console   (linux/mqtt_cli.c, replaces the MCU UART CLI)
 * The protocol state machine itself is the same source file the MCU uses.
 * LICENSE: MIT
 */

#include "gbt_27930_bms.h"
#include "settings_store.h"
#include "can.h"
#include "helpers.h"

#include "can_backends.h"
#include "settings_nvm_file.h"
#include "mqtt_cli.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void trace_publish(void *user, const char *line)
{
	(void)user;
	MqttCli_Publish("can", line, false);
}

/* SocketCAN interface names never contain '.' or ':', so a dotted host or a
 * host:port pair selects the socketcand backend instead. */
static bool is_socketcand_endpoint(const char *spec)
{
	return strchr(spec, ':') != NULL || strchr(spec, '.') != NULL ||
	       strcmp(spec, "localhost") == 0;
}

static void split_host_port(const char *spec, char *host, size_t hostlen, int *port)
{
	const char *colon = strrchr(spec, ':');

	if (colon != NULL && colon[1] != '\0') {
		bool digits = true;
		for (const char *p = colon + 1; *p != '\0'; ++p) {
			if (*p < '0' || *p > '9') {
				digits = false;
				break;
			}
		}
		if (digits) {
			size_t n = (size_t)(colon - spec);
			if (n >= hostlen) {
				n = hostlen - 1;
			}
			memcpy(host, spec, n);
			host[n] = '\0';
			*port = atoi(colon + 1);
			return;
		}
	}
	snprintf(host, hostlen, "%s", spec);
}

static void publish_state_if_changed(void)
{
	static GbtChargeState last_state = (GbtChargeState)-1;
	static uint32_t last_pub = 0;
	char line[64];

	GbtChargeState st = GbtGetChargeState();
	uint32_t now = GetTS();

	/* publish on change and every 5 s (retained, so late subscribers see it) */
	if (st == last_state && (uint32_t)(now - last_pub) < 5000u) {
		return;
	}
	last_state = st;
	last_pub = now;

	snprintf(line, sizeof(line), "STATE=%s", GbtChargeStateName(st));
	MqttCli_Publish("state", line, true);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "Usage: %s [options]\n"
	        "  --if <spec>       CAN transport:\n"
	        "                      vcan0                  local SocketCAN interface\n"
	        "                      192.168.10.2           socketcand host  (port 29536)\n"
	        "                      192.168.10.2:22222     socketcand endpoint\n"
	        "                      localhost              socketcand on this machine\n"
	        "  --bus <name>      bus name to open on socketcand (default can0)\n"
	        "  --host <host>     MQTT broker host           (default 127.0.0.1)\n"
	        "  --port <port>     MQTT broker port           (default 1883)\n"
	        "  --user <user>     MQTT username              (default: none)\n"
	        "  --pass <pass>     MQTT password              (default: none)\n"
	        "  --topic <base>    MQTT base topic            (default gbt)\n"
	        "  --client-id <id>  MQTT client id             (default gbt-linux)\n"
	        "  --settings <file> settings storage file      (default gbt_settings.bin)\n"
	        "\n"
	        "MQTT console (same command syntax as the MCU UART CLI):\n"
	        "  publish commands to  <base>/cmd\n"
	        "  read  responses from <base>/resp\n"
	        "  retained charge state on <base>/state\n"
	        "  raw CAN trace (\"DCAN ON\") on <base>/can\n",
	        argv0);
}

int main(int argc, char **argv)
{
	const char *if_spec = "can0";
	const char *bus_name = "can0";
	const char *host = "127.0.0.1";
	const char *user = NULL;
	const char *pass = NULL;
	const char *base = "gbt";
	const char *client_id = "gbt-linux";
	const char *settings = "gbt_settings.bin";
	int port = 1883;

	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (strcmp(a, "--if") == 0 && v != NULL) {
			if_spec = argv[++i];
		} else if (strcmp(a, "--bus") == 0 && v != NULL) {
			bus_name = argv[++i];
		} else if (strcmp(a, "--host") == 0 && v != NULL) {
			host = argv[++i];
		} else if (strcmp(a, "--port") == 0 && v != NULL) {
			port = atoi(argv[++i]);
		} else if (strcmp(a, "--user") == 0 && v != NULL) {
			user = argv[++i];
		} else if (strcmp(a, "--pass") == 0 && v != NULL) {
			pass = argv[++i];
		} else if (strcmp(a, "--topic") == 0 && v != NULL) {
			base = argv[++i];
		} else if (strcmp(a, "--client-id") == 0 && v != NULL) {
			client_id = argv[++i];
		} else if (strcmp(a, "--settings") == 0 && v != NULL) {
			settings = argv[++i];
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "unknown option: %s\n", a);
			usage(argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	bool use_socketcand = is_socketcand_endpoint(if_spec);
	if (use_socketcand) {
		char sc_host[256];
		int sc_port = 29536;
		split_host_port(if_spec, sc_host, sizeof(sc_host), &sc_port);
		CanSocketcand_SetServer(sc_host, sc_port);
		CanSocketcand_SetBus(bus_name);
		CanSetBackend(CanSocketcand_Backend());
	} else {
		CanSocketcan_SetInterface(if_spec);
		CanSetBackend(CanSocketcan_Backend());
	}
	CanSetTraceSink(trace_publish, NULL);

	SettingsNvmFile_SetPath(settings);

	if (!MqttCli_Init(host, port, user, pass, base, client_id)) {
		return 1;
	}
	if (!MqttCli_Connect()) {
		fprintf(stderr, "gbt-linux: starting without MQTT broker, will keep retrying\n");
	}

	CanInit(); /* fails hard if the interface is missing */
	SettingsStore_Init(); /* restore settings (same blob format as MCU flash) */

	SetupCharge();

	fprintf(stderr, "gbt-linux: GB/T 27930 BMS running\n");

	while (!g_stop) {
		LoopCharge();
		MqttCli_Loop();
		publish_state_if_changed();

		struct pollfd pfd;
		pfd.fd = MqttCli_Socket();
		pfd.events = POLLIN | (MqttCli_WantWrite() ? POLLOUT : 0);
		pfd.revents = 0;
		if (pfd.fd >= 0) {
			poll(&pfd, 1, 20);
		} else {
			DelayMs(20);
		}
	}

	MqttCli_Disconnect();
	CanClose();
	fprintf(stderr, "gbt-linux: bye\n");
	return 0;
}
