/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome.c
 * @brief Home electrical network (rbhome): unit registry, measurement injection, 3-second and
 *        200 ms accumulation, external period results, identity/bind API and boot forensics.
 *
 * Owns g_home_network and its recursive lock (rbpower_home_lock()). Measurements arrive only
 * through the inject API (rbAmp I2C bridge, ESP-NOW hub); the native ESP32-ADC engine has been
 * removed. Finished 15-minute results are rolled up by rbhome_statistics.c.
 */
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbhome_types.h"
#include "rbhome_tariff.h"
#include "rbpower.h"
#include "rbtelemetry.h"
#include "rbhome_main_supply.h"
#include "rbhome_alt_supply.h"
#include "rbpower_lifetime_api.h"
#include <stdarg.h>
#include "rbextflash_operations.h"   /* rbpower_flash_remove_unit — unit deletion reclaims its slot */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR — place g_home_network in PSRAM */
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */

//=============================================================================
// HELPER FUNCTIONS for home_init
//=============================================================================
static rbpower_unit_definition_t* find_unit_definition_by_id(rbpower_unit_id_t unit_id);
static void initialize_specialized_structures(const rbpower_extended_config_t* config);
static void initialize_unit_statistics(uint8_t unit_index, rbpower_unit_id_t unit_id, rbpower_stat_period_type_t stat_class);
static void initialize_unit_realtime_data(uint8_t unit_index, rbpower_unit_id_t unit_id);
static void create_adc_source_mapping(rbpower_unit_id_t unit_id, uint8_t adc_channel, uint8_t sensor_profile_id);
static void assign_system_units_by_type_and_role(void);
static bool validate_predefined_unit(const rbpower_unit_definition_t* unit);
/* Defined in rbhome_statistics.c (same amalgamated translation unit, see rbpower.c). */
static bool should_aggregate_to_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t target_period);
static void aggregate_15min_from_3sec_period(rbpower_unit_id_t unit_id, const rbpower_period_statistics_t *completed_3sec_period);
//=============================================================================
// NEW UNIT_ID SYSTEM - IMPLEMENTATION
//=============================================================================

/*
 * @brief Create unit_id for ADC source
 */
rbpower_unit_id_t rbpower_create_adc_unit_id(uint8_t sensor_profile_id, uint8_t adc_channel)
{
    rbpower_unit_id_t unit_id = 0;
    
    // Validate parameters
    if (sensor_profile_id >= RBPOWER_MAX_SENSOR_PROFILES) {
        ESP_LOGE("RBHOME", "Invalid sensor_profile_id: %d (max %d)", 
                 sensor_profile_id, RBPOWER_MAX_SENSOR_PROFILES - 1);
        return RBPOWER_HOME_INVALID_ID;
    }
    
    // Form unit_id
    unit_id |= (RBPOWER_UNIT_TYPE_ADC & RBPOWER_UNIT_TYPE_MASK);
    unit_id |= ((sensor_profile_id << RBPOWER_UNIT_PROFILE_SHIFT) & RBPOWER_UNIT_PROFILE_MASK);
    unit_id |= (adc_channel & RBPOWER_UNIT_LOCAL_MASK);
    
    return unit_id;
}

/*
 * @brief Create unit_id for I2C source
 */
rbpower_unit_id_t rbpower_create_i2c_unit_id(uint8_t sensor_profile_id, uint8_t i2c_bus_num, uint8_t i2c_address)
{
    rbpower_unit_id_t unit_id = 0;
    
    // Validate parameters
    if (sensor_profile_id >= RBPOWER_MAX_SENSOR_PROFILES) {
        ESP_LOGE("RBHOME", "Invalid sensor_profile_id: %d", sensor_profile_id);
        return RBPOWER_HOME_INVALID_ID;
    }
    if (i2c_bus_num >= RBPOWER_MAX_I2C_BUSES) {
        ESP_LOGE("RBHOME", "Invalid i2c_bus_num: %d", i2c_bus_num);
        return RBPOWER_HOME_INVALID_ID;
    }
    if (i2c_address >= RBPOWER_MAX_I2C_ADDRESSES) {
        ESP_LOGE("RBHOME", "Invalid i2c_address: 0x%02X", i2c_address);
        return RBPOWER_HOME_INVALID_ID;
    }
    
    // Form unit_id
    unit_id |= (RBPOWER_UNIT_TYPE_I2C & RBPOWER_UNIT_TYPE_MASK);
    unit_id |= ((sensor_profile_id << RBPOWER_UNIT_PROFILE_SHIFT) & RBPOWER_UNIT_PROFILE_MASK);
    
    // Pack bus_num and address into LOCAL field
    uint8_t local_info = 0;
    local_info |= ((i2c_bus_num << RBPOWER_I2C_BUS_SHIFT) & RBPOWER_I2C_BUS_MASK);
    local_info |= (i2c_address & RBPOWER_I2C_ADDR_MASK);
    unit_id |= local_info;
    
    return unit_id;
}

/*
 * @brief Create unit_id for SPI source
 */
rbpower_unit_id_t rbpower_create_spi_unit_id(uint8_t sensor_profile_id, uint8_t spi_device_id)
{
    rbpower_unit_id_t unit_id = 0;
    
    // Validate parameters
    if (sensor_profile_id >= RBPOWER_MAX_SENSOR_PROFILES) {
        ESP_LOGE("RBHOME", "Invalid sensor_profile_id: %d", sensor_profile_id);
        return RBPOWER_HOME_INVALID_ID;
    }
    
    // Form unit_id
    unit_id |= (RBPOWER_UNIT_TYPE_SPI & RBPOWER_UNIT_TYPE_MASK);
    unit_id |= ((sensor_profile_id << RBPOWER_UNIT_PROFILE_SHIFT) & RBPOWER_UNIT_PROFILE_MASK);
    unit_id |= (spi_device_id & RBPOWER_UNIT_LOCAL_MASK);
    
    return unit_id;
}

/*
 * @brief Create unit_id for ESP-NOW source
 */
rbpower_unit_id_t rbpower_create_espnow_unit_id(uint8_t sensor_profile_id, const uint8_t device_mac[6])
{
    rbpower_unit_id_t unit_id = 0;
    
    // Validate parameters
    if (sensor_profile_id >= RBPOWER_MAX_SENSOR_PROFILES || !device_mac) {
        ESP_LOGE("RBHOME", "Invalid parameters for ESP-NOW unit_id");
        return RBPOWER_HOME_INVALID_ID;
    }
    
    // Form unit_id
    unit_id |= (RBPOWER_UNIT_TYPE_ESP_NOW & RBPOWER_UNIT_TYPE_MASK);
    unit_id |= ((sensor_profile_id << RBPOWER_UNIT_PROFILE_SHIFT) & RBPOWER_UNIT_PROFILE_MASK);
    
    // Simple hash from last 2 bytes of MAC address
    uint8_t mac_hash = device_mac[4] ^ device_mac[5];
    unit_id |= (mac_hash & RBPOWER_UNIT_LOCAL_MASK);
    
    return unit_id;
}

/*
 * @brief Get source type from unit_id
 */
uint16_t rbpower_get_unit_type(rbpower_unit_id_t unit_id)
{
    return (unit_id & RBPOWER_UNIT_TYPE_MASK);
}

/*
 * @brief Get sensor profile ID from unit_id
 */
uint8_t rbpower_get_sensor_profile_id(rbpower_unit_id_t unit_id)
{
    return (uint8_t)((unit_id & RBPOWER_UNIT_PROFILE_MASK) >> RBPOWER_UNIT_PROFILE_SHIFT);
}

/*
 * @brief Get local information from unit_id
 */
uint8_t rbpower_get_unit_local_info(rbpower_unit_id_t unit_id)
{
    return (uint8_t)(unit_id & RBPOWER_UNIT_LOCAL_MASK);
}

/*
 * @brief Check if unit is local (ADC)
 */
bool rbpower_is_local_unit(rbpower_unit_id_t unit_id)
{
    return (rbpower_get_unit_type(unit_id) == RBPOWER_UNIT_TYPE_ADC);
}
 
// External variables from statistics system
extern rbpower_statistics_system_t g_stats_system;
extern rbpower_period_accumulator_t g_period_accumulators[RBPOWER_MAX_HOME_UNITS][RBPOWER_RAM_PERIOD_COUNT];

// External functions from statistics system
/** @brief Whether the statistics system is initialized (rbhome_statistics.c). @return true if initialized. */
extern bool rbpower_statistics_is_initialized(void);
extern void reset_period_accumulator(rbpower_period_accumulator_t* acc);


//=============================================================================
// RBHOME INTEGRATION - Internal implementation
//=============================================================================
// Hidden home network instance. EXT_RAM_BSS_ATTR places this large struct (units[] +
// supply + accumulators) in PSRAM instead of internal DRAM — it scales with the unit count
// (rbAmp per-unit) and would otherwise blow the 520KB SRAM. Cold-ish data; the realtime
// hot path touches one unit at a time (cached). DMA buffers live elsewhere (internal).
EXT_RAM_BSS_ATTR rbpower_electrical_network_t g_home_network = {0};
static bool g_home_network_initialized = false;

// Recursive mutex guarding all cross-task access to g_home_network. WRITERS (the esp_timer
// 3-sec period task, the rbAmp / ESP-NOW inject paths) and
// READERS (the httpd config/telemetry JSON builders on the webserver task) run on different
// tasks; without this they race → torn floats / inconsistent unit arrays.
// RECURSIVE so a write transaction that calls a self-locking JSON
// builder (e.g. end_3sec → create_telemetry) does not self-deadlock. NULL before
// rbpower_home_init() creates it → lock/unlock are no-ops then (no other task runs yet).
static SemaphoreHandle_t g_home_network_mutex = NULL;

void rbpower_home_lock(void) {
    if (g_home_network_mutex) {
        xSemaphoreTakeRecursive(g_home_network_mutex, portMAX_DELAY);
    }
}

bool rbpower_home_lock_timeout(uint32_t timeout_ms) {
    if (!g_home_network_mutex) return true;
    return xSemaphoreTakeRecursive(g_home_network_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* Count active metering units (valid + active, excluding the voltage bus) — provisioning status. */
uint8_t rbpower_home_active_unit_count(void) {
    uint8_t n = 0;
    rbpower_home_lock();
    for (int i = 0; i < g_home_network.units_count; i++) {
        rbpower_home_unit_t *u = &g_home_network.units[i];
        if (u->has_valid_definition && u->definition.is_active &&
            u->definition.type != RBPOWER_METRIC_VOLTAGE_BUS) n++;
    }
    rbpower_home_unlock();
    return n;
}

void rbpower_home_unlock(void) {
    if (g_home_network_mutex) {
        xSemaphoreGiveRecursive(g_home_network_mutex);
    }
}
static rbpower_home_creation_result_t g_creation_result = {0};

// Mapping table from unit_id to indices in units[] array
static struct {
    rbpower_unit_id_t unit_id;
    uint8_t unit_index;
    bool is_valid;
} g_unit_id_map[RBPOWER_MAX_HOME_UNITS] = {0};

static esp_err_t rbpower_update_home_network_from_buffer(const rbpower_unit_realtime_halfcycle_t *buffer, uint8_t units_count, uint32_t sequence_number);
static esp_err_t rbpower_update_home_network_from_200ms_buffer(const rbpower_unit_realtime_average_200ms_t *buffer, uint8_t units_count, uint32_t sequence_number);
static void calculate_final_statistics(uint8_t unit_id, const rbpower_period_accumulator_t* acc, rbpower_period_statistics_t* stats);

//=============================================================================
// UNIT_ID MAPPING FUNCTIONS
//=============================================================================

/*
 * @brief Find index in units[] array by unit_id
 * @param unit_id Unit ID (16 bit)
 * @return Array index or 0xFF if not found
 */
uint8_t find_unit_index_by_id(rbpower_unit_id_t unit_id) {
    for (uint8_t i = 0; i < RBPOWER_MAX_HOME_UNITS; i++) {
        if (g_unit_id_map[i].is_valid && g_unit_id_map[i].unit_id == unit_id) {
            return g_unit_id_map[i].unit_index;
        }
    }
    return 0xFF;
}


/**
 * @brief Add mapping from unit_id to index
 * @param unit_id Unit ID (16 bits)
 * @param unit_index Index in units[] array
 * @return true on success
 */
static bool add_unit_id_mapping(rbpower_unit_id_t unit_id, uint8_t unit_index) {
    for (uint8_t i = 0; i < RBPOWER_MAX_HOME_UNITS; i++) {
        if (!g_unit_id_map[i].is_valid) {
            g_unit_id_map[i].unit_id = unit_id;
            g_unit_id_map[i].unit_index = unit_index;
            g_unit_id_map[i].is_valid = true;
            return true;
        }
    }
    ESP_LOGE("RBHOME", "Unit ID map full!");
    return false;
}
//=============================================================================
// STATISTICS ACCUMULATION MANAGEMENT
//=============================================================================
/** Accumulation control flags for each unit */
static struct {
    bool accumulation_enabled[RBPOWER_MAX_HOME_UNITS];  // Whether accumulation is enabled for unit
    bool ms200_done[RBPOWER_MAX_HOME_UNITS];            // Flag for switching to 200ms data
    uint8_t remaining_200ms_calls[RBPOWER_MAX_HOME_UNITS]; // Counter for remaining 200ms calls
    uint8_t expected_200ms_calls[RBPOWER_MAX_HOME_UNITS];  // Expected number of 200ms calls
    uint64_t period_start_time_us[RBPOWER_MAX_HOME_UNITS]; // Start time of current 3-sec period
} g_accumulation_control = {0};

/** Direct references to 3-sec period accumulators for fast access */
extern rbpower_period_accumulator_t g_period_accumulators[RBPOWER_MAX_HOME_UNITS][RBPOWER_RAM_PERIOD_COUNT];

//=============================================================================
// RBHOME API IMPLEMENTATION
//=============================================================================

/**
 * @brief Get period mask based on stat_class
 */
static uint32_t get_periods_mask_by_stat_class(rbpower_stat_period_type_t stat_class) {
    switch (stat_class) {
        case RBPOWER_STAT_PERIOD_3SEC:
            return (1 << RBPOWER_STAT_PERIOD_3SEC);  // Only 3 sec
            
        case RBPOWER_STAT_PERIOD_15MIN:
            return (1 << RBPOWER_STAT_PERIOD_3SEC) |  // 3 sec + 15 min
                   (1 << RBPOWER_STAT_PERIOD_15MIN);
                   
        case RBPOWER_STAT_PERIOD_1HOUR:
            return (1 << RBPOWER_STAT_PERIOD_3SEC) |  // 3 sec + 15 min + 1 hr
                   (1 << RBPOWER_STAT_PERIOD_15MIN) |
                   (1 << RBPOWER_STAT_PERIOD_1HOUR);
                   
        case RBPOWER_STAT_PERIOD_DAILY:
            return (1 << RBPOWER_STAT_PERIOD_3SEC) |  // Up to daily
                   (1 << RBPOWER_STAT_PERIOD_15MIN) |
                   (1 << RBPOWER_STAT_PERIOD_1HOUR) |
                   (1 << RBPOWER_STAT_PERIOD_DAILY);
                   
        case RBPOWER_STAT_PERIOD_WEEKLY:
            return (1 << RBPOWER_STAT_PERIOD_3SEC) |  // Up to weekly
                   (1 << RBPOWER_STAT_PERIOD_15MIN) |
                   (1 << RBPOWER_STAT_PERIOD_1HOUR) |
                   (1 << RBPOWER_STAT_PERIOD_DAILY) |
                   (1 << RBPOWER_STAT_PERIOD_WEEKLY);
                   
        case RBPOWER_STAT_PERIOD_MONTHLY:
            return (1 << RBPOWER_STAT_PERIOD_3SEC) |  // Up to monthly
                   (1 << RBPOWER_STAT_PERIOD_15MIN) |
                   (1 << RBPOWER_STAT_PERIOD_1HOUR) |
                   (1 << RBPOWER_STAT_PERIOD_DAILY) |
                   (1 << RBPOWER_STAT_PERIOD_WEEKLY) |
                   (1 << RBPOWER_STAT_PERIOD_MONTHLY);
                   
        case RBPOWER_STAT_PERIOD_YEARLY:
            return (1 << RBPOWER_STAT_PERIOD_3SEC) |  // All periods
                   (1 << RBPOWER_STAT_PERIOD_15MIN) |
                   (1 << RBPOWER_STAT_PERIOD_1HOUR) |
                   (1 << RBPOWER_STAT_PERIOD_DAILY) |
                   (1 << RBPOWER_STAT_PERIOD_WEEKLY) |
                   (1 << RBPOWER_STAT_PERIOD_MONTHLY) |
                   (1 << RBPOWER_STAT_PERIOD_YEARLY);
                   
        default:
            return (1 << RBPOWER_STAT_PERIOD_3SEC);   // Default: only 3 sec
    }
}

/**
 * @brief Get string with names of enabled periods for logging
 */
static void get_enabled_periods_string(uint32_t periods_mask, char* buffer, size_t buffer_size) {
    buffer[0] = '\0';
    bool first = true;
    
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_3SEC)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "3sec", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_15MIN)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "15min", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_1HOUR)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "1hr", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_DAILY)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "1day", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_WEEKLY)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "1week", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_MONTHLY)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "1month", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    if (periods_mask & (1 << RBPOWER_STAT_PERIOD_YEARLY)) {
        if (!first) strncat(buffer, " + ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "1year", buffer_size - strlen(buffer) - 1);
        first = false;
    }
    
    if (first) {
        strncpy(buffer, "none", buffer_size - 1);
    }
}

rbpower_err_t rbpower_home_init(const rbpower_extended_config_t* config) {
    if (!config) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!config->enable_home_network) {
        ESP_LOGI("RBHOME", "Home network disabled in config");
        return RBPOWER_OK;
    }
    
    if (g_home_network_initialized) {
        ESP_LOGI("RBHOME", "Home network already initialized");
        return RBPOWER_ERR_ALREADY_INIT;
    }
    
    ESP_LOGI("RBHOME", "Initializing home network integration");

    // Create the cross-task guard BEFORE any update task / httpd handler can touch g_home_network.
    if (g_home_network_mutex == NULL) {
        g_home_network_mutex = xSemaphoreCreateRecursiveMutex();
        if (g_home_network_mutex == NULL) {
            ESP_LOGE("RBHOME", "Failed to create g_home_network mutex");
            return RBPOWER_ERR_NO_MEMORY;
        }
    }

    // Initialize network structure
    memset(&g_home_network, 0, sizeof(rbpower_electrical_network_t));
    ESP_LOGI("RBHOME", "Structure size: %zu bytes", sizeof(rbpower_electrical_network_t));

    // Initialize arrays as invalid
    for (uint8_t i = 0; i < RBPOWER_MAX_HOME_UNITS; i++) {
        g_home_network.units[i].has_valid_definition = false;
        g_home_network.units[i].has_realtime_data = false;
        g_home_network.units[i].has_statistics_enabled = false;
        g_home_network.units[i].definition.unit_id = 0; // RBPOWER_HOME_INVALID_ID = 0
        g_home_network.units[i].definition.parent_unit_id = 0;
    }
    
    // Initialize system metrics
    g_home_network.system.voltage_bus_unit_id = 0; // RBPOWER_HOME_INVALID_ID = 0
    g_home_network.system.main_supply_unit_id = 0;
    g_home_network.system.alternative_supply_unit_id = 0;
    g_home_network.system.system_frequency_hz = 50.0f;  // Default 50Hz
    g_home_network.system.net_direction = RBPOWER_CURRENT_DIRECTION_UNKNOWN;
    g_home_network.system.net_power_w = 0.0f;
    
    // Initialize source mappings
    for (uint8_t i = 0; i < RBPOWER_MAX_HOME_UNITS; i++) {
        g_home_network.source_mappings.mappings[i].unit_id = 0;
        g_home_network.source_mappings.mappings[i].is_active = false;
    }
    
    // === CREATE UNITS FROM PREDEFINED CONFIGURATION ===
    if (config->units_config.predefined_units && 
        config->units_config.predefined_units_count > 0) {
        
        ESP_LOGI("RBHOME", "Creating %d predefined units", 
                 config->units_config.predefined_units_count);
        
        for (uint8_t i = 0; i < config->units_config.predefined_units_count; i++) {
            const rbpower_unit_definition_t* predefined = &config->units_config.predefined_units[i];
            
            // Check limits
            if (g_home_network.units_count >= RBPOWER_MAX_HOME_UNITS) {
                ESP_LOGE("RBHOME", "Too many units! Max: %d", RBPOWER_MAX_HOME_UNITS);
                break;
            }
            
            // Validate predefined unit
            if (!validate_predefined_unit(predefined)) {
                ESP_LOGE("RBHOME", "Invalid predefined unit[%d]: %s", i, predefined->name);
                continue;
            }
            
            // Find free slot for unit
            uint8_t unit_index = g_home_network.units_count;
            
            // === COPY DEFINITION ===
            g_home_network.units[unit_index].definition = *predefined;
            g_home_network.units[unit_index].has_valid_definition = true;
            g_home_network.units[unit_index].hardware_present = true;  // default present; a detection
                                                                      // layer may clear it
            g_home_network.units[unit_index].has_realtime_data = false;
            g_home_network.units[unit_index].last_update_us = esp_timer_get_time();
            
            // Set creation timestamp
            g_home_network.units[unit_index].definition.created_timestamp_us = esp_timer_get_time();
            
            // === ADD UNIT_ID MAPPING ===
            if (!add_unit_id_mapping(predefined->unit_id, unit_index)) {
                ESP_LOGE("RBHOME", "Failed to add unit_id mapping for 0x%04X", predefined->unit_id);
                continue;
            }
            
            // === CREATE SOURCE MAPPING ===
            if (predefined->sources[0].source_type == RBPOWER_SOURCE_ADC_SENSOR) {
                create_adc_source_mapping(predefined->unit_id, 
                                        predefined->sources[0].source_id,
                                        predefined->sources[0].sensor_profile_id);
            }
            
            // === INITIALIZE REALTIME DATA ===
            initialize_unit_realtime_data(unit_index, predefined->unit_id);
            
            // === INITIALIZE STATISTICS ===
            initialize_unit_statistics(unit_index, predefined->unit_id, predefined->stat_class);
            
            g_home_network.units_count++;
            
            ESP_LOGI("RBHOME", "Created predefined unit[%d] 0x%04X: '%s' type=%d role=%d", 
                     unit_index, predefined->unit_id, predefined->name, 
                     predefined->type, predefined->role);
        }
        
        // === PROGRAMMATIC ASSIGNMENT OF SYSTEM IDs ===
        assign_system_units_by_type_and_role();
        
        // === INITIALIZE SPECIALIZED STRUCTURES ===
        initialize_specialized_structures(config);

        // === INITIALIZE LIFETIME ACCUMULATORS ===
        /* The lifetime module must be initialized here: without it every lifetime read returns
         * NOT_INIT (etstat 0 / lifetime (ERR)), restore never runs (src=seed forever) and the
         * RTC/NVS slots are never written, while the 15-min feeder keeps writing the unit structs
         * directly. All boot-time
         * units (I2C fleet, ESP-NOW nodes) are predefined by the config phase, so this point —
         * right after unit creation + specialized init — is where restore finds them all. */
        rbpower_err_t lifetime_err = rbpower_lifetime_init();
        if (lifetime_err != RBPOWER_OK) {
            ESP_LOGW("RBHOME", "Failed to initialize lifetime accumulators: %d", lifetime_err);
            // Don't stop initialization - lifetime accumulators are not critical
        } else {
            // === RESTORE DATA FROM RTC MEMORY ===
            rbpower_err_t restore_err = rbpower_lifetime_restore_from_rtc();
            if (restore_err == RBPOWER_OK) {
                ESP_LOGI("RBHOME", "Lifetime data restored from RTC Memory");
            } else if (restore_err == RBPOWER_ERR_NO_DATA) {
                ESP_LOGI("RBHOME", "No valid RTC data found, starting fresh");
            } else {
                ESP_LOGW("RBHOME", "Failed to restore lifetime data from RTC: %d", restore_err);
            }
        }

        ESP_LOGI("RBHOME", "Created %d units from predefined configuration", 
                 g_home_network.units_count);
    } else {
        ESP_LOGW("RBHOME", "No predefined units specified, creating empty network");
    }
    
    // Fill creation result
    g_creation_result.total_units_created = g_home_network.units_count;
    g_creation_result.adc_mappings_created = g_home_network.source_mappings.adc_mappings_count;
    g_creation_result.voltage_units_created = (g_home_network.system.voltage_bus_unit_id != 0) ? 1 : 0;
    g_creation_result.current_units_created = g_creation_result.total_units_created - g_creation_result.voltage_units_created;
    g_creation_result.has_voltage_bus = (g_home_network.system.voltage_bus_unit_id != 0);
    g_creation_result.has_main_supply = (g_home_network.system.main_supply_unit_id != 0);
    
    snprintf(g_creation_result.status_message, sizeof(g_creation_result.status_message),
            "Created %d units (%dV, %dI) with %d mappings",
            g_creation_result.total_units_created,
            g_creation_result.voltage_units_created,
            g_creation_result.current_units_created,
            g_creation_result.adc_mappings_created);
    
    // Set system ready flags
    g_home_network.is_synchronized = false;
    g_home_network.system_healthy = true;
    g_home_network.last_update_us = esp_timer_get_time();
    
    /* Realtime data arrives only through the inject API (rbAmp bridge / ESP-NOW hub): the HAL-driven
     * rbpower_home / rbpower_200ms tasks were removed together with the native ESP32-ADC engine. */
    g_home_network_initialized = true;
    
    // === INITIALIZE ACCUMULATION CONTROL ===
    memset(&g_accumulation_control, 0, sizeof(g_accumulation_control));
    
    ESP_LOGD("RBHOME", "Checking statistics availability...");
    ESP_LOGI("RBHOME", "rbpower_statistics_is_initialized() = %s", 
             rbpower_statistics_is_initialized() ? "TRUE" : "FALSE");
    
    // Enable accumulation for all created units (if statistics available)
    if (rbpower_statistics_is_initialized()) {
        ESP_LOGD("RBHOME", "Statistics available - enabling accumulation");
        for (uint8_t i = 0; i < g_home_network.units_count; i++) {
            if (g_home_network.units[i].has_valid_definition) {
                
                // ✅ FIXED: get correct period mask
                rbpower_stat_period_type_t stat_class = g_home_network.units[i].definition.stat_class;
                uint32_t periods_mask = get_periods_mask_by_stat_class(stat_class);
                
                // ✅ FIXED: enable only needed periods. Bound = RBPOWER_RAM_PERIOD_COUNT (3): only
                // 3SEC/15MIN/1HOUR have a RAM accumulator; day+ mask bits map to no slot (flash-recompute).
                for (uint8_t period = 0; period < RBPOWER_RAM_PERIOD_COUNT; period++) {
                    if (periods_mask & (1 << period)) {
                        g_period_accumulators[i][period].is_enabled = true;
                    }
                }
                
                g_accumulation_control.accumulation_enabled[i] = true;
                
                // ✅ FIXED: logging with correct periods
                char periods_str[128];
                get_enabled_periods_string(periods_mask, periods_str, sizeof(periods_str));
                
                ESP_LOGD("RBHOME", "Statistics enabled for unit %d (%s)", i, periods_str);
            }
        }
    } else {
        ESP_LOGI("RBHOME", "Statistics NOT available during HOME initialization");
        ESP_LOGI("RBHOME", "Accumulation will be enabled later in rbpower_start_accumulation_system()");
    }
    ESP_LOGI("RBHOME", "Home network initialized: %s", g_creation_result.status_message);

    g_home_network_initialized = true;
    return RBPOWER_OK;
}

