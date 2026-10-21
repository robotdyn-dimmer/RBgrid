/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_config.c
 * @brief Static flash-buffer layout configuration (default layout, validation, lookup, dump)
 *
 * Load/save of the JSON file are not implemented: load returns the default layout and save
 * returns ESP_ERR_NOT_SUPPORTED. The runtime slot configuration lives in
 * rbextflash_config_runtime.c.
 */

#include "rbextflash_config.h"
#include "rbextflash_structures.h"
#include "external_flash_config.h"
#include "littlefs_raw.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "RBFLASH_CFG";

/* ================================================================================================
 * INITIALISATION
 * ================================================================================================ */

esp_err_t rbpower_flash_config_init(void)
{
    ESP_LOGD(TAG, "Initializing Flash config module...");

    // Check that the /config filesystem is available
    if (!external_flash_is_config_mounted()) {
        ESP_LOGE(TAG, "Config partition not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Config partition is mounted at: %s",
             external_flash_get_config_mount_point());

    // Check whether the configuration file exists
    struct stat st;
    if (stat(RBPOWER_CONFIG_PATH, &st) == 0) {
        ESP_LOGD(TAG, "Config file exists: %s (size: %ld bytes)",
                 RBPOWER_CONFIG_PATH, st.st_size);
    } else {
        ESP_LOGW(TAG, "Config file not found: %s", RBPOWER_CONFIG_PATH);
        ESP_LOGD(TAG, "Will need to create default configuration on first use");
    }

    ESP_LOGI(TAG, "Flash config module initialized");
    return ESP_OK;
}

/* ================================================================================================
 * DEFAULT CONFIGURATION
 * ================================================================================================ */

esp_err_t rbpower_flash_config_create_default(rbpower_flash_config_t* config_out)
{
    if (!config_out) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Creating default configuration...");

    memset(config_out, 0, sizeof(rbpower_flash_config_t));

    // Version
    config_out->version_major = RBPOWER_CONFIG_VERSION_MAJOR;
    config_out->version_minor = RBPOWER_CONFIG_VERSION_MINOR;

    // STATS partition information
    uint32_t stats_offset, stats_size;
    if (external_flash_get_partition_info("stats", &stats_offset, &stats_size)) {
        config_out->stats_partition_offset = stats_offset;
        config_out->stats_partition_size = stats_size;
        ESP_LOGD(TAG, "STATS partition: offset=0x%08lX, size=%lu bytes",
                 (unsigned long)stats_offset, (unsigned long)stats_size);
    } else {
        ESP_LOGE(TAG, "Failed to get STATS partition info");
        return ESP_FAIL;
    }

    // Timestamp (seconds since boot, esp_timer)
    config_out->last_updated = esp_timer_get_time() / 1000000ULL;

    // Create one example unit
    config_out->unit_count = 1;

    rbpower_flash_unit_config_t* unit = &config_out->units[0];
    unit->unit_id = 0x4401;
    snprintf(unit->unit_name, sizeof(unit->unit_name), "Main Supply");
    unit->unit_role = 1;  // MAIN_SUPPLY
    unit->enabled = true;
    unit->creation_timestamp = config_out->last_updated;

    // Create the period buffers for each data type
    uint32_t current_offset = 0;
    unit->buffer_count = 0;

    // Local helper that appends one buffer to `unit` at `current_offset` (hidden from Doxygen)
    /// @cond INTERNAL
    #define ADD_BUFFER(per, dtype, rec_size, sectors) do { \
        rbpower_flash_buffer_config_t* buf = &unit->buffers[unit->buffer_count++]; \
        buf->period = per; \
        buf->data_type = dtype; \
        buf->enabled = true; \
        buf->flash_offset = current_offset; \
        buf->record_size = rec_size; \
        buf->total_sectors = sectors; \
        buf->records_per_sector = 4096 / rec_size; \
        buf->buffer_size_bytes = sectors * 4096; \
        buf->physical_capacity = buf->records_per_sector * sectors; \
        buf->logical_capacity = RBPOWER_RECORDS_COUNT[per]; \
        current_offset += buf->buffer_size_bytes; \
        ESP_LOGD(TAG, "  Buffer[%d]: period=%d, type=%d, offset=%lu, size=%lu, sectors=%d", \
                 unit->buffer_count - 1, per, dtype, \
                 (unsigned long)buf->flash_offset, \
                 (unsigned long)buf->buffer_size_bytes, \
                 sectors); \
    } while(0)

    // HOUR period buffers (1440 records = 60 days; grown using the sectors freed by the removed quality/direction buffers)
    ADD_BUFFER(RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ELECTRICAL, 28, 10);
    ADD_BUFFER(RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ENERGY, 16, 6);
    ADD_BUFFER(RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ECONOMICS, 16, 6);
    // QUALITY/DIRECTION buffers removed — no ZC/THD source in the rbAmp fleet (sectors freed → HOUR retention).

    // DAY period buffers
    ADD_BUFFER(RBPOWER_PERIOD_DAY, RBPOWER_DATA_TYPE_ELECTRICAL, 28, 2);
    ADD_BUFFER(RBPOWER_PERIOD_DAY, RBPOWER_DATA_TYPE_ENERGY, 16, 2);
    ADD_BUFFER(RBPOWER_PERIOD_DAY, RBPOWER_DATA_TYPE_ECONOMICS, 16, 2);

    #undef ADD_BUFFER
    /// @endcond

    ESP_LOGD(TAG, "Default configuration created | Units: %d | Buffers: %d | Flash used: %lu bytes (%.1f KB)",
             config_out->unit_count, unit->buffer_count,
             (unsigned long)current_offset, (float)current_offset / 1024.0f);

    return ESP_OK;
}

