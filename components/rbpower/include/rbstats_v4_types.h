/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstats_v4_types.h
 * @brief On-flash statistics record format v4 and the pure v3->v4 record converters.
 *
 * Contract: documents/api/CONTRACT_V4_RC.md §1 (q bits), §2 (record forms), Appendix A
 * (binary sizes: common header ts4+dur4+q2+res1 = 11 B; electrical 41 B, energy 43 B,
 * economics 31 B). `en`/`n` are NEVER stored; they are derived at serialization.
 *
 * v4 is the on-flash format (RBPOWER_RECORD_FORMAT_VERSION 4). The region migrator
 * (rbstats_migrate_v3v4.c) converts v3 records with these functions and registers itself with
 * the migration engine (rbstats_migrate_register()). The history JSON serializes the v4 forms
 * (schema_ver 4: dur, q, res, et_in/et_out, plan_id/plan_ver).
 */
#ifndef RBSTATS_V4_TYPES_H
#define RBSTATS_V4_TYPES_H

#include <stdint.h>
#include "rbstorage_transfer_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- quality bitmask `q` (contract §1; u16) ---- */
#define RBSTATS_Q_PARTIAL      (1u << 0)  ///< dur < nominal period length
#define RBSTATS_Q_TIME_UNSYNC  (1u << 1)  ///< Accumulated under invalid time (survives restamp)
#define RBSTATS_Q_CLOCK_STEP   (1u << 2)  ///< >2 s time step inside the period
#define RBSTATS_Q_SRC_GAP      (1u << 3)  ///< Source absent for part of the period / missing child
#define RBSTATS_Q_RECOMPUTED   (1u << 4)  ///< Recomputed from children after reboot
#define RBSTATS_Q_ESTIMATED    (1u << 5)  ///< Contains estimated components (nominal V etc.)
#define RBSTATS_Q_UNSIGNED_SRC (1u << 6)  ///< Direction-less source (BASIC module)
#define RBSTATS_Q_OVERRANGE    (1u << 7)  ///< Clipping / overload
#define RBSTATS_Q_MIGRATED     (1u << 8)  ///< Carried over from v3: dur=nominal, res=0, no et_*
#define RBSTATS_Q_SRC_RESTART  (1u << 9)  ///< Metering module restarted inside the period (firmware update / reset)
#define RBSTATS_Q_SRC_FW       (1u << 10) ///< Metering module firmware version changed inside the period (implies SRC_RESTART)
#define RBSTATS_Q_REVERSE_LOST (1u << 11) ///< Reverse flow seen on a direction-less (BASIC) source, not in the energy (CT reversed?)

/* ---- electrical present-mask (which JSON keys this record carries; fields_by_source) ---- */
#define RBSTATS_EL_V      (1u << 0)   ///< Voltage present
#define RBSTATS_EL_I      (1u << 1)   ///< Current present
#define RBSTATS_EL_P_ACT  (1u << 2)   ///< Active power present
#define RBSTATS_EL_P_REA  (1u << 3)   ///< Reactive power present (JSON key p_rea, not q_rea)
#define RBSTATS_EL_P_APP  (1u << 4)   ///< Apparent power present
#define RBSTATS_EL_PF     (1u << 5)   ///< Power factor present
#define RBSTATS_EL_F      (1u << 6)   ///< Frequency present
#define RBSTATS_EL_THD_V  (1u << 7)   ///< Voltage THD present
#define RBSTATS_EL_THD_I  (1u << 8)   ///< Current THD present
#define RBSTATS_EL_ENV    (1u << 9)   ///< Min/max envelope (v/i/p) present
/** @brief All electrical fields (v3 stored every field regardless of source). */
#define RBSTATS_EL_ALL_V3 (RBSTATS_EL_V | RBSTATS_EL_I | RBSTATS_EL_P_ACT | RBSTATS_EL_P_REA | \
                           RBSTATS_EL_P_APP | RBSTATS_EL_PF | RBSTATS_EL_F | RBSTATS_EL_THD_V | \
                           RBSTATS_EL_THD_I | RBSTATS_EL_ENV)