//=============================================================================
// HELPER FUNCTIONS
//=============================================================================

/**
 * @brief Validate predefined unit
 */
static bool validate_predefined_unit(const rbpower_unit_definition_t* unit) {
    if (!unit->name[0]) {
        ESP_LOGE("RBHOME", "Unit name is empty");
        return false;
    }
    
    if (unit->sources_count == 0) {
        ESP_LOGE("RBHOME", "Unit has no sources");
        return false;
    }
    
    if (unit->unit_id == 0) {
        ESP_LOGE("RBHOME", "Unit has invalid ID (0)");
        return false;
    }
    
    // Check unit_id uniqueness
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].definition.unit_id == unit->unit_id) {
            ESP_LOGE("RBHOME", "Duplicate unit_id: 0x%04X", unit->unit_id);
            return false;
        }
    }
    
    return true;
}

/**
 * @brief Programmatic assignment of system units by type + role
 */
static void assign_system_units_by_type_and_role(void) {
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        rbpower_unit_definition_t* def = &g_home_network.units[i].definition;
        
        // Voltage Bus
        if (def->type == RBPOWER_METRIC_VOLTAGE_BUS && 
            def->role == RBPOWER_ROLE_MAIN_SUPPLY) {
            g_home_network.system.voltage_bus_unit_id = def->unit_id;
            ESP_LOGI("RBHOME", "Assigned Voltage Bus: unit_id=0x%04X", def->unit_id);
        }
        
        // Main Supply  
        if (def->type == RBPOWER_METRIC_CURRENT_LOAD && 
            def->role == RBPOWER_ROLE_MAIN_SUPPLY) {
            g_home_network.system.main_supply_unit_id = def->unit_id;
            ESP_LOGI("RBHOME", "Assigned Main Supply: unit_id=0x%04X", def->unit_id);
        }
        
        // Alternative Supply
        if (def->type == RBPOWER_METRIC_CURRENT_LOAD && 
            def->role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY) {
            g_home_network.system.alternative_supply_unit_id = def->unit_id;
            ESP_LOGI("RBHOME", "Assigned Alternative Supply: unit_id=0x%04X", def->unit_id);
        }
    }
}

/**
 * @brief Create ADC source mapping
 */
static void create_adc_source_mapping(rbpower_unit_id_t unit_id, uint8_t adc_channel, uint8_t sensor_profile_id) {
    if (g_home_network.source_mappings.active_mappings_count >= RBPOWER_MAX_HOME_UNITS) {
        ESP_LOGE("RBHOME", "Too many source mappings!");
        return;
    }
    
    uint8_t mapping_idx = g_home_network.source_mappings.active_mappings_count;
    
    g_home_network.source_mappings.mappings[mapping_idx].unit_id = unit_id;
    g_home_network.source_mappings.mappings[mapping_idx].source_type = RBPOWER_SOURCE_ADC_SENSOR;
    g_home_network.source_mappings.mappings[mapping_idx].is_active = true;
    g_home_network.source_mappings.mappings[mapping_idx].sensor_profile_id = sensor_profile_id;
    
    g_home_network.source_mappings.active_mappings_count++;
    g_home_network.source_mappings.adc_mappings_count++;
    
    ESP_LOGI("RBHOME", "Created ADC mapping: unit_id=0x%04X -> adc_ch=%d, profile=%d", 
             unit_id, adc_channel, sensor_profile_id);
}

/**
 * @brief Initialize unit realtime data
 */
static void initialize_unit_realtime_data(uint8_t unit_index, rbpower_unit_id_t unit_id) {
    // Initialize realtime data
    g_home_network.units[unit_index].realtime.unit_id = unit_id;
    g_home_network.units[unit_index].realtime.is_valid = false;
    
    // Initialize 200ms realtime data
    g_home_network.units[unit_index].realtime_average_200ms.unit_id = unit_id;
    g_home_network.units[unit_index].realtime_average_200ms.is_valid = false;
    
    ESP_LOGD("RBHOME", "Initialized realtime data for unit 0x%04X", unit_id);
}

/**
 * @brief Initialize unit statistics
 */
static void initialize_unit_statistics(uint8_t unit_index, rbpower_unit_id_t unit_id, rbpower_stat_period_type_t stat_class) {
    // Initialize statistics structure
    memset(&g_home_network.units[unit_index].statistics, 0, sizeof(rbpower_unit_statistics_t));
    g_home_network.units[unit_index].statistics.unit_id = unit_id;
    g_home_network.units[unit_index].statistics.statistics_enabled = true;
    
    // Initialize statistics update time
    for (uint8_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
        g_home_network.units[unit_index].statistics.last_period_update[period] = 0;
        g_home_network.units[unit_index].statistics.auto_update_enabled[period] = true;
    }
    
    ESP_LOGD("RBHOME", "Initialized statistics for unit 0x%04X with stat_class=%d", unit_id, stat_class);
}

/**
 * @brief Initialize specialized structures
 */
static void initialize_specialized_structures(const rbpower_extended_config_t* config) {
    // === INITIALIZE VOLTAGE BUS ===
    if (g_home_network.system.voltage_bus_unit_id != 0) {
        esp_err_t err = rbpower_voltage_bus_init(&g_home_network.specialized.voltage_bus,
                                                g_home_network.system.voltage_bus_unit_id,
                                                "Voltage Bus",
                                                config->base_hardware_config->network.nominal_voltage);
        
        if (err == ESP_OK) {
            g_home_network.specialized.has_voltage_bus = true;
            ESP_LOGI("RBHOME", "Initialized specialized Voltage Bus 0x%04X", 
                     g_home_network.system.voltage_bus_unit_id);
        } else {
            ESP_LOGE("RBHOME", "Failed to initialize Voltage Bus: %s", esp_err_to_name(err));
        }
    }
    
    // === INITIALIZE MAIN SUPPLY ===
    if (g_home_network.system.main_supply_unit_id != 0) {
        // Find unit definition to get parameters
        rbpower_unit_definition_t* main_def = find_unit_definition_by_id(g_home_network.system.main_supply_unit_id);
        if (main_def) {
            // Allocate memory for Main Supply
            rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)calloc(1, sizeof(rbpower_main_supply_t));
            if (main_supply) {
                esp_err_t err = rbpower_main_supply_init(main_supply,
                                                       main_def->unit_id,
                                                       main_def->name,
                                                       main_def->max_current_a,
                                                       main_def->rated_power_w);
                
                if (err == ESP_OK) {
                    g_home_network.specialized.main_supply = main_supply;
                    g_home_network.specialized.has_main_supply = true;
                    ESP_LOGI("RBHOME", "Initialized specialized Main Supply 0x%04X (%.0fA, %.0fW)", 
                             main_def->unit_id, main_def->max_current_a, main_def->rated_power_w);
                } else {
                    ESP_LOGE("RBHOME", "Failed to initialize Main Supply: %s", esp_err_to_name(err));
                    free(main_supply);
                }
            } else {
                ESP_LOGE("RBHOME", "Failed to allocate memory for Main Supply");
            }
        }
    }
    
    // === INITIALIZE ALTERNATIVE SUPPLY ===
    if (g_home_network.system.alternative_supply_unit_id != 0) {
        // Find unit definition to get parameters
        rbpower_unit_definition_t* alt_def = find_unit_definition_by_id(g_home_network.system.alternative_supply_unit_id);
        if (alt_def) {
            // Allocate memory for Alternative Supply
            rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)calloc(1, sizeof(rbpower_alternative_supply_t));
            if (alt_supply) {
                // Determine alternative source type (currently default - solar)
                rbpower_alt_source_type_t alt_type = RBPOWER_ALT_SOURCE_SOLAR_INVERTER;
                float rated_power_kw = alt_def->rated_power_w / 1000.0f; // Convert to kW
                
                esp_err_t err = rbpower_alt_supply_init(alt_supply,
                                                      alt_def->unit_id,
                                                      alt_def->name,
                                                      alt_type,
                                                      rated_power_kw);
                
                if (err == ESP_OK) {
                    g_home_network.specialized.alternative_supply = alt_supply;
                    g_home_network.specialized.has_alternative_supply = true;
                    ESP_LOGI("RBHOME", "Initialized specialized Alternative Supply 0x%04X (%.1fkW, type=%d)", 
                             alt_def->unit_id, rated_power_kw, alt_type);
                } else {
                    ESP_LOGE("RBHOME", "Failed to initialize Alternative Supply: %s", esp_err_to_name(err));
                    free(alt_supply);
                }
            } else {
                ESP_LOGE("RBHOME", "Failed to allocate memory for Alternative Supply");
            }
        }
    }
}

/**
 * @brief Find unit definition by ID
 */
static rbpower_unit_definition_t* find_unit_definition_by_id(rbpower_unit_id_t unit_id) {
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition && 
            g_home_network.units[i].definition.unit_id == unit_id) {
            return &g_home_network.units[i].definition;
        }
    }
    return NULL;
}

rbpower_err_t rbpower_get_network(rbpower_electrical_network_t** network) {
    if (!network) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    *network = &g_home_network;
    return RBPOWER_OK;
}

rbpower_err_t rbpower_get_unit_by_id(rbpower_unit_id_t unit_id, rbpower_home_unit_t** unit) {
    if (!unit) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Find index by unit_id
    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || !g_home_network.units[unit_index].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    *unit = &g_home_network.units[unit_index];
    return RBPOWER_OK;
}

rbpower_err_t rbpower_get_unit_by_name(const char* name, rbpower_home_unit_t** unit) {
    if (!name || !unit) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition &&
            strcmp(g_home_network.units[i].definition.name, name) == 0) {
            *unit = &g_home_network.units[i];
            return RBPOWER_OK;
        }
    }
    
    return RBPOWER_ERR_INVALID_ARG;
}

rbpower_unit_id_t rbpower_find_unit_by_adc_channel(uint8_t adc_channel) {
    if (!g_home_network_initialized) {
        return RBPOWER_HOME_INVALID_ID;
    }
    
    for (uint8_t i = 0; i < RBPOWER_MAX_HOME_UNITS; i++) {
        const rbpower_source_mapping_t* mapping = &g_home_network.source_mappings.mappings[i];
        
        if (mapping->is_active && 
            mapping->source_type == RBPOWER_SOURCE_ADC_SENSOR &&
            mapping->source_data.adc_source.adc_channel == adc_channel) {
            return mapping->unit_id;
        }
    }
    
    return RBPOWER_HOME_INVALID_ID;
}

bool rbpower_home_is_initialized(void) {
    return g_home_network_initialized;
}

rbpower_err_t rbpower_get_creation_result(rbpower_home_creation_result_t* result) {
    if (!result) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    *result = g_creation_result;
    return RBPOWER_OK;
}

void rbpower_home_print_network_structure(void) {
    if (!g_home_network_initialized) {
        ESP_LOGI("RBHOME", " === Home network not initialized ===");
        return;
    }
    
    ESP_LOGI("RBHOME", "=== HOME NETWORK STRUCTURE ===");
    ESP_LOGI("RBHOME", "Total units: %d", g_home_network.units_count);
    ESP_LOGI("RBHOME", "ADC mappings: %d", g_home_network.source_mappings.adc_mappings_count);
    ESP_LOGI("RBHOME", "Voltage Bus unit: 0x%04X", g_home_network.system.voltage_bus_unit_id);
    ESP_LOGI("RBHOME", "Main Supply unit: 0x%04X", g_home_network.system.main_supply_unit_id);

    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition) {
            const rbpower_unit_definition_t* def = &g_home_network.units[i].definition;
            ESP_LOGI("RBHOME", "Unit[%d] ID=0x%04X: '%s' (%s) - %s", 
                     i, def->unit_id, def->name, def->location,
                     def->is_active ? "ACTIVE" : "INACTIVE");
        }
    }

    ESP_LOGI("RBHOME", "=== END STRUCTURE ===");
    ESP_LOGD("RBHOME", "=== 200MS STATUS ===");
    
    bool has_200ms_data = false;
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition && 
            g_home_network.units[i].realtime_average_200ms.is_valid) {
            
            const rbpower_unit_realtime_average_200ms_t* data = &g_home_network.units[i].realtime_average_200ms;
            ESP_LOGD("RBHOME", "Unit[%d]: 200ms data VALID (samples=%lu, age=%lu us)",
                     (int)i, (unsigned long)data->average_200ms.samples_count, 
                     (unsigned long)(esp_timer_get_time() - data->last_update_us));
            has_200ms_data = true;
        }
    }
    
    if (!has_200ms_data) {
        ESP_LOGD("RBHOME", "NO VALID 200MS DATA");
    }    
}

//=============================================================================
// 3-SEC PERIOD STATISTICS ACCUMULATION MANAGEMENT. BASE PERIOD
//=============================================================================

/**
 * @brief Initialize accumulation for new 3-second period
 * Called from statistics_timer_callback at period start
 */
static void init_3sec_accumulation_period(uint8_t unit_id, uint64_t period_start_time_us, uint64_t period_end_time_us)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return;
    }
    
    // Check that statistics is enabled for this unit
    if (!rbpower_statistics_is_initialized() || !g_stats_system.system_initialized || 
        !g_stats_system.config.statistics_enabled) {
        ESP_LOGD("RBHOME", "Unit[%d]: Statistics system not ready", unit_id);
        g_accumulation_control.accumulation_enabled[unit_id] = true;
        return;
    }
    
    //ESP_LOGI("RBHOME-3SEK-ACC", " accumulation_period INIT for unit %d accumulation_enabled=%d, has_valid_definition=%d, definition.is_active=%d",
    //             unit_id, g_accumulation_control.accumulation_enabled[unit_id], g_home_network.units[unit_id].has_valid_definition,
    //             g_home_network.units[unit_id].definition.is_active);

    // Check that unit exists and is active
    if (!g_home_network.units[unit_id].has_valid_definition || 
        !g_home_network.units[unit_id].definition.is_active) {
        //ESP_LOGI("RBHOME-3SEK-ACC", "INIT Unit[%d]: inactive or undefined", unit_id);
        g_accumulation_control.accumulation_enabled[unit_id] = false;
        return;
    }

   // === FORCE ENABLE ACCUMULATION ===
    g_accumulation_control.accumulation_enabled[unit_id] = true;
    
    // Reset control flags
    g_accumulation_control.ms200_done[unit_id] = false;
    g_accumulation_control.remaining_200ms_calls[unit_id] = 0;
    g_accumulation_control.expected_200ms_calls[unit_id] = 0;
    g_accumulation_control.period_start_time_us[unit_id] = period_start_time_us;
    
    // Reset 3-sec period accumulator for this unit
    if (g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC].is_enabled) {
        reset_period_accumulator(&g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC]);
        g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC].time_boundaries.period_start_us = period_start_time_us;
        g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC].time_boundaries.period_end_us = period_end_time_us;
        g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC].is_active = true; // ← IMPORTANT!
        
        //ESP_LOGI("RBHOME-3SEK-ACC", "INIT Unit[%d]: 3-sec period started (%llu - %llu us)", unit_id, period_start_time_us, period_end_time_us);
    } else {
        ESP_LOGD("RBHOME-3SEC-ACC", "INIT Unit[%d]: 3-sec accumulator not enabled", unit_id);
        g_accumulation_control.accumulation_enabled[unit_id] = false;
    }
}

/**
 * @brief Finalize 3-second period accumulation
 * Called from statistics_timer_callback at period end
 */
static void finalize_3sec_accumulation_period(uint8_t unit_id, uint64_t period_end_time_us)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || !g_accumulation_control.accumulation_enabled[unit_id]) {
        return;
    }
    
    rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC];
    
    if (!acc->is_enabled || !acc->is_active) {
        return;
    }
    
    // Force finalize accumulation
    g_accumulation_control.ms200_done[unit_id] = false;
    g_accumulation_control.remaining_200ms_calls[unit_id] = 0;
    g_accumulation_control.expected_200ms_calls[unit_id] = 0;
    //g_accumulation_control.accumulation_enabled[unit_id] = false;
    
    // Finalize time boundaries
    acc->time_boundaries.period_end_us = period_end_time_us;
    acc->time_boundaries.actual_duration_ms = (period_end_time_us - acc->time_boundaries.period_start_us) / 1000;
    acc->metadata.period_complete = true;
    acc->metadata.last_update_us = period_end_time_us;
    
    // Calculate final values (RMS, average powers)
    rbpower_period_statistics_t* stats_3sec = &g_home_network.units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_3SEC];
    calculate_final_statistics(unit_id, acc, stats_3sec);
    
    // Update statistics last update time
    g_home_network.units[unit_id].statistics.last_period_update[RBPOWER_STAT_PERIOD_3SEC] = period_end_time_us;
    
    // === NEW: ACCUMULATE INTO LIFETIME ACCUMULATORS ===
    rbpower_unit_id_t real_unit_id = g_home_network.units[unit_id].definition.unit_id;
/*
    rbpower_err_t lifetime_err = rbpower_lifetime_process_3sec_period(real_unit_id, stats_3sec);
    if (lifetime_err != RBPOWER_OK) {
        ESP_LOGW("RBHOME-3SEC", "Failed to accumulate lifetime data for unit %d: %d", real_unit_id, lifetime_err);
    }
*/    
    // Mark period as completed
    acc->is_active = false;
}

/**
 * @brief Add a unit's current data to its 3-second period accumulator.
 * @param unit_id Unit index
 * @param use_200ms_data true - use the 200 ms data, false - use the half-cycle data
 */
