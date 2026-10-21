/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage.c
 * @brief Filesystem management: LittleFS partition mount/format/check,
 *        boot flags in NVS, and storage diagnostic tests.
 */

#include "rbstorage.h"
#include "rbstorage_types.h"
#include "rbstorage_transfer_types.h"
#include "rbstorage_config.h"
#include "rbstorage_init.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "rbjson.h"
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "RBSTORAGE";

// Mutex for thread-safe operations
static SemaphoreHandle_t g_storage_mutex = NULL;

// Global configuration
static rbstorage_config_t g_storage_config;

// Module initialization flag
static bool g_module_initialized = false;

// Default partition configuration
static const rbstorage_partition_config_t DEFAULT_PARTITION_CONFIG[RBSTORAGE_PARTITION_COUNT] = {
    [RBSTORAGE_PARTITION_STORAGE] = {
        .partition_label = "storage",
        .mount_path = "/storage",
        .max_files = 10,
        .format_on_fail = true,
        .enable_by_default = true,
        .check_interval_sec = 3600  // Check once per hour
    },
    [RBSTORAGE_PARTITION_CONFIG] = {
        .partition_label = "config",     // Same label as in the partition table
        .mount_path = "/config",
        .max_files = 5,
        .format_on_fail = false,        // Never format automatically!
        /* OFF: "config" is owned by littlefs_raw (external_flash_manager.c). A second LittleFS instance over the
         * same partition (VFS mount + .initialized marker write, and a recovery FORMAT on any mount error) can
         * corrupt or wipe units/tariffs/flash_slots/stats_fmt. Nothing reads the VFS /config path. */
        .enable_by_default = false,
        .check_interval_sec = 86400     // Check once per day
    },
    [RBSTORAGE_PARTITION_LOGS] = {
        .partition_label = "logs",
        .mount_path = "/logs", 
        .max_files = 3,
        .format_on_fail = true,
        .enable_by_default = true,
        .check_interval_sec = 7200      // Check every 2 hours
    }
};

// Internal function prototypes
static esp_err_t storage_lock(void);
static void storage_unlock(void);
static uint16_t calculate_checksum(const rbstorage_nvs_data_t* data);
static esp_err_t mount_littlefs_partition(rbstorage_partition_id_t partition_id);
static esp_err_t unmount_littlefs_partition(rbstorage_partition_id_t partition_id);
static esp_err_t format_littlefs_partition(rbstorage_partition_id_t partition_id);
static esp_err_t check_partition_health(rbstorage_partition_id_t partition_id);
static esp_err_t create_initialization_marker(rbstorage_partition_id_t partition_id);
static bool check_initialization_marker(rbstorage_partition_id_t partition_id);

