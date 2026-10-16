/*
 * can_dispatch.c
 *
 * Routes the shared can.h interface to the Linux CAN backend selected by
 * main.c and implements the shared CanFrameObserve() DCAN trace hook.
 * LICENSE: MIT
 */

#include "can.h"
#include "can_backends.h"
#include "helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const CanBackendOps *active_ops = NULL;
static void (*trace_sink)(void *user, const char *line) = NULL;
static void *trace_user = NULL;

void CanSetBackend(const CanBackendOps *ops)
{
	active_ops = ops;
}

void CanSetTraceSink(void (*fn)(void *user, const char *line), void *user)
{
	trace_sink = fn;
	trace_user = user;
}

void CanClose(void)
{
	if (active_ops != NULL && active_ops->close != NULL) {
		active_ops->close();
	}
}

void CanInit(void)
{
	if (active_ops == NULL || active_ops->init == NULL) {
		fprintf(stderr, "gbt-linux: no CAN backend selected\n");
		exit(1);
	}
	active_ops->init();
}

void CanSendMsg(uint32_t id, uint8_t *buf, uint8_t len)
{
	if (active_ops != NULL && active_ops->send != NULL) {
		active_ops->send(id, buf, len);
	}
}

void CanRxPump(void)
{
	if (active_ops != NULL && active_ops->pump != NULL) {
		active_ops->pump();
	}
}

/* Strong implementation of the shared observer hook (weak default in
 * can_buffer.c): DCAN frame trace, mirrored to MQTT or stdout. */
void CanFrameObserve(uint32_t id, uint8_t *buf, uint8_t len, bool is_tx)
{
	char line[128];
	uint8_t d[8] = {0};

	if (!DebugCan_IsEnabled()) {
		return;
	}
	memcpy(d, buf, (len > 8u) ? 8u : len);
	snprintf(line, sizeof(line),
	         "%lu,%08lX,true,%s,0,%u,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X",
	         (unsigned long)GetTS(),
	         (unsigned long)id,
	         is_tx ? "Tx" : "Rx",
	         (unsigned int)len,
	         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);

	if (trace_sink != NULL) {
		trace_sink(trace_user, line);
	} else {
		puts(line);
	}
}
