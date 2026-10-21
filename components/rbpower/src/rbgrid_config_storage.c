/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_config_storage.c
 * @brief Storage layer for RBGrid configuration on external Flash
 *
 * Reads and writes the per-domain configuration files on the external-flash LittleFS config
 * partition and delegates JSON processing to rbjson.c. Also implements the single-slot
 * full-configuration backup (/backup/) and the rbAmp fleet persistence (/rbamp.json).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "rbgrid_config_api.h"
#include "rbgrid.h"      /* legacy-id physics backfill (RBGRID_IS_I2C_UNIT / I2C_GET_*) */
#include "rbgrid_rbamp_config.h"   /* rbamp_bridge_config_t (fleet consistency filter) */
#include "rbgrid_config_storage.h"
#include "littlefs_raw.h"
#include "external_flash_config.h"
#include "rbjson.h"
#include "rbhome_types.h"   /* rbpower_home_lock/unlock — guard g_home_network read during save */
#include "esp_app_desc.h"   /* backup slot metadata: fw version stamp */
#include "cJSON.h"

static const char *TAG = "RBGRID_STORAGE";

#define CONFIG_BUFFER_SIZE 2048  ///< Generic config buffer size, in bytes (kept small to avoid stack overflow)

// Use single compact configuration file to save space
#define CONFIG_FILE_COMPACT   "/config.json"     ///< System configuration file
#define CONFIG_FILE_HARDWARE   "/hardware.json"  ///< Hardware configuration file
#define CONFIG_FILE_UNITS      "/units.json"     ///< Units configuration file
#define CONFIG_FILE_TARIFFS    "/tariffs.json"   ///< Tariff configuration file
#define CONFIG_COMPACT_SIZE   8192  ///< Maximum size of a config file other than units.json, in bytes

// Backup folder and file paths
#define BACKUP_FOLDER        "/backup"               ///< Backup folder on the config partition
#define BACKUP_FILE_DEFAULT  "/backup/config.json"   ///< Default backup of /config.json

// Forward declarations for internal use
esp_err_t rbgrid_storage_create_backup(const char* tag);


/* Disabled helper: read a file from external Flash. */
/*
 static esp_err_t read_config_file(const char *filename, char *buffer, size_t buffer_size)
{
    if (!filename || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t actual_size = 0;
    esp_err_t ret = littlefs_raw_read(filename, buffer, buffer_size - 1, &actual_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read %s", filename);
        return ret;
    }
    
    buffer[actual_size] = '\0';
    ESP_LOGI(TAG, "Read %s (%zu bytes)", filename, actual_size);
    return ESP_OK;
}
*/
/* Disabled helper: write a file to external Flash. */
/*
static esp_err_t write_config_file(const char *filename, const char *data)
{
    if (!filename || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t size = strlen(data);
    esp_err_t ret = littlefs_raw_write(filename, data, size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write %s", filename);
        return ret;
    }
    
    ESP_LOGI(TAG, "Wrote %s (%zu bytes)", filename, size);
    return ESP_OK;
}
*/

// ========== PUBLIC API ==========

/* Initialize the configuration storage (documented in rbgrid_config_storage.h). */
esp_err_t rbgrid_storage_init(void)
{
    ESP_LOGI(TAG, "Initializing RBGrid storage on EXTERNAL Flash...");
    
    // Check if external Flash is mounted
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash config partition not mounted!");
        return ESP_ERR_INVALID_STATE;
    }
    
    // The Flash should already be initialized and mounted by external_flash_init()
    // JSON files should already be created by create_config_file_structure()
    
    // Create backup folder if it doesn't exist
    esp_err_t ret = littlefs_raw_mkdir(BACKUP_FOLDER);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Backup folder created: %s", BACKUP_FOLDER);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Backup folder already exists: %s", BACKUP_FOLDER);
    } else {
        ESP_LOGW(TAG, "Failed to create backup folder: %s (%s)", BACKUP_FOLDER, esp_err_to_name(ret));
        // Not a critical error, continue
    }
    
    ESP_LOGI(TAG, "RBGrid storage ready on EXTERNAL Flash");
    return ESP_OK;
}

/* Load SYSTEM (extended_config) configurations from compact file */
esp_err_t rbgrid_storage_load_system(rbpower_extended_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char *buffer = (char *)malloc(CONFIG_COMPACT_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }
    
    // Read compact config file
    size_t size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_COMPACT, buffer, 
                                      CONFIG_COMPACT_SIZE - 1, &size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Compact config not found, using defaults");
        free(buffer);
        return ret;
    }
    buffer[size] = '\0';
    
    ESP_LOGI(TAG, "Read config from Flash (%zu bytes)", size);
    
    // Parse JSON string
    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON from Flash");
        return ESP_FAIL;
    }
    
    // Parse only system section (rbpower_extended_config_t fields)
    cJSON* system = cJSON_GetObjectItem(root, "system");
    if (system) {
        rbgrid_json_err_t json_ret = rbgrid_json_parse_system_section(system, config);
        if (json_ret != RBGRID_JSON_OK) {
            ESP_LOGE(TAG, "Failed to parse system configuration: %d", json_ret);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "No system section in config file");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Configuration loaded successfully from compact storage");
    return ESP_OK;
}

/* Save SYSTEM (extended_config) configurations to compact file */
esp_err_t rbgrid_storage_save_system(const rbpower_extended_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving configuration using JSON module...");
    
    // Create backup of current config before saving new one (if current config exists)
    if (littlefs_raw_exists(CONFIG_FILE_COMPACT)) {
        ESP_LOGI(TAG, "Creating backup of current configuration...");
        esp_err_t backup_ret = rbgrid_storage_create_backup(NULL);
        if (backup_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create backup: %s", esp_err_to_name(backup_ret));
            // Not critical, continue with save
        } else {
            ESP_LOGI(TAG, "Backup created successfully");
        }
    }
    
    // Use unified JSON function - it automatically creates compact format when g_json_context is NULL
    // Works with any rbpower_extended_config_t pointer (local or global)
    char* json_str = rbgrid_json_create_system_config(config);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to create JSON configuration");
        return ESP_ERR_NO_MEM;
    }
    
    size_t json_len = strlen(json_str);
    
    // Check size limit
    if (json_len >= CONFIG_COMPACT_SIZE) {
        ESP_LOGE(TAG, "JSON too large: %zu bytes (max: %d)", json_len, CONFIG_COMPACT_SIZE);
        free(json_str);
        return ESP_ERR_NO_MEM;
    }
    
    // Write to compact file
    esp_err_t ret = littlefs_raw_write(CONFIG_FILE_COMPACT, json_str, json_len);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to %s (%zu bytes)", CONFIG_FILE_COMPACT, json_len);
    } else {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(ret));
    }
    
    // Free allocated JSON string
    free(json_str);
    
    return ret;
}


/* Check if compact configuration file exists */
esp_err_t rbgrid_storage_check_files(void)
{
    ESP_LOGI(TAG, "Checking compact configuration on EXTERNAL Flash...");
    
    if (littlefs_raw_exists(CONFIG_FILE_COMPACT)) {
        // Get file size
        char buffer[64];
        size_t size = 0;
        if (littlefs_raw_read(CONFIG_FILE_COMPACT, buffer, sizeof(buffer), &size) == ESP_OK) {
            ESP_LOGI(TAG, "  [✓] %s (%zu bytes)", CONFIG_FILE_COMPACT, size);
        } else {
            ESP_LOGI(TAG, "  [✓] %s (exists)", CONFIG_FILE_COMPACT);
        }
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "  [✗] %s not found", CONFIG_FILE_COMPACT);
        
        // Create default compact config
        ESP_LOGI(TAG, "Creating default compact configuration...");
        rbpower_extended_config_t default_config = {
            .enable_home_network = false,
            .home_name = "Default",
            .location = "Unknown",
            .base_hardware_config = NULL
        };
        
        return rbgrid_storage_save_system(&default_config);
    }
}