static void accumulate_unit_data(uint8_t unit_id, bool use_200ms_data)
{

    if (unit_id >= RBPOWER_MAX_HOME_UNITS || 
        !g_accumulation_control.accumulation_enabled[unit_id] ||
        !g_home_network.units[unit_id].has_valid_definition) {
        return;
    }
    // ==== VOLTAGE BUS ====
    // Check if this is voltage bus
    if (g_home_network.units[unit_id].definition.unit_id == g_home_network.system.voltage_bus_unit_id) {
        // This is voltage bus - accumulate into specialized structure
        rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
        
/*
        // Debug: output only first call and every 50th
        static uint32_t vbus_acc_count = 0;
        vbus_acc_count++;
        if (vbus_acc_count == 1 || vbus_acc_count % 50 == 0) {
            ESP_LOGI("VBUS_ACC", "Accumulating voltage bus: count=%lu, mode=%s, samples=%lu", 
                     vbus_acc_count, use_200ms_data ? "200ms" : "halfcycle", 
                     vbus->statistics_3sec.rms_samples_count);
        }
*/        
        if (use_200ms_data) {
            // From 200ms data
            const rbpower_unit_realtime_average_200ms_t* data_200ms = 
                &g_home_network.units[unit_id].realtime_average_200ms;
            if (!data_200ms->is_valid) return;
            
            float voltage_rms = data_200ms->average_200ms.voltage_rms_avg;
            vbus->statistics_3sec.voltage_rms_sum_squares += (double)(voltage_rms * voltage_rms);
            vbus->statistics_3sec.rms_samples_count++;
            
            if (voltage_rms < vbus->statistics_3sec.voltage_min_3sec) {
                vbus->statistics_3sec.voltage_min_3sec = voltage_rms;
                vbus->statistics_3sec.voltage_min_time_us = esp_timer_get_time();
            }
            if (voltage_rms > vbus->statistics_3sec.voltage_max_3sec) {
                vbus->statistics_3sec.voltage_max_3sec = voltage_rms;
                vbus->statistics_3sec.voltage_max_time_us = esp_timer_get_time();
            }
            
            vbus->statistics_3sec.frequency_sum += data_200ms->zc_analysis.zc_frequency_measured_hz;
            vbus->statistics_3sec.frequency_samples_count++;
            
            float thd = data_200ms->harmonics.thd_voltage_percent;
            vbus->statistics_3sec.thd_voltage_sum_squares += (double)(thd * thd);
            vbus->statistics_3sec.thd_samples_count++;
        } else {
            // From halfcycle data
            const rbpower_unit_realtime_halfcycle_t* data_halfcycle = 
                &g_home_network.units[unit_id].realtime;
            if (!data_halfcycle->is_valid) return;
            
            float voltage_rms = data_halfcycle->halfcycle.voltage_rms;
            vbus->statistics_3sec.voltage_rms_sum_squares += (double)(voltage_rms * voltage_rms);
            vbus->statistics_3sec.rms_samples_count++;
            
            if (voltage_rms < vbus->statistics_3sec.voltage_min_3sec) {
                vbus->statistics_3sec.voltage_min_3sec = voltage_rms;
                vbus->statistics_3sec.voltage_min_time_us = esp_timer_get_time();
            }
            if (voltage_rms > vbus->statistics_3sec.voltage_max_3sec) {
                vbus->statistics_3sec.voltage_max_3sec = voltage_rms;
                vbus->statistics_3sec.voltage_max_time_us = esp_timer_get_time();
            }
            
            vbus->statistics_3sec.frequency_sum += data_halfcycle->zc_analysis.zc_frequency_measured_hz;
            vbus->statistics_3sec.frequency_samples_count++;
            
            float thd = data_halfcycle->harmonics.thd_voltage_percent;
            vbus->statistics_3sec.thd_voltage_sum_squares += (double)(thd * thd);
            vbus->statistics_3sec.thd_samples_count++;
        }
        return; // Exit after voltage bus processing
    }
    
    // ==== MAIN SUPPLY ====
    // Check if this is main supply
    if (g_home_network.units[unit_id].definition.unit_id == g_home_network.system.main_supply_unit_id) {
        // This is main supply - accumulate into specialized structure
        rbpower_main_supply_t* msupply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
        
        // Extended debug
        static bool first_time = true;
        if (first_time) {
            ESP_LOGI("MSUPPLY_ACC", "Main supply check: has_main_supply=%d, msupply_ptr=%p", 
                    g_home_network.specialized.has_main_supply, msupply);
            if (msupply) {
                ESP_LOGI("MSUPPLY_ACC", "Main supply state: is_active=%d, unit_id=0x%04X, name='%s'",
                        msupply->is_active, msupply->unit_id, msupply->name);
            }
            first_time = false;
        }
        
        if (!msupply || !msupply->is_active) {
            static uint32_t error_count = 0;
            if (error_count++ % 100 == 0) {
                ESP_LOGW("MSUPPLY_ACC", "Main supply not ready: msupply=%p, active=%d", 
                        msupply, msupply ? msupply->is_active : -1);
            }
            return;
        }
        
        if (use_200ms_data) {
            // From 200ms data
            const rbpower_unit_realtime_average_200ms_t* data_200ms = 
                &g_home_network.units[unit_id].realtime_average_200ms;
            if (!data_200ms->is_valid) return;
            
            // === ELECTRICAL ACCUMULATORS ===
            // RMS current (RMS aggregation)
            float current = data_200ms->average_200ms.current_rms_avg;
            msupply->statistics_3sec.electrical.current_rms_sum_squares += (double)(current * current);
            msupply->statistics_3sec.electrical.rms_samples_count++;
            
            // Powers (linear aggregation)
            msupply->statistics_3sec.electrical.power_active_sum += data_200ms->average_200ms.power_active_avg_w;
            msupply->statistics_3sec.electrical.power_reactive_sum += data_200ms->average_200ms.power_reactive_avg_var;
            msupply->statistics_3sec.electrical.power_apparent_sum += data_200ms->average_200ms.power_apparent_avg_va;
            msupply->statistics_3sec.electrical.power_factor_sum += data_200ms->average_200ms.power_factor_avg;

            // Frequency
            if (data_200ms->zc_analysis.zc_frequency_measured_hz > 0) {
                msupply->statistics_3sec.electrical.frequency_sum += data_200ms->zc_analysis.zc_frequency_measured_hz;
                msupply->statistics_3sec.electrical.frequency_samples_count++;
            }

            // realtime_200ms
            msupply->realtime_200ms.current_rms = current;
            msupply->realtime_200ms.power_active_w = data_200ms->average_200ms.power_active_avg_w;
            msupply->realtime_200ms.power_reactive_var = data_200ms->average_200ms.power_reactive_avg_var;
            msupply->realtime_200ms.power_apparent_va = data_200ms->average_200ms.power_apparent_avg_va;
            msupply->realtime_200ms.power_factor = data_200ms->average_200ms.power_factor_avg;
            msupply->realtime_200ms.samples_count = data_200ms->average_200ms.samples_count;
            msupply->realtime_200ms.direction = data_200ms->zc_analysis.current_direction;
            msupply->realtime_200ms.energy_increment_wh = data_200ms->energy.energy_increment_200ms_wh;

            // === ENERGY ACCUMULATORS ===
            if (data_200ms->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_CONSUMING) {
                msupply->statistics_3sec.energy.consumed_wh_accumulator += data_200ms->energy.energy_increment_200ms_wh;
                msupply->statistics_3sec.energy.consuming_samples++;
                msupply->statistics_3sec.energy.consuming_time_us += 200000; // 200ms in microseconds
            } else if (data_200ms->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING) {
                msupply->statistics_3sec.energy.supplied_wh_accumulator += fabsf(data_200ms->energy.energy_increment_200ms_wh);
                msupply->statistics_3sec.energy.supplying_samples++;
                msupply->statistics_3sec.energy.supplying_time_us += 200000; // 200ms in microseconds
            } else {
                msupply->statistics_3sec.energy.zero_samples++;
            }
            
            // Net energy accumulator
            msupply->statistics_3sec.energy.net_wh_accumulator += data_200ms->energy.net_energy_balance_wh;
            
            // === EXTREMES ACCUMULATORS ===
            // Current
            if (current < msupply->statistics_3sec.extremes.current_min) {
                msupply->statistics_3sec.extremes.current_min = current;
            }
            if (current > msupply->statistics_3sec.extremes.current_max) {
                msupply->statistics_3sec.extremes.current_max = current;
            }
            
            // Power (using instant_power_net_w)
            float power_net = data_200ms->instant_power_net_w;
            if (power_net < msupply->statistics_3sec.extremes.power_min) {
                msupply->statistics_3sec.extremes.power_min = power_net;
                msupply->statistics_3sec.extremes.power_min_time_us = esp_timer_get_time();
            }
            if (power_net > msupply->statistics_3sec.extremes.power_max) {
                msupply->statistics_3sec.extremes.power_max = power_net;
                msupply->statistics_3sec.extremes.power_max_time_us = esp_timer_get_time();
            }
            
            // Power peak time (maximum absolute power)
            if (fabsf(power_net) > fabsf(msupply->statistics_3sec.extremes.power_max)) {
                msupply->statistics_3sec.extremes.power_peak_time_us = esp_timer_get_time();
            }
            
            // === POWER QUALITY ACCUMULATORS ===
            if (data_200ms->harmonics.thd_current_percent >= 0) {
                float thd_current = data_200ms->harmonics.thd_current_percent;
                msupply->statistics_3sec.power_quality.thd_current_sum_squares += (double)(thd_current * thd_current);
                msupply->statistics_3sec.power_quality.thd_samples_count++;
                
                // THD maximum
                if (thd_current > msupply->statistics_3sec.power_quality.thd_current_max) {
                    msupply->statistics_3sec.power_quality.thd_current_max = thd_current;
                }
            }
            
            // Quality alerts (when error flags present)
            if (data_200ms->error_flags & (RBPOWER_ERROR_THD_HIGH | RBPOWER_ERROR_VOLTAGE_OUT_OF_RANGE | 
                                        RBPOWER_ERROR_CURRENT_OUT_OF_RANGE | RBPOWER_ERROR_FREQUENCY_DRIFT)) {
                msupply->statistics_3sec.power_quality.quality_alerts_count++;
            }
            
            // === DIRECTION STATS ACCUMULATORS ===
            // Direction changes
            msupply->statistics_3sec.direction_stats.direction_changes_count += data_200ms->zc_analysis.direction_changes_count;
            
            // Count direction transitions (requires tracking previous state)
            // msupply->statistics_3sec.direction_stats.consuming_to_supplying_count += ...; // TODO: need transition tracking logic
            // msupply->statistics_3sec.direction_stats.supplying_to_consuming_count += ...; // TODO: need transition tracking logic
            // msupply->statistics_3sec.direction_stats.rapid_transitions_count += ...; // TODO: need rapid transition detection logic
            
            // ZC statistics
            msupply->statistics_3sec.direction_stats.total_zc_events += data_200ms->zc_analysis.zc_transitions_count;
            msupply->statistics_3sec.direction_stats.missed_zc_events += data_200ms->zc_analysis.zc_missed_count;
            
            if (data_200ms->zc_analysis.zc_stability_percent >= 0) {
                msupply->statistics_3sec.direction_stats.zc_stability_sum += data_200ms->zc_analysis.zc_stability_percent;
                msupply->statistics_3sec.direction_stats.zc_stability_samples++;
            }
            
            // Phase statistics
            if (data_200ms->zc_analysis.phase_balance_factor >= -1.0f && data_200ms->zc_analysis.phase_balance_factor <= 1.0f) {
                float phase_balance = data_200ms->zc_analysis.phase_balance_factor;
                msupply->statistics_3sec.direction_stats.phase_balance_sum += phase_balance;
                msupply->statistics_3sec.direction_stats.phase_balance_sum_squares += (double)(phase_balance * phase_balance);
                msupply->statistics_3sec.direction_stats.phase_samples_count++;
            }
            
            // Frequency deviations (deviation from nominal 50Hz)
            if (data_200ms->zc_analysis.zc_frequency_measured_hz > 0) {
                float freq_deviation = data_200ms->zc_analysis.zc_frequency_measured_hz - 50.0f;
                msupply->statistics_3sec.direction_stats.frequency_deviation_sum_squares += (double)(freq_deviation * freq_deviation);
                msupply->statistics_3sec.direction_stats.frequency_deviation_samples++;
            }
            
            msupply->statistics_3sec.economics.samples_count++;
            
            // === METADATA ACCUMULATORS ===
            msupply->statistics_3sec.metadata.samples_count++;
            if (data_200ms->is_valid) {
                msupply->statistics_3sec.metadata.valid_samples_count++;
            } else {
                msupply->statistics_3sec.metadata.error_samples_count++;
            }
            
            // Timestamps
            uint64_t current_time = esp_timer_get_time();
            if (msupply->statistics_3sec.metadata.first_sample_time_us == 0) {
                msupply->statistics_3sec.metadata.first_sample_time_us = current_time;
            }
            msupply->statistics_3sec.metadata.last_sample_time_us = current_time;
            msupply->statistics_3sec.metadata.last_update_us = current_time;
            msupply->statistics_3sec.metadata.last_zc_timestamp_us = data_200ms->timestamp_ZC_event;
            
            // Counters and flags
            msupply->statistics_3sec.metadata.data_valid = data_200ms->is_valid;
            msupply->statistics_3sec.metadata.update_counter++;
            
        } else {
            // From halfcycle data
            const rbpower_unit_realtime_halfcycle_t* data_halfcycle = 
                &g_home_network.units[unit_id].realtime;
            if (!data_halfcycle->is_valid) return;
            
            // === ELECTRICAL ACCUMULATORS ===
            // RMS current (RMS aggregation)
            float current = data_halfcycle->halfcycle.current_rms;
            msupply->statistics_3sec.electrical.current_rms_sum_squares += (double)(current * current);
            msupply->statistics_3sec.electrical.rms_samples_count++;
            
            // Powers (linear aggregation)
            msupply->statistics_3sec.electrical.power_active_sum += data_halfcycle->halfcycle.power_active_w;
            msupply->statistics_3sec.electrical.power_reactive_sum += data_halfcycle->halfcycle.power_reactive_var;
            
            // Calculate apparent power and power factor
            float apparent = sqrtf(powf(data_halfcycle->halfcycle.power_active_w, 2) + 
                                powf(data_halfcycle->halfcycle.power_reactive_var, 2));
            msupply->statistics_3sec.electrical.power_apparent_sum += apparent;
            if (apparent > 0) {
                msupply->statistics_3sec.electrical.power_factor_sum += 
                    data_halfcycle->halfcycle.power_active_w / apparent;
            }
            
            // Frequency
            if (data_halfcycle->zc_analysis.zc_frequency_measured_hz > 0) {
                msupply->statistics_3sec.electrical.frequency_sum += data_halfcycle->zc_analysis.zc_frequency_measured_hz;
                msupply->statistics_3sec.electrical.frequency_samples_count++;
            }
            
            // === ENERGY ACCUMULATORS ===
            if (data_halfcycle->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_CONSUMING) {
                msupply->statistics_3sec.energy.consumed_wh_accumulator += data_halfcycle->energy.energy_increment_halfcycle_wh;
                msupply->statistics_3sec.energy.consuming_samples++;
                // msupply->statistics_3sec.energy.consuming_time_us += ...; // TODO: need halfcycle time in microseconds
            } else if (data_halfcycle->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING) {
                msupply->statistics_3sec.energy.supplied_wh_accumulator += fabsf(data_halfcycle->energy.energy_increment_halfcycle_wh);
                msupply->statistics_3sec.energy.supplying_samples++;
                // msupply->statistics_3sec.energy.supplying_time_us += ...; // TODO: need halfcycle time in microseconds
            } else {
                msupply->statistics_3sec.energy.zero_samples++;
            }
            
            // Net energy accumulator
            msupply->statistics_3sec.energy.net_wh_accumulator += data_halfcycle->energy.net_energy_balance_wh;
            
            // === EXTREMES ACCUMULATORS ===
            // Current
            if (current < msupply->statistics_3sec.extremes.current_min) {
                msupply->statistics_3sec.extremes.current_min = current;
            }
            if (current > msupply->statistics_3sec.extremes.current_max) {
                msupply->statistics_3sec.extremes.current_max = current;
            }
            
            // Power (using instant_power_net_w)
            float power_net = data_halfcycle->instant_power_net_w;
            if (power_net < msupply->statistics_3sec.extremes.power_min) {
                msupply->statistics_3sec.extremes.power_min = power_net;
                msupply->statistics_3sec.extremes.power_min_time_us = esp_timer_get_time();
            }
            if (power_net > msupply->statistics_3sec.extremes.power_max) {
                msupply->statistics_3sec.extremes.power_max = power_net;
                msupply->statistics_3sec.extremes.power_max_time_us = esp_timer_get_time();
            }
            
            // Power peak time (maximum absolute power)
            if (fabsf(power_net) > fabsf(msupply->statistics_3sec.extremes.power_max)) {
                msupply->statistics_3sec.extremes.power_peak_time_us = esp_timer_get_time();
            }
            
            // === POWER QUALITY ACCUMULATORS ===
            if (data_halfcycle->harmonics.thd_current_percent >= 0) {
                float thd_current = data_halfcycle->harmonics.thd_current_percent;
                msupply->statistics_3sec.power_quality.thd_current_sum_squares += (double)(thd_current * thd_current);
                msupply->statistics_3sec.power_quality.thd_samples_count++;
                
                // THD maximum
                if (thd_current > msupply->statistics_3sec.power_quality.thd_current_max) {
                    msupply->statistics_3sec.power_quality.thd_current_max = thd_current;
                }
            }
            
            // Quality alerts (when error flags present)
            if (data_halfcycle->error_flags & (RBPOWER_ERROR_THD_HIGH | RBPOWER_ERROR_VOLTAGE_OUT_OF_RANGE | 
                                            RBPOWER_ERROR_CURRENT_OUT_OF_RANGE | RBPOWER_ERROR_FREQUENCY_DRIFT)) {
                msupply->statistics_3sec.power_quality.quality_alerts_count++;
            }
            
            // === DIRECTION STATS ACCUMULATORS ===
            // Direction changes
            msupply->statistics_3sec.direction_stats.direction_changes_count += data_halfcycle->zc_analysis.direction_changes_count;
            
            // Count direction transitions (requires tracking previous state)
            // msupply->statistics_3sec.direction_stats.consuming_to_supplying_count += ...; // TODO: need transition tracking logic
            // msupply->statistics_3sec.direction_stats.supplying_to_consuming_count += ...; // TODO: need transition tracking logic
            // msupply->statistics_3sec.direction_stats.rapid_transitions_count += ...; // TODO: need rapid transition detection logic
            
            // ZC statistics
            msupply->statistics_3sec.direction_stats.total_zc_events += data_halfcycle->zc_analysis.zc_transitions_count;
            msupply->statistics_3sec.direction_stats.missed_zc_events += data_halfcycle->zc_analysis.zc_missed_count;
            
            if (data_halfcycle->zc_analysis.zc_stability_percent >= 0) {
                msupply->statistics_3sec.direction_stats.zc_stability_sum += data_halfcycle->zc_analysis.zc_stability_percent;
                msupply->statistics_3sec.direction_stats.zc_stability_samples++;
            }
            
            // Phase statistics
            if (data_halfcycle->zc_analysis.phase_balance_factor >= -1.0f && data_halfcycle->zc_analysis.phase_balance_factor <= 1.0f) {
                float phase_balance = data_halfcycle->zc_analysis.phase_balance_factor;
                msupply->statistics_3sec.direction_stats.phase_balance_sum += phase_balance;
                msupply->statistics_3sec.direction_stats.phase_balance_sum_squares += (double)(phase_balance * phase_balance);
                msupply->statistics_3sec.direction_stats.phase_samples_count++;
            }
            
            // Frequency deviations (deviation from nominal 50Hz)
            if (data_halfcycle->zc_analysis.zc_frequency_measured_hz > 0) {
                float freq_deviation = data_halfcycle->zc_analysis.zc_frequency_measured_hz - 50.0f;
                msupply->statistics_3sec.direction_stats.frequency_deviation_sum_squares += (double)(freq_deviation * freq_deviation);
                msupply->statistics_3sec.direction_stats.frequency_deviation_samples++;
            }
            
            msupply->statistics_3sec.economics.samples_count++;
            
            // === METADATA ACCUMULATORS ===
            msupply->statistics_3sec.metadata.samples_count++;
            if (data_halfcycle->is_valid) {
                msupply->statistics_3sec.metadata.valid_samples_count++;
            } else {
                msupply->statistics_3sec.metadata.error_samples_count++;
            }
            
            // Timestamps
            uint64_t current_time = esp_timer_get_time();
            if (msupply->statistics_3sec.metadata.first_sample_time_us == 0) {
                msupply->statistics_3sec.metadata.first_sample_time_us = current_time;
            }
            msupply->statistics_3sec.metadata.last_sample_time_us = current_time;
            msupply->statistics_3sec.metadata.last_update_us = current_time;
            msupply->statistics_3sec.metadata.last_zc_timestamp_us = data_halfcycle->timestamp_ZC_event;
            
            // Counters and flags
            msupply->statistics_3sec.metadata.data_valid = data_halfcycle->is_valid;
            msupply->statistics_3sec.metadata.update_counter++;
        }
        
        return; // Exit after main supply processing
    }
    
    // Check if this is alternative supply
    if (g_home_network.units[unit_id].definition.unit_id == g_home_network.system.alternative_supply_unit_id) {
        // This is alternative supply - accumulate into specialized structure
        rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
        
        // Extended debug
        static bool first_time_alt = true;
        if (first_time_alt) {
            ESP_LOGI("ALT_SUPPLY_ACC", "Alternative supply check: has_alternative_supply=%d, alt_supply_ptr=%p", 
                     g_home_network.specialized.has_alternative_supply, alt_supply);
            if (alt_supply) {
                ESP_LOGI("ALT_SUPPLY_ACC", "Alternative supply state: is_active=%d, unit_id=0x%04X, name='%s', type=%d",
                         alt_supply->is_active, alt_supply->unit_id, alt_supply->name, alt_supply->type);
            }
            first_time_alt = false;
        }
        
        if (!alt_supply || !alt_supply->is_active) {
            static uint32_t error_count = 0;
            if (error_count++ % 100 == 0) {
                ESP_LOGW("ALT_SUPPLY_ACC", "Alternative supply not ready: alt_supply=%p, active=%d", 
                         alt_supply, alt_supply ? alt_supply->is_active : -1);
            }
            return;
        }
       
        if (use_200ms_data) {
            // From 200ms data
            const rbpower_unit_realtime_average_200ms_t* data_200ms = 
                &g_home_network.units[unit_id].realtime_average_200ms;
            if (!data_200ms->is_valid) return;
            
            // Electrical accumulators
            float current = data_200ms->average_200ms.current_rms_avg;
            alt_supply->statistics_3sec.current_rms_sum_squares += (double)(current * current);
            alt_supply->statistics_3sec.power_active_sum += data_200ms->average_200ms.power_active_avg_w;
            alt_supply->statistics_3sec.power_reactive_sum += data_200ms->average_200ms.power_reactive_avg_var;
            alt_supply->statistics_3sec.power_factor_sum += data_200ms->average_200ms.power_factor_avg;
            alt_supply->statistics_3sec.electrical_samples_count++;
            
            // TODO: add DC parameters processing when available in data_200ms
            // realtime_200ms
            alt_supply->realtime_200ms.current_rms = current;
            alt_supply->realtime_200ms.power_active_w = data_200ms->average_200ms.power_active_avg_w;
            alt_supply->realtime_200ms.power_reactive_var = data_200ms->average_200ms.power_reactive_avg_var;
            alt_supply->realtime_200ms.power_apparent_va = data_200ms->average_200ms.power_apparent_avg_va;
            alt_supply->realtime_200ms.power_factor = data_200ms->average_200ms.power_factor_avg;
            alt_supply->realtime_200ms.samples_count = data_200ms->average_200ms.samples_count;
            alt_supply->realtime_200ms.direction = data_200ms->zc_analysis.current_direction;
            alt_supply->realtime_200ms.energy_increment_wh = data_200ms->energy.energy_increment_200ms_wh;

            
            // Energy - for alternative sources negative power = generation
            if (data_200ms->average_200ms.power_active_avg_w < -10.0f) {
                // Generation (negative power)
                alt_supply->statistics_3sec.generated_wh_accumulator += 
                    fabsf(data_200ms->energy.energy_increment_200ms_wh);
                alt_supply->statistics_3sec.generating_samples++;
            } else if (data_200ms->average_200ms.power_active_avg_w > 10.0f) {
                // Consumption (positive power - e.g., standby)
                alt_supply->statistics_3sec.consumed_wh_accumulator += 
                    data_200ms->energy.energy_increment_200ms_wh;
                alt_supply->statistics_3sec.consuming_samples++;
            } else {
                alt_supply->statistics_3sec.idle_samples++;
            }
            
            // Efficiency (approximate estimate based on power and rating)
            if (data_200ms->average_200ms.power_active_avg_w < -50.0f) {
                float efficiency = fminf(100.0f, 
                    fabsf(data_200ms->average_200ms.power_active_avg_w) / 
                    (alt_supply->config.rated_power_kw * 10.0f)); // percent of 1% rating
                alt_supply->statistics_3sec.efficiency_sum += efficiency;
                alt_supply->statistics_3sec.dc_samples_count++;
            }
            
            // Extremes
            float power = fabsf(data_200ms->average_200ms.power_active_avg_w);
            if (data_200ms->average_200ms.power_active_avg_w < -10.0f && 
                power > alt_supply->statistics_3sec.power_max_generated) {
                alt_supply->statistics_3sec.power_max_generated = power;
                alt_supply->statistics_3sec.peak_generation_time_us = esp_timer_get_time();
            }
            if (data_200ms->average_200ms.power_active_avg_w > 10.0f && 
                power > alt_supply->statistics_3sec.power_max_consumed) {
                alt_supply->statistics_3sec.power_max_consumed = power;
            }
            
            // Operating modes
            if (power > 50.0f) {
                alt_supply->statistics_3sec.mode_samples[RBPOWER_ALT_MODE_GENERATING]++;
            } else if (power > 5.0f) {
                alt_supply->statistics_3sec.mode_samples[RBPOWER_ALT_MODE_STANDBY]++;
            } else {
                alt_supply->statistics_3sec.mode_samples[RBPOWER_ALT_MODE_OFF]++;
            }
            
            // Economic accumulators
            float tariff_rate = rbpower_get_current_tariff_rate_simple();
            rbpower_tariff_zone_type_t current_zone;
            if (rbpower_tariff_get_current_zone(&current_zone, NULL, NULL) == RBPOWER_OK) {
                if (current_zone < RBPOWER_ALT_SUPPLY_TARIFF_ZONES) {
                    alt_supply->statistics_3sec.tariff_samples[current_zone]++;
                }
            }
            
            // Revenue from generation
            if (alt_supply->statistics_3sec.generated_wh_accumulator > 0) {
                float feed_in_rate = alt_supply->config.feed_in_tariff > 0 ? 
                                    alt_supply->config.feed_in_tariff : tariff_rate;
                alt_supply->statistics_3sec.generation_revenue_accumulator += 
                    (fabsf(data_200ms->energy.energy_increment_200ms_wh) / 1000.0f) * feed_in_rate;
            }
            
            // Consumption costs
            if (alt_supply->statistics_3sec.consumed_wh_accumulator > 0) {
                alt_supply->statistics_3sec.consumption_cost_accumulator += 
                    (data_200ms->energy.energy_increment_200ms_wh / 1000.0f) * tariff_rate;
            }
            
            // Self-consumption value
            if (data_200ms->average_200ms.power_active_avg_w < -10.0f) {
                float self_consumption_ratio = alt_supply->config.self_consumption_priority / 100.0f;
                alt_supply->statistics_3sec.self_consumption_value_accumulator += 
                    (fabsf(data_200ms->energy.energy_increment_200ms_wh) / 1000.0f) * 
                    tariff_rate * self_consumption_ratio;
            }

        } else {
            // From halfcycle data
            const rbpower_unit_realtime_halfcycle_t* data_halfcycle = 
                &g_home_network.units[unit_id].realtime;
            if (!data_halfcycle->is_valid) return;
            
            // Electrical accumulators
            float current = data_halfcycle->halfcycle.current_rms;
            alt_supply->statistics_3sec.current_rms_sum_squares += (double)(current * current);
            alt_supply->statistics_3sec.power_active_sum += data_halfcycle->halfcycle.power_active_w;
            alt_supply->statistics_3sec.power_reactive_sum += data_halfcycle->halfcycle.power_reactive_var;
            
            // Power factor
            float apparent = sqrtf(powf(data_halfcycle->halfcycle.power_active_w, 2) + 
                                 powf(data_halfcycle->halfcycle.power_reactive_var, 2));
            if (apparent > 0) {
                alt_supply->statistics_3sec.power_factor_sum += 
                    data_halfcycle->halfcycle.power_active_w / apparent;
            }
            alt_supply->statistics_3sec.electrical_samples_count++;
            
            // Energy - for alternative sources negative power = generation
            if (data_halfcycle->halfcycle.power_active_w < -10.0f) {
                // Generation
                alt_supply->statistics_3sec.generated_wh_accumulator += 
                    fabsf(data_halfcycle->energy.energy_increment_halfcycle_wh);
                alt_supply->statistics_3sec.generating_samples++;
            } else if (data_halfcycle->halfcycle.power_active_w > 10.0f) {
                // Consumption
                alt_supply->statistics_3sec.consumed_wh_accumulator += 
                    data_halfcycle->energy.energy_increment_halfcycle_wh;
                alt_supply->statistics_3sec.consuming_samples++;
            } else {
                alt_supply->statistics_3sec.idle_samples++;
            }
            
            // Efficiency (approximate estimate)
            if (data_halfcycle->halfcycle.power_active_w < -50.0f) {
                float efficiency = fminf(100.0f, 
                    fabsf(data_halfcycle->halfcycle.power_active_w) / 
                    (alt_supply->config.rated_power_kw * 10.0f));
                alt_supply->statistics_3sec.efficiency_sum += efficiency;
                alt_supply->statistics_3sec.dc_samples_count++;
            }
            
            // Extremes
            float power = fabsf(data_halfcycle->halfcycle.power_active_w);
            if (data_halfcycle->halfcycle.power_active_w < -10.0f && 
                power > alt_supply->statistics_3sec.power_max_generated) {
                alt_supply->statistics_3sec.power_max_generated = power;
                alt_supply->statistics_3sec.peak_generation_time_us = esp_timer_get_time();
            }
            if (data_halfcycle->halfcycle.power_active_w > 10.0f && 
                power > alt_supply->statistics_3sec.power_max_consumed) {
                alt_supply->statistics_3sec.power_max_consumed = power;
            }
            
            // Operating modes
            if (power > 50.0f) {
                alt_supply->statistics_3sec.mode_samples[RBPOWER_ALT_MODE_GENERATING]++;
            } else if (power > 5.0f) {
                alt_supply->statistics_3sec.mode_samples[RBPOWER_ALT_MODE_STANDBY]++;
            } else {
                alt_supply->statistics_3sec.mode_samples[RBPOWER_ALT_MODE_OFF]++;
            }
            
            // Economic and tariff accumulators (similar to 200ms)
            float tariff_rate = rbpower_get_current_tariff_rate_simple();
            rbpower_tariff_zone_type_t current_zone;
            if (rbpower_tariff_get_current_zone(&current_zone, NULL, NULL) == RBPOWER_OK) {
                if (current_zone < RBPOWER_ALT_SUPPLY_TARIFF_ZONES) {
                    alt_supply->statistics_3sec.tariff_samples[current_zone]++;
                }
            }
            
            // Revenue from generation
            if (data_halfcycle->halfcycle.power_active_w < -10.0f) {
                float feed_in_rate = alt_supply->config.feed_in_tariff > 0 ? 
                                    alt_supply->config.feed_in_tariff : tariff_rate;
                alt_supply->statistics_3sec.generation_revenue_accumulator += 
                    (fabsf(data_halfcycle->energy.energy_increment_halfcycle_wh) / 1000.0f) * feed_in_rate;
            }
            
            // Consumption costs
            if (data_halfcycle->halfcycle.power_active_w > 10.0f) {
                alt_supply->statistics_3sec.consumption_cost_accumulator += 
                    (data_halfcycle->energy.energy_increment_halfcycle_wh / 1000.0f) * tariff_rate;
            }
            
            // Self-consumption value
            if (data_halfcycle->halfcycle.power_active_w < -10.0f) {
                float self_consumption_ratio = alt_supply->config.self_consumption_priority / 100.0f;
                alt_supply->statistics_3sec.self_consumption_value_accumulator += 
                    (fabsf(data_halfcycle->energy.energy_increment_halfcycle_wh) / 1000.0f) * 
                    tariff_rate * self_consumption_ratio;
            }
        }

        // Track state changes
        static rbpower_alt_supply_state_t last_state = RBPOWER_ALT_STATE_OFFLINE;
        rbpower_alt_supply_state_t current_state = 
            (alt_supply->statistics_3sec.power_max_generated > 50.0f) ? RBPOWER_ALT_STATE_RUNNING :
            (alt_supply->statistics_3sec.power_max_consumed > 5.0f) ? RBPOWER_ALT_STATE_READY : 
            RBPOWER_ALT_STATE_OFFLINE;
            
        if (current_state != last_state) {
            alt_supply->statistics_3sec.state_transitions++;
            last_state = current_state;
        }
        
        return; // Exit after alternative supply processing
    }
    
    // Standard processing for other units
    rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_3SEC];
    
    if (!acc->is_enabled || !acc->is_active) {
        return;
    }
    
    if (use_200ms_data) {
        // Accumulation from 200ms data
        const rbpower_unit_realtime_average_200ms_t* data_200ms = 
            &g_home_network.units[unit_id].realtime_average_200ms;
            
        if (!data_200ms->is_valid) {
            return;
        }
        
        // RMS accumulators (RMS aggregation)
        float voltage_rms = data_200ms->average_200ms.voltage_rms_avg;
        float current_rms = data_200ms->average_200ms.current_rms_avg;
        
        if (voltage_rms > 0) {
            acc->electrical.voltage_rms_sum_squares += voltage_rms * voltage_rms;
        }
        if (current_rms >= 0) {
            acc->electrical.current_rms_sum_squares += current_rms * current_rms;
        }
        acc->electrical.rms_samples_count++;
        
        // Powers (linear aggregation)
        acc->electrical.power_active_sum += data_200ms->average_200ms.power_active_avg_w;
        acc->electrical.power_reactive_sum += data_200ms->average_200ms.power_reactive_avg_var;
        acc->electrical.power_apparent_sum += data_200ms->average_200ms.power_apparent_avg_va;
        acc->electrical.power_factor_sum += data_200ms->average_200ms.power_factor_avg;
        acc->electrical.power_samples_count++;
        
        // Energy (direct summation)
// Energy (direct summation considering direction)
        if (data_200ms->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_CONSUMING) {
            acc->energy.consumed_wh_accumulator += data_200ms->energy.energy_increment_200ms_wh;
            acc->energy.consuming_samples++;
        } else if (data_200ms->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING) {
            acc->energy.supplied_wh_accumulator += fabsf(data_200ms->energy.energy_increment_200ms_wh);
            acc->energy.supplying_samples++;
        } else {
            acc->energy.zero_samples++;
        }


        // Frequency
        if (data_200ms->zc_analysis.zc_frequency_measured_hz > 0) {
            acc->electrical.frequency_sum += data_200ms->zc_analysis.zc_frequency_measured_hz;
            acc->electrical.frequency_samples_count++;
        }
        
    } else {
        // Accumulation from halfcycle data
        const rbpower_unit_realtime_halfcycle_t* data_halfcycle = 
            &g_home_network.units[unit_id].realtime;
            
        if (!data_halfcycle->is_valid) {
            return;
        }
        
        // RMS accumulators (RMS aggregation)
        float voltage_rms = data_halfcycle->halfcycle.voltage_rms;
        float current_rms = data_halfcycle->halfcycle.current_rms;
        
        if (voltage_rms > 0) {
            acc->electrical.voltage_rms_sum_squares += voltage_rms * voltage_rms;
        }
        if (current_rms >= 0) {
            acc->electrical.current_rms_sum_squares += current_rms * current_rms;
        }
        acc->electrical.rms_samples_count++;
        
        // Powers (linear aggregation)
        acc->electrical.power_active_sum += data_halfcycle->halfcycle.power_active_w;
        acc->electrical.power_reactive_sum += data_halfcycle->halfcycle.power_reactive_var;
        acc->electrical.power_apparent_sum += sqrt(pow(data_halfcycle->halfcycle.power_active_w, 2) + 
                                                   pow(data_halfcycle->halfcycle.power_reactive_var, 2));
        
        if (data_halfcycle->halfcycle.power_active_w != 0) {
            float apparent_power = sqrt(pow(data_halfcycle->halfcycle.power_active_w, 2) + 
                                       pow(data_halfcycle->halfcycle.power_reactive_var, 2));
            if (apparent_power > 0) {
                acc->electrical.power_factor_sum += data_halfcycle->halfcycle.power_active_w / apparent_power;
            }
        }
        acc->electrical.power_samples_count++;
        
        // Energy (direct summation considering direction)
        if (data_halfcycle->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_CONSUMING) {
            acc->energy.consumed_wh_accumulator += data_halfcycle->energy.energy_increment_halfcycle_wh;
            acc->energy.consuming_samples++;
        } else if (data_halfcycle->zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING) {
            acc->energy.supplied_wh_accumulator += fabsf(data_halfcycle->energy.energy_increment_halfcycle_wh);
            acc->energy.supplying_samples++;
        } else {
            acc->energy.zero_samples++;
        }
        
        // Frequency
        if (data_halfcycle->zc_analysis.zc_frequency_measured_hz > 0) {
            acc->electrical.frequency_sum += data_halfcycle->zc_analysis.zc_frequency_measured_hz;
            acc->electrical.frequency_samples_count++;
        }
    }
    
    // === UPDATE EXTREMES ===
    uint64_t timestamp = esp_timer_get_time();
    float voltage, current, power;
    
    if (use_200ms_data) {
        const rbpower_unit_realtime_average_200ms_t* data_200ms = 
            &g_home_network.units[unit_id].realtime_average_200ms;
        voltage = data_200ms->average_200ms.voltage_rms_avg;
        current = data_200ms->average_200ms.current_rms_avg;
        power = data_200ms->average_200ms.power_active_avg_w;
    } else {
        const rbpower_unit_realtime_halfcycle_t* data_halfcycle = 
            &g_home_network.units[unit_id].realtime;
        voltage = data_halfcycle->halfcycle.voltage_rms;
        current = data_halfcycle->halfcycle.current_rms;
        power = data_halfcycle->halfcycle.power_active_w;
    }
    
    // Initialize extremes on first update
    if (!acc->extremes.extremes_initialized) {
        acc->extremes.voltage_min = acc->extremes.voltage_max = voltage;
        acc->extremes.current_min = acc->extremes.current_max = current;
        acc->extremes.power_min = acc->extremes.power_max = power;
        acc->extremes.power_peak_time_us = timestamp;
        acc->extremes.extremes_initialized = true;
    } else {
        // Update extremes
        if (voltage < acc->extremes.voltage_min) {
            acc->extremes.voltage_min = voltage;
            acc->extremes.voltage_min_time_us = timestamp;
        }
        if (voltage > acc->extremes.voltage_max) {
            acc->extremes.voltage_max = voltage;
            acc->extremes.voltage_max_time_us = timestamp;
        }
        
        if (current < acc->extremes.current_min) {
            acc->extremes.current_min = current;
        }
        if (current > acc->extremes.current_max) {
            acc->extremes.current_max = current;
        }
        
        if (power < acc->extremes.power_min) {
            acc->extremes.power_min = power;
            acc->extremes.power_min_time_us = timestamp;
        }
        if (power > acc->extremes.power_max) {
            acc->extremes.power_max = power;
            acc->extremes.power_max_time_us = timestamp;
        }
        
        // Update peak time (maximum absolute power)
        if (fabsf(power) > fabsf(acc->extremes.power_max)) {
            acc->extremes.power_peak_time_us = timestamp;
        }
    }
    
    // Update valid samples counter
    acc->metadata.valid_samples_count++;

    // === UPDATE METADATA ===
    acc->metadata.samples_count++;
    acc->metadata.last_sample_time_us = esp_timer_get_time();
    acc->metadata.last_update_us = esp_timer_get_time();
}

