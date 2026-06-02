#ifndef HELPERS_H
#define HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

uint32_t GetTS(void);
void DelayMs(uint32_t ms);
void LED(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* HELPERS_H */