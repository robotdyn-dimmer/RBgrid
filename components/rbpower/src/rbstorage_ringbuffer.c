/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_ringbuffer.c
 * @brief Implementation of ring-buffer reads from the per-unit data files.
 */

#include "rbstorage_ringbuffer.h"
#include "rbstorage_structures.h"
#include "rbstorage_init.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "RBSTORAGE_RING";

/* ================================================================================================
 * PRIVATE FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Build the data file path for (unit, data type, period) (same as in rbstorage_write.c).
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
        ESP_LOGE(TAG, "Failed to get directory of unit %04X: 0x%x", unit_id, ret);
        return ret;
    }
    
    // Get the data file path
    ret = rbpower_storage_get_data_file_path(unit_directory, data_type, period_type, 
                                            file_path, path_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get data file path: 0x%x", ret);
        return ret;
    }
    
    return ESP_OK;
}

/**
 * @brief Compute the index of the oldest record in the ring buffer.
 */
static uint32_t calculate_oldest_index(uint32_t current_index, uint32_t total_records, uint32_t max_records)
{
    if (total_records == 0) {
        return 0;
    }
    
    if (total_records <= max_records) {
        // Not wrapped yet: the oldest record is at index 0
        return 0;
    } else {
        // Wrapped: the oldest record is at the head (next slot to be overwritten)
        return current_index;
    }
}

/**
 * @brief Compute the index of the newest record in the ring buffer.
 */
static uint32_t calculate_newest_index(uint32_t current_index, uint32_t total_records, uint32_t max_records)
{
    if (total_records == 0) {
        return 0;
    }
    
    // The newest record is the slot just before the current write index
    return (current_index == 0) ? (max_records - 1) : (current_index - 1);
}

/* ================================================================================================
 * RING BUFFER MANAGEMENT API
 * ================================================================================================ */

