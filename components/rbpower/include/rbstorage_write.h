/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_write.h
 * @brief API for writing period records into the per-unit ring-buffer data files.
 *
 * Each (unit, data type, period) has one pre-allocated data file: a
 * ::rbpower_file_header_t followed by `max_records` fixed-size records. A write
 * stores one record at `header.current_index`, advances the index modulo
 * `max_records` (wrapping over the oldest record) and rewrites the header.
 * The data file must already exist (it is created by the storage init code).
 *
 * Calling context: these functions do blocking VFS I/O and must be called from
 * the flash-writer task (or another task that owns storage writes), never from
 * an ISR. They do not take a lock of their own; the caller serialises writes.
 */

#ifndef RBPOWER_STORAGE_WRITE_H
#define RBPOWER_STORAGE_WRITE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "rbstorage_transfer_types.h"
#include "rbhome_types.h"
#include "esp_err.h"

    /* ================================================================================================
     * DATA TYPES FOR WRITING
     * ================================================================================================ */

    /**
     * @brief Write operation result
     */
    typedef struct
    {
        esp_err_t result;            ///< Operation result (same code the function returns)
        uint32_t records_written;    ///< Number of records written (0 or 1)
        uint32_t current_file_index; ///< Next write index in the file after this write (records)
        uint64_t write_timestamp_us; ///< Start of the write, esp_timer time (µs since boot)
        bool file_wrapped;           ///< true if the write index wrapped to 0 (ring buffer overwrote the oldest record)
    } rbpower_write_result_t;

    /* ================================================================================================
     * ELECTRICAL DATA WRITE API
     * ================================================================================================ */

    /**
     * @brief Write one electrical record for a unit (wrapper over rbpower_storage_write_generic_data()).
     *
     * @param[in]  unit_id         Unit ID
     * @param[in]  period_type     Period type (e.g. 1HOUR, DAY)
     * @param[in]  electrical_data Electrical record to write
     * @param[out] result          Write operation result
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG on NULL arguments, otherwise
     *         any code returned by rbpower_storage_write_generic_data()
     */
    esp_err_t rbpower_storage_write_electrical_data(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period_type,
        const rbpower_electrical_transfer_t *electrical_data,
        rbpower_write_result_t *result);

    /**
     * @brief Write one energy record for a unit (wrapper over rbpower_storage_write_generic_data()).
     *
     * @param[in]  unit_id     Unit ID
     * @param[in]  period_type Period type
     * @param[in]  energy_data Energy record to write
     * @param[out] result      Write operation result
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG on NULL arguments, otherwise
     *         any code returned by rbpower_storage_write_generic_data()
     */
    esp_err_t rbpower_storage_write_energy_data(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period_type,
        const rbpower_energy_transfer_t *energy_data,
        rbpower_write_result_t *result);

    /**
     * @brief Write one economics record for a unit (wrapper over rbpower_storage_write_generic_data()).
     *
     * @param[in]  unit_id        Unit ID
     * @param[in]  period_type    Period type
     * @param[in]  economics_data Economics record to write
     * @param[out] result         Write operation result
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG on NULL arguments, otherwise
     *         any code returned by rbpower_storage_write_generic_data()
     */
    esp_err_t rbpower_storage_write_economics_data(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period_type,
        const rbpower_economics_transfer_t *economics_data,
        rbpower_write_result_t *result);

    /* ================================================================================================
     * GENERIC WRITE FUNCTION
     * ================================================================================================ */

    /**
     * @brief Write one record of any data type into its ring-buffer data file.
     *
     * Opens the existing data file, validates its header and record size,
     * writes the record at the current index, advances the index (wrapping
     * modulo `max_records`), rewrites the header (index, total, timestamps,
     * write count, CRC) and flushes. Diagnostic logs are emitted only after all
     * flash operations are finished.
     *
     * @param[in]  unit_id     Unit ID
     * @param[in]  data_type   Data type (ELECTRICAL, ENERGY, ECONOMICS, QUALITY, DIRECTION)
     * @param[in]  period_type Period type
     * @param[in]  data_buffer Transfer structure; its size must equal the file's record size
     * @param[out] result      Write operation result
     * @retval ESP_OK                success
     * @retval ESP_ERR_INVALID_ARG   NULL argument or period_type out of range
     * @retval ESP_ERR_NOT_FOUND     data file does not exist
     * @retval ESP_ERR_INVALID_STATE file header failed validation
     * @retval ESP_ERR_INVALID_SIZE  file record size differs from the transfer size of data_type
     * @retval ESP_ERR_TIMEOUT       write took longer than 80% of the total time budget before flush
     * @retval ESP_FAIL              open/read/seek/write error
     * @return Other codes from the path helpers are passed through.
     */
    esp_err_t rbpower_storage_write_generic_data(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type,
        const void *data_buffer,
        rbpower_write_result_t *result);

    /* ================================================================================================
     * FILE STATUS FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Get the status of a data file from its header.
     *
     * @param[in]  unit_id       Unit ID
     * @param[in]  data_type     Data type
     * @param[in]  period_type   Period type
     * @param[out] current_index Next write index (records)
     * @param[out] total_records Total number of records ever written
     * @param[out] file_size     File size in bytes
     * @retval ESP_OK                success
     * @retval ESP_ERR_INVALID_ARG   NULL output pointer
     * @retval ESP_ERR_NOT_FOUND     data file does not exist
     * @retval ESP_ERR_INVALID_STATE file header failed validation
     * @retval ESP_FAIL              open/read error
     */
    esp_err_t rbpower_storage_get_file_status(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type,
        uint32_t *current_index,
        uint32_t *total_records,
        uint64_t *file_size);

    /**
     * @brief Check whether storage is ready for writing.
     *
     * @return true if the storage system is initialized
     */
    bool rbpower_storage_is_write_ready(void);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_WRITE_H