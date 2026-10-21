/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_write.c
 * @brief Implementation of period-record writes into the per-unit ring-buffer data files.
 *
 * Log output is deferred: status flags and timings are collected in a
 * ::flash_operation_log_t during the flash operations and printed only after
 * the file is closed, so logging does not add latency inside the flash I/O.
 */

#include "rbstorage_write.h"
#include "rbstorage_structures.h"
#include "rbstorage_config.h"
#include "rbstorage_init.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "RBSTORAGE_WRITE";

/* Maximum operation durations (microseconds) */
#define MAX_FILE_OPEN_TIME_US     100000    ///< File open budget, µs (100 ms)
#define MAX_FILE_WRITE_TIME_US    200000    ///< Record write budget, µs (200 ms)
#define MAX_HEADER_UPDATE_TIME_US 150000    ///< Header update budget, µs (150 ms)
#define MAX_FILE_FLUSH_TIME_US    300000    ///< Flush budget, µs (300 ms)
#define MAX_TOTAL_WRITE_TIME_US   800000    ///< Whole write operation budget, µs (800 ms)

/** @brief Deferred-log state collected during one write and printed afterwards. */
typedef struct {
    /* Operation status flags */
    bool params_invalid;        ///< NULL argument passed
    bool period_3sec_skip;      ///< 3-second period requested (never stored in flash)
    bool file_not_exists;       ///< Data file does not exist
    bool file_open_failed;      ///< fopen() failed
    bool header_read_failed;    ///< File header could not be read
    bool header_invalid;        ///< File header failed validation
    bool size_mismatch;         ///< Header record size differs from the expected transfer size
    bool seek_failed;           ///< fseek() to the record position failed
    bool write_failed;          ///< fwrite() of the record was short
    bool header_update_failed;  ///< Header rewrite after the record write failed
    bool flush_timeout;         ///< Operation aborted before flush: time budget exceeded
    
    /* Timings */
    uint32_t file_open_us;      ///< Time to open the file, µs
    uint32_t write_duration_us; ///< Time to write the record, µs
    uint32_t header_update_us;  ///< Time to rewrite the header, µs
    uint32_t flush_duration_us; ///< Time to flush and close, µs
    uint32_t total_duration_us; ///< Total operation time, µs
    
    /* Additional information */
    uint32_t write_index;       ///< Record index written (after success: the next write index)
    uint32_t max_records;       ///< Ring-buffer capacity of the file, records
    bool wrapped;               ///< Write index wrapped to 0
    char file_path[128];        ///< Data file path
    
    /* Sizes for debugging */
    size_t expected_size;       ///< Expected record size, bytes
    size_t actual_size;         ///< Actual record size / bytes written
    uint64_t record_offset;     ///< Byte offset of the record in the file
} flash_operation_log_t;

/**
 * @brief Print the deferred logs after the flash operations have finished.
 */
static void output_deferred_logs(const flash_operation_log_t* log, esp_err_t result)
{
    // Print logs only after all flash operations are complete
    if (log->params_invalid) {
        ESP_LOGE(TAG, "Invalid parameters for data write");
        return;
    }
    
    if (log->period_3sec_skip) {
        ESP_LOGD(TAG, "Skipping 3-second period (not stored in flash)");
        return;
    }
    
    if (log->file_not_exists) {
        ESP_LOGW(TAG, "Data file does not exist: %s", log->file_path);
        return;
    }
    
    if (log->file_open_failed) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", log->file_path);
        return;
    }
    
    if (log->header_read_failed) {
        ESP_LOGE(TAG, "Failed to read file header");
        return;
    }
    
    if (log->header_invalid) {
        ESP_LOGE(TAG, "Invalid file header: %s", log->file_path);
        return;
    }
    
    if (log->size_mismatch) {
        ESP_LOGE(TAG, "Record size mismatch: %zu != %zu",
                 log->actual_size, log->expected_size);
        return;
    }
    
    if (log->seek_failed) {
        ESP_LOGE(TAG, "Failed to seek to write position: %llu", log->record_offset);
        return;
    }
    
    if (log->write_failed) {
        ESP_LOGE(TAG, "Failed to write data: %zu != %zu",
                 log->actual_size, log->expected_size);
        return;
    }
    
    if (log->header_update_failed) {
        ESP_LOGW(TAG, "Failed to update file header");
    }
    
    if (log->flush_timeout) {
        ESP_LOGE(TAG, "Write operation aborted: critical timeout");
        return;
    }
    
    // Successful-operation logs
    if (result == ESP_OK) {
        ESP_LOGD(TAG, "Write complete: %s idx=%lu/%lu wrap=%s time=%lums (open=%lums write=%lums hdr=%lums flush=%lums)",
                 log->file_path, (unsigned long)log->write_index, (unsigned long)log->max_records, 
                 log->wrapped ? "YES" : "NO", (unsigned long)(log->total_duration_us / 1000),
                 (unsigned long)(log->file_open_us / 1000),
                 (unsigned long)(log->write_duration_us / 1000),
                 (unsigned long)(log->header_update_us / 1000), 
                 (unsigned long)(log->flush_duration_us / 1000));
                 
        // Slow-operation warnings
        if (log->total_duration_us > MAX_TOTAL_WRITE_TIME_US) {
            ESP_LOGW(TAG, "CRITICAL flash write delay: %lums (limit %lums)",
                     (unsigned long)(log->total_duration_us / 1000), 
                     (unsigned long)(MAX_TOTAL_WRITE_TIME_US / 1000));
        }
    }
}

