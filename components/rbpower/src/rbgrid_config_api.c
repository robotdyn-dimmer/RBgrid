/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_config_api.c
 * @brief Implementation of the RBgrid configuration API.
 *
 * Holds the boot-time configuration builder (an opaque context that collects the hardware config,
 * unit definitions, time and telemetry settings before the home network is instantiated), the
 * tariff-plan builder, runtime unit bind/rebind and point-slug management, read-only accessors for
 * the real-time (200 ms) and snapshot (3 s) data, the orderly system reboot and the JSON export of
 * flash-stored history records.
 *
 * Measurement comes from rbAmp I2C modules and ESP-NOW nodes. The ADC-typed helpers kept here
 * (rbgrid_config_add_adc_sensor, set_voltage_bus, set_main_supply, set_alternative_supply,
 * add_unit) no longer sample anything: they only create the legacy ADC-typed unit definitions of the
 * default units, which the rbAmp bridge then feeds.
 */

#include "rbgrid_config_api.h"
#include <inttypes.h>
#include "rbpower.h"
#include "rbgrid.h"
#include "rbhome_types.h"
#include "rbhome_main_supply.h"
#include "rbhome_alt_supply.h"
#include "rbpower_constants.h"
#include "rbextflash_operations.h"
#include "rbsensor_profiles.h"
#include "rbsensors.h"
#include "rbgrid_config_storage.h"   /* rbgrid_storage_load_rbamp — bind/rebind recover the CT model */
#include "rbpoint.h"                 /* point slugs + caps mapping */
#include "rbmqtt.h"                  /* LWT-on-reboot: explicit offline before esp_restart */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"   /* reboot hard-fallback one-shot */
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
static const char* TAG = "RBGRID_CONFIG";

// ========== EXTERNAL FUNCTIONS ==========

/* 
 * @brief Look up the slot index of a unit in the live home network (defined in rbhome.c).
 * @param[in] unit_id Unit ID to look up.
 * @return Index into g_home_network.units, or 0xFF when no unit has this ID.
 */
extern uint8_t find_unit_index_by_id(rbpower_unit_id_t unit_id);

// ========== INTERNAL STRUCTURES ==========

/**
 * @brief Maximum number of units accepted by rbgrid_config_add_unit().
 */
#define RBGRID_CONFIG_MAX_UNITS 32

/**
 * @brief Configuration builder context behind rbgrid_config_handle_t; fills the live rbpower structures directly.
 */
typedef struct rbgrid_config_context_s {
    // Main working structures
    rbpower_extended_config_t* extended_config;  ///< Main (extended) configuration, heap-allocated
    rbpower_hardware_config_t* hardware_config;  ///< Hardware configuration, heap-allocated
    rbpower_electrical_network_t* network;       ///< Electrical network (optional)
    
    // Dynamic unit array (grown on demand)
    rbpower_unit_definition_t* units_array;      ///< Array of unit definitions (realloc-grown)
    uint16_t units_allocated;                    ///< Allocated capacity of units_array, in entries
    uint16_t units_count;                        ///< Number of units currently defined
    
    // Telemetry configuration
    struct {
        bool configured;                         ///< Telemetry has been configured
        rbpower_telemetry_config_t config;       ///< Telemetry configuration
    } telemetry;                                ///< Telemetry settings collected by the builder
    
    // Scratch data used while building the configuration
    struct {
        int next_sensor_id;                      ///< Next sensor ID
        int next_unit_id;                       ///< Next unit ID
        uint8_t adc_channels_used;              ///< Number of legacy ADC channels in use
        bool has_voltage_bus;                   ///< A voltage bus is defined
        bool has_main_supply;                   ///< A main supply is defined
        bool has_alternative_supply;            ///< An alternative supply is defined
    } builder;                                  ///< Builder scratch state
    
    // Component readiness flags
    struct {
        bool base_initialized;                  ///< Base configuration ready
        bool network_configured;                ///< Network configured
        bool sensors_configured;                ///< Sensors configured
        bool units_configured;                  ///< Units configured
        bool telemetry_configured;              ///< Telemetry configured
    } status;                                   ///< Component readiness flags
    
    // State flags
    bool is_initialized;                        ///< Context initialized
    bool is_applied;                           ///< Configuration applied
    
} rbgrid_config_context_t;

// ========== GPIO->ADC MAPPING (legacy) ==========

/**
 * @brief Legacy ESP32 GPIO -> ADC1 channel table.
 * Used only to derive the ADC-typed unit_UID of the legacy default units; no native ADC sampling
 * remains. ADC1 channels only (ADC2 conflicts with Wi-Fi).
 */
static const struct {
    gpio_num_t gpio;
    uint8_t adc_channel;
} gpio_adc_map[] = {
    {GPIO_NUM_36, 0},  // ADC1_CHANNEL_0
    {GPIO_NUM_37, 1},  // ADC1_CHANNEL_1
    {GPIO_NUM_38, 2},  // ADC1_CHANNEL_2
    {GPIO_NUM_39, 3},  // ADC1_CHANNEL_3
    {GPIO_NUM_32, 4},  // ADC1_CHANNEL_4
    {GPIO_NUM_33, 5},  // ADC1_CHANNEL_5
    {GPIO_NUM_34, 6},  // ADC1_CHANNEL_6
    {GPIO_NUM_35, 7},  // ADC1_CHANNEL_7
};

/**
 * @brief Map a GPIO to its ADC1 channel; returns the channel (0..7) or -1 if the GPIO has no ADC1 channel.
 */
static int gpio_to_adc_channel(gpio_num_t gpio_pin)
{
    for (int i = 0; i < sizeof(gpio_adc_map) / sizeof(gpio_adc_map[0]); i++) {
        if (gpio_adc_map[i].gpio == gpio_pin) {
            return gpio_adc_map[i].adc_channel;
        }
    }
    return -1; // GPIO has no ADC1 channel
}

/**
 * @brief True when the GPIO is an ADC1 pin.
 */
static bool is_adc1_gpio(gpio_num_t gpio_pin)
{
    return gpio_to_adc_channel(gpio_pin) >= 0;
}

// ========== DEFAULT SENSOR PARAMETERS ==========

/**
 * @brief Fill default scale, noise threshold, full-scale value and rbsensor type for a sensor model.
 */
static void get_sensor_defaults(rbgrid_sensor_model_t model, 
                                float* scale_factor, 
                                float* noise_threshold,
                                float* max_value,
                                rbsensor_type_t* rbsensor_type)
{
    switch (model) {
        // Voltage sensors
        case RBGRID_SENSOR_ZMPT101B:
            *scale_factor = 0.150f;
            *noise_threshold = 15.0f;
            *max_value = 250.0f;
            *rbsensor_type = RBSENSOR_TYPE_VOLTAGE_ZMPT101B;
            break;
            
        case RBGRID_SENSOR_ZMPT107_1:
            *scale_factor = 0.187f;
            *noise_threshold = 15.0f;
            *max_value = 250.0f;
            *rbsensor_type = RBSENSOR_TYPE_VOLTAGE_ZMPT107_1;
            break;
            
        case RBGRID_SENSOR_ZMPT116B:
            *scale_factor = 0.200f;
            *noise_threshold = 15.0f;
            *max_value = 250.0f;
            *rbsensor_type = RBSENSOR_TYPE_VOLTAGE_ZMPT101B;  // Reuse the existing type
            break;
            
        // SCT013 current sensors
        case RBGRID_SENSOR_SCT013_10A:
            *scale_factor = 0.0049f;
            *noise_threshold = 10.0f;
            *max_value = 10.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_SCT013_10A;
            break;
            
        case RBGRID_SENSOR_SCT013_20A:
            *scale_factor = 0.0098f;
            *noise_threshold = 10.0f;
            *max_value = 20.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_SCT013_20A;
            break;
            
        case RBGRID_SENSOR_SCT013_30A:
            *scale_factor = 0.0147f;
            *noise_threshold = 10.0f;
            *max_value = 30.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_SCT013_30A;
            break;
            
        case RBGRID_SENSOR_SCT013_50A:
            *scale_factor = 0.0245f;
            *noise_threshold = 10.0f;
            *max_value = 50.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_SCT013_50A;
            break;
            
        case RBGRID_SENSOR_SCT013_100A:
            *scale_factor = 0.0490f;
            *noise_threshold = 10.0f;
            *max_value = 100.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_SCT013_100A;
            break;
            
        // ACS712 current sensors
        case RBGRID_SENSOR_ACS712_5A:
            *scale_factor = 0.185f;
            *noise_threshold = 5.0f;
            *max_value = 5.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_ACS712_5A;
            break;
            
        case RBGRID_SENSOR_ACS712_20A:
            *scale_factor = 0.100f;
            *noise_threshold = 5.0f;
            *max_value = 20.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_ACS712_20A;
            break;
            
        case RBGRID_SENSOR_ACS712_30A:
            *scale_factor = 0.066f;
            *noise_threshold = 5.0f;
            *max_value = 30.0f;
            *rbsensor_type = RBSENSOR_TYPE_CURRENT_ACS712_30A;
            break;
            
        default:
            *scale_factor = 1.0f;
            *noise_threshold = 10.0f;
            *max_value = 100.0f;
            *rbsensor_type = RBSENSOR_TYPE_UNKNOWN;  // UNKNOWN instead of CUSTOM
            break;
    }
}

// ========== MAIN FUNCTIONS ==========

rbgrid_config_handle_t rbgrid_config_create(const char* home_name, const char* location)
{
    if (!home_name || !location) {
        ESP_LOGE(TAG, "Invalid parameters: home_name and location are required");
        return NULL;
    }
    
    // Initialize the external flash with OTP validation (first call only)
    static bool flash_init_attempted = false;
    if (!flash_init_attempted) {
        flash_init_attempted = true;
        esp_err_t ret = rbpower_external_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "External Flash not available (code: %d), using RAM only mode", ret);
            rbpower_flash_status_t status = rbpower_external_flash_get_status();
            if (!status.otp_validated) {
                ESP_LOGE(TAG, "SECURITY WARNING: Device authentication failed!");
            }
            if (status.error_message) {
                ESP_LOGW(TAG, "Flash error: %s", status.error_message);
            }
        } else {
            rbpower_flash_status_t status = rbpower_external_flash_get_status();
            ESP_LOGI(TAG, "Configuration storage available:");
            ESP_LOGI(TAG, "  - OTP validated: %s", status.otp_validated ? "YES" : "NO");
            ESP_LOGI(TAG, "  - Device serial: 0x%08" PRIx32, status.device_serial);
            ESP_LOGI(TAG, "  - Config partition: %s", 
                    status.config_partition_mounted ? "MOUNTED" : "NOT MOUNTED");
            if (status.config_partition_mounted) {
                ESP_LOGI(TAG, "  - Config size: %d KB", status.config_partition_size / 1024);
            }
        }
    }
    
    // Allocate the context
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)calloc(1, sizeof(rbgrid_config_context_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate memory for config context");
        return NULL;
    }
    
    // Allocate extended_config
    ctx->extended_config = (rbpower_extended_config_t*)calloc(1, sizeof(rbpower_extended_config_t));
    if (!ctx->extended_config) {
        ESP_LOGE(TAG, "Failed to allocate memory for extended_config");
        free(ctx);
        return NULL;
    }
    
    // Allocate hardware_config
    ctx->hardware_config = (rbpower_hardware_config_t*)calloc(1, sizeof(rbpower_hardware_config_t));
    if (!ctx->hardware_config) {
        ESP_LOGE(TAG, "Failed to allocate memory for hardware_config");
        free(ctx->extended_config);
        free(ctx);
        return NULL;
    }
    
    // Link the structures
    ctx->extended_config->base_hardware_config = ctx->hardware_config;
    
    // Base extended_config parameters
    ctx->extended_config->enable_home_network = true;
    ctx->extended_config->home_name = strdup(home_name);  // Own copy of the string
    ctx->extended_config->location = strdup(location);     // Own copy of the string
    ctx->extended_config->language = "";                  // "" = not set: the installer chooses the language; until then the UI uses the browser language
    
    // Default rbhome settings
    ctx->extended_config->rbhome_settings.enable_default_thresholds = true;
    ctx->extended_config->rbhome_settings.statistics_update_mask = 0xFFFFFFFF;
    ctx->extended_config->rbhome_settings.logging_level = RBGRID_LOG_LEVEL_INFO;
    ctx->extended_config->rbhome_settings.enable_logging = true;
    
    // Default telemetry settings
    ctx->extended_config->rbhome_telemetry_settings.enable_telemetry = false;
    ctx->extended_config->rbhome_telemetry_settings.max_retry_attempts = 3;
    ctx->extended_config->rbhome_telemetry_settings.retry_delay_ms = 1000;
    
    // Default JSON settings
    ctx->extended_config->rbhome_JSON_settings.enable_json_api = true;
    ctx->extended_config->rbhome_JSON_settings.enable_JSON_compression = false;
    ctx->extended_config->rbhome_JSON_settings.enable_encryption = false;
    
    // Telemetry configuration: settings from the context, or defaults
    if (ctx->telemetry.configured) {
        // Copy telemetry settings from the context
        memcpy(&ctx->extended_config->telemetry_config, &ctx->telemetry.config, 
               sizeof(rbpower_telemetry_config_t));
    } else {
        // Default telemetry configuration
        ctx->extended_config->telemetry_config.telemetry_enabled = true;
        ctx->extended_config->telemetry_config.json_buffer_size = 4096;
        ctx->extended_config->telemetry_config.compact_json = false;
        
        // Telemetry channels (all disabled by default)
        for (int i = 0; i < RBPOWER_TELEMETRY_PROTOCOLS_MAX; i++) {
            ctx->extended_config->telemetry_config.channels[i].enabled = false;
            ctx->extended_config->telemetry_config.channels[i].send_interval_ms = 5000;
            ctx->extended_config->telemetry_config.channels[i].retry_count = 3;
            ctx->extended_config->telemetry_config.channels[i].timeout_ms = 5000;
        }
    }
    
    // Base hardware_config parameters (legacy ADC block kept for structure compatibility; no native ADC sampling)
    ctx->hardware_config->adc.sampling_frequency = 20000;  // legacy field, unused
    ctx->hardware_config->adc.adc_resolution = 12;         // 12 bit (legacy field)
    ctx->hardware_config->adc.adc_attenuation = 3;         // 11dB attenuation
    ctx->hardware_config->adc.channels_count = 0;          // no channels yet
    
    ctx->hardware_config->calculation_period_ms = 100;     // 100 ms calculation period
    ctx->hardware_config->averaging_cycles = 1;            // no averaging
    ctx->hardware_config->enable_harmonics = false;        // harmonics disabled
    ctx->hardware_config->enable_debug_logging = true;     // debug logging enabled
    
    // External flash is now initialized automatically through external_flash_init()
    // The external_flash field is a structure, not a pointer, so it's already initialized with zeros
    
    // Legacy ADC sensor slots (all disabled)
    ctx->hardware_config->adc_sensors.enable_filtering = false;
    ctx->hardware_config->adc_sensors.filter_samples = 1;
    for (int i = 0; i < RBPOWER_MAX_CHANNELS; i++) {
        ctx->hardware_config->adc_sensors.channels[i].enabled = false;
    }
    
    // Default time configuration
    ctx->hardware_config->time_config.enable_time_sync = false;
    ctx->hardware_config->time_config.primary_source = RBTIME_SOURCE_MANUAL;
    ctx->hardware_config->time_config.force_sync_on_startup = false;
    
    // Builder scratch data
    ctx->builder.next_sensor_id = 0;
    ctx->builder.next_unit_id = 1;  // unit IDs start at 1
    ctx->builder.adc_channels_used = 0;
    ctx->builder.has_voltage_bus = false;
    ctx->builder.has_main_supply = false;
    ctx->builder.has_alternative_supply = false;
    
    // Readiness flags
    ctx->status.base_initialized = true;
    ctx->status.network_configured = false;
    ctx->status.sensors_configured = false;
    ctx->status.units_configured = false;
    
    ctx->is_initialized = true;
    ctx->is_applied = false;
    
    // Empty dynamic unit array
    ctx->units_array = NULL;
    ctx->units_allocated = 0;
    ctx->units_count = 0;
    
    ESP_LOGI(TAG, "Config context created: home='%s', location='%s' with direct rbpower structures", 
             home_name, location);
    
    return (rbgrid_config_handle_t)ctx;
}

void rbgrid_config_destroy(rbgrid_config_handle_t config)
{
    if (!config) return;
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    // Free the configuration allocations
    if (ctx->extended_config) {
        free(ctx->extended_config);
    }
    if (ctx->hardware_config) {
        free(ctx->hardware_config);
    }
    
    free(ctx);
    ESP_LOGI(TAG, "Config context destroyed");
}

int rbgrid_config_set_network(rbgrid_config_handle_t config)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->status.network_configured) {
        ESP_LOGW(TAG, "Network already configured");
        return RBGRID_CONFIG_ERR_ALREADY_SET;
    }
    
    // Network parameters in hardware_config
    ctx->hardware_config->network.nominal_frequency = 50.0f;  // default 50 Hz
    ctx->hardware_config->network.nominal_voltage = 230.0f;   // default 230 V
    
    ctx->status.network_configured = true;
    ESP_LOGI(TAG, "Network configured (freq=%.1fHz, voltage=%.1fV)",
             (float)ctx->hardware_config->network.nominal_frequency,
             (float)ctx->hardware_config->network.nominal_voltage);
    
    return RBGRID_CONFIG_OK;
}

// ========== LEGACY ADC-TYPED SENSOR BINDING ==========
// No native ADC sampling remains. These calls only register an ADC-typed slot so that the legacy
// default units get their ADC-typed unit_UID; the rbAmp bridge feeds those units.

