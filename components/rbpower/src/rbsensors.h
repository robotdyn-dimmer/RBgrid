/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbsensors.h
 * @brief Sensor-profile types, the sensor context and legacy raw-ADC conversion helpers.
 *
 * The profile and preset types back the sensor catalogue (rbsensor_profiles.h) and the legacy
 * per-channel sensor descriptors of the configuration. The context and conversion functions belong
 * to the former on-chip ADC path: no sampling uses them in the current firmware (metering is done by
 * the rbAmp modules).
 *
 * Thread safety: a context is owned by one task; the access_mutex member is created but not taken
 * by these functions.
 */
#ifndef RBSENSORS_H
#define RBSENSORS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "../include/rbpower.h"

    /* ================================================================================================
     * SENSOR-SPECIFIC PARAMETERS
     * ================================================================================================ */

    /**
     * @brief Conversion parameters of an ADC-sampled sensor (e.g. ZMPT101B/107-1, SCT-013).
     */
    typedef struct
    {
        float scale_factor;         /**< Conversion factor from raw ADC RMS to the physical unit (V or A) */
        float zero_offset;          /**< Zero offset, raw ADC RMS units */
        float noise_threshold;      /**< Noise threshold, raw ADC RMS units */
        float linearity_correction; /**< Linearity correction factor */

        // Measurement range
        float min_value; /**< Minimum measurable value, V or A */
        float max_value; /**< Maximum measurable value, V or A */

        // Sensor-specific parameters (optional)
        union
        {
            struct
            {
                float turns_ratio;     /**< Transformer turns ratio */
                float burden_resistor; /**< Burden resistor, Ω */
            } transformer;             /**< Transformer-type sensors */

            struct
            {
                float divider_ratio;   /**< Divider ratio */
                float input_impedance; /**< Input impedance, Ω */
            } divider;                 /**< Resistive dividers */

            struct
            {
                float sensitivity;    /**< Sensitivity, mV/A or mV/V */
                float offset_voltage; /**< Offset voltage, V */
            } hall_effect;            /**< Hall-effect sensors */
        } sensor_specific;            /**< Sensor-specific parameters, selected by the sensor type */
    } rbsensor_adc_params_t;

    /* ================================================================================================
     * PROFILE STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Sensor profile.
     */
    typedef struct
    {
        // Identification
        uint8_t profile_id;     ///< Profile ID
        uint8_t channel_id;     ///< Channel ID
        uint8_t source_channel; ///< Channel on the data source (0xFF = not assigned)
        bool is_active;         ///< Profile is in use

        // Type and parameters
        rbsensor_class_t sensor_class;   /**< Measurement class (voltage, current) */
        rbsensor_type_t sensor_type;     /**< Sensor type (ZMPT101B, SCT013, ...) */
        rbsensor_source_t sensor_source; /**< Data source (ADC, I2C, ...) */

        // Name
        char name[32]; ///< Display name, NUL-terminated

        // Calibration
        struct
        {
            float zero_offset;              ///< Zero offset
            float scale_factor;             ///< Scale factor
            bool is_calibrated;             ///< Calibrated flag
            uint32_t calibration_timestamp; ///< Time of the last calibration, epoch s
        } calibration;                      ///< Calibration data

        // Filtering
        struct
        {
            bool enable_moving_avg;     ///< Enable the moving-average filter
            uint8_t moving_avg_samples; ///< Moving-average window, samples
            bool enable_median_filter;  ///< Enable the median filter
            uint8_t median_filter_size; ///< Median filter window, samples
            float noise_threshold;      ///< Noise threshold, raw RMS units
        } filtering;                    ///< Filtering settings

        // Calculations
        uint32_t calculations_mask; ///< Enabled calculations (RBPOWER_CALC_* flags)

        // Source-specific parameters
        union
        {
            rbsensor_adc_params_t adc_params; /**< Parameters of an ADC source */
            // Future sources:
            // rbsensor_i2c_params_t i2c_params;
            // rbsensor_spi_params_t spi_params;
        } source_params;                      ///< Parameters selected by sensor_source

    } rbsensor_profile_t;

    /* ================================================================================================
     * RUNTIME STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Sensor runtime data.
     */
    typedef struct
    {
        // Latest results
        float last_raw_value; /**< Latest raw value */
        float last_rms_value; /**< Latest RMS value */

        // RMS accumulators
        float rms_accumulator;  /**< Sum of squares for RMS */
        uint32_t samples_count; /**< Accumulated sample count */

        // Buffers
        float *raw_samples;       /**< Raw sample buffer (heap, may be NULL) */
        float *processed_samples; /**< Processed sample buffer (heap, may be NULL) */
        uint32_t buffer_size;     /**< Buffer length, samples */

    } rbsensor_runtime_t;

    /**
     * @brief Sensor context (allocated by rbsensors_init()).
     */
    typedef struct
    {
        // Sensor profiles
        rbsensor_profile_t profiles[4]; /**< Up to 4 channels */

        // Runtime data
        rbsensor_runtime_t runtime[4]; ///< Per-channel runtime data

        // State flags
        bool initialized;             ///< Context initialised
        uint8_t active_channels_mask; ///< Bit n set = channel n configured
        uint8_t active_sensors_count; ///< Number of configured channels
        int8_t adc_to_profile_map[4]; ///< Channel → profile index (-1 = unused)

        // System parameters
        float system_frequency_hz; ///< Mains frequency, Hz (default 50)
        uint32_t sampling_rate_hz; ///< Nominal sampling rate, Hz (legacy descriptor)

        // Calculation control
        struct
        {
            uint32_t realtime_mask;     ///< Calculations done in real time (RBPOWER_CALC_* flags)
            uint32_t deferred_mask;     ///< Calculations deferred to a task (RBPOWER_CALC_* flags)
            bool deferred_task_enabled; ///< Deferred task enabled
            void *deferred_task_handle; ///< Deferred task handle (TaskHandle_t)
            void *deferred_queue;       ///< Deferred task queue (QueueHandle_t)
            uint8_t deferred_task_core; ///< CPU core of the deferred task
        } calculations;                 ///< Calculation scheduling

        // Thread safety
        void *access_mutex; ///< Context mutex (SemaphoreHandle_t)

    } rbsensors_context_t;

    /* ================================================================================================
     * PRESET STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Built-in sensor preset.
     */
    typedef struct {
        rbsensor_type_t type;           /**< Sensor type */
        const char* name;               /**< Sensor name */
        const char* description;        /**< Sensor description */
        rbsensor_profile_t profile;     /**< Sensor profile */
    } rbsensor_preset_t;

    /* ================================================================================================
     * API FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Allocate and initialise a sensor context (4 inactive profiles, default parameters, mutex).
     * @param[out] context Receives the new context; free it with rbsensors_deinit().
     * @retval ESP_OK              Success.
     * @retval ESP_ERR_INVALID_ARG @p context is NULL.
     * @retval ESP_ERR_NO_MEM      Context or mutex allocation failed.
     */
    esp_err_t rbsensors_init(rbsensors_context_t **context);

    /**
     * @brief Free a sensor context, its sample buffers and its mutex.
     * @param[in] context Context from rbsensors_init() (NULL is accepted); invalid after the call.
     * @return ESP_OK (always).
     */
    esp_err_t rbsensors_deinit(rbsensors_context_t *context);

    /**
     * @brief Configure a channel profile from the user channel configuration.
     *
     * A disabled channel is left untouched. The class (voltage/current) is derived from the type.
     *
     * @param[in,out] ctx     Sensor context.
     * @param[in]     channel Channel (0-3).
     * @param[in]     config  User channel configuration.
     * @retval ESP_OK              Success (also when the channel is disabled).
     * @retval ESP_ERR_INVALID_ARG @p ctx or @p config is NULL, or @p channel >= 4.
     */
    esp_err_t rbsensors_configure_channel_from_config(rbsensors_context_t *ctx,
                                                      uint8_t channel,
                                                      const rbpower_sensor_channel_config_t *config);

    /* ================================================================================================
     * RAW-ADC CONVERSION FUNCTIONS (legacy)
     * ================================================================================================ */

    /**
     * @brief Convert a ZMPT107-1 raw RMS reading to voltage (rms_adc_value × scale_factor).
     * @param[in] rms_adc_value       Raw ADC RMS value (0-4095).
     * @param[in] reference_voltage_v ADC reference voltage, V (typically 3.3; unused).
     * @param[in] scale_factor        Transformer factor (237.0 for ZMPT107-1).
     * @return Voltage, V.
     */
    float rbsensors_convert_zmpt107_voltage(uint32_t rms_adc_value, float reference_voltage_v, float scale_factor);

    /**
     * @brief Convert an SCT-013-10A raw RMS reading to current.
     * @param[in] rms_adc_value       Raw ADC RMS value (0-4095).
     * @param[in] reference_voltage_v ADC reference voltage, V.
     * @param[in] scale_factor        Transformer factor (98.9 for SCT-013-10A).
     * @return Current, A.
     */
    float rbsensors_convert_sct013_current(uint32_t rms_adc_value, float reference_voltage_v, float scale_factor);

    /**
     * @brief Convert an ACS712 raw RMS reading to current.
     * @param[in] rms_adc_value          Raw ADC RMS value (0-4095).
     * @param[in] reference_voltage_v    ADC reference voltage, V.
     * @param[in] sensitivity_mv_per_a   Sensitivity, mV/A.
     * @param[in] zero_current_voltage_v Output voltage at zero current, V.
     * @return Current, A.
     */
    float rbsensors_convert_acs712_current(uint32_t rms_adc_value, float reference_voltage_v,
                                           float sensitivity_mv_per_a, float zero_current_voltage_v);

    /**
     * @brief Generic conversion: ((rms_adc_value × reference_voltage_v / 4095) − zero_offset) × scale_factor.
     * @param[in] rms_adc_value       Raw ADC RMS value (0-4095).
     * @param[in] reference_voltage_v ADC reference voltage, V.
     * @param[in] zero_offset         Zero offset, V.
     * @param[in] scale_factor        Scale factor, physical unit per V.
     * @return Physical value (V or A).
     */
    float rbsensors_convert_universal(uint32_t rms_adc_value, float reference_voltage_v,
                                      float zero_offset, float scale_factor);

#ifdef __cplusplus
}
#endif

#endif /* RBSENSORS_H */