/* Get storage statistics */
esp_err_t rbgrid_storage_get_stats(size_t *total_bytes, size_t *used_bytes)
{
    size_t total_blocks = 0, used_blocks = 0;
    esp_err_t ret = littlefs_raw_info(&total_blocks, &used_blocks);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Convert blocks to bytes (4KB per block)
    if (total_bytes) *total_bytes = total_blocks * 4096;
    if (used_bytes) *used_bytes = used_blocks * 4096;
    
    ESP_LOGI(TAG, "Storage: %zu KB used of %zu KB total", 
             (used_blocks * 4), (total_blocks * 4));
    
    return ESP_OK;
}

/* Check if backup exists */
esp_err_t rbgrid_storage_check_backup(const char* tag)
{
    const char* backup_file = tag ? tag : BACKUP_FILE_DEFAULT;
    
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if backup file exists
    bool exists = littlefs_raw_exists(backup_file);
    
    if (exists) {
        ESP_LOGI(TAG, "Backup exists: %s", backup_file);
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "Backup not found: %s", backup_file);
    return ESP_ERR_NOT_FOUND;
}

/* Create backup of current configuration */
esp_err_t rbgrid_storage_create_backup(const char* tag)
{
    const char* backup_file = tag ? tag : BACKUP_FILE_DEFAULT;
    
    ESP_LOGI(TAG, "Creating backup: %s", backup_file);
    
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Allocate buffer for file copy
    char *buffer = (char *)malloc(CONFIG_COMPACT_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }
    
    // Read current config
    size_t size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_COMPACT, buffer, 
                                      CONFIG_COMPACT_SIZE - 1, &size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read current config: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }
    buffer[size] = '\0';
    
    // Write to backup file
    ret = littlefs_raw_write(backup_file, buffer, size);
    free(buffer);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write backup: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Backup created successfully: %s (%zu bytes)", backup_file, size);
    return ESP_OK;
}

/* Restore configuration from backup */
esp_err_t rbgrid_storage_restore_from_backup(const char* tag)
{
    const char* backup_file = tag ? tag : BACKUP_FILE_DEFAULT;
    
    ESP_LOGI(TAG, "Restoring from backup: %s", backup_file);
    
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if backup exists
    esp_err_t ret = rbgrid_storage_check_backup(tag);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backup not found: %s", backup_file);
        return ret;
    }
    
    // Allocate buffer for file copy
    char *buffer = (char *)malloc(CONFIG_COMPACT_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }
    
    // Read backup file
    size_t size = 0;
    ret = littlefs_raw_read(backup_file, buffer, CONFIG_COMPACT_SIZE - 1, &size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read backup: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }
    buffer[size] = '\0';
    
    // Validate JSON format
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON in backup file");
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);
    
    // Write to main config file (replace)
    ret = littlefs_raw_write(CONFIG_FILE_COMPACT, buffer, size);
    free(buffer);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore config: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Configuration restored successfully from: %s (%zu bytes)", backup_file, size);
    return ESP_OK;
}

/* ===== Full-config backup/restore — MODULAR (one slot, last only). Each config domain = one littlefs file;
 * full backup/restore = the per-module file copied to/from /backup/<file>. No history (single slot). ===== */
static const char *const RBGRID_MODULE_CONFIGS[] = {
    CONFIG_FILE_COMPACT,    /* /config.json   — system */
    CONFIG_FILE_HARDWARE,   /* /hardware.json */
    CONFIG_FILE_UNITS,      /* /units.json    */
    CONFIG_FILE_TARIFFS,    /* /tariffs.json  */
    "/time.json",
    "/rbamp.json",          /* rbAmp fleet/unit bindings */
};
#define RBGRID_MODULE_CONFIG_COUNT (sizeof(RBGRID_MODULE_CONFIGS) / sizeof(RBGRID_MODULE_CONFIGS[0]))  ///< Number of files in the backup slot

/** @brief Build the backup-slot path of a config file ("/config.json" → "/backup/config.json"). */
static void rbgrid_backup_path_of(const char *f, char *out, size_t n)
{
    snprintf(out, n, "%s%s", BACKUP_FOLDER, f);   /* "/config.json" → "/backup/config.json" */
}
void *rbgrid_cfg_big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
/** @brief Copy a config file byte-for-byte via a PSRAM buffer sized for the largest file (units.json). */
static esp_err_t rbgrid_copy_file_raw(const char *src, const char *dst)
{
    char *buf = (char *)rbgrid_cfg_big_alloc(CONFIG_UNITS_SIZE);   /* the largest module file (units.json) */
    if (!buf) return ESP_ERR_NO_MEM;
    size_t sz = 0;
    esp_err_t r = littlefs_raw_read(src, buf, CONFIG_UNITS_SIZE, &sz);
    if (r == ESP_OK) r = littlefs_raw_write(dst, buf, sz);   /* binary copy by actual size */
    free(buf);
    return r;
}

/* Snapshot every present module config file → /backup/<file>. Overwrites the single slot. Returns count. */
int rbgrid_storage_backup_all(void)
{
    littlefs_raw_mkdir(BACKUP_FOLDER);
    int n = 0; char bp[80];
    for (size_t i = 0; i < RBGRID_MODULE_CONFIG_COUNT; i++) {
        const char *f = RBGRID_MODULE_CONFIGS[i];
        if (!littlefs_raw_exists(f)) continue;
        rbgrid_backup_path_of(f, bp, sizeof(bp));
        if (rbgrid_copy_file_raw(f, bp) == ESP_OK) n++;
        else ESP_LOGW(TAG, "backup: %s failed", f);
    }
    /* Slot metadata (for clients: "restore point from WHEN, on WHICH fw") — not a module config, so
     * restore_all/backup_count never touch it. ts is wall-clock (uptime-epoch before SNTP sync —
     * same convention as records; the client's epoch-floor heuristic applies). */
    {
        const esp_app_desc_t *app = esp_app_get_description();
        char meta[128];
        int ml = snprintf(meta, sizeof(meta), "{\"ts\":%lu,\"fw\":\"%s\",\"files\":%d}",
                          (unsigned long)time(NULL), app ? app->version : "?", n);
        if (ml > 0 && littlefs_raw_write(BACKUP_FOLDER "/_meta.json", meta, (size_t)ml) != ESP_OK)
            ESP_LOGW(TAG, "backup: _meta.json write failed (slot still valid)");
    }
    ESP_LOGI(TAG, "full config backup: %d module file(s)", n);
    return n;
}

/* Read the backup slot's metadata JSON (written by backup_all). Returns length, 0 if absent/invalid. */
int rbgrid_storage_backup_meta(char *out, size_t cap)
{
    size_t sz = 0;
    if (!out || cap < 2) return 0;
    if (littlefs_raw_read(BACKUP_FOLDER "/_meta.json", out, cap - 1, &sz) != ESP_OK || sz == 0) return 0;
    if (sz > cap - 1) sz = cap - 1;
    out[sz] = '\0';
    if (out[0] != '{') return 0;   /* corrupt — don't let it break the response JSON */
    return (int)sz;
}

/* Restore every module config file from /backup/<file> → /<file>. Reboot to apply (running config is in RAM).
 * Returns count restored. */
int rbgrid_storage_restore_all(void)
{
    int n = 0; char bp[80];
    for (size_t i = 0; i < RBGRID_MODULE_CONFIG_COUNT; i++) {
        const char *f = RBGRID_MODULE_CONFIGS[i];
        rbgrid_backup_path_of(f, bp, sizeof(bp));
        if (!littlefs_raw_exists(bp)) continue;
        if (rbgrid_copy_file_raw(bp, f) == ESP_OK) n++;
        else ESP_LOGW(TAG, "restore: %s failed", f);
    }
    ESP_LOGW(TAG, "full config restore: %d module file(s) — reboot to apply", n);
    return n;
}

/* Count of module config backups present in the slot. */
int rbgrid_storage_backup_count(void)
{
    int n = 0; char bp[80];
    for (size_t i = 0; i < RBGRID_MODULE_CONFIG_COUNT; i++) {
        rbgrid_backup_path_of(RBGRID_MODULE_CONFIGS[i], bp, sizeof(bp));
        if (littlefs_raw_exists(bp)) n++;
    }
    return n;
}

/* Save HARDWARE configuration to compact file
 * Works with both global context (g_extended_config->base_hardware_config) or local contexts */
