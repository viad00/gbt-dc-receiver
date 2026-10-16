#ifndef CAN_HELPERS_H
#define CAN_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Platform-independent CAN service interface.
 *
 * The GB/T 27930 state machine (gbt_27930_bms.c) uses only these functions.
 * Each platform provides the transport half:
 *   - MCU:    Core/Src/can.c         (bxCAN HAL)
 *   - Linux:  linux/can_socketcan.c  (SocketCAN)
 * Both share the RX ring buffer in Core/Src/can_buffer.c.
 */

/* Bring up the platform transport (MCU: bxCAN filters + start; host: SocketCAN socket). */
void CanInit(void);

/* Transmit one classic CAN frame (J1939 ids are 29-bit extended). */
void CanSendMsg(uint32_t id, uint8_t *buf, uint8_t len);

/* Pop one received frame from the shared RX ring buffer.
 * Pumps the platform transport first (CanRxPump), so busy-wait consumers
 * (J1939 transport protocol) receive frames even on single-threaded hosts.
 * Returns 1 when a frame was popped, 0 when the buffer is empty. */
uint8_t CanRingBufferPop(uint32_t *id, uint8_t *buf, uint8_t *len);

/* Feed one received frame into the shared RX ring buffer (platform RX path). */
void CanRingBufferPush(uint32_t id, const uint8_t *buf, uint8_t len);

/* Optional platform hooks.  Weak no-op defaults live in can_buffer.c:
 *   CanRxPump       - move pending transport RX into the ring buffer
 *                     (host: drain the SocketCAN socket; MCU: IRQ-driven, no-op)
 *   CanFrameObserve - per-frame observer for logging/tracing
 *                     (MCU: flash log + UART debug trace) */
void CanRxPump(void);
void CanFrameObserve(uint32_t id, uint8_t *buf, uint8_t len, bool is_tx);

/* Raw CAN frame trace toggle (DCAN CLI command). */
void DebugCan_SetEnabled(bool enabled);
bool DebugCan_IsEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_HELPERS_H */