/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_config.c
 * @brief Data-file layout configuration tables and lookup functions.
 */

#include "rbstorage_config.h"
#include "rbstorage_transfer_types.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char* TAG = "RBSTORAGE_CONFIG";

/* ================================================================================================
 * CONFIGURATION TABLES
 * ================================================================================================ */

/*
 * Role -> processing type mapping (documented in rbstorage_config.h).
 */
const rbpower_unit_processing_type_t UNIT_PROCESSING_MAP[RBPOWER_STORAGE_ROLE_MAX] = {
    [RBPOWER_ROLE_MAIN_SUPPLY]        = RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN,
    [RBPOWER_ROLE_ALTERNATIVE_SUPPLY] = RBPOWER_UNIT_PROCESSING_SPECIAL_ALT,
    [RBPOWER_ROLE_CIRCUIT_BREAKER]    = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_DEVICE_LOAD]        = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_ZONE_LOAD]          = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_RENEWABLE_SOURCE]   = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_STORAGE_SYSTEM]     = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_EV_CHARGING]        = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_HEATING_COOLING]    = RBPOWER_UNIT_PROCESSING_NORMAL,
    [RBPOWER_ROLE_SMART_APPLIANCE]    = RBPOWER_UNIT_PROCESSING_NORMAL,
    // Remaining indexes default to 0 (RBPOWER_UNIT_PROCESSING_NORMAL)
};

/*
 * Main data-file configuration matrix (documented in rbstorage_config.h).
 * 
 * Format: [role][period] -> {electrical, energy, economics, quality, direction}
 */
const rbpower_period_files_config_t UNIT_FILES_CONFIG[RBPOWER_STORAGE_ROLE_MAX][RBPOWER_PERIOD_MAX] = {
    // MAIN_SUPPLY - full statistics, all components
    [RBPOWER_ROLE_MAIN_SUPPLY] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, true, true},   // All files
        [RBPOWER_PERIOD_DAY]   = {true, true, true, true, true},   // All files
        [RBPOWER_PERIOD_MONTH] = {true, true, true, false, false}, // No quality, direction
        [RBPOWER_PERIOD_YEAR]  = {true, true, true, false, false}  // Core components
    },
    
    // ALTERNATIVE_SUPPLY - no quality (no grid power-quality monitoring)
    [RBPOWER_ROLE_ALTERNATIVE_SUPPLY] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, true},  // No quality
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, true},  // No quality
        [RBPOWER_PERIOD_MONTH] = {true, true, true, false, false}, // Core
        [RBPOWER_PERIOD_YEAR]  = {false, true, false, false, false} // Energy only
    },
    
    // CIRCUIT_BREAKER - reduced statistics
    [RBPOWER_ROLE_CIRCUIT_BREAKER] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, false}, // No quality, direction
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, false},
        [RBPOWER_PERIOD_MONTH] = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_YEAR]  = {false, true, false, false, false} // Energy only
    },
    
    // DEVICE_LOAD - basic device statistics
    [RBPOWER_ROLE_DEVICE_LOAD] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, false}, // Core components
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, false},
        [RBPOWER_PERIOD_MONTH] = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_YEAR]  = {false, true, false, false, false} // Energy only
    },
    
    // ZONE_LOAD - zone load statistics
    [RBPOWER_ROLE_ZONE_LOAD] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, false}, // Core components
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, false},
        [RBPOWER_PERIOD_MONTH] = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_YEAR]  = {false, true, false, false, false} // Energy only
    },
    
    // RENEWABLE_SOURCE - full statistics for renewable sources
    [RBPOWER_ROLE_RENEWABLE_SOURCE] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, true},  // With direction
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, true},  // With direction
        [RBPOWER_PERIOD_MONTH] = {true, true, true, false, false}, // Core
        [RBPOWER_PERIOD_YEAR]  = {false, true, true, false, false} // Energy + economics
    },
    
    // STORAGE_SYSTEM - full statistics for energy storage
    [RBPOWER_ROLE_STORAGE_SYSTEM] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, true},  // With direction
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, true},  // With direction
        [RBPOWER_PERIOD_MONTH] = {true, true, true, false, false}, // Core
        [RBPOWER_PERIOD_YEAR]  = {false, true, true, false, false} // Energy + economics
    },
    
    // EV_CHARGING - EV charger statistics
    [RBPOWER_ROLE_EV_CHARGING] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, false}, // Core components
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, false},
        [RBPOWER_PERIOD_MONTH] = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_YEAR]  = {false, true, true, false, false}  // Energy + economics
    },
    
    // HEATING_COOLING - HVAC statistics
    [RBPOWER_ROLE_HEATING_COOLING] = {
        [RBPOWER_PERIOD_HOUR]  = {true, true, true, false, false}, // Core components
        [RBPOWER_PERIOD_DAY]   = {true, true, true, false, false},
        [RBPOWER_PERIOD_MONTH] = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_YEAR]  = {false, true, true, false, false}  // Energy + economics
    },
    
    // SMART_APPLIANCE - basic smart-appliance statistics
    [RBPOWER_ROLE_SMART_APPLIANCE] = {
        [RBPOWER_PERIOD_HOUR]  = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_DAY]   = {false, true, true, false, false}, // No electrical
        [RBPOWER_PERIOD_MONTH] = {false, true, true, false, false}, // Energy + economics
        [RBPOWER_PERIOD_YEAR]  = {false, true, false, false, false} // Energy only
    }
    
    // Remaining roles default to all-false (no files are created)
};