esp_err_t rbgrid_storage_save_hardware(const rbpower_hardware_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving hardware configuration using JSON module...");
    
    // Create backup of current config before saving new one (if current config exists)
    if (littlefs_raw_exists(CONFIG_FILE_HARDWARE)) {
/*
        ESP_LOGI(TAG, "Creating backup of current configuration...");
        esp_err_t backup_ret = rbgrid_storage_create_backup(NULL);
        if (backup_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create backup: %s", esp_err_to_name(backup_ret));
            // Not critical, continue with save
        }
*/            
    }
    
    // Use JSON function to create hardware config
    // Pass NULL for context to get compact format
    char* json_str = rbgrid_json_create_hardware_config(config, NULL);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to create JSON hardware configuration");
        return ESP_ERR_NO_MEM;
    }
    
    size_t json_len = strlen(json_str);
    
    // Check size limit
    if (json_len >= CONFIG_COMPACT_SIZE) {
        ESP_LOGE(TAG, "JSON too large: %zu bytes (max: %d)", json_len, CONFIG_COMPACT_SIZE);
        free(json_str);
        return ESP_ERR_NO_MEM;
    }
    
    // Write to compact file
    esp_err_t ret = littlefs_raw_write(CONFIG_FILE_HARDWARE, json_str, json_len);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Hardware config saved to %s (%zu bytes)", CONFIG_FILE_HARDWARE, json_len);
    } else {
        ESP_LOGE(TAG, "Failed to save hardware config: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "JSON string: %s", json_str);
    ESP_LOGI(TAG, "JSON len = %d", json_len);
    // Free allocated JSON string
    free(json_str);
    
    return ret;
}

/* Load HARDWARE configuration from compact file
 * Works with both global context (g_extended_config->base_hardware_config) or local contexts */
esp_err_t rbgrid_storage_load_hardware(rbpower_hardware_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char *buffer = (char *)malloc(CONFIG_COMPACT_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }
    
    // Read compact config file
    size_t size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_HARDWARE, buffer, 
                                      CONFIG_COMPACT_SIZE - 1, &size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Compact config not found, using defaults");
        free(buffer);
        return ret;
    }
    buffer[size] = '\0';
    
    ESP_LOGI(TAG, "Read hardware config from Flash (%zu bytes)", size);
    ESP_LOGI(TAG, "JSON string: %s", buffer);
 
    // Parse JSON string
    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON from Flash");
        return ESP_FAIL;
    }
    
    // Check if this is a hardware configuration JSON by checking the "type" field
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "hardware") != 0) {
        ESP_LOGE(TAG, "Invalid JSON type - expected 'hardware', got: %s", 
                 type ? (cJSON_IsString(type) ? type->valuestring : "non-string") : "null");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Parse data section if it exists
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data) {
        rbgrid_json_err_t json_ret = rbgrid_json_parse_hardware_section(data, config);
        if (json_ret != RBGRID_JSON_OK) {
            ESP_LOGE(TAG, "Failed to parse hardware configuration: %d", json_ret);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "No data section in hardware config file, using empty config");
    }
    
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Hardware configuration loaded successfully from compact storage");
    return ESP_OK;
}

/* Save the units configuration to /units.json (documented in rbgrid_config_storage.h). */
esp_err_t rbgrid_storage_save_units(const char *json_str)
{
    ESP_LOGI(TAG, "Saving units configuration...");
    
    // Check if external Flash is mounted
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    char* json_to_save = NULL;
    
    if (json_str) {
        // Use provided JSON string
        json_to_save = (char*)json_str;
        ESP_LOGI(TAG, "Using provided JSON string");
    } else {
        // Create JSON from current units state in RAM
        // Pass NULL to use internal g_json_context from rbjson.c
        // Lock g_home_network for a consistent read vs the core-0/esp_timer mutators.
        rbpower_home_lock();
        json_to_save = rbgrid_json_create_units_config(NULL);
        rbpower_home_unlock();
        if (!json_to_save) {
            ESP_LOGE(TAG, "Failed to create JSON units configuration");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Created JSON from current units state");
    }
    
    size_t json_len = strlen(json_to_save);
    
    // Check size limit
    if (json_len >= CONFIG_UNITS_SIZE) {
        ESP_LOGE(TAG, "JSON too large: %zu bytes (max: %d)", json_len, CONFIG_UNITS_SIZE);
        if (!json_str) {
            free(json_to_save);
        }
        return ESP_ERR_NO_MEM;
    }
    
    // Check if file exists, create if not
    if (!littlefs_raw_exists(CONFIG_FILE_UNITS)) {
        ESP_LOGI(TAG, "Units config file does not exist, will be created");
    }
    
    // Write to file
    esp_err_t ret = littlefs_raw_write(CONFIG_FILE_UNITS, json_to_save, json_len);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Units config saved to %s (%zu bytes)", CONFIG_FILE_UNITS, json_len);
        
    } else {
        ESP_LOGE(TAG, "Failed to save units config: %s", esp_err_to_name(ret));
    }
    
    // Free allocated JSON string if we created it
    if (!json_str && json_to_save) {
        free(json_to_save);
    }
    if (ret == ESP_OK) rbpower_hw_rev_bump();   /* bind/unbind/rename → clients re-read /api/v1/sensors */
    
    return ret;
}

/**
 * @brief Rebuild a persisted I2C (rbAmp) unit's full definition from its units.json object.
 *
 * The unit is queued in the config builder with its stored identity (opaque unit_id, point,
 * device_uid/channel_idx); rbpower_home_init registers it like any other builder unit. Older
 * records without the identity fields get device_uid/channel_idx backfilled from the legacy id
 * bits, and point/caps from rbgrid_config_add_persisted_unit. The fleet is the consistency
 * filter: a unit whose id no fleet entry carries was unbound (deleted) and must not reappear.
 * @param[in] config_handle Configuration builder handle.
 * @param[in] unit          The unit's JSON object.
 * @param[in] source        The unit's first source JSON object.
 * @param[in] stat_period   Statistics class parsed from the unit.
 * @param[in] fleet         rbAmp fleet configuration, or NULL to skip the fleet filter.
 */