/* ================================================================================================
 * CONFIGURATION VALIDATION
 * ================================================================================================ */

bool rbpower_flash_config_validate(const rbpower_flash_config_t* config)
{
    if (!config) {
        ESP_LOGE(TAG, "NULL config pointer");
        return false;
    }

    // Check version
    if (config->version_major != RBPOWER_CONFIG_VERSION_MAJOR) {
        ESP_LOGE(TAG, "Incompatible config version: %d.%d (expected %d.x)",
                 config->version_major, config->version_minor,
                 RBPOWER_CONFIG_VERSION_MAJOR);
        return false;
    }

    // Check unit count
    if (config->unit_count > RBPOWER_CONFIG_MAX_UNITS) {
        ESP_LOGE(TAG, "Too many units: %d (max %d)",
                 config->unit_count, RBPOWER_CONFIG_MAX_UNITS);
        return false;
    }

    ESP_LOGD(TAG, "Validating configuration: %d units", config->unit_count);

    // Check each unit
    for (int i = 0; i < config->unit_count; i++) {
        const rbpower_flash_unit_config_t* unit = &config->units[i];

        if (unit->buffer_count > RBPOWER_CONFIG_MAX_BUFFERS) {
            ESP_LOGE(TAG, "Unit 0x%04X: too many buffers: %d (max %d)",
                     unit->unit_id, unit->buffer_count, RBPOWER_CONFIG_MAX_BUFFERS);
            return false;
        }

        // Check buffers
        for (int j = 0; j < unit->buffer_count; j++) {
            const rbpower_flash_buffer_config_t* buf = &unit->buffers[j];

            // Check basic parameters
            if (buf->period >= RBPOWER_PERIOD_MAX) {
                ESP_LOGE(TAG, "Unit 0x%04X buffer[%d]: invalid period %d",
                         unit->unit_id, j, buf->period);
                return false;
            }

            if (buf->data_type >= RBPOWER_DATA_TYPE_MAX) {
                ESP_LOGE(TAG, "Unit 0x%04X buffer[%d]: invalid data type %d",
                         unit->unit_id, j, buf->data_type);
                return false;
            }

            // Check sizes
            if (buf->total_sectors == 0 || buf->record_size == 0) {
                ESP_LOGE(TAG, "Unit 0x%04X buffer[%d]: invalid sizes",
                         unit->unit_id, j);
                return false;
            }

            // Check derived values (mismatch is only a warning)
            uint16_t expected_rps = 4096 / buf->record_size;
            if (buf->records_per_sector != expected_rps) {
                ESP_LOGW(TAG, "Unit 0x%04X buffer[%d]: records_per_sector mismatch: %d vs %d",
                         unit->unit_id, j, buf->records_per_sector, expected_rps);
            }
        }
    }

    ESP_LOGD(TAG, "Configuration validation PASSED");
    return true;
}

/* ================================================================================================
 * CONFIGURATION LOOKUP
 * ================================================================================================ */

const rbpower_flash_unit_config_t* rbpower_flash_config_find_unit(
    const rbpower_flash_config_t* config,
    rbpower_unit_id_t unit_id)
{
    if (!config) {
        return NULL;
    }

    for (int i = 0; i < config->unit_count; i++) {
        if (config->units[i].unit_id == unit_id) {
            return &config->units[i];
        }
    }

    return NULL;
}

const rbpower_flash_buffer_config_t* rbpower_flash_config_find_buffer(
    const rbpower_flash_config_t* config,
    rbpower_unit_id_t unit_id,
    rbpower_period_t period,
    rbpower_data_type_t data_type)
{
    const rbpower_flash_unit_config_t* unit = rbpower_flash_config_find_unit(config, unit_id);
    if (!unit) {
        return NULL;
    }

    for (int i = 0; i < unit->buffer_count; i++) {
        const rbpower_flash_buffer_config_t* buf = &unit->buffers[i];
        if (buf->period == period && buf->data_type == data_type) {
            return buf;
        }
    }

    return NULL;
}

/* ================================================================================================
 * LOG OUTPUT
 * ================================================================================================ */

