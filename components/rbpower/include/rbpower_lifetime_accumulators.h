/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_lifetime_accumulators.h
 * @brief Daily and lifetime energy/cost accumulators with RTC-memory backup.
 *
 * Per-unit accumulation of daily and lifetime energy and cost:
 * - embedded directly into the existing unit structures;
 * - small RAM footprint (separate layouts for regular and supply units);
 * - backed up to RTC slow memory (and an NVS snapshot) so the totals survive resets;
 * - automatic daily reset at local midnight once SNTP time is valid;
 * - graceful degradation when the RTC backup is invalid (counters restart from zero).
 *
 * Cost fields are expressed in the configured tariff currency (major units).
 */

#ifndef RBPOWER_LIFETIME_ACCUMULATORS_H
#define RBPOWER_LIFETIME_ACCUMULATORS_H

#include <stdint.h>
#include <stdbool.h>
#include "rbpower_enums.h"
#include "rbpower_constants.h"

/** @brief Unit identifier (forward declaration, same type as in the unit headers). */
typedef uint16_t rbpower_unit_id_t;

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// LIFETIME ACCUMULATOR DATA STRUCTURES
//=============================================================================

/**
 * @brief Lifetime accumulators of a regular (non-supply) unit.
 *
 * Embedded in rbhome_unit_statistics_t. Regular units normally only consume, but a point with
 * negative power (e.g. a renewable source) also books the supplied side.
 */
typedef struct {
    // === DAILY ACCUMULATORS ===
    float daily_energy_consumed_wh;     ///< Energy consumed since local midnight, Wh
    float daily_cost_consumed;          ///< Cost of consumption since local midnight, tariff currency

    // === LIFETIME ACCUMULATORS ===
    float total_energy_consumed_wh;     ///< Energy consumed over the device lifetime, Wh
    float total_cost_consumed;          ///< Cost of consumption over the device lifetime, tariff currency

    /** Exact lifetime import totalizer, mWh. A double is used because a float in Wh loses small
     *  increments after ~10 MWh, while a double stays exact up to ~9 GWh. Source of et_in in v4 records. */
    double et_in_mwh;

    // Export / generation side. A renewable source or any regular point with negative power must
    // book its supplied energy here, otherwise "energy out today" stays 0. The RTC/NVS image
    // already carries the supplied fields per unit.
    float daily_energy_supplied_wh;     ///< Energy supplied (exported) since local midnight, Wh
    float total_energy_supplied_wh;     ///< Energy supplied (exported) over the device lifetime, Wh
    float daily_cost_supplied;          ///< Revenue for supplied energy since local midnight, tariff currency
    float total_cost_supplied;          ///< Revenue for supplied energy over the device lifetime, tariff currency
    double et_out_mwh;                  ///< Exact lifetime export totalizer, mWh (source of et_out in v4 records)

    // === METADATA ===
    uint32_t last_rtc_sync_timestamp;   ///< Time of the last RTC backup sync, epoch s
    bool rtc_enabled;                   ///< true when this unit is synced to the RTC backup
    uint8_t _padding[3];                ///< Structure alignment padding

} rbpower_lifetime_accumulators_basic_t;

/**
 * @brief Extended lifetime accumulators of a supply unit (consumed and supplied sides).
 *
 * Embedded in rbpower_main_supply_t and rbpower_alternative_supply_t.
 */
typedef struct {
    // === DAILY ACCUMULATORS ===
    float daily_energy_consumed_wh;     ///< Energy consumed (imported) since local midnight, Wh
    float daily_energy_supplied_wh;     ///< Energy supplied (exported) since local midnight, Wh
    float daily_cost_consumed;          ///< Cost of consumption since local midnight, tariff currency
    float daily_cost_supplied;          ///< Revenue for supplied energy since local midnight, tariff currency

    // === LIFETIME ACCUMULATORS ===
    float total_energy_consumed_wh;     ///< Energy consumed over the device lifetime, Wh
    float total_energy_supplied_wh;     ///< Energy supplied over the device lifetime, Wh
    float total_cost_consumed;          ///< Cost of consumption over the device lifetime, tariff currency
    float total_cost_supplied;          ///< Revenue for supplied energy over the device lifetime, tariff currency

    // Exact totalizers (double; source of et_in/et_out in v4 records).
    double et_in_mwh;                   ///< Lifetime import (consumption), mWh
    double et_out_mwh;                  ///< Lifetime export (supply), mWh

    // === METADATA ===
    uint32_t last_rtc_sync_timestamp;   ///< Time of the last RTC backup sync, epoch s
    bool rtc_enabled;                   ///< true when this unit is synced to the RTC backup
    uint8_t _padding[3];                ///< Structure alignment padding

} rbpower_lifetime_accumulators_supply_t;

//=============================================================================
// RTC BACKUP STRUCTURES (persistence)
//=============================================================================

/**
 * @brief Compact per-unit record stored in RTC slow memory (52 bytes).
 */
