/*
 * flash_writer.c
 *
 *  Created on: Oct 23, 2025
 *      Author: vladislav utkin
 * LICENSE: MIT
 */


/* --------------------------------------------------------------------

Simple flash CAN logger
* Writes each CAN message (timestamp, id, flags, dlc, 8 bytes) immediately to FLASH
* Uses a linear area in FLASH (configurable below). On startup we scan the area

  to find the next free slot (so safe to power off at any time).

* UART interface (simple commands) to read sequence by index and erase the logging area.
NOTE: Adjust FLASH_LOG_START and FLASH_LOG_SIZE to appropriate sectors for your MCU.
-------------------------------------------------------------------- */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* ====== CONFIGURATION: set these to suit your device / partition plan ======

For STM32F405/STM32F4 series adjust LOG start and size to pages/sectors you reserved.
Example values below assume there is spare flash near the top of flash.
If you are unsure, change these macros to correct addresses for your project.
*/
#ifndef FLASH_LOG_START
#define FLASH_LOG_START    ((uint32_t)0x08020000) /* <-- CHANGE as needed */
#endif
#ifndef FLASH_LOG_SIZE
#define FLASH_LOG_SIZE     ((uint32_t)0x000C0000) /* Log store flash area size */
#endif

/* Each log entry is fixed-size for simple scanning and power-fail robustness. */
/* Layout (24 bytes total, written as 32-bit words): */
/* uint32_t magic;        // 0x4C4F4741 'LOGA' */
/* uint32_t timestamp_ms; // HAL_GetTick() */
/* uint32_t id_flags;     // id (lower 31 bits) | (is_tx ? 0x80000000 : 0) */
/* uint8_t  dlc;          // 1 byte */
/* uint8_t  data[8];      // always 8 bytes stored */
/* uint8_t  reserved[3];  // padding to 4-byte align */

#define FLASH_LOG_MAGIC    ((uint32_t)0x4C4F4741u) /* 'LOGA' */
#define FLASH_LOG_MAGIC_B    ((uint32_t)0x4C4F4742u) /* 'LOGB' */
#define FLASH_LOG_ENTRY_SIZE 24u
#define FLASH_LOG_END      (FLASH_LOG_START + FLASH_LOG_SIZE)

static uint32_t flash_log_write_addr = FLASH_LOG_START; /* next write address */
static uint32_t current_flash_log_magic = FLASH_LOG_MAGIC;
static bool flash_log_rx_started = false;

/* UART command handling (simple): receive ASCII commands terminated by \n or \r.

Commands:

  SEQ <n>   - stream all entries from sequence index n (0-based) as lines formatted

              exactly like DebugSendCanMsg output.

  ERASE     - erase the whole logging area (will report result).

  INFO      - report flash log start/size/used.


*/
static uint8_t uart_rx_char = 0;
static char uart_cmd_buf[64];
static uint8_t uart_cmd_idx = 0;

/* helper - transmit with blocking call (small) */
static void FlashLog_UARTSend(const char *s)
{
	while (HAL_UART_Transmit(&huart4, (uint8_t*)s, strlen(s), 500) != HAL_OK) {};
}

/* program a single 32-bit word to flash (caller must HAL_FLASH_Unlock/HAL_FLASH_Lock) */
static HAL_StatusTypeDef FlashLog_ProgramWord(uint32_t addr, uint32_t word)
{
/* address must be word aligned */
if ((addr & 3) != 0) return HAL_ERROR;
return HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word);
}

/* scan flash region to find next free slot. Safe to call on startup. */
static void FlashLog_ScanNextWriteAddr(void)
{

uint32_t addr = FLASH_LOG_START;
while (addr + FLASH_LOG_ENTRY_SIZE <= FLASH_LOG_END) {
     uint32_t magic = *(uint32_t*)addr;
     if (magic == 0xFFFFFFFFu) {
         /* first word free => this slot and following are free */
         flash_log_write_addr = addr;
         return;
     }
     /* otherwise assume entry present and skip */
     addr += FLASH_LOG_ENTRY_SIZE;
}
/* no space left */
flash_log_write_addr = FLASH_LOG_END;
}

