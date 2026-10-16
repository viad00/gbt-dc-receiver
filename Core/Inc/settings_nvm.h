#ifndef SETTINGS_NVM_H
#define SETTINGS_NVM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Platform NVM backend for the settings page (see settings_store.c).
 *
 * Implementations:
 *   MCU:   Core/Src/settings_nvm_stm32.c  (dedicated flash sector)
 *   Linux: linux/settings_nvm_file.c      (plain file)
 */

/* Copy the raw settings page into dst.  Returns false when the page holds no
 * data (erased/absent). */
bool SettingsNvm_Read(void *dst, uint32_t len);

/* Erase the settings page and program len bytes from src. */
bool SettingsNvm_Write(const void *src, uint32_t len);

/* Erase the settings page. */
void SettingsNvm_Erase(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_NVM_H */