/**
 * @brief Compute the final statistics from an accumulator.
 * @param unit_id Unit index
 * @param acc 3-second period accumulator
 * @param stats Output statistics structure
 */
static void calculate_final_statistics(uint8_t unit_id, 
                                      const rbpower_period_accumulator_t* acc,
                                      rbpower_period_statistics_t* stats)
{
    if (!acc || !stats || !acc->is_enabled) {
        return;
    }
    
    memset(stats, 0, sizeof(rbpower_period_statistics_t));
    
    // === TIME BOUNDARIES ===
    stats->period_start_us = acc->time_boundaries.period_start_us;
    stats->period_end_us = acc->time_boundaries.period_end_us;
    stats->period_duration_ms = acc->time_boundaries.actual_duration_ms;
    
    // === ELECTRICAL VALUES ===
    // RMS values (RMS aggregation)
    if (acc->electrical.rms_samples_count > 0) {
        stats->voltage_rms = sqrtf(acc->electrical.voltage_rms_sum_squares / acc->electrical.rms_samples_count);
        stats->current_rms = sqrtf(acc->electrical.current_rms_sum_squares / acc->electrical.rms_samples_count);
        ESP_LOGD("RBHOME-3SEK", " STAT RMS for unit %d ('%s'): U=%.1fV I=%.2fA",
                 unit_id, g_home_network.units[unit_id].definition.name,
                 stats->voltage_rms, stats->current_rms);
    }
        //ESP_LOGI("RBHOME-3SEK", " STAT count = %lu", acc->electrical.rms_samples_count);
    
    // Average powers (linear aggregation)
    if (acc->electrical.power_samples_count > 0) {
        stats->power_active_avg = acc->electrical.power_active_sum / acc->electrical.power_samples_count;
        stats->power_reactive_avg = acc->electrical.power_reactive_sum / acc->electrical.power_samples_count;
        stats->power_apparent_avg = acc->electrical.power_apparent_sum / acc->electrical.power_samples_count;
        stats->power_factor_avg = acc->electrical.power_factor_sum / acc->electrical.power_samples_count;
    }
    
    // Frequency
    if (acc->electrical.frequency_samples_count > 0) {
        stats->frequency_avg = acc->electrical.frequency_sum / acc->electrical.frequency_samples_count;
    }
    
    // === ENERGY FOR PERIOD ===
    stats->energy_period.consumed_wh = (float)acc->energy.consumed_wh_accumulator;
    stats->energy_period.supplied_wh = (float)acc->energy.supplied_wh_accumulator;
    stats->energy_period.net_wh = stats->energy_period.consumed_wh - stats->energy_period.supplied_wh;

    // === ECONOMICS ===
    stats->result_base.energy_consumed_wh = (float)acc->energy.consumed_wh_accumulator;
    stats->result_base.energy_supplied_wh = (float)acc->energy.supplied_wh_accumulator;
 
    // === EXTREMES ===
    if (acc->extremes.extremes_initialized) {
        stats->extremes.voltage_min = acc->extremes.voltage_min;
        stats->extremes.voltage_max = acc->extremes.voltage_max;
        stats->extremes.current_min = acc->extremes.current_min;
        stats->extremes.current_max = acc->extremes.current_max;
        stats->extremes.power_min = acc->extremes.power_min;
        stats->extremes.power_max = acc->extremes.power_max;
        stats->extremes.power_peak_time_us = acc->extremes.power_peak_time_us;
    }
    
    // === METADATA ===
    stats->samples_count = acc->metadata.samples_count;
    stats->valid_samples_count = acc->metadata.valid_samples_count;
    stats->data_completeness_percent = (acc->metadata.samples_count > 0) ? 
        (acc->metadata.valid_samples_count * 100 / acc->metadata.samples_count) : 0;
    stats->period_complete = acc->metadata.period_complete;
    stats->last_update_us = acc->metadata.last_update_us;
    stats->timestamp_ZC_event = acc->metadata.last_zc_timestamp_us;
}

/*
 * @brief Callback for 3-second statistics period start
 * Called from statistics_timer_callback
 */
void rbpower_home_start_3sec_period_callback(uint64_t period_start_time_us, uint64_t period_end_time_us)
{
    if (!g_home_network_initialized) {
        return;
    }

    rbpower_home_lock();   // write transaction (esp_timer 3-sec task: start-of-period init)

    // Update tariff zone cache at the start of each 3-second period
    // This ensures tariff accuracy for all calculations in the period
    esp_err_t tariff_err = rbpower_tariff_update_zone_cache();
    if (tariff_err != ESP_OK) {
        ESP_LOGD("RBHOME", "Failed to update tariff zone cache: %d", tariff_err);
    }
    
    // Initialize for voltage bus if exists
    if (g_home_network.specialized.has_voltage_bus) {
        extern void voltage_bus_start_3sec_period(rbpower_voltage_bus_t* voltage_bus, uint64_t period_start_us);
        voltage_bus_start_3sec_period(&g_home_network.specialized.voltage_bus, period_start_time_us);
    }
    
    // Initialize for main supply if exists
    if (g_home_network.specialized.has_main_supply) {
        main_supply_start_3sec_period((rbpower_main_supply_t*)g_home_network.specialized.main_supply, period_start_time_us);
    }
    
    // Initialize for alternative supply if exists
    if (g_home_network.specialized.has_alternative_supply) {
        alt_supply_start_3sec_period((rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply, period_start_time_us);
    }
    
    // Initialize accumulation for all active units
    for (uint8_t unit_id = 0; unit_id < g_home_network.units_count; unit_id++) {
        //ESP_LOGI("RBHOME-3SEK-START", " start_3sec_period_callback for unit %d accumulation_enabled=%d, has_valid_definition=%d, definition.is_active=%d",
                 //unit_id, g_accumulation_control.accumulation_enabled[unit_id], g_home_network.units[unit_id].has_valid_definition,
                 //g_home_network.units[unit_id].definition.is_active);
        if (g_home_network.units[unit_id].has_valid_definition && 
            g_home_network.units[unit_id].definition.is_active) {
            init_3sec_accumulation_period(unit_id, period_start_time_us, period_end_time_us);
        }
    }

    rbpower_home_unlock();
}

/* Experimental 4-flow scaffold — gather a live system snapshot from unit realtime powers, classified by role
 * (the `sysflows` verb → rbpower_compute_energy_balance). Without PV/battery units p_pv/p_batt stay 0
 * (no alt instantiated) → degenerate grid-only. Read-only over g_home_network; no side effects. */
void rbpower_gather_system_balance_input(rbpower_energy_balance_input_t* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (!g_home_network.units[i].has_valid_definition) continue;
        float p = g_home_network.units[i].realtime_average_200ms.average_200ms.power_active_avg_w;
        switch (g_home_network.units[i].definition.role) {
            case RBPOWER_ROLE_MAIN_SUPPLY:        out->p_grid += p; break;
            case RBPOWER_ROLE_ALTERNATIVE_SUPPLY:
            case RBPOWER_ROLE_RENEWABLE_SOURCE:
                /* Point convention: P > 0 = power flowing INTO the point (consumption), so a generator
                 * that produces reads P < 0. p_pv is generation (>= 0) — flip; a PV point drawing
                 * standby power at night (P > 0) is not generation (clamped to 0 by the balance). */
                out->p_pv   += -p; break;
            case RBPOWER_ROLE_STORAGE_SYSTEM:     out->p_batt += p; break;
            case RBPOWER_ROLE_ZONE_LOAD:
            case RBPOWER_ROLE_DEVICE_LOAD:
            case RBPOWER_ROLE_EV_CHARGING:
            case RBPOWER_ROLE_HEATING_COOLING:
            case RBPOWER_ROLE_SMART_APPLIANCE:    out->p_load += p; break;
            default: break;
        }
    }
}

/*
 * @brief Callback for 3-second statistics period end
 * Called from statistics_timer_callback
 */
