/*
 * settings_store.c
 *
 * Persistent settings storage in a dedicated flash sector.
 * LICENSE: MIT
 */

#include "settings_store.h"
#include "main.h"
#include "display_ui.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ====== CONFIGURATION ======
 * STM32F405 (1MB flash) sector map:
 *   0x08000000..0x0801FFFF  sectors 0..4 (application)
 *   0x08020000..0x080DFFFF  sectors 5..10 (CAN log area, 768KB)
 *   0x080E0000..0x080FFFFF  sector 11 (128KB) -> settings page
 * Settings use their own sector so logging is never affected.
 */
#define SETTINGS_FLASH_ADDR   ((uint32_t)0x080E0000u)
#define SETTINGS_SECTOR       FLASH_SECTOR_11
#define SETTINGS_MAGIC        ((uint32_t)0x5354534Bu) /* 'KSTS' */
#define SETTINGS_VERSION      ((uint32_t)1u)

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

static HAL_StatusTypeDef settings_erase(void)
{
	FLASH_EraseInitTypeDef ei = {0};
	uint32_t err = 0;
	HAL_FLASH_Unlock();
	ei.TypeErase = FLASH_TYPEERASE_SECTORS;
	ei.Sector = SETTINGS_SECTOR;
	ei.NbSectors = 1;
	ei.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&ei, &err);
	HAL_FLASH_Lock();
	return st;
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

	if (settings_erase() != HAL_OK) {
		return false;
	}

	HAL_FLASH_Unlock();
	const uint32_t *w = (const uint32_t *)&blob;
	uint32_t n = (uint32_t)(sizeof(SettingsBlob) / sizeof(uint32_t));
	for (uint32_t i = 0; i < n; ++i) {
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
		                      SETTINGS_FLASH_ADDR + i * 4u, w[i]) != HAL_OK) {
			HAL_FLASH_Lock();
			return false;
		}
	}
	HAL_FLASH_Lock();
	settings_valid = true;
	return true;
}

void SettingsStore_Clear(void)
{
	(void)settings_erase();
	settings_valid = false;
}

void SettingsStore_Init(void)
{
	const SettingsBlob *b = (const SettingsBlob *)SETTINGS_FLASH_ADDR;

	if (b->magic != SETTINGS_MAGIC || b->version != SETTINGS_VERSION) {
		settings_valid = false;
		return;
	}
	if (b->crc != settings_crc(b)) {
		settings_valid = false;
		return;
	}

	GbtSetRuntime(&b->runtime);
	FlashLog_SetEnabled(b->log_enabled);
	settings_valid = true;
}
