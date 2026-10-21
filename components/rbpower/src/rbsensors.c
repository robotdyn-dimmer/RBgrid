/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbsensors.c
 * @brief Sensor context, channel profile configuration, sensor-type names and legacy raw-ADC
 *        conversion helpers (see rbsensors.h).
 */
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbsensors.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "math.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "RBSENSORS";

/* ================================================================================================
 * CONSTANTS
 * ================================================================================================ */
#define RBPOWER_MAX_SENSORS 4 ///< Maximum 4 channels

/* ================================================================================================
 * INITIALIZATION
 * ================================================================================================ */

esp_err_t rbsensors_init(rbsensors_context_t **context)
{
    if (context == NULL)
    {
        ESP_LOGE(TAG, "Context pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate memory for context
    rbsensors_context_t *sensors_ctx = (rbsensors_context_t *)calloc(1, sizeof(rbsensors_context_t));
    if (sensors_ctx == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for sensors context");
        return ESP_ERR_NO_MEM;
    }

    // Create mutex for thread safety
    sensors_ctx->access_mutex = xSemaphoreCreateMutex();
    if (sensors_ctx->access_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(sensors_ctx);
        return ESP_ERR_NO_MEM;
    }

    // Initialize basic parameters
    sensors_ctx->initialized = true;
    sensors_ctx->active_channels_mask = 0;
    sensors_ctx->active_sensors_count = 0;

    // Initialize ADC channel mapping (initially all unused)
    for (int i = 0; i < 4; i++)
    {
        sensors_ctx->adc_to_profile_map[i] = -1; // -1 = unused channel
    }

    // Initialize sensor profiles
    for (int i = 0; i < RBPOWER_MAX_SENSORS; i++)
    {
        rbsensor_profile_t *profile = &sensors_ctx->profiles[i];

        // Basic parameters
        profile->profile_id = i;
        profile->channel_id = i;
        profile->is_active = false;
        profile->sensor_type = RBSENSOR_TYPE_UNKNOWN;
        profile->sensor_class = RBSENSOR_CLASS_VOLTAGE;
        profile->sensor_source = RBSENSOR_SOURCE_ADC;
        profile->source_channel = 0xFF; // Not assigned
        memset(profile->name, 0, sizeof(profile->name));

        // Reset calibration
        profile->calibration.zero_offset = 0.0f;
        profile->calibration.scale_factor = 1.0f;
        profile->calibration.is_calibrated = false;
        profile->calibration.calibration_timestamp = 0;

        // Default filtering parameters
        profile->filtering.enable_moving_avg = false;
        profile->filtering.moving_avg_samples = 8;
        profile->filtering.enable_median_filter = false;
        profile->filtering.median_filter_size = 5;
        profile->filtering.noise_threshold = 0.01f;

        // Default calculations
        profile->calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_INSTANTANEOUS;

        // Initialize runtime data
        rbsensor_runtime_t *runtime = &sensors_ctx->runtime[i];
        memset(runtime, 0, sizeof(rbsensor_runtime_t));

        runtime->raw_samples = NULL;
        runtime->processed_samples = NULL;
        runtime->buffer_size = 0;
    }

    // System parameters
    sensors_ctx->system_frequency_hz = 50.0f; // Default 50Hz
    sensors_ctx->sampling_rate_hz = 20000;    // Default 20kHz

    // Initialize calculation management
    sensors_ctx->calculations.realtime_mask = RBPOWER_CALC_GROUP_REALTIME;
    sensors_ctx->calculations.deferred_mask = RBPOWER_CALC_GROUP_QUALITY |
                                              RBPOWER_CALC_GROUP_ADVANCED |
                                              RBPOWER_CALC_GROUP_STATISTICS;
    sensors_ctx->calculations.deferred_task_enabled = false;
    sensors_ctx->calculations.deferred_task_handle = NULL;
    sensors_ctx->calculations.deferred_queue = NULL;
    sensors_ctx->calculations.deferred_task_core = 0; // Core 0 by default

    *context = sensors_ctx;

    // ===== Structure size analysis =====
    ESP_LOGD(TAG, "=== STRUCTURE SIZE ANALYSIS ===\n"
             "  rbsensors_context_t: %zu bytes\n"
             "  rbsensor_profile_t: %zu bytes\n"
             "  rbsensor_runtime_t: %zu bytes\n"
             "  Total profiles: %zu bytes\n"
             "  Total runtime: %zu bytes\n"
             "  Allocated memory address: %p",
             sizeof(rbsensors_context_t),
             sizeof(rbsensor_profile_t),
             sizeof(rbsensor_runtime_t),
             sizeof(rbsensor_profile_t) * 4,
             sizeof(rbsensor_runtime_t) * 4,
             sensors_ctx);

    ESP_LOGI(TAG, "Sensors context initialized successfully");
    ESP_LOGI(TAG, "Max sensors: %d, Real-time calculations: 0x%08lX",
             RBPOWER_MAX_SENSORS, sensors_ctx->calculations.realtime_mask);

    return ESP_OK;
}

esp_err_t rbsensors_deinit(rbsensors_context_t *context)
{
    if (context == NULL)
    {
        ESP_LOGW(TAG, "Context is NULL, nothing to deinitialize");
        return ESP_OK;
    }

    if (!context->initialized)
    {
        ESP_LOGW(TAG, "Context not initialized");
        free(context);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing sensors context...");

    // Free runtime data buffers
    for (int i = 0; i < RBPOWER_MAX_SENSORS; i++)
    {
        rbsensor_runtime_t *runtime = &context->runtime[i];

        if (runtime->raw_samples != NULL)
        {
            free(runtime->raw_samples);
            runtime->raw_samples = NULL;
        }

        if (runtime->processed_samples != NULL)
        {
            free(runtime->processed_samples);
            runtime->processed_samples = NULL;
        }

        runtime->buffer_size = 0;
    }

    // Delete mutex
    if (context->access_mutex != NULL)
    {
        vSemaphoreDelete(context->access_mutex);
        context->access_mutex = NULL;
    }

    // Reset flags
    context->initialized = false;
    context->active_channels_mask = 0;
    context->active_sensors_count = 0;

    // Free context memory
    free(context);

    ESP_LOGI(TAG, "Sensors context deinitialized successfully");

    return ESP_OK;
}

/* ================================================================================================
 * CONFIGURATION
 * ================================================================================================ */
esp_err_t rbsensors_configure_channel_from_config(rbsensors_context_t *ctx,
                                                  uint8_t channel,
                                                  const rbpower_sensor_channel_config_t *user_config)
{
    ESP_LOGD("RBSENSORS", "=== CHANNEL %d CONFIGURATION START ===", channel);

    if (ctx == NULL || user_config == NULL || channel >= 4)
    {
        ESP_LOGE("RBSENSORS", "Invalid parameters: ctx=%p, config=%p, channel=%d", ctx, user_config, channel);
        return ESP_ERR_INVALID_ARG;
    }

    if (!user_config->enabled)
    {
        ESP_LOGD("RBSENSORS", "Channel %d disabled", channel);
        return ESP_OK; // Channel disabled
    }

    ESP_LOGD("RBSENSORS", "Configuring channel %d: type=%d, name='%s'",
             channel, user_config->type, user_config->name);

    rbsensor_profile_t *profile = &ctx->profiles[channel];

    // State BEFORE changes
    ESP_LOGD("RBSENSORS", "BEFORE: active=%s, type=%d, active_count=%d, mask=0x%02X",
             profile->is_active ? "YES" : "NO", profile->sensor_type,
             ctx->active_sensors_count, ctx->active_channels_mask);

    // Copy user parameters
    profile->profile_id = channel;
    profile->channel_id = channel;
    profile->source_channel = channel;
    profile->is_active = true;
    profile->sensor_type = user_config->type;
    profile->sensor_source = RBSENSOR_SOURCE_ADC; // ADC only for now
    strncpy(profile->name, user_config->name, sizeof(profile->name) - 1);
    profile->name[sizeof(profile->name) - 1] = '\0'; // Ensure null-terminator

    // Determine class by type
    if (user_config->type >= RBSENSOR_TYPE_VOLTAGE_ZMPT101B &&
        user_config->type <= RBSENSOR_TYPE_VOLTAGE_CUSTOM)
    {
        profile->sensor_class = RBSENSOR_CLASS_VOLTAGE;
    }
    else
    {
        profile->sensor_class = RBSENSOR_CLASS_CURRENT;
    }

    // Copy conversion parameters
    profile->source_params.adc_params.scale_factor = user_config->scale_factor;
    profile->source_params.adc_params.zero_offset = user_config->zero_offset;
    profile->source_params.adc_params.noise_threshold = user_config->noise_threshold;
    profile->source_params.adc_params.min_value = user_config->min_value;
    profile->source_params.adc_params.max_value = user_config->max_value;
    profile->source_params.adc_params.linearity_correction = user_config->linearity_correction;

    // Set basic calibration parameters
    profile->calibration.zero_offset = 0.0f;
    profile->calibration.scale_factor = 1.0f;
    profile->calibration.is_calibrated = false;
    profile->calibration.calibration_timestamp = 0;

    // Basic filtering parameters
    profile->filtering.enable_moving_avg = false;
    profile->filtering.moving_avg_samples = 8;
    profile->filtering.enable_median_filter = false;
    profile->filtering.median_filter_size = 5;
    profile->filtering.noise_threshold = user_config->noise_threshold;

    // Default calculations depending on class
    if (profile->sensor_class == RBSENSOR_CLASS_VOLTAGE)
    {
        profile->calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_INSTANTANEOUS;
    }
    else
    {
        profile->calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_ACTIVE_POWER;
    }

    // Check if channel was not already activated
    if (!(ctx->active_channels_mask & (1 << channel)))
    {
        // Update active channel mask only if channel not yet active
        ctx->active_channels_mask |= (1 << channel);
        ctx->active_sensors_count++;
    }

    // State AFTER changes
    ESP_LOGD("RBSENSORS", "AFTER: active=%s, type=%d, class=%d, active_count=%d, mask=0x%02X",
             profile->is_active ? "YES" : "NO", profile->sensor_type, profile->sensor_class,
             ctx->active_sensors_count, ctx->active_channels_mask);

    ESP_LOGI("RBSENSORS", "Configured channel %d: ID '%d' name '%s' type=%d class=%d",
             channel, profile->profile_id, profile->name, user_config->type, profile->sensor_class);

    ESP_LOGD("RBSENSORS", "=== CHANNEL %d CONFIGURATION END ===", channel);

    return ESP_OK;
}

/* ================================================================================================
 * PUBLIC API FUNCTION IMPLEMENTATION
 * ================================================================================================ */

const char *rbpower_get_sensor_type_name(rbsensor_type_t type)
{
    switch (type)
    {
    case RBSENSOR_TYPE_VOLTAGE_ZMPT101B:
        return "ZMPT101B";
    case RBSENSOR_TYPE_VOLTAGE_ZMPT107_1:
        return "ZMPT107-1";
    case RBSENSOR_TYPE_VOLTAGE_DIVIDER:
        return "Voltage Divider";
    case RBSENSOR_TYPE_VOLTAGE_CUSTOM:
        return "Custom Voltage";

    case RBSENSOR_TYPE_CURRENT_SCT013_10A:
        return "SCT-013-10A";
    case RBSENSOR_TYPE_CURRENT_SCT013_20A:
        return "SCT-013-20A";
    case RBSENSOR_TYPE_CURRENT_SCT013_30A:
        return "SCT-013-30A";
    case RBSENSOR_TYPE_CURRENT_SCT013_50A:
        return "SCT-013-50A";
    case RBSENSOR_TYPE_CURRENT_SCT013_100A:
        return "SCT-013-100A";
    case RBSENSOR_TYPE_CURRENT_ACS712_5A:
        return "ACS712-5A";
    case RBSENSOR_TYPE_CURRENT_ACS712_20A:
        return "ACS712-20A";
    case RBSENSOR_TYPE_CURRENT_ACS712_30A:
        return "ACS712-30A";
    case RBSENSOR_TYPE_CURRENT_CUSTOM:
        return "Custom Current";

    default:
        return "Unknown";
    }
}

bool rbpower_is_sensor_compatible_with_channel(rbsensor_type_t type, uint8_t channel)
{
    // Basic check - all types are compatible with all channels
    // More complex logic can be added in the future
    return (channel < 4 && type != RBSENSOR_TYPE_UNKNOWN);
}

/* ================================================================================================
 * HIGH-PERFORMANCE SENSOR CONVERSION FUNCTIONS
 * ================================================================================================ */

float rbsensors_convert_zmpt107_voltage(uint32_t rms_adc_value, float reference_voltage_v, float scale_factor)
{
    // Conversion: ADC RMS → ADC voltage → physical voltage
    // float voltage_rms_adc = (rms_adc_value * reference_voltage_v) / 4095.0f;
    return (float)rms_adc_value * scale_factor;
}

float rbsensors_convert_sct013_current(uint32_t rms_adc_value, float reference_voltage_v, float scale_factor)
{
    // Similar to voltage - through transformer coefficient
    float voltage_rms_adc = rms_adc_value * (reference_voltage_v / scale_factor);
    return voltage_rms_adc;
}

float rbsensors_convert_acs712_current(uint32_t rms_adc_value, float reference_voltage_v,
                                       float sensitivity_mv_per_a, float zero_current_voltage_v)
{
    // ACS712: bipolar output with zero at center
    float voltage_rms_adc = (rms_adc_value * reference_voltage_v) / 4095.0f;
    float voltage_offset = voltage_rms_adc - zero_current_voltage_v;
    return (voltage_offset * 1000.0f) / sensitivity_mv_per_a; // mV to V and divide by sensitivity
}

float rbsensors_convert_universal(uint32_t rms_adc_value, float reference_voltage_v,
                                  float zero_offset, float scale_factor)
{
    float voltage_rms_adc = (rms_adc_value * reference_voltage_v) / 4095.0f;
    return (voltage_rms_adc - zero_offset) * scale_factor;
}
