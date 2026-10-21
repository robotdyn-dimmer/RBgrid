/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_init.c
 * @brief Creation of the statistics file structure on the `storage` partition
 *        (directories, pre-allocated data files, unit metadata, global index).
 */

#include "rbstorage_init.h"
#include "rbstorage_config.h"
#include "rbstorage_structures.h"
#include "rbstorage_transfer_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "esp_vfs.h"

static const char* TAG = "RBSTORAGE_INIT";

/* ================================================================================================
 * INTERNAL FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Check whether a directory exists.
 */
static bool directory_exists(const char* path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/**
 * @brief Create a directory unless it already exists.
 */
static esp_err_t create_directory_safe(const char* path)
{
    if (directory_exists(path)) {
        ESP_LOGV(TAG, "Directory already exists: %s", path);
        return ESP_OK;
    }
    
    if (mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create directory %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Created directory: %s", path);
    return ESP_OK;
}

/**
 * @brief Build a data file name ("<type>_<period>.dat").
 */
static esp_err_t get_data_filename(
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    char* filename,
    size_t filename_size)
{
    if (filename == NULL || filename_size < 32) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const char* data_name = rbpower_get_data_type_name(data_type);
    const char* period_name = rbpower_get_period_name(period_type);
    
    snprintf(filename, filename_size, "%s_%s.dat", data_name, period_name);
    return ESP_OK;
}

/* ================================================================================================
 * BASE STRUCTURE FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_create_base_structure(void)
{
    ESP_LOGI(TAG, "Creating base storage structure");
    
    // Create the base directories
    esp_err_t ret = create_directory_safe(RBPOWER_STORAGE_BASE_PATH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create base path: %s", RBPOWER_STORAGE_BASE_PATH);
        return ret;
    }
    
    ret = create_directory_safe(RBPOWER_STORAGE_UNITS_PATH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create units path: %s", RBPOWER_STORAGE_UNITS_PATH);
        return ret;
    }
    
    ret = create_directory_safe(RBPOWER_STORAGE_ARCHIVE_PATH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create archive path: %s", RBPOWER_STORAGE_ARCHIVE_PATH);
        return ret;
    }
    
    ESP_LOGI(TAG, "Base storage structure created successfully");
    return ESP_OK;
}

esp_err_t rbpower_storage_create_version_file(void)
{
    FILE* file = fopen(RBPOWER_STORAGE_VERSION_FILE, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to create version file: %s", strerror(errno));
        return ESP_FAIL;
    }
    
    rbpower_version_t version;
    rbpower_get_current_version(&version);
    
    uint32_t timestamp = esp_timer_get_time() / 1000000;
    
    fprintf(file, "rbpower_storage_version=%d.%d.%d\n", 
            version.major, version.minor, version.patch);
    fprintf(file, "created_timestamp=%"PRIu32"\n", timestamp);
    fprintf(file, "format_version=1\n");
    
    fclose(file);
    
    ESP_LOGI(TAG, "Created version file: %d.%d.%d", 
             version.major, version.minor, version.patch);
    return ESP_OK;
}

/* ================================================================================================
 * UNIT DIRECTORY FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_create_unit_directory(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    char* directory_path,
    size_t path_size)
{
    if (directory_path == NULL || path_size < 64) {
        ESP_LOGE(TAG, "Invalid directory path buffer");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get the unit directory name
    char unit_dir_name[RBPOWER_STORAGE_MAX_DIR_NAME];
    esp_err_t ret = rbpower_storage_get_unit_directory_name(
        unit_id, unit_role, unit_type, unit_dir_name, sizeof(unit_dir_name));
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGD(TAG, "Unit %04X excluded from storage (VOLTAGE_BUS)", unit_id);
        } else {
            ESP_LOGE(TAG, "Failed to get directory name for unit %04X", unit_id);
        }
        return ret;
    }
    
    // Build the full path
    snprintf(directory_path, path_size, "%s/%s", RBPOWER_STORAGE_UNITS_PATH, unit_dir_name);
    
    // Create the directory
    ret = create_directory_safe(directory_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create unit directory: %s", directory_path);
        return ret;
    }
    
    ESP_LOGD(TAG, "Created unit directory: %s (unit %04X)", directory_path, unit_id);
    return ESP_OK;
}

bool rbpower_storage_unit_directory_exists(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type)
{
    char directory_path[96];
    esp_err_t ret = rbpower_storage_create_unit_directory(
        unit_id, unit_role, unit_type, directory_path, sizeof(directory_path));
    
    if (ret != ESP_OK) {
        return false;
    }
    
    return directory_exists(directory_path);
}

/* ================================================================================================
 * DATA FILE FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_create_data_file(
    const char* unit_directory,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    bool force_recreate)
{
    if (unit_directory == NULL) {
        ESP_LOGE(TAG, "Unit directory is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get the file name
    char filename[32];
    esp_err_t ret = get_data_filename(data_type, period_type, filename, sizeof(filename));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get filename for data_type %d, period %d", data_type, period_type);
        return ret;
    }
    
    // Build the full file path
    char file_path[128];
    snprintf(file_path, sizeof(file_path), "%s/%s", unit_directory, filename);
    
    // Check whether the file exists
    struct stat st;
    bool file_exists = (stat(file_path, &st) == 0);
    
    if (file_exists && !force_recreate) {
        ESP_LOGV(TAG, "Data file already exists: %s", file_path);
        return ESP_OK;
    }
    
    // Build the file header
    rbpower_file_header_t header;
    ret = rbpower_init_file_header(&header, data_type, period_type);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize file header");
        return ret;
    }
    
    // Create the file
    FILE* file = fopen(file_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to create data file %s: %s", file_path, strerror(errno));
        return ESP_FAIL;
    }
    
    // Write the header
    size_t written = fwrite(&header, 1, sizeof(header), file);
    if (written != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to write header to %s: written %zu != %zu", 
                 file_path, written, sizeof(header));
        fclose(file);
        return ESP_FAIL;
    }
    
    // Pre-allocate the data area (fill with zeros)
    uint32_t data_size = header.max_records * header.record_size;
    
    // Write zeros in blocks to limit stack/RAM use
    const size_t block_size = 1024;
    uint8_t zero_block[block_size];
    memset(zero_block, 0, block_size);
    
    uint32_t remaining = data_size;
    while (remaining > 0) {
        size_t write_size = (remaining > block_size) ? block_size : remaining;
        size_t written_block = fwrite(zero_block, 1, write_size, file);
        
        if (written_block != write_size) {
            ESP_LOGE(TAG, "Failed to write data block to %s", file_path);
            fclose(file);
            return ESP_FAIL;
        }
        
        remaining -= write_size;
    }
    
    fclose(file);
    
    ESP_LOGD(TAG, "Created data file: %s (header: %zu bytes, data: %"PRIu32" bytes)",
             file_path, sizeof(header), data_size);
    
    return ESP_OK;
}

esp_err_t rbpower_storage_get_data_file_path(
    const char* unit_directory,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    char* file_path,
    size_t path_size)
{
    if (unit_directory == NULL || file_path == NULL || path_size < 128) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char filename[32];
    esp_err_t ret = get_data_filename(data_type, period_type, filename, sizeof(filename));
    if (ret != ESP_OK) {
        return ret;
    }
    
    snprintf(file_path, path_size, "%s/%s", unit_directory, filename);
    return ESP_OK;
}

const char* rbpower_storage_get_data_filename(
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type)
{
    static char filename_buffer[32];
    
    if (get_data_filename(data_type, period_type, filename_buffer, sizeof(filename_buffer)) == ESP_OK) {
        return filename_buffer;
    }
    
    return "unknown.dat";
}

/* ================================================================================================
 * METADATA FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_create_unit_metadata(
    const char* unit_directory,
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    bool force_recreate)
{
    if (unit_directory == NULL) {
        ESP_LOGE(TAG, "Unit directory is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    char meta_path[128];
    snprintf(meta_path, sizeof(meta_path), "%s/.meta", unit_directory);
    
    // Check whether it exists
    struct stat st;
    bool file_exists = (stat(meta_path, &st) == 0);
    
    if (file_exists && !force_recreate) {
        ESP_LOGV(TAG, "Metadata file already exists: %s", meta_path);
        return ESP_OK;
    }
    
    // Build the metadata
    rbpower_unit_metadata_t metadata;
    esp_err_t ret = rbpower_init_unit_metadata(&metadata, unit_id, unit_role, unit_type, unit_directory);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize unit metadata");
        return ret;
    }
    
    // Write the file
    FILE* file = fopen(meta_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to create metadata file %s: %s", meta_path, strerror(errno));
        return ESP_FAIL;
    }
    
    size_t written = fwrite(&metadata, 1, sizeof(metadata), file);
    if (written != sizeof(metadata)) {
        ESP_LOGE(TAG, "Failed to write metadata to %s: written %zu != %zu",
                 meta_path, written, sizeof(metadata));
        fclose(file);
        return ESP_FAIL;
    }
    
    fclose(file);
    
    ESP_LOGD(TAG, "Created metadata file: %s (unit %04X)", meta_path, unit_id);
    return ESP_OK;
}

/* ================================================================================================
 * UNIT STRUCTURE CREATION
 * ================================================================================================ */

esp_err_t rbpower_storage_create_unit_structure(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    bool force_recreate,
    rbpower_unit_creation_result_t* result)
{
    if (result == NULL) {
        ESP_LOGE(TAG, "Result pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize the result
    memset(result, 0, sizeof(rbpower_unit_creation_result_t));
    result->unit_id = unit_id;
    result->result = ESP_OK;
    
    ESP_LOGI(TAG, "Creating structure for unit %04X (role %d, type %d)", 
             unit_id, unit_role, unit_type);
    
    // 1. Create the unit directory
    char unit_directory[96];
    esp_err_t ret = rbpower_storage_create_unit_directory(
        unit_id, unit_role, unit_type, unit_directory, sizeof(unit_directory));
    
    if (ret != ESP_OK) {
        result->result = ret;
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGD(TAG, "Unit %04X excluded from storage", unit_id);
        } else {
            ESP_LOGE(TAG, "Failed to create directory for unit %04X", unit_id);
        }
        return ret;
    }
    
    // Store the directory name
    const char* dir_name = strrchr(unit_directory, '/');
    if (dir_name) {
        strncpy(result->unit_directory, dir_name + 1, sizeof(result->unit_directory) - 1);
        result->unit_directory[sizeof(result->unit_directory) - 1] = '\0';
    }
    
    // 2. Create the data files according to the configuration
    for (rbpower_period_type_t period = 0; period < RBPOWER_PERIOD_MAX; period++) {
        for (rbpower_data_type_t data_type = 0; data_type < RBPOWER_DATA_TYPE_MAX; data_type++) {
            
            if (!rbpower_storage_should_create_file(unit_role, period, data_type)) {
                continue; // File not needed for this role/period
            }
            
            ret = rbpower_storage_create_data_file(unit_directory, data_type, period, force_recreate);
            if (ret == ESP_OK) {
                result->created_files++;
                
                // Account for the file size
                rbpower_file_header_t header;
                if (rbpower_init_file_header(&header, data_type, period) == ESP_OK) {
                    result->total_size += rbpower_get_file_size_from_header(&header);
                }
            } else {
                ESP_LOGW(TAG, "Failed to create data file: %s/%s", 
                         unit_directory, rbpower_storage_get_data_filename(data_type, period));
                result->failed_files++;
            }
        }
    }
    
    // 3. Create the unit metadata
    ret = rbpower_storage_create_unit_metadata(unit_directory, unit_id, unit_role, unit_type, force_recreate);
    if (ret == ESP_OK) {
        result->metadata_created = true;
    } else {
        ESP_LOGW(TAG, "Failed to create metadata for unit %04X", unit_id);
    }
    
    // Update the overall result
    if (result->failed_files == 0 && result->metadata_created) {
        result->result = ESP_OK;
        ESP_LOGI(TAG, "Unit %04X structure created successfully: %d files, %llu bytes",
                 unit_id, result->created_files, result->total_size);
    } else {
        result->result = ESP_FAIL;
        ESP_LOGW(TAG, "Unit %04X structure creation completed with issues: %d failed files",
                 unit_id, result->failed_files);
    }
    
    return result->result;
}

/* ================================================================================================
 * GLOBAL INDEX FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_create_global_index(
    const rbpower_unit_id_t* units_list,
    uint8_t units_count,
    bool force_recreate)
{
    if (units_list == NULL) {
        ESP_LOGE(TAG, "Units list is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check whether the index exists
    struct stat st;
    bool index_exists = (stat(RBPOWER_STORAGE_INDEX_FILE, &st) == 0);
    
    if (index_exists && !force_recreate) {
        ESP_LOGD(TAG, "Global index already exists: %s", RBPOWER_STORAGE_INDEX_FILE);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Creating global index for %d units", units_count);
    
    // Allocate the index structure
    rbpower_global_index_t* index = malloc(sizeof(rbpower_global_index_t));
    if (index == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for global index");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize the index header
    memset(index, 0, sizeof(rbpower_global_index_t));
    index->magic = RBPOWER_GLOBAL_INDEX_MAGIC;
    rbpower_get_current_version(&index->version);
    index->creation_timestamp = esp_timer_get_time() / 1000000;
    index->last_scan_timestamp = index->creation_timestamp;
    
    // Filter the units that keep statistics
    rbpower_unit_id_t* filtered_units = NULL;
    uint8_t filtered_count = 0;
    
    esp_err_t ret = rbpower_storage_filter_units_for_stats(
        units_list, units_count, &filtered_units, &filtered_count);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to filter units for statistics");
        free(index);
        return ret;
    }
    
    index->total_units = units_count;
    index->active_units = filtered_count;
    index->special_units = 0;
    index->units_count = filtered_count;
    
    // Clear the per-role counters
    memset(index->units_by_role, 0, sizeof(index->units_by_role));
    
    // Process each unit
    index->total_estimated_size = 0;
    
    for (uint8_t i = 0; i < filtered_count && i < RBPOWER_MAX_UNITS; i++) {
        rbpower_unit_id_t unit_id = filtered_units[i];
        
        // TODO: take the real unit role/type from rbhome
        // Placeholders for now
        rbpower_load_role_t unit_role = RBPOWER_ROLE_DEVICE_LOAD; // Placeholder
        rbpower_metric_type_t unit_type = RBPOWER_METRIC_POWER_LOAD; // Placeholder
        
        rbpower_unit_index_entry_t* entry = &index->units[i];
        entry->unit_id = unit_id;
        entry->role = unit_role;
        entry->type = unit_type;
        entry->last_activity = index->creation_timestamp;
        
        // Determine the processing type
        rbpower_unit_processing_type_t processing_type = 
            rbpower_storage_get_unit_processing_type(unit_role, unit_type);
        
        entry->is_special_unit = (processing_type == RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN ||
                                 processing_type == RBPOWER_UNIT_PROCESSING_SPECIAL_ALT);
        
        if (entry->is_special_unit) {
            index->special_units++;
        }
        
        // Get the path name
        char temp_path[RBPOWER_STORAGE_MAX_DIR_NAME];
        esp_err_t path_ret = rbpower_storage_get_unit_directory_name(
            unit_id, unit_role, unit_type, temp_path, sizeof(temp_path));
        
        if (path_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get directory name for unit %04X", unit_id);
            snprintf(entry->unit_path, sizeof(entry->unit_path), "%04X", unit_id);
        } else {
            // Copy the result, truncating if needed
            strncpy(entry->unit_path, temp_path, sizeof(entry->unit_path) - 1);
            entry->unit_path[sizeof(entry->unit_path) - 1] = '\0';
        }
        
        // Count files and size
        entry->files_count = rbpower_storage_count_unit_files(unit_role);
        entry->estimated_size = rbpower_storage_estimate_unit_size(unit_role);
        
        index->total_estimated_size += entry->estimated_size;
        
        // Update the per-role counter
        if (unit_role < RBPOWER_STORAGE_ROLE_MAX) {
            index->units_by_role[unit_role]++;
        }
        
        ESP_LOGV(TAG, "Added unit %04X to index: role=%d, files=%d, size=%llu",
                 unit_id, unit_role, entry->files_count, entry->estimated_size);
    }
    
    // Get the available space
    uint64_t total_bytes, free_bytes;
    if (rbpower_storage_get_space_info(&total_bytes, &free_bytes) == ESP_OK) {
        index->available_space = free_bytes;
    } else {
        index->available_space = 0;
    }
    
    // Compute the index CRC
    index->index_crc32 = esp_crc32_le(0, (const uint8_t*)index, 
                                     sizeof(rbpower_global_index_t) - sizeof(index->index_crc32));
    
    // Save the index to its file
    ret = rbpower_storage_save_global_index(index);
    
    // Free memory
    uint64_t est_size = index->total_estimated_size;   /* read before free */
    free(filtered_units);
    free(index);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Global index created successfully: %d active units, %llu estimated bytes",
                 filtered_count, est_size);
    } else {
        ESP_LOGE(TAG, "Failed to save global index");
    }
    
    return ret;
}

