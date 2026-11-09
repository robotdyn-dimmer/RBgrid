/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpeak.h
 * @brief Import peak accumulators at the grid input, the base for capacity / peak tariffs (backlog C1, dynamic
 *        tariffs P4).
 *
 * Metering only: nothing here changes money. Per billing cycle (local time, starting on `cycle_day`, default 1):
 *  - the highest 15-min mean import power (BE "kwartierpiek"),
 *  - the highest hourly mean import power,
 *  - the highest hourly mean per day, so the "mean of the 3 highest days" (NO "kapasitetsledd") can be read.
 * The input is the import energy of every MAIN_SUPPLY point per closed 15-min period, summed over the points
 * (three phases = one peak). The previous cycle's figures are kept for reading.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Load the state from NVS. Called by rbdynprice_init(). */
void rbpeak_init(void);

/**
 * @brief Add one MAIN_SUPPLY point's energy for the 15-min period ending at q_end_ts. Calls for the same period
 *        are summed; the period is finalized when a later period arrives or by rbpeak_tick().
 * @param q_end_ts   Period end (UTC s), a 15-min boundary.
 * @param import_wh  Imported energy (Wh); export (negative) counts as 0.
 */
void rbpeak_add_period(uint32_t q_end_ts, float import_wh);

/** @brief Finalize a pending period that no point will add to any more (called every ~30 s). */
void rbpeak_tick(uint32_t now);

/** @brief `get peaks`. Caller deletes. */
cJSON *rbpeak_json(void);

/** @brief `set peaks`: {"cycle_day":1..28} (billing cycle start day) and/or {"reset":true}. */
esp_err_t rbpeak_set(const cJSON *data, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