/*
 * Special folder names (documented in rbstorage_config.h).
 */
const char* SPECIAL_UNIT_NAMES[4] = {
    [RBPOWER_UNIT_PROCESSING_NORMAL] = NULL,
    [RBPOWER_UNIT_PROCESSING_EXCLUDE] = NULL,
    [RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN] = "Unit_MainSupply",
    [RBPOWER_UNIT_PROCESSING_SPECIAL_ALT] = "Unit_AltSupply"
};

/**
 * @brief Processing type names for debug output.
 */
static const char* PROCESSING_TYPE_NAMES[4] = {
    [RBPOWER_UNIT_PROCESSING_NORMAL] = "normal",
    [RBPOWER_UNIT_PROCESSING_EXCLUDE] = "exclude",
    [RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN] = "main_supply",
    [RBPOWER_UNIT_PROCESSING_SPECIAL_ALT] = "alt_supply"
};

/* ================================================================================================
 * PROCESSING TYPE FUNCTIONS
 * ================================================================================================ */

rbpower_unit_processing_type_t rbpower_storage_get_unit_processing_type(
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type)
{
    // VOLTAGE_BUS is excluded regardless of role
    if (unit_type == RBPOWER_METRIC_VOLTAGE_BUS) {
        ESP_LOGD(TAG, "Unit with type VOLTAGE_BUS excluded from storage");
        return RBPOWER_UNIT_PROCESSING_EXCLUDE;
    }
    
    // Validate the role
    if (unit_role >= RBPOWER_STORAGE_ROLE_MAX) {
        ESP_LOGW(TAG, "Invalid unit role: %d, using normal processing", unit_role);
        return RBPOWER_UNIT_PROCESSING_NORMAL;
    }
    
    return UNIT_PROCESSING_MAP[unit_role];
}

bool rbpower_storage_should_exclude_unit(
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type)
{
    return (rbpower_storage_get_unit_processing_type(unit_role, unit_type) == 
            RBPOWER_UNIT_PROCESSING_EXCLUDE);
}

esp_err_t rbpower_storage_get_unit_directory_name(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    char* directory_name,
    size_t name_size)
{
    if (directory_name == NULL || name_size < RBPOWER_STORAGE_MAX_DIR_NAME) {
        ESP_LOGE(TAG, "Invalid directory name buffer");
        return ESP_ERR_INVALID_ARG;
    }
    
    rbpower_unit_processing_type_t processing_type = 
        rbpower_storage_get_unit_processing_type(unit_role, unit_type);
    
    switch (processing_type) {
        case RBPOWER_UNIT_PROCESSING_EXCLUDE:
            ESP_LOGD(TAG, "Unit %04X excluded from storage", unit_id);
            return ESP_ERR_NOT_SUPPORTED;
            
        case RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN:
            strncpy(directory_name, "Unit_MainSupply", name_size - 1);
            directory_name[name_size - 1] = '\0';
            ESP_LOGD(TAG, "Unit %04X -> special directory: %s", unit_id, directory_name);
            break;
            
        case RBPOWER_UNIT_PROCESSING_SPECIAL_ALT:
            strncpy(directory_name, "Unit_AltSupply", name_size - 1);
            directory_name[name_size - 1] = '\0';
            ESP_LOGD(TAG, "Unit %04X -> special directory: %s", unit_id, directory_name);
            break;
            
        case RBPOWER_UNIT_PROCESSING_NORMAL:
        default:
            snprintf(directory_name, name_size, "%04X", unit_id);
            ESP_LOGD(TAG, "Unit %04X -> normal directory: %s", unit_id, directory_name);
            break;
    }
    
    return ESP_OK;
}

