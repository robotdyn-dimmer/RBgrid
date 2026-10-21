/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_config_runtime.c
 * @brief Compact runtime configuration manager: RAM tables of units and their history ring buffers, saved as per-unit JSON files
 */

#include "rbextflash_config_runtime.h"
#include "rbstorage_transfer_types.h"
#include "external_flash_config.h"
#include "littlefs_raw.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "RBFLASH_RT";

// External Flash functions (documented in external_flash_config.h)
extern external_flash_state_t* external_flash_get_state(void);

/* ================================================================================================
 * INITIALIZATION AND DEINITIALIZATION
 * ================================================================================================ */

esp_err_t rbpower_runtime_config_init(rbpower_runtime_config_manager_t* manager)
{
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Initializing runtime config manager...");

    memset(manager, 0, sizeof(rbpower_runtime_config_manager_t));

    // Check storage availability via the global flash status
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGD(TAG, "External Flash not available, runtime manager will work in RAM-only mode");
        manager->stats_partition_offset = 0;
        manager->stats_partition_size = 0;
        manager->unit_count = 0;
        manager->buffer_count = 0;
        manager->units = NULL;
        manager->buffers = NULL;
        manager->is_initialized = false;  // Mark as not initialized
        return ESP_OK;  // Return success so the system continues in RAM-only mode
    }

    // Get STATS partition info
    uint32_t stats_offset, stats_size;
    if (external_flash_get_partition_info("stats", &stats_offset, &stats_size)) {
        manager->stats_partition_offset = stats_offset;
        manager->stats_partition_size = stats_size;
        ESP_LOGD(TAG, "STATS partition: offset=0x%08lX, size=%lu bytes",
                 (unsigned long)stats_offset, (unsigned long)stats_size);
    } else {
        ESP_LOGE(TAG, "Failed to get STATS partition info");
        return ESP_FAIL;
    }

    manager->unit_count = 0;
    manager->buffer_count = 0;
    manager->units = NULL;
    manager->buffers = NULL;
    manager->is_initialized = true;

    ESP_LOGD(TAG, "Runtime config manager initialized (0 units, 0 buffers)");
    return ESP_OK;
}

void rbpower_runtime_config_deinit(rbpower_runtime_config_manager_t* manager)
{
    if (!manager) {
        return;
    }

    ESP_LOGD(TAG, "Deinitializing runtime config manager...");

    // Free the units array
    if (manager->units) {
        free(manager->units);
        manager->units = NULL;
    }

    // Free the buffers array
    if (manager->buffers) {
        free(manager->buffers);
        manager->buffers = NULL;
    }

    manager->unit_count = 0;
    manager->buffer_count = 0;
    manager->is_initialized = false;

    ESP_LOGD(TAG, "Runtime config manager deinitialized");
}

/* ================================================================================================
 * ADDING UNITS AND BUFFERS
 * ================================================================================================ */