rbgrid_unit_uid_t rbgrid_config_add_adc_sensor(rbgrid_config_handle_t config,
                                               gpio_num_t gpio_pin,
                                               rbsensor_type_t sensor_type)
{
    if (!config) {
        return RBGRID_INVALID_UNIT_UID;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_INVALID_UNIT_UID;
    }
    
    // Validate the GPIO
    if (gpio_pin >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid GPIO pin: %d", gpio_pin);
        return RBGRID_INVALID_UNIT_UID;
    }
    
    // The GPIO must map to an ADC1 channel
    int adc_channel = gpio_to_adc_channel(gpio_pin);
    if (adc_channel < 0) {
        ESP_LOGE(TAG, "GPIO%d does not support ADC1", gpio_pin);
        return RBGRID_INVALID_UNIT_UID;
    }
    
    // ADC channel limit
    if (ctx->hardware_config->adc.channels_count >= RBPOWER_MAX_CHANNELS) {
        ESP_LOGE(TAG, "ADC channel limit reached (%d)", RBPOWER_MAX_CHANNELS);
        return RBGRID_INVALID_UNIT_UID;
    }
    
    // Reject a duplicate channel
    for (int i = 0; i < ctx->hardware_config->adc.channels_count; i++) {
        if (ctx->hardware_config->adc.channels[i] == adc_channel) {
            ESP_LOGE(TAG, "ADC channel %d (GPIO%d) already in use", adc_channel, gpio_pin);
            return RBGRID_INVALID_UNIT_UID;
        }
    }
    
    // Add the channel to hardware_config->adc
    uint8_t channel_idx = ctx->hardware_config->adc.channels_count;
    ctx->hardware_config->adc.channels[channel_idx] = adc_channel;
    ctx->hardware_config->adc.channels_count++;
    
    // Configure the sensor slot in hardware_config->adc_sensors from its profile
    rbpower_sensor_channel_config_t* sensor_cfg = &ctx->hardware_config->adc_sensors.channels[channel_idx];

    // Look up the sensor preset in rbsensor_profiles
    const rbsensor_preset_t* preset = rbsensor_get_preset_by_type(sensor_type);
    if (preset) {
        // Use the preset parameters
        sensor_cfg->enabled = true;
        sensor_cfg->type = sensor_type;
        strncpy(sensor_cfg->name, preset->name, sizeof(sensor_cfg->name) - 1);
        sensor_cfg->name[sizeof(sensor_cfg->name) - 1] = '\0';
        sensor_cfg->scale_factor = preset->profile.source_params.adc_params.scale_factor;
        sensor_cfg->zero_offset = preset->profile.source_params.adc_params.zero_offset;
        sensor_cfg->noise_threshold = preset->profile.source_params.adc_params.noise_threshold;
        sensor_cfg->min_value = preset->profile.source_params.adc_params.min_value;
        sensor_cfg->max_value = preset->profile.source_params.adc_params.max_value;
        sensor_cfg->linearity_correction = preset->profile.source_params.adc_params.linearity_correction;
    } else {
        // No preset: use defaults
        sensor_cfg->enabled = true;
        sensor_cfg->type = sensor_type;
        strncpy(sensor_cfg->name, "Custom Sensor", sizeof(sensor_cfg->name) - 1);
        sensor_cfg->name[sizeof(sensor_cfg->name) - 1] = '\0';
        sensor_cfg->scale_factor = 1.0f;
        sensor_cfg->zero_offset = 0.0f;
        sensor_cfg->noise_threshold = 10.0f;
        sensor_cfg->min_value = 0.0f;
        sensor_cfg->max_value = 100.0f;
        sensor_cfg->linearity_correction = 1.0f;
    }
    
    // Update builder counters
    ctx->builder.adc_channels_used++;
    ctx->status.sensors_configured = true;
    
    // Build the unit_UID via rbgrid.c
    rbgrid_unit_uid_t unit_uid = rbgrid_create_adc_unit_id(adc_channel);
    if (unit_uid == RBGRID_INVALID_UNIT_UID) {
        ESP_LOGE(TAG, "Failed to create unit_UID for ADC channel %d", adc_channel);
        return RBGRID_INVALID_UNIT_UID;
    }
    
    ESP_LOGI(TAG, "ADC sensor added: unit_UID=0x%04X, GPIO%d (CH%d), type=%d (%s)", 
             unit_uid, gpio_pin, adc_channel, sensor_type, sensor_cfg->name);
    
    return unit_uid;  // return the unit_UID
}
// ========== UNIT CONFIGURATION ==========

/** @brief Collision guard. Returns the index of an existing unit with this id in the builder
 * array, or -1 if the id is free. Two units sharing a unit_id corrupt statistics (data accumulates
 * to the wrong unit) and collide on the same flash STATS slot — so every add path rejects a dup. */
static int rbgrid_find_unit_in_array(rbgrid_config_context_t* ctx, rbgrid_unit_id_t unit_id)
{
    if (!ctx || !ctx->units_array) return -1;
    for (uint16_t i = 0; i < ctx->units_count; i++) {
        if (ctx->units_array[i].unit_id == unit_id) return (int)i;
    }
    return -1;
}

/* ================================================================================================
 * Point tombstones + slug helpers
 * ================================================================================================ */

static char    s_point_tombstones[RBPOINT_TOMBSTONE_MAX][RBPOINT_SLUG_MAX + 1];
static uint8_t s_point_tombstone_count = 0;

bool rbgrid_point_tombstoned(const char* slug)
{
    if (!slug || !slug[0]) return false;
    for (uint8_t i = 0; i < s_point_tombstone_count; i++) {
        if (strcmp(s_point_tombstones[i], slug) == 0) return true;
    }
    return false;
}

void rbgrid_point_tombstone_add(const char* slug)
{
    if (!slug || !slug[0] || rbgrid_point_tombstoned(slug)) return;
    if (s_point_tombstone_count >= RBPOINT_TOMBSTONE_MAX) {
        /* evict the oldest: its reuse protection ages out */
        memmove(&s_point_tombstones[0], &s_point_tombstones[1],
                (RBPOINT_TOMBSTONE_MAX - 1) * sizeof(s_point_tombstones[0]));
        s_point_tombstone_count = RBPOINT_TOMBSTONE_MAX - 1;
    }
    strncpy(s_point_tombstones[s_point_tombstone_count], slug, RBPOINT_SLUG_MAX);
    s_point_tombstones[s_point_tombstone_count][RBPOINT_SLUG_MAX] = '\0';
    s_point_tombstone_count++;
}

bool rbgrid_point_tombstone_remove(const char* slug)
{
    for (uint8_t i = 0; i < s_point_tombstone_count; i++) {
        if (strcmp(s_point_tombstones[i], slug) == 0) {
            memmove(&s_point_tombstones[i], &s_point_tombstones[i + 1],
                    (s_point_tombstone_count - i - 1) * sizeof(s_point_tombstones[0]));
            s_point_tombstone_count--;
            return true;
        }
    }
    return false;
}

int rbgrid_point_tombstones_get(char out[][33], int max)
{
    int n = (s_point_tombstone_count < max) ? s_point_tombstone_count : max;
    for (int i = 0; i < n; i++) strcpy(out[i], s_point_tombstones[i]);
    return n;
}

/** @brief rbpoint_slug_uniquify() predicate: slug is taken if a LIVE unit holds it or it is tombstoned. */
static bool point_taken_live(const char* slug, void* arg)
{
    (void)arg;
    return rbpower_home_find_unit_by_point(slug) != RBPOWER_HOME_INVALID_ID ||
           rbgrid_point_tombstoned(slug);
}

/** @brief rbpoint_slug_uniquify() predicate for the boot builder: scan ctx->units_array (live network not up yet). */
static bool point_taken_builder(const char* slug, void* arg)
{
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)arg;
    if (ctx && ctx->units_array) {
        for (uint16_t i = 0; i < ctx->units_count; i++) {
            if (strcmp(ctx->units_array[i].point, slug) == 0) return true;
        }
    }
    return rbgrid_point_tombstoned(slug);
}

bool rbgrid_config_builder_has_source(rbgrid_config_handle_t config,
                                      rbpower_source_type_t source_type,
                                      uint64_t device_uid, uint8_t channel_idx)
{
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    if (!ctx || !ctx->units_array) return false;
    for (uint16_t i = 0; i < ctx->units_count; i++) {
        const rbpower_unit_definition_t* u = &ctx->units_array[i];
        if (u->sources_count > 0 && u->sources[0].source_type == source_type &&
            u->device_uid == device_uid && u->channel_idx == channel_idx) return true;
    }
    return false;
}