void rbpower_home_end_3sec_period_callback(uint64_t period_end_time_us)
{
    if (!g_home_network_initialized) {
        return;
    }

    rbpower_home_lock();   // write transaction (esp_timer 3-sec task: finalize + 15-min aggregate)

    rbpower_home_housekeeping_tick();         /* heartbeat / day rollover / bucket baseline / NVS flush */

    // Finalize for voltage bus if exists
    if (g_home_network.specialized.has_voltage_bus) {
        extern void voltage_bus_finalize_3sec_period(rbpower_voltage_bus_t* voltage_bus, uint64_t period_end_us);
        voltage_bus_finalize_3sec_period(&g_home_network.specialized.voltage_bus, period_end_time_us);
    }
    
    // Finalize for main supply if exists. Step5 sub-B: the 3sec→15min aggregation was removed —
    // it rolled into a god-struct accumulator nothing consumed (the generic per-unit pipeline
    // produces this unit's real period records). Only the 3sec finalize (telemetry feed) remains.
    if (g_home_network.specialized.has_main_supply) {
        rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
        main_supply_finalize_3sec_period(main_supply, period_end_time_us);
    }
    
    // Finalize for alternative supply if exists
    if (g_home_network.specialized.has_alternative_supply) {
        rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
        alt_supply_finalize_3sec_period(alt_supply, period_end_time_us);
        
        // Aggregate from 3-sec to 15-min for alternative supply
        if (alt_supply->statistics_3sec.period_complete) {
            aggregate_alt_supply_15min_from_3sec(alt_supply);
        }
    }
    
    // Finalize accumulation for all active units
    for (uint8_t unit_id = 0; unit_id < g_home_network.units_count; unit_id++) {
        //ESP_LOGI("RBHOME-3SEK-END", " end_3sec_period_callback for unit %d accumulation_enabled=%d, has_valid_definition=%d", unit_id, g_accumulation_control.accumulation_enabled[unit_id], g_home_network.units[unit_id].has_valid_definition);
        if (g_home_network.units[unit_id].has_valid_definition && 
            g_accumulation_control.accumulation_enabled[unit_id]) {
            finalize_3sec_accumulation_period(unit_id, period_end_time_us);
                // Integrate tariff with 3-sec period

               // === NEW: AGGREGATE TO 15-MIN ===
            if (should_aggregate_to_period(unit_id, RBPOWER_STAT_PERIOD_15MIN)) {
                const rbpower_period_statistics_t* completed_3sec = &g_home_network.units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_3SEC];

                /* Aggregate ONLY a 3sec result that (a) is FRESH — finalize above just stamped
                 * period_end_us with exactly this boundary's timestamp (an early-returned finalize
                 * leaves a STALE result here, and re-adding it would double-count the last live
                 * period), and (b) actually CONTAINS samples. An EXTERNAL-source unit (rbAmp/STD
                 * inject) has no native 200ms accumulation: its 3sec result is permanently empty,
                 * yet it was aggregated every 3 s → the 15MIN acc filled with 300 zero children,
                 * finalize_15min produced a ZERO native quarter per boundary and rolled it into
                 * the HOUR acc. Consequences on hardware: HOUR averages
                 * diluted by the zero children (V = 222/√2 = 156, avgP = E/2h), and — worse — the
                 * zero native quarter's rollup dispatched the hour boundary BEFORE the inject
                 * quarter for that same boundary arrived, so the XX:00 quarter slid into the NEXT
                 * hour record (HOUR energy short by exactly one quarter, dt = 4500 s). With empty
                 * quarters skipped, the 15MIN acc of an external-source unit never activates, and
                 * the hour boundary is dispatched by the INJECT rollup — which aggregates the
                 * boundary quarter BEFORE dispatching, putting it in the correct hour. */
                /* 200-ms live injects now fill the 3sec result of an external-source unit too (samples > 0, energy 0),
                 * so the zero native quarter would close the hour at hh:00:00 before the injected boundary
                 * quarter arrives (e.g. 10:45–11:00 moved into the 11:00 hour). Skip such units explicitly. */
                if (completed_3sec->samples_count > 0 &&
                    completed_3sec->period_end_us == period_end_time_us &&
                    !rbpower_home_unit_is_period_injected(unit_id)) {
                    aggregate_15min_from_3sec_period(unit_id, completed_3sec);
                }
            }
        }
    }

    // Release the write lock BEFORE telemetry: the builder (rbgrid_json_create_telemetry)
    // re-locks for a consistent read-snapshot, and the actual WS/MQTT send then runs
    // lock-free (a publish must never hold the g_home_network lock across the network).
    rbpower_home_unlock();

       // === NEW: TELEMETRY AFTER CYCLE ===
    // Send summary telemetry data in one packet for all units
    rbpower_telemetry_on_3sec_period_end(period_end_time_us);

}

/**
 * @brief Apply a half-cycle measurement buffer to the home network (used by the inject API;
 *        originally the native HAL buffer path).
 */
static esp_err_t rbpower_update_home_network_from_buffer(
    const rbpower_unit_realtime_halfcycle_t *buffer,
    uint8_t units_count,
    uint32_t sequence_number)
{
    if (!g_home_network_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    rbpower_home_lock();   // write transaction (core-0 update task / inject path)
    uint64_t update_timestamp = esp_timer_get_time();

    // Update units data
    for (uint8_t i = 0; i < units_count; i++) {
        const rbpower_unit_realtime_halfcycle_t *src = &buffer[i];
        // Buffer convention: the entry carries the unit INDEX (not unit_id) in its low byte
        uint8_t unit_index = (uint8_t)(src->unit_id & 0xFF);  // Take lower byte as index
        
        
        if (unit_index < RBPOWER_MAX_HOME_UNITS && 
            g_home_network.units[unit_index].has_valid_definition) {
            
            // Copy real-time data
            memcpy(&g_home_network.units[unit_index].realtime, src, 
                   sizeof(rbpower_unit_realtime_halfcycle_t));
            
            // Restore correct unit_id
            g_home_network.units[unit_index].realtime.unit_id = g_home_network.units[unit_index].definition.unit_id;
            
            g_home_network.units[unit_index].has_realtime_data = true;
            g_home_network.units[unit_index].last_update_us = update_timestamp;
            g_home_network.units[unit_index].total_updates++;
            // === ACCUMULATE INTO STATISTICS ===
            // Accumulate data into 3-sec accumulator (if statistics enabled)
            if (g_accumulation_control.accumulation_enabled[unit_index] && 
                !g_accumulation_control.ms200_done[unit_index]) {
                // Accumulation from halfcycle data
                accumulate_unit_data(unit_index, false);
            }            
        }
    }
    
    // Update system aggregated data
    //rbpower_update_system_aggregated_values();
    
    g_home_network.last_update_us = update_timestamp;
    g_home_network.is_synchronized = true;

    rbpower_home_unlock();
    return ESP_OK;
}

/*
 * @brief Get the 200 ms averaged data of a unit by ID
 * @param unit_id ID unit
 * @param result 200 ms data result
 * @return RBPOWER_OK on success
 */
rbpower_err_t rbpower_get_unit_200ms_data(rbpower_unit_id_t unit_id, rbpower_unit_realtime_average_200ms_t* result) {
    if (!result) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Find index by unit_id
    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || !g_home_network.units[unit_index].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Copy 200ms data
    memcpy(result, &g_home_network.units[unit_index].realtime_average_200ms, 
           sizeof(rbpower_unit_realtime_average_200ms_t));
    
    return RBPOWER_OK;
}

/*
 * @brief Get aggregated 200ms data of all units
 * @param voltage_unit 200ms voltage unit data (can be NULL)
 * @param current_units Array of 200ms current units data (can be NULL)
 * @param current_units_count Number of current units
 * @return RBPOWER_OK on success
 */
rbpower_err_t rbpower_get_aggregated_200ms_data(rbpower_unit_realtime_average_200ms_t* voltage_unit,
                                               rbpower_unit_realtime_average_200ms_t* current_units,
                                               uint8_t* current_units_count) {
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    uint8_t found_current_units = 0;
    
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (!g_home_network.units[i].has_valid_definition) {
            continue;
        }
        
        const rbpower_unit_definition_t* def = &g_home_network.units[i].definition;
        
        // Voltage unit
        if (def->type == RBPOWER_METRIC_VOLTAGE_BUS && voltage_unit != NULL) {
            memcpy(voltage_unit, &g_home_network.units[i].realtime_average_200ms,
                   sizeof(rbpower_unit_realtime_average_200ms_t));
        }
        
        // Current units
        if (def->type == RBPOWER_METRIC_CURRENT_LOAD && current_units != NULL && 
            found_current_units < RBPOWER_MAX_HOME_UNITS) {
            memcpy(&current_units[found_current_units], &g_home_network.units[i].realtime_average_200ms,
                   sizeof(rbpower_unit_realtime_average_200ms_t));
            found_current_units++;
        }
    }
    
    if (current_units_count) {
        *current_units_count = found_current_units;
    }
    
    return RBPOWER_OK;
}

/*
 * @brief Print 200ms data diagnostics for debugging
 */
void rbpower_print_200ms_diagnostics(void) {
    if (!g_home_network_initialized) {
        ESP_LOGD("RBHOME", "200ms diagnostic: Home network not initialized");
        return;
    }
    
    ESP_LOGI("RBHOME", "=== 200MS DIAGNOSTIC ===");
    
    bool found_data = false;
    
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (!g_home_network.units[i].has_valid_definition) {
            continue;
        }
        
        const rbpower_unit_definition_t* def = &g_home_network.units[i].definition;
        const rbpower_unit_realtime_average_200ms_t* data = &g_home_network.units[i].realtime_average_200ms;
        
        if (!data->is_valid) {
            ESP_LOGD("RBHOME", "Unit[%u] '%s': NO VALID 200ms DATA", i, def->name);
            continue;
        }
        
        found_data = true;
        
        if (def->type == RBPOWER_METRIC_VOLTAGE_BUS) {
            ESP_LOGI("RBHOME", "Unit[%u] VOLTAGE '%s':", i, def->name);
            ESP_LOGI("RBHOME", "  U_avg=%.1fV (min=%.1f, max=%.1f)", 
                     data->average_200ms.voltage_rms_avg,
                     data->extremes.window_200ms.voltage_min,
                     data->extremes.window_200ms.voltage_max);
            ESP_LOGI("RBHOME", "  Frequency=%.1fHz, Quality=%u%%, Samples=%d",
                     data->zc_analysis.zc_frequency_measured_hz,
                     data->data_quality_percent,
                     (int)data->average_200ms.samples_count);
        } else if (def->type == RBPOWER_METRIC_CURRENT_LOAD) {
            const char* direction_str = "UNKNOWN";
            switch(data->zc_analysis.current_direction) {
                case RBPOWER_CURRENT_DIRECTION_CONSUMING: direction_str = "CONSUMING"; break;
                case RBPOWER_CURRENT_DIRECTION_SUPPLYING: direction_str = "SUPPLYING"; break;
                case RBPOWER_CURRENT_DIRECTION_ZERO: direction_str = "ZERO"; break;
                case RBPOWER_CURRENT_DIRECTION_BIDIRECTIONAL: direction_str = "BIDIRECT"; break;
                default: break;
            }
            
            ESP_LOGI("RBHOME", "Unit[%u] CURRENT '%s':", i, def->name);
            ESP_LOGI("RBHOME", "  I_avg=%.2fA, P_avg=%.0fW, PF=%.3f",
                     data->average_200ms.current_rms_avg,
                     data->average_200ms.power_active_avg_w,
                     data->average_200ms.power_factor_avg);
            ESP_LOGI("RBHOME", "  Direction=%s, Energy=%.3fWh, Samples=%d",
                     direction_str,
                     data->energy.energy_increment_200ms_wh,
                     (int)data->average_200ms.samples_count);
        }
        
        ESP_LOGI("RBHOME", "  Last update: %llu us ago",
                 (esp_timer_get_time() - data->last_update_us));
    }
    
    if (!found_data) {
        ESP_LOGI("RBHOME", "NO VALID 200ms DATA");
    }
    
    ESP_LOGI("RBHOME", "=== END 200MS DIAGNOSTIC ===");
}

/*
 * @brief Get 3-second unit statistics by ID
 */
rbpower_err_t rbpower_get_unit_3sec_statistics(rbpower_unit_id_t unit_id, rbpower_period_statistics_t* stats)
{
    if (!stats) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Find index by unit_id
    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || !g_home_network.units[unit_index].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Copy 3-sec statistics
    memcpy(stats, &g_home_network.units[unit_index].statistics.periods[RBPOWER_STAT_PERIOD_3SEC], 
           sizeof(rbpower_period_statistics_t));
    
    return RBPOWER_OK;
}

/*
 * @brief Print statistics accumulation diagnostics
 */
void rbpower_print_accumulation_diagnostics(void)
{
    if (!g_home_network_initialized) {
        ESP_LOGD("RBHOME", "Statistics accumulation: Home network not initialized");
        return;
    }
    
    ESP_LOGI("RBHOME", "=== STATISTICS ACCUMULATION DIAGNOSTIC ===");
    ESP_LOGI("RBHOME", "Statistics initialized: %s", rbpower_statistics_is_initialized() ? "YES" : "NO");
    
    bool found_accumulation = false;
    
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (!g_home_network.units[i].has_valid_definition) {
            continue;
        }
        
        const rbpower_unit_definition_t* def = &g_home_network.units[i].definition;
        bool accumulation_enabled = g_accumulation_control.accumulation_enabled[i];
        bool ms200_done = g_accumulation_control.ms200_done[i];
        uint8_t remaining_calls = g_accumulation_control.remaining_200ms_calls[i];
        
        const rbpower_period_statistics_t* stats = &g_home_network.units[i].statistics.periods[RBPOWER_STAT_PERIOD_3SEC];        
        
        ESP_LOGI("RBSTAT-DIAG", "Unit[%u] '%s':", i, def->name);
        ESP_LOGI("RBSTAT-DIAG", "  Accumulation: %s, 200ms_mode: %s, Remaining_200ms: %d",
                 accumulation_enabled ? "ON" : "OFF",
                 ms200_done ? "YES" : "NO",
                 remaining_calls);
        ESP_LOGI("RBSTAT-DIAG", " STAT DIAGNOSTIC RMS: U=%.1fV I=%.2fA", stats->voltage_rms, stats->current_rms);
        if (accumulation_enabled) {
            const rbpower_period_accumulator_t* acc = &g_period_accumulators[i][RBPOWER_STAT_PERIOD_3SEC];
            ESP_LOGI("RBSTAT-DIAG", "  Accumulator: active=%s, samples=%lu, valid=%lu",
                     acc->is_active ? "YES" : "NO",
                     acc->metadata.samples_count,
                     acc->metadata.valid_samples_count);
            
            // Show last statistics

            if (stats->period_complete) {
                ESP_LOGI("RBSTAT-DIAG", "  Last statistics: U=%.1fV, I=%.2fA, P=%.0fW, E=%.3fWh",
                         stats->voltage_rms, stats->current_rms, 
                         stats->power_active_avg, stats->energy_period.consumed_wh);
            }
            found_accumulation = true;
        }
    }
    
    if (!found_accumulation) {
        ESP_LOGI("RBHOME", "NO ACTIVE ACCUMULATION");
    }
    rbpower_test_aggregation_api();
    rbpower_print_all_aggregation_diagnostics();
    ESP_LOGI("RBHOME", "=== END ACCUMULATION DIAGNOSTIC ===");
}

/*
 * @brief Start statistics accumulation system (called after timers start)
 */
rbpower_err_t rbpower_start_accumulation_system(void)
{
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (!rbpower_statistics_is_initialized()) {
        ESP_LOGI("RBHOME", "Statistics system not initialized");
        return RBPOWER_ERR_NOT_INIT;
    }
    
    ESP_LOGI("RBHOME", "Starting statistics accumulation system...");
    ESP_LOGI("RBHOME", "System voltage_bus_unit_id = 0x%04X", g_home_network.system.voltage_bus_unit_id);
    ESP_LOGI("RBHOME", "System main_supply_unit_id = 0x%04X", g_home_network.system.main_supply_unit_id);
    
    // Check timer availability
    rbpower_timer_info_t timer_info;
    rbpower_err_t err = rbpower_statistics_timers_get_info(RBPOWER_STAT_PERIOD_3SEC, &timer_info);
    if (err != RBPOWER_OK || timer_info.state != RBPOWER_TIMER_STATE_RUNNING) {
        ESP_LOGE("RBHOME", "3-sec timer not running");
        return RBPOWER_ERR_INVALID_STATE;
    }
    /*
   // === FIX: ENABLE ACCUMULATION FOR ALL UNITS ===
    uint8_t enabled_units = 0;
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition) {
            // Enable accumulator
            g_period_accumulators[i][RBPOWER_STAT_PERIOD_3SEC].is_enabled = true;
            
            // === MAIN FIX: ENABLE ACCUMULATION CONTROL ===
            g_accumulation_control.accumulation_enabled[i] = true;
            g_accumulation_control.ms200_done[i] = false;
            g_accumulation_control.remaining_200ms_calls[i] = 0;
            g_accumulation_control.expected_200ms_calls[i] = 0;
            g_accumulation_control.period_start_time_us[i] = 0;
            
            ESP_LOGI("RBHOME", "Accumulation enabled for unit %d ('%s'), unit_id=0x%04X%s", 
                     i, g_home_network.units[i].definition.name,
                     g_home_network.units[i].definition.unit_id,
                     g_home_network.units[i].definition.unit_id == g_home_network.system.voltage_bus_unit_id ? " [VOLTAGE BUS]" : "");
            // Enable periods for all units
            // ONLY 15MIN and HOUR use RAM accumulators
            g_period_accumulators[i][RBPOWER_STAT_PERIOD_15MIN].is_enabled = true;
            g_period_accumulators[i][RBPOWER_STAT_PERIOD_1HOUR].is_enabled = true;

            // DAY/WEEK/MONTH/YEAR DON'T use RAM accumulators (dynamic calculation from Flash)
            // g_period_accumulators[i][RBPOWER_STAT_PERIOD_DAILY].is_enabled = false;   // Commented out - not needed
            // g_period_accumulators[i][RBPOWER_STAT_PERIOD_WEEKLY].is_enabled = false;  // Commented out - not needed
            // g_period_accumulators[i][RBPOWER_STAT_PERIOD_MONTHLY].is_enabled = false; // Commented out - not needed
            // g_period_accumulators[i][RBPOWER_STAT_PERIOD_YEARLY].is_enabled = false;  // Commented out - not needed

            g_period_accumulators[i][RBPOWER_STAT_PERIOD_15MIN].is_active = true;
            g_period_accumulators[i][RBPOWER_STAT_PERIOD_1HOUR].is_active = true;

            ESP_LOGI("RBHOME", "Statistics enabled for unit %d (3sec + 15min + 1hr) | DAY/WEEK/MONTH/YEAR from Flash", i);
            enabled_units++;
        }
    }
    
    ESP_LOGI("RBHOME", "Accumulation system started for %d units", enabled_units);
    ESP_LOGI("RBHOME", "Next 3-sec period: %llu us", timer_info.next_trigger_time_us);
    */
    return RBPOWER_OK;
}

/**
 * @brief Apply a 200 ms measurement buffer to the home network (used by the inject API;
 *        originally the native HAL buffer path).
 */