/* initialize logging: scan area and start UART rx interrupt to accept commands */
 void FlashLog_Init(void)
{
FlashLog_ScanNextWriteAddr();
flash_log_rx_started = false;
/* start UART IT to collect commands */
HAL_UART_Receive_IT(&huart4, &uart_rx_char, 1);
FlashLog_UARTSend("INIT OK\n\r");
}

/* Write one entry if space available. Immediate write to flash (no buffering). */
 void FlashLog_Write(uint32_t id, uint8_t *buf, uint8_t dlc, bool is_tx)
{
/* start logging only when CAN is up and first RX was seen */
HAL_CAN_StateTypeDef can_state = HAL_CAN_GetState(&hcan1);
if (can_state != HAL_CAN_STATE_READY && can_state != HAL_CAN_STATE_LISTENING) {
    return;
}

if (!flash_log_rx_started) {
    if (is_tx) {
        return;
    }
    flash_log_rx_started = true;
}

if (flash_log_write_addr + FLASH_LOG_ENTRY_SIZE > FLASH_LOG_END) {
     /* no space: do nothing */
     return;

}

uint32_t words[6]; /* 6 * 4 = 24 bytes */
words[0] = current_flash_log_magic;
if (current_flash_log_magic == FLASH_LOG_MAGIC) current_flash_log_magic = FLASH_LOG_MAGIC_B; // CHANGE magic to b variant after 1st write
words[1] = (uint32_t)HAL_GetTick();
words[2] = (is_tx ? 0x80000000u : 0u) | (id & 0x7FFFFFFFu);
/* pack dlc and first 3 data bytes into word3 */
uint8_t data8[8] = {0,0,0,0,0,0,0,0};
for (int i=0;i<8;i++) data8[i] = buf[i];
words[3] = ((uint32_t)dlc) | ((uint32_t)data8[0] << 8) | ((uint32_t)data8[1] << 16) | ((uint32_t)data8[2] << 24);
words[4] = ((uint32_t)data8[3]) | ((uint32_t)data8[4] << 8) | ((uint32_t)data8[5] << 16) | ((uint32_t)data8[6] << 24);
words[5] = ((uint32_t)data8[7]); /* remaining byte + padding zeros */

HAL_FLASH_Unlock();
/* program 6 words */
uint32_t addr = flash_log_write_addr;
for (int i=0;i<6;i++) {

     if (FlashLog_ProgramWord(addr + i*4, words[i]) != HAL_OK) {


         /* programming failed; stop and lock */


         HAL_FLASH_Lock();


         return;


     }

}
HAL_FLASH_Lock();
flash_log_write_addr += FLASH_LOG_ENTRY_SIZE;
}

/* read one entry at given address, return 0 on success, non-zero on invalid/empty */
static int FlashLog_ReadEntry(uint32_t addr,
                           uint32_t *out_timestamp,
                           uint32_t *out_id,
                           uint8_t *out_is_tx,
                           uint8_t *out_dlc,
                           uint8_t out_data[8])
{
if (addr + FLASH_LOG_ENTRY_SIZE > FLASH_LOG_END) return -1;
uint32_t magic = *(uint32_t*)addr;
if (!(magic == FLASH_LOG_MAGIC || magic == FLASH_LOG_MAGIC_B)) return -2;
uint32_t t = *(uint32_t*)(addr + 4);
uint32_t idf = *(uint32_t*)(addr + 8);
uint32_t w3 = *(uint32_t*)(addr + 12);
uint32_t w4 = *(uint32_t*)(addr + 16);
uint32_t w5 = *(uint32_t*)(addr + 20);
*out_timestamp = t;
*out_is_tx = (idf & 0x80000000u) ? 1 : 0;
*out_id = idf & 0x7FFFFFFFu;
out_data[0] = (uint8_t)(w3 >> 8);
out_data[1] = (uint8_t)(w3 >> 16);
out_data[2] = (uint8_t)(w3 >> 24);
*out_dlc = (uint8_t)(w3 & 0xFFu);
out_data[3] = (uint8_t)(w4 & 0xFFu);
out_data[4] = (uint8_t)((w4 >> 8) & 0xFFu);
out_data[5] = (uint8_t)((w4 >> 16) & 0xFFu);
out_data[6] = (uint8_t)((w4 >> 24) & 0xFFu);
out_data[7] = (uint8_t)(w5 & 0xFFu);
return 0;
}