static void restore_persisted_i2c_unit(rbgrid_config_handle_t config_handle, cJSON* unit,
                                       cJSON* source, rbpower_stat_period_type_t stat_period,
                                       const rbamp_bridge_config_t* fleet)
{
    rbpower_unit_definition_t def;
    memset(&def, 0, sizeof(def));

    cJSON* jid = cJSON_GetObjectItem(unit, "unit_id");
    cJSON* jname = cJSON_GetObjectItem(unit, "name");
    if (!cJSON_IsNumber(jid) || jid->valueint <= 0 || jid->valueint > 0xFFFF) {
        ESP_LOGW(TAG, "persisted i2c unit without a valid unit_id — skipped");
        return;
    }
    def.unit_id = (rbpower_unit_id_t)jid->valueint;
    strncpy(def.name, (cJSON_IsString(jname) && jname->valuestring[0]) ? jname->valuestring : "rbAmp",
            sizeof(def.name) - 1);
    cJSON* jloc = cJSON_GetObjectItem(unit, "location");
    strncpy(def.location, (cJSON_IsString(jloc) && jloc->valuestring[0]) ? jloc->valuestring : "rbAmp I2C",
            sizeof(def.location) - 1);
    cJSON* jsn = cJSON_GetObjectItem(unit, "short_name"); /* cosmetic, "" = unset */
    if (cJSON_IsString(jsn)) strncpy(def.short_name, jsn->valuestring, sizeof(def.short_name) - 1);

    def.type = RBPOWER_METRIC_CURRENT_LOAD;   /* i2c units are current loads today */
    def.role = RBPOWER_ROLE_DEVICE_LOAD;
    cJSON* jrole = cJSON_GetObjectItem(unit, "role");
    if (cJSON_IsString(jrole)) {
        static const char* role_text[] = { "MAIN_SUPPLY", "ALTERNATIVE_SUPPLY", "CIRCUIT_BREAKER",
            "DEVICE_LOAD", "ZONE_LOAD", "RENEWABLE_SOURCE", "STORAGE_SYSTEM", "EV_CHARGING",
            "HEATING_COOLING", "SMART_APPLIANCE" };
        for (int i = 0; i < (int)(sizeof(role_text)/sizeof(role_text[0])); i++) {
            if (strcmp(jrole->valuestring, role_text[i]) == 0) { def.role = (rbpower_load_role_t)i; break; }
        }
    }
    def.stat_class = stat_period;

    def.sources[0].source_type = RBPOWER_SOURCE_I2C_SENSOR;
    cJSON* jsid  = cJSON_GetObjectItem(source, "source_id");
    cJSON* jprof = cJSON_GetObjectItem(source, "sensor_profile_id");
    def.sources[0].source_id         = cJSON_IsNumber(jsid)  ? (uint8_t)jsid->valueint  : 0;
    def.sources[0].sensor_profile_id = cJSON_IsNumber(jprof) ? (uint8_t)jprof->valueint : 0;
    def.sources[0].weight_factor = 1.0f;
    def.sources[0].is_primary    = true;
    def.sources[0].invert_sign   = cJSON_IsTrue(cJSON_GetObjectItem(source, "invert_sign"));
    def.sources_count = 1;
    def.parent_unit_id = RBPOWER_HOME_INVALID_ID;

    def.bidirectional_metering = cJSON_IsTrue(cJSON_GetObjectItem(unit, "bidirectional_metering"));
    cJSON* jacc = cJSON_GetObjectItem(unit, "accumulate_energy");
    def.accumulate_energy = jacc ? cJSON_IsTrue(jacc) : true;
    cJSON* jrv = cJSON_GetObjectItem(unit, "rated_voltage_v");
    cJSON* jmc = cJSON_GetObjectItem(unit, "max_current_a");
    cJSON* jrp = cJSON_GetObjectItem(unit, "rated_power_w");
    def.rated_voltage_v = cJSON_IsNumber(jrv) ? (float)jrv->valuedouble : 230.0f;
    def.max_current_a   = cJSON_IsNumber(jmc) ? (float)jmc->valuedouble : 10.0f;
    def.rated_power_w   = cJSON_IsNumber(jrp) ? (float)jrp->valuedouble
                                              : def.max_current_a * def.rated_voltage_v;
    {   /* EV station overrides (0 = system.ev) */
        cJSON* jev = cJSON_GetObjectItem(unit, "charge_threshold_w");
        if (cJSON_IsNumber(jev) && jev->valuedouble >= 0 && jev->valuedouble <= 100000) def.ev_charge_threshold_w = (uint32_t)jev->valuedouble;
        jev = cJSON_GetObjectItem(unit, "idle_timeout_min");
        if (cJSON_IsNumber(jev) && jev->valuedouble >= 0 && jev->valuedouble <= 1440) def.ev_idle_timeout_min = (uint16_t)jev->valuedouble;
    }
    cJSON* jth = cJSON_GetObjectItem(unit, "thresholds");
    if (cJSON_IsObject(jth)) {
        cJSON* v;
        v = cJSON_GetObjectItem(jth, "over_power_w");    def.thresholds.over_power_w    = cJSON_IsNumber(v) ? (float)v->valuedouble : def.rated_power_w * 0.9f;
        v = cJSON_GetObjectItem(jth, "under_voltage_v"); def.thresholds.under_voltage_v = cJSON_IsNumber(v) ? (float)v->valuedouble : 207.0f;
        v = cJSON_GetObjectItem(jth, "over_current_a");  def.thresholds.over_current_a  = cJSON_IsNumber(v) ? (float)v->valuedouble : def.max_current_a * 0.9f;
        v = cJSON_GetObjectItem(jth, "over_voltage_v");  def.thresholds.over_voltage_v  = cJSON_IsNumber(v) ? (float)v->valuedouble : 253.0f;
        def.thresholds.enable_alerts = cJSON_IsTrue(cJSON_GetObjectItem(jth, "enable_alerts"));
    } else {
        def.thresholds.over_power_w    = def.rated_power_w * 0.9f;
        def.thresholds.over_current_a  = def.max_current_a * 0.9f;
        def.thresholds.under_voltage_v = 207.0f;
        def.thresholds.over_voltage_v  = 253.0f;
        def.thresholds.enable_alerts   = true;
    }
    cJSON* jb = cJSON_GetObjectItem(unit, "billing");
    cJSON* jcost = cJSON_IsObject(jb) ? cJSON_GetObjectItem(jb, "cost_per_kwh") : NULL;
    def.billing.cost_per_kwh = cJSON_IsNumber(jcost) ? (float)jcost->valuedouble : 5.50f;
    cJSON* jact = cJSON_GetObjectItem(unit, "is_active");
    def.is_active = jact ? cJSON_IsTrue(jact) : true;

    /* Identity fields; older JSON without them → backfill physics from the legacy id bits. */
    cJSON* jpt = cJSON_GetObjectItem(unit, "point");
    if (cJSON_IsString(jpt)) strncpy(def.point, jpt->valuestring, sizeof(def.point) - 1);
    cJSON* jcaps = cJSON_GetObjectItem(unit, "caps");
    def.caps = cJSON_IsNumber(jcaps) ? (uint32_t)jcaps->valuedouble : 0;
    cJSON* jdev = cJSON_GetObjectItem(unit, "device_uid");
    cJSON* jchx = cJSON_GetObjectItem(unit, "channel_idx");
    if (cJSON_IsNumber(jdev)) {
        def.device_uid  = (uint64_t)jdev->valuedouble;
        def.channel_idx = cJSON_IsNumber(jchx) ? (uint8_t)jchx->valueint : def.sources[0].source_id;
    } else if (RBGRID_IS_I2C_UNIT(def.unit_id)) {
        def.device_uid  = RBGRID_I2C_GET_MODULE(def.unit_id);
        def.channel_idx = RBGRID_I2C_GET_CHANNEL(def.unit_id);
    } else {
        ESP_LOGW(TAG, "persisted i2c unit 0x%04X: no physics fields and a non-legacy id — skipped",
                 (unsigned)def.unit_id);
        return;
    }
    cJSON* jph = cJSON_GetObjectItem(unit, "phase");
    cJSON* jgr = cJSON_GetObjectItem(unit, "group_id");
    cJSON* jrt = cJSON_GetObjectItem(unit, "role_trunk");
    def.phase      = cJSON_IsNumber(jph) ? (uint8_t)jph->valueint : 0;
    def.group_id   = cJSON_IsNumber(jgr) ? (uint16_t)jgr->valueint : 0;
    def.role_trunk = cJSON_IsNumber(jrt) ? (uint8_t)jrt->valueint : 0;

    /* Fleet consistency: restore only a unit some fleet entry still binds (unbind sets entry 0). */
    if (fleet) {
        bool bound = false;
        for (uint8_t m = 0; m < fleet->module_count && m < RBAMP_MAX_MODULES && !bound; ++m) {
            for (uint8_t e = 0; e < fleet->modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
                if (fleet->modules[m].entries[e].unit_id == (uint16_t)def.unit_id) { bound = true; break; }
            }
        }
        if (!bound) {
            ESP_LOGW(TAG, "persisted i2c unit 0x%04X '%s' not bound in fleet — not restored (deleted)",
                     (unsigned)def.unit_id, def.name);
            return;
        }
    }

    int rc = rbgrid_config_add_persisted_unit(config_handle, &def);
    if (rc != RBGRID_CONFIG_OK) {
        ESP_LOGE(TAG, "persisted i2c unit 0x%04X '%s' restore failed: %d",
                 (unsigned)def.unit_id, def.name, rc);
    }
}