// Take the module lock (5 s timeout)
static esp_err_t storage_lock(void)
{
    if (g_storage_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_storage_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

// Release the module lock
static void storage_unlock(void)
{
    if (g_storage_mutex != NULL) {
        xSemaphoreGive(g_storage_mutex);
    }
}

// Compute the byte-sum checksum of the NVS blob
static uint16_t calculate_checksum(const rbstorage_nvs_data_t* data)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint16_t sum = 0;
    
    // Skip the checksum field itself
    for (size_t i = 0; i < sizeof(rbstorage_nvs_data_t); i++) {
        if (i >= offsetof(rbstorage_nvs_data_t, checksum) && 
            i < offsetof(rbstorage_nvs_data_t, checksum) + sizeof(data->checksum)) {
            continue;
        }
        sum += bytes[i];
    }
    
    return sum;
}

// Module initialization
esp_err_t rbpower_storage_init(const rbstorage_config_t* config)
{
    if (g_module_initialized) {
        ESP_LOGW(TAG, "Module already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing storage module");
    
    // Create the mutex
    g_storage_mutex = xSemaphoreCreateMutex();
    if (g_storage_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize the configuration
    memset(&g_storage_config, 0, sizeof(g_storage_config));
    
    if (config != NULL) {
        // Copy the caller's configuration
        memcpy(&g_storage_config, config, sizeof(rbstorage_config_t));
    } else {
        // Use the default configuration
        for (int i = 0; i < RBSTORAGE_PARTITION_COUNT; i++) {
            g_storage_config.partitions[i] = DEFAULT_PARTITION_CONFIG[i];
            g_storage_config.states[i].is_enabled = DEFAULT_PARTITION_CONFIG[i].enable_by_default;
        }
        
        g_storage_config.check_on_mount = true;
        g_storage_config.periodic_checks = true;
        g_storage_config.check_depth = 1;
        // Do NOT set first_boot here!
    }
    
    g_storage_config.initialized = true;
    g_module_initialized = true;
    
    uint64_t start_time = esp_timer_get_time();
    
    // Load flags from NVS
    esp_err_t ret = rbpower_storage_load_boot_flags();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load boot flags: 0x%x, assuming first boot", (unsigned int)ret);
        g_storage_config.first_boot = true;  // Set ONLY when the flags could not be loaded
    }
    // If the flags loaded successfully, first_boot was already set correctly from NVS
    
    g_storage_config.init_time_us = esp_timer_get_time() - start_time;
    
    ESP_LOGI(TAG, "Storage module initialized in %llu us", (unsigned long long)g_storage_config.init_time_us);
    return ESP_OK;
}

// Module deinitialization
esp_err_t rbpower_storage_deinit(void)
{
    if (!g_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Deinitializing storage module");
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Save the flags before shutting down
    rbpower_storage_save_boot_flags();
    
    // Unmount all partitions
    for (int i = 0; i < RBSTORAGE_PARTITION_COUNT; i++) {
        if (g_storage_config.states[i].is_mounted) {
            unmount_littlefs_partition(i);
        }
    }
    
    g_module_initialized = false;
    g_storage_config.initialized = false;
    
    storage_unlock();
    
    // Delete the mutex
    if (g_storage_mutex != NULL) {
        vSemaphoreDelete(g_storage_mutex);
        g_storage_mutex = NULL;
    }
    
    return ESP_OK;
}

// One-time preparation on first boot
esp_err_t rbpower_storage_first_boot_init(void)
{
    if (!g_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Guard against repeated calls
    if (!g_storage_config.first_boot) {
        ESP_LOGW(TAG, "first_boot_init called but not first boot! Skipping.");
        return ESP_OK;  // Not an error, just skip
    }
    
    ESP_LOGI(TAG, "Starting first boot initialization");
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    uint64_t start_time = esp_timer_get_time();
    int success_count = 0;
    int error_count = 0;
    
    // Process each partition
    for (int i = 0; i < RBSTORAGE_PARTITION_COUNT; i++) {
        rbstorage_partition_config_t* part_config = &g_storage_config.partitions[i];
        rbstorage_partition_state_t* part_state = &g_storage_config.states[i];
        
        ESP_LOGI(TAG, "Processing partition %d: %s", i, part_config->partition_label);
        
        if (!part_state->is_enabled) {
            ESP_LOGI(TAG, "Partition %s is disabled, skipping", part_config->partition_label);
            continue;
        }
        
        // Check that the partition exists in the partition table
        const esp_partition_t* partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
            part_config->partition_label
        );
        
        if (partition == NULL) {
            ESP_LOGE(TAG, "Partition %s not found in partition table", part_config->partition_label);
            part_state->errors_count++;
            error_count++;
            continue;
        }
        
        ESP_LOGI(TAG, "Found partition %s, size: %u bytes", 
                 part_config->partition_label, (unsigned int)partition->size);
        
        // Try to mount the partition
        ret = mount_littlefs_partition(i);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to mount partition %s: 0x%x", 
                     part_config->partition_label, (unsigned int)ret);
            
            // Does the error indicate filesystem corruption?
            bool is_corruption = (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL);
            
            if (part_config->format_on_fail || is_corruption) {
                ESP_LOGI(TAG, "Formatting partition %s", part_config->partition_label);
                
                ret = format_littlefs_partition(i);
                if (ret == ESP_OK) {
                    // Retry the mount
                    ret = mount_littlefs_partition(i);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Successfully mounted after format");
                        part_state->recovery_count++;
                        part_state->last_recovery_time = esp_timer_get_time();
                    } else {
                        ESP_LOGE(TAG, "Failed to mount after format: 0x%x", (unsigned int)ret);
                        part_state->errors_count++;
                        error_count++;
                        continue;
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to format partition: 0x%x", (unsigned int)ret);
                    part_state->errors_count++;
                    error_count++;
                    continue;
                }
            } else {
                // Never format critical partitions without explicit permission
                ESP_LOGE(TAG, "Critical partition %s mount failed, not formatting", 
                         part_config->partition_label);
                part_state->errors_count++;
                error_count++;
                continue;
            }
        }
        
        // Check the initialization marker
        if (!check_initialization_marker(i)) {
            ESP_LOGI(TAG, "Creating initialization marker for %s", part_config->partition_label);
            ret = create_initialization_marker(i);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to create initialization marker: 0x%x", (unsigned int)ret);
            }
        }
        
        part_state->is_initialized = true;
        part_state->first_boot_done = true;
        part_state->mount_count++;
        success_count++;
        
        ESP_LOGI(TAG, "Partition %s initialized successfully", part_config->partition_label);
    }
    
    // Update the global counters
    g_storage_config.total_boots++;
    if (error_count == 0) {
        g_storage_config.successful_boots++;
    }
    
    // IMPORTANT: clear the first-boot flag
    g_storage_config.first_boot = false;
    
    // Save the flags to NVS
    ret = rbpower_storage_save_boot_flags();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save boot flags: 0x%x", (unsigned int)ret);
    }
    
    uint32_t duration_ms = (esp_timer_get_time() - start_time) / 1000;
    
    storage_unlock();
    
    ESP_LOGI(TAG, "First boot init completed in %lu ms. Success: %d, Errors: %d", 
             (unsigned long)duration_ms, success_count, error_count);
    
    return (error_count == 0) ? ESP_OK : ESP_FAIL;
}

