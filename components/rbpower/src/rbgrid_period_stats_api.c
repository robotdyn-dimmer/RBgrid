/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_period_stats_api.c
 * @brief Period statistics history API (rbgrid_get_period_history()).
 *
 * Reads aggregated statistics (HOUR, DAY, WEEK, MONTH, YEAR) from flash as v4 records and
 * converts them into rbpower_period_statistics_t rows.
 */

#include "rbgrid_config_api.h"
#include "rbextflash_operations.h"
#include "rbstorage_transfer_types.h"
#include "rbstats_v4_types.h"
#include "rbhome_types.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "RBGRID_PERIOD_API";

/**
 * @brief Map rbpower_stat_period_type_t to the flash rbpower_period_t (falls back to HOUR).
 */
static rbpower_period_t map_stat_period_to_flash_period(rbpower_stat_period_type_t stat_period)
{
    switch (stat_period) {
        case RBPOWER_STAT_PERIOD_1HOUR:  return RBPOWER_PERIOD_HOUR;
        case RBPOWER_STAT_PERIOD_DAILY:  return RBPOWER_PERIOD_DAY;
        case RBPOWER_STAT_PERIOD_WEEKLY: return RBPOWER_PERIOD_WEEK;
        case RBPOWER_STAT_PERIOD_MONTHLY:return RBPOWER_PERIOD_MONTH;
        case RBPOWER_STAT_PERIOD_YEARLY: return RBPOWER_PERIOD_YEAR;
        default:
            ESP_LOGE(TAG, "Invalid stat_period: %d", stat_period);
            return RBPOWER_PERIOD_HOUR; // Fallback
    }
}

/**
 * @brief Fill the electrical fields of a statistics row from a v4 electrical record.
 */
static void populate_electrical_data(rbpower_period_statistics_t* stats,
                                     const rbstats_electrical_v4_t* elec)
{
    if (!stats || !elec) return;

    // Electrical data
    stats->voltage_rms = (float)elec->voltage_rms_v10 / 10.0f;
    stats->current_rms = (float)elec->current_rms_ma / 1000.0f;
    stats->power_active_avg = (float)elec->power_active_w10 / 10.0f;
    stats->power_reactive_avg = (float)elec->power_reactive_var10 / 10.0f;
    stats->power_apparent_avg = (float)elec->power_apparent_va10 / 10.0f;
    stats->power_factor_avg = (float)elec->power_factor_100 / 100.0f;
    stats->frequency_avg = (float)elec->frequency_hz10;   // field holds RAW Hz (uint8, despite the _hz10 name) — no /10

    // Quality indicators
    stats->power_quality.thd_voltage_avg = (float)elec->thd_voltage_100 / 100.0f;
    stats->power_quality.thd_current_avg = (float)elec->thd_current_100 / 100.0f;

    // Timestamp
    stats->period_start_us = (int64_t)elec->h.ts * 1000000LL; // sec -> us
}

/**
 * @brief Fill the energy fields (Wh) and the v4 dur/q metadata of a row from a v4 energy record.
 */
static void populate_energy_data(rbpower_period_statistics_t* stats,
                                 const rbstats_energy_v4_t* energy)
{
    if (!stats || !energy) return;

    stats->energy_period.consumed_wh = (float)((double)energy->ec_mwh / 1000.0);
    stats->energy_period.supplied_wh = (float)((double)energy->es_mwh / 1000.0);
    stats->energy_period.net_wh = stats->energy_period.consumed_wh - stats->energy_period.supplied_wh;
    // v4 record metadata travels with the result (picked up by the history JSON).
    stats->v4_dur_s = energy->h.dur;
    stats->v4_q     = energy->h.q;
}

/**
 * @brief Fill the cost/revenue fields of a row from a v4 economics record (cents -> currency units).
 */
static void populate_economics_data(rbpower_period_statistics_t* stats,
                                    const rbstats_econ_v4_t* econ)
{
    if (!stats || !econ) return;

    // Convert minor units (cents) to major currency units (rbgrid_snapshot_base_t has no net_cost field)
    stats->result_base.cost_consumed = (float)((double)econ->c_cents / 100.0);
    stats->result_base.revenue_supplied = (float)((double)econ->r_cents / 100.0);
    // net_cost is calculated on-demand: cost_consumed - revenue_supplied
}

/* ================================================================================================
 * PUBLIC API
 * ================================================================================================ */

/*
 * Read period history rows (declared and documented in rbgrid_config_api.h).
 */
