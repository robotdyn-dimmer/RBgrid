/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbsensor_profiles.h
 * @brief Built-in sensor presets (voltage and current transducers).
 *
 * The presets fill the legacy per-channel sensor descriptors (rbpower_sensor_channel_config_t) when a
 * channel-based unit is added through the configuration API. Scale factors are expressed in raw RMS
 * units of the former on-chip ADC path; no sampling uses them in the current firmware (metering is done
 * by the rbAmp modules).
 */

#ifndef RBSENSOR_PROFILES_H
#define RBSENSOR_PROFILES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbsensors.h"

    /**
     * @brief Preset: ZMPT107-1 voltage transformer (calibrated).
     */
    static const rbsensor_preset_t RBSENSOR_PRESET_ZMPT107_1 = {
        .type = RBSENSOR_TYPE_VOLTAGE_ZMPT107_1,
        .name = "ZMPT107-1",
        .description = "AC Voltage Transformer 250V",
        .profile = {
            .sensor_class = RBSENSOR_CLASS_VOLTAGE,
            .sensor_type = RBSENSOR_TYPE_VOLTAGE_ZMPT107_1,
            .sensor_source = RBSENSOR_SOURCE_ADC,

            // Source-specific parameters
            .source_params.adc_params = {
                .scale_factor = 0.187f,       // V AC = RMS * 0.187
                .zero_offset = 0.0f,          // Corrected at the acquisition layer
                .noise_threshold = 15.0f,     // Raw RMS units
                .linearity_correction = 1.0f, // Linear over the working range
                .min_value = 0.0f,            // Minimum 0 V
                .max_value = 250.0f,          // Maximum 250 V AC

                // Transformer-specific parameters
                .sensor_specific.transformer = {
                    .turns_ratio = 1000.0f,
                    .burden_resistor = 1000.0f,
                }},

            // Calibration
            .calibration = {
                .is_calibrated = true,
                .calibration_timestamp = 0,
            },

            // Filtering
            .filtering = {
                .enable_moving_avg = false, // RMS is already averaged
                .enable_median_filter = false,
            },

            // Calculations
            .calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_INSTANTANEOUS,
        }};

    /**
     * @brief Preset: ZMPT101B voltage transformer (typical values).
     */
    static const rbsensor_preset_t RBSENSOR_PRESET_ZMPT101B = {
        .type = RBSENSOR_TYPE_VOLTAGE_ZMPT101B,
        .name = "ZMPT101B",
        .description = "AC Voltage Transformer 250V",
        .profile = {
            .sensor_class = RBSENSOR_CLASS_VOLTAGE,
            .sensor_type = RBSENSOR_TYPE_VOLTAGE_ZMPT101B,
            .sensor_source = RBSENSOR_SOURCE_ADC,

            .source_params.adc_params = {
                .scale_factor = 0.200f, // Typical value
                .zero_offset = 0.0f,
                .noise_threshold = 20.0f,
                .linearity_correction = 1.0f,
                .min_value = 0.0f,
                .max_value = 250.0f,

                .sensor_specific.transformer = {
                    .turns_ratio = 1000.0f,
                    .burden_resistor = 1000.0f,
                }},

            .calibration = {
                .is_calibrated = false, // Requires calibration
            },

            .calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_INSTANTANEOUS,
        }};

    /* ================================================================================================
     * CURRENT SENSOR PRESETS
     * ================================================================================================ */

    /**
     * @brief Preset: SCT-013-10A current transformer.
     */
    static const rbsensor_preset_t RBSENSOR_PRESET_SCT013_10A = {
        .type = RBSENSOR_TYPE_CURRENT_SCT013_10A,
        .name = "SCT-013-10A",
        .description = "Current Transformer 10A max",
        .profile = {
            .sensor_class = RBSENSOR_CLASS_CURRENT,
            .sensor_type = RBSENSOR_TYPE_CURRENT_SCT013_10A,
            .sensor_source = RBSENSOR_SOURCE_ADC,

            .source_params.adc_params = {
                .scale_factor = 0.0098f, // A per raw RMS unit (preliminary)
                .zero_offset = 0.0f,
                .noise_threshold = 10.0f,
                .linearity_correction = 1.0f,
                .min_value = 0.0f,
                .max_value = 10.0f,

                .sensor_specific.transformer = {
                    .turns_ratio = 1800.0f,
                    .burden_resistor = 198.0f,
                }},

            .calibration = {
                .is_calibrated = false,
            },

            .calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_ACTIVE_POWER,
        }};

    /**
     * @brief Preset: SCT-013-50A current transformer.
     */
    static const rbsensor_preset_t RBSENSOR_PRESET_SCT013_50A = {
        .type = RBSENSOR_TYPE_CURRENT_SCT013_50A,
        .name = "SCT-013-50A",
        .description = "Current Transformer 50A max",
        .profile = {
            .sensor_class = RBSENSOR_CLASS_CURRENT,
            .sensor_type = RBSENSOR_TYPE_CURRENT_SCT013_50A,
            .sensor_source = RBSENSOR_SOURCE_ADC,

            .source_params.adc_params = {
                .scale_factor = 0.049f, // A per raw RMS unit (50 A / 1024 units)
                .zero_offset = 0.0f,
                .noise_threshold = 10.0f,
                .linearity_correction = 1.0f,
                .min_value = 0.0f,
                .max_value = 50.0f,

                .sensor_specific.transformer = {
                    .turns_ratio = 1800.0f,
                    .burden_resistor = 39.0f,  // Smaller burden for the higher current
                }},

            .calibration = {
                .is_calibrated = false,
            },

            .calculations_mask = RBPOWER_CALC_RMS | RBPOWER_CALC_ACTIVE_POWER,
        }};

    /* ================================================================================================
     * PRESET TABLE
     * ================================================================================================ */
    /** @brief All built-in presets. */
    static const rbsensor_preset_t *ALL_PRESETS[] = {
        &RBSENSOR_PRESET_ZMPT107_1,
        &RBSENSOR_PRESET_ZMPT101B,
        &RBSENSOR_PRESET_SCT013_10A,
        &RBSENSOR_PRESET_SCT013_50A,
        // Add new presets here
    };

    /** @brief Number of entries in ALL_PRESETS. */
    #define RBSENSOR_PRESET_COUNT (sizeof(ALL_PRESETS) / sizeof(ALL_PRESETS[0]))

    /**
     * @brief Find the preset of a sensor type.
     * @param[in] type Sensor type.
     * @return Pointer to the preset, or NULL if there is none.
     */
    static inline const rbsensor_preset_t* rbsensor_get_preset_by_type(rbsensor_type_t type) {
        for (size_t i = 0; i < RBSENSOR_PRESET_COUNT; i++) {
            if (ALL_PRESETS[i]->type == type) {
                return ALL_PRESETS[i];
            }
        }
        return NULL;
    }

#ifdef __cplusplus
}
#endif

#endif /* RBSENSOR_PROFILES_H */