typedef struct {
    uint16_t unit_id;                   ///< Unit ID
    uint16_t flags;                     ///< Flags: bit0 = RBPOWER_RTC_FLAG_IS_SUPPLY_UNIT, other bits reserved

    // Base data (present for every unit), 16 bytes
    float daily_energy_consumed_wh;     ///< Energy consumed since local midnight, Wh
    float total_energy_consumed_wh;     ///< Lifetime energy consumed, Wh
    float daily_cost_consumed;          ///< Cost of consumption since local midnight, tariff currency
    float total_cost_consumed;          ///< Lifetime cost of consumption, tariff currency

    // Supplied side, 16 bytes
    float daily_energy_supplied_wh;     ///< Energy supplied since local midnight, Wh
    float total_energy_supplied_wh;     ///< Lifetime energy supplied, Wh
    float daily_cost_supplied;          ///< Revenue for supplied energy since local midnight, tariff currency
    float total_cost_supplied;          ///< Lifetime revenue for supplied energy, tariff currency

    // Exact totalizers, 16 bytes. Changing this layout invalidates an existing RTC backup once
    // (CRC mismatch) after a firmware update, which is acceptable.
    double et_in_mwh;                   ///< Lifetime import totalizer, mWh
    double et_out_mwh;                  ///< Lifetime export totalizer, mWh (0 for regular units without export)

} rbpower_rtc_unit_data_t;  // 52 bytes per unit

/**
 * @brief Complete RTC backup image (header + one record per unit, ~3.3 KB).
 *
 * Lives in RTC slow memory (8 KB) and is also written to NVS as a snapshot.
 */
typedef struct {
    // === HEADER ===
    uint32_t magic;                     ///< RBPOWER_LIFETIME_MAGIC ('RBPW', 0x52425057) for validation
    uint32_t timestamp_sntp;            ///< Time of the last save, epoch s
    uint32_t crc32;                     ///< CRC32 of the unit data
    uint16_t units_count;               ///< Number of valid entries in @c units
    uint16_t _reserved;                 ///< Reserved for future extensions

    // === UNIT DATA ===
    rbpower_rtc_unit_data_t units[RBPOWER_MAX_HOME_UNITS];  ///< Per-unit records (52 B x RBPOWER_MAX_HOME_UNITS)

} rbpower_rtc_backup_t;  // 16 + 52 x units bytes (~3.3 KB; RTC slow mem is 8 KB; also the NVS snapshot)

//=============================================================================
// API DATA TYPES
//=============================================================================

/**
 * @brief Full lifetime data of one unit, as returned by rbpower_lifetime_get_unit_data().
 */
typedef struct {
    rbpower_unit_id_t unit_id;          ///< Unit ID
    bool is_supply_unit;                ///< true for a supply unit (main/alternative), false for a regular unit

    // Daily data
    float daily_energy_consumed_wh;     ///< Energy consumed since local midnight, Wh
    float daily_energy_supplied_wh;     ///< Energy supplied since local midnight, Wh (0 for regular units)
    float daily_cost_consumed;          ///< Cost of consumption since local midnight, tariff currency
    float daily_cost_supplied;          ///< Revenue since local midnight, tariff currency (0 for regular units)

    // Lifetime data
    float total_energy_consumed_wh;     ///< Lifetime energy consumed, Wh
    float total_energy_supplied_wh;     ///< Lifetime energy supplied, Wh (0 for regular units)
    float total_cost_consumed;          ///< Lifetime cost of consumption, tariff currency
    float total_cost_supplied;          ///< Lifetime revenue, tariff currency (0 for regular units)

    // Metadata
    uint32_t last_rtc_sync_timestamp;   ///< Time of the last RTC backup sync, epoch s
    bool rtc_enabled;                   ///< true when the unit is synced to the RTC backup

} rbpower_lifetime_unit_data_t;

/**
 * @brief Runtime statistics of the lifetime/RTC subsystem.
 */
typedef struct {
    // Operation counters
    uint32_t total_accumulations;       ///< Number of accumulation calls
    uint32_t total_rtc_syncs;          ///< Number of RTC backup syncs
    uint32_t rtc_sync_errors;          ///< Number of failed RTC backup syncs
    uint32_t daily_resets_count;       ///< Number of daily resets performed

    // Timestamps
    uint64_t last_accumulation_time_us; ///< Time of the last accumulation, µs since boot
    uint64_t last_rtc_sync_time_us;    ///< Time of the last RTC sync, µs since boot
    uint64_t last_daily_reset_time_us; ///< Time of the last daily reset, µs since boot

    // RTC state
    bool rtc_valid;                    ///< true when the RTC backup data is valid
    bool rtc_system_enabled;           ///< true when the RTC backup subsystem is enabled
    uint32_t rtc_backup_size_bytes;    ///< Size of rbpower_rtc_backup_t, bytes

} rbpower_lifetime_stats_t;

//=============================================================================
// CONSTANTS AND DEFINITIONS
//=============================================================================

#define RBPOWER_LIFETIME_MAGIC          0x52425057U    ///< RTC backup magic, 'RBPW'
#define RBPOWER_LIFETIME_RTC_SYNC_INTERVAL_SEC  3      ///< RTC sync interval, s (every 3-second period)
#define RBPOWER_LIFETIME_MAX_UNITS      RBPOWER_MAX_HOME_UNITS ///< Maximum number of units with accumulators

/** @brief rbpower_rtc_unit_data_t.flags bit: the unit is a supply unit. */
#define RBPOWER_RTC_FLAG_IS_SUPPLY_UNIT  (1 << 0)

//=============================================================================
// FORWARD DECLARATIONS
//=============================================================================

struct rbpower_period_statistics;  // Avoids circular header dependencies

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_LIFETIME_ACCUMULATORS_H