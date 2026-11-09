/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbactivation.h
 * @brief Price-based activations ("cheap windows", dynamic tariffs P3) and the price notification `price_signal`.
 *
 * The hub only computes and notifies; it controls nothing (operator 2026-11-09). Consumers — HA, the web app,
 * ACRouter — read the windows and decide. All rules act on the all-in buy price of each 15-min period
 * (rbdynprice_resolve with the plan zone underneath), so time-of-use grid fees count.
 *
 * Rule types:
 *  - threshold: active while the all-in buy ≤ on_below; it stays active until the price reaches off_above
 *    (hysteresis);
 *  - cheapest:  the N cheapest 15-min periods (any, or one consecutive block) in a daily local window
 *    [from_h, to_h), which may cross midnight;
 *  - deadline:  N periods of run time before the local hour deadline_h each day (the 24 h window ending there).
 *
 * `price_signal` (≤ 1 KB, schema v1) goes to MQTT `rbgrid/<mac>/price` (retained) at each 15-min boundary and on a
 * change; `get signal` returns the same JSON.
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

#define RBACT_MAX_RULES 8

/** @brief Load the rules (NVS). Called by rbdynprice_init(). */
void rbact_init(void);
/** @brief Re-evaluate the rules, emit window events, publish price_signal when due (called every ~30 s). */
void rbact_tick(uint32_t now);
/** @brief `get rules`: rules + their state now. Caller deletes. */
cJSON *rbact_rules_json(void);
/**
 * @brief `set rules`: {"rules":[…]} replaces all rules; {"signal":{"buy_cheap_below":x|null,"sell_low_below":y|null}}
 *        sets the signal flags. A rejection names the field.
 */
esp_err_t rbact_set(const cJSON *data, char *err_field, size_t field_len, char *err_msg, size_t msg_len);
/** @brief `get signal`: the price_signal JSON now. Caller deletes. */
cJSON *rbact_signal_json(void);
/** @brief Rank of the 15-min period containing ts within its local day by all-in buy (1 = cheapest); 0 if unknown. */
int rbact_rank(uint32_t ts, bool sell, int *of_n);

#ifdef __cplusplus
}
#endif