esp_err_t rbgrid_storage_load_units(rbgrid_config_handle_t config_handle)
{
    if (!config_handle) {
        ESP_LOGE(TAG, "Invalid config handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Loading units configuration from Flash...");
    
    // Check if external Flash is mounted
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if file exists
    if (!littlefs_raw_exists(CONFIG_FILE_UNITS)) {
        ESP_LOGW(TAG, "Units config file %s does not exist", CONFIG_FILE_UNITS);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Allocate buffer with maximum size
    char* buffer = (char*)rbgrid_cfg_big_alloc(CONFIG_UNITS_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for JSON");
        return ESP_ERR_NO_MEM;
    }
    
    // Read file
    size_t size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_UNITS, buffer, 
                                      CONFIG_UNITS_SIZE - 1, &size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read units config: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }
    buffer[size] = '\0';
    
    ESP_LOGI(TAG, "Read units config from Flash (%zu bytes)", size);
    ESP_LOGD(TAG, "JSON string: %s", buffer);
    
    // Parse JSON string
    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON from Flash");
        return ESP_FAIL;
    }
    
    // Check if this is a units configuration JSON by checking the "type" field
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type) || 
        (strcmp(type->valuestring, "units_config") != 0 && strcmp(type->valuestring, "units") != 0)) {
        ESP_LOGE(TAG, "Invalid JSON type - expected 'units_config' or 'units', got: %s", 
                 type ? (cJSON_IsString(type) ? type->valuestring : "non-string") : "null");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get units count
    cJSON* units_count_json = cJSON_GetObjectItem(root, "units_count");
    int units_count = 0;
    if (units_count_json && cJSON_IsNumber(units_count_json)) {
        units_count = units_count_json->valueint;
        ESP_LOGI(TAG, "Units count in config: %d", units_count);
    }
    
    // Parse data section if it exists
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) {
        ESP_LOGW(TAG, "No data section in units config");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Get units array
    cJSON* units_array = cJSON_GetObjectItem(data, "units");
    if (!units_array || !cJSON_IsArray(units_array)) {
        ESP_LOGE(TAG, "No units array in data section");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Legacy ADC-typed channel ids (default units fed by the rbAmp bridge) are not validated here;
    // they are checked when the unit is created.
    ESP_LOGW(TAG, "ADC channel validation skipped - will be validated during unit creation");

    /* Point tombstones (retired slugs) — load BEFORE units so autogen avoids them. */
    {
        cJSON* tarr = cJSON_GetObjectItem(data, "point_tombstones");
        if (cJSON_IsArray(tarr)) {
            cJSON* t = NULL;
            cJSON_ArrayForEach(t, tarr) {
                if (cJSON_IsString(t)) rbgrid_point_tombstone_add(t->valuestring);
            }
        }
    }
    /* The fleet is the consistency filter for restoring I2C units (deleted units don't reappear). */
    rbamp_bridge_config_t t7_fleet;
    bool t7_have_fleet = (rbgrid_storage_load_rbamp(&t7_fleet) == ESP_OK);

    // Process each unit
    cJSON* unit = NULL;
    cJSON_ArrayForEach(unit, units_array) {
        // Get unit type
        cJSON* unit_type = cJSON_GetObjectItem(unit, "type");
        if (!unit_type || !cJSON_IsString(unit_type)) {
            ESP_LOGW(TAG, "Unit without type, skipping");
            continue;
        }
        
        // Get unit role
        cJSON* unit_role = cJSON_GetObjectItem(unit, "role");
        const char* role_str = (unit_role && cJSON_IsString(unit_role)) ? unit_role->valuestring : "";
        
        // Get unit name
        cJSON* unit_name = cJSON_GetObjectItem(unit, "name");
        const char* name = (unit_name && cJSON_IsString(unit_name)) ? unit_name->valuestring : "Unknown";
        
        // Get stat_class (period)
        cJSON* stat_class = cJSON_GetObjectItem(unit, "stat_class");
        rbpower_stat_period_type_t stat_period = RBPOWER_STAT_PERIOD_1HOUR; // Default
        if (stat_class && cJSON_IsString(stat_class)) {
            // Convert string to enum using the correct values
            const char* stat_str = stat_class->valuestring;
            if (strcmp(stat_str, "3SEC") == 0) stat_period = RBPOWER_STAT_PERIOD_3SEC;
            else if (strcmp(stat_str, "15MIN") == 0) stat_period = RBPOWER_STAT_PERIOD_15MIN;
            else if (strcmp(stat_str, "1HOUR") == 0) stat_period = RBPOWER_STAT_PERIOD_1HOUR;
            else if (strcmp(stat_str, "DAILY") == 0) stat_period = RBPOWER_STAT_PERIOD_DAILY;
            else if (strcmp(stat_str, "WEEKLY") == 0) stat_period = RBPOWER_STAT_PERIOD_WEEKLY;
            else if (strcmp(stat_str, "MONTHLY") == 0) stat_period = RBPOWER_STAT_PERIOD_MONTHLY;
            else if (strcmp(stat_str, "YEARLY") == 0) stat_period = RBPOWER_STAT_PERIOD_YEARLY;
        }
        
        // Get sources array
        cJSON* sources = cJSON_GetObjectItem(unit, "sources");
        if (!sources || !cJSON_IsArray(sources)) {
            ESP_LOGW(TAG, "Unit '%s' without sources, skipping", name);
            continue;
        }
        
        // Process first source (for now we support only one source per unit)
        cJSON* source = cJSON_GetArrayItem(sources, 0);
        if (!source) {
            ESP_LOGW(TAG, "Unit '%s' has empty sources array, skipping", name);
            continue;
        }
        
        // Get source type
        cJSON* source_type = cJSON_GetObjectItem(source, "source_type");
        if (!source_type || !cJSON_IsString(source_type)) {
            ESP_LOGW(TAG, "Unit '%s' source without type, skipping", name);
            continue;
        }
        
        /* Restore I2C (rbAmp) units with their persisted opaque identity. ESP-NOW units are
         * rebuilt from the node registry at boot — skipped here by design. */
        if (strcmp(source_type->valuestring, "I2C_SENSOR") == 0) {
            restore_persisted_i2c_unit(config_handle, unit, source, stat_period,
                                       t7_have_fleet ? &t7_fleet : NULL);
            continue;
        }
        if (strcmp(source_type->valuestring, "ADC_SENSOR") != 0) {
            ESP_LOGW(TAG, "Unit '%s' has unsupported source type '%s', skipping",
                     name, source_type->valuestring);
            continue;
        }
        
        // Get source_id (legacy ADC-typed channel id of a default unit fed by the rbAmp bridge)
        cJSON* source_id = cJSON_GetObjectItem(source, "source_id");
        if (!source_id || !cJSON_IsNumber(source_id)) {
            ESP_LOGW(TAG, "Unit '%s' source without ID, skipping", name);
            continue;
        }
        
        uint8_t adc_channel = (uint8_t)source_id->valueint;

        /* Per-install CT orientation: invert_sign must be parsed back, otherwise a saved value
         * silently reverts to false on reboot. weight_factor / is_primary are not parsed yet
         * because nothing consumes them at runtime. */
        bool src_invert = cJSON_IsTrue(cJSON_GetObjectItem(source, "invert_sign"));
        
        // Channel validation is done by rbgrid_create_adc_unit_id when creating the unit
        ESP_LOGI(TAG, "Unit '%s' uses ADC channel %d", name, adc_channel);
        
        // Create the legacy ADC-typed unit_uid for this channel
        rbgrid_unit_uid_t sensor_uid = rbgrid_create_adc_unit_id(adc_channel);
        
        ESP_LOGI(TAG, "Processing unit: type=%s, role=%s, name=%s, adc_ch=%d, uid=0x%04X",
                 unit_type->valuestring, role_str, name, adc_channel, sensor_uid);
        
        // Create unit based on type and role
        rbgrid_config_error_t config_err = RBGRID_CONFIG_OK;
        
        if (strcmp(unit_type->valuestring, "VOLTAGE_BUS") == 0 && 
            strcmp(role_str, "MAIN_SUPPLY") == 0) {
            // Create Voltage Bus
            config_err = rbgrid_config_set_voltage_bus(config_handle, sensor_uid, name);
            if (config_err != RBGRID_CONFIG_OK) {
                ESP_LOGE(TAG, "Failed to configure voltage bus: %d", config_err);
            } else {
                ESP_LOGI(TAG, "✅ Voltage bus configured: %s", name);
            }
            
        } else if (strcmp(unit_type->valuestring, "CURRENT_LOAD") == 0 && 
                   strcmp(role_str, "MAIN_SUPPLY") == 0) {
            // Create Main Supply
            config_err = rbgrid_config_set_main_supply(config_handle, sensor_uid, name, stat_period);
            if (config_err != RBGRID_CONFIG_OK) {
                ESP_LOGE(TAG, "Failed to configure main supply: %d", config_err);
            } else {
                ESP_LOGI(TAG, "✅ Main supply configured: %s", name);
            }
            
        } else if (strcmp(unit_type->valuestring, "CURRENT_LOAD") == 0 && 
                   strcmp(role_str, "ALTERNATIVE_SUPPLY") == 0) {
            // Create Alternative Supply
            config_err = rbgrid_config_set_alternative_supply(config_handle, sensor_uid, name, stat_period);
            if (config_err != RBGRID_CONFIG_OK) {
                ESP_LOGE(TAG, "Failed to configure alternative supply: %d", config_err);
            } else {
                ESP_LOGI(TAG, "✅ Alternative supply configured: %s", name);
            }
            
        } else {
            // Create regular unit
            config_err = rbgrid_config_add_unit(config_handle, sensor_uid, name, stat_period);
            if (config_err != RBGRID_CONFIG_OK) {
                ESP_LOGE(TAG, "Failed to configure unit '%s': %d", name, config_err);
            } else {
                ESP_LOGI(TAG, "✅ Unit configured: %s", name);
            }
        }

        /* Apply the parsed source options to whichever unit the branch above produced/updated. */
        if (config_err == RBGRID_CONFIG_OK && src_invert) {
            rbgrid_config_set_source_invert(config_handle, sensor_uid, true);
        }

        /* This legacy path recreates units via the config API, which does not carry the persisted
         * declarative fields (rated_power_w / max_current_a / phase / short_name / group_id /
         * role_trunk / EV overrides). Re-apply them onto the freshly created definition, as the
         * I2C path does. Apply to the CONFIG copy too: at boot this runs before the home network
         * exists, so the live lookup returns NULL; the config copy is what the home network is
         * built from. */
        if (config_err == RBGRID_CONFIG_OK) {
          rbpower_unit_definition_t* defs[2] = { rbgrid_config_unit_def_mutable(config_handle, sensor_uid),
                                                 rbpower_home_get_unit_definition_mutable(sensor_uid) };
          for (int di = 0; di < 2; di++) {
            rbpower_unit_definition_t* def = defs[di];
            if (def) {
                cJSON* j;
                if ((j = cJSON_GetObjectItem(unit, "rated_power_w")) && cJSON_IsNumber(j))
                    def->rated_power_w = (float)j->valuedouble;
                if ((j = cJSON_GetObjectItem(unit, "max_current_a")) && cJSON_IsNumber(j))
                    def->max_current_a = (float)j->valuedouble;
                if ((j = cJSON_GetObjectItem(unit, "phase")) && cJSON_IsNumber(j))
                    def->phase = (uint8_t)j->valueint;
                if ((j = cJSON_GetObjectItem(unit, "group_id")) && cJSON_IsNumber(j))
                    def->group_id = (uint16_t)j->valueint;
                if ((j = cJSON_GetObjectItem(unit, "role_trunk")) && cJSON_IsNumber(j))
                    def->role_trunk = (uint8_t)j->valueint;
                if ((j = cJSON_GetObjectItem(unit, "charge_threshold_w")) && cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 100000)
                    def->ev_charge_threshold_w = (uint32_t)j->valuedouble;
                if ((j = cJSON_GetObjectItem(unit, "idle_timeout_min")) && cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 1440)
                    def->ev_idle_timeout_min = (uint16_t)j->valuedouble;
                if ((j = cJSON_GetObjectItem(unit, "short_name")) && cJSON_IsString(j)) {
                    strncpy(def->short_name, j->valuestring, sizeof(def->short_name) - 1);
                    def->short_name[sizeof(def->short_name) - 1] = '\0';
                }
            }
          }
        }
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Units configuration loaded and applied successfully");
    return ESP_OK;
}

// ================================================================================================
// TIME CONFIGURATION STORAGE
// ================================================================================================

#define CONFIG_FILE_TIME "/time.json"   ///< Time configuration file

/* Save time configuration to Flash
 * Saves the JSON configuration received from web interface directly to file */
esp_err_t rbgrid_storage_save_time(const char* json_config)
{
    if (!json_config) {
        ESP_LOGE(TAG, "Invalid JSON config for time save");
        return ESP_ERR_INVALID_ARG;
    }

    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Saving time configuration to %s", CONFIG_FILE_TIME);

    // Validate JSON
    cJSON* root = cJSON_Parse(json_config);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON format");
        return ESP_ERR_INVALID_ARG;
    }

    // Check that it has the config structure we expect
    cJSON* config = cJSON_GetObjectItem(root, "config");
    if (!config) {
        ESP_LOGE(TAG, "Missing 'config' object in JSON");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);

    // Create backup of current time config if exists
    if (littlefs_raw_exists(CONFIG_FILE_TIME)) {
        ESP_LOGI(TAG, "Creating backup of current time configuration...");
        char backup_path[64];
        snprintf(backup_path, sizeof(backup_path), "%s/time.json", BACKUP_FOLDER);

        // Ensure backup folder exists
        littlefs_raw_mkdir(BACKUP_FOLDER);

        // Copy current to backup
        char current_config[4096];  // Buffer for config file
        size_t actual_size = 0;
        if (littlefs_raw_read(CONFIG_FILE_TIME, current_config, sizeof(current_config), &actual_size) == ESP_OK) {
            littlefs_raw_write(backup_path, current_config, actual_size);
            ESP_LOGI(TAG, "Time config backup created");
        }
    }

    // Write new configuration
    size_t json_len = strlen(json_config);
    esp_err_t ret = littlefs_raw_write(CONFIG_FILE_TIME, json_config, json_len);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ Time configuration saved successfully (%zu bytes)", json_len);
        // Filesystem will be synced automatically when needed
    } else {
        ESP_LOGE(TAG, "Failed to save time configuration: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* Load time configuration from Flash and apply it
 * Uses API functions to apply configuration to ensure consistency */
esp_err_t rbgrid_storage_load_time(rbgrid_config_handle_t config_handle)
{
    if (!config_handle) {
        ESP_LOGE(TAG, "Invalid config handle");
        return ESP_ERR_INVALID_ARG;
    }

    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Loading time configuration from %s", CONFIG_FILE_TIME);

    // Check if file exists
    if (!littlefs_raw_exists(CONFIG_FILE_TIME)) {
        ESP_LOGW(TAG, "Time configuration file not found");
        return ESP_ERR_NOT_FOUND;
    }

    // Read configuration file
    char buffer[4096];  // Buffer for config file
    size_t actual_size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_TIME, buffer, sizeof(buffer) - 1, &actual_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time configuration: %s", esp_err_to_name(ret));
        return ret;
    }
    buffer[actual_size < sizeof(buffer) ? actual_size : sizeof(buffer) - 1] = '\0';   /* cJSON_Parse needs the NUL */


    // Parse JSON
    cJSON* root = cJSON_Parse(buffer);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse time configuration JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Get config object
    cJSON* config = cJSON_GetObjectItem(root, "config");
    if (!config) {
        ESP_LOGE(TAG, "Missing 'config' object in time configuration");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Apply configuration using API functions for consistency
    rbgrid_config_error_t config_err = RBGRID_CONFIG_OK;

    // 1. SNTP configuration
    cJSON* sntp_server = cJSON_GetObjectItem(config, "sntp_server");
    cJSON* sntp_backup = cJSON_GetObjectItem(config, "sntp_server_backup");
    cJSON* timezone = cJSON_GetObjectItem(config, "timezone");

    if (sntp_server && sntp_backup && timezone) {
        config_err = rbgrid_config_set_sntp_time(
            config_handle,
            cJSON_IsString(sntp_server) ? sntp_server->valuestring : "pool.ntp.org",
            cJSON_IsString(sntp_backup) ? sntp_backup->valuestring : "time.nist.gov",
            cJSON_IsString(timezone) ? timezone->valuestring : "UTC+0"
        );
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to apply SNTP configuration: %d", config_err);
        } else {
            ESP_LOGI(TAG, "✅ SNTP configuration applied");
        }
    }

    // 2. Fallback configuration
    cJSON* enable_rtc_fallback = cJSON_GetObjectItem(config, "enable_rtc_fallback");
    cJSON* enable_cpu_fallback = cJSON_GetObjectItem(config, "enable_cpu_time_fallback");
    cJSON* manual_time_us = cJSON_GetObjectItem(config, "manual_time_us");

    bool rtc_fallback = enable_rtc_fallback ? cJSON_IsTrue(enable_rtc_fallback) : false;
    bool cpu_fallback = enable_cpu_fallback ? cJSON_IsTrue(enable_cpu_fallback) : true;
    uint64_t manual_time = 1704067200; // Default: Jan 1, 2024

    if (manual_time_us && cJSON_IsNumber(manual_time_us)) {
        // Convert microseconds to seconds
        manual_time = (uint64_t)(manual_time_us->valuedouble / 1000000.0);
    }

    config_err = rbgrid_config_set_time_fallback(
        config_handle,
        rtc_fallback,
        cpu_fallback,
        manual_time
    );
    if (config_err != RBGRID_CONFIG_OK) {
        ESP_LOGW(TAG, "Failed to apply time fallback configuration: %d", config_err);
    } else {
        ESP_LOGI(TAG, "✅ Time fallback configuration applied");
    }

    // 3. Advanced settings
    cJSON* resync_interval = cJSON_GetObjectItem(config, "resync_interval_sec");
    cJSON* sntp_timeout = cJSON_GetObjectItem(config, "sntp_timeout_ms");
    cJSON* sync_retry = cJSON_GetObjectItem(config, "sync_retry_count");
    cJSON* enable_dst = cJSON_GetObjectItem(config, "enable_dst");

    uint32_t resync_sec = resync_interval ? (uint32_t)resync_interval->valueint : 3600;
    uint32_t timeout_ms = sntp_timeout ? (uint32_t)sntp_timeout->valueint : 10000;
    uint8_t retry_count = sync_retry ? (uint8_t)sync_retry->valueint : 3;
    bool dst_enabled = enable_dst ? cJSON_IsTrue(enable_dst) : false;

    config_err = rbgrid_config_set_time_advanced(
        config_handle,
        resync_sec,
        timeout_ms,
        retry_count,
        dst_enabled
    );
    if (config_err != RBGRID_CONFIG_OK) {
        ESP_LOGW(TAG, "Failed to apply advanced time settings: %d", config_err);
    } else {
        ESP_LOGI(TAG, "✅ Advanced time settings applied");
    }

    // 4. Additional settings that may not have API functions yet
    cJSON* enable_time_sync = cJSON_GetObjectItem(config, "enable_time_sync");
    cJSON* primary_source = cJSON_GetObjectItem(config, "primary_source");
    cJSON* force_sync = cJSON_GetObjectItem(config, "force_sync_on_startup");
    cJSON* time_logging = cJSON_GetObjectItem(config, "enable_time_logging");

    // Log what was loaded for debugging
    ESP_LOGI(TAG, "Time configuration loaded:");
    ESP_LOGI(TAG, "  - Time sync enabled: %s",
             enable_time_sync ? (cJSON_IsTrue(enable_time_sync) ? "true" : "false") : "not set");
    ESP_LOGI(TAG, "  - Primary source: %s",
             primary_source ? (cJSON_IsString(primary_source) ? primary_source->valuestring : "invalid") : "not set");
    ESP_LOGI(TAG, "  - Force sync on startup: %s",
             force_sync ? (cJSON_IsTrue(force_sync) ? "true" : "false") : "not set");
    ESP_LOGI(TAG, "  - Time logging enabled: %s",
             time_logging ? (cJSON_IsTrue(time_logging) ? "true" : "false") : "not set");

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Time configuration loaded and applied successfully");
    return ESP_OK;
}

/* Save tariffs configuration to external Flash (documented in rbgrid_config_storage.h). */
esp_err_t rbgrid_storage_save_tariffs(const char *json_str)
{
    ESP_LOGI(TAG, "Saving tariffs configuration...");

    // Check parameters
    if (!json_str) {
        ESP_LOGE(TAG, "Invalid parameters: json_str is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if external Flash is mounted
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    size_t json_len = strlen(json_str);

    // Check size limit - use same as other configs
    if (json_len >= CONFIG_COMPACT_SIZE) {
        ESP_LOGE(TAG, "JSON too large: %zu bytes (max: %d)", json_len, CONFIG_COMPACT_SIZE);
        return ESP_ERR_NO_MEM;
    }

    // Check if file exists, create if not
    if (!littlefs_raw_exists(CONFIG_FILE_TARIFFS)) {
        ESP_LOGI(TAG, "Tariffs config file does not exist, will be created");
    }

    // Write to file
    esp_err_t ret = littlefs_raw_write(CONFIG_FILE_TARIFFS, json_str, json_len);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Tariffs config saved to %s (%zu bytes)", CONFIG_FILE_TARIFFS, json_len);
        ESP_LOGI(TAG, "Tariffs configuration saved successfully");
    } else {
        ESP_LOGE(TAG, "Failed to write tariffs config file: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* Load tariffs configuration from external Flash and apply it (documented in rbgrid_config_storage.h). */
esp_err_t rbgrid_storage_load_tariffs(rbgrid_config_handle_t config_handle)
{
    ESP_LOGI(TAG, "Loading tariffs configuration...");

    // Check parameters
    if (!config_handle) {
        ESP_LOGE(TAG, "Invalid config handle");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if external Flash is mounted
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    // Check if file exists
    if (!littlefs_raw_exists(CONFIG_FILE_TARIFFS)) {
        ESP_LOGW(TAG, "Tariffs config file does not exist");
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate buffer for reading - use same size as other configs
    char* buffer = malloc(CONFIG_COMPACT_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for tariffs buffer");
        return ESP_ERR_NO_MEM;
    }

    // Read file
    size_t actual_size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_TARIFFS, buffer, CONFIG_COMPACT_SIZE - 1, &actual_size);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read tariffs config file: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }

    // Null-terminate the string
    buffer[actual_size] = '\0';

    ESP_LOGI(TAG, "Tariffs config loaded from %s (%zu bytes)", CONFIG_FILE_TARIFFS, actual_size);

    // Parse JSON and apply tariff configuration
    rbgrid_json_err_t json_ret = rbgrid_json_parse_tariff_plan_config(buffer);

    if (json_ret != RBGRID_JSON_OK) {
        ESP_LOGE(TAG, "Failed to parse tariff configuration: %d", json_ret);
        free(buffer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Tariff configuration parsed successfully");

    // Get the parsed tariff plan from global variable
    extern rbpower_tariff_plan_t g_current_tariff_plan;

    // Create tariff builder from parsed plan
    rbgrid_tariff_builder_t tariff_builder = rbgrid_tariff_builder_create(
        g_current_tariff_plan.currency_code[0] ? g_current_tariff_plan.currency_code : "EUR",
        "AUTO"  // region will be auto-detected
    );
    if (!tariff_builder) {
        ESP_LOGE(TAG, "Failed to create tariff builder");
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    // VAT is handled separately through tariff plan, not in builder
    rbgrid_config_error_t config_ret = RBGRID_CONFIG_OK;

    // Add tariff zones from parsed plan
    for (int i = 0; i < g_current_tariff_plan.active_zones_count && i < RBPOWER_TARIFF_ZONE_COUNT; i++) {
        rbpower_tariff_zone_t* zone = &g_current_tariff_plan.zones[i];

        if (!zone->is_enabled) continue;

        // Determine zone type based on conditions
        const char* zone_type = "OTHER";
        bool is_time_based = false;
        char time_range[32] = "";

        // Check if zone has time range conditions
        for (int j = 0; j < zone->conditions_count; j++) {
            if (zone->conditions[j].condition_type == RBPOWER_TARIFF_CONDITION_TIME_RANGE &&
                zone->conditions[j].is_active) {
                is_time_based = true;
                snprintf(time_range, sizeof(time_range), "%02d:%02d-%02d:%02d",
                        zone->conditions[j].params.time_range.start_hour,
                        zone->conditions[j].params.time_range.start_minute,
                        zone->conditions[j].params.time_range.end_hour,
                        zone->conditions[j].params.time_range.end_minute);
                break;
            }
        }

        // Map zone ID to type string
        switch (zone->zone_id) {
            case RBPOWER_TARIFF_ZONE_NIGHT:
                zone_type = "NIGHT";
                break;
            case RBPOWER_TARIFF_ZONE_DAY:
                zone_type = "DAY";
                break;
            case RBPOWER_TARIFF_ZONE_EVENING_PEAK:
                zone_type = "PEAK";
                break;
            case RBPOWER_TARIFF_ZONE_WEEKEND:
                zone_type = "WEEKEND";
                break;
            default:
                zone_type = "OTHER";
                break;
        }

        // Add zone based on type
        if (is_time_based && strlen(time_range) > 0) {
            config_ret = rbgrid_tariff_builder_add_time_zone(
                tariff_builder,
                zone->zone_name,
                zone->rate_buying_per_kwh,
                time_range
            );
        } else if (zone->zone_id == RBPOWER_TARIFF_ZONE_WEEKEND) {
            // Weekend zone - check for weekday mask in conditions
            uint8_t weekday_mask = 0x41; // Default: Saturday (0x40) + Sunday (0x01)
            for (int j = 0; j < zone->conditions_count; j++) {
                if (zone->conditions[j].condition_type == RBPOWER_TARIFF_CONDITION_WEEKDAY &&
                    zone->conditions[j].is_active) {
                    weekday_mask = zone->conditions[j].params.weekday.weekday_mask;
                    break;
                }
            }
            config_ret = rbgrid_tariff_builder_add_weekday_zone(
                tariff_builder,
                zone->zone_name,
                zone->rate_buying_per_kwh,
                weekday_mask
            );
        } else {
            // For other zones, try to add as time zone with default time
            const char* default_time = NULL;
            switch (zone->zone_id) {
                case RBPOWER_TARIFF_ZONE_NIGHT:
                    default_time = "23:00-07:00";
                    break;
                case RBPOWER_TARIFF_ZONE_DAY:
                    default_time = "07:00-18:00";
                    break;
                case RBPOWER_TARIFF_ZONE_EVENING_PEAK:
                    default_time = "18:00-23:00";
                    break;
                default:
                    default_time = "00:00-23:59";
                    break;
            }
            config_ret = rbgrid_tariff_builder_add_time_zone(
                tariff_builder,
                zone->zone_name,
                zone->rate_buying_per_kwh,
                default_time
            );
        }

        if (config_ret != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to add zone %s: %d", zone->zone_name, config_ret);
        } else {
            ESP_LOGI(TAG, "Added zone: %s (%.3f EUR/kWh)", zone->zone_name, zone->rate_buying_per_kwh);
        }
    }

    // Set net metering if enabled
    if (g_current_tariff_plan.net_metering_enabled) {
        float selling_rate = 0;

        // Calculate default selling rate if not specified
        if (g_current_tariff_plan.active_zones_count > 0) {
            for (int i = 0; i < g_current_tariff_plan.active_zones_count; i++) {
                if (g_current_tariff_plan.zones[i].rate_selling_per_kwh > 0) {
                    selling_rate = g_current_tariff_plan.zones[i].rate_selling_per_kwh;
                    break;
                }
            }
        }

        config_ret = rbgrid_tariff_builder_set_net_metering(
            tariff_builder,
            selling_rate,
            g_current_tariff_plan.net_metering_default_ratio
        );

        if (config_ret != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to set net metering: %d", config_ret);
        } else {
            ESP_LOGI(TAG, "Net metering enabled: ratio=%.2f, selling_rate=%.3f",
                    g_current_tariff_plan.net_metering_default_ratio, selling_rate);
        }
    }

    // Add fixed payments
    for (int i = 0; i < g_current_tariff_plan.fixed_payments_count && i < 8; i++) {
        rbpower_tariff_fixed_payment_t* payment = &g_current_tariff_plan.fixed_payments[i];

        if (!payment->is_enabled) continue;

        // Add fixed fee based on type
        if (payment->payment_type == RBPOWER_TARIFF_FIXED_MONTHLY) {
            // Use set_monthly_fee for the main monthly fee
            if (strcmp(payment->payment_name, "Monthly Fee") == 0) {
                config_ret = rbgrid_tariff_builder_set_monthly_fee(
                    tariff_builder,
                    payment->amount
                );
            } else {
                // Use add_custom_fee for other monthly fees
                config_ret = rbgrid_tariff_builder_add_custom_fee(
                    tariff_builder,
                    payment->payment_name,
                    payment->amount,
                    RBPOWER_TARIFF_FIXED_MONTHLY
                );
            }
        } else if (payment->payment_type == RBPOWER_TARIFF_FIXED_POWER_BASED) {
            config_ret = rbgrid_tariff_builder_set_power_fee(
                tariff_builder,
                payment->amount
            );
        } else {
            // For other types, use custom fee
            config_ret = rbgrid_tariff_builder_add_custom_fee(
                tariff_builder,
                payment->payment_name,
                payment->amount,
                payment->payment_type
            );
        }

        if (config_ret != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to add fixed fee %s: %d", payment->payment_name, config_ret);
        } else {
            ESP_LOGI(TAG, "Added fixed fee: %s (%.2f EUR)", payment->payment_name, payment->amount);
        }
    }

    // Validate tariff builder
    config_ret = rbgrid_tariff_builder_validate(tariff_builder);
    if (config_ret != RBGRID_CONFIG_OK) {
        ESP_LOGE(TAG, "Tariff validation failed: %d", config_ret);
        rbgrid_tariff_builder_destroy(tariff_builder);
        free(buffer);
        return ESP_FAIL;
    }

    // Apply tariff plan to configuration
    config_ret = rbgrid_config_apply_tariff_plan(config_handle, tariff_builder);
    if (config_ret != RBGRID_CONFIG_OK) {
        ESP_LOGE(TAG, "Failed to apply tariff plan: %d", config_ret);
        rbgrid_tariff_builder_destroy(tariff_builder);
        free(buffer);
        return ESP_FAIL;
    }

    // Clean up
    rbgrid_tariff_builder_destroy(tariff_builder);
    free(buffer);

    ESP_LOGI(TAG, "Tariff configuration loaded and applied successfully");
    ESP_LOGI(TAG, "  - Plan: %s", g_current_tariff_plan.plan_name);
    ESP_LOGI(TAG, "  - Active zones: %d", g_current_tariff_plan.active_zones_count);
    ESP_LOGI(TAG, "  - Fixed payments: %d", g_current_tariff_plan.fixed_payments_count);

    return ESP_OK;
}

/* ================================================================================================
 * rbAmp FLEET CONFIG persistence — /rbamp.json. Bus params + per-module address/entry map.
 * Unlike the other loaders this takes the struct directly (the bridge lives in main/, not the
 * rbgrid_config handle); main.cpp calls load before rbamp_bridge_start, falling back to defaults.
 * ================================================================================================ */
#define CONFIG_FILE_RBAMP "/rbamp.json"   ///< rbAmp fleet configuration file

esp_err_t rbgrid_storage_save_rbamp(const rbamp_bridge_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char *json = rbgrid_json_create_rbamp_config(cfg);
    if (!json) {
        ESP_LOGE(TAG, "Failed to serialize rbAmp config");
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(json);
    esp_err_t ret = littlefs_raw_write(CONFIG_FILE_RBAMP, json, len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "rbAmp config saved to %s (%zu bytes, %u modules)",
                 CONFIG_FILE_RBAMP, len, (unsigned)cfg->module_count);
        rbpower_hw_rev_bump();
    } else {
        ESP_LOGE(TAG, "Failed to save rbAmp config: %s", esp_err_to_name(ret));
    }
    free(json);
    return ret;
}

esp_err_t rbgrid_storage_load_rbamp(rbamp_bridge_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "External Flash not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    if (!littlefs_raw_exists(CONFIG_FILE_RBAMP)) {
        ESP_LOGW(TAG, "rbAmp config file %s does not exist", CONFIG_FILE_RBAMP);
        return ESP_ERR_NOT_FOUND;
    }

    char *buffer = (char*)malloc(CONFIG_COMPACT_SIZE);
    if (!buffer) return ESP_ERR_NO_MEM;

    size_t size = 0;
    esp_err_t ret = littlefs_raw_read(CONFIG_FILE_RBAMP, buffer, CONFIG_COMPACT_SIZE - 1, &size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read rbAmp config: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }
    buffer[size] = '\0';

    rbgrid_json_err_t jerr = rbgrid_json_parse_rbamp_config(buffer, cfg);
    free(buffer);
    if (jerr != RBGRID_JSON_OK) {
        ESP_LOGE(TAG, "Failed to parse rbAmp config (%d)", jerr);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "rbAmp config loaded from Flash (%u modules)", (unsigned)cfg->module_count);
    return ESP_OK;
}