esp_err_t rbpower_storage_get_files_config(
    rbpower_load_role_t unit_role,
    rbpower_period_type_t period_type,
    rbpower_period_files_config_t* config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (unit_role >= RBPOWER_STORAGE_ROLE_MAX || period_type >= RBPOWER_PERIOD_MAX) {
        ESP_LOGE(TAG, "Invalid role (%d) or period (%d)", unit_role, period_type);
        return ESP_ERR_INVALID_ARG;
    }
    
    *config = UNIT_FILES_CONFIG[unit_role][period_type];
    ESP_LOGV(TAG, "Config for role %d, period %d: e=%d en=%d ec=%d q=%d d=%d",
             unit_role, period_type,
             config->save_electrical, config->save_energy, config->save_economics,
             config->save_quality, config->save_direction);
    
    return ESP_OK;
}

bool rbpower_storage_should_create_file(
    rbpower_load_role_t unit_role,
    rbpower_period_type_t period_type,
    rbpower_data_type_t data_type)
{
    if (unit_role >= RBPOWER_STORAGE_ROLE_MAX || 
        period_type >= RBPOWER_PERIOD_MAX ||
        data_type >= RBPOWER_DATA_TYPE_MAX) {
        ESP_LOGW(TAG, "Invalid parameters: role=%d, period=%d, data_type=%d",
                 unit_role, period_type, data_type);
        return false;
    }
    
    const rbpower_period_files_config_t* config = &UNIT_FILES_CONFIG[unit_role][period_type];
    
    switch (data_type) {
        case RBPOWER_DATA_TYPE_ELECTRICAL:
            return config->save_electrical;
        case RBPOWER_DATA_TYPE_ENERGY:
            return config->save_energy;
        case RBPOWER_DATA_TYPE_ECONOMICS:
            return config->save_economics;
        case RBPOWER_DATA_TYPE_QUALITY:
            return config->save_quality;
        case RBPOWER_DATA_TYPE_DIRECTION:
            return config->save_direction;
        default:
            return false;
    }
}