static esp_err_t rbpower_update_home_network_from_200ms_buffer(
    const rbpower_unit_realtime_average_200ms_t *buffer,
    uint8_t units_count,
    uint32_t sequence_number)
{
    if (!g_home_network_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    rbpower_home_lock();   // write transaction (core-0 200ms update task / inject path)
    uint64_t update_timestamp = esp_timer_get_time();

    // Update 200ms data of units
    for (uint8_t i = 0; i < units_count; i++) {
        const rbpower_unit_realtime_average_200ms_t *src = &buffer[i];
        // Buffer convention: the entry carries the unit INDEX (not unit_id) in its low byte
        uint8_t unit_index = (uint8_t)(src->unit_id & 0xFF);  // Take lower byte as index
        
        
        
        if (unit_index < RBPOWER_MAX_HOME_UNITS && 
            g_home_network.units[unit_index].has_valid_definition) {
            
            // Copy 200ms data
            memcpy(&g_home_network.units[unit_index].realtime_average_200ms, src, 
                   sizeof(rbpower_unit_realtime_average_200ms_t));
            
            // Restore correct unit_id
            g_home_network.units[unit_index].realtime_average_200ms.unit_id = g_home_network.units[unit_index].definition.unit_id;
            
            g_home_network.units[unit_index].last_update_us = update_timestamp;
            // === 200MS DATA ACCUMULATION CONTROL ===
            if (g_accumulation_control.accumulation_enabled[unit_index]) {
                
                if (!g_accumulation_control.ms200_done[unit_index]) {
                    // === FIRST 200ms CALL - SWITCH ===
                    g_accumulation_control.ms200_done[unit_index] = true;
                    
                    // Calculate remaining time until end of 3-sec period
                    uint64_t current_time = esp_timer_get_time();
                    uint64_t next_trigger_time = g_stats_system.timers[RBPOWER_STAT_PERIOD_3SEC].next_trigger_time_us;
                    //ESP_LOGI("RBHOME-1st200MS", " === 1-st 200ms ===");
                    if (next_trigger_time > current_time) {
                        uint64_t time_remaining = next_trigger_time - current_time;
                        g_accumulation_control.expected_200ms_calls[unit_index] = time_remaining / 200000; // 200ms in us
                        g_accumulation_control.remaining_200ms_calls[unit_index] = g_accumulation_control.expected_200ms_calls[unit_index];
                        //ESP_LOGI("RBHOME-1st200MS", " time_remaining=%llu us, expected_calls=%d", time_remaining, g_accumulation_control.expected_200ms_calls[unit_id]);                        
                    } else {
                        // Period already ended or about to end
                        g_accumulation_control.remaining_200ms_calls[unit_index] = 0;
                    }
                    
                    // DON'T accumulate first 200ms data (this is past data)
                    continue; // Skip accumulation for this unit
                    
                } else if (g_accumulation_control.remaining_200ms_calls[unit_index] > 0) {
                    // === SUBSEQUENT 200ms CALLS - ACCUMULATION ===
                    // Accumulation from 200ms data
                    accumulate_unit_data(unit_index, true);
                    
                    g_accumulation_control.remaining_200ms_calls[unit_index]--;
                    
                    // Check 200ms accumulation completion
                    if (g_accumulation_control.remaining_200ms_calls[unit_index] == 0) {
                        //ESP_LOGI("RBHOME-1st200MS", " 200 ms finished for unit %d ('%s')",unit_index, g_home_network.units[unit_index].definition.name); 
                        g_accumulation_control.ms200_done[unit_index] = false;
                    }
                }
            }
        }
    }

    rbpower_home_unlock();
    return ESP_OK;
}

/* ================================================================================================
 * EXTERNAL-SOURCE MEASUREMENT INJECTION
 * ------------------------------------------------------------------------------------------------
 * One apply logic for all sources. The static apply functions key their input buffer by unit
 * *index* (src->unit_id holds the index in its low byte), so we resolve the caller's real
 * unit_id to an index, stamp it into a 1-element temp, and hand it to the apply function.
 * Zero duplication of accumulation logic. Safe to call from any source task (ESP-NOW/I2C driver).
 * ================================================================================================ */

rbpower_err_t rbpower_home_inject_halfcycle(rbpower_unit_id_t unit_id,
                                            const rbpower_unit_realtime_halfcycle_t *data)
{
    if (data == NULL) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || unit_index >= RBPOWER_MAX_HOME_UNITS ||
        !g_home_network.units[unit_index].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    // Buffer convention: the apply function reads (src->unit_id & 0xFF) as index.
    rbpower_unit_realtime_halfcycle_t tmp = *data;
    tmp.unit_id = unit_index;
    rbpower_update_home_network_from_buffer(&tmp, 1, 0);
    return RBPOWER_OK;
}

/* Per-unit energy-source provenance map (set by the rbAmp bridge, read by the telemetry
 * serializer). Small fixed table keyed by unit_id; first-set wins a slot, later sets update it. */
#define RBPOWER_ENERGY_MODE_MAP_SIZE 16 ///< Energy-mode map capacity
static struct { rbpower_unit_id_t id; uint8_t mode; bool used; }
    g_unit_energy_mode_map[RBPOWER_ENERGY_MODE_MAP_SIZE];

void rbpower_set_unit_energy_mode(rbpower_unit_id_t unit_id, uint8_t mode)
{
    for (int i = 0; i < RBPOWER_ENERGY_MODE_MAP_SIZE; i++) {
        if (g_unit_energy_mode_map[i].used && g_unit_energy_mode_map[i].id == unit_id) {
            g_unit_energy_mode_map[i].mode = mode;
            return;
        }
    }
    for (int i = 0; i < RBPOWER_ENERGY_MODE_MAP_SIZE; i++) {
        if (!g_unit_energy_mode_map[i].used) {
            g_unit_energy_mode_map[i].id = unit_id;
            g_unit_energy_mode_map[i].mode = mode;
            g_unit_energy_mode_map[i].used = true;
            return;
        }
    }
    /* table full → silently drop (16 i2c units is well past a typical installation; metadata-only loss). */
}

uint8_t rbpower_get_unit_energy_mode(rbpower_unit_id_t unit_id)
{
    for (int i = 0; i < RBPOWER_ENERGY_MODE_MAP_SIZE; i++) {
        if (g_unit_energy_mode_map[i].used && g_unit_energy_mode_map[i].id == unit_id) {
            return g_unit_energy_mode_map[i].mode;
        }
    }
    return 0xFF;  /* unknown */
}

/* Hardware-presence gating. Sets the home_unit's hardware_present flag by real hex id
 * (the compute gate at rbhome_statistics.c reads it). Resolves id→index; no-op for an unknown id. */
void rbpower_set_unit_hardware_present(rbpower_unit_id_t unit_id, bool present)
{
    uint8_t idx = find_unit_index_by_id(unit_id);
    if (idx == 0xFF || idx >= RBPOWER_MAX_HOME_UNITS) return;
    if (g_home_network.units[idx].hardware_present != present) rbpower_hw_rev_bump();
    g_home_network.units[idx].hardware_present = present;
}

bool rbpower_get_unit_hardware_present(rbpower_unit_id_t unit_id)
{
    uint8_t idx = find_unit_index_by_id(unit_id);
    if (idx == 0xFF || idx >= RBPOWER_MAX_HOME_UNITS) return false;
    return g_home_network.units[idx].hardware_present;
}

/* Remove a unit at runtime: stop its compute, drop it from the id→index map (so it's no longer
 * found / serialized), disable its period accumulators, and RECLAIM + ERASE its flash STATS slot.
 * The array index becomes a hole (no risky compaction) — units_count is unchanged and the hole is
 * excluded from compute/telemetry/serialization; the next restart rebuilds g_home_network clean
 * from the saved config (without this unit). Caller persists the updated units.json. */
/* External-source unbind hook (e.g. rbAmp bridge clears the fleet entry's unit_id
 * + persists /rbamp.json so a deleted rbAmp unit doesn't respawn on reboot). */
static rbpower_unit_unbind_cb_t s_unit_unbind_cb = NULL;
void rbpower_register_unit_unbind_cb(rbpower_unit_unbind_cb_t cb) { s_unit_unbind_cb = cb; }

/* External-source bind hook (symmetric to unbind; rbAmp bridge sets the fleet
 * entry's unit_id + persists /rbamp.json so the source starts injecting + the unit survives reboot). */
static rbpower_unit_bind_cb_t s_unit_bind_cb = NULL;
void rbpower_register_unit_bind_cb(rbpower_unit_bind_cb_t cb) { s_unit_bind_cb = cb; }
static rbpower_unit_feed_rebind_cb_t s_unit_feed_rebind_cb = NULL;
void rbpower_register_unit_feed_rebind_cb(rbpower_unit_feed_rebind_cb_t cb) { s_unit_feed_rebind_cb = cb; }
int rbpower_unit_feed_rebind(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch)
{
    return s_unit_feed_rebind_cb ? s_unit_feed_rebind_cb(unit_id, module, ch) : -1;
}
static rbpower_unit_ct_cb_t s_unit_ct_cb = NULL;
void rbpower_register_unit_ct_cb(rbpower_unit_ct_cb_t cb) { s_unit_ct_cb = cb; }
esp_err_t rbpower_unit_set_ct(rbpower_unit_id_t unit_id, uint8_t ct_code)
{
    return s_unit_ct_cb ? s_unit_ct_cb(unit_id, ct_code) : ESP_ERR_NOT_SUPPORTED;
}

/* ---- Opaque-handle identity API ---- */

const rbpower_unit_definition_t* rbpower_home_get_unit_definition(rbpower_unit_id_t unit_id)
{
    uint8_t idx = find_unit_index_by_id(unit_id);
    if (idx == 0xFF || idx >= RBPOWER_MAX_HOME_UNITS ||
        !g_home_network.units[idx].has_valid_definition) return NULL;
    return &g_home_network.units[idx].definition;
}

rbpower_unit_definition_t* rbpower_home_get_unit_definition_mutable(rbpower_unit_id_t unit_id)
{
    return (rbpower_unit_definition_t*)rbpower_home_get_unit_definition(unit_id);
}

rbpower_unit_id_t rbpower_home_find_unit_by_source(rbpower_source_type_t source_type,
                                                   uint64_t device_uid, uint8_t channel_idx,
                                                   rbpower_unit_id_t exclude_uid)
{
    if (!g_home_network_initialized) return RBPOWER_HOME_INVALID_ID;
    for (uint8_t i = 0; i < g_home_network.units_count && i < RBPOWER_MAX_HOME_UNITS; i++) {
        const rbpower_home_unit_t* u = &g_home_network.units[i];
        if (!u->has_valid_definition || u->definition.sources_count == 0) continue;
        if (u->definition.unit_id == exclude_uid) continue;
        if (u->definition.sources[0].source_type != source_type) continue;
        if (u->definition.device_uid == device_uid &&
            u->definition.channel_idx == channel_idx) {
            return u->definition.unit_id;
        }
    }
    return RBPOWER_HOME_INVALID_ID;
}

rbpower_unit_id_t rbpower_home_find_unit_by_point(const char* point)
{
    if (!g_home_network_initialized || !point || !point[0]) return RBPOWER_HOME_INVALID_ID;
    for (uint8_t i = 0; i < g_home_network.units_count && i < RBPOWER_MAX_HOME_UNITS; i++) {
        const rbpower_home_unit_t* u = &g_home_network.units[i];
        if (!u->has_valid_definition) continue;
        if (strcmp(u->definition.point, point) == 0) return u->definition.unit_id;
    }
    return RBPOWER_HOME_INVALID_ID;
}

rbpower_unit_id_t rbpower_home_alloc_unit_id(void)
{
    /* Occupancy = live units + flash slot map (a freed unit's history slot keeps its unit_id in
     * the map until reclaimed — reusing that id would graft dead history onto a new unit). */
    uint16_t slot_units[64];
    int used_slots = 0;
    if (rbpower_flash_slot_map_get(slot_units, 64, &used_slots) != ESP_OK) used_slots = 0;

    for (uint16_t cand = 0x0100; cand <= 0x3FFF; cand++) {
        if (find_unit_index_by_id((rbpower_unit_id_t)cand) != 0xFF) continue;
        bool in_slot_map = false;
        for (int s = 0; s < used_slots; s++) {
            if (slot_units[s] == cand) { in_slot_map = true; break; }
        }
        if (!in_slot_map) return (rbpower_unit_id_t)cand;
    }
    return RBPOWER_HOME_INVALID_ID;
}

rbpower_err_t rbpower_home_rebind_unit_source(rbpower_unit_id_t unit_id,
                                              const rbpower_data_source_t* new_source,
                                              uint64_t device_uid, uint8_t channel_idx,
                                              float rated_current_a)
{
    if (!g_home_network_initialized) return RBPOWER_ERR_NOT_INIT;
    if (!new_source) return RBPOWER_ERR_INVALID_ARG;
    rbpower_unit_definition_t* def = rbpower_home_get_unit_definition_mutable(unit_id);
    if (!def) return RBPOWER_ERR_INVALID_ARG;

    /* Clear the OLD source binding first (fleet entry lookup is by unit_id — still the old sensor). */
    if (s_unit_unbind_cb) s_unit_unbind_cb(unit_id);

    rbpower_home_lock();
    def->sources[0]   = *new_source;
    def->sources_count = 1;
    def->device_uid   = device_uid;
    def->channel_idx  = channel_idx;
    if (rated_current_a > 0.0f) {
        def->max_current_a = rated_current_a;
        def->rated_power_w = rated_current_a * def->rated_voltage_v;
        def->thresholds.over_current_a = rated_current_a * 0.9f;
        def->thresholds.over_power_w   = def->rated_power_w * 0.9f;
    }
    rbpower_home_unlock();

    /* Bind the NEW sensor (fleet entry located via def->device_uid/channel_idx). */
    if (s_unit_bind_cb) s_unit_bind_cb(def);
    ESP_LOGW("RBHOME", "Unit 0x%04X REBOUND to dev=%llu ch=%u (id/point/history kept)",
             unit_id, (unsigned long long)device_uid, channel_idx);
    return RBPOWER_OK;
}

rbpower_err_t rbpower_home_remove_unit(rbpower_unit_id_t unit_id)
{
    if (!g_home_network_initialized) return RBPOWER_ERR_NOT_INIT;
    uint8_t idx = find_unit_index_by_id(unit_id);
    if (idx == 0xFF || idx >= RBPOWER_MAX_HOME_UNITS ||
        !g_home_network.units[idx].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    // Hierarchy guard (API contract): block deleting a unit that has children — delete them first
    // (no silent orphaning). All current units are flat (children_count=0), so this is defensive.
    if (g_home_network.units[idx].definition.children_count > 0) {
        ESP_LOGW("RBHOME", "Unit 0x%04X has %u children — delete them first",
                 unit_id, g_home_network.units[idx].definition.children_count);
        return RBPOWER_ERR_INVALID_STATE;
    }

    rbpower_home_lock();
    /* Mark the unit removed — invalid + inactive + no hardware. The compute gate, the telemetry
     * serializer, and the units.json serializer all skip !has_valid_definition. */
    g_home_network.units[idx].has_valid_definition = false;
    g_home_network.units[idx].definition.is_active = false;
    g_home_network.units[idx].hardware_present = false;
    /* Clear the tombstone's stored id so validate_predefined_unit's duplicate-id scan (which walks
     * every slot < units_count regardless of has_valid_definition) can't match this stale id — else
     * a delete-then-rebind WITHOUT a reboot would be wrongly rejected. */
    g_home_network.units[idx].definition.unit_id = 0;
    /* Drop from the id→index map so find_unit_index_by_id no longer resolves it. */
    for (int i = 0; i < RBPOWER_MAX_HOME_UNITS; i++) {
        if (g_unit_id_map[i].is_valid && g_unit_id_map[i].unit_id == unit_id) {
            g_unit_id_map[i].is_valid = false;
        }
    }
    /* Disable its period accumulators so nothing aggregates for it. Only the 3 RAM-accumulated
     * slots exist (3SEC/15MIN/1HOUR); day+ are flash-recompute with no accumulator. */
    for (int p = 0; p < RBPOWER_RAM_PERIOD_COUNT; p++) {
        g_period_accumulators[idx][p].is_enabled = false;
        g_period_accumulators[idx][p].is_active = false;
    }
    rbpower_stats_forget_unit(idx);   /* open-hour NVS image: never restored into a re-created same-id unit */
    rbpower_home_unlock();

    /* Reclaim + erase its flash STATS slot and reset its ring cursors (no-op in RAM-only mode). */
    esp_err_t fe = rbpower_flash_remove_unit(unit_id);
    ESP_LOGW("RBHOME", "Unit 0x%04X REMOVED (compute stopped, slot reclaimed: %s)",
             unit_id, esp_err_to_name(fe));

    /* Unbind at the source layer so the unit doesn't respawn on reboot (rbAmp bridge clears the
     * fleet entry + persists /rbamp.json). No-op for sources with no binding. */
    if (s_unit_unbind_cb) {
        s_unit_unbind_cb(unit_id);
    }
    return RBPOWER_OK;
}

rbpower_err_t rbpower_home_create_unit(const rbpower_unit_definition_t* def)
{
    if (!g_home_network_initialized) return RBPOWER_ERR_NOT_INIT;
    if (!def) return RBPOWER_ERR_INVALID_ARG;

    rbpower_home_lock();
    rbpower_err_t rc = RBPOWER_OK;
    do {
        /* Already bound? (id→index resolves via the map) → reject FIRST as INVALID_STATE so the caller
         * maps it to HTTP 409. This MUST precede validate_predefined_unit, which also rejects a
         * duplicate id but only as INVALID_ARG — that would surface as a misleading 400. */
        if (find_unit_index_by_id(def->unit_id) != 0xFF) { rc = RBPOWER_ERR_INVALID_STATE; break; }
        if (!validate_predefined_unit(def)) { rc = RBPOWER_ERR_INVALID_ARG; break; }

        /* Reuse a removed (tombstone) slot below units_count so repeated delete/bind cycles don't
         * grow units_count unbounded; else append a fresh slot. */
        uint8_t unit_index = 0xFF;
        bool appending = false;
        for (uint8_t i = 0; i < g_home_network.units_count && i < RBPOWER_MAX_HOME_UNITS; i++) {
            if (!g_home_network.units[i].has_valid_definition) { unit_index = i; break; }
        }
        if (unit_index == 0xFF) {
            if (g_home_network.units_count >= RBPOWER_MAX_HOME_UNITS) { rc = RBPOWER_ERR_NO_MEMORY; break; }
            unit_index = g_home_network.units_count;
            appending = true;
        }

        /* Register into the live network — mirrors the per-unit block in rbpower_home_init. */
        g_home_network.units[unit_index].definition = *def;
        g_home_network.units[unit_index].has_valid_definition = true;
        g_home_network.units[unit_index].hardware_present = true;
        g_home_network.units[unit_index].has_realtime_data = false;
        g_home_network.units[unit_index].last_update_us = esp_timer_get_time();
        g_home_network.units[unit_index].definition.created_timestamp_us = esp_timer_get_time();

        if (!add_unit_id_mapping(def->unit_id, unit_index)) {
            g_home_network.units[unit_index].has_valid_definition = false;
            rc = RBPOWER_ERR_NO_MEMORY;
            break;
        }
        if (def->sources[0].source_type == RBPOWER_SOURCE_ADC_SENSOR) {
            create_adc_source_mapping(def->unit_id, def->sources[0].source_id,
                                      def->sources[0].sensor_profile_id);
        }
        initialize_unit_realtime_data(unit_index, def->unit_id);
        initialize_unit_statistics(unit_index, def->unit_id, def->stat_class);

        /* Enable this unit's period accumulators per its CONFIGURED DEPTH (stat_class), mirroring the
         * per-unit enable-loop in rbpower_home_init (the "period depth" of the unit config). That
         * init loop runs at boot — before this runtime unit exists (rbAmp units are created post-boot
         * via add_rbamp_module) — so without enabling here the unit's accumulators stay is_enabled=false,
         * should_aggregate_to_period() is false, the natural 15min→1HOUR rollup is silently skipped, the
         * 1HOUR acc never seeds (dt=0) and NO natural HOUR records are produced (15MIN full, 1HOUR
         * empty after the hour boundaries). Driving it from get_periods_mask_by
         * _stat_class — not a blanket 1HOUR force-enable — keeps the period depth under config control
         * (a 15MIN-depth unit must NOT auto-grow a 1HOUR record). Replaces the two band-aid force-enables
         * that previously lived in rbpower_rollup_after_15min and rbpower_home_inject_period_result.
         * Done unconditionally (not gated on rbpower_statistics_is_initialized like the boot init-loop):
         * this is always a post-boot runtime call, the accumulator array is a static global, and the
         * old band-aids enabled unconditionally too — so this strictly supersedes them with no ordering
         * fragility. */
        uint32_t periods_mask = get_periods_mask_by_stat_class(def->stat_class);
        for (uint8_t period = 0; period < RBPOWER_RAM_PERIOD_COUNT; period++) {   // only RAM-accumulated slots exist
            if (periods_mask & (1u << period)) {
                g_period_accumulators[unit_index][period].is_enabled = true;
            }
        }
        g_accumulation_control.accumulation_enabled[unit_index] = true;
        if (appending) g_home_network.units_count++;

        ESP_LOGW("RBHOME", "Unit 0x%04X CREATED at runtime (idx %u, '%s')",
                 def->unit_id, unit_index, def->name);
    } while (0);
    rbpower_home_unlock();

    if (rc != RBPOWER_OK) return rc;

    /* Same id as a unit removed in this boot → its runtime flash buffers still exist; re-occupy their
     * (erased) slot so the records written from now on survive the next boot. No-op otherwise. */
    rbpower_flash_reattach_unit(def->unit_id);

    /* Bind at the source layer (rbAmp bridge sets the fleet entry unit_id + persists /rbamp.json so
     * the source starts injecting + the unit survives reboot). No-op for sources with no binding. */
    if (s_unit_bind_cb) {
        s_unit_bind_cb(def);
    }
    return RBPOWER_OK;
}

/* Injected-period electrical envelope: STD modules keep no latch
 * banks, so the finished 15-min RESULT carries energy only and the HOUR record would show
 * pf/f/q/s = 0 for every rbAmp unit. The hub already receives P/Q/S/PF on each 1-s inject, so
 * average them here over the open period: Σ P/Q/S plus the system frequency sampled per inject.
 * Consumed (and reset) by rbpower_home_inject_period_result on the 15MIN inject: Q = ΣQ/n,
 * S = ΣS/n, PF = |ΣP|/ΣS (energy-weighted, not a mean of per-sample PF), f = Σf/n_f. */
/* Per-unit sums of the live injects over the open period: Σ P (W), Σ Q (var), Σ S (VA), Σ f (Hz);
 * n = power samples, n_f = frequency samples. */
/** @cond INTERNAL */
typedef struct {
    double   p_sum, q_sum, s_sum, f_sum;
    uint32_t n, n_f;
} rbhome_ipe_acc_t;
/** @endcond */
static rbhome_ipe_acc_t s_ipe[RBPOWER_MAX_HOME_UNITS];

/* Live-data age per unit (otherwise frozen values look live while the module is silent).
 * Stamped on every 200 ms inject; -1 = never fed since boot. */
static int64_t s_unit_live_us[RBPOWER_MAX_HOME_UNITS];
int32_t rbpower_home_unit_age_ms(rbpower_unit_id_t unit_id)
{
    uint8_t ix = find_unit_index_by_id(unit_id);
    if (ix == 0xFF || ix >= RBPOWER_MAX_HOME_UNITS || s_unit_live_us[ix] == 0) return -1;
    int64_t d = (esp_timer_get_time() - s_unit_live_us[ix]) / 1000;
    return d > INT32_MAX ? INT32_MAX : (int32_t)d;
}

rbpower_err_t rbpower_home_inject_200ms(rbpower_unit_id_t unit_id,
                                        const rbpower_unit_realtime_average_200ms_t *data)
{
    if (data == NULL) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || unit_index >= RBPOWER_MAX_HOME_UNITS ||
        !g_home_network.units[unit_index].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    rbpower_unit_realtime_average_200ms_t tmp = *data;
    tmp.unit_id = unit_index;
    s_unit_live_us[unit_index] = esp_timer_get_time();   /* data age for the API (age_s / q_live STALE) */
    rbpower_home_housekeeping_tick();   /* boot forensics heartbeat + periodic housekeeping (rate-limited) */
    /* Per-install CT orientation (units-config sources[].invert_sign): flip the SIGNED
     * quantities at the inject boundary so every downstream consumer (direction split, 3sec/15min
     * accumulation, energy in/out, billing) sees the corrected direction. Magnitudes (I, S, PF)
     * stay as-is; the consuming/supplying pair swaps. */
    if (g_home_network.units[unit_index].definition.sources[0].invert_sign) {
        tmp.average_200ms.power_active_avg_w     = -tmp.average_200ms.power_active_avg_w;
        tmp.average_200ms.power_reactive_avg_var = -tmp.average_200ms.power_reactive_avg_var;
        tmp.instant_power_net_w = -tmp.instant_power_net_w;
        float swap_w = tmp.instant_power_consuming_w;
        tmp.instant_power_consuming_w = tmp.instant_power_supplying_w;
        tmp.instant_power_supplying_w = swap_w;
    }
    /* Direction label for injected (rbAmp / ESP-NOW) frames: the (removed) native ZC analyser was
     * the only producer of zc_analysis.current_direction, so every injected unit would report dir=0
     * (UNKNOWN), e.g. a main input under 1 kW import. Derive it from the signed
     * active power when the source left it UNKNOWN. ±0.5 W dead band = ZERO. The 3-sec energy
     * split below keys on this label too (energy_increment is 0 for injected frames, so no
     * accounting changes — only the label and the consuming/supplying sample counters). */
    if (tmp.zc_analysis.current_direction == RBPOWER_CURRENT_DIRECTION_UNKNOWN) {
        float pw = tmp.average_200ms.power_active_avg_w;
        tmp.zc_analysis.current_direction = (pw >  0.5f) ? RBPOWER_CURRENT_DIRECTION_CONSUMING
                                          : (pw < -0.5f) ? RBPOWER_CURRENT_DIRECTION_SUPPLYING
                                                         : RBPOWER_CURRENT_DIRECTION_ZERO;
    }
    /* Period electrical envelope for the injected unit (see s_ipe). Sampled AFTER the sign flip so the
     * averaged P/Q follow the corrected direction; S and PF are magnitudes. */
    {
        rbhome_ipe_acc_t *a = &s_ipe[unit_index];
        float pw = tmp.average_200ms.power_active_avg_w;
        float qv = tmp.average_200ms.power_reactive_avg_var;
        float sv = tmp.average_200ms.power_apparent_avg_va;
        if (isfinite(pw) && isfinite(sv)) {
            a->p_sum += pw;
            a->q_sum += isfinite(qv) ? qv : 0.0f;
            a->s_sum += (sv > 0.0f) ? sv : fabsf(pw);
            a->n++;
            float hz = g_home_network.system.system_frequency_hz;
            if (hz >= 40.0f && hz <= 70.0f) { a->f_sum += hz; a->n_f++; }
        }
    }
    rbpower_update_home_network_from_200ms_buffer(&tmp, 1, 0);
    return RBPOWER_OK;
}

/* Feed the system grid frequency from an external source (rbAmp modules measure it but the 200ms
 * inject struct carries no frequency field, so it's set directly here — like the net-power compute).
 * The telemetry builder prefers this over the voltage_bus stats (otherwise freq = 0). */
/* System voltage-bus unit id (0 = none). The rbAmp bridge uses it to route a voltage-bearing
 * module's mains voltage into the specialized voltage bus, whose unit has a legacy ADC-era
 * definition nobody else feeds (otherwise v_bus.v = 0). */
/* Which rbAmp module feeds the voltage bus (0xFF = none). Set by the bridge when it routes a
 * module's mains voltage into the vbus unit; telemetry uses it to label v_bus with its REAL
 * source (i2c/rbamp/module) instead of the ADC-era definition ("adc"/"native_adc"). */
static uint8_t s_vbus_feed_module = 0xFF;
void rbpower_home_set_voltage_bus_feed(uint8_t module) { s_vbus_feed_module = module; }
uint8_t rbpower_home_get_voltage_bus_feed(void) { return s_vbus_feed_module; }

/* rbAmp feed per unit (see rbhome_types.h) — tiny static map keyed by unit id, same shape as the
 * energy-mode map. module=0xFF clears the slot. Plain stores: safe from the bridge task. */
#define RBPOWER_UNIT_FEED_MAP_SIZE 16 ///< rbAmp feed map capacity
static struct { rbpower_unit_id_t id; uint8_t module; uint8_t source; bool used; }
    g_unit_rbamp_feed_map[RBPOWER_UNIT_FEED_MAP_SIZE];

static float   s_ct_load_pct[RBPOWER_MAX_HOME_UNITS];
static uint8_t s_ct_state[RBPOWER_MAX_HOME_UNITS];
static bool    s_ct_known[RBPOWER_MAX_HOME_UNITS];
void rbpower_set_unit_ct_load(rbpower_unit_id_t unit_id, float load_pct, uint8_t state)
{
    uint8_t i = find_unit_index_by_id(unit_id);
    if (i >= RBPOWER_MAX_HOME_UNITS) return;
    s_ct_known[i] = load_pct >= 0.0f;
    s_ct_load_pct[i] = load_pct;
    s_ct_state[i] = state;
}
bool rbpower_get_unit_ct_load(rbpower_unit_id_t unit_id, float *load_pct, uint8_t *state)
{
    uint8_t i = find_unit_index_by_id(unit_id);
    if (i >= RBPOWER_MAX_HOME_UNITS || !s_ct_known[i]) return false;
    if (load_pct) *load_pct = s_ct_load_pct[i];
    if (state) *state = s_ct_state[i];
    return true;
}

void rbpower_set_unit_rbamp_feed(rbpower_unit_id_t unit_id, uint8_t module, uint8_t source)
{
    if (unit_id == 0) return;
    for (int i = 0; i < RBPOWER_UNIT_FEED_MAP_SIZE; i++) {
        if (g_unit_rbamp_feed_map[i].used && g_unit_rbamp_feed_map[i].id == unit_id) {
            if (module == 0xFF) { g_unit_rbamp_feed_map[i].used = false; return; }
            g_unit_rbamp_feed_map[i].module = module;
            g_unit_rbamp_feed_map[i].source = source;
            return;
        }
    }
    if (module == 0xFF) return;
    for (int i = 0; i < RBPOWER_UNIT_FEED_MAP_SIZE; i++) {
        if (!g_unit_rbamp_feed_map[i].used) {
            g_unit_rbamp_feed_map[i].id     = unit_id;
            g_unit_rbamp_feed_map[i].module = module;
            g_unit_rbamp_feed_map[i].source = source;
            g_unit_rbamp_feed_map[i].used   = true;
            return;
        }
    }
    /* table full → drop (label-only metadata; 16 bridge-fed units is past any current fleet). */
}

bool rbpower_get_unit_rbamp_feed(rbpower_unit_id_t unit_id, uint8_t* module, uint8_t* source)
{
    for (int i = 0; i < RBPOWER_UNIT_FEED_MAP_SIZE; i++) {
        if (g_unit_rbamp_feed_map[i].used && g_unit_rbamp_feed_map[i].id == unit_id) {
            if (module) *module = g_unit_rbamp_feed_map[i].module;
            if (source) *source = g_unit_rbamp_feed_map[i].source;
            return true;
        }
    }
    return false;
}

/* ---- Periodic housekeeping (see rbhome_types.h) ----
 * Hosted by whichever periodic path a given hub actually runs: the 3-s statistics callback, the
 * 200-ms live inject, and the rbAmp bridge poll loop (on I2C-only hubs the 3-s callback alone does
 * not keep the heartbeat current — the boot line would show uptime_before=5s after a 70-s run). Rate-limited
 * to 2 s; takes the (recursive) home lock itself, so it is safe from any of those contexts. */
void rbpower_home_housekeeping_tick(void)
{
    static int64_t s_last_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (s_last_us != 0 && now_us - s_last_us < 2000000LL) return;
    s_last_us = now_us;
    if (!g_home_network_initialized) return;
    rbpower_home_lock();
    rbpower_boot_heartbeat();                 /* boot forensics: last-known uptime */
    rbpower_lifetime_check_day_rollover();    /* midnight passed in downtime -> daily counters from zero */
    rbpower_stats_boot_clock_tick();          /* calendar-bucket baseline + DAY+ closes for downtime */
    rbpower_lifetime_nvs_tick();              /* flush a throttled lifetime snapshot (cost after energy) */
    rbpower_home_unlock();
}

/* Pre-restart hook: the rbAmp bridge registers a flush of the open quarter of every module, so a
 * PLANNED restart (console reboot / OTA / config apply) leaves only the real downtime to the
 * reboot-gap tail. Without it the whole open quarter (up to 15 min, measured while the hub was alive)
 * would be credited to the hour the hub came back in. */
static void (*s_pre_restart_hook)(void) = NULL;
void rbpower_set_pre_restart_hook(void (*fn)(void)) { s_pre_restart_hook = fn; }

/* ---- Boot forensics (see rbhome_types.h) ----
 * RTC_NOINIT boot marker: magic, planned-restart flag, last heartbeat uptime (s), task-watchdog
 * culprits per core and trigger text, last wall-clock time; panic forensics (p_*: core, exception,
 * PC, cause, vaddr, 4 backtrace PCs — with console NONE and no coredump a panic would otherwise
 * leave no trace); and the last stage of the statistics task when the task watchdog fired. */
/** @cond INTERNAL */
typedef struct { uint32_t magic; uint32_t planned; uint32_t uptime_s; char wdt_task[2][16]; char wdt_trig[40]; uint32_t last_wall_ts;
                 /* panic forensics */
                 uint32_t p_magic; uint32_t p_core; uint32_t p_exc; uint32_t p_pc; uint32_t p_cause; uint32_t p_vaddr; uint32_t p_bt[4];
                 char stats_stage[24]; } rb_boot_marker_t;   /* last stage of rbpwr_stats when the TWDT fired */
/** @endcond */
/* what the stats task is doing right now — static strings only; copied into the boot marker by the TWDT hook */
static const char* volatile s_stats_stage = "";
void rbpower_stats_stage(const char* stage) { s_stats_stage = stage ? stage : ""; }
#define RB_PANIC_MAGIC 0x50414E43u   ///< Panic record magic, "PANC"
/** @brief Whether the wall clock is valid (rbtime.c). @return true if valid. */
extern bool rbtime_is_time_valid(void);
/** @brief Current wall-clock time (rbtime.c). @return Epoch s. */
extern uint64_t rbtime_get_unix_timestamp(void);
/* boot info for the API (lets the UI explain a gap: "the device restarted at 01:14 — hours 00:00–01:14 not recorded") */
static char     s_boot_reason[40] = "unknown";
static bool     s_boot_planned = false;
static uint32_t s_boot_last_wall = 0;   /* last heartbeat wall ts before the reset (0 = unknown) */
static bool s_boot_checked = false;   /* heartbeat is a no-op until boot_check has read the marker */
#define RB_BOOT_MAGIC 0x424F4F54u   ///< Boot marker magic, "BOOT"
static RTC_NOINIT_ATTR rb_boot_marker_t s_boot_marker;
void rbpower_mark_planned_restart(void)
{
    static bool s_once = false;
    if (!s_once && s_pre_restart_hook) { s_once = true; s_pre_restart_hook(); }
    s_boot_marker.magic = RB_BOOT_MAGIC; s_boot_marker.planned = 1;
}
/* Task-watchdog ISR hook (main.cpp: esp_task_wdt_isr_user_handler): remember which task was running on
 * each core so the next boot line can name the spinner — the panic print is invisible (console NONE). */
/* Linked with -Wl,--wrap=esp_panic_handler (main/CMakeLists.txt): record where it crashed into the RTC marker,
 * then run IDF's handler unchanged. IRAM, no flash access, no locks. */
#include "esp_private/panic_internal.h"
#include "esp_debug_helpers.h"
#include "xtensa_context.h"
#include "esp_cpu_utils.h"
/** @brief Original ESP-IDF panic handler (linker --wrap). @param[in] info Panic information. */
extern void __real_esp_panic_handler(panic_info_t *info);
/**
 * @brief Panic handler wrapper: records the crash into the RTC boot marker, then calls the IDF handler.
 * @param[in] info Panic information.
 * @note Does not return normally.
 */
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info) {
        if (s_boot_marker.magic != RB_BOOT_MAGIC) { s_boot_marker.magic = RB_BOOT_MAGIC; s_boot_marker.planned = 0; }
        s_boot_marker.p_magic = RB_PANIC_MAGIC;
        s_boot_marker.p_core = (uint32_t)info->core;
        s_boot_marker.p_exc = (uint32_t)info->exception;
        s_boot_marker.p_pc = (uint32_t)info->addr;
        s_boot_marker.p_cause = 0; s_boot_marker.p_vaddr = 0;
        for (int i = 0; i < 4; ++i) s_boot_marker.p_bt[i] = 0;
        const XtExcFrame *xf = (const XtExcFrame *)info->frame;
        if (xf) {
            s_boot_marker.p_pc = xf->pc; s_boot_marker.p_cause = xf->exccause; s_boot_marker.p_vaddr = xf->excvaddr;
            esp_backtrace_frame_t fr = { .pc = xf->pc, .sp = xf->a1, .next_pc = xf->a0, .exc_frame = xf };
            for (int i = 0; i < 4; ++i) {
                if (!esp_backtrace_get_next_frame(&fr)) break;
                s_boot_marker.p_bt[i] = esp_cpu_process_stack_pc(fr.pc);
            }
        }
    }
    __real_esp_panic_handler(info);
}

