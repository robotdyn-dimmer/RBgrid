/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_buffer_write.c
 * @brief Implementation of the statistics write API for the flash ring buffers
 *
 * High-level functions that write period statistics records through
 * rbpower_flash_write_record() (rbextflash_operations.c).
 */

#include "rbextflash_buffer_write.h"
#include "rbextflash_operations.h"
#include "rbstorage_transfer_types.h"
#include "rbstats_migrate.h"   /* store gate + pending queue during deferred migration */
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "FLASH_WRITE";

/* ================================================================================================
 * HELPER FUNCTIONS
 * ================================================================================================ */

/*
 * Check whether a period + data-type combination can be written.
 */
bool rbpower_flash_buffer_is_write_supported(
    rbpower_period_type_t period,
    rbpower_data_type_t data_type)
{
    // DIRECTION is written only for HOUR and DAY
    if (data_type == RBPOWER_DATA_TYPE_DIRECTION) {
        return (period == RBPOWER_PERIOD_HOUR || period == RBPOWER_PERIOD_DAY);
    }

    // All other combinations are supported (HOUR, DAY, WEEK, MONTH, YEAR)
    return true;
}

/*
 * Get a string description of a write result.
 */
const char* rbpower_flash_write_result_to_string(esp_err_t result)
{
    switch (result) {
        case ESP_OK: return "Success";
        case ESP_ERR_INVALID_ARG: return "Invalid argument";
        case ESP_ERR_NOT_FOUND: return "Buffer not found";
        case ESP_ERR_NO_MEM: return "Out of memory";
        case ESP_ERR_TIMEOUT: return "Timeout";
        case ESP_ERR_INVALID_STATE: return "Invalid state";
        case ESP_ERR_NOT_SUPPORTED: return "Not supported";
        default: return "Unknown error";
    }
}

/**
 * @brief Internal write helper that also measures the write duration
 */
static esp_err_t write_to_flash_internal(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    rbpower_data_type_t data_type,
    const void* data,
    size_t data_size,
    rbpower_flash_write_result_t* result)
{
    uint64_t start_time = esp_timer_get_time();

    ESP_LOGI(TAG, "write_internal: unit=0x%04X, period=%d, type=%d, size=%u",
             (unsigned)unit_id, (int)period, (int)data_type, (unsigned)data_size);

    // Call the low-level API
    esp_err_t ret = rbpower_flash_write_record(
        unit_id,
        (rbpower_period_t)period,
        data_type,
        data,
        data_size
    );

    uint64_t end_time = esp_timer_get_time();
    uint32_t duration_us = (uint32_t)(end_time - start_time);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "write_internal failed: unit=0x%04X, period=%d, type=%d, error=%s, duration=%lu us",
                 (unsigned)unit_id, (int)period, (int)data_type, esp_err_to_name(ret), (unsigned long)duration_us);
    } else {
        ESP_LOGI(TAG, "write_internal success: unit=0x%04X, period=%d, type=%d, duration=%lu us",
                 (unsigned)unit_id, (int)period, (int)data_type, (unsigned long)duration_us);
    }

    // Fill in the result
    if (result) {
        result->result = ret;
        result->records_written = (ret == ESP_OK) ? 1 : 0;
        result->buffer_index = 0;  // TODO: get the real index from rbextflash_operations
        result->buffer_wrapped = false;  // TODO: get the buffer wrap state
        result->write_duration_us = duration_us;
    }

    return ret;
}

