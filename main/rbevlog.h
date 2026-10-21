/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbevlog.h
 * @brief On-hub event log, readable over HTTP.
 *
 * A hub on a live installation is reached over Wi-Fi/OTA only (no USB console), so its diagnostics must be
 * readable remotely. Every rbpower diag line (QTR/HOUR/gap/boot/poll/FAIL...) lands in a PSRAM ring;
 * event and FAIL lines are also appended to the `logs` LittleFS partition (rotated files that survive
 * reboots and OTA). Read over HTTP: GET /api/v1/system/events. Remote console verbs (debug builds with
 * CONFIG_RBGRID_BENCH_FULL or CONFIG_RBGRID_DEBUG_BRIDGE): POST /api/v1/debug/cmd.
 *
 * Call order at boot: rbevlog_init() → rbpower storage init → rbevlog_attach_storage() → web server start →
 * rbevlog_register_http(). rbevlog_add() is thread-safe (internal mutex) and may be called from any task,
 * but not from an ISR.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Allocate the ring (PSRAM) and hook the rbpower diag log sink. Call early (before the first diag line).
 *         On allocation failure the event log stays off. */
void rbevlog_init(void);
/** @brief Mount `logs` (unless rbstorage already did), continue seq/boot numbering from the files, start the
 *         writer task and register a shutdown flush. Call AFTER rbpower storage init (it may mount the
 *         partition itself). Without the partition the log runs RAM-only. */
void rbevlog_attach_storage(void);
/** @brief Append one line outside the diag trace (e.g. Wi-Fi status). Thread-safe; a line is dropped (and
 *         counted) if the lock is not obtained within 50 ms.
 *  @param[in] msg  NUL-terminated text; truncated to 199 bytes, CR/LF replaced by spaces. NULL is ignored. */
void rbevlog_add(const char *msg);
/** @brief Register GET /api/v1/system/events (+ POST /api/v1/debug/cmd on debug builds) on the running web
 *         server. Logs an error if the server has no free URI handler slots. */
void rbevlog_register_http(void);

#ifdef __cplusplus
}
#endif