static uint32_t FlashLog_CountSequences(void)
{
    uint32_t addr = FLASH_LOG_START;
    uint32_t seq_count = 0;
    while (addr + FLASH_LOG_ENTRY_SIZE <= FLASH_LOG_END) {
        uint32_t magic = *(uint32_t*)addr;
        if (magic == 0xFFFFFFFFu) {
            break;
        }
        if (magic == FLASH_LOG_MAGIC) {
            seq_count++;
        }
        addr += FLASH_LOG_ENTRY_SIZE;
    }
    return seq_count;
}

/* stream a sequence (sequence index = N): we define one "sequence" as a contiguous set of entries

starting at the first entry and continuing until an empty slot. To support multiple boot sequences,
we write a "session separator" by simply beginning logging at the next free slot. The user requested
using a magic for new sequence: the approach here is that each run produces contiguous entries; the
"sequence index" corresponds to a run number found by scanning the whole area and counting runs.
*/
static void FlashLog_StreamSequence(uint32_t seq_index)
{
/* scan flash, find sequences by looking for entries and sequence boundaries (an empty slot).

    A run is a contiguous group of valid entries. */

uint32_t addr = FLASH_LOG_START;
uint32_t current_seq = 0;
bool in_run = false;
uint32_t run_start = 0;
uint32_t run_end = 0;
while (addr + FLASH_LOG_ENTRY_SIZE <= FLASH_LOG_END) {
     uint32_t magic = *(uint32_t*)addr;
     if (magic == FLASH_LOG_MAGIC) {
         if (!in_run) {
             in_run = true;
             run_start = addr;
         }
         run_end = addr + FLASH_LOG_ENTRY_SIZE;
     } else {
         if (in_run) {
             /* finished a run */
			if (current_seq == seq_index) break;
             current_seq++;
             in_run = false;
         }
     }
     addr += FLASH_LOG_ENTRY_SIZE;
}
/* if we reached EOF while in_run and current_seq==seq_index, run_end already set */
if (!in_run && current_seq != seq_index) {
     FlashLog_UARTSend("SEQ NOT FOUND\r\n");
     return;
}
uint32_t ts_tracker = 0;
/* stream entries from run_start to run_end-ENTRY_SIZE */
for (uint32_t a = run_start; a + FLASH_LOG_ENTRY_SIZE <= FLASH_LOG_END; a += FLASH_LOG_ENTRY_SIZE) {
     uint32_t ts, id;
     uint8_t is_tx, dlc;
     uint8_t data[8];
     if (FlashLog_ReadEntry(a, &ts, &id, &is_tx, &dlc, data) == 0) {
    	 if (ts_tracker <= ts) ts_tracker = ts;
    	 else break; // Exit when monotonic resets (new seq start)
         /* format same as DebugSendCanMsg: "%lu,%08lX,true,Rx,0,%u,%02X,...\n\r" */
         char line[128];
         /* is_tx==1 -> "true,Tx" else "true,Rx" - keep "true" token for compatibility */
         const char *dir = is_tx ? "Tx" : "Rx";
         int l = snprintf(line, sizeof(line), "%lu,%08lX,true,%s,0,%u,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n\r",
                 (unsigned long)ts, (unsigned long)id, dir, (unsigned int)dlc,
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
         if (l > 0) HAL_UART_Transmit(&huart4, (uint8_t*)line, l, 1000);
     } else {
    	 break; // Exit when failed to read entry
     }
}
}

/* Erase the logging area (uses sector erase). This implementation erases the whole configured area

by mass erasing sectors that intersect; if compilation environment provides FLASH_Erase_Sector
helper use it - here we use HAL_FLASHEx_Erase with an erase init structure suitable for F4. */
static void FlashLog_EraseAll(void) {
FLASH_EraseInitTypeDef EraseInitStruct;
uint32_t PageError = 0;
/* We'll perform a page (sector) erase over the configured region.

    NOTE: This may erase more than the strictly configured area depending on sector layout.


    Make sure FLASH_LOG_START and FLASH_LOG_SIZE align to sector boundaries for safety. */
FlashLog_UARTSend("ERASE START\r\n");
HAL_FLASH_Unlock();
/* On F4 use SECTOR-based erase; compute sector from address using helper macros if available.

    For simplicity, perform an erase by addresses: iterate sectors from FLASH_LOG_START to FLASH_LOG_END.


    User should adapt if target uses different flash layout. */
uint32_t addr = FLASH_LOG_START;
while (addr < FLASH_LOG_END) {
     /* Erase one 128KB sector (common for sectors 5+) - if your sectors are different adjust. */


     /* We set sector by address using HAL helper (if available). We'll fall back to PAGE erase if not. */


     EraseInitStruct.TypeErase = FLASH_TYPEERASE_SECTORS;


     /* attempt to calculate sector number - many projects have FLASH_SECTOR_ definitions */
#ifdef FLASH_SECTOR_0


     /* try computing sector index by dividing */


     uint32_t sector = 5 + (addr - 0x08020000) / 0x20000u; /* rough: 128KB per sector */


     EraseInitStruct.Sector = sector;


     EraseInitStruct.NbSectors = 1;


     EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;


     if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {


         HAL_FLASH_Lock();


         FlashLog_UARTSend("ERASE FAILED\r\n");


         return;


     }
#else


     /* If sector macros are not available, attempt to use page erase as fallback */


     EraseInitStruct.TypeErase = FLASH_TYPEERASE_MASSERASE;


     if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) != HAL_OK) {


         HAL_FLASH_Lock();


         FlashLog_UARTSend("ERASE FAILED\r\n");


         return;


     }
     /* mass erase done - break */
     break;
#endif
     addr += 0x20000u; /* skip 128KB */
}
HAL_FLASH_Lock();
/* after erase rescan */
FlashLog_ScanNextWriteAddr();
current_flash_log_magic = FLASH_LOG_MAGIC;
flash_log_rx_started = false;
FlashLog_UARTSend("ERASE OK\r\n");
}

