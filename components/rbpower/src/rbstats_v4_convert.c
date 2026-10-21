/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstats_v4_convert.c
 * @brief Pure v3->v4 statistics record converters.
 *
 * No flash/RTOS dependencies, so the conversion rules are unit-testable via the selftest
 * verb (rbpower_logic_test.c). The region-level migrator (rbstats_migrate_v3v4.c) walks the v3 rings and applies these per record.
 */
#include "rbstats_v4_types.h"

#include <string.h>

/* Wall-clock era boundary (2020-09-13): below this a v3 timestamp is uptime-based garbage
 * from a never-synced boot — such records go to the ts=0 unsynced shelf. Same
 * constant as the time write gate (rbstats_time_unsynced_gate). */
#define RBSTATS_WALL_CLOCK_MIN 1600000000u   ///< First wall-clock-era timestamp, epoch s

/* Nominal period length (see rbstats_v4_types.h). */
uint32_t rbstats_v4_nominal_dur(rbpower_period_type_t period)
{
    switch (period) {
        case RBPOWER_PERIOD_HOUR:  return 3600u;
        case RBPOWER_PERIOD_DAY:   return 86400u;
        case RBPOWER_PERIOD_WEEK:  return 604800u;
        case RBPOWER_PERIOD_MONTH: return 2592000u;   /* 30 d nominal */
        case RBPOWER_PERIOD_YEAR:  return 31536000u;  /* 365 d nominal */
        default:                   return 0u;
    }
}

/** @brief Fill the common v4 header of a migrated record from its v3 timestamp. */
static void conv_hdr(uint32_t v3_ts, rbpower_period_type_t period, rbstats_v4_hdr_t *h)
{
    h->dur = rbstats_v4_nominal_dur(period);  /* true coverage unknown for migrated records */
    h->res = 0;
    h->q   = RBSTATS_Q_MIGRATED;
    if (v3_ts == 0xFFFFFFFFu) {
        h->ts = 0;                            /* blank/erased slot — readers skip ts==0 */
        h->q |= RBSTATS_Q_TIME_UNSYNC;
    } else {
        /* ts is PRESERVED as stored — including uptime-era values — so the live read shim
         * (rbstats_flash_read_v4 over a v3 region) is value-transparent to today's history
         * JSON. Shelving uptime-era records to ts=0 is the REGION MIGRATOR's
         * one-time rule: it zeroes ts on records carrying TIME_UNSYNC when rewriting. */
        h->ts = v3_ts;
        if (v3_ts < RBSTATS_WALL_CLOCK_MIN) {
            h->q |= RBSTATS_Q_TIME_UNSYNC;
        }
    }
}

void rbstats_v4_conv_electrical(const rbpower_electrical_transfer_t *in,
                                rbpower_period_type_t period, rbstats_electrical_v4_t *out)
{
    memset(out, 0, sizeof(*out));
    conv_hdr(in->timestamp, period, &out->h);
    out->present = RBSTATS_EL_ALL_V3;         /* v3 stored every field regardless of source */
    out->voltage_rms_v10      = in->voltage_rms_v10;
    out->current_rms_ma       = in->current_rms_ma;
    out->power_active_w10     = in->power_active_w10;
    out->power_reactive_var10 = in->power_reactive_var10;
    out->power_apparent_va10  = in->power_apparent_va10;
    out->power_factor_100     = in->power_factor_100;
    out->frequency_hz10       = in->frequency_hz10;
    out->thd_voltage_100      = in->thd_voltage_100;
    out->thd_current_100      = in->thd_current_100;
    out->voltage_min_v10      = in->voltage_min_v10;
    out->voltage_max_v10      = in->voltage_max_v10;
    out->current_min_ma       = in->current_min_ma;
    out->current_max_ma       = in->current_max_ma;
    out->power_min_w10        = in->power_min_w10;
    out->power_max_w10        = in->power_max_w10;
}

void rbstats_v4_conv_energy(const rbpower_energy_transfer_t *in,
                            rbpower_period_type_t period, rbstats_energy_v4_t *out)
{
    memset(out, 0, sizeof(*out));
    conv_hdr(in->timestamp, period, &out->h);
    /* v3 stored unsigned whole Wh → mWh. 0xFFFFFFFF fields mean an uninitialized slot the
     * ring reader should have skipped; map defensively to 0 + SRC_GAP rather than 4.2 TWh. */
    if (in->energy_consumed_wh != 0xFFFFFFFFu) {
        out->ec_mwh = (int64_t)in->energy_consumed_wh * 1000;
    } else {
        out->h.q |= RBSTATS_Q_SRC_GAP;
    }
    if (in->energy_supplied_wh != 0xFFFFFFFFu) {
        out->es_mwh = (int64_t)in->energy_supplied_wh * 1000;
    } else {
        out->h.q |= RBSTATS_Q_SRC_GAP;
    }
    /* et_in/et_out: no v3 lifetime totalizers exist at record granularity — stay 0; the
     * MIGRATED bit tells the serializer to OMIT et_* keys (contract §2). */
}

void rbstats_v4_conv_econ(const rbpower_economics_transfer_t *in,
                          rbpower_period_type_t period, rbstats_econ_v4_t *out)
{
    memset(out, 0, sizeof(*out));
    conv_hdr(in->timestamp, period, &out->h);
    /* v3 "kopecks" are currency minor units (cents) — same unit, wider type in v4. */
    if (in->cost_consumed_kopecks != 0xFFFFFFFFu) {
        out->c_cents = (int64_t)in->cost_consumed_kopecks;
    } else {
        out->h.q |= RBSTATS_Q_SRC_GAP;
    }
    if (in->revenue_supplied_kopecks != 0xFFFFFFFFu) {
        out->r_cents = (int64_t)in->revenue_supplied_kopecks;
    } else {
        out->h.q |= RBSTATS_Q_SRC_GAP;
    }
    /* plan_id/plan_ver unknown for migrated records → 0. */
}
