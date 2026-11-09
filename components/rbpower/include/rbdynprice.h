/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbdynprice.h
 * @brief Dynamic (spot) energy prices: providers, price stores, the per-period price, fetch schedule.
 *
 * The hub owns the whole loop (documents/architecture/DYNAMIC_TARIFFS_DESIGN.md). A user picks a primary and an
 * optional backup provider from one catalogue: network providers the hub fetches itself (Energy-Charts in P1) and
 * Home Assistant (local; it pushes with `set price`). Each provider has its own price store (≤ 200 entries,
 * UTC from/until). Per 15-min period: a primary entry if one covers it, else a backup entry, else the tariff plan.
 *
 * A stored value is the provider's price in currency/kWh. Its class decides what it means:
 *  - spot:   energy rate = price × factor + markup (+ the zone rate when zones stack); the plan's per-kWh fees and VAT
 *            are added on top;
 *  - all_in: the price already is what the user pays per kWh; nothing is added.
 * Floors (optional) cut the final buy / sell. Negative prices are real values.
 *
 * All functions are thread-safe (one spinlock around the stores, one mutex around the config).
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

#define RBDP_STORE_MAX 200          /**< Entries per provider store: 2 days of 15-min prices + DST */

/** Where the price of a period came from (stable codes, see rbdynprice_prov_code()). */
typedef enum {
    RBDP_PROV_PLAN = 0,             /**< No external price: the tariff plan's zone */
    RBDP_PROV_SOURCE,               /**< The primary provider (network) */
    RBDP_PROV_BACKUP,               /**< The backup provider (network) */
    RBDP_PROV_HA,                   /**< Home Assistant push (primary or backup role) */
} rbdp_prov_t;

/** The price in force for one instant. */
typedef struct {
    bool        covered;            /**< An external price covers the instant (false: buy/sell = plan, all_in false) */
    bool        backup;             /**< The backup provider supplied it */
    bool        hourly_mean;        /**< Mean of the hour's 15-min prices (contract switch) */
    rbdp_prov_t prov;               /**< Provenance */
    float       raw;                /**< Provider price, currency/kWh (spot or all-in, per class) */
    float       buy;                /**< Final buy price, all-in (fees + VAT included), currency/kWh */
    float       sell;               /**< Final sell price, currency/kWh */
    uint32_t    until;              /**< End of the covering entry (UTC s); 0 if not covered */
} rbdp_price_t;

/**
 * @brief Load the config and the stored prices (NVS) and start the fetch task. Call once, after NVS and the
 *        tariff system are up.
 */
void rbdynprice_init(void);

/**
 * @brief The price for an instant.
 * @param ts         Instant (UTC s), e.g. a 15-min period's midpoint.
 * @param zone_buy   The plan zone's energy rate at ts (currency/kWh, before fees/VAT).
 * @param zone_sell  The plan zone's sell rate at ts.
 * @param[out] out   Result. When not covered: buy = zone_buy with fees+VAT, sell = zone_sell, covered = false.
 * @return true if an external price covers ts.
 */
bool rbdynprice_resolve(uint32_t ts, float zone_buy, float zone_sell, rbdp_price_t *out);

/* ---- Home Assistant store (`set price`) ---------------------------------------------------------- */

/**
 * @brief Store one pushed price for [from, until). Same `from` replaces; entries that ended over an hour ago drop.
 * @param buy   Price, currency/kWh (class = the HA provider's class in the config). May be negative.
 * @param sell  Sell price, currency/kWh; NAN = none (the plan's / formula's sell applies).
 * @return ESP_ERR_INVALID_ARG on |price| > 100 or until ≤ from.
 */
esp_err_t rbdynprice_ha_put(float buy, float sell, uint32_t from, uint32_t until);
/** @brief Drop every pushed price. */
void rbdynprice_ha_clear(void);
/** @brief Declare that this build talks to Home Assistant (ESPHome): the HA provider is offered. */
void rbdynprice_set_ha_present(bool present);

/* ---- State --------------------------------------------------------------------------------------- */

/** @brief Stable code of the state now: ok | backup | today_only | stale | plan_fallback | disabled. */
const char *rbdynprice_state(void);
/** @brief The provider in use now: its id, or "plan". Sets *is_backup when the backup covers now. */
const char *rbdynprice_source_now(bool *is_backup);
/** @brief End of the continuous external coverage from now (UTC s); 0 if none. */
uint32_t rbdynprice_valid_until(void);
/** @brief Stable code of a provenance. */
const char *rbdynprice_prov_code(rbdp_prov_t p);
/** @brief Bump on every change of stored prices or state; hosts poll it to publish. */
uint32_t rbdynprice_generation(void);

/* ---- JSON API (rbgrid_cmd, MQTT, HTTP: one implementation) --------------------------------------- */

/** @brief `get dynprice`: config (no tokens) + status + providers[]. Caller deletes. */
cJSON *rbdynprice_config_json(void);

/**
 * @brief `set dynprice`: merge the given fields into the config, validate, save, reschedule fetches.
 * @param data       Request object.
 * @param err_field  Out: the rejected field name on error (may be "").
 * @param err_msg    Out: message on error.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG with err_field/err_msg set.
 */
esp_err_t rbdynprice_set_config(const cJSON *data, char *err_field, size_t field_len, char *err_msg, size_t msg_len);

/**
 * @brief `get price`: state, the HA store entries (`prices`, legacy shape) and the resolved price per 15-min period
 *        over [from, until) (`resolved`, ≤ 200). from = 0 → now − 1 h; until = 0 → end of the stored range.
 */
cJSON *rbdynprice_price_json(uint32_t from, uint32_t until);

/**
 * @brief `get price_log`: the price applied to each closed 15-min period, [from, until) (default: the last 24 h),
 *        `{"type":"price_log","currency","cols":["from","buy","sell","prov"],"rows":[[…]],"more"}`, ≤ 400 rows.
 *        prov: source | backup | ha_push | plan, "+hourly_mean" appended when the hourly mean applied.
 */
cJSON *rbdynprice_price_log_json(uint32_t from, uint32_t until);

#ifdef __cplusplus
}
#endif
