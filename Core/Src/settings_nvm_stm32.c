/*
 * settings_nvm_stm32.c
 *
 * STM32 NVM backend for the settings store: a dedicated flash sector.
 * LICENSE: MIT
 */

#include "settings_nvm.h"

#include "stm32f4xx_hal.h"

#include <string.h>
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

bool SettingsNvm_Read(void *dst, uint32_t len)
{
	memcpy(dst, (const void *)SETTINGS_FLASH_ADDR, len);
	return true;
}

bool SettingsNvm_Write(const void *src, uint32_t len)
{
	if (settings_erase() != HAL_OK) {
		return false;
	}

	HAL_FLASH_Unlock();
	const uint8_t *p = (const uint8_t *)src;
	uint32_t n = (len + 3u) / 4u;
	bool ok = true;
	for (uint32_t i = 0; i < n && ok; ++i) {
		uint32_t word = 0xFFFFFFFFu;
		memcpy(&word, p + i * 4u, (len - i * 4u) >= 4u ? 4u : (len - i * 4u));
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
		                      SETTINGS_FLASH_ADDR + i * 4u, word) != HAL_OK) {
			ok = false;
		}
	}
	HAL_FLASH_Lock();
	return ok;
}

void SettingsNvm_Erase(void)
{
	(void)settings_erase();
}
