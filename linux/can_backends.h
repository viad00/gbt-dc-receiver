#ifndef CAN_BACKENDS_H
#define CAN_BACKENDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* Linux CAN backends for the shared can.h interface.  Exactly one backend is
 * active per run; main.c selects it with CanSetBackend() before CanInit():
 *   - SocketCAN  (can_socketcan.c)   direct local interface, e.g. can0/vcan0
 *   - socketcand (can_socketcand.c)  TCP tunnel to a socketcand daemon,
 *                                    https://github.com/linux-can/socketcand
 *
 * can.h entry points (CanInit/CanSendMsg/CanRxPump) are dispatched to the
 * active backend by can_dispatch.c, which also implements the shared
 * CanFrameObserve() trace hook.  Trace lines match the MCU debug format:
 * <ms>,<id:08X>,true,<Rx|Tx>,0,<dlc>,<d0..d7 hex>
 */

typedef struct {
	void (*init)(void);
	void (*send)(uint32_t id, uint8_t *buf, uint8_t len);
	void (*pump)(void);
	void (*close)(void);
} CanBackendOps;

/* Select the active backend (call before CanInit()). */
void CanSetBackend(const CanBackendOps *ops);

/* Close the active backend (called on shutdown). */
void CanClose(void);

/* Optional DCAN trace sink (e.g. MQTT publisher); without a sink the trace
 * lines go to stdout. */
void CanSetTraceSink(void (*fn)(void *user, const char *line), void *user);

/* ---- SocketCAN backend ---- */

const CanBackendOps *CanSocketcan_Backend(void);

/* Select the CAN interface (default "can0"). */
void CanSocketcan_SetInterface(const char *ifname);

/* ---- socketcand backend ---- */

const CanBackendOps *CanSocketcand_Backend(void);

/* socketcand server endpoint (default 127.0.0.1:29536). */
void CanSocketcand_SetServer(const char *host, int port);

/* Bus name to `< open >` on the server (default "can0", max 16 chars). */
void CanSocketcand_SetBus(const char *bus);

#ifdef __cplusplus
}
#endif

#endif /* CAN_BACKENDS_H */
