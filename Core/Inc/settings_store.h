#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "gbt_27930_bms.h"

/* Persistent settings storage in a dedicated flash sector (independent of the
 * CAN log area). */

void SettingsStore_Init(void);              /* load settings into runtime on boot */
bool SettingsStore_Save(void);              /* save current runtime + UI prefs */
void SettingsStore_Clear(void);             /* erase the settings page */
bool SettingsStore_IsValid(void);           /* settings present in flash */

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_STORE_H */
