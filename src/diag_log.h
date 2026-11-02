/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Xiegu X6100 - diagnostic log (1KO125, Hany El Imam 2026)
 *
 * /mnt/ptt.log on the DATA partition, one line per event, UTC time: every
 * TX command of the APP with its origin (front PTT key, CAT USB/Bluetooth,
 * CAT LAN, FT8, voice messages, ATU, SWR scan...), and every RX/TX change
 * reported by the BASE, marked "NOT COMMANDED" when the APP had not asked
 * for TX (microphone PTT, rear PTT jack, or the BASE itself). CW is left
 * out: the CW key and the TX of the CW modes.
 *
 * The file is limited to DIAG_MAX_BYTES; the previous part is kept as
 * ptt.log.old. At most DIAG_MAX_PER_S lines a second are written; the next
 * line says how many were skipped. The lines are written by a thread of their
 * own, so the caller never waits for the SD card.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Writes the start line. Call first thing in main(). */
void diag_init(void);
/* Name of the origin of the TX commands of the calling thread
 * ("CAT LAN", "FT8"...); a static string. Returns the previous one (NULL:
 * none), to put back after a single command. */
const char *diag_set_source(const char *name);
/* Copies the calling thread's origin ("thread <id>" if it has none). */
void diag_source_copy(char *dst, size_t size);
void diag_ptt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
