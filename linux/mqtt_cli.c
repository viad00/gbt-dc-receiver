/*
 * mqtt_cli.c
 *
 * MQTT console for the Linux host build, replacing the MCU UART CLI.  The
 * command parser is the shared Core/Src/gbt_cli.c; only the transport
 * differs: commands arrive on <base>/cmd and responses go to <base>/resp.
 * LICENSE: MIT
 */

#include "mqtt_cli.h"

#include "gbt_cli.h"
#include "helpers.h"

#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define RESP_BUF_SIZE   8192
#define CMD_BUF_SIZE    512
#define TOPIC_SIZE      256
#define RECONNECT_MS    2000u
#define KEEPALIVE_S     30

static struct mosquitto *mosq = NULL;
static char topic_cmd[TOPIC_SIZE];
static char topic_resp[TOPIC_SIZE];
static char topic_base[TOPIC_SIZE];
static char conn_host[256] = "127.0.0.1";
static int conn_port = 1883;
static bool is_connected = false;
static uint32_t last_connect_try = 0;

typedef struct {
	char buf[RESP_BUF_SIZE];
	size_t len;
} RespAcc;

static void resp_append(void *user, const char *text)
{
	RespAcc *acc = (RespAcc *)user;
	size_t n = strlen(text);
	if (acc->len + n >= sizeof(acc->buf)) {
		n = sizeof(acc->buf) - acc->len - 1;
	}
	memcpy(&acc->buf[acc->len], text, n);
	acc->len += n;
	acc->buf[acc->len] = '\0';
}

static void on_connect(struct mosquitto *m, void *userdata, int rc)
{
	(void)userdata;
	if (rc != 0) {
		fprintf(stderr, "gbt-linux: MQTT connect refused (%s)\n", mosquitto_strerror(rc));
		return;
	}
	is_connected = true;
	fprintf(stderr, "gbt-linux: MQTT connected, listening on %s\n", topic_cmd);
	/* re-subscribe on every (re)connect */
	mosquitto_subscribe(m, NULL, topic_cmd, 1);
}

static void on_disconnect(struct mosquitto *m, void *userdata, int rc)
{
	(void)m;
	(void)userdata;
	is_connected = false;
	if (rc != 0) {
		fprintf(stderr, "gbt-linux: MQTT connection lost (%s), reconnecting\n",
		        mosquitto_strerror(rc));
	}
}

static void run_command(const char *cmd, RespAcc *acc)
{
	printf("> %s\n", cmd);
	if (!GbtCli_ProcessCommand(cmd, resp_append, acc)) {
		resp_append(acc, "UNKNOWN CMD\r\n");
	}
	fputs(acc->buf, stdout);
	fflush(stdout);
}

static void on_message(struct mosquitto *m, void *userdata, const struct mosquitto_message *msg)
{
	(void)m;
	(void)userdata;

	if (msg->topic == NULL || msg->payloadlen <= 0 ||
	    strcmp(msg->topic, topic_cmd) != 0) {
		return;
	}

	static char payload[CMD_BUF_SIZE];
	int n = msg->payloadlen;
	if (n >= (int)sizeof(payload)) {
		n = (int)sizeof(payload) - 1;
	}
	memcpy(payload, msg->payload, (size_t)n);
	payload[n] = '\0';

	/* one message may carry several newline-separated commands */
	char *saveptr = NULL;
	for (char *line = strtok_r(payload, "\n", &saveptr); line != NULL;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		size_t ln = strlen(line);
		while (ln > 0 && (line[ln - 1] == '\r' || line[ln - 1] == ' ' ||
		                  line[ln - 1] == '\t')) {
			line[--ln] = '\0';
		}
		if (ln == 0) {
			continue;
		}

		RespAcc acc;
		acc.len = 0;
		acc.buf[0] = '\0';
		run_command(line, &acc);
		if (acc.len > 0) {
			mosquitto_publish(m, NULL, topic_resp, (int)acc.len, acc.buf, 0, false);
		}
	}
}

bool MqttCli_Init(const char *host, int port, const char *user, const char *pass,
                  const char *base_topic, const char *client_id)
{
	const char *base = (base_topic != NULL && base_topic[0] != '\0') ? base_topic : "gbt";

	if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "gbt-linux: mosquitto_lib_init failed\n");
		return false;
	}

	mosq = mosquitto_new((client_id != NULL && client_id[0] != '\0') ? client_id : "gbt-linux",
	                     true, NULL);
	if (mosq == NULL) {
		fprintf(stderr, "gbt-linux: mosquitto_new failed\n");
		return false;
	}

	strncpy(conn_host, (host != NULL && host[0] != '\0') ? host : "127.0.0.1",
	        sizeof(conn_host) - 1);
	conn_port = (port > 0) ? port : 1883;

	snprintf(topic_base, sizeof(topic_base), "%s", base);
	snprintf(topic_cmd, sizeof(topic_cmd), "%s/cmd", base);
	snprintf(topic_resp, sizeof(topic_resp), "%s/resp", base);

	if (user != NULL && user[0] != '\0') {
		mosquitto_username_pw_set(mosq, user, (pass != NULL) ? pass : NULL);
	}

	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_disconnect_callback_set(mosq, on_disconnect);
	mosquitto_message_callback_set(mosq, on_message);
	return true;
}

bool MqttCli_Connect(void)
{
	int rc;

	if (mosq == NULL) {
		return false;
	}
	last_connect_try = GetTS();
	rc = mosquitto_connect(mosq, conn_host, conn_port, KEEPALIVE_S);
	if (rc != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "gbt-linux: MQTT connect to %s:%d failed: %s\n",
		        conn_host, conn_port, mosquitto_strerror(rc));
		is_connected = false;
		return false;
	}
	return true;
}

int MqttCli_Socket(void)
{
	if (mosq == NULL) {
		return -1;
	}
	/* valid as soon as TCP is up (CONNACK may still be in flight) */
	return mosquitto_socket(mosq);
}

bool MqttCli_WantWrite(void)
{
	return mosq != NULL && mosquitto_want_write(mosq);
}

void MqttCli_Loop(void)
{
	if (mosq == NULL) {
		return;
	}

	if (mosquitto_socket(mosq) < 0) {
		uint32_t now = GetTS();
		if ((uint32_t)(now - last_connect_try) >= RECONNECT_MS) {
			last_connect_try = now;
			(void)mosquitto_reconnect(mosq); /* retried on the next pass */
		}
	}

	if (mosquitto_socket(mosq) >= 0) {
		mosquitto_loop_read(mosq, 1);
		mosquitto_loop_write(mosq, 1);
		mosquitto_loop_misc(mosq);
	}
}

void MqttCli_Publish(const char *subtopic, const char *payload, bool retain)
{
	char topic[2 * TOPIC_SIZE + 4];

	if (mosq == NULL || !is_connected || subtopic == NULL || payload == NULL) {
		return;
	}
	snprintf(topic, sizeof(topic), "%s/%s", topic_base, subtopic);
	mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload, 0, retain);
}

void MqttCli_Disconnect(void)
{
	if (mosq != NULL) {
		mosquitto_disconnect(mosq);
		mosquitto_destroy(mosq);
		mosq = NULL;
	}
	mosquitto_lib_cleanup();
	is_connected = false;
}