/* UART RX complete callback - accumulate a command line */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
if (huart->Instance != UART4) {
     /* ignore other UARTs */
     return;

}
stop_tx = 1;
char c = (char)uart_rx_char;
if (c == '\r' || c == '\n') {
     if (uart_cmd_idx > 0) {
         uart_cmd_buf[uart_cmd_idx] = '\0';
         /* process command */
         if (strncmp(uart_cmd_buf, "SEQ ", 4) == 0) {
             uint32_t idx = (uint32_t)atoi(&uart_cmd_buf[4]);
             FlashLog_StreamSequence(idx);
         } else if (strcmp(uart_cmd_buf, "COUNT") == 0) {
             char line[64];
             uint32_t count = FlashLog_CountSequences();
             int l = snprintf(line, sizeof(line), "COUNT=%lu\r\n", (unsigned long)count);
             HAL_UART_Transmit(&huart4, (uint8_t*)line, l, 200);
         } else if (strcmp(uart_cmd_buf, "ERASE") == 0) {
             FlashLog_EraseAll();
         } else if (strcmp(uart_cmd_buf, "INFO") == 0) {
             char line[128];
             int l = snprintf(line, sizeof(line), "LOG_START=0x%08lX SIZE=0x%08lX USED=0x%08lX\r\n",
                 (unsigned long)FLASH_LOG_START, (unsigned long)FLASH_LOG_SIZE,
                 (unsigned long)(flash_log_write_addr - FLASH_LOG_START));
             HAL_UART_Transmit(&huart4, (uint8_t*)line, l, 200);
         } else {
             FlashLog_UARTSend("UNKNOWN CMD\r\n");
         }
     }
     uart_cmd_idx = 0;
} else {
     if (uart_cmd_idx < (sizeof(uart_cmd_buf)-1)) {
         uart_cmd_buf[uart_cmd_idx++] = c;
     } else {
         uart_cmd_idx = 0;
     }
}
/* restart RX IT */
HAL_UART_Receive_IT(&huart4, &uart_rx_char, 1);
}

/* -------------------------------------------------------------------- */