/* ================================================================================================
 * FILTERING FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_storage_filter_units_for_stats(
    const rbpower_unit_id_t* input_units,
    uint8_t input_count,
    rbpower_unit_id_t** filtered_units,
    uint8_t* filtered_count)
{
    if (input_units == NULL || filtered_units == NULL || filtered_count == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for units filtering");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (input_count == 0) {
        *filtered_units = NULL;
        *filtered_count = 0;
        return ESP_OK;
    }
    
    // Allocate a buffer of the maximum size
    rbpower_unit_id_t* temp_units = malloc(input_count * sizeof(rbpower_unit_id_t));
    if (temp_units == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for filtered units");
        return ESP_ERR_NO_MEM;
    }
    
    uint8_t count = 0;
    for (uint8_t i = 0; i < input_count; i++) {
        rbpower_unit_id_t unit_id = input_units[i];
        
        // For now only the VOLTAGE_BUS check is applied
        // TODO: look up the unit definition from rbhome
        
        // Exclude the predefined VOLTAGE_BUS unit
        if (unit_id == 0x0007) {  // RBGRID_UNIT_VOLTAGE_BUS from rbgrid.h
            ESP_LOGD(TAG, "Excluding VOLTAGE_BUS unit %04X from statistics", unit_id);
            continue;
        }
        
        temp_units[count++] = unit_id;
        ESP_LOGV(TAG, "Including unit %04X in statistics", unit_id);
    }
    
    // Shrink the array to the actual size
    if (count == 0) {
        free(temp_units);
        *filtered_units = NULL;
        *filtered_count = 0;
    } else {
        rbpower_unit_id_t* final_units = realloc(temp_units, count * sizeof(rbpower_unit_id_t));
        if (final_units == NULL) {
            ESP_LOGW(TAG, "Failed to shrink filtered units array, keeping original");
            final_units = temp_units;  // Keep the original buffer
        }
        *filtered_units = final_units;
        *filtered_count = count;
    }
    
    ESP_LOGI(TAG, "Filtered units: %d -> %d (excluded %d units)", 
             input_count, *filtered_count, input_count - *filtered_count);
    
    return ESP_OK;
}

uint8_t rbpower_storage_count_unit_files(rbpower_load_role_t unit_role)
{
    if (unit_role >= RBPOWER_STORAGE_ROLE_MAX) {
        ESP_LOGW(TAG, "Invalid role %d for file counting", unit_role);
        return 0;
    }
    
    uint8_t total_files = 0;
    
    for (rbpower_period_type_t period = 0; period < RBPOWER_PERIOD_MAX; period++) {
        const rbpower_period_files_config_t* config = &UNIT_FILES_CONFIG[unit_role][period];
        
        if (config->save_electrical) total_files++;
        if (config->save_energy) total_files++;
        if (config->save_economics) total_files++;
        if (config->save_quality) total_files++;
        if (config->save_direction) total_files++;
    }
    
    ESP_LOGV(TAG, "Role %d will have %d files total", unit_role, total_files);
    return total_files;
}

uint64_t rbpower_storage_estimate_unit_size(rbpower_load_role_t unit_role)
{
    if (unit_role >= RBPOWER_STORAGE_ROLE_MAX) {
        ESP_LOGW(TAG, "Invalid role %d for size estimation", unit_role);
        return 0;
    }
    
    uint64_t total_size = 0;
    const uint32_t header_size = 64;  // File header size, bytes
    
    for (rbpower_period_type_t period = 0; period < RBPOWER_PERIOD_MAX; period++) {
        const rbpower_period_files_config_t* config = &UNIT_FILES_CONFIG[unit_role][period];
        uint32_t records_count = rbpower_get_records_count(period);
        
        if (config->save_electrical) {
            total_size += header_size + (records_count * rbpower_get_transfer_size(RBPOWER_DATA_TYPE_ELECTRICAL));
        }
        if (config->save_energy) {
            total_size += header_size + (records_count * rbpower_get_transfer_size(RBPOWER_DATA_TYPE_ENERGY));
        }
        if (config->save_economics) {
            total_size += header_size + (records_count * rbpower_get_transfer_size(RBPOWER_DATA_TYPE_ECONOMICS));
        }
        if (config->save_quality) {
            total_size += header_size + (records_count * rbpower_get_transfer_size(RBPOWER_DATA_TYPE_QUALITY));
        }
        if (config->save_direction) {
            total_size += header_size + (records_count * rbpower_get_transfer_size(RBPOWER_DATA_TYPE_DIRECTION));
        }
    }
    
    ESP_LOGD(TAG, "Estimated size for role %d: %llu bytes (%.1f KB)", 
             unit_role, total_size, (float)total_size / 1024.0f);
    
    return total_size;
}

/* ================================================================================================
 * UTILITY FUNCTIONS
 * ================================================================================================ */

const char* rbpower_storage_get_processing_type_name(rbpower_unit_processing_type_t processing_type)
{
    if (processing_type < 4) {
        return PROCESSING_TYPE_NAMES[processing_type];
    }
    return "unknown";
}

bool rbpower_storage_is_role_valid(rbpower_load_role_t unit_role)
{
    return (unit_role < RBPOWER_STORAGE_ROLE_MAX);
}

void rbpower_storage_print_role_config(rbpower_load_role_t unit_role)
{
    if (unit_role >= RBPOWER_STORAGE_ROLE_MAX) {
        ESP_LOGW(TAG, "Invalid role %d for config printing", unit_role);
        return;
    }
    
    ESP_LOGI(TAG, "Configuration for role %d:", unit_role);
    ESP_LOGI(TAG, "  Processing type: %s", 
             rbpower_storage_get_processing_type_name(UNIT_PROCESSING_MAP[unit_role]));
    ESP_LOGI(TAG, "  Total files: %d", rbpower_storage_count_unit_files(unit_role));
    ESP_LOGI(TAG, "  Estimated size: %llu bytes", rbpower_storage_estimate_unit_size(unit_role));
    
    ESP_LOGI(TAG, "  Files by period:");
    for (rbpower_period_type_t period = 0; period < RBPOWER_PERIOD_MAX; period++) {
        const rbpower_period_files_config_t* config = &UNIT_FILES_CONFIG[unit_role][period];
        ESP_LOGI(TAG, "    %s: elec=%d, ener=%d, econ=%d, qual=%d, dir=%d",
                 rbpower_get_period_name(period),
                 config->save_electrical, config->save_energy, config->save_economics,
                 config->save_quality, config->save_direction);
    }
}