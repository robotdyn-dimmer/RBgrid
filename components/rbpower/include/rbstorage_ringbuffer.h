/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_ringbuffer.h
 * @brief Read-side API for the ring buffers stored in per-unit data files.
 *
 * A data file is a ::rbpower_file_header_t followed by `max_records`
 * fixed-size records. `current_index` in the header is the next write slot
 * (head); once `total_records` reaches `max_records` the buffer is full and the
 * oldest record is the one at `current_index`. Indexes are record numbers,
 * not byte offsets.
 *
 * Calling context: blocking VFS reads; call from a task, never from an ISR.
 * No internal locking: a concurrent writer may advance the head between the
 * header read and the record reads.
 */

#ifndef RBPOWER_STORAGE_RINGBUFFER_H
#define RBPOWER_STORAGE_RINGBUFFER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "rbstorage_structures.h"
#include "rbstorage_transfer_types.h"
#include "rbhome_types.h"
#include "esp_err.h"

    /* ================================================================================================
     * RING BUFFER DATA TYPES
     * ================================================================================================ */

    /**
     * @brief Ring buffer status information
     */
    typedef struct
    {
        uint32_t current_index;     ///< Next write index (head), records
        uint32_t total_records;     ///< Total number of records ever written
        uint32_t max_records;       ///< Ring-buffer capacity, records
        uint32_t available_records; ///< Records available for reading: min(total_records, max_records)
        bool is_full;               ///< Buffer is full (total_records >= max_records; writes overwrite the oldest)
        bool is_empty;              ///< Buffer is empty (total_records == 0)
        uint32_t oldest_index;      ///< Index of the oldest record
        uint32_t newest_index;      ///< Index of the newest record (the slot before current_index)
    } rbpower_ringbuffer_status_t;

    /**
     * @brief Ring buffer read result
     */
    typedef struct
    {
        esp_err_t result;      ///< Operation result (same code the function returns)
        uint32_t records_read; ///< Number of records copied to the caller's buffer
        uint32_t start_index;  ///< Index of the first record read
        uint32_t end_index;    ///< Index of the last record read
        bool buffer_wrapped;   ///< The read crossed the end of the file and continued at index 0
    } rbpower_ringbuffer_read_result_t;

    /* ================================================================================================
     * RING BUFFER MANAGEMENT API
     * ================================================================================================ */

    /**
     * @brief Get the ring-buffer status of a data file from its header.
     *
     * @param[in]  unit_id     Unit ID
     * @param[in]  data_type   Data type
     * @param[in]  period_type Period type
     * @param[out] status      Filled with the status (zeroed first)
     * @retval ESP_OK                success
     * @retval ESP_ERR_INVALID_ARG   status is NULL
     * @retval ESP_ERR_NOT_FOUND     data file does not exist
     * @retval ESP_ERR_INVALID_STATE file header failed validation
     * @retval ESP_FAIL              open/read error
     * @return Other codes from the path helpers are passed through.
     */
    esp_err_t rbpower_ringbuffer_get_status(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type,
        rbpower_ringbuffer_status_t *status);

    /**
     * @brief Read the newest N records, oldest first.
     *
     * An empty buffer is not an error: ESP_OK with records_read = 0.
     *
     * @param[in]  unit_id     Unit ID
     * @param[in]  data_type   Data type
     * @param[in]  period_type Period type
     * @param[out] buffer      Destination for the records
     * @param[in]  buffer_size Size of buffer, bytes
     * @param[in]  max_records Maximum number of records to read (> 0)
     * @param[out] result      Read result
     * @retval ESP_OK               success (possibly 0 records)
     * @retval ESP_ERR_INVALID_ARG  NULL pointer or max_records == 0
     * @retval ESP_ERR_INVALID_SIZE buffer too small for the records to read
     * @return Any error from rbpower_ringbuffer_get_status() or
     *         rbpower_ringbuffer_read_from_index().
     */
    esp_err_t rbpower_ringbuffer_read_latest(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type,
        void *buffer,
        size_t buffer_size,
        uint32_t max_records,
        rbpower_ringbuffer_read_result_t *result);

    /**
     * @brief Read records in a timestamp range.
     *
     * @note Not implemented yet: the range is ignored and the newest records
     *       that fit into buffer are returned (rbpower_ringbuffer_read_latest()).
     *
     * @param[in]  unit_id         Unit ID
     * @param[in]  data_type       Data type
     * @param[in]  period_type     Period type
     * @param[in]  start_timestamp Range start, epoch s (currently ignored)
     * @param[in]  end_timestamp   Range end, epoch s (currently ignored)
     * @param[out] buffer          Destination for the records
     * @param[in]  buffer_size     Size of buffer, bytes
     * @param[out] result          Read result
     * @return Same codes as rbpower_ringbuffer_read_latest()
     */
    esp_err_t rbpower_ringbuffer_read_timerange(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type,
        uint32_t start_timestamp,
        uint32_t end_timestamp,
        void *buffer,
        size_t buffer_size,
        rbpower_ringbuffer_read_result_t *result);

    /**
     * @brief Read consecutive records starting at an index, wrapping at the end of the file.
     *
     * At most min(max_records, available records) are read. A seek or short
     * read stops the loop early; the call still returns ESP_OK and
     * result->records_read tells how many records were copied.
     *
     * @param[in]  unit_id     Unit ID
     * @param[in]  data_type   Data type
     * @param[in]  period_type Period type
     * @param[in]  start_index First record index (must be < max_records of the file)
     * @param[in]  max_records Maximum number of records to read (> 0)
     * @param[out] buffer      Destination for the records
     * @param[in]  buffer_size Size of buffer, bytes
     * @param[out] result      Read result
     * @retval ESP_OK                success
     * @retval ESP_ERR_INVALID_ARG   NULL pointer, max_records == 0 or start_index out of range
     * @retval ESP_ERR_INVALID_STATE file header failed validation
     * @retval ESP_ERR_INVALID_SIZE  buffer too small
     * @retval ESP_FAIL              open/header read error
     */
    esp_err_t rbpower_ringbuffer_read_from_index(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type,
        uint32_t start_index,
        uint32_t max_records,
        void *buffer,
        size_t buffer_size,
        rbpower_ringbuffer_read_result_t *result);

    /* ================================================================================================
     * SPECIALIZED READING FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Read the newest electrical records (typed wrapper over rbpower_ringbuffer_read_latest()).
     *
     * @param[in]  unit_id     Unit ID
     * @param[in]  period_type Period type
     * @param[out] records     Array of at least max_records elements
     * @param[in]  max_records Maximum number of records
     * @param[out] result      Read result
     * @retval ESP_ERR_INVALID_ARG records or result is NULL
     * @return Otherwise the codes of rbpower_ringbuffer_read_latest()
     */
    esp_err_t rbpower_ringbuffer_read_electrical_latest(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period_type,
        rbpower_electrical_transfer_t *records,
        uint32_t max_records,
        rbpower_ringbuffer_read_result_t *result);

    /**
     * @brief Read energy records in a time range (typed wrapper over rbpower_ringbuffer_read_timerange()).
     *
     * @note The range is currently ignored; see rbpower_ringbuffer_read_timerange().
     *
     * @param[in]  unit_id         Unit ID
     * @param[in]  period_type     Period type
     * @param[in]  start_timestamp Range start, epoch s
     * @param[in]  end_timestamp   Range end, epoch s
     * @param[out] records         Array of at least max_records elements
     * @param[in]  max_records     Maximum number of records
     * @param[out] result          Read result
     * @retval ESP_ERR_INVALID_ARG records or result is NULL
     * @return Otherwise the codes of rbpower_ringbuffer_read_timerange()
     */
    esp_err_t rbpower_ringbuffer_read_energy_timerange(
        rbpower_unit_id_t unit_id,
        rbpower_period_type_t period_type,
        uint32_t start_timestamp,
        uint32_t end_timestamp,
        rbpower_energy_transfer_t *records,
        uint32_t max_records,
        rbpower_ringbuffer_read_result_t *result);

    /* ================================================================================================
     * RING BUFFER UTILITIES
     * ================================================================================================ */

    /**
     * @brief Get the number of records available for reading.
     *
     * @param[in] unit_id     Unit ID
     * @param[in] data_type   Data type
     * @param[in] period_type Period type
     * @return Number of available records, or 0 on error
     */
    uint32_t rbpower_ringbuffer_get_available_records(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type);

    /**
     * @brief Check whether the ring buffer is full.
     *
     * @param[in] unit_id     Unit ID
     * @param[in] data_type   Data type
     * @param[in] period_type Period type
     * @return true if the buffer is full; false if not full or on error
     */
    bool rbpower_ringbuffer_is_full(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type);

    /**
     * @brief Check whether the ring buffer is empty.
     *
     * @param[in] unit_id     Unit ID
     * @param[in] data_type   Data type
     * @param[in] period_type Period type
     * @return true if the buffer is empty or on error
     */
    bool rbpower_ringbuffer_is_empty(
        rbpower_unit_id_t unit_id,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_RINGBUFFER_H