esp_err_t rbpower_ringbuffer_get_status(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    rbpower_ringbuffer_status_t* status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialise the status
    memset(status, 0, sizeof(rbpower_ringbuffer_status_t));
    
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
        ESP_LOGD(TAG, "Data file does not exist: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Open the file for reading
    FILE* file = fopen(file_path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", file_path);
        return ESP_FAIL;
    }
    
    // Read the file header
    rbpower_file_header_t header;
    size_t read_bytes = fread(&header, 1, sizeof(header), file);
    fclose(file);
    
    if (read_bytes != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to read file header: %s", file_path);
        return ESP_FAIL;
    }
    
    // Validate the header
    if (!rbpower_validate_file_header(&header)) {
        ESP_LOGE(TAG, "Invalid file header: %s", file_path);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Fill in the status
    status->current_index = header.current_index;
    status->total_records = header.total_records;
    status->max_records = header.max_records;
    
    // Compute derived values
    if (status->total_records == 0) {
        status->available_records = 0;
        status->is_empty = true;
        status->is_full = false;
        status->oldest_index = 0;
        status->newest_index = 0;
    } else {
        status->is_empty = false;
        status->is_full = (status->total_records >= status->max_records);
        status->available_records = (status->is_full) ? status->max_records : status->total_records;
        status->oldest_index = calculate_oldest_index(status->current_index, status->total_records, status->max_records);
        status->newest_index = calculate_newest_index(status->current_index, status->total_records, status->max_records);
    }
    
    ESP_LOGD(TAG, "Ring buffer status unit %04X: records=%lu/%lu, current_index=%lu, oldest=%lu, newest=%lu", 
             unit_id, (unsigned long)status->available_records, (unsigned long)status->max_records, (unsigned long)status->current_index,
             (unsigned long)status->oldest_index, (unsigned long)status->newest_index);
    
    return ESP_OK;
}

esp_err_t rbpower_ringbuffer_read_latest(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    void* buffer,
    size_t buffer_size,
    uint32_t max_records,
    rbpower_ringbuffer_read_result_t* result)
{
    if (buffer == NULL || result == NULL || max_records == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialise the result
    memset(result, 0, sizeof(rbpower_ringbuffer_read_result_t));
    result->result = ESP_OK;
    
    // Get the buffer status
    rbpower_ringbuffer_status_t status;
    esp_err_t ret = rbpower_ringbuffer_get_status(unit_id, data_type, period_type, &status);
    if (ret != ESP_OK) {
        result->result = ret;
        return ret;
    }
    
    if (status.is_empty) {
        ESP_LOGD(TAG, "Ring buffer empty for unit %04X", unit_id);
        result->result = ESP_OK;
        return ESP_OK;
    }
    
    // Number of records to read
    uint32_t records_to_read = (max_records > status.available_records) ? 
                              status.available_records : max_records;
    
    // Check the buffer size
    uint16_t record_size = rbpower_get_transfer_size(data_type);
    size_t required_size = records_to_read * record_size;
    
    if (buffer_size < required_size) {
        ESP_LOGE(TAG, "Buffer too small: %zu < %zu", buffer_size, required_size);
        result->result = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Start index for the read: N records back from the newest
    uint32_t start_index;
    if (records_to_read >= status.available_records) {
        start_index = status.oldest_index;
    } else {
        // Start at (newest_index - records_to_read + 1), wrapping below 0
        if (status.newest_index >= (records_to_read - 1)) {
            start_index = status.newest_index - records_to_read + 1;
        } else {
            start_index = status.max_records - (records_to_read - 1 - status.newest_index);
        }
    }
    
    // Read the records from the file
    ret = rbpower_ringbuffer_read_from_index(unit_id, data_type, period_type, 
                                           start_index, records_to_read, 
                                           buffer, buffer_size, result);
    
    return ret;
}

esp_err_t rbpower_ringbuffer_read_from_index(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    uint32_t start_index,
    uint32_t max_records,
    void* buffer,
    size_t buffer_size,
    rbpower_ringbuffer_read_result_t* result)
{
    if (buffer == NULL || result == NULL || max_records == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialise the result
    memset(result, 0, sizeof(rbpower_ringbuffer_read_result_t));
    result->result = ESP_OK;
    result->start_index = start_index;
    
    // Get the file path
    char file_path[128];
    esp_err_t ret = get_data_file_path(unit_id, data_type, period_type, 
                                      file_path, sizeof(file_path));
    if (ret != ESP_OK) {
        result->result = ret;
        return ret;
    }
    
    // Open the file for reading
    FILE* file = fopen(file_path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        result->result = ESP_FAIL;
        return ESP_FAIL;
    }
    
    // Read the header
    rbpower_file_header_t header;
    size_t read_bytes = fread(&header, 1, sizeof(header), file);
    if (read_bytes != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to read file header");
        fclose(file);
        result->result = ESP_FAIL;
        return ESP_FAIL;
    }
    
    // Validate the header
    if (!rbpower_validate_file_header(&header)) {
        ESP_LOGE(TAG, "Invalid file header");
        fclose(file);
        result->result = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check the index
    if (start_index >= header.max_records) {
        ESP_LOGE(TAG, "Start index out of range: %lu >= %lu", (unsigned long)start_index, (unsigned long)header.max_records);
        fclose(file);
        result->result = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }
    
    // Number of records to read
    uint32_t available_records = (header.total_records >= header.max_records) ? 
                                header.max_records : header.total_records;
    uint32_t records_to_read = (max_records > available_records) ? available_records : max_records;
    
    // Check the buffer size
    size_t required_size = records_to_read * header.record_size;
    if (buffer_size < required_size) {
        ESP_LOGE(TAG, "Buffer too small: %zu < %zu", buffer_size, required_size);
        fclose(file);
        result->result = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Read records sequentially, wrapping at the end of the ring buffer
    uint8_t* buffer_ptr = (uint8_t*)buffer;
    uint32_t current_index = start_index;
    uint32_t records_read = 0;
    bool wrapped = false;
    
    uint64_t data_start_offset = sizeof(rbpower_file_header_t);
    
    for (uint32_t i = 0; i < records_to_read; i++) {
        // Byte offset of the record
        uint64_t record_offset = data_start_offset + (current_index * header.record_size);
        
        // Seek to the record
        if (fseek(file, record_offset, SEEK_SET) != 0) {
            ESP_LOGW(TAG, "Failed to seek to read position: %llu", (unsigned long long)record_offset);
            break;
        }
        
        // Read the record
        size_t bytes_read = fread(buffer_ptr, 1, header.record_size, file);
        if (bytes_read != header.record_size) {
            ESP_LOGW(TAG, "Partial record read: %zu != %lu", bytes_read, (unsigned long)header.record_size);
            break;
        }
        
        buffer_ptr += header.record_size;
        records_read++;
        
        // Advance to the next index, wrapping at the end of the ring buffer
        current_index = (current_index + 1) % header.max_records;
        if (current_index == 0 && i < (records_to_read - 1)) {
            wrapped = true;
        }
    }
    
    fclose(file);
    
    // Fill in the result
    result->records_read = records_read;
    result->end_index = (current_index == 0) ? (header.max_records - 1) : (current_index - 1);
    result->buffer_wrapped = wrapped;
    result->result = ESP_OK;
    
    ESP_LOGD(TAG, "Read %lu records of unit %04X: indexes %lu-%lu, wrapped=%s",
             (unsigned long)records_read, unit_id, (unsigned long)start_index, (unsigned long)result->end_index, wrapped ? "YES" : "NO");
    
    return ESP_OK;
}

esp_err_t rbpower_ringbuffer_read_timerange(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    void* buffer,
    size_t buffer_size,
    rbpower_ringbuffer_read_result_t* result)
{
    // TODO: implement timestamp search.
    // For now the newest records are read instead.
    ESP_LOGW(TAG, "Time-range read not implemented yet, reading the latest records instead");
    
    uint16_t record_size = rbpower_get_transfer_size(data_type);
    uint32_t max_records = buffer_size / record_size;
    
    return rbpower_ringbuffer_read_latest(unit_id, data_type, period_type, 
                                         buffer, buffer_size, max_records, result);
}

/* ================================================================================================
 * TYPE-SPECIFIC READ FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_ringbuffer_read_electrical_latest(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period_type,
    rbpower_electrical_transfer_t* records,
    uint32_t max_records,
    rbpower_ringbuffer_read_result_t* result)
{
    if (records == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t buffer_size = max_records * sizeof(rbpower_electrical_transfer_t);
    
    return rbpower_ringbuffer_read_latest(unit_id, RBPOWER_DATA_TYPE_ELECTRICAL, period_type, 
                                         records, buffer_size, max_records, result);
}

esp_err_t rbpower_ringbuffer_read_energy_timerange(
    rbpower_unit_id_t unit_id,
    rbpower_period_type_t period_type,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    rbpower_energy_transfer_t* records,
    uint32_t max_records,
    rbpower_ringbuffer_read_result_t* result)
{
    if (records == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t buffer_size = max_records * sizeof(rbpower_energy_transfer_t);
    
    return rbpower_ringbuffer_read_timerange(unit_id, RBPOWER_DATA_TYPE_ENERGY, period_type,
                                           start_timestamp, end_timestamp,
                                           records, buffer_size, result);
}

/* ================================================================================================
 * RING BUFFER UTILITIES
 * ================================================================================================ */

uint32_t rbpower_ringbuffer_get_available_records(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type)
{
    rbpower_ringbuffer_status_t status;
    esp_err_t ret = rbpower_ringbuffer_get_status(unit_id, data_type, period_type, &status);
    
    if (ret != ESP_OK) {
        return 0;
    }
    
    return status.available_records;
}

bool rbpower_ringbuffer_is_full(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type)
{
    rbpower_ringbuffer_status_t status;
    esp_err_t ret = rbpower_ringbuffer_get_status(unit_id, data_type, period_type, &status);
    
    if (ret != ESP_OK) {
        return false;
    }
    
    return status.is_full;
}

bool rbpower_ringbuffer_is_empty(
    rbpower_unit_id_t unit_id,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type)
{
    rbpower_ringbuffer_status_t status;
    esp_err_t ret = rbpower_ringbuffer_get_status(unit_id, data_type, period_type, &status);
    
    if (ret != ESP_OK) {
        return true; // Treat as empty on error
    }
    
    return status.is_empty;
}