/** @brief Common v4 record header (11 bytes, packed). */
typedef struct {
    uint32_t ts;    ///< Period END, epoch s; 0 = unsynced shelf (never in the since-stream)
    uint32_t dur;   ///< Covered duration, s (PARTIAL when < nominal)
    uint16_t q;     ///< RBSTATS_Q_* bitmask; 0 = GOOD
    uint8_t  res;   ///< Source resolution class; 0 = unknown (migrated)
} __attribute__((packed)) rbstats_v4_hdr_t;

/** @brief Electrical v4 record: header 11 + present 2 + v3 payload 28 = 41 bytes (packed). */
typedef struct {
    rbstats_v4_hdr_t h;             ///< Common header
    uint16_t present;               ///< RBSTATS_EL_* mask
    uint16_t voltage_rms_v10;       ///< RMS voltage, V x 10
    uint16_t current_rms_ma;        ///< RMS current, mA
    int16_t  power_active_w10;      ///< Active power, W x 10
    int16_t  power_reactive_var10;  ///< Reactive power, var x 10
    uint16_t power_apparent_va10;   ///< Apparent power, VA x 10
    int8_t   power_factor_100;      ///< Power factor x 100
    uint8_t  frequency_hz10;        ///< Frequency, RAW Hz (uint8; name kept from v3)
    uint16_t thd_voltage_100;       ///< Voltage THD, % x 100
    uint16_t thd_current_100;       ///< Current THD, % x 100
    uint16_t voltage_min_v10;       ///< Min RMS voltage in the period, V x 10
    uint16_t voltage_max_v10;       ///< Max RMS voltage in the period, V x 10
    uint16_t current_min_ma;        ///< Min RMS current in the period, mA
    uint16_t current_max_ma;        ///< Max RMS current in the period, mA
    int16_t  power_min_w10;         ///< Min active power in the period, W x 10
    int16_t  power_max_w10;         ///< Max active power in the period, W x 10
} __attribute__((packed)) rbstats_electrical_v4_t;

/** @brief Energy v4 record: 11 + 4 x 8 = 43 bytes (packed). en = ec - es is derived, NOT stored. */
typedef struct {
    rbstats_v4_hdr_t h;  ///< Common header
    int64_t ec_mwh;      ///< Energy consumed this period, mWh
    int64_t es_mwh;      ///< Energy supplied this period, mWh
    int64_t et_in_mwh;   ///< Lifetime import totalizer AT period end, mWh (0 when MIGRATED)
    int64_t et_out_mwh;  ///< Lifetime export totalizer AT period end, mWh (0 when MIGRATED)
} __attribute__((packed)) rbstats_energy_v4_t;

/** @brief Economics v4 record: 11 + 8 + 8 + 2 + 2 = 31 bytes (packed). n = c - r is derived, NOT stored. */
typedef struct {
    rbstats_v4_hdr_t h;  ///< Common header
    int64_t  c_cents;    ///< Cost of consumption, currency minor units (cents)
    int64_t  r_cents;    ///< Revenue for supply, currency minor units (cents)
    uint16_t plan_id;    ///< Tariff plan ID (0 = unknown/migrated)
    uint16_t plan_ver;   ///< Tariff plan version
} __attribute__((packed)) rbstats_econ_v4_t;

_Static_assert(sizeof(rbstats_v4_hdr_t) == 11, "v4 header must be 11 bytes (contract App. A)");
_Static_assert(sizeof(rbstats_electrical_v4_t) == 41, "v4 electrical must be 41 bytes");
_Static_assert(sizeof(rbstats_energy_v4_t) == 43, "v4 energy must be 43 bytes");
_Static_assert(sizeof(rbstats_econ_v4_t) == 31, "v4 economics must be 31 bytes");

/* ---- v4 retention: records per ring and unit (see sizing note below) ---- */
#define RBSTATS_V4_RECORDS_HOUR  1440   ///< HOUR ring depth: 60 days
#define RBSTATS_V4_RECORDS_DAY    400   ///< DAY ring depth: ~13 months (decouples the MONTH window from the ring size)
#define RBSTATS_V4_RECORDS_WEEK     8   ///< WEEK ring depth
#define RBSTATS_V4_RECORDS_MONTH   36   ///< MONTH ring depth: 3 years
#define RBSTATS_V4_RECORDS_YEAR    10   ///< YEAR ring depth
/* SIZING NOTE (sector-granular recompute of contract Appendix A): with 4 KB sector rounding and
 * 1 wrap-reserve sector per ring this retention costs 80 sectors + 1 header = 324 KB per unit,
 * i.e. ELEVEN full-depth units on the 3.69 MB STATS region (not the 16-17 of a record-byte
 * estimate). More units need one of: HOUR 1440->720 (-80 KB -> 15 units), per-unit depth
 * tiering (enabled_periods_mask is already per unit), or a bigger flash chip. */