esp_err_t rbpower_storage_save_global_index(const rbpower_global_index_t* index)
{
    if (index == NULL) {
        ESP_LOGE(TAG, "Index pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE* file = fopen(RBPOWER_STORAGE_INDEX_FILE, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to create index file %s: %s", 
                 RBPOWER_STORAGE_INDEX_FILE, strerror(errno));
        return ESP_FAIL;
    }
    
    size_t written = fwrite(index, 1, sizeof(rbpower_global_index_t), file);
    if (written != sizeof(rbpower_global_index_t)) {
        ESP_LOGE(TAG, "Failed to write index: written %zu != %zu",
                 written, sizeof(rbpower_global_index_t));
        fclose(file);
        return ESP_FAIL;
    }
    
    fclose(file);
    
    ESP_LOGD(TAG, "Global index saved successfully: %s", RBPOWER_STORAGE_INDEX_FILE);
    return ESP_OK;
}

esp_err_t rbpower_storage_load_global_index(rbpower_global_index_t* index)
{
    if (index == NULL) {
        ESP_LOGE(TAG, "Index pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE* file = fopen(RBPOWER_STORAGE_INDEX_FILE, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open index file %s: %s", 
                 RBPOWER_STORAGE_INDEX_FILE, strerror(errno));
        return ESP_FAIL;
    }
    
    size_t read_bytes = fread(index, 1, sizeof(rbpower_global_index_t), file);
    if (read_bytes != sizeof(rbpower_global_index_t)) {
        ESP_LOGE(TAG, "Failed to read index: read %zu != %zu",
                 read_bytes, sizeof(rbpower_global_index_t));
        fclose(file);
        return ESP_FAIL;
    }
    
    fclose(file);
    
    // Check the magic number
    if (index->magic != RBPOWER_GLOBAL_INDEX_MAGIC) {
        ESP_LOGE(TAG, "Invalid index magic: 0x%08"PRIX32, index->magic);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check the CRC
    uint32_t calculated_crc = esp_crc32_le(0, (const uint8_t*)index,
                                          sizeof(rbpower_global_index_t) - sizeof(index->index_crc32));
    
    if (index->index_crc32 != calculated_crc) {
        ESP_LOGE(TAG, "Index CRC mismatch: 0x%08"PRIX32" != 0x%08"PRIX32,
                 index->index_crc32, calculated_crc);
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGD(TAG, "Global index loaded successfully: %"PRIu32" active units", index->active_units);
    return ESP_OK;
}

esp_err_t rbpower_storage_get_space_info(uint64_t* total_bytes, uint64_t* free_bytes)
{
    if (total_bytes == NULL || free_bytes == NULL) {
        ESP_LOGE(TAG, "Space info pointers are NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Filesystem size info (simplified approach)
    // Fixed values are used instead of querying LittleFS
    // A complete implementation should take these values from the partition
    *total_bytes = 1024 * 1024; // 1MB default
    *free_bytes = 512 * 1024;   // 512KB free (rough estimate)
    
    // TODO: use the real LittleFS partition size API
    // (esp_littlefs_info()) for exact values
    
    ESP_LOGV(TAG, "Storage space: %llu total, %llu free (%.1f%% used)",
             *total_bytes, *free_bytes,
             100.0f * (1.0f - (float)*free_bytes / (float)*total_bytes));
    
    return ESP_OK;
}

/* ================================================================================================
 * MAIN INITIALIZATION FUNCTION
 * ================================================================================================ */

esp_err_t rbpower_storage_initialize_filesystem(
    const rbpower_unit_id_t* units_list,
    uint8_t units_count,
    bool force_recreate,
    rbpower_storage_init_result_t* result)
{
    if (units_list == NULL || result == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for filesystem initialization");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "=== Starting filesystem initialization ===");
    ESP_LOGI(TAG, "Units to process: %d, Force recreate: %s", units_count, force_recreate ? "YES" : "NO");
    
    // Initialize the result
    memset(result, 0, sizeof(rbpower_storage_init_result_t));
    result->total_units = units_count;
    result->overall_result = ESP_OK;
    
    uint64_t start_time = esp_timer_get_time();
    
    // 1. Create the base structure
    esp_err_t ret = rbpower_storage_create_base_structure();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create base structure");
        result->overall_result = ret;
        return ret;
    }
    result->total_directories_created += 3; // base, units, archive
    
    // 2. Create the version file
    ret = rbpower_storage_create_version_file();
    if (ret == ESP_OK) {
        result->version_file_created = true;
    } else {
        ESP_LOGW(TAG, "Failed to create version file");
    }
    
    // 3. Process the units
    for (uint8_t i = 0; i < units_count; i++) {
        rbpower_unit_id_t unit_id = units_list[i];
        
        // TODO: take the real unit role/type from rbhome
        rbpower_load_role_t unit_role = RBPOWER_ROLE_DEVICE_LOAD; // Placeholder
        rbpower_metric_type_t unit_type = RBPOWER_METRIC_POWER_LOAD; // Placeholder
        
        // Should this unit be excluded?
        if (rbpower_storage_should_exclude_unit(unit_role, unit_type)) {
            result->excluded_units++;
            ESP_LOGD(TAG, "Unit %04X excluded from storage", unit_id);
            continue;
        }
        
        // Create the unit structure
        rbpower_unit_creation_result_t unit_result;
        ret = rbpower_storage_create_unit_structure(unit_id, unit_role, unit_type, 
                                                   force_recreate, &unit_result);
        
        if (ret == ESP_OK) {
            result->processed_units++;
            result->total_files_created += unit_result.created_files;
            result->total_directories_created += 1; // unit directory
            result->total_allocated_size += unit_result.total_size;
        } else {
            result->failed_units++;
            ESP_LOGW(TAG, "Failed to create structure for unit %04X", unit_id);
        }
    }
    
    // 4. Create the global index
    ret = rbpower_storage_create_global_index(units_list, units_count, force_recreate);
    if (ret == ESP_OK) {
        result->index_created = true;
    } else {
        ESP_LOGW(TAG, "Failed to create global index");
    }
    
    // 5. Determine the overall result
    if (result->failed_units == 0 && result->index_created && result->processed_units > 0) {
        result->overall_result = ESP_OK;
    } else if (result->processed_units > 0) {
        result->overall_result = ESP_OK; // Partial success is reported as ESP_OK
    } else {
        result->overall_result = ESP_FAIL;
    }
    
    uint64_t end_time = esp_timer_get_time();
    uint32_t duration_ms = (end_time - start_time) / 1000;
    
    ESP_LOGI(TAG, "=== Filesystem initialization completed in %"PRIu32" ms ===", duration_ms);
    rbpower_storage_print_init_statistics(result);
    
    return result->overall_result;
}

/* ================================================================================================
 * UTILITY FUNCTIONS
 * ================================================================================================ */

void rbpower_storage_print_init_statistics(const rbpower_storage_init_result_t* result)
{
    if (result == NULL) {
        ESP_LOGW(TAG, "Init statistics: NULL result");
        return;
    }
    
    ESP_LOGI(TAG, "=== Storage Initialization Statistics ===");
    ESP_LOGI(TAG, "Total units: %d", result->total_units);
    ESP_LOGI(TAG, "Processed: %d, Failed: %d, Excluded: %d", 
             result->processed_units, result->failed_units, result->excluded_units);
    ESP_LOGI(TAG, "Files created: %d, Directories: %d", 
             result->total_files_created, result->total_directories_created);
    ESP_LOGI(TAG, "Total allocated size: %llu bytes (%.1f KB)", 
             result->total_allocated_size, (float)result->total_allocated_size / 1024.0f);
    ESP_LOGI(TAG, "Index created: %s, Version file: %s",
             result->index_created ? "YES" : "NO",
             result->version_file_created ? "YES" : "NO");
    ESP_LOGI(TAG, "Overall result: %s (0x%x)",
             result->overall_result == ESP_OK ? "SUCCESS" : "FAILED",
             result->overall_result);
    ESP_LOGI(TAG, "==========================================");
}

bool rbpower_storage_is_initialized(void)
{
    // Check that the base directories and files exist
    if (!directory_exists(RBPOWER_STORAGE_BASE_PATH)) {
        return false;
    }
    
    if (!directory_exists(RBPOWER_STORAGE_UNITS_PATH)) {
        return false;
    }
    
    // Check that the version file exists
    struct stat st;
    if (stat(RBPOWER_STORAGE_VERSION_FILE, &st) != 0) {
        return false;
    }
    
    // Check that the index file exists
    if (stat(RBPOWER_STORAGE_INDEX_FILE, &st) != 0) {
        return false;
    }
    
    return true;
}