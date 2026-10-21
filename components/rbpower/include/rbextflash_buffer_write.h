/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_buffer_write.h
 * @brief API for writing statistics to Flash buffers
 *
 * High-level API for writing statistical data to the sector-based flash ring buffers
 * (internal statistics partition or optional external NOR chip). Each record is written
 * through rbpower_flash_write_record(), which serialises access with its own mutex, so these
 * functions may be called from any task (not from an ISR).
 *
 * Supported periods: HOUR, DAY, WEEKLY, MONTHLY, YEARLY
 * Supported data types: ELECTRICAL, ENERGY, ECONOMICS, QUALITY, DIRECTION
 */

#ifndef RBPOWER_FLASH_BUFFER_WRITE_H
#define RBPOWER_FLASH_BUFFER_WRITE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rbstorage_transfer_types.h"
#include "rbhome_types.h"
#include "rbstats_v4_types.h"   /* rbstats_v4_meta_t (write-path record metadata) */

    /* ================================================================================================
     * WRITE RESULT
     * ================================================================================================ */

    /**
     * @brief Flash buffer write result
     */
    typedef struct
    {
        esp_err_t result;           ///< Operation result code
        uint32_t records_written;   ///< Number of successfully written records
        uint32_t buffer_index;      ///< Buffer index after write (not filled yet: always 0)
        bool buffer_wrapped;        ///< Buffer reached end and wrapped (not filled yet: always false)
        uint32_t write_duration_us; ///< Write operation duration (microseconds)
    } rbpower_flash_write_result_t;

    /* ================================================================================================
     * STATISTICS WRITE API (TYPED FUNCTIONS)
     * ================================================================================================ */

    /**
     * @brief Write electrical data to Flash
     *
     * Writes the transfer struct as-is (raw record) through rbpower_flash_write_record().
     * Unlike rbpower_flash_buffer_write_record_set() it does not build v4 records and does not
     * pass the store gate (pending queue).
     *
     * @param[in] unit_id Unit ID (e.g., RBPOWER_UNIT_ID_MAIN_SUPPLY)
     * @param[in] period Statistics period (HOUR/DAY/WEEKLY/MONTHLY/YEARLY)
     * @param[in] data Pointer to electrical data
     * @param[out] result Write result (can be NULL)
     * @retval ESP_OK                 Record written
     * @retval ESP_ERR_INVALID_ARG    @p data is NULL
     * @retval ESP_ERR_NOT_SUPPORTED  Period/type combination not written to flash
     * @return Any other error from rbpower_flash_write_record()
     *
     * @note Automatically determines buffer by unit_id and period
     * @note Thread-safe (the lower layer holds its mutex)
     */
    esp_err_t rbpower_flash_buffer_write_electrical(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_electrical_transfer_t *data,
        rbpower_flash_write_result_t *result);

    /**
     * @brief Write energy data to Flash
     *
     * Same behaviour as rbpower_flash_buffer_write_electrical().
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Statistics period (HOUR/DAY/WEEKLY/MONTHLY/YEARLY)
     * @param[in] data Pointer to energy data
     * @param[out] result Write result (can be NULL)
     * @retval ESP_OK                 Record written
     * @retval ESP_ERR_INVALID_ARG    @p data is NULL
     * @retval ESP_ERR_NOT_SUPPORTED  Period/type combination not written to flash
     * @return Any other error from rbpower_flash_write_record()
     */
    esp_err_t rbpower_flash_buffer_write_energy(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_energy_transfer_t *data,
        rbpower_flash_write_result_t *result);

    /**
     * @brief Write economics data to Flash
     *
     * Same behaviour as rbpower_flash_buffer_write_electrical().
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Statistics period (HOUR/DAY/WEEKLY/MONTHLY/YEARLY)
     * @param[in] data Pointer to economics data
     * @param[out] result Write result (can be NULL)
     * @retval ESP_OK                 Record written
     * @retval ESP_ERR_INVALID_ARG    @p data is NULL
     * @retval ESP_ERR_NOT_SUPPORTED  Period/type combination not written to flash
     * @return Any other error from rbpower_flash_write_record()
     */
    esp_err_t rbpower_flash_buffer_write_economics(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_economics_transfer_t *data,
        rbpower_flash_write_result_t *result);

    /**
     * @brief Write power quality data to Flash
     *
     * Used only for MAIN_SUPPLY units.
     *
     * @param[in] unit_id Unit ID (typically RBPOWER_UNIT_ID_MAIN_SUPPLY)
     * @param[in] period Statistics period (HOUR/DAY/WEEKLY/MONTHLY/YEARLY)
     * @param[in] data Pointer to quality data
     * @param[out] result Write result (can be NULL)
     * @retval ESP_OK                 Record written
     * @retval ESP_ERR_INVALID_ARG    @p data is NULL
     * @retval ESP_ERR_NOT_SUPPORTED  Period/type combination not written to flash
     * @return Any other error from rbpower_flash_write_record()
     */
    esp_err_t rbpower_flash_buffer_write_quality(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_quality_transfer_t *data,
        rbpower_flash_write_result_t *result);

    /**
     * @brief Write current direction data to Flash
     *
     * Used for bidirectional units (MAIN_SUPPLY, ALT_SUPPLY).
     * Written only for HOUR and DAY periods (not for MONTHLY/YEARLY).
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Statistics period (only HOUR or DAY)
     * @param[in] data Pointer to direction data
     * @param[out] result Write result (can be NULL)
     * @retval ESP_OK                 Record written
     * @retval ESP_ERR_INVALID_ARG    @p data is NULL
     * @retval ESP_ERR_NOT_SUPPORTED  @p period is not HOUR or DAY
     * @return Any other error from rbpower_flash_write_record()
     */
    esp_err_t rbpower_flash_buffer_write_direction(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_direction_transfer_t *data,
        rbpower_flash_write_result_t *result);

    /* ================================================================================================
     * BATCH WRITE
     * ================================================================================================ */

    /**
     * @brief Write complete data set for a period
     *
     * Writes all data types for a given period in one operation.
     * Used for regular units (ZONE, LOAD). Equivalent to
     * rbpower_flash_buffer_write_record_set() with meta = NULL.
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Statistics period
     * @param[in] electrical Electrical data (required)
     * @param[in] energy Energy data (required)
     * @param[in] economics Economics data (required)
     * @return See rbpower_flash_buffer_write_record_set()
     *
     * @note Aborts on first error
     */
    esp_err_t rbpower_flash_buffer_write_basic_set(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_electrical_transfer_t *electrical,
        const rbpower_energy_transfer_t *energy,
        const rbpower_economics_transfer_t *economics);

    /**
     * @brief Write a period record set WITH v4 metadata (dur/q/res/plan/et_*).
     *
     * The canonical write entry: basic_set forwards here with meta=NULL (defaults).
     *
     * Store gate: while a deferred format migration owns the region, or while the wall clock
     * has never been synced, the set is placed in the pending queue (rbstats_pending_enqueue())
     * instead of flash and is written later.
     *
     * With the v4 on-flash format (#RBPOWER_RECORD_FORMAT_VERSION >= 4) the transfer structs
     * are converted to v4 records, the common header is rebuilt from @p meta, and the precise
     * mWh / cent values in @p meta (when meta->precise is set) replace the rounded transfer
     * fields. The three records (ELECTRICAL, ENERGY, ECONOMICS) are written in that order.
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Statistics period
     * @param[in] electrical Electrical data (required)
     * @param[in] energy Energy data (required)
     * @param[in] economics Economics data (required)
     * @param[in] meta Record-set metadata, or NULL for defaults (dur=nominal, q=0, res=0)
     * @retval ESP_OK               All records written, or the set was queued
     * @retval ESP_ERR_INVALID_ARG  A data pointer is NULL
     * @retval ESP_ERR_NO_MEM       The pending queue could not be allocated
     * @return Any other error from rbpower_flash_write_record() (aborts on the first error)
     */
    esp_err_t rbpower_flash_buffer_write_record_set(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_electrical_transfer_t *electrical,
        const rbpower_energy_transfer_t *energy,
        const rbpower_economics_transfer_t *economics,
        const rbstats_v4_meta_t *meta);

    /**
     * @brief Write extended data set for special units
     *
     * Writes all data types including QUALITY and DIRECTION.
     * Used for MAIN_SUPPLY and ALT_SUPPLY.
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Statistics period
     * @param[in] electrical Electrical data (required)
     * @param[in] energy Energy data (required)
     * @param[in] economics Economics data (required)
     * @param[in] quality Quality data (can be NULL)
     * @param[in] direction Direction data (can be NULL, only for HOUR/DAY)
     * @retval ESP_OK               Basic set written (QUALITY/DIRECTION failures are only logged)
     * @retval ESP_ERR_INVALID_ARG  A mandatory data pointer is NULL
     * @return Any error from rbpower_flash_buffer_write_basic_set()
     */
    esp_err_t rbpower_flash_buffer_write_extended_set(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period,
        const rbpower_electrical_transfer_t *electrical,
        const rbpower_energy_transfer_t *energy,
        const rbpower_economics_transfer_t *economics,
        const rbpower_quality_transfer_t *quality,
        const rbpower_direction_transfer_t *direction);

    /* ================================================================================================
     * UTILITY FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Check if write is supported for given period+type combination
     *
     * @param[in] period Statistics period
     * @param[in] data_type Data type
     * @return true if combination is supported (currently false only for DIRECTION outside
     *         HOUR and DAY)
     *
     * @note 3SEC and 15MIN periods are not written to flash by the callers; this function
     *       does not check them
     * @note DIRECTION is written only for HOUR and DAY
     */
    bool rbpower_flash_buffer_is_write_supported(
        rbpower_period_type_t period,
        rbpower_data_type_t data_type);

    /**
     * @brief Get string description of write result
     *
     * @param[in] result Write result code
     * @return Static description string (do not free)
     */
    const char *rbpower_flash_write_result_to_string(esp_err_t result);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_FLASH_BUFFER_WRITE_H
