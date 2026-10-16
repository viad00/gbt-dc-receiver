/*
 * gbt_cli.c
 *
 * Portable runtime CLI shared by the STM32 firmware and the Linux host
 * program.  Command set and response texts match the historical UART CLI.
 * LICENSE: MIT
 */

#include "gbt_cli.h"

#include "gbt_27930_bms.h"
#include "settings_store.h"
#include "can.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static void cli_write(GbtCliWriteFn write, void *user, const char *fmt, ...)
{
	char line[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	write(user, line);
}

bool GbtCli_ProcessCommand(const char *cmd, GbtCliWriteFn write, void *user)
{
	if (cmd == NULL || write == NULL) {
		return false;
	}

	if (strncmp(cmd, "RSET ", 5) == 0) {
		char field_name[32];
		long val;
		if (sscanf(&cmd[5], "%31s %ld", field_name, &val) == 2) {
			GbtRuntimeField f = GbtRuntimeFieldFromString(field_name);
			if (f < GBT27930_RUNTIME_FIELD_COUNT) {
				GbtSetRuntimeField(f, (int32_t)val);
				cli_write(write, user, "SET %s=%ld\r\n", field_name, val);
			} else {
				write(user, "UNKNOWN FIELD\r\n");
			}
		} else {
			write(user, "USAGE: RSET <FIELD> <value>\r\n");
		}
		return true;
	}

	if (strncmp(cmd, "RGET ", 5) == 0) {
		char field_name[32];
		if (sscanf(&cmd[5], "%31s", field_name) == 1) {
			GbtRuntimeField f = GbtRuntimeFieldFromString(field_name);
			if (f < GBT27930_RUNTIME_FIELD_COUNT) {
				int32_t val = GbtGetRuntimeField(f);
				cli_write(write, user, "%s=%ld\r\n", field_name, (long)val);
			} else {
				write(user, "UNKNOWN FIELD\r\n");
			}
		} else {
			write(user, "USAGE: RGET <FIELD>\r\n");
		}
		return true;
	}

	if (strcmp(cmd, "RLIST") == 0) {
		write(user, "--- Runtime fields ---\r\n");
		for (uint32_t i = 0; i < (uint32_t)GBT27930_RUNTIME_FIELD_COUNT; ++i) {
			int32_t val = GbtGetRuntimeField((GbtRuntimeField)i);
			const char *nm = GbtRuntimeFieldName((GbtRuntimeField)i);
			cli_write(write, user, "  %s=%ld\r\n", nm, (long)val);
		}
		return true;
	}

	if (strcmp(cmd, "SAVE") == 0) {
		write(user, SettingsStore_Save() ? "SETTINGS SAVED\r\n" : "SAVE FAILED\r\n");
		return true;
	}

	if (strcmp(cmd, "STATE") == 0) {
		GbtChargeState st = GbtGetChargeState();
		cli_write(write, user, "STATE=%s\r\n", GbtChargeStateName(st));
		return true;
	}

	if (strcmp(cmd, "DCAN ON") == 0) {
		DebugCan_SetEnabled(true);
		write(user, "DEBUG CAN: ON\r\n");
		return true;
	}

	if (strcmp(cmd, "DCAN OFF") == 0) {
		DebugCan_SetEnabled(false);
		write(user, "DEBUG CAN: OFF\r\n");
		return true;
	}

	if (strcmp(cmd, "DCAN") == 0) {
		write(user, DebugCan_IsEnabled() ? "DEBUG CAN: ON\r\n" : "DEBUG CAN: OFF\r\n");
		return true;
	}

	return false;
}