/**
 * @brief Check whether an operation has exceeded its time budget (µs).
 */
static inline bool is_operation_timeout(uint32_t start_time_us, uint32_t max_time_us) {
    uint32_t elapsed = esp_timer_get_time() - start_time_us;
    return elapsed > max_time_us;
}

/**
 * @brief No-op WDT reset hook (storage functions run in an async task that feeds its own WDT).
 */
static inline void safe_wdt_reset(void) {
    // The WDT is fed in the flash_write_task context, not here.
    // This function is kept for backward compatibility.
}

/**
 * @brief Check for a critical timeout during critical operations (and call the WDT hook).
 * @return true if the operation must be aborted due to a critical timeout
 */
IRAM_ATTR static inline bool check_critical_timeout(uint32_t start_time_us, uint32_t critical_limit_us) {
    uint32_t elapsed = esp_timer_get_time() - start_time_us;
    
    // WDT hook (no-op)
    safe_wdt_reset();
    
    return (elapsed > critical_limit_us);
}

/* ================================================================================================
 * PRIVATE FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Build the data file path for (unit, data type, period).
 */
static esp_err_t get_data_file_path(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type, 
    rbpower_period_type_t period_type,
    char* file_path,
    size_t path_size)
{
    if (file_path == NULL || path_size < 128) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Unit role and type (fixed placeholder values)
    rbpower_load_role_t unit_role = RBPOWER_ROLE_DEVICE_LOAD;
    rbpower_metric_type_t unit_type = RBPOWER_METRIC_POWER_LOAD;
    
    // Get the unit directory
    char unit_directory[96];
    esp_err_t ret = rbpower_storage_create_unit_directory(
        unit_id, unit_role, unit_type, unit_directory, sizeof(unit_directory));
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Get the data file path
    ret = rbpower_storage_get_data_file_path(unit_directory, data_type, period_type, 
                                            file_path, path_size);
    if (ret != ESP_OK) {
        return ret;
    }
    
    return ESP_OK;
}

/**
 * @brief Rewrite the file header after a record write (restores the file position afterwards).
 */