esp_err_t rbpower_runtime_add_unit(rbpower_runtime_config_manager_t* manager,
                                    rbpower_unit_id_t unit_id,
                                    const char* unit_name,
                                    uint16_t unit_role)
{
    if (!manager || !manager->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (manager->unit_count >= RBPOWER_RUNTIME_MAX_UNITS) {
        ESP_LOGE(TAG, "Cannot add unit: limit reached (%d)", RBPOWER_RUNTIME_MAX_UNITS);
        return ESP_ERR_NO_MEM;
    }

    // Check whether it already exists
    for (int i = 0; i < manager->unit_count; i++) {
        if (manager->units[i].unit_id == unit_id) {
            ESP_LOGW(TAG, "Unit 0x%04X already exists", unit_id);
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Grow the units array
    rbpower_unit_runtime_t* new_units = (rbpower_unit_runtime_t*)realloc(
        manager->units,
        (manager->unit_count + 1) * sizeof(rbpower_unit_runtime_t)
    );

    if (!new_units) {
        ESP_LOGE(TAG, "Failed to allocate memory for unit");
        return ESP_ERR_NO_MEM;
    }

    manager->units = new_units;

    // Fill in the new unit
    rbpower_unit_runtime_t* unit = &manager->units[manager->unit_count];
    memset(unit, 0, sizeof(rbpower_unit_runtime_t));

    unit->unit_id = unit_id;
    unit->unit_role = unit_role;
    unit->enabled = true;
    unit->buffer_count = 0;

    if (unit_name) {
        strncpy(unit->unit_name, unit_name, sizeof(unit->unit_name) - 1);
        unit->unit_name[sizeof(unit->unit_name) - 1] = '\0';
    } else {
        snprintf(unit->unit_name, sizeof(unit->unit_name), "Unit_%04X", unit_id);
    }

    manager->unit_count++;

    ESP_LOGD(TAG, "Added unit 0x%04X ('%s'), total units: %d",
             unit_id, unit->unit_name, manager->unit_count);

    return ESP_OK;
}

esp_err_t rbpower_runtime_add_buffer(rbpower_runtime_config_manager_t* manager,
                                      const rbpower_buffer_runtime_t* buffer)
{
    if (!manager || !manager->is_initialized || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    // No buffer limit check here: the count is bounded by the 4 fixed slots and the type matrix

    // Grow the buffers array
    rbpower_buffer_runtime_t* new_buffers = (rbpower_buffer_runtime_t*)realloc(
        manager->buffers,
        (manager->buffer_count + 1) * sizeof(rbpower_buffer_runtime_t)
    );

    if (!new_buffers) {
        ESP_LOGE(TAG, "Failed to allocate memory for buffer");
        return ESP_ERR_NO_MEM;
    }

    manager->buffers = new_buffers;

    // Copy the buffer data
    memcpy(&manager->buffers[manager->buffer_count], buffer, sizeof(rbpower_buffer_runtime_t));

    // Start with an invalid cache (filled on first access)
    manager->buffers[manager->buffer_count].cached_active_sector = 0;
    manager->buffers[manager->buffer_count].cached_next_record_index = 0;
    manager->buffers[manager->buffer_count].cached_sequence_number = 0;
    manager->buffers[manager->buffer_count].cached_newest_timestamp = 0;  // Loaded from flash later
    manager->buffers[manager->buffer_count].cache_valid = false;

    manager->buffer_count++;

    // Increment the owning unit's buffer count
    for (int i = 0; i < manager->unit_count; i++) {
        if (manager->units[i].unit_id == buffer->unit_id) {
            manager->units[i].buffer_count++;
            break;
        }
    }

    ESP_LOGD(TAG, "Added buffer: unit=0x%04X, period=%d, type=%d, offset=%lu, total buffers: %d",
             buffer->unit_id, buffer->period, buffer->data_type,
             (unsigned long)buffer->flash_offset, manager->buffer_count);

    return ESP_OK;
}

/* ================================================================================================
 * LOOKUP
 * ================================================================================================ */

const rbpower_unit_runtime_t* rbpower_runtime_find_unit(
    const rbpower_runtime_config_manager_t* manager,
    rbpower_unit_id_t unit_id)
{
    if (!manager || !manager->is_initialized) {
        return NULL;
    }

    for (int i = 0; i < manager->unit_count; i++) {
        if (manager->units[i].unit_id == unit_id) {
            return &manager->units[i];
        }
    }

    return NULL;
}

const rbpower_buffer_runtime_t* rbpower_runtime_find_buffer(
    const rbpower_runtime_config_manager_t* manager,
    rbpower_unit_id_t unit_id,
    rbpower_period_t period,
    rbpower_data_type_t data_type)
{
    if (!manager || !manager->is_initialized) {
        return NULL;
    }

    for (int i = 0; i < manager->buffer_count; i++) {
        const rbpower_buffer_runtime_t* buf = &manager->buffers[i];
        if (buf->unit_id == unit_id &&
            buf->period == period &&
            buf->data_type == data_type) {
            return buf;
        }
    }

    return NULL;
}

esp_err_t rbpower_runtime_get_unit_buffers(
    const rbpower_runtime_config_manager_t* manager,
    rbpower_unit_id_t unit_id,
    const rbpower_buffer_runtime_t** buffers_out,
    uint8_t max_buffers,
    uint8_t* count_out)
{
    if (!manager || !manager->is_initialized || !buffers_out || !count_out) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t count = 0;

    for (int i = 0; i < manager->buffer_count && count < max_buffers; i++) {
        if (manager->buffers[i].unit_id == unit_id) {
            buffers_out[count++] = &manager->buffers[i];
        }
    }

    *count_out = count;
    return ESP_OK;
}

/* ================================================================================================
 * LOG OUTPUT
 * ================================================================================================ */

void rbpower_runtime_config_dump(const rbpower_runtime_config_manager_t* manager)
{
    if (!manager || !manager->is_initialized) {
        ESP_LOGE(TAG, "Manager not initialized");
        return;
    }

    ESP_LOGI(TAG, "=== RBPOWER RUNTIME CONFIGURATION ===");
    ESP_LOGI(TAG, "STATS partition: offset=0x%08lX, size=%lu KB",
             (unsigned long)manager->stats_partition_offset,
             (unsigned long)manager->stats_partition_size / 1024);
    ESP_LOGI(TAG, "Units: %d", manager->unit_count);
    ESP_LOGI(TAG, "Total buffers: %d", manager->buffer_count);
    ESP_LOGI(TAG, "");

    const char* period_names[] = {"HOUR", "DAY", "WEEK", "MONTH", "YEAR"};
    const char* type_names[] = {"ELECTRICAL", "ENERGY", "ECONOMICS", "QUALITY", "DIRECTION"};

    for (int i = 0; i < manager->unit_count; i++) {
        const rbpower_unit_runtime_t* unit = &manager->units[i];

        ESP_LOGI(TAG, "Unit[%d]: ID=0x%04X, Name='%s', Role=%d, Enabled=%s",
                 i, unit->unit_id, unit->unit_name, unit->unit_role,
                 unit->enabled ? "YES" : "NO");
        ESP_LOGI(TAG, "  Buffers: %d", unit->buffer_count);

        // Find and print this unit's buffers
        for (int j = 0; j < manager->buffer_count; j++) {
            const rbpower_buffer_runtime_t* buf = &manager->buffers[j];
            if (buf->unit_id == unit->unit_id) {
                ESP_LOGI(TAG, "    %s/%s: offset=%lu, size=%lu, sectors=%d, capacity=%lu/%lu",
                         period_names[buf->period],
                         type_names[buf->data_type],
                         (unsigned long)buf->flash_offset,
                         (unsigned long)buf->buffer_size_bytes,
                         buf->total_sectors,
                         (unsigned long)buf->physical_capacity,
                         (unsigned long)buf->logical_capacity);
            }
        }
        ESP_LOGI(TAG, "");
    }

    // Print memory usage
    size_t mem_usage = rbpower_runtime_config_get_memory_usage(manager);
    ESP_LOGI(TAG, "Memory usage: Units=%d bytes (%d×%d) | Buffers=%d bytes (%d×%d) | Total=%d bytes (%.1f KB)\n",
             manager->unit_count * sizeof(rbpower_unit_runtime_t), manager->unit_count, sizeof(rbpower_unit_runtime_t),
             manager->buffer_count * sizeof(rbpower_buffer_runtime_t), manager->buffer_count, sizeof(rbpower_buffer_runtime_t),
             mem_usage, (float)mem_usage / 1024.0f);
}

size_t rbpower_runtime_config_get_memory_usage(const rbpower_runtime_config_manager_t* manager)
{
    if (!manager || !manager->is_initialized) {
        return 0;
    }

    return sizeof(rbpower_runtime_config_manager_t) +
           manager->unit_count * sizeof(rbpower_unit_runtime_t) +
           manager->buffer_count * sizeof(rbpower_buffer_runtime_t);
}

/* ================================================================================================
 * JSON SERIALIZATION / DESERIALIZATION
 * ================================================================================================ */

// JSON configuration constants
// Files are stored in the LittleFS root with an identifying prefix
#define RBPOWER_JSON_FILENAME_FMT   "/configstat_unit_%04X.json"  ///< File name format (unit ID in hex)
#define RBPOWER_JSON_FILE_PREFIX    "configstat_unit_"            ///< Prefix used to find the files
#define RBPOWER_JSON_MAX_PATH       64 ///< Maximum file path length, bytes
/** @def MAX_JSON_SIZE
 * @brief Maximum JSON file size accepted by rbpower_runtime_config_load_unit(), bytes (defined inside that function). */

/**
 * @brief Serialize a unit configuration to JSON
 */
static cJSON* rbpower_unit_to_json(const rbpower_runtime_config_manager_t* manager,
                                     rbpower_unit_id_t unit_id)
{
    // Find the unit
    const rbpower_unit_runtime_t* unit = rbpower_runtime_find_unit(manager, unit_id);
    if (!unit) {
        ESP_LOGE(TAG, "Unit 0x%04X not found", unit_id);
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root object");
        return NULL;
    }

    // Add the unit information
    cJSON_AddNumberToObject(root, "unit_id", unit->unit_id);
    cJSON_AddStringToObject(root, "unit_name", unit->unit_name);
    cJSON_AddNumberToObject(root, "unit_role", unit->unit_role);
    cJSON_AddBoolToObject(root, "enabled", unit->enabled);

    // Create the buffers array
    cJSON* buffers_array = cJSON_CreateArray();
    if (!buffers_array) {
        ESP_LOGE(TAG, "Failed to create buffers array");
        cJSON_Delete(root);
        return NULL;
    }

    // Add all buffers of this unit
    for (int i = 0; i < manager->buffer_count; i++) {
        const rbpower_buffer_runtime_t* buf = &manager->buffers[i];
        if (buf->unit_id != unit_id) {
            continue;
        }

        cJSON* buf_obj = cJSON_CreateObject();
        if (!buf_obj) {
            ESP_LOGE(TAG, "Failed to create buffer object");
            cJSON_Delete(buffers_array);
            cJSON_Delete(root);
            return NULL;
        }

        cJSON_AddNumberToObject(buf_obj, "period", buf->period);
        cJSON_AddNumberToObject(buf_obj, "data_type", buf->data_type);
        cJSON_AddNumberToObject(buf_obj, "total_sectors", buf->total_sectors);
        cJSON_AddNumberToObject(buf_obj, "record_size", buf->record_size);
        cJSON_AddNumberToObject(buf_obj, "flash_offset", buf->flash_offset);
        cJSON_AddNumberToObject(buf_obj, "buffer_size_bytes", buf->buffer_size_bytes);
        cJSON_AddNumberToObject(buf_obj, "physical_capacity", buf->physical_capacity);
        cJSON_AddNumberToObject(buf_obj, "logical_capacity", buf->logical_capacity);

        cJSON_AddItemToArray(buffers_array, buf_obj);
    }

    cJSON_AddItemToObject(root, "buffers", buffers_array);

    return root;
}

/**
 * @brief Deserialize a unit configuration from JSON
 */
static esp_err_t rbpower_unit_from_json(rbpower_runtime_config_manager_t* manager,
                                         cJSON* root)
{
    if (!manager || !root) {
        return ESP_ERR_INVALID_ARG;
    }

    // Extract the unit data
    cJSON* unit_id_obj = cJSON_GetObjectItem(root, "unit_id");
    cJSON* unit_name_obj = cJSON_GetObjectItem(root, "unit_name");
    cJSON* unit_role_obj = cJSON_GetObjectItem(root, "unit_role");
    cJSON* enabled_obj = cJSON_GetObjectItem(root, "enabled");

    if (!cJSON_IsNumber(unit_id_obj) || !cJSON_IsString(unit_name_obj) ||
        !cJSON_IsNumber(unit_role_obj)) {
        ESP_LOGE(TAG, "Invalid JSON: missing required unit fields");
        return ESP_ERR_INVALID_ARG;
    }

    rbpower_unit_id_t unit_id = (rbpower_unit_id_t)unit_id_obj->valueint;
    const char* unit_name = unit_name_obj->valuestring;
    uint16_t unit_role = (uint16_t)unit_role_obj->valueint;

    // Add the unit
    esp_err_t ret = rbpower_runtime_add_unit(manager, unit_id, unit_name, unit_role);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add unit 0x%04X: %s", unit_id, esp_err_to_name(ret));
        return ret;
    }

    // If an enabled field is present, apply it
    if (cJSON_IsBool(enabled_obj)) {
        for (int i = 0; i < manager->unit_count; i++) {
            if (manager->units[i].unit_id == unit_id) {
                manager->units[i].enabled = cJSON_IsTrue(enabled_obj);
                break;
            }
        }
    }

    // Extract the buffers array
    cJSON* buffers_array = cJSON_GetObjectItem(root, "buffers");
    if (!cJSON_IsArray(buffers_array)) {
        ESP_LOGW(TAG, "No buffers array in JSON for unit 0x%04X", unit_id);
        return ESP_OK;
    }

    // Add all buffers
    cJSON* buf_obj = NULL;
    cJSON_ArrayForEach(buf_obj, buffers_array) {
        cJSON* period_obj = cJSON_GetObjectItem(buf_obj, "period");
        cJSON* data_type_obj = cJSON_GetObjectItem(buf_obj, "data_type");
        cJSON* total_sectors_obj = cJSON_GetObjectItem(buf_obj, "total_sectors");
        cJSON* record_size_obj = cJSON_GetObjectItem(buf_obj, "record_size");
        cJSON* flash_offset_obj = cJSON_GetObjectItem(buf_obj, "flash_offset");
        cJSON* buffer_size_obj = cJSON_GetObjectItem(buf_obj, "buffer_size_bytes");
        cJSON* physical_cap_obj = cJSON_GetObjectItem(buf_obj, "physical_capacity");
        cJSON* logical_cap_obj = cJSON_GetObjectItem(buf_obj, "logical_capacity");

        if (!cJSON_IsNumber(period_obj) || !cJSON_IsNumber(data_type_obj) ||
            !cJSON_IsNumber(total_sectors_obj) || !cJSON_IsNumber(record_size_obj) ||
            !cJSON_IsNumber(flash_offset_obj) || !cJSON_IsNumber(buffer_size_obj) ||
            !cJSON_IsNumber(physical_cap_obj) || !cJSON_IsNumber(logical_cap_obj)) {
            ESP_LOGW(TAG, "Invalid buffer entry in JSON, skipping");
            continue;
        }

        rbpower_buffer_runtime_t buf;
        buf.unit_id = unit_id;
        buf.period = (uint8_t)period_obj->valueint;
        buf.data_type = (uint8_t)data_type_obj->valueint;
        buf.total_sectors = (uint16_t)total_sectors_obj->valueint;
        buf.record_size = (uint16_t)record_size_obj->valueint;
        buf.flash_offset = (uint32_t)flash_offset_obj->valuedouble;
        buf.buffer_size_bytes = (uint32_t)buffer_size_obj->valuedouble;
        buf.physical_capacity = (uint32_t)physical_cap_obj->valuedouble;
        buf.logical_capacity = (uint32_t)logical_cap_obj->valuedouble;

        ret = rbpower_runtime_add_buffer(manager, &buf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add buffer: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

/* ================================================================================================
 * LOAD/SAVE FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_runtime_config_save_unit(const rbpower_runtime_config_manager_t* manager,
                                            rbpower_unit_id_t unit_id)
{
    if (!manager || !manager->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Saving unit 0x%04X config to JSON...", unit_id);

    // Check storage availability via the global flash status
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGD(TAG, "External Flash not available, skipping save (RAM-only mode)");
        return ESP_OK;  // Silently skip the save
    }

    // Serialize the unit to JSON
    cJSON* root = rbpower_unit_to_json(manager, unit_id);
    if (!root) {
        ESP_LOGE(TAG, "Failed to serialize unit 0x%04X", unit_id);
        return ESP_FAIL;
    }

    // Convert to a string
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to convert JSON to string");
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);

    // Build the file path
    char filepath[RBPOWER_JSON_MAX_PATH];
    snprintf(filepath, sizeof(filepath), RBPOWER_JSON_FILENAME_FMT, unit_id);

    // Write through the LittleFS API
    esp_err_t ret = littlefs_raw_write(filepath, json_str, json_len);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write JSON to file: %s (%s)", filepath, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Unit 0x%04X config saved to %s (%d bytes)", unit_id, filepath, json_len);
    return ESP_OK;
}

esp_err_t rbpower_runtime_config_load_unit(rbpower_runtime_config_manager_t* manager,
                                            rbpower_unit_id_t unit_id)
{
    if (!manager || !manager->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Loading unit 0x%04X config from JSON...", unit_id);

    // Check storage availability via the global flash status
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGD(TAG, "External Flash not available, cannot load (RAM-only mode)");
        return ESP_ERR_NOT_FOUND;  // Report "not found"
    }

    // Build the file path
    char filepath[RBPOWER_JSON_MAX_PATH];
    snprintf(filepath, sizeof(filepath), RBPOWER_JSON_FILENAME_FMT, unit_id);

    // Check that the file exists
    if (!littlefs_raw_exists(filepath)) {
        ESP_LOGE(TAG, "Config file not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate the read buffer (JSON is at most 32 KB)
    #define MAX_JSON_SIZE 32768 // Maximum JSON file size, bytes
    char* json_str = (char*)malloc(MAX_JSON_SIZE);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to allocate buffer for JSON (%d bytes)", MAX_JSON_SIZE);
        return ESP_ERR_NO_MEM;
    }

    // Read the file through the LittleFS API
    size_t actual_size = 0;
    esp_err_t ret = littlefs_raw_read(filepath, json_str, MAX_JSON_SIZE - 1, &actual_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read JSON file: %s (%s)", filepath, esp_err_to_name(ret));
        free(json_str);
        return ret;
    }

    json_str[actual_size] = '\0';

    // Parse the JSON
    cJSON* root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", cJSON_GetErrorPtr());
        return ESP_FAIL;
    }

    // Deserialize
    ret = rbpower_unit_from_json(manager, root);
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Unit 0x%04X config loaded successfully", unit_id);
    } else {
        ESP_LOGE(TAG, "Failed to load unit 0x%04X: %s", unit_id, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t rbpower_runtime_config_load_all(rbpower_runtime_config_manager_t* manager)
{
    if (!manager || !manager->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Loading all unit configs from JSON...");

    // Check storage availability via the global flash status
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGD(TAG, "External Flash not available, cannot load (RAM-only mode)");
        return ESP_ERR_NOT_FOUND;  // Report "not found"
    }

    // Try to load the known unit IDs
    // This list could later be extended or read from a separate index file
    const rbpower_unit_id_t known_units[] = {
        0x4400,  // Unit 1
        0x4401,  // Unit 2
        0x4402,  // Unit 3
        // Add other known units as needed
    };

    int loaded_count = 0;
    int failed_count = 0;

    for (int i = 0; i < sizeof(known_units) / sizeof(known_units[0]); i++) {
        rbpower_unit_id_t unit_id = known_units[i];

        // Check that the file exists
        char filepath[RBPOWER_JSON_MAX_PATH];
        snprintf(filepath, sizeof(filepath), RBPOWER_JSON_FILENAME_FMT, unit_id);

        if (littlefs_raw_exists(filepath)) {
            ESP_LOGD(TAG, "Found config file for unit 0x%04X", unit_id);

            esp_err_t ret = rbpower_runtime_config_load_unit(manager, unit_id);
            if (ret == ESP_OK) {
                loaded_count++;
            } else {
                failed_count++;
                ESP_LOGW(TAG, "Failed to load unit 0x%04X: %s", unit_id, esp_err_to_name(ret));
            }
        }
    }

    ESP_LOGD(TAG, "Load complete: %d units loaded, %d failed", loaded_count, failed_count);

    return (loaded_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ================================================================================================
 * TEST FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_runtime_config_test(void)
{
    ESP_LOGI(TAG, "===  RUNTIME CONFIG TEST  ===");

    rbpower_runtime_config_manager_t manager;
    esp_err_t ret;

    // Test 1: Initialization
    ESP_LOGD(TAG, "Test 1: Initializing manager...");
    ret = rbpower_runtime_config_init(&manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 1 FAILED: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGD(TAG, "✓ Test 1 PASSED");

    // Test 2: Add a unit
    ESP_LOGD(TAG, "Test 2: Adding unit...");
    ret = rbpower_runtime_add_unit(&manager, 0x4401, "Main Supply", 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 2 FAILED: %s", esp_err_to_name(ret));
        rbpower_runtime_config_deinit(&manager);
        return ret;
    }
    ESP_LOGD(TAG, "✓ Test 2 PASSED");

    // Test 3: Add buffers
    ESP_LOGD(TAG, "Test 3: Adding buffers...");

    rbpower_buffer_runtime_t buf;
    uint32_t offset = 0;

    // HOUR/ELECTRICAL
    buf.unit_id = 0x4401;
    buf.period = RBPOWER_PERIOD_HOUR;
    buf.data_type = RBPOWER_DATA_TYPE_ELECTRICAL;
    buf.total_sectors = 6;
    buf.record_size = 28;
    buf.flash_offset = offset;
    buf.buffer_size_bytes = 6 * 4096;
    buf.physical_capacity = 876;
    buf.logical_capacity = 720;
    offset += buf.buffer_size_bytes;

    ret = rbpower_runtime_add_buffer(&manager, &buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 3 FAILED: %s", esp_err_to_name(ret));
        rbpower_runtime_config_deinit(&manager);
        return ret;
    }

    // HOUR/ENERGY
    buf.data_type = RBPOWER_DATA_TYPE_ENERGY;
    buf.total_sectors = 4;
    buf.record_size = 16;
    buf.flash_offset = offset;
    buf.buffer_size_bytes = 4 * 4096;
    buf.physical_capacity = 1024;
    offset += buf.buffer_size_bytes;

    ret = rbpower_runtime_add_buffer(&manager, &buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 3 FAILED: %s", esp_err_to_name(ret));
        rbpower_runtime_config_deinit(&manager);
        return ret;
    }

    ESP_LOGD(TAG, "✓ Test 3 PASSED: added 2 buffers");

    // Test 4: Find the unit
    ESP_LOGD(TAG, "Test 4: Finding unit...");
    const rbpower_unit_runtime_t* unit = rbpower_runtime_find_unit(&manager, 0x4401);
    if (!unit) {
        ESP_LOGE(TAG, "✗ Test 4 FAILED: unit not found");
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "✓ Test 4 PASSED: found unit 0x%04X ('%s'), buffers=%d",
             unit->unit_id, unit->unit_name, unit->buffer_count);

    // Test 5: Find a buffer
    ESP_LOGD(TAG, "Test 5: Finding buffer...");
    const rbpower_buffer_runtime_t* found_buf = rbpower_runtime_find_buffer(
        &manager, 0x4401, RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ELECTRICAL);
    if (!found_buf) {
        ESP_LOGE(TAG, "✗ Test 5 FAILED: buffer not found");
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "✓ Test 5 PASSED: found buffer at offset=%lu, size=%lu",
             (unsigned long)found_buf->flash_offset,
             (unsigned long)found_buf->buffer_size_bytes);

    // Test 6: Print the configuration
    ESP_LOGD(TAG, "Test 6: Dumping configuration...");
    rbpower_runtime_config_dump(&manager);
    ESP_LOGD(TAG, "✓ Test 6 PASSED");

    // Test 7: Save to JSON
    ESP_LOGD(TAG, "Test 7: Saving unit config to JSON...");
    ret = rbpower_runtime_config_save_unit(&manager, 0x4401);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 7 FAILED: %s", esp_err_to_name(ret));
        rbpower_runtime_config_deinit(&manager);
        return ret;
    }
    ESP_LOGD(TAG, "✓ Test 7 PASSED");

    // Test 8: Deinitialize to free memory
    ESP_LOGD(TAG, "Test 8: Clearing manager for reload test...");
    rbpower_runtime_config_deinit(&manager);
    ESP_LOGD(TAG, "✓ Test 8 PASSED");

    // Test 9: Load from JSON
    ESP_LOGD(TAG, "Test 9: Re-initializing and loading from JSON...");
    ret = rbpower_runtime_config_init(&manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 9 FAILED (init): %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rbpower_runtime_config_load_unit(&manager, 0x4401);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 9 FAILED (load): %s", esp_err_to_name(ret));
        rbpower_runtime_config_deinit(&manager);
        return ret;
    }
    ESP_LOGD(TAG, "✓ Test 9 PASSED");

    // Test 10: Verify the loaded data
    ESP_LOGD(TAG, "Test 10: Verifying loaded data...");
    unit = rbpower_runtime_find_unit(&manager, 0x4401);
    if (!unit) {
        ESP_LOGE(TAG, "✗ Test 10 FAILED: unit not found after reload");
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }

    if (strcmp(unit->unit_name, "Main Supply") != 0) {
        ESP_LOGE(TAG, "✗ Test 10 FAILED: unit name mismatch");
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }

    if (unit->buffer_count != 2) {
        ESP_LOGE(TAG, "✗ Test 10 FAILED: buffer count mismatch (expected 2, got %d)",
                 unit->buffer_count);
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }

    found_buf = rbpower_runtime_find_buffer(&manager, 0x4401,
                                             RBPOWER_PERIOD_HOUR,
                                             RBPOWER_DATA_TYPE_ELECTRICAL);
    if (!found_buf) {
        ESP_LOGE(TAG, "✗ Test 10 FAILED: buffer not found after reload");
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }

    if (found_buf->total_sectors != 6 || found_buf->record_size != 28) {
        ESP_LOGE(TAG, "✗ Test 10 FAILED: buffer data mismatch");
        rbpower_runtime_config_deinit(&manager);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "✓ Test 10 PASSED: all data verified correctly");

    // Test 11: Print the configuration after loading
    ESP_LOGD(TAG, "Test 11: Dumping reloaded configuration...");
    rbpower_runtime_config_dump(&manager);
    ESP_LOGD(TAG, "✓ Test 11 PASSED");

    // Test 12: Final deinitialization
    ESP_LOGD(TAG, "Test 12: Final cleanup...");
    rbpower_runtime_config_deinit(&manager);
    ESP_LOGD(TAG, "✓ Test 12 PASSED");

    ESP_LOGI(TAG, "===  ALL RUNTIME CONFIG TESTS PASSED ✓  ===");

    return ESP_OK;
}