// Mount a LittleFS partition
static esp_err_t mount_littlefs_partition(rbstorage_partition_id_t partition_id)
{
    if (partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    rbstorage_partition_state_t* state = &g_storage_config.states[partition_id];
    
    if (state->is_mounted) {
        ESP_LOGW(TAG, "Partition %s already mounted", config->partition_label);
        return ESP_OK;
    }
    
    esp_vfs_littlefs_conf_t conf = {
        .base_path = config->mount_path,
        .partition_label = config->partition_label,
        .format_if_mount_failed = false,  // Never format automatically
        .dont_mount = false,
    };
    
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret == ESP_OK) {
        state->is_mounted = true;
        ESP_LOGI(TAG, "Mounted %s at %s", config->partition_label, config->mount_path);
    }
    
    return ret;
}

// Unmount a partition
static esp_err_t unmount_littlefs_partition(rbstorage_partition_id_t partition_id)
{
    if (partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    rbstorage_partition_state_t* state = &g_storage_config.states[partition_id];
    
    if (!state->is_mounted) {
        return ESP_OK;
    }
    
    esp_err_t ret = esp_vfs_littlefs_unregister(config->partition_label);
    if (ret == ESP_OK) {
        state->is_mounted = false;
        ESP_LOGI(TAG, "Unmounted %s", config->partition_label);
    }
    
    return ret;
}

// Format a partition
static esp_err_t format_littlefs_partition(rbstorage_partition_id_t partition_id)
{
    if (partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    
    ESP_LOGI(TAG, "Formatting partition %s", config->partition_label);
    
    return esp_littlefs_format(config->partition_label);
}

// Create the initialization marker file
static esp_err_t create_initialization_marker(rbstorage_partition_id_t partition_id)
{
    if (partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    
    char marker_path[128];
    snprintf(marker_path, sizeof(marker_path), "%s/.initialized", config->mount_path);
    
    FILE* f = fopen(marker_path, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    
    // Write JSON with the initialization info
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "timestamp", (double)esp_timer_get_time());
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(root, "partition", config->partition_label);
    
    char* json_str = cJSON_Print(root);
    if (json_str != NULL) {
        fprintf(f, "%s", json_str);
        free(json_str);
    }
    
    cJSON_Delete(root);
    fclose(f);
    
    return ESP_OK;
}

// Check whether the initialization marker file exists
static bool check_initialization_marker(rbstorage_partition_id_t partition_id)
{
    if (partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return false;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    
    char marker_path[128];
    snprintf(marker_path, sizeof(marker_path), "%s/.initialized", config->mount_path);
    
    struct stat st;
    return (stat(marker_path, &st) == 0);
}

// Load the boot flags from NVS
esp_err_t rbpower_storage_load_boot_flags(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    ret = nvs_open(RBSTORAGE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    rbstorage_nvs_data_t nvs_data;
    size_t length = sizeof(nvs_data);
    
    ret = nvs_get_blob(nvs_handle, RBSTORAGE_NVS_KEY, &nvs_data, &length);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Check magic and version
    if (nvs_data.magic != RBSTORAGE_NVS_MAGIC || 
        nvs_data.version != RBSTORAGE_NVS_VERSION) {
        ESP_LOGW(TAG, "Invalid NVS data magic or version");
        return ESP_ERR_INVALID_VERSION;
    }
    
    // Check the checksum
    uint16_t calculated_checksum = calculate_checksum(&nvs_data);
    if (calculated_checksum != nvs_data.checksum) {
        ESP_LOGW(TAG, "NVS data checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    
    // Copy the data into the configuration
    for (int i = 0; i < RBSTORAGE_PARTITION_COUNT; i++) {
        g_storage_config.states[i].first_boot_done = nvs_data.partitions[i].first_boot_done;
        g_storage_config.states[i].skip_checks = nvs_data.partitions[i].skip_checks;
        g_storage_config.states[i].mount_count = nvs_data.partitions[i].mount_count;
        g_storage_config.states[i].last_check_time = nvs_data.partitions[i].last_check_time;
        g_storage_config.states[i].recovery_count = nvs_data.partitions[i].recovery_count;
        g_storage_config.states[i].last_recovery_time = nvs_data.partitions[i].last_recovery_time;
    }
    
    g_storage_config.total_boots = nvs_data.total_boots;
    g_storage_config.successful_boots = nvs_data.successful_boots;
    g_storage_config.first_boot = false;  // Data loaded, so this is not the first boot
    
    ESP_LOGI(TAG, "Boot flags loaded. Total boots: %lu, Successful: %lu", 
             (unsigned long)g_storage_config.total_boots, (unsigned long)g_storage_config.successful_boots);
    
    return ESP_OK;
}

// Save the boot flags to NVS
esp_err_t rbpower_storage_save_boot_flags(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    ret = nvs_open(RBSTORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    rbstorage_nvs_data_t nvs_data = {
        .magic = RBSTORAGE_NVS_MAGIC,
        .version = RBSTORAGE_NVS_VERSION,
        .total_boots = g_storage_config.total_boots,
        .successful_boots = g_storage_config.successful_boots
    };
    
    // Copy the per-partition data
    for (int i = 0; i < RBSTORAGE_PARTITION_COUNT; i++) {
        nvs_data.partitions[i].first_boot_done = g_storage_config.states[i].first_boot_done;
        nvs_data.partitions[i].skip_checks = g_storage_config.states[i].skip_checks;
        nvs_data.partitions[i].mount_count = g_storage_config.states[i].mount_count;
        nvs_data.partitions[i].last_check_time = g_storage_config.states[i].last_check_time;
        nvs_data.partitions[i].recovery_count = g_storage_config.states[i].recovery_count;
        nvs_data.partitions[i].last_recovery_time = g_storage_config.states[i].last_recovery_time;
    }
    
    // Compute the checksum
    nvs_data.checksum = calculate_checksum(&nvs_data);
    
    ret = nvs_set_blob(nvs_handle, RBSTORAGE_NVS_KEY, &nvs_data, sizeof(nvs_data));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Boot flags saved");
    return ret;
}

// Public partition functions

esp_err_t rbpower_storage_mount_partition(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    ret = mount_littlefs_partition(partition_id);
    
    storage_unlock();
    return ret;
}

esp_err_t rbpower_storage_unmount_partition(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    ret = unmount_littlefs_partition(partition_id);
    
    storage_unlock();
    return ret;
}

esp_err_t rbpower_storage_format_partition(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Unmount before formatting
    if (g_storage_config.states[partition_id].is_mounted) {
        unmount_littlefs_partition(partition_id);
    }
    
    ret = format_littlefs_partition(partition_id);
    
    storage_unlock();
    return ret;
}

esp_err_t rbpower_storage_check_partition(rbstorage_partition_id_t partition_id, 
                                         rbstorage_operation_result_t* result)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    uint64_t start_time = esp_timer_get_time();
    
    ret = check_partition_health(partition_id);
    
    if (result != NULL) {
        result->error_code = ret;
        result->partition_id = partition_id;
        result->duration_ms = (esp_timer_get_time() - start_time) / 1000;
        
        if (ret != ESP_OK) {
            snprintf(result->error_msg, sizeof(result->error_msg), 
                    "Partition check failed: 0x%x", (unsigned int)ret);
        } else {
            strcpy(result->error_msg, "OK");
        }
    }
    
    storage_unlock();
    return ret;
}

// Partition health check
static esp_err_t check_partition_health(rbstorage_partition_id_t partition_id)
{
    if (!g_storage_config.states[partition_id].is_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    
    // Basic check: create and delete a test file
    char test_path[128];
    snprintf(test_path, sizeof(test_path), "%s/.health_check", config->mount_path);
    
    FILE* f = fopen(test_path, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    
    fprintf(f, "test");
    fclose(f);
    
    // Delete the test file
    if (remove(test_path) != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Partition state control

esp_err_t rbpower_storage_enable_partition(rbstorage_partition_id_t partition_id, bool enable)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    g_storage_config.states[partition_id].is_enabled = enable;
    
    storage_unlock();
    return ESP_OK;
}

esp_err_t rbpower_storage_set_check_mode(rbstorage_partition_id_t partition_id, bool skip_checks)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    g_storage_config.states[partition_id].skip_checks = skip_checks;
    
    storage_unlock();
    return ESP_OK;
}

bool rbpower_storage_is_partition_ready(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return false;
    }
    
    rbstorage_partition_state_t* state = &g_storage_config.states[partition_id];
    
    return state->is_enabled && state->is_mounted && state->is_initialized;
}

// Information getters

esp_err_t rbpower_storage_get_info(rbstorage_partition_id_t partition_id,
                                  size_t* total_bytes, size_t* used_bytes)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!g_storage_config.states[partition_id].is_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return esp_littlefs_info(g_storage_config.partitions[partition_id].partition_label, 
                            total_bytes, used_bytes);
}

const rbstorage_partition_state_t* rbpower_storage_get_state(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return NULL;
    }
    
    return &g_storage_config.states[partition_id];
}

const rbstorage_config_t* rbpower_storage_get_config(void)
{
    if (!g_module_initialized) {
        return NULL;
    }
    
    return &g_storage_config;
}

const char* rbpower_storage_get_partition_name(rbstorage_partition_id_t partition_id)
{
    static const char* names[RBSTORAGE_PARTITION_COUNT] = {
        "storage",
        "config", 
        "logs"
    };
    
    if (partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return "unknown";
    }
    
    return names[partition_id];
}

const char* rbpower_storage_get_mount_path(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return NULL;
    }
    
    return g_storage_config.partitions[partition_id].mount_path;
}

// Recover a corrupted partition
esp_err_t rbpower_storage_recover_partition(rbstorage_partition_id_t partition_id)
{
    if (!g_module_initialized || partition_id >= RBSTORAGE_PARTITION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Starting recovery for partition %d: %s", 
             partition_id, rbpower_storage_get_partition_name(partition_id));
    
    esp_err_t ret = storage_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    
    rbstorage_partition_config_t* config = &g_storage_config.partitions[partition_id];
    rbstorage_partition_state_t* state = &g_storage_config.states[partition_id];
    
    // 1. Unmount the partition if mounted
    if (state->is_mounted) {
        ESP_LOGI(TAG, "Unmounting corrupted partition");
        ret = unmount_littlefs_partition(partition_id);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to unmount partition: 0x%x", (unsigned int)ret);
        }
    }
    
    // 2. Format the partition
    ESP_LOGI(TAG, "Formatting partition %s", config->partition_label);
    ret = format_littlefs_partition(partition_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format partition: 0x%x", (unsigned int)ret);
        storage_unlock();
        return ret;
    }
    
    // 3. Mount again
    ESP_LOGI(TAG, "Remounting partition");
    ret = mount_littlefs_partition(partition_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount after format: 0x%x", (unsigned int)ret);
        state->errors_count++;
        storage_unlock();
        return ret;
    }
    
    // 4. Create the initialization marker
    ESP_LOGI(TAG, "Creating initialization marker");
    ret = create_initialization_marker(partition_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create initialization marker: 0x%x", (unsigned int)ret);
    }
    
    // 5. Update the state
    state->is_initialized = true;
    state->mount_count++;
    state->recovery_count++;
    state->last_recovery_time = esp_timer_get_time();
    
    // 6. Save the flags
    rbpower_storage_save_boot_flags();
    
    storage_unlock();
    
    ESP_LOGI(TAG, "Partition %s recovered successfully", config->partition_label);
    return ESP_OK;
}

/* ================================================================================================
 * DIAGNOSTIC TEST FUNCTIONS
 * ================================================================================================ */

/*
 * Test the unit-to-storage mapping logic (documented in rbstorage.h).
 */
esp_err_t rbpower_storage_test_unit_mapping(void)
{
    ESP_LOGI(TAG, "=== Testing Unit Mapping Logic ===");
    
    // Test case structure
    typedef struct {
        rbpower_unit_id_t unit_id;
        rbpower_load_role_t role;
        rbpower_metric_type_t type;
        const char* expected_dir;
        bool should_exclude;
    } test_case_t;
    
    // Test unit_ids for different roles
    const test_case_t test_cases[] = {
        // Regular units
        {0x4401, RBPOWER_ROLE_MAIN_SUPPLY, RBPOWER_METRIC_POWER_LOAD, "Unit_MainSupply", false},
        {0x4402, RBPOWER_ROLE_ALTERNATIVE_SUPPLY, RBPOWER_METRIC_POWER_LOAD, "Unit_AltSupply", false},
        {0x4403, RBPOWER_ROLE_DEVICE_LOAD, RBPOWER_METRIC_CURRENT_LOAD, "4403", false},
        {0x4404, RBPOWER_ROLE_ZONE_LOAD, RBPOWER_METRIC_ENERGY_COUNTER, "4404", false},
        
        // VOLTAGE_BUS units (must be excluded)
        {0x0007, RBPOWER_ROLE_MAIN_SUPPLY, RBPOWER_METRIC_VOLTAGE_BUS, "", true},
        {0x1234, RBPOWER_ROLE_DEVICE_LOAD, RBPOWER_METRIC_VOLTAGE_BUS, "", true},
    };
    
    const size_t test_count = sizeof(test_cases) / sizeof(test_cases[0]);
    uint8_t passed = 0;
    
    for (size_t i = 0; i < test_count; i++) {
        const test_case_t* test = &test_cases[i];
        char dir_name[RBPOWER_STORAGE_MAX_DIR_NAME];
        
        ESP_LOGI(TAG, "Test %zu: Unit %04X, Role %d, Type %d", 
                 i + 1, test->unit_id, test->role, test->type);
        
        // Exclusion check
        bool excluded = rbpower_storage_should_exclude_unit(test->role, test->type);
        if (excluded != test->should_exclude) {
            ESP_LOGE(TAG, "  FAIL: Expected exclude=%d, got exclude=%d", 
                     test->should_exclude, excluded);
            continue;
        }
        
        if (excluded) {
            ESP_LOGI(TAG, "  PASS: Unit correctly excluded");
            passed++;
            continue;
        }
        
        // Directory name check
        esp_err_t ret = rbpower_storage_get_unit_directory_name(
            test->unit_id, test->role, test->type, dir_name, sizeof(dir_name));
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  FAIL: Directory name function returned error: 0x%x", ret);
            continue;
        }
        
        if (strcmp(dir_name, test->expected_dir) != 0) {
            ESP_LOGE(TAG, "  FAIL: Expected dir '%s', got '%s'", test->expected_dir, dir_name);
            continue;
        }
        
        ESP_LOGI(TAG, "  PASS: Directory name '%s'", dir_name);
        passed++;
    }
    
    // Unit filtering test
    ESP_LOGI(TAG, "\n=== Testing Units Filtering ===");
    
    rbpower_unit_id_t test_units[] = {0x4401, 0x4402, 0x0007, 0x4404, 0x1234};
    const uint8_t test_units_count = sizeof(test_units) / sizeof(test_units[0]);
    
    rbpower_unit_id_t* filtered_units = NULL;
    uint8_t filtered_count = 0;
    
    esp_err_t ret = rbpower_storage_filter_units_for_stats(
        test_units, test_units_count, &filtered_units, &filtered_count);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Units filtering failed: 0x%x", ret);
    } else {
        ESP_LOGI(TAG, "Filtering: %d -> %d units", test_units_count, filtered_count);
        for (uint8_t i = 0; i < filtered_count; i++) {
            ESP_LOGI(TAG, "  Filtered unit %d: %04X", i, filtered_units[i]);
        }
        
        // Expect 3 units (0x0007 and 0x1234 are excluded as VOLTAGE_BUS)
        if (filtered_count == 3) {
            ESP_LOGI(TAG, "  PASS: Correct number of filtered units");
            passed++;
        } else {
            ESP_LOGE(TAG, "  FAIL: Expected 3 filtered units, got %d", filtered_count);
        }
        
        free(filtered_units);
    }
    
    // Files configuration test
    ESP_LOGI(TAG, "\n=== Testing Files Configuration ===");
    
    rbpower_period_files_config_t config;
    ret = rbpower_storage_get_files_config(RBPOWER_ROLE_MAIN_SUPPLY, RBPOWER_PERIOD_HOUR, &config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MAIN_SUPPLY/HOUR config: e=%d en=%d ec=%d q=%d d=%d",
                 config.save_electrical, config.save_energy, config.save_economics,
                 config.save_quality, config.save_direction);
        
        // MAIN_SUPPLY at HOUR must have all files enabled
        if (config.save_electrical && config.save_energy && config.save_economics &&
            config.save_quality && config.save_direction) {
            ESP_LOGI(TAG, "  PASS: MAIN_SUPPLY has full configuration");
            passed++;
        } else {
            ESP_LOGE(TAG, "  FAIL: MAIN_SUPPLY should have all files enabled");
        }
    } else {
        ESP_LOGE(TAG, "Failed to get files config: 0x%x", ret);
    }
    
    // Final result
    ESP_LOGI(TAG, "\n=== Test Results ===");
    ESP_LOGI(TAG, "Passed: %d/%zu tests", passed, test_count + 2);
    
    if (passed == test_count + 2) {
            ESP_LOGI(TAG, "All unit mapping tests PASSED!");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Some unit mapping tests FAILED!");
        return ESP_FAIL;
    }
}

/*
 * Test creation of the storage file structure (documented in rbstorage.h).
 */
esp_err_t rbpower_storage_test_filesystem_creation(void)
{
    ESP_LOGI(TAG, "=== Testing Filesystem Creation ===");
    
    // Test units for building the structure
    rbpower_unit_id_t test_units[] = {
        0x4401, // MAIN_SUPPLY
        0x4402, // ALT_SUPPLY 
        0x4403, // DEVICE_LOAD
        0x4404, // ZONE_LOAD
        0x0007  // VOLTAGE_BUS (must be excluded)
    };
    
    const uint8_t units_count = sizeof(test_units) / sizeof(test_units[0]);
    
    ESP_LOGI(TAG, "Testing filesystem initialization with %d units", units_count);
    
    // Build the file structure
    rbpower_storage_init_result_t result;
    esp_err_t ret = rbpower_storage_initialize_filesystem(
        test_units, units_count, true, &result); // force_recreate = true for the test
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ Filesystem initialization completed successfully");
        
        // Check the results
        ESP_LOGI(TAG, "Test results:");
        ESP_LOGI(TAG, "  - Total units: %d", result.total_units);
        ESP_LOGI(TAG, "  - Processed: %d, Failed: %d, Excluded: %d",
                 result.processed_units, result.failed_units, result.excluded_units);
        ESP_LOGI(TAG, "  - Files created: %d, Directories: %d",
                 result.total_files_created, result.total_directories_created);
        ESP_LOGI(TAG, "  - Total size: %llu bytes (%.1f KB)",
                 result.total_allocated_size, (float)result.total_allocated_size / 1024.0f);
        ESP_LOGI(TAG, "  - Index created: %s", result.index_created ? "YES" : "NO");
        
        // Decide whether the test passed
        bool test_passed = (result.processed_units >= 3 && 
                           result.failed_units == 0 &&
                           result.index_created);
                           
        ESP_LOGI(TAG, "Test validation: processed=%d (>=3?), failed=%d (=0?), index=%d", 
                 result.processed_units, result.failed_units, result.index_created ? 1 : 0);
        
        if (test_passed) {
            ESP_LOGI(TAG, "✅ All filesystem creation tests PASSED!");
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "❌ Some filesystem creation tests FAILED!");
            return ESP_FAIL;
        }
        
    } else {
        ESP_LOGE(TAG, "❌ Filesystem initialization FAILED: 0x%x", ret);
        rbpower_storage_print_init_statistics(&result);
        return ret;
    }
}