int rbgrid_config_set_voltage_bus(rbgrid_config_handle_t config,
                                  int sensor_unit_uid,
                                  const char* name)
{
    if (!config || !name) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->builder.has_voltage_bus) {
        ESP_LOGW(TAG, "Voltage bus already configured");
        return RBGRID_CONFIG_ERR_ALREADY_SET;
    }
    
    // Must be an ADC-typed unit_UID
    rbgrid_unit_id_t unit_uid = (rbgrid_unit_id_t)sensor_unit_uid;
    rbgrid_unit_type_t unit_type = rbgrid_get_unit_type(unit_uid);
    if (unit_type != RBGRID_UNIT_TYPE_ADC) {
        ESP_LOGE(TAG, "unit_UID 0x%04X is not ADC type (type=%d)", unit_uid, unit_type);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Extract the ADC channel from the unit_UID
    uint8_t adc_channel = RBGRID_GET_LOCAL(unit_uid);
    
    // Find the channel index in the array
    int sensor_id = -1;
    for (int i = 0; i < ctx->hardware_config->adc.channels_count; i++) {
        if (ctx->hardware_config->adc.channels[i] == adc_channel) {
            sensor_id = i;
            break;
        }
    }
    
    if (sensor_id < 0) {
        ESP_LOGE(TAG, "ADC channel %d from unit_UID 0x%04X not found in configuration", 
                 adc_channel, unit_uid);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Check the sensor slot configuration
    rbpower_sensor_channel_config_t* sensor_cfg = &ctx->hardware_config->adc_sensors.channels[sensor_id];
    if (!sensor_cfg->enabled) {
        ESP_LOGE(TAG, "Sensor channel %d is not enabled", sensor_id);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Must be a voltage sensor
    bool is_voltage = false;
    rbsensor_type_t sensor_type = sensor_cfg->type;
    if (sensor_type >= RBSENSOR_TYPE_VOLTAGE_ZMPT101B && 
        sensor_type <= RBSENSOR_TYPE_VOLTAGE_CUSTOM) {
        is_voltage = true;
    }
    
    if (!is_voltage) {
        ESP_LOGE(TAG, "Sensor channel %d (type=%d) is not a voltage sensor", sensor_id, sensor_type);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }

    // Reject a duplicate unit_id (corrupts stats + collides on the flash slot).
    if (rbgrid_find_unit_in_array(ctx, unit_uid) >= 0) {
        ESP_LOGE(TAG, "unit_UID 0x%04X already registered — duplicate voltage_bus rejected", unit_uid);
        return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
    }

    // Allocate or grow the unit array if needed
    if (ctx->units_count >= ctx->units_allocated) {
        uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
        rbpower_unit_definition_t* new_array = (rbpower_unit_definition_t*)realloc(
            ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
        if (!new_array) {
            ESP_LOGE(TAG, "Failed to allocate memory for units");
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }
        ctx->units_array = new_array;
        ctx->units_allocated = new_size;
    }
    
    // Unit definition for the voltage bus
    rbpower_unit_definition_t* vbus_unit = &ctx->units_array[ctx->units_count];
    memset(vbus_unit, 0, sizeof(rbpower_unit_definition_t));
    
    // Basic parameters
    vbus_unit->unit_id = unit_uid;  // the unit_UID is the unit ID
    strncpy(vbus_unit->name, name, sizeof(vbus_unit->name) - 1);
    strncpy(vbus_unit->location, "Main Panel", sizeof(vbus_unit->location) - 1);
    vbus_unit->type = RBPOWER_METRIC_VOLTAGE_BUS;  // the library creates an rbpower_voltage_bus_t
    vbus_unit->role = RBPOWER_ROLE_MAIN_SUPPLY;
    vbus_unit->stat_class = RBPOWER_STAT_PERIOD_1HOUR;
    
    // Data source: legacy ADC-typed binding
    vbus_unit->sources[0].source_type = RBPOWER_SOURCE_ADC_SENSOR;
    vbus_unit->sources[0].source_id = adc_channel; // ADC channel (from the unit_UID)
    vbus_unit->sources[0].sensor_profile_id = sensor_id; // index into the sensor slots
    vbus_unit->sources[0].weight_factor = 1.0f;
    vbus_unit->sources[0].is_primary = true;
    vbus_unit->sources[0].invert_sign = false;
    vbus_unit->sources_count = 1;
    
    // Hierarchy
    vbus_unit->parent_unit_id = RBPOWER_HOME_INVALID_ID;
    vbus_unit->children_count = 0;
    
    // Voltage bus parameters
    vbus_unit->bidirectional_metering = false;
    vbus_unit->accumulate_energy = false;
    vbus_unit->rated_power_w = 0.0f;
    vbus_unit->max_current_a = 0.0f;
    vbus_unit->rated_voltage_v = 230.0f;
    
    // Voltage thresholds
    vbus_unit->thresholds.under_voltage_v = 207.0f;
    vbus_unit->thresholds.over_voltage_v = 253.0f;
    vbus_unit->thresholds.enable_alerts = true;
    
    // Billing
    vbus_unit->billing.cost_per_kwh = 0.0f;
    vbus_unit->billing.time_of_use_enabled = false;
    
    vbus_unit->is_active = true;
    vbus_unit->created_timestamp_us = 0;
    
    ctx->units_count++;
    ctx->builder.has_voltage_bus = true;
    ctx->status.units_configured = true;
    
    ESP_LOGI(TAG, "Voltage bus configured: name='%s', unit_UID=0x%04X, adc_channel=%d", 
             name, unit_uid, adc_channel);
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_main_supply(rbgrid_config_handle_t config,
                                  int sensor_unit_uid,
                                  const char* name,
                                  rbpower_stat_period_type_t stat_period)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->builder.has_main_supply) {
        ESP_LOGW(TAG, "Main supply already configured");
        return RBGRID_CONFIG_ERR_ALREADY_SET;
    }
    
    // Must be an ADC-typed unit_UID
    rbgrid_unit_id_t unit_uid = (rbgrid_unit_id_t)sensor_unit_uid;
    rbgrid_unit_type_t unit_type = rbgrid_get_unit_type(unit_uid);
    if (unit_type != RBGRID_UNIT_TYPE_ADC) {
        ESP_LOGE(TAG, "unit_UID 0x%04X is not ADC type (type=%d)", unit_uid, unit_type);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Extract the ADC channel from the unit_UID
    uint8_t adc_channel = RBGRID_GET_LOCAL(unit_uid);
    
    // Find the channel index in the array
    int sensor_id = -1;
    for (int i = 0; i < ctx->hardware_config->adc.channels_count; i++) {
        if (ctx->hardware_config->adc.channels[i] == adc_channel) {
            sensor_id = i;
            break;
        }
    }
    
    if (sensor_id < 0) {
        ESP_LOGE(TAG, "ADC channel %d from unit_UID 0x%04X not found in configuration", 
                 adc_channel, unit_uid);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Check the sensor slot configuration
    rbpower_sensor_channel_config_t* sensor_cfg = &ctx->hardware_config->adc_sensors.channels[sensor_id];
    if (!sensor_cfg->enabled) {
        ESP_LOGE(TAG, "Sensor channel %d is not enabled", sensor_id);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Must be a current sensor
    bool is_current = false;
    rbsensor_type_t sensor_type = sensor_cfg->type;
    if ((sensor_type >= RBSENSOR_TYPE_CURRENT_SCT013_10A && 
         sensor_type <= RBSENSOR_TYPE_CURRENT_SCT013_100A) ||
        (sensor_type >= RBSENSOR_TYPE_CURRENT_ACS712_5A && 
         sensor_type <= RBSENSOR_TYPE_CURRENT_CUSTOM)) {
        is_current = true;
    }
    
    if (!is_current) {
        ESP_LOGE(TAG, "Sensor channel %d (type=%d) is not a current sensor", sensor_id, sensor_type);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // max_current from the sensor profile
    float max_current = sensor_cfg->max_value;
    if (max_current <= 0) {
        max_current = 50.0f;  // default
    }
    
    // Allocate or grow the unit array if needed
    if (ctx->units_count >= ctx->units_allocated) {
        uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
        rbpower_unit_definition_t* new_array = (rbpower_unit_definition_t*)realloc(
            ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
        if (!new_array) {
            ESP_LOGE(TAG, "Failed to allocate memory for units");
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }
        ctx->units_array = new_array;
        ctx->units_allocated = new_size;
    }
    
    // Unit definition for the main supply
    rbpower_unit_definition_t* main_unit = &ctx->units_array[ctx->units_count];
    memset(main_unit, 0, sizeof(rbpower_unit_definition_t));
    
    // Basic parameters
    main_unit->unit_id = unit_uid;  // the unit_UID is the unit ID
    if (name && strlen(name) > 0) {
        strncpy(main_unit->name, name, sizeof(main_unit->name) - 1);
    } else {
        strncpy(main_unit->name, "Main Supply", sizeof(main_unit->name) - 1);
    }
    strncpy(main_unit->location, "Main Panel", sizeof(main_unit->location) - 1);
    main_unit->type = RBPOWER_METRIC_CURRENT_LOAD;  // the library creates an rbpower_main_supply_t
    main_unit->role = RBPOWER_ROLE_MAIN_SUPPLY;     // the role selects the specialized structure
    main_unit->stat_class = stat_period;
    
    // Data source: legacy ADC-typed binding
    main_unit->sources[0].source_type = RBPOWER_SOURCE_ADC_SENSOR;
    main_unit->sources[0].source_id = adc_channel; // ADC channel (from the unit_UID)
    main_unit->sources[0].sensor_profile_id = sensor_id; // index into the sensor slots
    main_unit->sources[0].weight_factor = 1.0f;
    main_unit->sources[0].is_primary = true;
    main_unit->sources[0].invert_sign = false;
    main_unit->sources_count = 1;
    
    // Hierarchy
    main_unit->parent_unit_id = RBPOWER_HOME_INVALID_ID;
    main_unit->children_count = 0;
    
    // Main supply parameters
    main_unit->bidirectional_metering = true;  // the main supply can be bidirectional
    main_unit->accumulate_energy = true;
    main_unit->rated_power_w = max_current * 230.0f;
    main_unit->max_current_a = max_current;
    main_unit->rated_voltage_v = 230.0f;
    
    // Thresholds
    main_unit->thresholds.over_power_w = main_unit->rated_power_w * 0.9f;
    main_unit->thresholds.under_voltage_v = 207.0f;
    main_unit->thresholds.over_current_a = max_current * 0.9f;
    main_unit->thresholds.over_voltage_v = 253.0f;
    main_unit->thresholds.enable_alerts = true;
    
    // Main supply billing
    main_unit->billing.cost_per_kwh = 5.50f;  // base tariff
    main_unit->billing.time_of_use_enabled = true;
    main_unit->billing.current_tariff_zone = 0;
    
    main_unit->is_active = true;
    main_unit->created_timestamp_us = 0;
    
    ctx->units_count++;
    ctx->builder.has_main_supply = true;
    ctx->status.units_configured = true;
    
    ESP_LOGI(TAG, "Main supply configured: name='%s', unit_UID=0x%04X, adc_channel=%d, max_current=%.1fA", 
             main_unit->name, unit_uid, adc_channel, max_current);
    
    return RBGRID_CONFIG_OK;
}

/** @brief SCT-013 sensor type -> rated current (A). CT code mapping lives in the rbAmp
 * source (provisioning); here we only need the rating for unit thresholds. */
static float rbamp_sensor_rated_current(rbsensor_type_t t)
{
    switch (t) {
        case RBSENSOR_TYPE_CURRENT_SCT013_10A: return 10.0f;
        case RBSENSOR_TYPE_CURRENT_SCT013_20A: return 20.0f;
        case RBSENSOR_TYPE_CURRENT_SCT013_30A: return 30.0f;
        case RBSENSOR_TYPE_CURRENT_SCT013_50A: return 50.0f;
        default:                               return 10.0f;
    }
}

int rbgrid_config_add_rbamp_module(rbgrid_config_handle_t config,
                                   uint8_t i2c_address,
                                   uint8_t module_index,
                                   const rbgrid_rbamp_channel_t* channels,
                                   uint8_t channel_count)
{
    if (!config || !channels || channel_count == 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }

    for (uint8_t i = 0; i < channel_count; ++i) {
        const rbgrid_rbamp_channel_t* ch = &channels[i];

        /* The home network registers at most RBPOWER_MAX_HOME_UNITS units; warn at
         * config time instead of silently dropping the unit later. */
        if (ctx->units_count >= RBPOWER_MAX_HOME_UNITS) {
            ESP_LOGE(TAG, "rbamp module %u ch%u: unit count %u would exceed RBPOWER_MAX_HOME_UNITS (%d) "
                          "- unit will NOT register; raise the cap",
                     module_index, ch->channel, ctx->units_count, RBPOWER_MAX_HOME_UNITS);
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }

        // Reject a duplicate unit_id. The loop computes ids from (module,channel); a repeated
        // channel in this call, or a collision with an already-added unit (e.g. another module mapped to
        // the same id), would corrupt stats + the shared flash slot.
        rbpower_unit_id_t cand_uid = (rbpower_unit_id_t)RBGRID_MAKE_I2C_UNIT_ID(0, module_index, ch->channel);
        if (rbgrid_find_unit_in_array(ctx, cand_uid) >= 0) {
            ESP_LOGE(TAG, "rbamp: unit_UID 0x%04X (mod %u ch %u) already exists — duplicate rejected",
                     (unsigned)cand_uid, module_index, ch->channel);
            return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
        }
        /* A units.json-restored unit may hold this sensor under an OPAQUE id the check above
         * can't see — occupancy is by physics. (main.cpp also pre-filters; this is the backstop.) */
        if (rbgrid_config_builder_has_source(config, RBPOWER_SOURCE_I2C_SENSOR,
                                             module_index, ch->channel)) {
            ESP_LOGW(TAG, "rbamp: sensor (mod %u ch %u) already restored from units.json — skipped",
                     module_index, ch->channel);
            continue;
        }

        if (ctx->units_count >= ctx->units_allocated) {
            uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
            rbpower_unit_definition_t* na = (rbpower_unit_definition_t*)realloc(
                ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
            if (!na) { ESP_LOGE(TAG, "rbamp: units realloc failed"); return RBGRID_CONFIG_ERR_NO_MEMORY; }
            ctx->units_array = na;
            ctx->units_allocated = new_size;
        }

        rbpower_unit_definition_t* u = &ctx->units_array[ctx->units_count];
        memset(u, 0, sizeof(*u));
        u->unit_id = cand_uid;
        if (ch->name && ch->name[0]) strncpy(u->name, ch->name, sizeof(u->name) - 1);
        else snprintf(u->name, sizeof(u->name), "rbAmp %u.%u", module_index, ch->channel);
        strncpy(u->location, "rbAmp I2C", sizeof(u->location) - 1);
        u->type = RBPOWER_METRIC_CURRENT_LOAD;
        u->role = RBPOWER_ROLE_DEVICE_LOAD;
        u->stat_class = RBPOWER_STAT_PERIOD_1HOUR;

        u->sources[0].source_type      = RBPOWER_SOURCE_I2C_SENSOR;
        u->sources[0].source_id        = ch->channel;     /* rbAmp channel index */
        u->sources[0].sensor_profile_id = (uint8_t)ch->sensor_type;
        u->sources[0].weight_factor    = 1.0f;
        u->sources[0].is_primary       = true;
        u->sources[0].invert_sign      = false;
        u->sources_count = 1;

        /* Identity: physics in explicit fields + point slug + caps (legacy canonical id kept). */
        u->device_uid  = module_index;
        u->channel_idx = ch->channel;
        u->caps = rbpoint_caps_for_source(RBPOWER_SOURCE_I2C_SENSOR, u->type, false);
        {
            char base[RBPOINT_SLUG_MAX + 1];
            rbpoint_slug_from_name(u->name, base);
            rbpoint_slug_uniquify(base, u->point, point_taken_builder, ctx);
        }

        u->parent_unit_id = RBPOWER_HOME_INVALID_ID;
        float rated = rbamp_sensor_rated_current(ch->sensor_type);
        u->accumulate_energy = true;
        u->max_current_a  = rated;
        u->rated_voltage_v = 230.0f;
        u->rated_power_w   = rated * 230.0f;
        u->thresholds.over_current_a = rated * 0.9f;
        u->thresholds.over_power_w   = u->rated_power_w * 0.9f;
        u->thresholds.under_voltage_v = 207.0f;
        u->thresholds.over_voltage_v  = 253.0f;
        u->thresholds.enable_alerts   = true;
        u->billing.cost_per_kwh = 5.50f;
        u->is_active = true;

        ctx->units_count++;
        ctx->status.units_configured = true;
        ESP_LOGI(TAG, "rbAmp module %u (0x%02X) ch%u -> unit_UID=0x%04X '%s' (%.0fA)",
                 module_index, i2c_address, ch->channel, u->unit_id, u->name, rated);
    }
    return RBGRID_CONFIG_OK;
}

/* ESP-NOW rbAmp module — registers a REMOTE rbAmp's channels as STANDARD sensor-units, mirroring
 * rbgrid_config_add_rbamp_module exactly but with source_type=ESP_NOW and an ESP-NOW unit_id derived
 * from (node MAC, channel). Config-time registration ⇒ the units get the full standard treatment at
 * rbpower_home_init (flash buffers, source mappings, period accumulators) — identical to a local rbAmp
 * unit, so history/billing/tariff all work. The ESP-NOW hub then just feeds them by the same unit_id. */
int rbgrid_config_add_espnow_module(rbgrid_config_handle_t config,
                                    const uint8_t node_mac[6],
                                    const rbgrid_rbamp_channel_t* channels,
                                    uint8_t channel_count)
{
    if (!config || !node_mac || !channels || channel_count == 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }

    for (uint8_t i = 0; i < channel_count; ++i) {
        const rbgrid_rbamp_channel_t* ch = &channels[i];

        if (ctx->units_count >= RBPOWER_MAX_HOME_UNITS) {
            ESP_LOGE(TAG, "espnow ch%u: unit count %u would exceed RBPOWER_MAX_HOME_UNITS (%d)",
                     ch->channel, ctx->units_count, RBPOWER_MAX_HOME_UNITS);
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }

        rbpower_unit_id_t cand_uid = rbgrid_create_espnow_unit_id(node_mac, ch->channel);
        if (rbgrid_find_unit_in_array(ctx, cand_uid) >= 0) {
            ESP_LOGE(TAG, "espnow: unit_UID 0x%04X (ch %u) already exists — duplicate rejected",
                     (unsigned)cand_uid, ch->channel);
            return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
        }

        if (ctx->units_count >= ctx->units_allocated) {
            uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
            rbpower_unit_definition_t* na = (rbpower_unit_definition_t*)realloc(
                ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
            if (!na) { ESP_LOGE(TAG, "espnow: units realloc failed"); return RBGRID_CONFIG_ERR_NO_MEMORY; }
            ctx->units_array = na;
            ctx->units_allocated = new_size;
        }

        rbpower_unit_definition_t* u = &ctx->units_array[ctx->units_count];
        memset(u, 0, sizeof(*u));
        u->unit_id = cand_uid;
        if (ch->name && ch->name[0]) strncpy(u->name, ch->name, sizeof(u->name) - 1);
        else snprintf(u->name, sizeof(u->name), "ESPNOW %02X%02X.%u", node_mac[4], node_mac[5], ch->channel);
        strncpy(u->location, "ESP-NOW node", sizeof(u->location) - 1);
        u->type = RBPOWER_METRIC_CURRENT_LOAD;
        u->role = RBPOWER_ROLE_DEVICE_LOAD;
        u->stat_class = RBPOWER_STAT_PERIOD_1HOUR;

        u->sources[0].source_type       = RBPOWER_SOURCE_ESP_NOW;
        u->sources[0].source_id          = ch->channel;     /* the node's flat channel_id (module*4+ch) */
        u->sources[0].sensor_profile_id  = (uint8_t)ch->sensor_type;
        u->sources[0].weight_factor      = 1.0f;
        u->sources[0].is_primary         = true;
        u->sources[0].invert_sign        = false;
        u->sources_count = 1;

        /* Identity: device_uid = node MAC packed LE48, channel_idx = flat channel. */
        u->device_uid = ((uint64_t)node_mac[0]) | ((uint64_t)node_mac[1] << 8) |
                        ((uint64_t)node_mac[2] << 16) | ((uint64_t)node_mac[3] << 24) |
                        ((uint64_t)node_mac[4] << 32) | ((uint64_t)node_mac[5] << 40);
        u->channel_idx = ch->channel;
        u->caps = rbpoint_caps_for_source(RBPOWER_SOURCE_ESP_NOW, u->type, false);
        {
            char base[RBPOINT_SLUG_MAX + 1];
            rbpoint_slug_from_name(u->name, base);
            rbpoint_slug_uniquify(base, u->point, point_taken_builder, ctx);
        }

        u->parent_unit_id = RBPOWER_HOME_INVALID_ID;
        float rated = rbamp_sensor_rated_current(ch->sensor_type);
        u->accumulate_energy = true;
        u->max_current_a  = rated;
        u->rated_voltage_v = 230.0f;
        u->rated_power_w   = rated * 230.0f;
        u->thresholds.over_current_a = rated * 0.9f;
        u->thresholds.over_power_w   = u->rated_power_w * 0.9f;
        u->thresholds.under_voltage_v = 207.0f;
        u->thresholds.over_voltage_v  = 253.0f;
        u->thresholds.enable_alerts   = true;
        u->billing.cost_per_kwh = 5.50f;
        u->is_active = true;

        ctx->units_count++;
        ctx->status.units_configured = true;
        ESP_LOGI(TAG, "ESP-NOW node %02X:%02X:%02X:%02X:%02X:%02X ch%u -> unit_UID=0x%04X '%s' (%.0fA)",
                 node_mac[0],node_mac[1],node_mac[2],node_mac[3],node_mac[4],node_mac[5],
                 ch->channel, u->unit_id, u->name, rated);
    }
    return RBGRID_CONFIG_OK;
}

/** @brief Recover the RBgrid sensor type for an rbAmp hardware CT code (1=5A, 2=10A,
 * 3=30A, 4=50A, 6=20A; RBgrid has no 5A type → 10A). Inverse of the fleet provisioning mapping. */
static rbsensor_type_t rbamp_ct_code_to_sensor_type(uint8_t ct_code)
{
    switch (ct_code) {
        case 1: return RBSENSOR_TYPE_CURRENT_SCT013_10A;  /* 5A clamp → 10A type (no native 5A) */
        case 2: return RBSENSOR_TYPE_CURRENT_SCT013_10A;
        case 3: return RBSENSOR_TYPE_CURRENT_SCT013_30A;
        case 4: return RBSENSOR_TYPE_CURRENT_SCT013_50A;
        case 6: return RBSENSOR_TYPE_CURRENT_SCT013_20A;
        default: return RBSENSOR_TYPE_CURRENT_SCT013_10A;
    }
}

int rbgrid_config_bind_unit(const rbgrid_create_unit_req_t* req, rbpower_unit_id_t* out_uid)
{
    if (!req || !req->name[0]) return RBGRID_CONFIG_ERR_INVALID_PARAM;

    /* Common unit defaults — mirror rbgrid_config_add_rbamp_module's per-unit block. */
    rbpower_unit_definition_t u;
    memset(&u, 0, sizeof(u));
    strncpy(u.name, req->name, sizeof(u.name) - 1);
    strncpy(u.location, req->location[0] ? req->location : "rbAmp I2C", sizeof(u.location) - 1);
    u.role            = req->role;
    u.stat_class      = req->stat_class;
    u.parent_unit_id  = RBPOWER_HOME_INVALID_ID;
    u.accumulate_energy = true;
    u.rated_voltage_v = 230.0f;
    u.is_active       = true;
    u.billing.cost_per_kwh = 5.50f;
    u.sources[0].weight_factor = 1.0f;
    u.sources[0].is_primary    = true;
    u.sources[0].invert_sign   = false;
    u.sources_count = 1;

    rbpower_unit_id_t uid;
    float rated;

    if (strcmp(req->iface, "i2c") == 0) {
        /* Bus is FORCED to 0 to match add_rbamp_module's canonical id (the fleet is single-bus and the
         * fleet match below is module/ch only). A client-supplied bus!=0 would otherwise compute a
         * non-canonical uid that the duplicate guard misses → a phantom unit. */
        if (req->bus != 0) {
            ESP_LOGE(TAG, "bind: only i2c bus 0 supported (got %u)", req->bus);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }

        /* Sensor occupancy is decided by scanning live units' PHYSICAL binding
         * (device_uid=module, channel_idx=ch), not by id equality — ids are opaque now. */
        if (rbpower_home_find_unit_by_source(RBPOWER_SOURCE_I2C_SENSOR,
                                             req->module, req->ch,
                                             RBPOWER_HOME_INVALID_ID) != RBPOWER_HOME_INVALID_ID) {
            ESP_LOGW(TAG, "bind: i2c sensor (mod %u ch %u) already bound", req->module, req->ch);
            return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
        }

        /* unit_id = opaque allocated handle (0x0100..0x3FFF), never derived from physics. */
        uid = rbpower_home_alloc_unit_id();
        if (uid == RBPOWER_HOME_INVALID_ID) {
            ESP_LOGE(TAG, "bind: unit-id space exhausted");
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }

        /* The CT model lives in the persisted fleet entry (the sensor survived the unbind). */
        rbamp_bridge_config_t fleet;
        uint8_t ct_code = 0;
        bool found = false;
        if (rbgrid_storage_load_rbamp(&fleet) == ESP_OK &&
            req->module < fleet.module_count && req->module < RBAMP_MAX_MODULES) {
            const rbamp_module_cfg_t* fm = &fleet.modules[req->module];
            for (uint8_t e = 0; e < fm->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
                if (fm->entries[e].source == req->ch) { ct_code = fm->entries[e].ct_code; found = true; break; }
            }
        }
        if (!found) {
            ESP_LOGE(TAG, "bind: i2c sensor (mod %u ch %u) not found in fleet", req->module, req->ch);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }
        rbsensor_type_t st = rbamp_ct_code_to_sensor_type(ct_code);
        rated = rbamp_sensor_rated_current(st);
        u.type = RBPOWER_METRIC_CURRENT_LOAD;
        u.sources[0].source_type       = RBPOWER_SOURCE_I2C_SENSOR;
        u.sources[0].source_id         = req->ch;
        u.sources[0].sensor_profile_id = (uint8_t)st;
        u.device_uid  = req->module;    /* physics in explicit fields, not id bits */
        u.channel_idx = req->ch;
        u.caps = rbpoint_caps_for_source(RBPOWER_SOURCE_I2C_SENSOR, u.type, false);
    } else {
        /* Only I2C (rbAmp) sensors can be bound at runtime. ADC-typed units exist only as the legacy
         * binding of the default units created at boot; there are no unbound ADC sensors to bind.
         * Reject cleanly (handler -> 400) rather than build a wrong unit. */
        ESP_LOGW(TAG, "bind: iface '%s' not supported yet (only i2c/rbAmp)", req->iface);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }

    /* Point slug — explicit (validated, 409 on taken/tombstoned unless reuse) or autogen. */
    if (req->point[0]) {
        if (!rbpoint_slug_valid(req->point)) {
            ESP_LOGE(TAG, "bind: invalid point slug '%s'", req->point);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }
        if (rbpower_home_find_unit_by_point(req->point) != RBPOWER_HOME_INVALID_ID) {
            return RBGRID_CONFIG_ERR_POINT_TAKEN;
        }
        if (rbgrid_point_tombstoned(req->point)) {
            if (!req->reuse) return RBGRID_CONFIG_ERR_POINT_TOMBSTONED;
            rbgrid_point_tombstone_remove(req->point);
            ESP_LOGW(TAG, "bind: tombstoned point '%s' RESURRECTED (reuse:true)", req->point);
        }
        strncpy(u.point, req->point, RBPOINT_SLUG_MAX);
    } else {
        char base[RBPOINT_SLUG_MAX + 1];
        rbpoint_slug_from_name(req->name, base);
        rbpoint_slug_uniquify(base, u.point, point_taken_live, NULL);
    }

    u.unit_id = uid;
    u.max_current_a = rated;
    u.rated_power_w = rated * u.rated_voltage_v;
    u.thresholds.over_current_a   = rated * 0.9f;
    u.thresholds.over_power_w     = u.rated_power_w * 0.9f;
    u.thresholds.under_voltage_v  = 207.0f;
    u.thresholds.over_voltage_v   = 253.0f;
    u.thresholds.enable_alerts    = true;

    if (out_uid) *out_uid = uid;   /* report the (computed) id even on the DUPLICATE path */

    rbpower_err_t rc = rbpower_home_create_unit(&u);
    if (rc == RBPOWER_ERR_INVALID_STATE) return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;  /* already bound */
    if (rc == RBPOWER_ERR_NO_MEMORY)     return RBGRID_CONFIG_ERR_NO_MEMORY;
    if (rc != RBPOWER_OK)                return RBGRID_CONFIG_ERR_INVALID_PARAM;

    ESP_LOGI(TAG, "bind: created unit 0x%04X point='%s' '%s' (%.0fA, role=%d stat=%d)",
             (unsigned)uid, u.point, u.name, rated, (int)u.role, (int)u.stat_class);
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_rebind_unit(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch)
{
    const rbpower_unit_definition_t* def = rbpower_home_get_unit_definition(unit_id);
    if (!def) return RBGRID_CONFIG_ERR_INVALID_PARAM;
    uint8_t fm_mod = 0xFF, fm_src = 0;
    if ((def->sources_count == 0 || def->sources[0].source_type != RBPOWER_SOURCE_I2C_SENSOR) &&
        rbpower_get_unit_rbamp_feed(unit_id, &fm_mod, &fm_src)) {
        /* bridge-fed unit (e.g. the main input): the fleet entry is its binding — the target must be free of I2C units */
        if (rbpower_home_find_unit_by_source(RBPOWER_SOURCE_I2C_SENSOR, module, ch, unit_id) != RBPOWER_HOME_INVALID_ID)
            return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
        int r = rbpower_unit_feed_rebind(unit_id, module, ch);
        return r == 0 ? RBGRID_CONFIG_OK : r == -2 ? RBGRID_CONFIG_ERR_DUPLICATE_SENSOR : RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    if (def->sources_count == 0 || def->sources[0].source_type != RBPOWER_SOURCE_I2C_SENSOR) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;   /* only live I2C units rebind today */
    }
    if (def->device_uid == module && def->channel_idx == ch) {
        return RBGRID_CONFIG_OK;   /* no-op rebind to the same sensor */
    }
    /* Target sensor must be free (excluding ourselves — covered by the equality check above). */
    if (rbpower_home_find_unit_by_source(RBPOWER_SOURCE_I2C_SENSOR, module, ch,
                                         unit_id) != RBPOWER_HOME_INVALID_ID) {
        return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
    }
    /* Target must exist in the fleet; recover its CT model (same lookup as bind). */
    rbamp_bridge_config_t fleet;
    uint8_t ct_code = 0;
    bool found = false;
    if (rbgrid_storage_load_rbamp(&fleet) == ESP_OK &&
        module < fleet.module_count && module < RBAMP_MAX_MODULES) {
        const rbamp_module_cfg_t* fm = &fleet.modules[module];
        for (uint8_t e = 0; e < fm->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
            if (fm->entries[e].source == ch) { ct_code = fm->entries[e].ct_code; found = true; break; }
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "rebind: i2c sensor (mod %u ch %u) not found in fleet", module, ch);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    rbsensor_type_t st = rbamp_ct_code_to_sensor_type(ct_code);
    rbpower_data_source_t src = def->sources[0];
    src.source_id         = ch;
    src.sensor_profile_id = (uint8_t)st;
    rbpower_err_t rc = rbpower_home_rebind_unit_source(unit_id, &src, module, ch,
                                                       rbamp_sensor_rated_current(st));
    return (rc == RBPOWER_OK) ? RBGRID_CONFIG_OK : RBGRID_CONFIG_ERR_INVALID_PARAM;
}

int rbgrid_config_rename_point(rbpower_unit_id_t unit_id, const char* new_point, bool reuse)
{
    rbpower_unit_definition_t* def = rbpower_home_get_unit_definition_mutable(unit_id);
    if (!def) return RBGRID_CONFIG_ERR_INVALID_PARAM;
    if (!rbpoint_slug_valid(new_point)) return RBGRID_CONFIG_ERR_INVALID_PARAM;
    if (strcmp(def->point, new_point) == 0) return RBGRID_CONFIG_OK;
    if (rbpower_home_find_unit_by_point(new_point) != RBPOWER_HOME_INVALID_ID) {
        return RBGRID_CONFIG_ERR_POINT_TAKEN;
    }
    if (rbgrid_point_tombstoned(new_point)) {
        if (!reuse) return RBGRID_CONFIG_ERR_POINT_TOMBSTONED;
        rbgrid_point_tombstone_remove(new_point);
    }
    char old_point[RBPOINT_SLUG_MAX + 1];
    strncpy(old_point, def->point, RBPOINT_SLUG_MAX); old_point[RBPOINT_SLUG_MAX] = '\0';
    strncpy(def->point, new_point, RBPOINT_SLUG_MAX); def->point[RBPOINT_SLUG_MAX] = '\0';
    if (old_point[0]) rbgrid_point_tombstone_add(old_point);
    /* POINT_RENAMED — there is no event/audit queue yet; the log line is the record. */
    ESP_LOGW(TAG, "EVENT POINT_RENAMED unit 0x%04X: '%s' -> '%s'",
             (unsigned)unit_id, old_point, new_point);
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_add_persisted_unit(rbgrid_config_handle_t config,
                                     const rbpower_unit_definition_t* def)
{
    if (!config || !def) return RBGRID_CONFIG_ERR_INVALID_PARAM;
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    if (!ctx->is_initialized) return RBGRID_CONFIG_ERR_NOT_INITIALIZED;

    if (ctx->units_count >= RBPOWER_MAX_HOME_UNITS) {
        ESP_LOGE(TAG, "persisted unit 0x%04X: unit cap reached", (unsigned)def->unit_id);
        return RBGRID_CONFIG_ERR_NO_MEMORY;
    }
    if (rbgrid_find_unit_in_array(ctx, def->unit_id) >= 0) {
        ESP_LOGE(TAG, "persisted unit 0x%04X: duplicate id in builder", (unsigned)def->unit_id);
        return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
    }
    if (def->sources_count > 0 &&
        rbgrid_config_builder_has_source(config, def->sources[0].source_type,
                                         def->device_uid, def->channel_idx)) {
        ESP_LOGE(TAG, "persisted unit 0x%04X: sensor dev=%llu ch=%u already queued",
                 (unsigned)def->unit_id, (unsigned long long)def->device_uid, def->channel_idx);
        return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
    }

    if (ctx->units_count >= ctx->units_allocated) {
        uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
        rbpower_unit_definition_t* na = (rbpower_unit_definition_t*)realloc(
            ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
        if (!na) return RBGRID_CONFIG_ERR_NO_MEMORY;
        ctx->units_array = na;
        ctx->units_allocated = new_size;
    }

    rbpower_unit_definition_t* u = &ctx->units_array[ctx->units_count];
    *u = *def;
    /* Self-migration of records written before point slugs existed: backfill point (from name) and caps when absent. */
    if (!u->point[0]) {
        char base[RBPOINT_SLUG_MAX + 1];
        rbpoint_slug_from_name(u->name, base);
        rbpoint_slug_uniquify(base, u->point, point_taken_builder, ctx);
    }
    if (u->caps == 0 && u->sources_count > 0) {
        u->caps = rbpoint_caps_for_source(u->sources[0].source_type, u->type,
                                          u->bidirectional_metering);
    }
    ctx->units_count++;
    ctx->status.units_configured = true;
    ESP_LOGI(TAG, "persisted unit restored: 0x%04X point='%s' '%s' (dev=%llu ch=%u)",
             (unsigned)u->unit_id, u->point, u->name,
             (unsigned long long)u->device_uid, u->channel_idx);
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_alternative_supply(rbgrid_config_handle_t config,
                                         rbgrid_unit_uid_t unit_uid,
                                         const char* name,
                                         rbpower_stat_period_type_t stat_period)
{
    if (!config || unit_uid == RBGRID_INVALID_UNIT_UID) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->builder.has_alternative_supply) {
        ESP_LOGW(TAG, "Alternative supply already configured");
        return RBGRID_CONFIG_ERR_ALREADY_SET;
    }
    
    // Extract the ADC channel from the unit_UID
    uint8_t adc_channel = RBGRID_GET_LOCAL(unit_uid);
    
    // Must be an ADC-typed unit
    if (RBGRID_GET_TYPE(unit_uid) != RBGRID_UNIT_TYPE_ADC) {
        ESP_LOGE(TAG, "Unit 0x%04X is not an ADC unit", unit_uid);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Find the sensor index in the configuration
    int sensor_id = -1;
    for (int i = 0; i < ctx->hardware_config->adc.channels_count; i++) {
        if (ctx->hardware_config->adc.channels[i] == adc_channel) {
            sensor_id = i;
            break;
        }
    }
    
    if (sensor_id < 0) {
        ESP_LOGE(TAG, "ADC channel %d not found in configuration", adc_channel);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Check the sensor slot configuration
    rbpower_sensor_channel_config_t* sensor_cfg = &ctx->hardware_config->adc_sensors.channels[sensor_id];
    if (!sensor_cfg->enabled) {
        ESP_LOGE(TAG, "Sensor channel %d is not enabled", sensor_id);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Must be a current sensor
    bool is_current = false;
    rbsensor_type_t sensor_type = sensor_cfg->type;
    if ((sensor_type >= RBSENSOR_TYPE_CURRENT_SCT013_10A && 
         sensor_type <= RBSENSOR_TYPE_CURRENT_SCT013_100A) ||
        (sensor_type >= RBSENSOR_TYPE_CURRENT_ACS712_5A && 
         sensor_type <= RBSENSOR_TYPE_CURRENT_CUSTOM)) {
        is_current = true;
    }
    
    if (!is_current) {
        ESP_LOGE(TAG, "Sensor channel %d (type=%d) is not a current sensor", sensor_id, sensor_type);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // max_current from the sensor profile
    float max_current = sensor_cfg->max_value;
    if (max_current <= 0) {
        max_current = 50.0f;  // default
    }
    
    // Allocate or grow the unit array if needed
    if (ctx->units_count >= ctx->units_allocated) {
        uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
        rbpower_unit_definition_t* new_array = (rbpower_unit_definition_t*)realloc(
            ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
        if (!new_array) {
            ESP_LOGE(TAG, "Failed to allocate memory for units");
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }
        ctx->units_array = new_array;
        ctx->units_allocated = new_size;
    }
    
    // Unit definition for the alternative supply
    rbpower_unit_definition_t* alt_unit = &ctx->units_array[ctx->units_count];
    memset(alt_unit, 0, sizeof(rbpower_unit_definition_t));
    
    // Basic parameters
    alt_unit->unit_id = unit_uid;  // the unit_UID is the unit ID
    if (name && strlen(name) > 0) {
        strncpy(alt_unit->name, name, sizeof(alt_unit->name) - 1);
    } else {
        strncpy(alt_unit->name, "Alternative Supply", sizeof(alt_unit->name) - 1);
    }
    strncpy(alt_unit->location, "Alternative Panel", sizeof(alt_unit->location) - 1);
    alt_unit->type = RBPOWER_METRIC_CURRENT_LOAD;  // the library creates an rbpower_alternative_supply_t
    alt_unit->role = RBPOWER_ROLE_ALTERNATIVE_SUPPLY;  // the role selects the specialized structure
    alt_unit->stat_class = stat_period;
    
    // Data source: legacy ADC-typed binding
    alt_unit->sources[0].source_type = RBPOWER_SOURCE_ADC_SENSOR;
    alt_unit->sources[0].source_id = adc_channel; // ADC channel (from the unit_UID)
    alt_unit->sources[0].sensor_profile_id = sensor_id; // index into the sensor slots
    alt_unit->sources[0].weight_factor = 1.0f;
    alt_unit->sources[0].is_primary = true;
    alt_unit->sources[0].invert_sign = false;
    alt_unit->sources_count = 1;
    
    // Hierarchy
    alt_unit->parent_unit_id = RBPOWER_HOME_INVALID_ID;
    alt_unit->children_count = 0;
    
    // Alternative supply parameters
    alt_unit->bidirectional_metering = true;  // the alternative supply can be bidirectional
    alt_unit->accumulate_energy = true;
    alt_unit->rated_power_w = max_current * 230.0f;
    alt_unit->max_current_a = max_current;
    alt_unit->rated_voltage_v = 230.0f;
    
    // Thresholds
    alt_unit->thresholds.over_power_w = alt_unit->rated_power_w * 0.9f;
    alt_unit->thresholds.under_voltage_v = 207.0f;
    alt_unit->thresholds.over_current_a = max_current * 0.9f;
    alt_unit->thresholds.over_voltage_v = 253.0f;
    alt_unit->thresholds.enable_alerts = true;
    
    // Alternative supply billing (negative = feed-in for generation)
    alt_unit->billing.cost_per_kwh = -3.00f;  // feed-in tariff for alternative sources
    alt_unit->billing.time_of_use_enabled = true;
    alt_unit->billing.current_tariff_zone = 0;
    
    alt_unit->is_active = true;
    alt_unit->created_timestamp_us = 0;
    
    ctx->units_count++;
    ctx->builder.has_alternative_supply = true;
    ctx->status.units_configured = true;
    
    ESP_LOGI(TAG, "Alternative supply configured: name='%s', unit_UID=0x%04X, adc_channel=%d, max_current=%.1fA", 
             alt_unit->name, unit_uid, adc_channel, max_current);
    
    return RBGRID_CONFIG_OK;
}

/* Per-install CT orientation (rbgrid_config_set_source_invert below): set sources[0].invert_sign on a unit, in BOTH places it lives —
 * the config definition (so a later instantiation inherits it) and the LIVE home-network unit
 * (so a loader running after home-init applies immediately). Consumed at the inject boundaries
 * (rbpower_home_inject_200ms / _inject_period_result). Both copies must be written, otherwise the
 * orientation is serialized but never applied. */
/* The config-context copy of a unit definition (what the home network is built from at boot); NULL if absent. */
rbpower_unit_definition_t* rbgrid_config_unit_def_mutable(rbgrid_config_handle_t config, int unit_uid)
{
    if (!config || unit_uid <= 0) return NULL;
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    int ci = rbgrid_find_unit_in_array(ctx, (rbgrid_unit_id_t)unit_uid);
    return (ci >= 0) ? &ctx->units_array[ci] : NULL;
}

int rbgrid_config_set_source_invert(rbgrid_config_handle_t config, int unit_uid, bool invert)
{
    extern rbpower_electrical_network_t g_home_network;   /* also declared further down in this file */
    if (!config || unit_uid <= 0) return RBGRID_CONFIG_ERR_INVALID_PARAM;
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    bool touched = false;

    int ci = rbgrid_find_unit_in_array(ctx, (rbgrid_unit_id_t)unit_uid);
    if (ci >= 0) {
        ctx->units_array[ci].sources[0].invert_sign = invert;
        touched = true;
    }
    uint8_t ui = find_unit_index_by_id((rbpower_unit_id_t)unit_uid);
    if (ui != 0xFF && ui < RBPOWER_MAX_HOME_UNITS) {
        g_home_network.units[ui].definition.sources[0].invert_sign = invert;
        touched = true;
    }
    if (touched && invert) {
        ESP_LOGI(TAG, "unit 0x%04X: source invert_sign = true (CT orientation)", (unsigned)unit_uid);
    }
    return touched ? RBGRID_CONFIG_OK : RBGRID_CONFIG_ERR_INVALID_PARAM;
}

// Add a regular unit (legacy ADC-typed binding)

int rbgrid_config_add_unit(rbgrid_config_handle_t config,
                        int sensor_unit_uid,
                        const char* name,
                        rbpower_stat_period_type_t stat_period)
{
    if (!config || !name || sensor_unit_uid <= 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    // Unit limit
    if (ctx->units_count >= RBGRID_CONFIG_MAX_UNITS) {
        ESP_LOGE(TAG, "Unit limit reached (%d)", RBGRID_CONFIG_MAX_UNITS);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }

    // Must be an ADC-typed unit_UID
    rbgrid_unit_id_t unit_uid = (rbgrid_unit_id_t)sensor_unit_uid;
    rbgrid_unit_type_t unit_type = rbgrid_get_unit_type(unit_uid);
    if (unit_type != RBGRID_UNIT_TYPE_ADC) {
        ESP_LOGE(TAG, "unit_UID 0x%04X is not ADC type (type=%d)", unit_uid, unit_type);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }

    // Reject a duplicate unit_id (a regular unit colliding with a supply or another unit
    // corrupts stats + the flash slot). Most likely collision path: a repeated user-passed id.
    if (rbgrid_find_unit_in_array(ctx, unit_uid) >= 0) {
        ESP_LOGE(TAG, "unit_UID 0x%04X already registered — duplicate unit rejected", unit_uid);
        return RBGRID_CONFIG_ERR_DUPLICATE_SENSOR;
    }

    // Extract the ADC channel from the unit_UID
    uint8_t adc_channel = RBGRID_GET_LOCAL(unit_uid);
    
    // Find the channel index in the array
    int sensor_id = -1;
    for (int i = 0; i < ctx->hardware_config->adc.channels_count; i++) {
        if (ctx->hardware_config->adc.channels[i] == adc_channel) {
            sensor_id = i;
            break;
        }
    }
    
    if (sensor_id < 0) {
        ESP_LOGE(TAG, "ADC channel %d from unit_UID 0x%04X not found in configuration", 
                 adc_channel, unit_uid);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Sensor slot configuration from hardware_config->adc_sensors
    rbpower_sensor_channel_config_t* sensor_cfg = &ctx->hardware_config->adc_sensors.channels[sensor_id];
    if (!sensor_cfg->enabled) {
        ESP_LOGE(TAG, "Sensor channel %d is not enabled", sensor_id);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    
    // Maximum current from the sensor type
    float max_current = sensor_cfg->max_value;
    if (max_current <= 0) {
        max_current = 50.0f;  // default
    }
    
    rbpower_metric_type_t metric_type = RBPOWER_METRIC_CURRENT_LOAD;
    rbpower_load_role_t role = RBPOWER_ROLE_ZONE_LOAD;
    
    // Metric type from the sensor type
    rbsensor_type_t sensor_type = sensor_cfg->type;
    if ((sensor_type >= RBSENSOR_TYPE_VOLTAGE_ZMPT101B && 
         sensor_type <= RBSENSOR_TYPE_VOLTAGE_CUSTOM)) {
        // Voltage sensors
        metric_type = RBPOWER_METRIC_VOLTAGE_BUS;
        role = RBPOWER_ROLE_MAIN_SUPPLY;  // MAIN_SUPPLY role for the voltage bus
        max_current = 0.0f;  // not used for voltage
    } else if ((sensor_type >= RBSENSOR_TYPE_CURRENT_SCT013_10A && 
                sensor_type <= RBSENSOR_TYPE_CURRENT_CUSTOM)) {
        // Current sensors
        metric_type = RBPOWER_METRIC_CURRENT_LOAD;
        role = RBPOWER_ROLE_ZONE_LOAD;
    } else {
        // Unknown types: keep the defaults
        ESP_LOGW(TAG, "Unknown sensor type %d, using defaults", sensor_type);
    }
    
    // Allocate or grow the unit array if needed
    if (ctx->units_count >= ctx->units_allocated) {
        uint16_t new_size = ctx->units_allocated == 0 ? 8 : ctx->units_allocated * 2;
        rbpower_unit_definition_t* new_array = (rbpower_unit_definition_t*)realloc(
            ctx->units_array, new_size * sizeof(rbpower_unit_definition_t));
        if (!new_array) {
            ESP_LOGE(TAG, "Failed to allocate memory for units");
            return RBGRID_CONFIG_ERR_NO_MEMORY;
        }
        ctx->units_array = new_array;
        ctx->units_allocated = new_size;
    }
    
    // Unit definition for a regular unit
    rbpower_unit_definition_t* unit = &ctx->units_array[ctx->units_count];
    memset(unit, 0, sizeof(rbpower_unit_definition_t));
    
    // Basic parameters
    unit->unit_id = unit_uid;  // the caller's unit_UID
    strncpy(unit->name, name, sizeof(unit->name) - 1);
    strncpy(unit->location, "Main Panel", sizeof(unit->location) - 1);  // default
    unit->type = metric_type;
    unit->role = role;
    unit->stat_class = stat_period;
    
    // Data source: legacy ADC-typed binding
    unit->sources[0].source_type = RBPOWER_SOURCE_ADC_SENSOR;
    unit->sources[0].source_id = adc_channel;  // ADC channel
    unit->sources[0].sensor_profile_id = sensor_id;    // index into the sensor slots
    unit->sources[0].weight_factor = 1.0f;
    unit->sources[0].is_primary = true;
    unit->sources[0].invert_sign = false;
    unit->sources_count = 1;
    
    // Hierarchy
    unit->parent_unit_id = RBPOWER_HOME_INVALID_ID;
    unit->children_count = 0;
    
    // Unit parameters
    unit->bidirectional_metering = false;  // regular units are unidirectional
    unit->accumulate_energy = true;
    unit->rated_power_w = max_current * 230.0f;
    unit->max_current_a = max_current;
    unit->rated_voltage_v = 230.0f;
    
    // Thresholds
    unit->thresholds.over_power_w = unit->rated_power_w * 0.9f;
    unit->thresholds.under_voltage_v = 207.0f;
    unit->thresholds.over_current_a = max_current * 0.9f;
    unit->thresholds.over_voltage_v = 253.0f;
    unit->thresholds.enable_alerts = true;
    
    // Regular unit billing
    unit->billing.cost_per_kwh = 5.50f;  // base tariff
    unit->billing.time_of_use_enabled = false;
    unit->billing.current_tariff_zone = 0;
    
    unit->is_active = true;
    unit->created_timestamp_us = 0;
    
    ctx->units_count++;
    ctx->status.units_configured = true;
    
    ESP_LOGI(TAG, "Unit added: name='%s', unit_UID=0x%04X, adc_channel=%d, type=%d, max_current=%.1fA", 
             name, unit_uid, adc_channel, metric_type, max_current);
    
    return RBGRID_CONFIG_OK;
}


// ========== TIME CONFIGURATION ==========

int rbgrid_config_set_sntp_time(rbgrid_config_handle_t config,
                                const char* primary_server,
                                const char* backup_server,
                                const char* timezone)
{
    if (!config || !primary_server) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    /* A TZ string without an offset ("<UTC+9>") is a valid POSIX NAME with no
     * offset → the device ran its days in UTC while the config echoed "+9". An invalid string is NOT
     * stored: the previous valid TZ is kept (or UTC0). The SNTP servers are applied regardless —
     * an early return here would leave the device on MANUAL time (2024). */
    const char* tz_eff = timezone;
    if (timezone && timezone[0] && !rbtime_timezone_is_valid(timezone)) {
        const char* prev = ctx->hardware_config->time_config.timezone;
        tz_eff = (prev[0] && rbtime_timezone_is_valid(prev)) ? prev : "UTC0";
        ESP_LOGE(TAG, "timezone '%s' rejected: not a POSIX TZ with an offset (e.g. \"<UTC+9>-9\", \"CST-8\", "
                      "\"CET-1CEST,M3.5.0,M10.5.0/3\") — keeping '%s'; SNTP servers applied", timezone, tz_eff);
    }
    
    // Basic time parameters
    ctx->hardware_config->time_config.enable_time_sync = false;
    ctx->hardware_config->time_config.primary_source = RBTIME_SOURCE_SNTP;
    
    // SNTP servers
    strncpy(ctx->hardware_config->time_config.sntp_server, 
            primary_server, 
            sizeof(ctx->hardware_config->time_config.sntp_server) - 1);
    ctx->hardware_config->time_config.sntp_server[sizeof(ctx->hardware_config->time_config.sntp_server) - 1] = '\0';
    
    if (backup_server && strlen(backup_server) > 0) {
        strncpy(ctx->hardware_config->time_config.sntp_server_backup, 
                backup_server, 
                sizeof(ctx->hardware_config->time_config.sntp_server_backup) - 1);
        ctx->hardware_config->time_config.sntp_server_backup[sizeof(ctx->hardware_config->time_config.sntp_server_backup) - 1] = '\0';
    } else {
        // Standard backup server
        strncpy(ctx->hardware_config->time_config.sntp_server_backup, 
                "time.nist.gov", 
                sizeof(ctx->hardware_config->time_config.sntp_server_backup) - 1);
    }
    
    // Time zone
    if (tz_eff && strlen(tz_eff) > 0) {
        if (tz_eff != ctx->hardware_config->time_config.timezone) {
            strncpy(ctx->hardware_config->time_config.timezone, 
                    tz_eff, 
                    sizeof(ctx->hardware_config->time_config.timezone) - 1);
            ctx->hardware_config->time_config.timezone[sizeof(ctx->hardware_config->time_config.timezone) - 1] = '\0';
        }
    } else {
        // default UTC
        strncpy(ctx->hardware_config->time_config.timezone, 
                "UTC+0", 
                sizeof(ctx->hardware_config->time_config.timezone) - 1);
    }
    
    // Defaults for values not set yet
    if (ctx->hardware_config->time_config.sntp_timeout_ms == 0) {
        ctx->hardware_config->time_config.sntp_timeout_ms = 10000;  // 10 s
    }
    if (ctx->hardware_config->time_config.resync_interval_sec == 0) {
        ctx->hardware_config->time_config.resync_interval_sec = 3600;  // 1 h
    }
    if (ctx->hardware_config->time_config.sync_retry_count == 0) {
        ctx->hardware_config->time_config.sync_retry_count = 3;
    }
    
    ctx->hardware_config->time_config.force_sync_on_startup = true;
    ctx->hardware_config->time_config.enable_time_logging = true;
    
    ESP_LOGI(TAG, "SNTP time configured: primary=%s, backup=%s, timezone=%s", 
             primary_server, 
             backup_server ? backup_server : "time.nist.gov",
             timezone ? timezone : "UTC+0");
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_time_fallback(rbgrid_config_handle_t config,
                                    bool enable_rtc,
                                    bool enable_manual,
                                    uint64_t manual_time_sec)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // RTC fallback
    ctx->hardware_config->time_config.enable_rtc_fallback = enable_rtc;
    
    // Manual fallback
    if (enable_manual) {
        ctx->hardware_config->time_config.fallback_source = RBTIME_SOURCE_MANUAL;
        ctx->hardware_config->time_config.enable_cpu_time_fallback = true;
        
        // Manual time
        if (manual_time_sec > 0) {
            // seconds -> microseconds
            ctx->hardware_config->time_config.manual_time_us = manual_time_sec * 1000000ULL;
        } else {
            // default: 2024-01-01 00:00:00 UTC
            ctx->hardware_config->time_config.manual_time_us = 1704067200000000ULL;
        }
    } else {
        ctx->hardware_config->time_config.fallback_source = RBTIME_SOURCE_NONE;
        ctx->hardware_config->time_config.enable_cpu_time_fallback = false;
    }
    
    ESP_LOGI(TAG, "Time fallback configured: RTC=%s, Manual=%s, manual_time=%llu", 
             enable_rtc ? "YES" : "NO",
             enable_manual ? "YES" : "NO",
             (unsigned long long)(ctx->hardware_config->time_config.manual_time_us / 1000000ULL));
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_time_advanced(rbgrid_config_handle_t config,
                                    uint32_t sync_interval_sec,
                                    uint32_t timeout_ms,
                                    uint8_t retry_count,
                                    bool enable_dst)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Advanced parameters
    if (sync_interval_sec > 0) {
        ctx->hardware_config->time_config.resync_interval_sec = sync_interval_sec;
    }
    
    if (timeout_ms > 0 && timeout_ms <= 60000) {  // at most 60 s
        ctx->hardware_config->time_config.sntp_timeout_ms = timeout_ms;
    }
    
    if (retry_count > 0 && retry_count <= 10) {  // at most 10 retries
        ctx->hardware_config->time_config.sync_retry_count = retry_count;
    }
    
    ctx->hardware_config->time_config.enable_dst = enable_dst;
    
    // Additional parameters
    ctx->hardware_config->time_config.max_drift_sec = 60;  // maximum drift 60 s
    
    ESP_LOGI(TAG, "Advanced time config: interval=%lu s, timeout=%lu ms, retries=%u, DST=%s",
             (unsigned long)ctx->hardware_config->time_config.resync_interval_sec,
             (unsigned long)ctx->hardware_config->time_config.sntp_timeout_ms,
             (unsigned int)ctx->hardware_config->time_config.sync_retry_count,
             enable_dst ? "YES" : "NO");
    
    return RBGRID_CONFIG_OK;
}

// ========== TELEMETRY CONFIGURATION ==========

int rbgrid_config_set_telemetry_basic(rbgrid_config_handle_t config,
                                      bool enabled,
                                      uint32_t json_buffer_size,
                                      bool compact_json)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Store the basic telemetry settings
    ctx->telemetry.config.telemetry_enabled = enabled;
    ctx->telemetry.config.json_buffer_size = json_buffer_size > 0 ? json_buffer_size : 4096;
    ctx->telemetry.config.compact_json = compact_json;
    
    ctx->telemetry.configured = true;
    ctx->status.telemetry_configured = true;
    
    ESP_LOGI(TAG, "Telemetry basic config: enabled=%s, buffer=%lu, compact=%s",
             enabled ? "YES" : "NO",
             (unsigned long)ctx->telemetry.config.json_buffer_size,
             compact_json ? "YES" : "NO");
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_telemetry_mqtt(rbgrid_config_handle_t config,
                                     bool enabled,
                                     const char* broker_url,
                                     const char* topic,
                                     uint8_t qos,
                                     uint32_t send_interval_ms)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // MQTT channel
    rbpower_telemetry_channel_t* mqtt = &ctx->telemetry.config.channels[RBPOWER_TELEMETRY_MQTT];
    mqtt->enabled = enabled;
    mqtt->send_interval_ms = send_interval_ms > 0 ? send_interval_ms : 3000;
    mqtt->config.mqtt.broker_url = broker_url;
    mqtt->config.mqtt.topic = topic ? topic : "rbpower/measurements";
    mqtt->config.mqtt.qos = (qos <= 2) ? qos : 1;
    
    // Defaults when not configured
    if (mqtt->retry_count == 0) {
        mqtt->retry_count = 3;
    }
    if (mqtt->timeout_ms == 0) {
        mqtt->timeout_ms = 5000;
    }
    
    ctx->telemetry.configured = true;
    ctx->status.telemetry_configured = true;
    
    ESP_LOGI(TAG, "MQTT telemetry: enabled=%s, broker=%s, topic=%s, qos=%u, interval=%lu ms",
             enabled ? "YES" : "NO",
             broker_url ? broker_url : "NULL",
             mqtt->config.mqtt.topic,
             (unsigned int)mqtt->config.mqtt.qos,
             (unsigned long)mqtt->send_interval_ms);
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_telemetry_http(rbgrid_config_handle_t config,
                                     bool enabled,
                                     const char* endpoint_url,
                                     const char* auth_token,
                                     uint32_t send_interval_ms)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // HTTP channel
    rbpower_telemetry_channel_t* http = &ctx->telemetry.config.channels[RBPOWER_TELEMETRY_HTTP];
    http->enabled = enabled;
    http->send_interval_ms = send_interval_ms > 0 ? send_interval_ms : 15000;
    http->config.http.endpoint_url = endpoint_url;
    http->config.http.auth_token = auth_token;
    
    // Defaults when not configured
    if (http->retry_count == 0) {
        http->retry_count = 2;
    }
    if (http->timeout_ms == 0) {
        http->timeout_ms = 10000;
    }
    
    ctx->telemetry.configured = true;
    ctx->status.telemetry_configured = true;
    
    ESP_LOGI(TAG, "HTTP telemetry: enabled=%s, endpoint=%s, auth=%s, interval=%lu ms",
             enabled ? "YES" : "NO",
             endpoint_url ? endpoint_url : "NULL",
             auth_token ? "***" : "NULL",
             (unsigned long)http->send_interval_ms);
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_telemetry_websocket(rbgrid_config_handle_t config,
                                          bool enabled,
                                          const char* server_url,
                                          uint16_t port,
                                          uint32_t send_interval_ms)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // WebSocket channel
    rbpower_telemetry_channel_t* ws = &ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET];
    ws->enabled = enabled;
    ws->send_interval_ms = send_interval_ms > 0 ? send_interval_ms : 1000;
    ws->config.websocket.server_url = server_url;
    ws->config.websocket.port = port > 0 ? port : 8080;
    
    // Defaults when not configured
    if (ws->retry_count == 0) {
        ws->retry_count = 5;
    }
    if (ws->timeout_ms == 0) {
        ws->timeout_ms = 3000;
    }
    
    ctx->telemetry.configured = true;
    ctx->status.telemetry_configured = true;
    
    ESP_LOGI(TAG, "WebSocket telemetry: enabled=%s, server=%s:%u, interval=%lu ms",
             enabled ? "YES" : "NO",
             server_url ? server_url : "NULL",
             (unsigned int)ws->config.websocket.port,
             (unsigned long)ws->send_interval_ms);
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_telemetry_channel_advanced(rbgrid_config_handle_t config,
                                                 rbpower_telemetry_protocol_t protocol,
                                                 uint32_t retry_count,
                                                 uint32_t timeout_ms)
{
    if (!config || protocol >= RBPOWER_TELEMETRY_PROTOCOLS_MAX) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Advanced channel parameters
    rbpower_telemetry_channel_t* channel = &ctx->telemetry.config.channels[protocol];
    
    if (retry_count > 0 && retry_count <= 10) {
        channel->retry_count = retry_count;
    }
    
    if (timeout_ms > 0 && timeout_ms <= 60000) {
        channel->timeout_ms = timeout_ms;
    }
    
    const char* protocol_names[] = {"MQTT", "HTTP", "WebSocket"};
    ESP_LOGI(TAG, "%s telemetry advanced: retries=%lu, timeout=%lu ms",
             protocol_names[protocol],
             (unsigned long)channel->retry_count,
             (unsigned long)channel->timeout_ms);
    
    return RBGRID_CONFIG_OK;
}

int rbgrid_config_set_telemetry_defaults(rbgrid_config_handle_t config)
{
    if (!config) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_config_context_t* ctx = (rbgrid_config_context_t*)config;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Basic settings
    ctx->telemetry.config.telemetry_enabled = true;
    ctx->telemetry.config.json_buffer_size = 4096;
    ctx->telemetry.config.compact_json = true;
    
    // MQTT off
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_MQTT].enabled = false;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_MQTT].send_interval_ms = 3000;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_MQTT].retry_count = 3;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_MQTT].timeout_ms = 5000;
    
    // HTTP off
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_HTTP].enabled = false;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_HTTP].send_interval_ms = 15000;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_HTTP].retry_count = 2;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_HTTP].timeout_ms = 10000;
    
    // WebSocket only
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET].enabled = true;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET].send_interval_ms = 3000;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET].retry_count = 5;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET].timeout_ms = 3000;
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET].config.websocket.server_url = "";
    ctx->telemetry.config.channels[RBPOWER_TELEMETRY_WEBSOCKET].config.websocket.port = 8080;
    
    ctx->telemetry.configured = true;
    ctx->status.telemetry_configured = true;
    
    ESP_LOGI(TAG, "Telemetry set to defaults: WebSocket only, interval=3000ms");
    
    return RBGRID_CONFIG_OK;
}

//=============================================================================
// TARIFF BUILDER - STRUCTURES AND IMPLEMENTATION
//=============================================================================

/**
 * @brief Tariff zone as collected by the builder (before it is applied).
 */
typedef struct {
    char zone_name[32];                     ///< Zone name (also used to guess zone_type)
    float rate_per_kwh;                     ///< Buying rate, currency units per kWh
    char time_range[16];                    ///< Time-of-use window "HH:MM-HH:MM", e.g. "23:00-07:00"
    uint8_t weekday_mask;                   ///< Weekday bit mask
    uint8_t start_month;                    ///< Season start month for seasonal zones (1-12)
    uint8_t end_month;                      ///< Season end month for seasonal zones (1-12)
    rbpower_tariff_zone_type_t zone_type;   ///< Zone type
    bool is_time_based;                     ///< Time-of-use zone
    bool is_weekday_based;                  ///< Weekday zone
    bool is_seasonal;                       ///< Seasonal zone
    float selling_rate_per_kwh;             ///< Selling rate, currency units per kWh (0 = not set)
} rbgrid_tariff_builder_zone_t;

/**
 * @brief Holiday as collected by the builder.
 */
typedef struct {
    uint16_t day_of_year;                   ///< Day of year (1-366)
    char name[32];                          ///< Holiday name
    float rate_per_kwh;                     ///< Rate for this day, currency units per kWh
} rbgrid_tariff_builder_holiday_t;

/**
 * @brief Fixed fee as collected by the builder.
 */
typedef struct {
    char fee_name[32];                      ///< Fee name
    float amount;                           ///< Amount, in the plan currency
    rbpower_tariff_fixed_type_t fee_type;   ///< Fee type
} rbgrid_tariff_builder_fee_t;

/**
 * @brief Tariff builder context behind rbgrid_tariff_builder_t.
 */
typedef struct rbgrid_tariff_builder_s {
    // Base settings
    char currency[8];                       ///< Currency code, e.g. "EUR", "USD"
    char region[8];                         ///< Region code, e.g. "DE", "US"
    char timezone[32];                      ///< Time zone name, e.g. "Europe/Berlin"
    
    // Tariff zones
    rbgrid_tariff_builder_zone_t zones[RBGRID_TARIFF_BUILDER_MAX_ZONES];  ///< Tariff zones
    uint8_t zones_count;                    ///< Number of valid entries in zones
    
    // Holidays
    rbgrid_tariff_builder_holiday_t holidays[RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS];  ///< Holidays
    uint8_t holidays_count;                 ///< Number of valid entries in holidays
    
    // Fixed fees
    rbgrid_tariff_builder_fee_t fixed_fees[RBGRID_TARIFF_BUILDER_MAX_FIXED_FEES];  ///< Fixed fees
    uint8_t fixed_fees_count;               ///< Number of valid entries in fixed_fees
    
    // Net metering
    bool net_metering_enabled;              ///< Net metering enabled
    float default_selling_rate_per_kwh;     ///< Default selling rate per kWh (0 = buying rate x net_ratio)
    float net_ratio;                        ///< Selling/buying ratio, 0..1
    
    // Progressive (tiered) pricing
    bool progressive_enabled;               ///< Progressive tiers enabled
    float tier_thresholds_kwh[4];           ///< Tier thresholds, kWh
    float tier_rates_per_kwh[4];            ///< Tier rates, currency units per kWh
    uint8_t tiers_count;                    ///< Number of valid tiers (1-4)
    
    // State flags
    bool is_initialized;                    ///< Builder initialized
    bool has_base_settings;                 ///< Currency and region set
    
} rbgrid_tariff_builder_context_t;

//=============================================================================
// TARIFF BUILDER FUNCTIONS
//=============================================================================

rbgrid_tariff_builder_t rbgrid_tariff_builder_create(const char* currency, const char* region)
{
    if (!currency || !region) {
        ESP_LOGE(TAG, "Invalid currency or region");
        return NULL;
    }
    
    rbgrid_tariff_builder_context_t* builder = malloc(sizeof(rbgrid_tariff_builder_context_t));
    if (!builder) {
        ESP_LOGE(TAG, "Failed to allocate tariff builder");
        return NULL;
    }
    
    // Clear the structure
    memset(builder, 0, sizeof(rbgrid_tariff_builder_context_t));
    
    // Base settings
    strncpy(builder->currency, currency, sizeof(builder->currency) - 1);
    strncpy(builder->region, region, sizeof(builder->region) - 1);
    
    // Flags
    builder->is_initialized = true;
    builder->has_base_settings = true;
    
    // Default net metering ratio
    builder->net_ratio = 0.7f;
    
    ESP_LOGI(TAG, "Tariff builder created: %s, %s", currency, region);
    
    return (rbgrid_tariff_builder_t)builder;
}

rbgrid_tariff_builder_t rbgrid_tariff_builder_create_empty(void)
{
    rbgrid_tariff_builder_context_t* builder = malloc(sizeof(rbgrid_tariff_builder_context_t));
    if (!builder) {
        ESP_LOGE(TAG, "Failed to allocate empty tariff builder");
        return NULL;
    }
    
    // Clear the structure
    memset(builder, 0, sizeof(rbgrid_tariff_builder_context_t));
    
    // Flags
    builder->is_initialized = true;
    builder->has_base_settings = false;
    
    // Default net metering ratio
    builder->net_ratio = 0.7f;
    
    ESP_LOGI(TAG, "Empty tariff builder created");
    
    return (rbgrid_tariff_builder_t)builder;
}

rbgrid_config_error_t rbgrid_tariff_builder_set_region_settings(
    rbgrid_tariff_builder_t builder,
    const char* currency,
    const char* region, 
    const char* timezone)
{
    if (!builder || !currency || !region) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Apply the settings
    strncpy(ctx->currency, currency, sizeof(ctx->currency) - 1);
    strncpy(ctx->region, region, sizeof(ctx->region) - 1);
    
    if (timezone) {
        strncpy(ctx->timezone, timezone, sizeof(ctx->timezone) - 1);
    }
    
    ctx->has_base_settings = true;
    
    ESP_LOGI(TAG, "Builder region settings updated: %s, %s", currency, region);
    
    return RBGRID_CONFIG_OK;
}

void rbgrid_tariff_builder_destroy(rbgrid_tariff_builder_t builder)
{
    if (!builder) {
        return;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    ESP_LOGI(TAG, "Destroying tariff builder");
    
    // Clear and free
    memset(ctx, 0, sizeof(rbgrid_tariff_builder_context_t));
    free(ctx);
}

//=============================================================================
// TARIFF ZONE FUNCTIONS
//=============================================================================

/**
 * @brief Guess the tariff zone type from the zone name (English keywords plus a few Russian stems).
 */
static rbpower_tariff_zone_type_t get_zone_type_from_name(const char* zone_name)
{
    if (!zone_name) return RBPOWER_TARIFF_ZONE_DAY;
    
    // Lower-case copy for matching
    char lower_name[32];
    strncpy(lower_name, zone_name, sizeof(lower_name) - 1);
    for (int i = 0; lower_name[i]; i++) {
        lower_name[i] = tolower(lower_name[i]);
    }
    
    if (strstr(lower_name, "night")) return RBPOWER_TARIFF_ZONE_NIGHT;
    if (strstr(lower_name, "morning") || strstr(lower_name, "утр")) return RBPOWER_TARIFF_ZONE_MORNING_PEAK;
    if (strstr(lower_name, "evening") || strstr(lower_name, "веч")) return RBPOWER_TARIFF_ZONE_EVENING_PEAK;
    if (strstr(lower_name, "peak") || strstr(lower_name, "пик")) return RBPOWER_TARIFF_ZONE_EVENING_PEAK;
    if (strstr(lower_name, "WEEKEND") || strstr(lower_name, "выходн")) return RBPOWER_TARIFF_ZONE_WEEKEND;
    if (strstr(lower_name, "holiday") || strstr(lower_name, "праздн")) return RBPOWER_TARIFF_ZONE_HOLIDAY;
    if (strstr(lower_name, "critical")) return RBPOWER_TARIFF_ZONE_CRITICAL_PEAK;
    
    return RBPOWER_TARIFF_ZONE_DAY; // default
}

rbgrid_config_error_t rbgrid_tariff_builder_add_time_zone(
    rbgrid_tariff_builder_t builder,
    const char* zone_name,
    float rate_per_kwh,
    const char* time_range)
{
    if (!builder || !zone_name || !time_range || rate_per_kwh <= 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->zones_count >= RBGRID_TARIFF_BUILDER_MAX_ZONES) {
        ESP_LOGE(TAG, "Maximum zones reached (%d)", RBGRID_TARIFF_BUILDER_MAX_ZONES);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }
    
    // Append the zone
    rbgrid_tariff_builder_zone_t* zone = &ctx->zones[ctx->zones_count];
    memset(zone, 0, sizeof(rbgrid_tariff_builder_zone_t));
    
    strncpy(zone->zone_name, zone_name, sizeof(zone->zone_name) - 1);
    zone->rate_per_kwh = rate_per_kwh;
    strncpy(zone->time_range, time_range, sizeof(zone->time_range) - 1);
    zone->zone_type = get_zone_type_from_name(zone_name);
    zone->is_time_based = true;
    
    ctx->zones_count++;
    
    ESP_LOGI(TAG, "Added time zone: %s, %.1f EUR/MWh, %s", 
             zone_name, rate_per_kwh, time_range);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_add_weekday_zone(
    rbgrid_tariff_builder_t builder,
    const char* zone_name,
    float rate_per_kwh,
    uint8_t weekday_mask)
{
    if (!builder || !zone_name || rate_per_kwh <= 0 || weekday_mask == 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->zones_count >= RBGRID_TARIFF_BUILDER_MAX_ZONES) {
        ESP_LOGE(TAG, "Maximum zones reached (%d)", RBGRID_TARIFF_BUILDER_MAX_ZONES);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }
    
    // Append the zone
    rbgrid_tariff_builder_zone_t* zone = &ctx->zones[ctx->zones_count];
    memset(zone, 0, sizeof(rbgrid_tariff_builder_zone_t));
    
    strncpy(zone->zone_name, zone_name, sizeof(zone->zone_name) - 1);
    zone->rate_per_kwh = rate_per_kwh;
    zone->weekday_mask = weekday_mask;
    zone->zone_type = get_zone_type_from_name(zone_name);
    zone->is_weekday_based = true;
    
    ctx->zones_count++;
    
    ESP_LOGI(TAG, "Added weekday zone: %s, %.1f EUR/MWh, mask 0x%02X", 
             zone_name, rate_per_kwh, weekday_mask);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_add_seasonal_zone(
    rbgrid_tariff_builder_t builder,
    const char* zone_name,
    float rate_per_kwh,
    uint8_t start_month,
    uint8_t end_month)
{
    if (!builder || !zone_name || rate_per_kwh <= 0 || 
        start_month < 1 || start_month > 12 || 
        end_month < 1 || end_month > 12) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->zones_count >= RBGRID_TARIFF_BUILDER_MAX_ZONES) {
        ESP_LOGE(TAG, "Maximum zones reached (%d)", RBGRID_TARIFF_BUILDER_MAX_ZONES);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }
    
    // Append the zone
    rbgrid_tariff_builder_zone_t* zone = &ctx->zones[ctx->zones_count];
    memset(zone, 0, sizeof(rbgrid_tariff_builder_zone_t));
    
    strncpy(zone->zone_name, zone_name, sizeof(zone->zone_name) - 1);
    zone->rate_per_kwh = rate_per_kwh;
    zone->start_month = start_month;
    zone->end_month = end_month;
    zone->zone_type = get_zone_type_from_name(zone_name);
    zone->is_seasonal = true;
    
    ctx->zones_count++;
    
    ESP_LOGI(TAG, "Added seasonal zone: %s, %.1f EUR/MWh, months %d-%d", 
             zone_name, rate_per_kwh, start_month, end_month);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_set_zone_selling_rate(
    rbgrid_tariff_builder_t builder,
    const char* zone_name,
    float selling_rate_per_kwh)
{
    if (!builder || !zone_name || selling_rate_per_kwh <= 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Find the zone by name
    for (uint8_t i = 0; i < ctx->zones_count; i++) {
        if (strcmp(ctx->zones[i].zone_name, zone_name) == 0) {
            ctx->zones[i].selling_rate_per_kwh = selling_rate_per_kwh;
            ESP_LOGI(TAG, "Set selling rate for zone %s: %.1f EUR/MWh", 
                     zone_name, selling_rate_per_kwh);
            return RBGRID_CONFIG_OK;
        }
    }
    
    ESP_LOGE(TAG, "Zone not found: %s", zone_name);
    return RBGRID_CONFIG_ERR_INVALID_PARAM;
}

//=============================================================================
// HOLIDAYS AND FIXED FEES
//=============================================================================

rbgrid_config_error_t rbgrid_tariff_builder_add_holiday_list(
    rbgrid_tariff_builder_t builder,
    const uint16_t* holiday_days,
    uint8_t holidays_count,
    float holiday_rate_per_kwh)
{
    if (!builder || !holiday_days || holidays_count == 0 || 
        holidays_count > RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS ||
        holiday_rate_per_kwh <= 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->holidays_count + holidays_count > RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS) {
        ESP_LOGE(TAG, "Too many holidays, max %d", RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }
    
    // Append the holidays
    for (uint8_t i = 0; i < holidays_count; i++) {
        if (holiday_days[i] < 1 || holiday_days[i] > 366) {
            ESP_LOGE(TAG, "Invalid day of year: %d", holiday_days[i]);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }
        
        rbgrid_tariff_builder_holiday_t* holiday = &ctx->holidays[ctx->holidays_count];
        holiday->day_of_year = holiday_days[i];
        holiday->rate_per_kwh = holiday_rate_per_kwh;
        snprintf(holiday->name, sizeof(holiday->name), "Holiday_%d", holiday_days[i]);
        
        ctx->holidays_count++;
    }
    
    ESP_LOGI(TAG, "Added %d holidays with rate %.1f EUR/MWh", holidays_count, holiday_rate_per_kwh);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_add_single_holiday(
    rbgrid_tariff_builder_t builder,
    uint16_t day_of_year,
    const char* holiday_name,
    float rate_per_kwh)
{
    if (!builder || day_of_year < 1 || day_of_year > 366 || rate_per_kwh <= 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->holidays_count >= RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS) {
        ESP_LOGE(TAG, "Maximum holidays reached (%d)", RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }
    
    // Append the holiday
    rbgrid_tariff_builder_holiday_t* holiday = &ctx->holidays[ctx->holidays_count];
    holiday->day_of_year = day_of_year;
    holiday->rate_per_kwh = rate_per_kwh;
    
    if (holiday_name) {
        strncpy(holiday->name, holiday_name, sizeof(holiday->name) - 1);
    } else {
        snprintf(holiday->name, sizeof(holiday->name), "Holiday_%d", day_of_year);
    }
    
    ctx->holidays_count++;
    
    ESP_LOGI(TAG, "Added holiday: %s (day %d), %.1f EUR/MWh", 
             holiday->name, day_of_year, rate_per_kwh);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_set_country_holidays(
    rbgrid_tariff_builder_t builder,
    const char* country_code,
    float holiday_rate_per_kwh)
{
    if (!builder || !country_code || holiday_rate_per_kwh <= 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Built-in holiday sets per country
    uint16_t holidays[16];
    uint8_t count = 0;
    
    if (strcmp(country_code, "DE") == 0) {
        // German holidays (approximate days of year)
        uint16_t german_holidays[] = {1, 110, 121, 275, 358, 359, 365}; // New Year, Easter, Labour Day, Unity Day, Christmas
        count = 7;
        memcpy(holidays, german_holidays, sizeof(german_holidays));
    } else if (strcmp(country_code, "US") == 0) {
        // US holidays
        uint16_t us_holidays[] = {1, 50, 145, 186, 327, 359}; // New Year, Presidents Day, Memorial Day, Independence Day, Thanksgiving, Christmas
        count = 6;
        memcpy(holidays, us_holidays, count * sizeof(uint16_t));
    } else if (strcmp(country_code, "RU") == 0) {
        // Russian holidays
        uint16_t ru_holidays[] = {1, 7, 58, 125, 129, 159, 316, 359}; // New Year, Orthodox Christmas, Feb 23, May 9, May 1, Jun 8, Jun 12, Nov 4 (approximate days of year)
        count = 8;
        memcpy(holidays, ru_holidays, count * sizeof(uint16_t));
    } else {
        ESP_LOGE(TAG, "Unsupported country code: %s", country_code);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    return rbgrid_tariff_builder_add_holiday_list(builder, holidays, count, holiday_rate_per_kwh);
}

rbgrid_config_error_t rbgrid_tariff_builder_set_monthly_fee(
    rbgrid_tariff_builder_t builder,
    float monthly_amount)
{
    if (!builder || monthly_amount < 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    return rbgrid_tariff_builder_add_custom_fee(builder, "Monthly Fee", 
                                               monthly_amount, RBPOWER_TARIFF_FIXED_MONTHLY);
}

rbgrid_config_error_t rbgrid_tariff_builder_set_power_fee(
    rbgrid_tariff_builder_t builder,
    float rate_per_kw_month)
{
    if (!builder || rate_per_kw_month < 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    return rbgrid_tariff_builder_add_custom_fee(builder, "Power Fee", 
                                               rate_per_kw_month, RBPOWER_TARIFF_FIXED_POWER_BASED);
}

rbgrid_config_error_t rbgrid_tariff_builder_add_custom_fee(
    rbgrid_tariff_builder_t builder,
    const char* fee_name,
    float amount,
    rbpower_tariff_fixed_type_t fee_type)
{
    if (!builder || !fee_name || amount < 0) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    if (ctx->fixed_fees_count >= RBGRID_TARIFF_BUILDER_MAX_FIXED_FEES) {
        ESP_LOGE(TAG, "Maximum fixed fees reached (%d)", RBGRID_TARIFF_BUILDER_MAX_FIXED_FEES);
        return RBGRID_CONFIG_ERR_SENSOR_LIMIT;
    }
    
    // Append the fixed fee
    rbgrid_tariff_builder_fee_t* fee = &ctx->fixed_fees[ctx->fixed_fees_count];
    strncpy(fee->fee_name, fee_name, sizeof(fee->fee_name) - 1);
    fee->amount = amount;
    fee->fee_type = fee_type;
    
    ctx->fixed_fees_count++;
    
    ESP_LOGI(TAG, "Added fixed fee: %s, %.2f %s", 
             fee_name, amount, ctx->currency);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_set_net_metering(
    rbgrid_tariff_builder_t builder,
    float selling_rate_per_kwh,
    float net_ratio)
{
    // selling_rate_per_kwh == 0 is allowed: ratio-based selling rate
    // selling_rate_per_kwh < 0 is an error
    if (!builder || selling_rate_per_kwh < 0 || net_ratio < 0 || net_ratio > 1.0f) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    ctx->net_metering_enabled = true;
    ctx->default_selling_rate_per_kwh = selling_rate_per_kwh;
    ctx->net_ratio = net_ratio;

    if (selling_rate_per_kwh > 0) {
        ESP_LOGI(TAG, "Net metering enabled: fixed rate %.3f EUR/kWh, ratio %.1f",
                 selling_rate_per_kwh, net_ratio);
    } else {
        ESP_LOGI(TAG, "Net metering enabled: calculated rate (buying * %.1f), ratio %.1f",
                 net_ratio, net_ratio);
    }
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_tariff_builder_set_progressive_tiers(
    rbgrid_tariff_builder_t builder,
    const float* tier_thresholds_kwh,
    const float* tier_rates_per_kwh,
    uint8_t tiers_count)
{
    if (!builder || !tier_thresholds_kwh || !tier_rates_per_kwh || 
        tiers_count == 0 || tiers_count > 4) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    ctx->progressive_enabled = true;
    ctx->tiers_count = tiers_count;
    
    for (uint8_t i = 0; i < tiers_count; i++) {
        ctx->tier_thresholds_kwh[i] = tier_thresholds_kwh[i];
        ctx->tier_rates_per_kwh[i] = tier_rates_per_kwh[i];
    }
    
    ESP_LOGI(TAG, "Progressive tariff enabled with %d tiers", tiers_count);
    
    return RBGRID_CONFIG_OK;
}

//=============================================================================
// TARIFF PLAN VALIDATION AND APPLY
//=============================================================================

/** @brief Parse a tariff time-of-use window "HH:MM-HH:MM" (the same format parse_time_range_to_condition in
 * rbhome_tariff.c consumes at runtime) into 1-2 normalized [start,end) minute intervals — 2 when the
 * window wraps midnight (start>end). Returns the interval count (1 or 2), or 0 on malformed /
 * out-of-range / zero-length input. Kept semantically identical to the engine parser so add-time
 * validation and runtime selection never disagree. */
static int tariff_time_range_intervals(const char* tr, int out_s[2], int out_e[2])
{
    unsigned sh, sm, eh, em;
    if (!tr || sscanf(tr, "%u:%u-%u:%u", &sh, &sm, &eh, &em) != 4) return 0;
    if (sh > 23 || eh > 23 || sm > 59 || em > 59) return 0;
    int s = (int)(sh * 60 + sm), e = (int)(eh * 60 + em);
    if (s == e) return 0;                                   // zero-length window — zone never active
    if (s < e)  { out_s[0] = s; out_e[0] = e; return 1; }   // same-day [s,e)
    out_s[0] = s; out_e[0] = 1440; out_s[1] = 0; out_e[1] = e; return 2;  // wraps midnight: [s,24h)+[0,e)
}

rbgrid_config_error_t rbgrid_tariff_builder_validate(rbgrid_tariff_builder_t builder)
{
    if (!builder) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    rbgrid_tariff_builder_context_t* ctx = (rbgrid_tariff_builder_context_t*)builder;
    
    if (!ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // Base settings
    if (!ctx->has_base_settings) {
        ESP_LOGE(TAG, "Base settings (currency, region) not set");
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // At least one tariff zone
    if (ctx->zones_count == 0) {
        ESP_LOGE(TAG, "At least one tariff zone is required");
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Check each zone
    for (uint8_t i = 0; i < ctx->zones_count; i++) {
        rbgrid_tariff_builder_zone_t* zone = &ctx->zones[i];
        
        if (zone->rate_per_kwh <= 0) {
            ESP_LOGE(TAG, "Zone %s has invalid rate: %.2f", zone->zone_name, zone->rate_per_kwh);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }
        
        // Time-of-use zones: the range must be "HH:MM-HH:MM" with valid
        // hours/minutes and non-zero length. Checking only for non-empty let a malformed/zero-length
        // range silently create a zone that is NEVER active = a silent billing error.
        // Rejected at add time.
        if (zone->is_time_based) {
            int s_[2], e_[2];
            if (strlen(zone->time_range) == 0) {
                ESP_LOGE(TAG, "Time zone %s has empty time range", zone->zone_name);
                return RBGRID_CONFIG_ERR_INVALID_PARAM;
            }
            if (tariff_time_range_intervals(zone->time_range, s_, e_) == 0) {
                ESP_LOGE(TAG, "Time zone %s: malformed/out-of-range/zero-length time range '%s' "
                              "(need HH:MM-HH:MM within 00:00-23:59, start!=end)",
                         zone->zone_name, zone->time_range);
                return RBGRID_CONFIG_ERR_INVALID_PARAM;
            }
        }
        
        // Seasonal zones
        if (zone->is_seasonal) {
            if (zone->start_month < 1 || zone->start_month > 12 ||
                zone->end_month < 1 || zone->end_month > 12) {
                ESP_LOGE(TAG, "Seasonal zone %s has invalid months: %d-%d", 
                         zone->zone_name, zone->start_month, zone->end_month);
                return RBGRID_CONFIG_ERR_INVALID_PARAM;
            }
        }
    }
    
    // Add-time OVERLAP rejection: two pure time-of-use zones (time-based, NON-seasonal) whose HH:MM
    // windows intersect make tariff selection ambiguous for the overlapping minutes. The engine would
    // priority-resolve it silently; at config-add such an overlap is almost always a user mistake →
    // reject it loudly. Seasonal zones are excluded (their season dimension
    // legitimately separates same-clock windows). GAPS are NOT rejected — uncovered minutes correctly
    // fall back to the default rate.
    for (uint8_t i = 0; i < ctx->zones_count; i++) {
        const rbgrid_tariff_builder_zone_t* zi = &ctx->zones[i];
        if (!zi->is_time_based || zi->is_seasonal) continue;
        int si[2], ei[2]; int ni = tariff_time_range_intervals(zi->time_range, si, ei);
        if (ni == 0) continue;  // already rejected in the per-zone loop; defensive
        for (uint8_t j = (uint8_t)(i + 1); j < ctx->zones_count; j++) {
            const rbgrid_tariff_builder_zone_t* zj = &ctx->zones[j];
            if (!zj->is_time_based || zj->is_seasonal) continue;
            int sj[2], ej[2]; int nj = tariff_time_range_intervals(zj->time_range, sj, ej);
            if (nj == 0) continue;
            for (int a = 0; a < ni; a++) {
                for (int b = 0; b < nj; b++) {
                    if (si[a] < ej[b] && sj[b] < ei[a]) {   // half-open interval intersection
                        ESP_LOGE(TAG, "Tariff zones '%s' (%s) and '%s' (%s) OVERLAP in time-of-use "
                                      "window — ambiguous, rejected", zi->zone_name, zi->time_range,
                                 zj->zone_name, zj->time_range);
                        return RBGRID_CONFIG_ERR_INVALID_PARAM;
                    }
                }
            }
        }
    }

    // Holidays
    for (uint8_t i = 0; i < ctx->holidays_count; i++) {
        rbgrid_tariff_builder_holiday_t* holiday = &ctx->holidays[i];
        
        if (holiday->day_of_year < 1 || holiday->day_of_year > 366) {
            ESP_LOGE(TAG, "Holiday %s has invalid day: %d", holiday->name, holiday->day_of_year);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }
        
        if (holiday->rate_per_kwh <= 0) {
            ESP_LOGE(TAG, "Holiday %s has invalid rate: %.2f", holiday->name, holiday->rate_per_kwh);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }
    }
    
    // Net metering
    if (ctx->net_metering_enabled) {
        if (ctx->net_ratio < 0 || ctx->net_ratio > 1.0f) {
            ESP_LOGE(TAG, "Invalid net ratio: %.2f", ctx->net_ratio);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }

        // selling_rate = 0 is allowed: ratio-based selling rate
        // only negative values are an error
        if (ctx->default_selling_rate_per_kwh < 0) {
            ESP_LOGE(TAG, "Invalid selling rate: %.2f (must be >= 0)", ctx->default_selling_rate_per_kwh);
            return RBGRID_CONFIG_ERR_INVALID_PARAM;
        }

        // Log the net metering mode
        if (ctx->default_selling_rate_per_kwh == 0) {
            ESP_LOGI(TAG, "Net metering mode: calculated (buying * %.2f)", ctx->net_ratio);
        } else {
            ESP_LOGI(TAG, "Net metering mode: fixed rate %.3f", ctx->default_selling_rate_per_kwh);
        }
    }
    
    ESP_LOGI(TAG, "Tariff builder validation passed: %d zones, %d holidays, %d fees", 
             ctx->zones_count, ctx->holidays_count, ctx->fixed_fees_count);
    
    return RBGRID_CONFIG_OK;
}

rbgrid_config_error_t rbgrid_config_apply_tariff_plan(
    rbgrid_config_handle_t config,
    rbgrid_tariff_builder_t builder)
{
    if (!config || !builder) {
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }
    
    // Validate the builder
    rbgrid_config_error_t validation_result = rbgrid_tariff_builder_validate(builder);
    if (validation_result != RBGRID_CONFIG_OK) {
        ESP_LOGE(TAG, "Tariff builder validation failed: %d", validation_result);
        return validation_result;
    }
    
    rbgrid_tariff_builder_context_t* builder_ctx = (rbgrid_tariff_builder_context_t*)builder;
    rbgrid_config_context_t* config_ctx = (rbgrid_config_context_t*)config;
    
    if (!config_ctx->is_initialized) {
        return RBGRID_CONFIG_ERR_NOT_INITIALIZED;
    }
    
    // === APPLY TO THE LIVE TARIFF ENGINE ===
    ESP_LOGI(TAG, "=== Applying tariff plan from builder ===");
    ESP_LOGI(TAG, "Currency: %s, Region: %s", builder_ctx->currency, builder_ctx->region);
    ESP_LOGI(TAG, "Zones: %d, Holidays: %d, Fixed fees: %d", 
             builder_ctx->zones_count, builder_ctx->holidays_count, builder_ctx->fixed_fees_count);
    
    // Diagnostic logging before the hand-off
    ESP_LOGI(TAG, "Before passing to rbhome_tariff: zones_count=%d, pointer=%p", 
             builder_ctx->zones_count, builder_ctx);
    ESP_LOGI(TAG, "Original struct size: %d bytes", sizeof(*builder_ctx));
    ESP_LOGI(TAG, "Size of zone: %d bytes", sizeof(rbgrid_tariff_builder_zone_t));
    ESP_LOGI(TAG, "Offset to zones_count: %d bytes", 
             (uint8_t*)&builder_ctx->zones_count - (uint8_t*)builder_ctx);
    
    // Field-by-field accessor (no shared struct layout between the modules)
    extern esp_err_t rbpower_tariff_apply_from_builder_safe(
        const char* currency, const char* region,
        uint8_t zones_count, const void* zones_data,
        uint8_t holidays_count, const void* holidays_data,
        uint8_t fixed_fees_count, const void* fixed_fees_data,
        bool net_metering_enabled, float default_selling_rate_per_kwh, float net_ratio);

    // Apply the builder data to the live tariff engine through that accessor
    esp_err_t tariff_apply_result = rbpower_tariff_apply_from_builder_safe(
        builder_ctx->currency, builder_ctx->region,
        builder_ctx->zones_count, builder_ctx->zones,
        builder_ctx->holidays_count, builder_ctx->holidays,
        builder_ctx->fixed_fees_count, builder_ctx->fixed_fees,
        builder_ctx->net_metering_enabled, builder_ctx->default_selling_rate_per_kwh, builder_ctx->net_ratio);
    if (tariff_apply_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply tariff plan to rbhome_tariff system: %d", tariff_apply_result);
        return RBGRID_CONFIG_ERR_INVALID_PARAM;
    }

    // Zone summary (first 3 only, to keep the log short)
    for (uint8_t i = 0; i < builder_ctx->zones_count && i < 3; i++) {
        rbgrid_tariff_builder_zone_t* zone = &builder_ctx->zones[i];
        ESP_LOGI(TAG, "Zone[%d]: %s, %.1f EUR/kWh %s", 
                 i, zone->zone_name, zone->rate_per_kwh,
                 zone->is_time_based ? zone->time_range : "");
    }
    if (builder_ctx->zones_count > 3) {
        ESP_LOGI(TAG, "... and %d more zones", builder_ctx->zones_count - 3);
    }
    
    // Fixed fees
    if (builder_ctx->fixed_fees_count > 0) {
        ESP_LOGI(TAG, "Fixed fees: %d", builder_ctx->fixed_fees_count);
        for (uint8_t i = 0; i < builder_ctx->fixed_fees_count && i < 3; i++) {
            rbgrid_tariff_builder_fee_t* fee = &builder_ctx->fixed_fees[i];
            ESP_LOGI(TAG, "  Fee[%d]: %s = %.2f %s (%s)",
                     i, fee->fee_name, fee->amount, builder_ctx->currency,
                     fee->fee_type == RBPOWER_TARIFF_FIXED_MONTHLY ? "monthly" :
                     fee->fee_type == RBPOWER_TARIFF_FIXED_DAILY ? "daily" :
                     fee->fee_type == RBPOWER_TARIFF_FIXED_POWER_BASED ? "per kW" :
                     fee->fee_type == RBPOWER_TARIFF_FIXED_GRID_USAGE ? "grid usage" :
                     fee->fee_type == RBPOWER_TARIFF_FIXED_RENEWABLE_LEVY ? "renewable levy" :
                     fee->fee_type == RBPOWER_TARIFF_FIXED_TAX ? "tax" : "custom");
        }
        if (builder_ctx->fixed_fees_count > 3) {
            ESP_LOGI(TAG, "... and %d more fees", builder_ctx->fixed_fees_count - 3);
        }
    }

    // Net metering
    if (builder_ctx->net_metering_enabled) {
        ESP_LOGI(TAG, "Net metering: %.1f EUR/kWh, ratio %.1f",
                 builder_ctx->default_selling_rate_per_kwh, builder_ctx->net_ratio);
    }

    ESP_LOGI(TAG, "✅ Tariff plan applied successfully to rbhome_tariff system");
    
    return RBGRID_CONFIG_OK;
}

//=============================================================================
// PUBLIC ACCESSORS: REAL-TIME DATA
//=============================================================================

/** @brief Live home network state (defined in rbhome.c). */
extern rbpower_electrical_network_t g_home_network;

/* Copy the full 3 s voltage statistics (parameters documented in rbgrid_config_api.h). */
rbgrid_err_t rbgrid_realtime_get_voltage(statistics_3sec_final_t* voltage_stats)
{
    if (!voltage_stats) {
        return RBGRID_ERR_INVALID_ARG;
    }
    
    if (!g_home_network.specialized.has_voltage_bus) {
        ESP_LOGI(TAG, "Voltage bus not found");
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
    
    // Copy out via memcpy
    memcpy(voltage_stats, &vbus->statistics_3sec.final, sizeof(statistics_3sec_final_t));
    
    return RBGRID_OK;
}


/* Read the main supply real-time (200 ms) values (parameters documented in rbgrid_config_api.h). Every output pointer may be NULL. */
rbgrid_err_t rbgrid_realtime_get_main_supply(float* current_rms, 
                                          float* power_active, 
                                          rbpower_current_direction_t* direction,
                                          float* power_reactive,
                                          float* power_apparent,
                                          float* power_factor,
                                          uint32_t* samples_count,
                                          float* energy_increment)
{
    if (!g_home_network.specialized.has_main_supply) {
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
    
    if (current_rms) {
        *current_rms = main_supply->realtime_200ms.current_rms;
    }
    
    if (power_active) {
        *power_active = main_supply->realtime_200ms.power_active_w;
    }
    
    if (direction) {
        *direction = main_supply->realtime_200ms.direction;
    }
    
    if (power_reactive) {
        *power_reactive = main_supply->realtime_200ms.power_reactive_var;
    }
    
    if (power_apparent) {
        *power_apparent = main_supply->realtime_200ms.power_apparent_va;
    }
    
    if (power_factor) {
        *power_factor = main_supply->realtime_200ms.power_factor;
    }
    
    if (samples_count) {
        *samples_count = main_supply->realtime_200ms.samples_count;
    }
    
    if (energy_increment) {
        *energy_increment = main_supply->realtime_200ms.energy_increment_wh;
    }
    
    return RBGRID_OK;
}


/* Read the alternative supply real-time (200 ms) values (parameters documented in rbgrid_config_api.h). Every output pointer may be NULL. */
rbgrid_err_t rbgrid_realtime_get_alt_supply(float* current_rms, 
                                         float* power_active, 
                                         rbpower_current_direction_t* direction,
                                         float* power_reactive,
                                         float* power_apparent,
                                         float* power_factor,
                                         uint32_t* samples_count,
                                         float* energy_increment,
                                         float* efficiency,
                                         float* dc_power)
{
    if (!g_home_network.specialized.has_alternative_supply) {
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
    
    if (current_rms) {
        *current_rms = alt_supply->realtime_200ms.current_rms;
    }
    
    if (power_active) {
        *power_active = alt_supply->realtime_200ms.power_active_w;
    }
    
    if (direction) {
        *direction = alt_supply->realtime_200ms.direction;
    }
    
    if (power_reactive) {
        *power_reactive = alt_supply->realtime_200ms.power_reactive_var;
    }
    
    if (power_apparent) {
        *power_apparent = alt_supply->realtime_200ms.power_apparent_va;
    }
    
    if (power_factor) {
        *power_factor = alt_supply->realtime_200ms.power_factor;
    }
    
    if (samples_count) {
        *samples_count = alt_supply->realtime_200ms.samples_count;
    }
    
    if (energy_increment) {
        *energy_increment = alt_supply->realtime_200ms.energy_increment_wh;
    }
    
    if (efficiency) {
        *efficiency = alt_supply->realtime_200ms.efficiency_percent;
    }
    
    if (dc_power) {
        *dc_power = alt_supply->realtime_200ms.dc_power;
    }
    
    return RBGRID_OK;
}

/* Read a unit's real-time (200 ms) averages by unit_UID (parameters documented in rbgrid_config_api.h). Every output pointer may be NULL. */
rbgrid_err_t rbgrid_realtime_get_unit(rbgrid_unit_uid_t unit_uid,
                                   float* current_rms, 
                                   float* power_active,
                                   float* power_reactive,
                                   float* power_apparent,
                                   float* power_factor,
                                   uint32_t* samples_count)
{
    // Unit slot index by unit_UID
    uint8_t unit_index = find_unit_index_by_id((rbpower_unit_id_t)unit_uid);
    if (unit_index == 0xFF || !g_home_network.units[unit_index].has_valid_definition) {
        ESP_LOGE(TAG, "Unit with UID 0x%04X not found or invalid", unit_uid);
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    // The unit's 200 ms data
    const rbpower_unit_realtime_average_200ms_t* data_200ms = 
        &g_home_network.units[unit_index].realtime_average_200ms;
    
    // Fill the outputs
    if (current_rms) {
        *current_rms = data_200ms->average_200ms.current_rms_avg;
    }
    
    if (power_active) {
        *power_active = data_200ms->average_200ms.power_active_avg_w;
    }
    
    if (power_reactive) {
        *power_reactive = data_200ms->average_200ms.power_reactive_avg_var;
    }
    
    if (power_apparent) {
        *power_apparent = data_200ms->average_200ms.power_apparent_avg_va;
    }
    
    if (power_factor) {
        *power_factor = data_200ms->average_200ms.power_factor_avg;
    }
    
    if (samples_count) {
        *samples_count = data_200ms->average_200ms.samples_count;
    }
    
    return RBGRID_OK;
}


//=============================================================================
// PUBLIC ACCESSORS: SNAPSHOT DATA - 3 s statistics
//=============================================================================

/* Copy the full 3 s voltage statistics (parameters documented in rbgrid_config_api.h). */
rbgrid_err_t rbgrid_snapshot_get_voltage(statistics_3sec_final_t* voltage_stats)
{
    if (!voltage_stats) {
        return RBGRID_ERR_INVALID_ARG;
    }
    
    if (!g_home_network.specialized.has_voltage_bus) {
        ESP_LOGI(TAG, "Voltage bus not found");
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
    
    // Copy out via memcpy
    memcpy(voltage_stats, &vbus->statistics_3sec.final, sizeof(statistics_3sec_final_t));
    
    return RBGRID_OK;
}

/* Copy the main supply 3 s result base (parameters documented in rbgrid_config_api.h). */
rbgrid_err_t rbgrid_snapshot_get_main_supply(rbgrid_snapshot_base_t* m_sup_snapshot)
{
    if (!m_sup_snapshot) {
        return RBGRID_ERR_INVALID_ARG;
    }
    
    if (!g_home_network.specialized.has_main_supply) {
        ESP_LOGI(TAG, "Main supply not found");
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
    
    // Copy the 3 s result
    memcpy(m_sup_snapshot, &main_supply->result_base_3sec, sizeof(rbgrid_snapshot_base_t));
    
    return RBGRID_OK;
}

/* Copy the alternative supply 3 s result base (parameters documented in rbgrid_config_api.h). */
rbgrid_err_t rbgrid_snapshot_get_alt_supply(rbgrid_snapshot_base_t* alt_sup_snapshot)
{
    if (!alt_sup_snapshot) {
        return RBGRID_ERR_INVALID_ARG;
    }
    
    if (!g_home_network.specialized.has_alternative_supply) {
        ESP_LOGI(TAG, "Alternative supply not found");
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
    
    // Copy the 3 s result
    memcpy(alt_sup_snapshot, &alt_supply->result_base_3sec, sizeof(rbgrid_snapshot_base_t));
    
    return RBGRID_OK;
}

/* Copy a unit's 3 s result base by unit_UID (parameters documented in rbgrid_config_api.h). */
rbgrid_err_t rbgrid_snapshot_get_unit(rbgrid_unit_uid_t unit_uid, rbgrid_snapshot_base_t* unit_snapshot_base)
{
    if (!unit_snapshot_base) {
        return RBGRID_ERR_INVALID_ARG;
    }
    
    // Unit slot index by unit_UID
    uint8_t unit_index = find_unit_index_by_id((rbpower_unit_id_t)unit_uid);
    if (unit_index == 0xFF || !g_home_network.units[unit_index].has_valid_definition) {
        ESP_LOGE(TAG, "Unit with UID 0x%04X not found or invalid", unit_uid);
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }
    
    // The unit's completed 3 s period
    const rbpower_period_statistics_t* completed_3sec_period = &g_home_network.units[unit_index].statistics.periods[RBPOWER_STAT_PERIOD_3SEC];
    
    // Copy the 3 s result
    memcpy(unit_snapshot_base, &completed_3sec_period->result_base, sizeof(rbgrid_snapshot_base_t));

    return RBGRID_OK;
}

// ========== EXTERNAL FLASH ==========

/* True when the external flash is available. */
bool rbgrid_flash_is_available(void) {
    return rbpower_external_flash_is_available();
}

/* True when OTP validation passed. */
bool rbgrid_otp_is_validated(void) {
    return rbpower_external_flash_otp_validated();
}

/* Device serial number (from OTP). */
uint32_t rbgrid_get_device_serial(void) {
    return rbpower_external_flash_get_serial();
}

/* Extended flash status. */
rbgrid_flash_status_t rbgrid_get_flash_status(void) {
    rbpower_flash_status_t power_status = rbpower_external_flash_get_status();
    
    rbgrid_flash_status_t status = {
        .external_flash_available = power_status.external_flash_available,
        .otp_validated = power_status.otp_validated,
        .device_serial = power_status.device_serial,
        .config_partition_mounted = power_status.config_partition_mounted,
        .config_partition_size = power_status.config_partition_size,
        .config_used_space = power_status.config_used_space,
        .error_message = power_status.error_message
    };
    
    return status;
}

/**
 * @brief One-shot esp_timer callback: hard restart when the orderly reboot path in
 *        rbgrid_system_reboot() does not finish in time.
 */
static void reboot_hard_fallback(void* arg)
{
    (void)arg;
    ESP_LOGE("RBGRID", "Planned reboot: orderly path did not finish in time — hard esp_restart()");
    { rbpower_mark_planned_restart(); esp_restart(); }
}

esp_err_t rbgrid_system_reboot(uint32_t delay_ms) {
    ESP_LOGI(TAG, "System reboot requested, delay: %lu ms", (unsigned long)delay_ms);
    /* Hard fallback: a planned reboot must ALWAYS happen. The orderly path below
     * (LWT, open-hour flush, et persist, deinit) normally takes ~1-2 s; if anything in it blocks
     * (seen after a WebSocket client storm: 200 OK, no restart, no LWT), this one-shot restarts
     * the chip anyway. esp_timer callbacks run on their own high-priority task. */
    {
        static esp_timer_handle_t s_fallback = NULL;
        if (!s_fallback) {
            const esp_timer_create_args_t ta = { .callback = reboot_hard_fallback, .arg = NULL,
                                                 .dispatch_method = ESP_TIMER_TASK, .name = "reboot_fb" };
            if (esp_timer_create(&ta, &s_fallback) != ESP_OK) s_fallback = NULL;
        }
        uint32_t d = (delay_ms > 3000) ? 3000 : delay_ms;
        if (s_fallback) esp_timer_start_once(s_fallback, ((uint64_t)d + 8000ull) * 1000ull);
    }

    // Cap the delay at 3 s
    if (delay_ms > 3000) {
        delay_ms = 3000;
        ESP_LOGW(TAG, "Reboot delay limited to 3000ms");
    }

    /* Contract "LWT on reboot": explicitly publish the retained "offline" to the
     * status topic BEFORE the restart. esp_restart() drops TCP without a FIN, so the broker would only
     * notice on keepalive (minutes), by which time the restarted client has already set "online",
     * i.e. without this publish the offline phase is never observable. The QoS1 packet goes out during
     * deinit+delay_ms (the esp-mqtt client lives until esp_restart). */
    (void)rbmqtt_publish_offline();

    /* Planned restart = don't lose the open hour: flush HOUR accumulators as PARTIAL records
     * (otherwise the RAM-only open hour is lost on every reboot/OTA). */
    (void)rbpower_stats_flush_open_hours();
    /* ... and pin the et totalizers to NVS NOW: the RTC image does not survive an image change
     * (.rtc_noinit layout may shift between builds, and a stale NVS snapshot loses energy
     * on restore), so a planned restart must leave a FRESH snapshot behind. */
    {
        extern rbpower_err_t rbpower_lifetime_sync_to_rtc(void);
        (void)rbpower_lifetime_sync_to_rtc();   /* rebuilds the image + NVS persist inside */
    }
    
    // Deinitialize rbpower
    ESP_LOGI(TAG, "Deinitializing rbpower...");
    rbpower_err_t power_err = rbpower_deinit();
    if (power_err != RBPOWER_OK) {
        ESP_LOGW(TAG, "rbpower_deinit failed: %d, continuing with reboot", power_err);
    } else {
        ESP_LOGI(TAG, "rbpower deinitialized successfully");
    }
    
    // Deinitialize rbgrid
    ESP_LOGI(TAG, "Deinitializing rbgrid...");
    rbgrid_err_t grid_err = rbgrid_deinit_system();
    if (grid_err != RBGRID_OK) {
        ESP_LOGW(TAG, "rbgrid_deinit_system failed: %d, continuing with reboot", grid_err);
    } else {
        ESP_LOGI(TAG, "rbgrid deinitialized successfully");
    }
    
    // TODO: deinitialize the other components too
    // - WebSocket
    // - WiFi
    // - Storage
    // - etc.
    
    // Delay before the restart
    if (delay_ms > 0) {
        ESP_LOGI(TAG, "Waiting %lu ms before reboot...", (unsigned long)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    
    ESP_LOGI(TAG, "Rebooting system now...");
    
    // Software restart
    { rbpower_mark_planned_restart(); esp_restart(); }

    // Not reached
    return ESP_OK;
}

//=============================================================================
// FLASH HISTORY DATA
//=============================================================================

// Includes for the history functions
#include "rbextflash_operations.h"
#include "rbextflash_config_runtime.h"
#include "rbstorage_transfer_types.h"
#include "rbjson.h"
#include "cJSON.h"

/**
 * @brief rbpower_period_t -> JSON string ("hour", "day", ...).
 */
static const char* period_to_string(rbpower_period_t period) {
    switch (period) {
        case RBPOWER_PERIOD_HOUR:  return "hour";
        case RBPOWER_PERIOD_DAY:   return "day";
        case RBPOWER_PERIOD_WEEK:  return "week";
        case RBPOWER_PERIOD_MONTH: return "month";
        case RBPOWER_PERIOD_YEAR:  return "year";
        default:                   return "unknown";
    }
}

/**
 * @brief rbpower_data_type_t -> JSON string ("electrical", "energy", ...).
 */
static const char* data_type_to_string(rbpower_data_type_t type) {
    switch (type) {
        case RBPOWER_DATA_TYPE_ELECTRICAL: return "electrical";
        case RBPOWER_DATA_TYPE_ENERGY:     return "energy";
        case RBPOWER_DATA_TYPE_ECONOMICS:  return "economics";
        case RBPOWER_DATA_TYPE_QUALITY:    return "quality";
        case RBPOWER_DATA_TYPE_DIRECTION:  return "direction";
        default:                           return "unknown";
    }
}

/**
 * @brief Record size for a data type.
 *
 * Buffer stride for the v4 READ shim output (rbstats_flash_read_v4), not the on-flash size;
 * 0 for types without a v4 read path.
 */
static size_t get_record_size(rbpower_data_type_t type) {
    switch (type) {
        case RBPOWER_DATA_TYPE_ELECTRICAL: return sizeof(rbstats_electrical_v4_t);
        case RBPOWER_DATA_TYPE_ENERGY:     return sizeof(rbstats_energy_v4_t);
        case RBPOWER_DATA_TYPE_ECONOMICS:  return sizeof(rbstats_econ_v4_t);
        default:                           return 0;   /* QUALITY/DIRECTION: no v4 read path */
    }
}

rbgrid_err_t rbgrid_history_get_buffer_info(
    rbpower_unit_id_t unit_id,
    rbpower_period_t period,
    rbpower_data_type_t type,
    uint32_t* total_available
) {
    if (!total_available) {
        ESP_LOGE(TAG, "rbgrid_history_get_buffer_info: total_available is NULL");
        return RBGRID_ERR_INVALID_ARG;
    }

    // Number of records ACTUALLY written (NOT capacity) — so history never returns the unwritten
    // 0xFF tail of the ring as records and reports the real count.
    esp_err_t err = rbpower_flash_get_record_count(unit_id, period, type, total_available);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d",
                 unit_id, period, type);
        *total_available = 0;
        return RBGRID_ERR_UNIT_NOT_FOUND;
    }

    ESP_LOGD(TAG, "Buffer info: unit=0x%04X, period=%s, type=%s, capacity=%lu",
             unit_id, period_to_string(period), data_type_to_string(type),
             (unsigned long)*total_available);

    return RBGRID_OK;
}

char* rbgrid_history_get_data(
    rbpower_unit_id_t unit_id,
    rbpower_period_t period,
    rbpower_data_type_t type,
    uint32_t offset,
    uint32_t limit
) {
    ESP_LOGI(TAG, "rbgrid_history_get_data: unit=0x%04X, period=%s, type=%s, offset=%lu, limit=%lu",
             unit_id, period_to_string(period), data_type_to_string(type),
             (unsigned long)offset, (unsigned long)limit);

    // 1. Validate parameters
    if (limit == 0 || limit > 720) {
        ESP_LOGE(TAG, "Invalid limit: %lu (must be 1-720)", (unsigned long)limit);
        return NULL;
    }

    // 2. Buffer info
    uint32_t total_available = 0;
    rbgrid_err_t err = rbgrid_history_get_buffer_info(unit_id, period, type, &total_available);
    if (err != RBGRID_OK) {
        /* A unit that exists but owns no stats ring for this (period,type) —
         * e.g. the voltage-bus unit — is an EMPTY history, not a server error (was HTTP 500). */
        if (rbpower_home_get_unit_definition(unit_id) != NULL) {
            total_available = 0;
        } else {
            ESP_LOGE(TAG, "Failed to get buffer info");
            return NULL;
        }
    }

    // 3-4. Clamp the [offset, offset+limit) window to what's available. If offset is at/past the
    //      end (or the ring is empty), the window is EMPTY — return a valid empty response, never
    //      NULL and never an underflowed `total_available - offset` (offset>=total wrapped that
    //      uint32 subtraction into a ~4 GB malloc).
    uint32_t records_to_read = 0;
    if (offset < total_available) {
        records_to_read = (offset + limit > total_available) ? (total_available - offset) : limit;
    } else if (total_available > 0) {
        ESP_LOGW(TAG, "Offset %lu >= total_available %lu → empty window",
                 (unsigned long)offset, (unsigned long)total_available);
    }

    // 5-6. Read the window from flash. Skip entirely for an empty window — malloc(0) is undefined
    //      and there's nothing to read; the response below is built with an empty data array.
    size_t record_size = get_record_size(type);
    void* data_buffer = NULL;
    uint32_t records_read = 0;
    if (records_to_read > 0) {
        data_buffer = malloc(record_size * records_to_read);
        if (!data_buffer) {
            ESP_LOGE(TAG, "Failed to allocate %lu bytes for data buffer",
                     (unsigned long)(record_size * records_to_read));
            return NULL;
        }
        esp_err_t flash_err = rbstats_flash_read_v4(
            unit_id, period, type, offset, records_to_read, data_buffer, &records_read);
        if (flash_err != ESP_OK) {
            ESP_LOGE(TAG, "Flash read failed: %d", flash_err);
            free(data_buffer);
            return NULL;
        }
        ESP_LOGI(TAG, "Read %lu records from Flash", (unsigned long)records_read);
    }

    // 7. Build the JSON response
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root");
        free(data_buffer);
        return NULL;
    }

    // Status
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "schema_ver", 4);   /* v4 record forms */

    // Request echo
    cJSON* request_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(request_obj, "period", period_to_string(period));
    cJSON_AddStringToObject(request_obj, "type", data_type_to_string(type));
    cJSON_AddNumberToObject(request_obj, "offset", offset);
    cJSON_AddNumberToObject(request_obj, "limit", limit);

    // unit_id formatted as "0004"
    char unit_id_str[5];
    snprintf(unit_id_str, sizeof(unit_id_str), "%04X", unit_id);
    /* Added directly: AddString(NULL) + ReplaceItemInObject would find no key, return false
     * and leak the new string node (~70 B) on every history request. */
    cJSON_AddStringToObject(request_obj, "unit_id", unit_id_str);

    cJSON_AddItemToObject(root, "request", request_obj);

    // Data array
    cJSON* data_array = cJSON_CreateArray();

    // 8. Serialize the records by type
    uint32_t serialized_count = 0;
    for (uint32_t i = 0; i < records_read; i++) {
        void* record_ptr = (uint8_t*)data_buffer + (i * record_size);
        cJSON* record_json = NULL;

        // Skip an uninitialized (erased-0xFF) ring slot: the v3→v4 read shim maps a blank slot's
        // 0xFFFFFFFF timestamp to h.ts == 0 — never emit it as a record. (Records without a wall-clock
        // stamp, ts=0, are not emitted by this endpoint.)
        // On native v4 flash (RBPOWER_RECORD_FORMAT_VERSION >= 4) the shim is bypassed and a blank
        // slot arrives raw as ts == 0xFFFFFFFF — skip that too.
        uint32_t rec_ts = ((const rbstats_v4_hdr_t*)record_ptr)->ts;
        if (rec_ts == 0 || rec_ts == 0xFFFFFFFFu) {
            continue;
        }

        switch (type) {
            case RBPOWER_DATA_TYPE_ELECTRICAL:
                record_json = rbjson_serialize_electrical((rbstats_electrical_v4_t*)record_ptr);
                break;
            case RBPOWER_DATA_TYPE_ENERGY:
                record_json = rbjson_serialize_energy((rbstats_energy_v4_t*)record_ptr);
                break;
            case RBPOWER_DATA_TYPE_ECONOMICS:
                record_json = rbjson_serialize_economics((rbstats_econ_v4_t*)record_ptr);
                break;
            // QUALITY/DIRECTION removed from the active pipeline (no ZC/THD source in the rbAmp fleet).
            default:
                ESP_LOGW(TAG, "Unknown data type: %d", type);
                break;
        }

        if (record_json) {
            cJSON_AddItemToArray(data_array, record_json);
            serialized_count++;
        } else {
            ESP_LOGW(TAG, "Failed to serialize record %lu of type %d", (unsigned long)i, type);
        }
    }

    ESP_LOGI(TAG, "Serialized %lu/%lu records", (unsigned long)serialized_count, (unsigned long)records_read);

    // Meta — "returned" is the number of records ACTUALLY emitted into the data array (after
    // skipping 0xFF tail slots / serialization failures), not the raw flash-read count, so it
    // always matches data.length. has_more reflects how far the read cursor
    // advanced (records_read), i.e. whether records remain beyond this window.
    cJSON* meta_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta_obj, "total_available", total_available);
    cJSON_AddNumberToObject(meta_obj, "returned", serialized_count);
    /* Records in the window that were NOT emitted (blank ring slots / records
     * without a wall-clock stamp) — explains total_available > returned without has_more lying. */
    if (records_read > serialized_count)
        cJSON_AddNumberToObject(meta_obj, "skipped", records_read - serialized_count);
    cJSON_AddBoolToObject(meta_obj, "has_more", (offset + records_read) < total_available);
    cJSON_AddItemToObject(root, "meta", meta_obj);

    cJSON_AddItemToObject(root, "data", data_array);

    // 9. Print to a string (ownership passes to the caller)
    char* json_string = cJSON_PrintUnformatted(root);

    // 10. Clean up
    cJSON_Delete(root);
    free(data_buffer);

    ESP_LOGI(TAG, "Generated JSON response, size=%d bytes", json_string ? (int)strlen(json_string) : 0);

    return json_string;
}