void rbpower_flash_config_dump(const rbpower_flash_config_t* config)
{
    if (!config) {
        ESP_LOGE(TAG, "NULL config pointer");
        return;
    }
    ESP_LOGI(TAG, " ===  RBPOWER FLASH CONFIGURATION   ===");
    ESP_LOGI(TAG, "Version: %d.%d", config->version_major, config->version_minor);
    ESP_LOGI(TAG, "STATS partition: offset=0x%08lX, size=%lu KB",
             (unsigned long)config->stats_partition_offset,
             (unsigned long)config->stats_partition_size / 1024);
    ESP_LOGI(TAG, "Units: %d", config->unit_count);
    ESP_LOGI(TAG, "");

    for (int i = 0; i < config->unit_count; i++) {
        const rbpower_flash_unit_config_t* unit = &config->units[i];

        ESP_LOGI(TAG, "Unit[%d]: ID=0x%04X, Name='%s', Role=%d, Enabled=%s",
                 i, unit->unit_id, unit->unit_name, unit->unit_role,
                 unit->enabled ? "YES" : "NO");
        ESP_LOGI(TAG, "  Buffers: %d", unit->buffer_count);

        for (int j = 0; j < unit->buffer_count; j++) {
            const rbpower_flash_buffer_config_t* buf = &unit->buffers[j];

            const char* period_names[] = {"HOUR", "DAY", "WEEK", "MONTH", "YEAR"};
            const char* type_names[] = {"ELECTRICAL", "ENERGY", "ECONOMICS", "QUALITY", "DIRECTION"};

            ESP_LOGI(TAG, "    [%d] %s/%s: offset=%lu, size=%lu, sectors=%d, capacity=%lu/%lu, %s",
                     j,
                     period_names[buf->period],
                     type_names[buf->data_type],
                     (unsigned long)buf->flash_offset,
                     (unsigned long)buf->buffer_size_bytes,
                     buf->total_sectors,
                     (unsigned long)buf->physical_capacity,
                     (unsigned long)buf->logical_capacity,
                     buf->enabled ? "ON" : "OFF");
        }
        ESP_LOGI(TAG, "");
    }
}

/* ================================================================================================
 * LOAD/SAVE STUBS (not implemented)
 * ================================================================================================ */

esp_err_t rbpower_flash_config_load(rbpower_flash_config_t* config_out)
{
    ESP_LOGW(TAG, "rbpower_flash_config_load() - NOT YET IMPLEMENTED | Using default configuration instead");
    return rbpower_flash_config_create_default(config_out);
}

esp_err_t rbpower_flash_config_save(const rbpower_flash_config_t* config)
{
    ESP_LOGW(TAG, "rbpower_flash_config_save() - NOT YET IMPLEMENTED");
    return ESP_ERR_NOT_SUPPORTED;
}

/* ================================================================================================
 * TEST FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_flash_config_test(void)
{
    ESP_LOGI(TAG, "\n ===  FLASH CONFIG TEST  ===");

    // Allocate the configuration on the heap (the structure is large, ~50 KB)
    rbpower_flash_config_t* config = (rbpower_flash_config_t*)malloc(sizeof(rbpower_flash_config_t));
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for config (%d bytes)", sizeof(rbpower_flash_config_t));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "Allocated config structure: %d bytes", sizeof(rbpower_flash_config_t));

    // Test 1: create the default configuration
    ESP_LOGD(TAG, "Test 1: Creating default configuration...");
    esp_err_t ret = rbpower_flash_config_create_default(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Test 1 FAILED: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGD(TAG, "✓ Test 1 PASSED");

    // Test 2: validate the configuration
    ESP_LOGD(TAG, "Test 2: Validating configuration...");
    if (!rbpower_flash_config_validate(config)) {
        ESP_LOGE(TAG, "✗ Test 2 FAILED: validation failed");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "✓ Test 2 PASSED");

    // Test 3: find the unit
    ESP_LOGD(TAG, "Test 3: Finding unit...");
    const rbpower_flash_unit_config_t* unit = rbpower_flash_config_find_unit(config, 0x4401);
    if (!unit) {
        ESP_LOGE(TAG, "✗ Test 3 FAILED: unit not found");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "✓ Test 3 PASSED: found unit 0x%04X ('%s')", unit->unit_id, unit->unit_name);

    // Test 4: find the buffer
    ESP_LOGD(TAG, "Test 4: Finding buffer...");
    const rbpower_flash_buffer_config_t* buf = rbpower_flash_config_find_buffer(
        config, 0x4401, RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ELECTRICAL);
    if (!buf) {
        ESP_LOGE(TAG, "✗ Test 4 FAILED: buffer not found");
        ret = ESP_FAIL;
        goto cleanup;
    }
    ESP_LOGD(TAG, "✓ Test 4 PASSED: found buffer at offset=%lu, size=%lu",
             (unsigned long)buf->flash_offset,
             (unsigned long)buf->buffer_size_bytes);

    // Test 5: dump the configuration
    ESP_LOGD(TAG, "Test 5: Dumping configuration...");
    rbpower_flash_config_dump(config);
    ESP_LOGD(TAG, "✓ Test 5 PASSED");

    ESP_LOGI(TAG, "===  ALL CONFIG TESTS PASSED ✓ ===");

    ret = ESP_OK;

cleanup:
    // Free memory
    free(config);
    return ret;
}
