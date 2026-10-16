/*
 * settings_nvm_file.c
 *
 * Linux NVM backend for the settings store: a plain binary file with the
 * same page layout the MCU stores in its flash sector.
 * LICENSE: MIT
 */

#include "settings_nvm.h"
#include "settings_nvm_file.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static char settings_path[512] = "gbt_settings.bin";

void SettingsNvmFile_SetPath(const char *path)
{
	if (path != NULL && path[0] != '\0') {
		strncpy(settings_path, path, sizeof(settings_path) - 1);
		settings_path[sizeof(settings_path) - 1] = '\0';
	}
}

bool SettingsNvm_Read(void *dst, uint32_t len)
{
	FILE *f = fopen(settings_path, "rb");
	if (f == NULL) {
		return false;
	}
	size_t n = fread(dst, 1, len, f);
	fclose(f);
	return n == (size_t)len;
}

bool SettingsNvm_Write(const void *src, uint32_t len)
{
	char tmp_path[sizeof(settings_path) + 8];
	FILE *f;

	/* write to a temp file first so a crash never leaves a torn page */
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", settings_path);
	f = fopen(tmp_path, "wb");
	if (f == NULL) {
		return false;
	}
	if (fwrite(src, 1, len, f) != (size_t)len) {
		fclose(f);
		remove(tmp_path);
		return false;
	}
	if (fclose(f) != 0) {
		remove(tmp_path);
		return false;
	}
	if (rename(tmp_path, settings_path) != 0) {
		remove(tmp_path);
		return false;
	}
	return true;
}

void SettingsNvm_Erase(void)
{
	remove(settings_path);
}