/*
 * Print structure sizes for debugging (documented in rbstorage.h).
 */
esp_err_t rbpower_storage_debug_struct_sizes(void)
{
    printf("\n=== DEBUG: Structure Sizes ===\n");
    printf("rbpower_unit_metadata_t: %zu bytes (expected: %d)\n", 
           sizeof(rbpower_unit_metadata_t), RBPOWER_UNIT_META_SIZE);
    printf("rbpower_version_t: %zu bytes\n", sizeof(rbpower_version_t));
    printf("rbpower_unit_id_t: %zu bytes\n", sizeof(rbpower_unit_id_t));
    printf("rbpower_load_role_t: %zu bytes\n", sizeof(rbpower_load_role_t));
    printf("rbpower_metric_type_t: %zu bytes\n", sizeof(rbpower_metric_type_t));
    printf("rbpower_period_files_config_t: %zu bytes\n", sizeof(rbpower_period_files_config_t));
    printf("rbpower_unit_migration_t: %zu bytes\n", sizeof(rbpower_unit_migration_t));
    printf("RBPOWER_PERIOD_MAX: %d\n", RBPOWER_PERIOD_MAX);
    printf("RBPOWER_DATA_TYPE_MAX: %d\n", RBPOWER_DATA_TYPE_MAX);
    
    // Detailed per-section breakdown of the structure
    printf("\n--- Detailed rbpower_unit_metadata_t breakdown ---\n");
    rbpower_unit_metadata_t dummy;
    printf("Header section (magic+version+timestamp): %zu bytes\n", 
           (size_t)&dummy.unit_id - (size_t)&dummy.magic);
    printf("Unit info section (unit_id to reserved_unit): %zu bytes\n",
           (size_t)&dummy.files_config_data - (size_t)&dummy.unit_id);
    printf("Files config section: %zu bytes\n",
           (size_t)&dummy.record_sizes - (size_t)&dummy.files_config_data);
    printf("Record sizes section: %zu bytes\n",
           (size_t)&dummy.is_special_unit - (size_t)&dummy.record_sizes);
    printf("Special flags section: %zu bytes\n",
           (size_t)&dummy.prev_unit_id - (size_t)&dummy.is_special_unit);
    printf("Migration section: %zu bytes\n",
           (size_t)&dummy.total_records_written - (size_t)&dummy.prev_unit_id);
    printf("Statistics section: %zu bytes\n",
           (size_t)&dummy.unit_directory_path - (size_t)&dummy.total_records_written);
    printf("Directory path section: %zu bytes\n",
           (size_t)&dummy.reserved_expansion - (size_t)&dummy.unit_directory_path);
    printf("Reserved expansion section: %zu bytes\n",
           (size_t)&dummy.metadata_crc32 - (size_t)&dummy.reserved_expansion);
    printf("CRC section: %zu bytes\n",
           sizeof(rbpower_unit_metadata_t) - ((size_t)&dummy.metadata_crc32 - (size_t)&dummy));
    printf("===============================\n\n");
    
    return ESP_OK;
}