/*
 * settings_store.c
 *
 * Persistent runtime settings storage (platform independent page layout).
 * LICENSE: MIT
 */

#include "settings_store.h"
#include "settings_nvm.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* Settings page layout is platform independent; the NVM backend lives behind
 * settings_nvm.h (STM32 flash sector on the MCU, plain file on the host). */

/* Platform flash log service state stored in the blob (flash_writer.c on the
 * MCU).  Weak defaults keep the host build linkable. */
bool FlashLog_IsEnabled(void);
void FlashLog_SetEnabled(bool enabled);

__attribute__((weak)) bool FlashLog_IsEnabled(void) { return false; }
__attribute__((weak)) void FlashLog_SetEnabled(bool enabled) { (void)enabled; }

#define SETTINGS_MAGIC        ((uint32_t)0x5354534Bu) /* 'KSTS' */
#define SETTINGS_VERSION      ((uint32_t)2u) /* v2: GbtRuntime gained BRO_WAIT_CONFIRM/BRO_CONFIRM/BRO_READY/WAIT_FOR_CHM/LOCK_BEFORE_START */

typedef struct {
	uint32_t magic;
	uint32_t version;
	GbtRuntime runtime;
	bool log_enabled;
	uint32_t crc;
} SettingsBlob;

static bool settings_valid = false;

/* Simple additive CRC (enough to catch erase/programming corruption) */
static uint32_t settings_crc(const SettingsBlob *b)
{
	const uint32_t *w = (const uint32_t *)b;
	uint32_t n = (uint32_t)(sizeof(SettingsBlob) / sizeof(uint32_t));
	uint32_t crc = 0x12345678u;
	for (uint32_t i = 0; i < n; ++i) {
		if (&w[i] == &b->crc) {
			continue;
		}
		crc = (crc << 1) | (crc >> 31);
		crc ^= w[i];
	}
	return crc;
}

bool SettingsStore_IsValid(void)
{
	return settings_valid;
}

bool SettingsStore_Save(void)
{
	SettingsBlob blob;
	memset(&blob, 0xFF, sizeof(blob));
	GbtGetRuntime(&blob.runtime);
	blob.magic = SETTINGS_MAGIC;
	blob.version = SETTINGS_VERSION;
	blob.log_enabled = FlashLog_IsEnabled();
	blob.crc = settings_crc(&blob);

	if (!SettingsNvm_Write(&blob, (uint32_t)sizeof(blob))) {
		return false;
	}
	settings_valid = true;
	return true;
}

void SettingsStore_Clear(void)
{
	SettingsNvm_Erase();
	settings_valid = false;
}

void SettingsStore_Init(void)
{
	SettingsBlob b;

	if (!SettingsNvm_Read(&b, (uint32_t)sizeof(b))) {
		settings_valid = false;
		return;
	}

	if (b.magic != SETTINGS_MAGIC || b.version != SETTINGS_VERSION) {
		settings_valid = false;
		return;
	}
	if (b.crc != settings_crc(&b)) {
		settings_valid = false;
		return;
	}

	GbtSetRuntime(&b.runtime);
	FlashLog_SetEnabled(b.log_enabled);
	settings_valid = true;
}