void rbpower_boot_note_wdt(const char* t0, const char* t1)
{
    if (s_boot_marker.magic != RB_BOOT_MAGIC) { s_boot_marker.magic = RB_BOOT_MAGIC; s_boot_marker.planned = 0; }
    strlcpy(s_boot_marker.wdt_task[0], t0 ? t0 : "?", sizeof(s_boot_marker.wdt_task[0]));
    strlcpy(s_boot_marker.wdt_task[1], t1 ? t1 : "?", sizeof(s_boot_marker.wdt_task[1]));
    strlcpy(s_boot_marker.stats_stage, s_stats_stage, sizeof(s_boot_marker.stats_stage));
}
/* TWDT ISR hook, second part: the IDF triggered-entry printer streams its text through a callback —
 * caption, then "\n - " <task> " (CPU n)" per starved entry. Keep a compact "task (CPU n),task2 (CPU m)". */
void rbpower_boot_note_wdt_msg(const char* msg)
{
    if (!msg || !msg[0]) return;
    if (!strncmp(msg, "Task watchdog", 13)) { s_boot_marker.wdt_trig[0] = 0; return; }   /* caption = fresh list */
    if (!strcmp(msg, "\n - ")) msg = s_boot_marker.wdt_trig[0] ? "," : "";
    strlcat(s_boot_marker.wdt_trig, msg, sizeof(s_boot_marker.wdt_trig));
}
void rbpower_boot_heartbeat(void)
{
    if (!s_boot_checked) return;   /* the housekeeping tick runs before boot_check at init — don't clobber uptime_before */
    if (s_boot_marker.magic != RB_BOOT_MAGIC) { s_boot_marker.magic = RB_BOOT_MAGIC; s_boot_marker.planned = 0; }
    s_boot_marker.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    if (rbtime_is_time_valid()) s_boot_marker.last_wall_ts = (uint32_t)rbtime_get_unix_timestamp();
}
void rbpower_boot_info(uint32_t* boot_ts, const char** reason, bool* planned, uint32_t* downtime_s)
{
    uint32_t now = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    uint32_t up  = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t bts = (now > up) ? now - up : 0;
    if (boot_ts)    *boot_ts = bts;
    if (reason)     *reason = s_boot_reason;
    if (planned)    *planned = s_boot_planned;
    if (downtime_s) *downtime_s = (bts && s_boot_last_wall && bts > s_boot_last_wall) ? bts - s_boot_last_wall : 0;
}
int rbpower_boot_check(char* out, size_t cap)
{
    if (!out || cap == 0) return 0;
    esp_reset_reason_t r = esp_reset_reason();
    bool valid = (s_boot_marker.magic == RB_BOOT_MAGIC);
    bool planned = valid && s_boot_marker.planned;
    uint32_t up = valid ? s_boot_marker.uptime_s : 0;
    const char* name = "other";
    bool bad = false;
    switch (r) {
        case ESP_RST_POWERON:  name = "power-on"; break;
        case ESP_RST_SW:       name = planned ? "planned restart" : "software reset (NOT planned: panic/abort?)"; bad = !planned; break;
        case ESP_RST_PANIC:    name = "PANIC"; bad = true; break;
        case ESP_RST_INT_WDT:  name = "interrupt watchdog"; bad = true; break;
        case ESP_RST_TASK_WDT: name = "task watchdog"; bad = true; break;
        case ESP_RST_WDT:      name = "other watchdog"; bad = true; break;
        case ESP_RST_BROWNOUT: name = "brownout"; bad = true; break;
        case ESP_RST_DEEPSLEEP: name = "deep sleep"; break;
        case ESP_RST_EXT:      name = "external pin"; break;
        default: break;
    }
    strlcpy(s_boot_reason, name, sizeof(s_boot_reason)); s_boot_planned = planned;
    s_boot_last_wall = valid ? s_boot_marker.last_wall_ts : 0;
    int n = snprintf(out, cap, "%sboot: reset=%s (%d) uptime_before=%lus", bad ? "FAIL " : "", name, (int)r, (unsigned long)up);
    if (valid && r == ESP_RST_PANIC && s_boot_marker.p_magic == RB_PANIC_MAGIC) {
        /* decode offline: xtensa-esp32-elf-addr2line -pfiaC -e main.elf <pc> <bt…> (image_md5 tells which elf) */
        n = rb_scat(out, cap, n, " core%lu exc=%lu pc=%08lx cause=%lu vaddr=%08lx bt=%08lx,%08lx,%08lx,%08lx",
                      (unsigned long)s_boot_marker.p_core, (unsigned long)s_boot_marker.p_exc,
                      (unsigned long)s_boot_marker.p_pc, (unsigned long)s_boot_marker.p_cause, (unsigned long)s_boot_marker.p_vaddr,
                      (unsigned long)s_boot_marker.p_bt[0], (unsigned long)s_boot_marker.p_bt[1],
                      (unsigned long)s_boot_marker.p_bt[2], (unsigned long)s_boot_marker.p_bt[3]);
    }
    if (valid && (r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT) && s_boot_marker.wdt_task[0][0]) {
        s_boot_marker.wdt_task[0][15] = 0; s_boot_marker.wdt_task[1][15] = 0;
        n = rb_scat(out, cap, n, " running: cpu0=%s cpu1=%s", s_boot_marker.wdt_task[0], s_boot_marker.wdt_task[1]);
        if (s_boot_marker.wdt_trig[0]) {
            s_boot_marker.wdt_trig[sizeof(s_boot_marker.wdt_trig) - 1] = 0;
            n = rb_scat(out, cap, n, " starved: %s", s_boot_marker.wdt_trig);
            {   /* printable only: a marker written by an older image has garbage here */
                char st[24]; size_t k = 0;
                for (; k < sizeof(st) - 1 && s_boot_marker.stats_stage[k] >= 0x20 && s_boot_marker.stats_stage[k] < 0x7F; k++) st[k] = s_boot_marker.stats_stage[k];
                st[k] = '\0';
                if (k) n = rb_scat(out, cap, n, " stats_stage=%s", st);
            }
        }
    }
    memset(&s_boot_marker, 0, sizeof(s_boot_marker));   /* clears p_magic too */
    s_boot_marker.magic = RB_BOOT_MAGIC;
    s_boot_checked = true;
    return n;
}

/* ---- Diagnostics trace (see rbhome_types.h) ---- */
static rbpower_diag_sink_t s_diag_sink = NULL;
static rbpower_diag_sink_t s_diag_log_sink = NULL;   /* on-hub event log: always on, independent of `diag` */
static bool     s_diag_enabled  = false;
static uint32_t s_diag_period_s = 60;
void     rbpower_diag_set_sink(rbpower_diag_sink_t sink) { s_diag_sink = sink; }
void     rbpower_diag_set_log_sink(rbpower_diag_sink_t sink) { s_diag_log_sink = sink; }
void     rbpower_diag_set(bool enabled, uint32_t period_s) { s_diag_enabled = enabled; s_diag_period_s = period_s; }
static inline bool diag_console_on(void) { return s_diag_enabled && s_diag_sink != NULL; }
/* Event lines are produced whenever anyone listens: the console (`diag on`) or the event log. */
bool     rbpower_diag_enabled(void) { return diag_console_on() || s_diag_log_sink != NULL; }
/* Periodic reconciliation cadence: the configured one while the console trace is on; with only the event
 * log listening, one line per quarter (900 s) — keeps the counter reference live for the QTR lines without
 * filling the log with a 60-s trace. */
uint32_t rbpower_diag_period_s(void)
{
    if (diag_console_on()) return s_diag_period_s;
    return s_diag_log_sink ? 900 : 0;
}
uint32_t rbpower_diag_period_cfg_s(void) { return s_diag_period_s; }
bool     rbpower_diag_console_on(void) { return diag_console_on(); }
/* 
 * @brief Format one diagnostics line and pass it to the sinks (see rbhome_types.h).
 * @param[in] fmt printf-style format, followed by its arguments.
 */
void rbpower_diag_printf(const char* fmt, ...)
{
    if (!rbpower_diag_enabled() || !fmt) return;
    char line[200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (s_diag_log_sink) s_diag_log_sink(line);
    if (diag_console_on()) s_diag_sink(line);
}

rbpower_unit_id_t rbpower_home_voltage_bus_unit_id(void)
{
    if (!g_home_network_initialized) return 0;
    return g_home_network.system.voltage_bus_unit_id;
}

static int64_t s_sysfreq_set_us = 0;   /* last external frequency feed (0 = never → nominal) */
void rbpower_home_set_system_frequency(float hz)
{
    if (!g_home_network_initialized) return;
    /* Sanity window: ignore <=0 and clearly-corrupt reads (a bad I2C frame must not poison the
     * system frequency). 40–70 Hz covers 50/60 Hz grids with margin. */
    if (hz >= 40.0f && hz <= 70.0f) { g_home_network.system.system_frequency_hz = hz; s_sysfreq_set_us = esp_timer_get_time(); }
}
/* system_frequency_hz defaults to the NOMINAL 50 Hz at init, so its value alone
 * cannot tell a measurement from the default — a feed within the last 10 s (same staleness rule as
 * the grid voltage) does. */
bool rbpower_home_system_frequency_fresh(void)
{
    return s_sysfreq_set_us != 0 && (esp_timer_get_time() - s_sysfreq_set_us) < 10LL * 1000000LL;
}

/* Flow-conservation diagnostics: injected-quarter counter per unit
 * INDEX + the public reader joining it with the aggregation-side counters (rbhome_statistics.c).
 * Lets a test assert injected == aggregated == Σ stored instead of inferring losses from sums. */
static uint32_t s_flow_injected_15min[RBPOWER_MAX_HOME_UNITS];

rbpower_err_t rbpower_statflow_get(rbpower_unit_id_t unit_id, uint32_t* injected,
                                   uint32_t* agg15, uint32_t* hour_fin, uint32_t* hour_quarters)
{
    uint8_t idx = find_unit_index_by_id(unit_id);
    if (idx == 0xFF || idx >= RBPOWER_MAX_HOME_UNITS) return RBPOWER_ERR_INVALID_ARG;
    if (injected) *injected = s_flow_injected_15min[idx];
    rbpower_statflow_read_by_index(idx, agg15, hour_fin, hour_quarters);
    return RBPOWER_OK;
}

/* etstat diagnostics: per-unit-index feed bookkeeping for the et_in/et_out
 * totalizer path, so a test can verify "et grows and survives the power-cycle" from outside.
 * boot_seed_* = the totalizer value captured right before the FIRST feed after boot. */
/** @brief Feed bookkeeping of the exact-totalizer path of one unit. */
typedef struct {
    uint32_t feeds;              ///< Feeds since boot
    time_t   last_feed_ts;       ///< Last feed, epoch s
    int64_t  boot_seed_in_mwh;   ///< Import totalizer before the first post-boot feed, mWh
    int64_t  boot_seed_out_mwh;  ///< Export totalizer before the first post-boot feed, mWh
} rbhome_et_diag_t;
static rbhome_et_diag_t s_et_diag[RBPOWER_MAX_HOME_UNITS];


/* List the ids of valid units (etstat and other C++-side verbs: g_home_network itself is not
 * visible from .cpp TUs). Returns the number written, up to cap. */
uint8_t rbpower_home_list_unit_ids(uint16_t* ids, uint8_t cap)
{
    if (!ids || cap == 0) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < g_home_network.units_count && n < cap; i++) {
        if (!g_home_network.units[i].has_valid_definition) continue;
        ids[n++] = g_home_network.units[i].definition.unit_id;
    }
    return n;
}

bool rbpower_home_get_et_diag(uint8_t unit_index, uint32_t* feeds, time_t* last_feed_ts,
                              int64_t* seed_in_mwh, int64_t* seed_out_mwh)
{
    if (unit_index >= RBPOWER_MAX_HOME_UNITS) return false;
    if (feeds)        *feeds        = s_et_diag[unit_index].feeds;
    if (last_feed_ts) *last_feed_ts = s_et_diag[unit_index].last_feed_ts;
    if (seed_in_mwh)  *seed_in_mwh  = s_et_diag[unit_index].boot_seed_in_mwh;
    if (seed_out_mwh) *seed_out_mwh = s_et_diag[unit_index].boot_seed_out_mwh;
    return true;
}

