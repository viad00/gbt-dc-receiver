#ifndef SETTINGS_NVM_FILE_H
#define SETTINGS_NVM_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* File-backed settings page for the Linux host build (settings_nvm.h). */

/* Select the storage file (default "gbt_settings.bin").  Call before
 * SettingsStore_Init(). */
void SettingsNvmFile_SetPath(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_NVM_FILE_H */