rbgrid_err_t rbgrid_get_period_history(
    rbpower_unit_id_t unit_id,
    rbpower_stat_period_type_t period,
    uint32_t start_index,
    uint32_t count,
    rbpower_period_statistics_t* stats_out,
    uint32_t* actual_count_out)
{
    // === VALIDATION ===
    if (!stats_out) {
        ESP_LOGE(TAG, "stats_out is NULL");
        return RBGRID_ERR_INVALID_ARG;
    }

    if (count == 0) {
        if (actual_count_out) {
            *actual_count_out = 0;
        }
        return RBGRID_OK;
    }

    // Map the period type
    rbpower_period_t flash_period = map_stat_period_to_flash_period(period);

    ESP_LOGI(TAG, "Reading period history: unit=0x%04X, period=%d, start=%lu, count=%lu",
             unit_id, period, (unsigned long)start_index, (unsigned long)count);

    // === ALLOCATE TEMPORARY BUFFERS ===
    rbstats_electrical_v4_t* elec_data = (rbstats_electrical_v4_t*)malloc(count * sizeof(rbstats_electrical_v4_t));
    rbstats_energy_v4_t* energy_data = (rbstats_energy_v4_t*)malloc(count * sizeof(rbstats_energy_v4_t));
    rbstats_econ_v4_t* econ_data = (rbstats_econ_v4_t*)malloc(count * sizeof(rbstats_econ_v4_t));

    if (!elec_data || !energy_data || !econ_data) {
        ESP_LOGE(TAG, "Failed to allocate temporary buffers");
        if (elec_data) free(elec_data);
        if (energy_data) free(energy_data);
        if (econ_data) free(econ_data);
        return RBGRID_ERR_NO_MEMORY;
    }

    // Clear the output
    memset(stats_out, 0, count * sizeof(rbpower_period_statistics_t));

    // === READ FROM FLASH ===
    uint32_t elec_read = 0;
    uint32_t energy_read = 0;
    uint32_t econ_read = 0;

    // 1. ELECTRICAL records
    esp_err_t ret = rbstats_flash_read_v4(unit_id, flash_period, RBPOWER_DATA_TYPE_ELECTRICAL,
                                                start_index, count, elec_data, &elec_read);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ELECTRICAL data: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Read %lu ELECTRICAL records", (unsigned long)elec_read);
    }

    // 2. ENERGY records
    ret = rbstats_flash_read_v4(unit_id, flash_period, RBPOWER_DATA_TYPE_ENERGY,
                                      start_index, count, energy_data, &energy_read);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ENERGY data: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Read %lu ENERGY records", (unsigned long)energy_read);
    }

    // 3. ECONOMICS records (may be absent for the YEAR period)
    ret = rbstats_flash_read_v4(unit_id, flash_period, RBPOWER_DATA_TYPE_ECONOMICS,
                                      start_index, count, econ_data, &econ_read);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "ECONOMICS data not available (expected for YEAR period)");
    } else {
        ESP_LOGI(TAG, "Read %lu ECONOMICS records", (unsigned long)econ_read);
    }

    // === MERGE ===
    /* Join the three rings by record ts (each newest-first), not by position: a set is written as three
     * separate records, and one missing write (power cut between them, a write-lock timeout) paired every
     * later row's timestamp with the NEIGHBOUR period's energy and cost, permanently.
     * A record without partners in the other rings yields a row with only its own fields. */
    uint32_t max_records = 0, ie = 0, in = 0, ic = 0;
    while (max_records < count && (ie < elec_read || in < energy_read || ic < econ_read)) {
        uint32_t t = 0;
        if (ie < elec_read   && elec_data[ie].h.ts   > t) t = elec_data[ie].h.ts;
        if (in < energy_read && energy_data[in].h.ts > t) t = energy_data[in].h.ts;
        if (ic < econ_read   && econ_data[ic].h.ts   > t) t = econ_data[ic].h.ts;
        rbpower_period_statistics_t* row = &stats_out[max_records];
        bool any = false;
        if (ie < elec_read   && elec_data[ie].h.ts   == t) { populate_electrical_data(row, &elec_data[ie]);  ie++; any = true; }
        if (in < energy_read && energy_data[in].h.ts == t) { populate_energy_data(row, &energy_data[in]);    in++; any = true; }
        if (ic < econ_read   && econ_data[ic].h.ts   == t) { populate_economics_data(row, &econ_data[ic]);   ic++; any = true; }
        if (!any) break;   /* cannot happen (t is one of the heads); guards a malformed ring */
        row->samples_count = 1; // One aggregated record = 1 sample
        max_records++;
    }

    // === CLEANUP ===
    free(elec_data);
    free(energy_data);
    free(econ_data);

    if (actual_count_out) {
        *actual_count_out = max_records;
    }

    ESP_LOGI(TAG, "Successfully read %lu period records", (unsigned long)max_records);

    return RBGRID_OK;
}
