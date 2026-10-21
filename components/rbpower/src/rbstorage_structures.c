/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_structures.c
 * @brief Initialization, CRC and validation of the file-storage data structures
 *        (ring-buffer file header, unit metadata).
 */

#include "rbstorage_structures.h"
#include "rbstorage_transfer_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include <string.h>

static const char* TAG = "RBSTORAGE_STRUCT";

/* ================================================================================================
 * STRUCTURE INITIALIZATION
 * ================================================================================================ */

esp_err_t rbpower_init_file_header(
    rbpower_file_header_t* header,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type)
{
    if (header == NULL) {
        ESP_LOGE(TAG, "Header pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (data_type >= RBPOWER_DATA_TYPE_MAX || period_type >= RBPOWER_PERIOD_MAX) {
        ESP_LOGE(TAG, "Invalid data_type (%d) or period_type (%d)", data_type, period_type);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear the structure
    memset(header, 0, sizeof(rbpower_file_header_t));
    
    // Fill in identification and version
    header->magic = RBPOWER_FILE_HEADER_MAGIC;
    rbpower_get_current_version(&header->version);
    header->creation_timestamp = esp_timer_get_time() / 1000000; // Seconds since boot (esp_timer), not wall-clock epoch
    
    // Fill in the ring-buffer configuration
    header->data_type = data_type;
    header->period_type = period_type;
    header->record_size = rbpower_get_transfer_size(data_type);
    header->max_records = rbpower_get_records_count(period_type);
    
    // Initialize the ring-buffer state
    header->current_index = 0;
    header->total_records = 0;
    header->first_timestamp = 0;     // Set on the first write
    header->last_timestamp = 0;      // Set on the first write
    
    // Integrity control
    header->write_count = 0;
    header->data_crc32 = 0;          // Not used yet
    header->reserved = 0;
    
    // Compute the header CRC (excluding the crc32_header field)
    header->crc32_header = rbpower_calculate_header_crc(header);
    
    ESP_LOGD(TAG, "Initialized file header: type=%s, period=%s, records=%"PRIu32", size=%"PRIu32,
             rbpower_get_data_type_name(data_type),
             rbpower_get_period_name(period_type),
             header->max_records,
             header->record_size);
    
    return ESP_OK;
}

esp_err_t rbpower_init_unit_metadata(
    rbpower_unit_metadata_t* metadata,
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    const char* directory_path)
{
    if (metadata == NULL || directory_path == NULL) {
        ESP_LOGE(TAG, "Metadata pointer or directory path is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strlen(directory_path) >= sizeof(metadata->unit_directory_path)) {
        ESP_LOGE(TAG, "Directory path too long: %zu >= %zu", 
                 strlen(directory_path), sizeof(metadata->unit_directory_path));
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear the structure
    memset(metadata, 0, sizeof(rbpower_unit_metadata_t));
    
    // Fill in the metadata header
    metadata->magic = RBPOWER_UNIT_META_MAGIC;
    rbpower_version_t version;
    rbpower_get_current_version(&version);
    metadata->version_major = version.major;
    metadata->version_minor = version.minor;
    metadata->version_patch = version.patch;
    metadata->version_reserved = version.reserved;
    metadata->creation_timestamp = esp_timer_get_time() / 1000000;
    
    // Basic unit information
    metadata->unit_id = unit_id;
    metadata->unit_role = (uint8_t)unit_role;
    metadata->unit_type = (uint8_t)unit_type;
    metadata->last_updated = metadata->creation_timestamp;
    metadata->unit_name_crc = 0; // TODO: compute from the unit name
    metadata->flags = 0;
    
    // File configuration for the unit
    // TODO: fill files_config_data[20] from the files configuration
    memset(metadata->files_config_data, 0, 20); // Zeros for now
    
    // Transfer-structure sizes
    for (rbpower_data_type_t data_type = 0; data_type < RBPOWER_DATA_TYPE_MAX; data_type++) {
        metadata->record_sizes[data_type] = rbpower_get_transfer_size(data_type);
    }
    
    // Special flags and counters
    rbpower_unit_processing_type_t processing_type = 
        rbpower_storage_get_unit_processing_type(unit_role, unit_type);
    metadata->is_special_unit = (processing_type == RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN ||
                                processing_type == RBPOWER_UNIT_PROCESSING_SPECIAL_ALT) ? 1 : 0;
    metadata->is_active = 1;
    metadata->files_count = rbpower_storage_count_unit_files(unit_role);
    metadata->migration_count = 0;
    metadata->original_unit_id = unit_id;
    metadata->total_size_bytes = rbpower_storage_estimate_unit_size(unit_role);
    
    // Initialize the migration history (empty for now)
    metadata->prev_unit_id = 0;
    metadata->prev_role = 0;
    metadata->migration_timestamp = 0;
    memset(metadata->migration_reason, 0, 32);
    
    // Usage statistics
    metadata->total_records_written = 0;
    metadata->last_write_timestamp = 0;
    metadata->write_errors_count = 0;
    metadata->recovery_count = 0;
    metadata->estimated_next_cleanup = 0;
    
    // Directory path
    strncpy(metadata->unit_directory_path, directory_path, 
            sizeof(metadata->unit_directory_path) - 1);
    metadata->unit_directory_path[sizeof(metadata->unit_directory_path) - 1] = '\0';
    
    // Compute the metadata CRC (excluding the metadata_crc32 field)
    uint32_t crc = esp_crc32_le(0, (const uint8_t*)metadata, 
                               sizeof(rbpower_unit_metadata_t) - sizeof(metadata->metadata_crc32));
    metadata->metadata_crc32 = crc;
    
    ESP_LOGD(TAG, "Initialized unit metadata: ID=%04X, role=%d, type=%d, special=%d, files=%d",
             unit_id, unit_role, unit_type, metadata->is_special_unit, metadata->files_count);
    
    return ESP_OK;
}

/* ================================================================================================
 * CRC CALCULATION
 * ================================================================================================ */

uint32_t rbpower_calculate_header_crc(const rbpower_file_header_t* header)
{
    if (header == NULL) {
        ESP_LOGE(TAG, "Header pointer is NULL for CRC calculation");
        return 0;
    }
    
    // CRC over the whole structure, skipping the crc32_header field
    const uint8_t* data = (const uint8_t*)header;
    const size_t crc_field_offset = offsetof(rbpower_file_header_t, crc32_header);
    const size_t crc_field_size = sizeof(header->crc32_header);
    
    uint32_t crc = 0;
    
    // CRC of the bytes before crc32_header
    if (crc_field_offset > 0) {
        crc = esp_crc32_le(crc, data, crc_field_offset);
    }
    
    // CRC of the bytes after crc32_header
    const size_t remaining_offset = crc_field_offset + crc_field_size;
    const size_t remaining_size = sizeof(rbpower_file_header_t) - remaining_offset;
    if (remaining_size > 0) {
        crc = esp_crc32_le(crc, data + remaining_offset, remaining_size);
    }
    
    return crc;
}

uint32_t rbpower_calculate_metadata_crc(const rbpower_unit_metadata_t* metadata)
{
    if (metadata == NULL) {
        ESP_LOGE(TAG, "Metadata pointer is NULL for CRC calculation");
        return 0;
    }
    
    // CRC over the whole structure, skipping the metadata_crc32 field
    const uint8_t* data = (const uint8_t*)metadata;
    const size_t crc_field_offset = offsetof(rbpower_unit_metadata_t, metadata_crc32);
    const size_t crc_field_size = sizeof(metadata->metadata_crc32);
    
    uint32_t crc = 0;
    
    // CRC of the bytes before metadata_crc32
    if (crc_field_offset > 0) {
        crc = esp_crc32_le(crc, data, crc_field_offset);
    }
    
    // CRC of the bytes after metadata_crc32 (if any)
    const size_t remaining_offset = crc_field_offset + crc_field_size;
    const size_t remaining_size = sizeof(rbpower_unit_metadata_t) - remaining_offset;
    if (remaining_size > 0) {
        crc = esp_crc32_le(crc, data + remaining_offset, remaining_size);
    }
    
    return crc;
}

/* ================================================================================================
 * VALIDATION
 * ================================================================================================ */

bool rbpower_validate_file_header(const rbpower_file_header_t* header)
{
    if (header == NULL) {
        ESP_LOGD(TAG, "Header validation: NULL pointer");
        return false;
    }
    
    // Check the magic number
    if (header->magic != RBPOWER_FILE_HEADER_MAGIC) {
        ESP_LOGD(TAG, "Header validation: invalid magic 0x%08"PRIX32, header->magic);
        return false;
    }
    
    // Check the version
    if (!rbpower_is_version_compatible(&header->version)) {
        ESP_LOGD(TAG, "Header validation: incompatible version %d.%d.%d",
                 header->version.major, header->version.minor, header->version.patch);
        return false;
    }
    
    // Check the data and period types
    if (header->data_type >= RBPOWER_DATA_TYPE_MAX) {
        ESP_LOGD(TAG, "Header validation: invalid data_type %"PRIu32, header->data_type);
        return false;
    }
    
    if (header->period_type >= RBPOWER_PERIOD_MAX) {
        ESP_LOGD(TAG, "Header validation: invalid period_type %"PRIu32, header->period_type);
        return false;
    }
    
    // Check the sizes
    uint16_t expected_record_size = rbpower_get_transfer_size(header->data_type);
    if (header->record_size != expected_record_size) {
        ESP_LOGD(TAG, "Header validation: record size mismatch %"PRIu32" != %d", 
                 header->record_size, expected_record_size);
        return false;
    }
    
    uint32_t expected_max_records = rbpower_get_records_count(header->period_type);
    if (header->max_records != expected_max_records) {
        ESP_LOGD(TAG, "Header validation: max records mismatch %"PRIu32" != %"PRIu32,
                 header->max_records, expected_max_records);
        return false;
    }
    
    // Check the header CRC
    uint32_t calculated_crc = rbpower_calculate_header_crc(header);
    if (header->crc32_header != calculated_crc) {
        ESP_LOGD(TAG, "Header validation: CRC mismatch 0x%08"PRIX32" != 0x%08"PRIX32,
                 header->crc32_header, calculated_crc);
        return false;
    }
    
    ESP_LOGV(TAG, "Header validation: OK for %s/%s", 
             rbpower_get_data_type_name(header->data_type),
             rbpower_get_period_name(header->period_type));
    
    return true;
}

bool rbpower_validate_unit_metadata(const rbpower_unit_metadata_t* metadata)
{
    if (metadata == NULL) {
        ESP_LOGD(TAG, "Metadata validation: NULL pointer");
        return false;
    }
    
    // Check the magic number
    if (metadata->magic != RBPOWER_UNIT_META_MAGIC) {
        ESP_LOGD(TAG, "Metadata validation: invalid magic 0x%08"PRIX32, metadata->magic);
        return false;
    }
    
    // Check the version
    rbpower_version_t version = {
        .major = metadata->version_major,
        .minor = metadata->version_minor,
        .patch = metadata->version_patch,
        .reserved = metadata->version_reserved
    };
    if (!rbpower_is_version_compatible(&version)) {
        ESP_LOGD(TAG, "Metadata validation: incompatible version %d.%d.%d",
                 metadata->version_major, metadata->version_minor, metadata->version_patch);
        return false;
    }
    
    // Check the unit role
    if (!rbpower_storage_is_role_valid(metadata->unit_role)) {
        ESP_LOGD(TAG, "Metadata validation: invalid unit_role %d", metadata->unit_role);
        return false;
    }
    
    // Check the directory path
    if (strlen(metadata->unit_directory_path) == 0) {
        ESP_LOGD(TAG, "Metadata validation: empty directory path");
        return false;
    }
    
    // Check the metadata CRC (excluding the metadata_crc32 field)
    uint32_t calculated_crc = esp_crc32_le(0, (const uint8_t*)metadata, 
                                          sizeof(rbpower_unit_metadata_t) - sizeof(metadata->metadata_crc32));
    if (metadata->metadata_crc32 != calculated_crc) {
        ESP_LOGD(TAG, "Metadata validation: CRC mismatch 0x%08"PRIX32" != 0x%08"PRIX32,
                 metadata->metadata_crc32, calculated_crc);
        return false;
    }
    
    ESP_LOGV(TAG, "Metadata validation: OK for unit %04X", metadata->unit_id);
    return true;
}

/* ================================================================================================
 * UTILITY FUNCTIONS
 * ================================================================================================ */

uint64_t rbpower_get_file_size_from_header(const rbpower_file_header_t* header)
{
    if (header == NULL) {
        ESP_LOGE(TAG, "Header pointer is NULL for size calculation");
        return 0;
    }
    
    if (!rbpower_validate_file_header(header)) {
        ESP_LOGW(TAG, "Invalid header for size calculation");
        return 0;
    }
    
    uint64_t header_size = RBPOWER_FILE_HEADER_SIZE;
    uint64_t data_size = (uint64_t)header->max_records * header->record_size;
    
    return header_size + data_size;
}