#ifndef GBT_CLI_H
#define GBT_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Portable runtime CLI.
 *
 * The same command parser is used by the STM32 firmware (UART4 console) and
 * by the Linux host program (MQTT console).  Only the output sink differs.
 *
 * Recognized commands:
 *   STATE                 - print current GB/T 27930 charge state
 *   RLIST                 - list all runtime fields
 *   RGET <FIELD>          - read one runtime field
 *   RSET <FIELD> <value>  - write one runtime field (clamped to range)
 *   SAVE                  - persist runtime fields to the settings store
 *   DCAN [ON|OFF]         - query/toggle raw CAN frame trace
 *
 * Platform-specific commands (flash log SEQ/COUNT/ERASE/INFO on the MCU) are
 * handled by the caller when GbtCli_ProcessCommand() returns false.
 */

/* Output sink: called one or more times per command with response chunks. */
typedef void (*GbtCliWriteFn)(void *user, const char *text);

/* Process one command line (without trailing CR/LF).
 * Returns true when the command was recognized by the portable CLI,
 * false when the caller should try its platform-specific commands. */
bool GbtCli_ProcessCommand(const char *cmd, GbtCliWriteFn write, void *user);

#ifdef __cplusplus
}
#endif

#endif /* GBT_CLI_H */