/**
 * @brief Write-path metadata of one record set.
 *
 * Carried from the finalize site to the flash layer (and through the pending queue of
 * rbstats_migrate.h) and stored in the v4 record headers/bodies.
 */
typedef struct {
    uint32_t dur;        ///< Covered duration, s (sum of child dur for DAY+)
    uint16_t q;          ///< RBSTATS_Q_* bitmask
    uint8_t  res;        ///< Source resolution class (1 = native quarter resolution)
    uint16_t plan_id;    ///< Active tariff plan at period end (0 = none)
    uint16_t plan_ver;   ///< Active tariff plan version
    int64_t  et_in_mwh;  ///< Lifetime import totalizer at period end, mWh
    int64_t  et_out_mwh; ///< Lifetime export totalizer at period end, mWh
    /* Precise period values for the v4 record body (the v3 transfer structs truncate to whole
     * Wh / carry rounded cents). When `precise` is set the v4 writer uses these instead of
     * re-deriving from the transfer structs. */
    int64_t  ec_mwh;     ///< Precise energy consumed in the period, mWh (used when @c precise)
    int64_t  es_mwh;     ///< Precise energy supplied in the period, mWh (used when @c precise)
    int64_t  c_cents;    ///< Precise cost of consumption, currency cents (used when @c precise)
    int64_t  r_cents;    ///< Precise revenue for supply, currency cents (used when @c precise)
    bool     precise;    ///< true: the ec/es/c/r fields above are valid and take precedence
} rbstats_v4_meta_t;

/**
 * @brief Nominal period length (dur of MIGRATED records; reference for the PARTIAL threshold).
 * @param[in] period Period type.
 * @return Nominal length, s (MONTH = 30 d, YEAR = 365 d); 0 for periods below HOUR or unknown.
 */
uint32_t rbstats_v4_nominal_dur(rbpower_period_type_t period);

/* ---- pure v3->v4 record converters (migrator building blocks; covered by the selftest) ----
 * Common header rule: dur = nominal (true coverage unknown); q |= MIGRATED; res = 0. ts is kept as
 * stored; a value below the wall-clock era (< 1.6e9, uptime-based) additionally sets TIME_UNSYNC
 * (the region migrator moves such records to the ts=0 unsynced shelf), and an erased ts
 * (0xFFFFFFFF) becomes ts=0 + TIME_UNSYNC. Pass only records already validated by the ring reader. */
/**
 * @brief Convert a v3 electrical record to v4.
 * @param[in]  in     v3 electrical record.
 * @param[in]  period Period type of the ring the record comes from.
 * @param[out] out    v4 record (fully overwritten).
 */
void rbstats_v4_conv_electrical(const rbpower_electrical_transfer_t *in,
                                rbpower_period_type_t period, rbstats_electrical_v4_t *out);
/**
 * @brief Convert a v3 energy record to v4 (Wh -> mWh; et_* stay 0).
 *
 * An erased (0xFFFFFFFF) energy field maps to 0 and sets RBSTATS_Q_SRC_GAP.
 *
 * @param[in]  in     v3 energy record.
 * @param[in]  period Period type of the ring the record comes from.
 * @param[out] out    v4 record (fully overwritten).
 */
void rbstats_v4_conv_energy(const rbpower_energy_transfer_t *in,
                            rbpower_period_type_t period, rbstats_energy_v4_t *out);
/**
 * @brief Convert a v3 economics record to v4 (minor units kept; plan_id/plan_ver = 0).
 *
 * An erased (0xFFFFFFFF) amount maps to 0 and sets RBSTATS_Q_SRC_GAP.
 *
 * @param[in]  in     v3 economics record.
 * @param[in]  period Period type of the ring the record comes from.
 * @param[out] out    v4 record (fully overwritten).
 */
void rbstats_v4_conv_econ(const rbpower_economics_transfer_t *in,
                          rbpower_period_type_t period, rbstats_econ_v4_t *out);

#ifdef __cplusplus
}
#endif
#endif /* RBSTATS_V4_TYPES_H */