/* ---- Module descriptor registry (see rbhome_types.h) ---- */
static rbpower_module_desc_t s_module_desc[RBPOWER_MODULE_DESC_MAX];
static volatile uint32_t s_hw_rev = 1;
uint32_t rbpower_hw_rev(void) { return s_hw_rev; }
static struct { uint8_t st; float scale; } s_sct[RBPOWER_MODULE_DESC_MAX][8];
static bool s_sct_init;
void rbpower_set_channel_sct(uint8_t module, uint8_t ch, uint8_t status, float scale)
{
    if (module >= RBPOWER_MODULE_DESC_MAX || ch >= 8) return;
    if (!s_sct_init) { for (int m = 0; m < RBPOWER_MODULE_DESC_MAX; ++m) for (int c = 0; c < 8; ++c) s_sct[m][c].st = 0xFF; s_sct_init = true; }
    if (s_sct[module][ch].st != status) s_hw_rev++;
    s_sct[module][ch].st = status; s_sct[module][ch].scale = scale;
}
bool rbpower_get_channel_sct(uint8_t module, uint8_t ch, uint8_t *status, float *scale)
{
    if (!s_sct_init || module >= RBPOWER_MODULE_DESC_MAX || ch >= 8 || s_sct[module][ch].st == 0xFF) return false;
    if (status) *status = s_sct[module][ch].st;
    if (scale) *scale = s_sct[module][ch].scale;
    return true;
}
void     rbpower_hw_rev_bump(void) { s_hw_rev++; }
void rbpower_set_module_desc(uint8_t module, const rbpower_module_desc_t* d)
{
    if (module >= RBPOWER_MODULE_DESC_MAX) return;
    rbpower_home_lock();
    rbpower_module_desc_t prev = s_module_desc[module];
    if (d) s_module_desc[module] = *d; else memset(&s_module_desc[module], 0, sizeof(s_module_desc[module]));
    rbpower_module_desc_t cur = s_module_desc[module];
    rbpower_home_unlock();
    if (memcmp(&prev, &cur, sizeof(prev)) == 0) return;
    rbpower_hw_rev_bump();
    if (prev.model[0] && cur.model[0] && (strcmp(prev.model, cur.model) != 0 || prev.n_i != cur.n_i))
        rbpower_diag_printf("module %u (0x%02X): %s -> %s, current channels %u -> %u", module, cur.addr, prev.model, cur.model,
                            prev.n_i, cur.n_i);
}
bool rbpower_get_module_desc(uint8_t module, rbpower_module_desc_t* out)
{
    if (module >= RBPOWER_MODULE_DESC_MAX || !out) return false;
    rbpower_home_lock();
    *out = s_module_desc[module];
    rbpower_home_unlock();
    return out->present || out->addr != 0;
}

/* ---- Per-phase live pool (contract in rbhome_types.h) ----
 * Sparse by design: only phase-grouped (three-phase module) units carry this, so a small pool instead of
 * widening every unit struct (64 units × 3 phases would be dead DRAM for single-CT installations). */
#define RBHOME_PHASE_LIVE_POOL 8 ///< Per-phase live pool size
/** @brief Per-phase live snapshot slot. */
typedef struct {
    rbpower_unit_id_t    unit_id;   ///< Unit ID (0 = free slot)
    int64_t              ts_us;     ///< Snapshot time, µs (esp_timer)
    uint8_t              count;     ///< Valid entries in @c ph
    rbpower_phase_live_t ph[3];     ///< Phase entries
} rbhome_phase_live_slot_t;
static rbhome_phase_live_slot_t s_phase_live[RBHOME_PHASE_LIVE_POOL];

rbpower_err_t rbpower_home_set_phase_live(rbpower_unit_id_t unit_id,
                                          const rbpower_phase_live_t* entries, uint8_t count)
{
    if (unit_id == 0 || count > 3 || (count > 0 && !entries)) return RBPOWER_ERR_INVALID_ARG;
    rbpower_home_lock();
    rbhome_phase_live_slot_t* slot = NULL;
    rbhome_phase_live_slot_t* oldest = &s_phase_live[0];
    for (int k = 0; k < RBHOME_PHASE_LIVE_POOL; k++) {
        if (s_phase_live[k].unit_id == unit_id) { slot = &s_phase_live[k]; break; }
        if (s_phase_live[k].unit_id == 0) { if (!slot) slot = &s_phase_live[k]; continue; }
        if (s_phase_live[k].ts_us < oldest->ts_us) oldest = &s_phase_live[k];
    }
    if (count == 0) {                       /* clear */
        if (slot && slot->unit_id == unit_id) memset(slot, 0, sizeof(*slot));
        rbpower_home_unlock();
        return RBPOWER_OK;
    }
    if (!slot) slot = oldest;               /* pool full → evict the stalest snapshot */
    slot->unit_id = unit_id;
    slot->ts_us = esp_timer_get_time();
    slot->count = count;
    memcpy(slot->ph, entries, (size_t)count * sizeof(rbpower_phase_live_t));
    {   /* Per-install CT orientation (sources[0].invert_sign) flips the per-phase P/Q too — the
         * top-level p_act is inverted at inject, so ph[].p must match (otherwise the sum shows
         * "export" while the phases show "consumption"). Current stays a magnitude. */
        uint8_t ui = find_unit_index_by_id(unit_id);
        if (ui != 0xFF && ui < RBPOWER_MAX_HOME_UNITS && g_home_network.units[ui].has_valid_definition &&
            g_home_network.units[ui].definition.sources[0].invert_sign) {
            for (uint8_t k = 0; k < count; k++) {
                slot->ph[k].p = -slot->ph[k].p;
                if (!isnan(slot->ph[k].q)) slot->ph[k].q = -slot->ph[k].q;
            }
        }
    }
    rbpower_home_unlock();
    return RBPOWER_OK;
}

uint8_t rbpower_home_get_phase_live(rbpower_unit_id_t unit_id,
                                    rbpower_phase_live_t out[3], int64_t* age_us)
{
    if (unit_id == 0 || !out) return 0;
    uint8_t n = 0;
    rbpower_home_lock();
    for (int k = 0; k < RBHOME_PHASE_LIVE_POOL; k++) {
        if (s_phase_live[k].unit_id != unit_id) continue;
        n = s_phase_live[k].count;
        memcpy(out, s_phase_live[k].ph, (size_t)n * sizeof(rbpower_phase_live_t));
        if (age_us) *age_us = esp_timer_get_time() - s_phase_live[k].ts_us;
        break;
    }
    rbpower_home_unlock();
    return n;
}

/* ---- Grid phase voltages (contract in rbhome_types.h) ---- */
static rbpower_grid_phase_t s_grid[3];

rbpower_err_t rbpower_home_set_grid_phase(uint8_t ph, float v, float f, uint8_t module, uint8_t vch)
{
    if (ph < 1 || ph > 3) return RBPOWER_ERR_INVALID_ARG;
    rbpower_grid_phase_t *g = &s_grid[ph - 1];
    rbpower_home_lock();
    g->ph = ph; g->v = v; g->f = f; g->src_module = module; g->src_vch = vch;
    g->ts_us = esp_timer_get_time();
    rbpower_home_unlock();
    return RBPOWER_OK;
}

uint8_t rbpower_home_get_grid(rbpower_grid_phase_t out[3])
{
    uint8_t n = 0;
    int64_t now = esp_timer_get_time();
    rbpower_home_lock();
    for (int k = 0; k < 3; k++) {
        const rbpower_grid_phase_t *g = &s_grid[k];
        if (g->ph == 0 || isnan(g->v) || g->v <= 1.0f || now - g->ts_us > RBPOWER_GRID_STALE_US) continue;
        if (out) out[n] = *g;
        n++;
    }
    rbpower_home_unlock();
    return n;
}

float rbpower_home_grid_voltage(uint8_t ph)
{
    rbpower_grid_phase_t g[3];
    uint8_t n = rbpower_home_get_grid(g);
    if (n == 0) return NAN;
    if (ph >= 1 && ph <= 3)
        for (uint8_t k = 0; k < n; k++) if (g[k].ph == ph) return g[k].v;
    float sum = 0.0f;
    for (uint8_t k = 0; k < n; k++) sum += g[k].v;
    return sum / (float)n;
}

/* One-shot p_min for the NEXT period result of a unit (the source's own short-window minimum, e.g. the rbAmp
 * bridge's 10-s rolling mean); NAN/unset = fall back to the period average (old behaviour). */
static float s_next_pmin[RBPOWER_MAX_HOME_UNITS] = { [0 ... RBPOWER_MAX_HOME_UNITS - 1] = NAN };
void rbpower_home_set_next_power_min(rbpower_unit_id_t unit_id, float p_min_w)
{
    uint8_t ix = find_unit_index_by_id(unit_id);
    if (ix != 0xFF && ix < RBPOWER_MAX_HOME_UNITS) s_next_pmin[ix] = p_min_w;
}

/* One-shot window end (quarter boundary) for the NEXT period result of a unit (see rbhome_types.h). */
static uint32_t s_next_qend[RBPOWER_MAX_HOME_UNITS];
void rbpower_home_set_next_period_end(rbpower_unit_id_t unit_id, uint32_t end_ts)
{
    uint8_t ix = find_unit_index_by_id(unit_id);
    if (ix != 0xFF && ix < RBPOWER_MAX_HOME_UNITS) s_next_qend[ix] = end_ts;
}

bool rbpower_home_unit_is_period_injected(uint8_t unit_index)
{
    if (unit_index >= RBPOWER_MAX_HOME_UNITS || !g_home_network.units[unit_index].has_valid_definition) return false;
    uint8_t m = rbpower_get_unit_energy_mode(g_home_network.units[unit_index].definition.unit_id);
    return m == RBPOWER_ENERGY_MODE_ON_DEVICE || m == RBPOWER_ENERGY_MODE_HOST;
}

rbpower_err_t rbpower_home_inject_period_result(rbpower_unit_id_t unit_id,
                                                rbpower_stat_period_type_t period,
                                                float avg_power_w,
                                                float max_power_w,
                                                float energy_wh,
                                                uint32_t period_ms,
                                                float voltage_v,
                                                float current_rms_a,
                                                float current_min_a,
                                                float current_max_a)
{
    if (!g_home_network_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    if (period >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || unit_index >= RBPOWER_MAX_HOME_UNITS ||
        !g_home_network.units[unit_index].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    /* Non-finite guard: a NaN/Inf added to the lifetime accumulator (+=) is STICKY and permanently
     * poisons the total. Clamp at the boundary regardless of caller. */
    if (!isfinite(energy_wh))   energy_wh = 0.0f;
    if (!isfinite(avg_power_w)) avg_power_w = 0.0f;
    if (!isfinite(max_power_w)) max_power_w = 0.0f;
    if (!isfinite(voltage_v))      voltage_v = 0.0f;
    if (!isfinite(current_rms_a))  current_rms_a = 0.0f;
    if (!isfinite(current_min_a))  current_min_a = 0.0f;
    if (!isfinite(current_max_a))  current_max_a = 0.0f;

    if (period == RBPOWER_STAT_PERIOD_15MIN) {
        s_flow_injected_15min[unit_index]++;   // flow diagnostics (statflow verb): quarters in
    }

    /* Per-install CT orientation (units-config sources[].invert_sign) — same boundary
     * rule as inject_200ms: flip the signed power/energy of the finished period before it reaches
     * the period slot and rollup. max flips with the sign too (the envelope keeps its meaning as
     * "largest magnitude in the corrected direction"). */
    if (g_home_network.units[unit_index].definition.sources[0].invert_sign) {
        avg_power_w = -avg_power_w;
        max_power_w = -max_power_w;
        energy_wh   = -energy_wh;
    }

    /* RMS current and its envelope are MAGNITUDES. Callers derive them from signed power
     * (bridge: i ≈ avg_p / V_bus), so a reversed-CT / exporting window hands a NEGATIVE
     * current here — and the hour aggregation's `>= 0` gates then silently drop it (HOUR
     * record i = 0 while v/P are fine). Normalize once at the
     * boundary; envelope order restored after the abs. */
    current_rms_a = fabsf(current_rms_a);
    current_min_a = fabsf(current_min_a);
    current_max_a = fabsf(current_max_a);
    if (current_min_a > current_max_a) {
        float swap_a = current_min_a;
        current_min_a = current_max_a;
        current_max_a = swap_a;
    }

    rbpower_home_lock();   // write transaction (rbAmp inject path → period slot + rollup)

    // Source-aware statistics: an external source (rbAmp) accumulates the period
    // ON-DEVICE and hands back the finished RESULT (avg power W, peak W, energy Wh
    // over its own wall-clock window). We write it straight into the period slot,
    // BYPASSING the 200ms sum-of-squares accumulator. RMS current/voltage are a
    // 200ms-path artifact and not meaningful for long periods → left zero here;
    // they stay available real-time via the live read path.
    rbpower_period_statistics_t *ps =
        &g_home_network.units[unit_index].statistics.periods[period];
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    memset(ps, 0, sizeof(*ps));

    ps->period_duration_ms = period_ms;
    ps->period_end_us = now_us;
    ps->period_start_us = (now_us >= (uint64_t)period_ms * 1000ULL)
                              ? now_us - (uint64_t)period_ms * 1000ULL
                              : 0;

    ps->power_active_avg = avg_power_w;     // W — the fundamental rbAmp period quantity
    ps->extremes.power_max = max_power_w;   // W — peak over the window
    ps->extremes.power_min = avg_power_w;   // no per-sample min from the device …
    if (unit_index < RBPOWER_MAX_HOME_UNITS && isfinite(s_next_pmin[unit_index])) {
        /* … unless the source supplied its short-window minimum (bridge 10-s rolling mean) */
        ps->extremes.power_min = (s_next_pmin[unit_index] < avg_power_w) ? s_next_pmin[unit_index] : avg_power_w;
        s_next_pmin[unit_index] = NAN;
    }

    // Real envelope: the source also supplies the period voltage + current so the
    // stored record carries a meaningful v/i envelope (not just power). Voltage is the shared v_bus
    // → a single value (min=max). Current carries the source-tracked per-window min/max. These feed
    // the 15min→1hour extremes aggregation → the history-API envelope. (0 = source didn't provide.)
    ps->voltage_rms = voltage_v;
    ps->current_rms = current_rms_a;
    if (voltage_v > 0.0f) {
        ps->extremes.voltage_min = voltage_v;
        ps->extremes.voltage_max = voltage_v;
    }
    if (current_max_a > 0.0f) {
        ps->extremes.current_min = current_min_a;
        ps->extremes.current_max = current_max_a;
    }

    /* Electrical envelope of the period from the 1-s injects (s_ipe): fills q/s/pf/f that the
     * module's energy RESULT does not carry → HOUR records of rbAmp units stop reading pf/f=0.
     * Consumed once per 15MIN inject; higher periods aggregate from the 15-min result. */
    if (period == RBPOWER_STAT_PERIOD_15MIN) {
        rbhome_ipe_acc_t *a = &s_ipe[unit_index];
        if (a->n > 0) {
            ps->power_reactive_avg = (float)(a->q_sum / a->n);
            ps->power_apparent_avg = (float)(a->s_sum / a->n);
            if (a->s_sum > 0.0) {
                float pf = (float)(fabs(a->p_sum) / a->s_sum);
                ps->power_factor_avg = (pf > 1.0f) ? 1.0f : pf;
            }
            if (a->n_f > 0) ps->frequency_avg = (float)(a->f_sum / a->n_f);
        }
        memset(a, 0, sizeof(*a));
    }

    // Energy sign-split: + consumed, - supplied (net keeps the sign).
    if (energy_wh >= 0.0f) {
        ps->energy_period.consumed_wh = energy_wh;
    } else {
        ps->energy_period.supplied_wh = -energy_wh;
    }
    ps->energy_period.net_wh = energy_wh;

    ps->samples_count = 1;
    ps->valid_samples_count = 1;
    ps->data_completeness_percent = 100;
    ps->period_complete = true;
    ps->last_update_us = now_us;

    // Source-agnostic rollup: an injected 15-min result feeds the SAME universal
    // hierarchy (1h → day → ...) as native units. finalize_15min_period does this for
    // native units; rbAmp units skip that path (their 15-min accumulator is disabled),
    // so trigger the shared rollup here.
    if (period == RBPOWER_STAT_PERIOD_15MIN) {
        // Feed the LIFETIME + daily energy accumulators (telemetry e_in/e_out and the HA Energy
        // Dashboard read these). Injected (rbAmp/ESP-NOW) units have no 200ms-finalize path, so add
        // each finished 15-min period's energy here — ONCE, on the atomic 15-min inject (higher
        // periods are aggregations, never re-add). The telemetry reads the SPECIALIZED accumulator
        // for supply units (main/alt) but the per-unit basic one for regular load channels — feed
        // the matching location (otherwise e_in/e_out stay 0).
        rbpower_home_unit_t* eu = &g_home_network.units[unit_index];
        rbpower_load_role_t erole = eu->definition.role;
        /* et_in/et_out (double mWh) feed: these exact totalizers ride
         * into every v4 record (set_meta.et_*); the only other feeder (3-sec lifetime path) is
         * disabled, so without this they stayed frozen at the boot-restore seed. */
        double e_mwh = (double)energy_wh * 1000.0;
        /* etstat diagnostics: per-index feed counter + wall-clock of the last feed;
         * on the first feed capture the boot-restore seed (totalizer value BEFORE we add). */
        if (unit_index < RBPOWER_MAX_HOME_UNITS) {
            if (s_et_diag[unit_index].feeds == 0) {
                rbpower_lifetime_get_totalizers(eu->definition.unit_id,
                                                &s_et_diag[unit_index].boot_seed_in_mwh,
                                                &s_et_diag[unit_index].boot_seed_out_mwh);
            }
            s_et_diag[unit_index].feeds++;
            s_et_diag[unit_index].last_feed_ts = time(NULL);
        }
        if (erole == RBPOWER_ROLE_MAIN_SUPPLY && g_home_network.specialized.has_main_supply) {
            rbpower_main_supply_t* ms = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
            if (ms) {
                if (energy_wh >= 0.0f) { ms->lifetime_accumulators.total_energy_consumed_wh += energy_wh;
                                         ms->lifetime_accumulators.daily_energy_consumed_wh += energy_wh;
                                         ms->lifetime_accumulators.et_in_mwh += e_mwh; }
                else { ms->lifetime_accumulators.total_energy_supplied_wh += -energy_wh;
                       ms->lifetime_accumulators.daily_energy_supplied_wh += -energy_wh;
                       ms->lifetime_accumulators.et_out_mwh += -e_mwh; }
            }
        } else if (erole == RBPOWER_ROLE_ALTERNATIVE_SUPPLY && g_home_network.specialized.has_alternative_supply) {
            rbpower_alternative_supply_t* as = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
            if (as) {
                if (energy_wh >= 0.0f) { as->lifetime_accumulators.total_energy_consumed_wh += energy_wh;
                                         as->lifetime_accumulators.daily_energy_consumed_wh += energy_wh;
                                         as->lifetime_accumulators.et_in_mwh += e_mwh; }
                else { as->lifetime_accumulators.total_energy_supplied_wh += -energy_wh;
                       as->lifetime_accumulators.daily_energy_supplied_wh += -energy_wh;
                       as->lifetime_accumulators.et_out_mwh += -e_mwh; }
            }
        } else {
            /* Regular load channel — basic accumulator (consumed only; loads don't generate).
             * NOTE: a SUPPLY-role unit WITHOUT its specialized struct (I2C-fed unit) also lands
             * here; the lifetime module reads/persists its basic accumulator, which carries both
             * import (et_in) and export (et_out) totalizers. */
            rbpower_lifetime_accumulators_basic_t* la = &eu->statistics.lifetime_accumulators;
            if (energy_wh >= 0.0f) { la->total_energy_consumed_wh += energy_wh;
                                     la->daily_energy_consumed_wh += energy_wh;
                                     la->et_in_mwh += e_mwh; }
            else { la->total_energy_supplied_wh += -energy_wh;      /* generation / export of a regular point */
                   la->daily_energy_supplied_wh += -energy_wh;
                   la->et_out_mwh += -e_mwh; }
        }

        /* Persist the totalizers: RTC image every feed (survives a warm reboot), throttled NVS
         * snapshot inside (survives a power cut). The historical auto-sync lived
         * in the 3-sec lifetime path, which is disabled — without this call nothing on the inject
         * path syncs, and a restored module would have no image to restore FROM.
         * sync_all snapshots every unit, so one call per inject covers the whole network. */
        (void)rbpower_lifetime_sync_to_rtc();

        /* Cost the injected 15-min energy with the current tariff BEFORE the rollup, so result_base
         * economics (cost_consumed/revenue_supplied) propagate up the hierarchy. The 3-sec finalize
         * path does its own costing; the inject path bypasses it, so without this the economics are 0.
         * Zone-aware buying/selling rate applied at the inject instant. */
        rbpower_tariff_cost_injected_period(unit_index, energy_wh, &ps->result_base);

        /* The unit's period accumulators are enabled per its configured depth (stat_class) at creation
         * time in rbpower_home_create_unit — no force-enable needed here. should_aggregate_to_period()
         * inside the rollup now correctly reflects the configured depth. */
        uint32_t q_end = s_next_qend[unit_index];   /* the boundary this quarter was collected for (0 = unknown) */
        s_next_qend[unit_index] = 0;
        rbpower_rollup_after_15min_at(unit_index, ps, q_end);
    }

    rbpower_home_unlock();
    return RBPOWER_OK;
}

/* Force-finalize an accumulated period for a unit BY REAL HEX ID (test/diagnostics: persist a HOUR record
 * now instead of waiting for the wall-clock boundary). Resolves the id→index (the finalize path is
 * index-based) then runs the same finalize the timer uses → store + rollup. For rbAmp units feed the
 * 1HOUR accumulator first via collect_period (period verb), then call this with period=1HOUR. */
rbpower_err_t rbpower_force_finalize_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period)
{
    if (!g_home_network_initialized) return RBPOWER_ERR_NOT_INIT;
    if (period >= RBPOWER_STAT_PERIOD_COUNT) return RBPOWER_ERR_INVALID_ARG;
    uint8_t idx = find_unit_index_by_id(unit_id);
    if (idx == 0xFF || idx >= RBPOWER_MAX_HOME_UNITS) return RBPOWER_ERR_INVALID_ARG;
    // FORCED variant: this is the test/diagnostic path (statforce1h / forceperiod) whose documented job is
    // to STORE a record NOW. The plain callback would early-return without storing when the period
    // hasn't reached the auto-threshold (e.g. HOUR needs >=3 fifteen-min rollups) yet still report
    // success. The forced variant arms readiness so the store actually happens.
    if (!rbpower_finalize_period_callback_forced((rbpower_unit_id_t)idx, period, (uint64_t)esp_timer_get_time()))
        return RBPOWER_ERR_INVALID_STATE;   /* no inputs accumulated — nothing to store (module absent?) */
    return RBPOWER_OK;
}

/*
 * @brief Statistics API test function (for debugging)
 * Call manually to verify aggregation operation
 */
void rbpower_test_aggregation_api(void)
{
    ESP_LOGI("RBHOME-TEST", "=== AGGREGATION API TEST ===");
    
    // Check all units and periods
    for (uint8_t unit_id = 0; unit_id < g_home_network.units_count; unit_id++) {
        if (!g_home_network.units[unit_id].has_valid_definition) continue;
        
        ESP_LOGI("RBHOME-TEST", "Unit[%d] '%s':", 
                 unit_id, g_home_network.units[unit_id].definition.name);
        
        for (rbpower_stat_period_type_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
            float completeness = rbpower_get_period_completeness(unit_id, period);
            bool ready = rbpower_is_period_ready(unit_id, period);
            
            if (completeness > 0) {
                rbpower_period_statistics_t stats;
                rbpower_err_t err = rbpower_get_unit_period_statistics(unit_id, period, &stats);
                
                if (err == RBPOWER_OK && stats.period_complete) {
                    ESP_LOGI("RBHOME-TEST", "  Period %d: ready=%s, completeness=%.1f%%, energy=%.1fWh",
                             period, ready ? "YES" : "NO", completeness, stats.energy_period.consumed_wh);
                } else {
                    ESP_LOGI("RBHOME-TEST", "  Period %d: accumulating, completeness=%.1f%%", period, completeness);
                }
            }
        }
    }
    
    ESP_LOGI("RBHOME-TEST", "=== END TEST ===");
}