/* ================================================================================================
 * TYPED WRITE FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_flash_buffer_write_electrical(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_electrical_transfer_t* data,
    rbpower_flash_write_result_t* result)
{
    if (!data) {
        ESP_LOGE(TAG, "Electrical data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!rbpower_flash_buffer_is_write_supported(period, RBPOWER_DATA_TYPE_ELECTRICAL)) {
        ESP_LOGD(TAG, "Electrical write not supported for period %d", period);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Writing ELECTRICAL: unit=0x%04X, period=%d, timestamp=%lu",
             (unsigned)unit_id, (int)period, (unsigned long)data->timestamp);

    return write_to_flash_internal(
        unit_id,
        period,
        RBPOWER_DATA_TYPE_ELECTRICAL,
        data,
        sizeof(rbpower_electrical_transfer_t),
        result
    );
}

esp_err_t rbpower_flash_buffer_write_energy(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_energy_transfer_t* data,
    rbpower_flash_write_result_t* result)
{
    if (!data) {
        ESP_LOGE(TAG, "Energy data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!rbpower_flash_buffer_is_write_supported(period, RBPOWER_DATA_TYPE_ENERGY)) {
        ESP_LOGD(TAG, "Energy write not supported for period %d", period);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Writing ENERGY: unit=0x%04X, period=%d, timestamp=%lu",
             (unsigned)unit_id, (int)period, (unsigned long)data->timestamp);

    return write_to_flash_internal(
        unit_id,
        period,
        RBPOWER_DATA_TYPE_ENERGY,
        data,
        sizeof(rbpower_energy_transfer_t),
        result
    );
}

esp_err_t rbpower_flash_buffer_write_economics(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_economics_transfer_t* data,
    rbpower_flash_write_result_t* result)
{
    if (!data) {
        ESP_LOGE(TAG, "Economics data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!rbpower_flash_buffer_is_write_supported(period, RBPOWER_DATA_TYPE_ECONOMICS)) {
        ESP_LOGD(TAG, "Economics write not supported for period %d", period);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Writing ECONOMICS: unit=0x%04X, period=%d, timestamp=%lu",
             (unsigned)unit_id, (int)period, (unsigned long)data->timestamp);

    return write_to_flash_internal(
        unit_id,
        period,
        RBPOWER_DATA_TYPE_ECONOMICS,
        data,
        sizeof(rbpower_economics_transfer_t),
        result
    );
}

esp_err_t rbpower_flash_buffer_write_quality(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_quality_transfer_t* data,
    rbpower_flash_write_result_t* result)
{
    if (!data) {
        ESP_LOGE(TAG, "Quality data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!rbpower_flash_buffer_is_write_supported(period, RBPOWER_DATA_TYPE_QUALITY)) {
        ESP_LOGD(TAG, "Quality write not supported for period %d", period);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Writing QUALITY: unit=0x%04X, period=%d, timestamp=%lu",
             (unsigned)unit_id, (int)period, (unsigned long)data->timestamp);

    return write_to_flash_internal(
        unit_id,
        period,
        RBPOWER_DATA_TYPE_QUALITY,
        data,
        sizeof(rbpower_quality_transfer_t),
        result
    );
}

esp_err_t rbpower_flash_buffer_write_direction(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_direction_transfer_t* data,
    rbpower_flash_write_result_t* result)
{
    if (!data) {
        ESP_LOGE(TAG, "Direction data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // DIRECTION is written only for HOUR and DAY
    if (period != RBPOWER_PERIOD_HOUR && period != RBPOWER_PERIOD_DAY) {
        ESP_LOGW(TAG, "Direction data only supported for HOUR and DAY periods, got %d", (int)period);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Writing DIRECTION: unit=0x%04X, period=%d, timestamp=%lu",
             (unsigned)unit_id, (int)period, (unsigned long)data->timestamp);

    return write_to_flash_internal(
        unit_id,
        period,
        RBPOWER_DATA_TYPE_DIRECTION,
        data,
        sizeof(rbpower_direction_transfer_t),
        result
    );
}

/* ================================================================================================
 * BATCH WRITE
 * ================================================================================================ */

esp_err_t rbpower_flash_buffer_write_basic_set(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_electrical_transfer_t* electrical,
    const rbpower_energy_transfer_t* energy,
    const rbpower_economics_transfer_t* economics)
{
    return rbpower_flash_buffer_write_record_set(unit_id, period, electrical, energy, economics, NULL);
}

