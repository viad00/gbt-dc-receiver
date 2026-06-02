#ifndef CAN_HELPERS_H
#define CAN_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void CanInit(void);
uint8_t CanRingBufferPop(uint32_t *id, uint8_t *buf, uint8_t *len);
void CanSendMsg(uint32_t id, uint8_t *buf, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* CAN_HELPERS_H */