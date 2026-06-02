#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void UI_Init(void);
void UI_Loop(void);
void UI_OnButtonInterrupt(uint16_t gpio_pin);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_UI_H */
