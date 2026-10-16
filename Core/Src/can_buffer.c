/*
 * can_buffer.c
 *
 * Shared CAN RX ring buffer and debug-trace toggle, used by both the STM32
 * firmware (Core/Src/can.c) and the Linux host program (linux/can_socketcan.c).
 * LICENSE: MIT
 */

#include "can.h"

#include <string.h>

#define CAN_RING_SIZE 256u

typedef struct {
	uint32_t buffer_id[CAN_RING_SIZE];
	uint8_t buffer_len[CAN_RING_SIZE];
	uint64_t buffer_data[CAN_RING_SIZE];
	uint8_t RecvPos;
	uint8_t SendPos;
} CanRingBuff;

static CanRingBuff ring_buffer;
static bool debug_can_enabled = false;

/* Weak default: nothing to pump.  The host build overrides this to drain its
 * SocketCAN socket into the ring buffer. */
__attribute__((weak)) void CanRxPump(void)
{
}

/* Weak default: drop frames.  The MCU build overrides this with the flash
 * logger + UART debug trace. */
__attribute__((weak)) void CanFrameObserve(uint32_t id, uint8_t *buf, uint8_t len, bool is_tx)
{
	(void)id;
	(void)buf;
	(void)len;
	(void)is_tx;
}

void DebugCan_SetEnabled(bool enabled)
{
	debug_can_enabled = enabled;
}

bool DebugCan_IsEnabled(void)
{
	return debug_can_enabled;
}

void CanRingBufferPush(uint32_t id, const uint8_t *buf, uint8_t len)
{
	if (len > sizeof(uint64_t)) {
		len = (uint8_t)sizeof(uint64_t);
	}
	memcpy(&(ring_buffer.buffer_id[ring_buffer.RecvPos]), &id, sizeof(id));
	memcpy(&(ring_buffer.buffer_len[ring_buffer.RecvPos]), &len, sizeof(len));
	memcpy(&(ring_buffer.buffer_data[ring_buffer.RecvPos]), buf, len);
	ring_buffer.RecvPos++;
}

uint8_t CanRingBufferPop(uint32_t *id, uint8_t *buf, uint8_t *len)
{
	CanRxPump();
	if (ring_buffer.SendPos == ring_buffer.RecvPos) return 0;
	memcpy(id, &(ring_buffer.buffer_id[ring_buffer.SendPos]), sizeof(uint32_t));
	memcpy(len, &(ring_buffer.buffer_len[ring_buffer.SendPos]), sizeof(uint8_t));
	memcpy(buf, &(ring_buffer.buffer_data[ring_buffer.SendPos]), sizeof(uint64_t));
	ring_buffer.SendPos++;
	CanFrameObserve(*id, buf, *len, false);
	return 1;
}