esp_err_t rbpower_flash_buffer_write_record_set(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_electrical_transfer_t* electrical,
    const rbpower_energy_transfer_t* energy,
    const rbpower_economics_transfer_t* economics,
    const rbstats_v4_meta_t* meta)
{
    esp_err_t ret;

    // Check input data
    if (!electrical || !energy || !economics) {
        ESP_LOGE(TAG, "NULL data in basic set write");
        return ESP_ERR_INVALID_ARG;
    }

    // Store gate: finalized results are NOT lost and NOT written badly. In two cases they land
    // in the PSRAM pending queue instead of flash:
    //  (a) while a deferred format migration owns the region — the queue is flushed after the
    //      migration completes;
    //  (b) while the wall clock has never synced (the records carry uptime timestamps) — on the
    //      first SNTP sync the uptime timestamps are re-stamped to epoch and the queue is flushed.
    if (rbstats_migrate_state() != RBSTATS_STORE_READY || rbstats_time_unsynced_gate()) {
        return rbstats_pending_enqueue(unit_id, period, electrical, energy, economics, meta);
    }

#if RBPOWER_RECORD_FORMAT_VERSION >= 4
    // On-flash format is v4: assemble v4 records from (transfer struct + meta) and write
    // them at the v4 strides. Fresh writes: the v4 converters fill the payload, then the header
    // is REBUILT (they mark MIGRATED, which is wrong for live records), and precise meta values
    // replace the whole-Wh / rounded-cent transfer fields when provided.
    {
        rbstats_electrical_v4_t ev4;
        rbstats_energy_v4_t     nv4;
        rbstats_econ_v4_t       cv4;
        rbstats_v4_conv_electrical(electrical, period, &ev4);
        rbstats_v4_conv_energy(energy, period, &nv4);
        rbstats_v4_conv_econ(economics, period, &cv4);

        rbstats_v4_hdr_t h;
        h.ts  = electrical->timestamp;
        h.dur = (meta && meta->dur) ? meta->dur : rbstats_v4_nominal_dur(period);
        h.q   = meta ? meta->q : 0;
        h.res = meta ? meta->res : 0;
        ev4.h = h;
        nv4.h = h;
        cv4.h = h;
        if (meta) {
            nv4.et_in_mwh  = meta->et_in_mwh;
            nv4.et_out_mwh = meta->et_out_mwh;
            cv4.plan_id    = meta->plan_id;
            cv4.plan_ver   = meta->plan_ver;
            if (meta->precise) {
                nv4.ec_mwh  = meta->ec_mwh;
                nv4.es_mwh  = meta->es_mwh;
                cv4.c_cents = meta->c_cents;
                cv4.r_cents = meta->r_cents;
            }
        }

        ESP_LOGI(TAG, "Writing v4 set: unit=0x%04X, period=%d dur=%lu q=0x%X",
                 (unsigned)unit_id, (int)period, (unsigned long)h.dur, (unsigned)h.q);
        ret = rbpower_flash_write_record(unit_id, period, RBPOWER_DATA_TYPE_ELECTRICAL, &ev4, sizeof(ev4));
        if (ret != ESP_OK) { ESP_LOGE(TAG, "v4 ELECTRICAL write failed: %s", esp_err_to_name(ret)); return ret; }
        ret = rbpower_flash_write_record(unit_id, period, RBPOWER_DATA_TYPE_ENERGY, &nv4, sizeof(nv4));
        if (ret != ESP_OK) { ESP_LOGE(TAG, "v4 ENERGY write failed: %s", esp_err_to_name(ret)); return ret; }
        ret = rbpower_flash_write_record(unit_id, period, RBPOWER_DATA_TYPE_ECONOMICS, &cv4, sizeof(cv4));
        if (ret != ESP_OK) { ESP_LOGE(TAG, "v4 ECONOMICS write failed: %s", esp_err_to_name(ret)); return ret; }
        return ESP_OK;
    }
#else
    // v3: metadata is plumbed to this point but NOT serialized — the raw transfer structs below
    // ARE the on-flash records in the v3 format.
    (void)meta;
#endif

    ESP_LOGI(TAG, "Writing basic set: unit=0x%04X, period=%d", (unsigned)unit_id, (int)period);

    // Write ELECTRICAL
    ret = rbpower_flash_buffer_write_electrical(unit_id, period, electrical, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write ELECTRICAL: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write ENERGY
    ret = rbpower_flash_buffer_write_energy(unit_id, period, energy, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write ENERGY: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write ECONOMICS
    ret = rbpower_flash_buffer_write_economics(unit_id, period, economics, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write ECONOMICS: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Basic set written successfully");
    return ESP_OK;
}

esp_err_t rbpower_flash_buffer_write_extended_set(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period,
    const rbpower_electrical_transfer_t* electrical,
    const rbpower_energy_transfer_t* energy,
    const rbpower_economics_transfer_t* economics,
    const rbpower_quality_transfer_t* quality,
    const rbpower_direction_transfer_t* direction)
{
    esp_err_t ret;

    // Check mandatory data
    if (!electrical || !energy || !economics) {
        ESP_LOGE(TAG, "NULL mandatory data in extended set write");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Writing extended set: unit=0x%04X, period=%d", (unsigned)unit_id, (int)period);

    // Write the basic set
    ret = rbpower_flash_buffer_write_basic_set(unit_id, period, electrical, energy, economics);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write basic set: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write QUALITY if provided
    if (quality) {
        ret = rbpower_flash_buffer_write_quality(unit_id, period, quality, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write QUALITY: %s", esp_err_to_name(ret));
            // Do not abort
        }
    }

    // Write DIRECTION if provided (HOUR and DAY only)
    if (direction) {
        if (period == RBPOWER_PERIOD_HOUR || period == RBPOWER_PERIOD_DAY) {
            ret = rbpower_flash_buffer_write_direction(unit_id, period, direction, NULL);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to write DIRECTION: %s", esp_err_to_name(ret));
                // Do not abort
            }
        } else {
            ESP_LOGD(TAG, "Skipping DIRECTION write for period %d (not HOUR/DAY)", (int)period);
        }
    }

    ESP_LOGI(TAG, "Extended set written successfully");
    return ESP_OK;
}