static esp_err_t update_file_header(
    FILE* file,
    uint32_t new_index,
    uint32_t total_records)
{
    if (file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Save the current position
    long current_pos = ftell(file);
    if (current_pos < 0) {
        return ESP_FAIL;
    }
    
    // Seek to the start of the file to read the header
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    
    // Read the existing header
    rbpower_file_header_t header;
    size_t read_bytes = fread(&header, 1, sizeof(header), file);
    if (read_bytes != sizeof(header)) {
        return ESP_FAIL;
    }
    
    // Update header fields (timestamps in seconds since boot)
    header.current_index = new_index;
    header.total_records = total_records;
    header.last_timestamp = esp_timer_get_time() / 1000000;
    header.write_count++;
    
    if (header.first_timestamp == 0) {
        header.first_timestamp = header.last_timestamp;
    }
    
    // WDT hook before the CRC computation (may be slow)
    safe_wdt_reset();
    
    // Recompute the header CRC
    header.crc32_header = rbpower_calculate_header_crc(&header);
    
    // Seek back to the start and write the updated header
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    
    // WDT hook before writing the header
    safe_wdt_reset();
    
    size_t written = fwrite(&header, 1, sizeof(header), file);
    if (written != sizeof(header)) {
        return ESP_FAIL;
    }
    
    // Restore the original position
    if (fseek(file, current_pos, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/* ================================================================================================
 * GENERIC WRITE FUNCTION
 * ================================================================================================ */

esp_err_t rbpower_storage_write_generic_data(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    const void* data_buffer,
    rbpower_write_result_t* result)
{
    // Initialise the deferred-log state
    flash_operation_log_t log = {0};
    esp_err_t ret = ESP_OK;
    
    if (data_buffer == NULL || result == NULL) {
        log.params_invalid = true;
        output_deferred_logs(&log, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    
    // WDT hook at the start of the critical operation
    safe_wdt_reset();
    uint32_t operation_start_us = esp_timer_get_time();
    
    // Initialise the result
    memset(result, 0, sizeof(rbpower_write_result_t));
    result->result = ESP_OK;
    result->write_timestamp_us = operation_start_us;
    
    // Check that the period can be stored in flash.
    // 3SEC and 15MIN periods are RAM-only and are not part of this period enum;
    // the check is kept as a safety net and should never trigger.
    if (period_type >= RBPOWER_PERIOD_MAX) {
        ESP_LOGW(TAG, "Invalid period for write: %d", period_type);
        result->result = ESP_ERR_INVALID_ARG;
        output_deferred_logs(&log, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get the data file path
    char file_path[128];
    ret = get_data_file_path(unit_id, data_type, period_type, 
                            file_path, sizeof(file_path));
    if (ret != ESP_OK) {
        result->result = ret;
        return ret;
    }
    strncpy(log.file_path, file_path, sizeof(log.file_path) - 1);
    
    // Check that the file exists
    struct stat st;
    bool file_exists = (stat(file_path, &st) == 0);
    
    if (!file_exists) {
        log.file_not_exists = true;
        result->result = ESP_ERR_NOT_FOUND;
        output_deferred_logs(&log, ESP_ERR_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }
    
    // WDT hook before opening the file
    safe_wdt_reset();
    
    // Open the file for read/write
    FILE* file = fopen(file_path, "r+b");
    if (file == NULL) {
        log.file_open_failed = true;
        result->result = ESP_FAIL;
        output_deferred_logs(&log, ESP_FAIL);
        return ESP_FAIL;
    }
    
    log.file_open_us = esp_timer_get_time() - operation_start_us;
    
    // Read the file header
    rbpower_file_header_t header;
    size_t read_bytes = fread(&header, 1, sizeof(header), file);
    if (read_bytes != sizeof(header)) {
        log.header_read_failed = true;
        fclose(file);
        result->result = ESP_FAIL;
        output_deferred_logs(&log, ESP_FAIL);
        return ESP_FAIL;
    }
    
    // Validate the header
    if (!rbpower_validate_file_header(&header)) {
        log.header_invalid = true;
        fclose(file);
        result->result = ESP_ERR_INVALID_STATE;
        output_deferred_logs(&log, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check the record size
    uint16_t expected_record_size = rbpower_get_transfer_size(data_type);
    if (header.record_size != expected_record_size) {
        log.size_mismatch = true;
        log.expected_size = expected_record_size;
        log.actual_size = header.record_size;
        fclose(file);
        result->result = ESP_ERR_INVALID_SIZE;
        output_deferred_logs(&log, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Compute the write position (ring buffer): header + index * record_size
    uint32_t write_index = header.current_index;
    uint64_t data_start_offset = sizeof(rbpower_file_header_t);
    uint64_t record_offset = data_start_offset + (write_index * header.record_size);
    log.record_offset = record_offset;
    log.write_index = write_index;
    log.max_records = header.max_records;
    
    // Seek to the write position
    if (fseek(file, record_offset, SEEK_SET) != 0) {
        log.seek_failed = true;
        fclose(file);
        result->result = ESP_FAIL;
        output_deferred_logs(&log, ESP_FAIL);
        return ESP_FAIL;
    }
    
    // WDT hook before writing the data
    safe_wdt_reset();
    uint32_t write_start_us = esp_timer_get_time();
    
    // Write the record
    size_t written = fwrite(data_buffer, 1, header.record_size, file);
    if (written != header.record_size) {
        log.write_failed = true;
        log.expected_size = header.record_size;
        log.actual_size = written;
        fclose(file);
        result->result = ESP_FAIL;
        output_deferred_logs(&log, ESP_FAIL);
        return ESP_FAIL;
    }
    
    log.write_duration_us = esp_timer_get_time() - write_start_us;
    
    // Advance the ring-buffer indexes
    uint32_t new_index = (write_index + 1) % header.max_records;
    uint32_t new_total = header.total_records + 1;
    log.wrapped = (new_index == 0 && header.total_records > 0);
    
    // WDT hook before updating the header
    safe_wdt_reset();
    uint32_t header_update_start_us = esp_timer_get_time();
    
    // Update the file header
    ret = update_file_header(file, new_index, new_total);
    if (ret != ESP_OK) {
        log.header_update_failed = true;
    }
    
    log.header_update_us = esp_timer_get_time() - header_update_start_us;
    
    // WDT hook before the flush
    safe_wdt_reset();
    uint32_t flush_start_us = esp_timer_get_time();
    
    // Check the time budget before flushing (no logging here)
    uint32_t elapsed = esp_timer_get_time() - operation_start_us;
    if (elapsed > MAX_TOTAL_WRITE_TIME_US * 0.8) {
        log.flush_timeout = true;
        fclose(file);
        result->result = ESP_ERR_TIMEOUT;
        output_deferred_logs(&log, ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    
    // Force the data out to flash
    fflush(file);
    fclose(file);
    
    log.flush_duration_us = esp_timer_get_time() - flush_start_us;
    log.total_duration_us = esp_timer_get_time() - operation_start_us;
    
    // Fill in the result
    result->result = ESP_OK;
    result->records_written = 1;
    result->current_file_index = new_index;
    result->file_wrapped = log.wrapped;
    
    // Store final values for logging
    log.write_index = new_index;
    
    // Print all deferred logs now that the flash operations are complete
    output_deferred_logs(&log, ESP_OK);
    
    return ESP_OK;
}

/* ================================================================================================
 * TYPE-SPECIFIC WRITE FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_write_electrical_data(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period_type,
    const rbpower_electrical_transfer_t* electrical_data,
    rbpower_write_result_t* result)
{
    if (electrical_data == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Writing electrical data: unit=%04X, period=%d, U=%.1fV, I=%.2fA, P=%.0fW", 
             unit_id, period_type, electrical_data->voltage_rms_v10 / 10.0f, 
             electrical_data->current_rms_ma / 1000.0f, electrical_data->power_active_w10 / 10.0f);
    
    return rbpower_storage_write_generic_data(unit_id, RBPOWER_DATA_TYPE_ELECTRICAL, 
                                             period_type, electrical_data, result);
}

esp_err_t rbpower_storage_write_energy_data(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period_type,
    const rbpower_energy_transfer_t* energy_data,
    rbpower_write_result_t* result)
{
    if (energy_data == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Writing energy data: unit=%04X, period=%d, consumed=%.2fWh, supplied=%.2fWh", 
             unit_id, period_type, (float)energy_data->energy_consumed_wh, (float)energy_data->energy_supplied_wh);
    
    return rbpower_storage_write_generic_data(unit_id, RBPOWER_DATA_TYPE_ENERGY, 
                                             period_type, energy_data, result);
}

esp_err_t rbpower_storage_write_economics_data(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period_type,
    const rbpower_economics_transfer_t* economics_data,
    rbpower_write_result_t* result)
{
    if (economics_data == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Writing economics data: unit=%04X, period=%d, cost=%.4f€, revenue=%.4f€", 
             unit_id, period_type, economics_data->cost_consumed_kopecks / 100.0f, economics_data->revenue_supplied_kopecks / 100.0f);
    
    return rbpower_storage_write_generic_data(unit_id, RBPOWER_DATA_TYPE_ECONOMICS, 
                                             period_type, economics_data, result);
}

/* ================================================================================================
 * FILE STATUS FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_get_file_status(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    uint32_t* current_index,
    uint32_t* total_records,
    uint64_t* file_size)
{
    if (current_index == NULL || total_records == NULL || file_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get the file path
    char file_path[128];
    esp_err_t ret = get_data_file_path(unit_id, data_type, period_type, 
                                      file_path, sizeof(file_path));
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Check that the file exists
    struct stat st;
    if (stat(file_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    *file_size = st.st_size;
    
    // Read the file header
    FILE* file = fopen(file_path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    
    rbpower_file_header_t header;
    size_t read_bytes = fread(&header, 1, sizeof(header), file);
    fclose(file);
    
    if (read_bytes != sizeof(header)) {
        return ESP_FAIL;
    }
    
    if (!rbpower_validate_file_header(&header)) {
        return ESP_ERR_INVALID_STATE;
    }
    
    *current_index = header.current_index;
    *total_records = header.total_records;
    
    return ESP_OK;
}

bool rbpower_storage_is_write_ready(void)
{
    // Ready when the storage system is initialized
    return rbpower_storage_is_initialized();
}