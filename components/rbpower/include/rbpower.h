/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower.h
 * @brief rbpower core API: library lifecycle, home-network (unit registry) access, unit_id helpers,
 *        external-flash status and the time system (rbtime).
 *
 * Measurement data comes only from rbAmp I2C modules and ESP-NOW nodes through the injection API;
 * rbpower itself starts no sampling hardware. The `adc`, `adc_sensors` and `calibration` members of
 * the configuration are legacy descriptors kept for configuration compatibility.
 *
 * Thread safety: rbpower_init()/rbpower_init_extended() must run once from the application task
 * before other tasks use the library. The lifecycle functions (start/stop/pause/resume) take an
 * internal mutex. Pointers returned into the home network (rbpower_get_network(),
 * rbpower_get_unit_by_id(), rbpower_get_unit_by_name()) refer to shared state that other tasks
 * update; hold rbpower_home_lock() while reading or writing through them.
 */

#ifndef RBPOWER_H
#define RBPOWER_H

#ifdef __cplusplus
extern "C"
{
#endif

    /* ================================================================================================
     * INCLUDES
     * ================================================================================================ */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rbpower_constants.h"
#include "rbpower_enums.h"
#include "rbhome_types.h"
#include "rbhome_tariff.h"
#include "driver/gpio.h"
    /** @brief GPIO number type used by rbpower. */
    typedef gpio_num_t rbpower_gpio_t;
#define RBPOWER_GPIO_INVALID GPIO_NUM_NC ///< "No GPIO" marker

    /* ================================================================================================
     * VERSION INFORMATION
     * ================================================================================================ */

#define RBPOWER_VERSION_MAJOR 1         ///< rbpower library major version
#define RBPOWER_VERSION_MINOR 1         ///< rbpower library minor version
#define RBPOWER_VERSION_PATCH 0         ///< rbpower library patch version
#define RBPOWER_VERSION "1.1.0"         ///< rbpower library version string (same as the firmware PROJECT_VER)

/* ================================================================================================
 * CALCULATION CONSTANTS
 * ================================================================================================ */
#define RBPOWER_MAX_SENSORS 4 ///< Maximum sensors

#define RBPOWER_CALC_INSTANTANEOUS (1 << 0)  ///< Calculation flag: instantaneous values
#define RBPOWER_CALC_RMS (1 << 1)            ///< Calculation flag: RMS values
#define RBPOWER_CALC_AVG (1 << 2)            ///< Calculation flag: average values
#define RBPOWER_CALC_PEAK (1 << 3)           ///< Calculation flag: peak values
#define RBPOWER_CALC_THD (1 << 4)            ///< Calculation flag: total harmonic distortion
#define RBPOWER_CALC_HARMONICS (1 << 5)      ///< Calculation flag: harmonic analysis
#define RBPOWER_CALC_ACTIVE_POWER (1 << 6)   ///< Calculation flag: active power
#define RBPOWER_CALC_REACTIVE_POWER (1 << 7) ///< Calculation flag: reactive power
#define RBPOWER_CALC_APPARENT_POWER (1 << 8) ///< Calculation flag: apparent power
#define RBPOWER_CALC_POWER_FACTOR (1 << 9)   ///< Calculation flag: power factor
#define RBPOWER_CALC_FREQUENCY (1 << 10)     ///< Calculation flag: frequency
#define RBPOWER_CALC_PHASE_ANGLE (1 << 11)   ///< Calculation flag: phase angle
#define RBPOWER_CALC_ENERGY (1 << 12)        ///< Calculation flag: energy accumulation

/** @brief Calculation group: real-time values. */
#define RBPOWER_CALC_GROUP_REALTIME (RBPOWER_CALC_INSTANTANEOUS | RBPOWER_CALC_RMS | RBPOWER_CALC_ACTIVE_POWER)
/** @brief Calculation group: power quality. */
#define RBPOWER_CALC_GROUP_QUALITY (RBPOWER_CALC_THD | RBPOWER_CALC_HARMONICS | RBPOWER_CALC_POWER_FACTOR)
/** @brief Calculation group: advanced values. */
#define RBPOWER_CALC_GROUP_ADVANCED (RBPOWER_CALC_REACTIVE_POWER | RBPOWER_CALC_PHASE_ANGLE | RBPOWER_CALC_FREQUENCY)
/** @brief Calculation group: statistics. */
#define RBPOWER_CALC_GROUP_STATISTICS (RBPOWER_CALC_AVG | RBPOWER_CALC_PEAK | RBPOWER_CALC_ENERGY)

    /* ================================================================================================
     * SIMPLIFIED SENSOR STRUCTURES
     * ================================================================================================ */
    /**
     * @brief Per-channel sensor configuration (legacy descriptor; not used for sampling).
     */
    typedef struct
    {
        bool enabled;         ///< Sensor enabled
        rbsensor_type_t type; ///< Sensor type
        char name[32];        ///< User-defined name

        // Conversion parameters
        float scale_factor;    ///< Scaling coefficient
        float zero_offset;     ///< Zero offset, in the measured unit (V or A)
        float noise_threshold; ///< Noise threshold, in the measured unit (V or A)

        // Ranges
        float min_value; ///< Minimum value, in the measured unit (V or A)
        float max_value; ///< Maximum value, in the measured unit (V or A)

        // Additional parameters
        float linearity_correction; ///< Linearity correction factor

    } rbpower_sensor_channel_config_t;

    /**
     * @brief Configuration of all sensor channels (legacy descriptor).
     */
    typedef struct
    {
        rbpower_sensor_channel_config_t channels[RBPOWER_MAX_CHANNELS]; ///< Channel configuration 0-3

        // Global parameters
        // bool enable_auto_calibration; // Auto-calibration on startup
        bool enable_filtering;  ///< Enable filtering
        uint8_t filter_samples; ///< Averaging filter size, samples

    } rbpower_sensors_config_t;

    /* ================================================================================================
     * CONFIGURATION STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Legacy ADC channel descriptor.
     *
     * No ADC sampling is started by the firmware and these fields are not validated.
     */
    typedef struct
    {
        uint8_t channels[RBPOWER_MAX_CHANNELS]; ///< Channel indices in order (0-7)
        uint8_t channels_count;                 ///< Number of valid channels[]
        uint32_t sampling_frequency;            ///< Nominal sampling frequency, Hz (validated 1000..50000)
        uint8_t adc_resolution;                 ///< Resolution, bits (9-12)
        uint8_t adc_attenuation;                ///< Attenuation setting (0-3)
    } rbpower_adc_config_t;

    /**
     * @brief Mains network parameters.
     */
    typedef struct
    {
        uint16_t nominal_frequency; ///< Nominal mains frequency, Hz (RBPOWER_FREQ_50HZ or RBPOWER_FREQ_60HZ)
        float nominal_voltage;      ///< Nominal voltage, V
    } rbpower_network_config_t;

    /**
     * @brief Calibration descriptor (legacy; metering accuracy is owned by the rbAmp modules).
     */
    typedef struct
    {
        float voltage_scale;       ///< Voltage scaling coefficient
        float voltage_offset;      ///< Voltage offset, V
        float current_scale[3];    ///< Current scaling coefficients
        float current_offset[3];   ///< Current offsets, A
        float phase_correction[3]; ///< Phase correction, degrees
    } rbpower_calibration_t;

    /* ================================================================================================
     * TIME AND SYNCHRONIZATION SYSTEM - STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Time synchronization source
     */
    typedef enum
    {
        RBTIME_SOURCE_NONE = 0,    /**< Synchronization disabled */
        RBTIME_SOURCE_SNTP,        /**< SNTP server (internet) */
        RBTIME_SOURCE_ESP_NOW_HUB, /**< ESP-NOW hub device */
        RBTIME_SOURCE_MANUAL,      /**< Manual time setting */
        RBTIME_SOURCE_RTC,         /**< External RTC module */
        RBTIME_SOURCE_GPS          /**< GPS module (future) */
    } rbtime_source_t;


    /**
     * @brief Time synchronization status
     */
    typedef enum
    {
        RBTIME_STATUS_UNINITIALIZED = 0, /**< Not initialized */
        RBTIME_STATUS_INITIALIZING,      /**< Initialization in progress */
        RBTIME_STATUS_SYNCING,           /**< Synchronization in progress */
        RBTIME_STATUS_SYNCED,            /**< Synchronized */
        RBTIME_STATUS_TIMEOUT,           /**< Synchronization timeout */
        RBTIME_STATUS_ERROR,             /**< Synchronization error */
        RBTIME_STATUS_FALLBACK           /**< Operating in fallback mode */
    } rbtime_status_t;


    /**
     * @brief Time system configuration
     */
    typedef struct
    {
        // === MAIN SETTINGS ===
        bool enable_time_sync;           /**< Enable time synchronization */
        rbtime_source_t primary_source;  /**< Primary time source */
        rbtime_source_t fallback_source; /**< Fallback time source */

        // === SNTP SETTINGS ===
        char sntp_server[64];        /**< SNTP server (pool.ntp.org) */
        char sntp_server_backup[64]; /**< Backup SNTP server */
        uint32_t sntp_timeout_ms;    /**< SNTP synchronization timeout, ms */

        // === TIME ZONE ===
        char timezone[32]; /**< POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3", "UTC+0") */
        bool enable_dst;   /**< Automatic daylight saving time */

        // === PERIODIC SYNCHRONIZATION ===
        uint32_t resync_interval_sec; /**< Resynchronization interval, s */
        uint32_t max_drift_sec;       /**< Drift that forces a resynchronization, s */

        // === ESP-NOW TIME ===
        uint8_t esp_now_hub_mac[6];  /**< ESP-NOW hub MAC address */
        uint32_t esp_now_timeout_ms; /**< ESP-NOW synchronization timeout, ms */

        // === FALLBACK PARAMETERS ===
        bool enable_rtc_fallback;      /**< Enable RTC fallback */
        bool enable_cpu_time_fallback; /**< Enable CPU time fallback */
        uint64_t manual_time_us;       /**< Manually set time, µs since the Unix epoch */

        // === ADDITIONAL PARAMETERS ===
        bool force_sync_on_startup; /**< Force synchronization on startup */
        bool enable_time_logging;   /**< Enable time logging */
        uint32_t sync_retry_count;  /**< Synchronization retry count */

        // === TIME CALLBACK ===
        void (*on_time_sync_status_changed)(rbtime_status_t old_status, rbtime_status_t new_status); ///< Optional status-change callback (NULL = none); runs in the time-system context
    } rbtime_config_t;

    /**
     * @brief Time system statistics
     */
    typedef struct
    {
        rbtime_status_t current_status;  /**< Current status */
        rbtime_source_t active_source;   /**< Active time source */
        uint64_t last_sync_timestamp_us; /**< Last synchronization time, µs */
        uint64_t system_start_time_us;   /**< System start time, µs */

        // Synchronization statistics
        uint32_t successful_syncs;       /**< Successful synchronizations */
        uint32_t failed_syncs;           /**< Failed synchronizations */
        uint32_t fallback_activations;   /**< Fallback mode activations */
        uint32_t time_corrections_count; /**< Time corrections count */

        // Time accuracy
        int32_t last_time_correction_sec; /**< Last time correction, s */
        int32_t max_observed_drift_sec;   /**< Maximum observed drift, s */
        float average_sync_duration_ms;   /**< Average synchronization duration, ms */

        // Network statistics
        uint32_t sntp_requests_sent;      /**< SNTP requests sent */
        uint32_t sntp_responses_received; /**< SNTP responses received */
        uint32_t esp_now_time_requests;   /**< ESP-NOW time requests */

    } rbtime_statistics_t;

    /**
     * @brief Callback for time-status notifications.
     * @param[in] old_status Previous status.
     * @param[in] new_status New status.
     * @param[in] user_data  Pointer passed to rbtime_register_event_callback().
     */
    typedef void (*rbtime_event_callback_t)(rbtime_status_t old_status, rbtime_status_t new_status, void *user_data);

/* ================================================================================================
 * MAIN rbpower CONFIGURATION STRUCTURE
 * ================================================================================================ */

// Include external Flash configuration
#include "external_flash_config.h"

    /**
     * @brief Main rbpower configuration.
     */
    typedef struct rbpower_config_s
    {
        rbpower_adc_config_t adc;              ///< Legacy ADC channel descriptor (no hardware started)
        rbpower_network_config_t network;      ///< Mains network parameters
        rbpower_sensors_config_t adc_sensors;  ///< Legacy sensor-channel descriptors
        rbpower_calibration_t calibration;     ///< Legacy calibration descriptor
        rbtime_config_t time_config;           ///< Time system configuration
        external_flash_state_t external_flash; ///< Consolidated external-flash state; storage is used when status.initialized is set
        uint32_t calculation_period_ms;        ///< Calculation period, ms
        uint8_t averaging_cycles;              ///< Averaging cycles count
        bool enable_harmonics;                 ///< Enable harmonic analysis
        bool enable_debug_logging;             ///< Enable debug output
        bool adc_dma_enabled;                  ///< Always forced to false by rbpower_init() (no ADC DMA in this firmware)
    } rbpower_hardware_config_t;

    //=============================================================================
    // RBHOME API FUNCTIONS
    //=============================================================================

    /**
     * @brief Full library bring-up: rbpower_init(), optional time sync, statistics system and timers,
     *        home network, accumulation, flash buffers, tariff system, and (by default) rbpower_start().
     *
     * Call once from the application task at boot. Failures of the statistics, flash-buffer and tariff
     * subsystems are logged but do not fail the call.
     *
     * @param[in] extended_config Extended configuration; its base_hardware_config must be valid
     *                            (time_config.enable_time_sync may be set by this call).
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG @p extended_config is NULL or the base configuration is invalid.
     * @return Any error of rbpower_init() or rbpower_home_init().
     */
    rbpower_err_t rbpower_init_extended(const rbpower_extended_config_t *extended_config);

    /**
     * @brief Initialise the home network (unit registry) from the extended configuration.
     *
     * Creates the recursive home-network mutex used by rbpower_home_lock().
     *
     * @param[in] config Extended configuration with the rbhome settings.
     * @retval RBPOWER_OK               Success, or the home network is disabled in @p config.
     * @retval RBPOWER_ERR_INVALID_ARG  @p config is NULL.
     * @retval RBPOWER_ERR_ALREADY_INIT Already initialised.
     * @retval RBPOWER_ERR_NO_MEMORY    The mutex or a unit allocation failed.
     */
    rbpower_err_t rbpower_home_init(const rbpower_extended_config_t *config);

    /**
     * @brief Get a pointer to the home network structure.
     * @param[out] network Receives the pointer.
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG @p network is NULL.
     * @retval RBPOWER_ERR_NOT_INIT    The home network is not initialised.
     *
     * @note Returns a pointer to the internal structure, NOT a copy; hold rbpower_home_lock() while using it.
     */
    rbpower_err_t rbpower_get_network(rbpower_electrical_network_t **network);

    /**
     * @brief Get a pointer to a unit by ID.
     * @param[in]  unit_id Unit ID (16 bit).
     * @param[out] unit    Receives a pointer to the internal unit (hold rbpower_home_lock() while using it).
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG @p unit is NULL, or no valid unit has this ID.
     * @retval RBPOWER_ERR_NOT_INIT    The home network is not initialised.
     */
    rbpower_err_t rbpower_get_unit_by_id(rbpower_unit_id_t unit_id, rbpower_home_unit_t **unit);

    /**
     * @brief Get a pointer to a unit by name.
     * @param[in]  name Unit name (e.g. "Main Supply", "Kitchen Load").
     * @param[out] unit Receives a pointer to the internal unit (hold rbpower_home_lock() while using it).
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG A parameter is NULL, or no unit has this name.
     * @retval RBPOWER_ERR_NOT_INIT    The home network is not initialised.
     */
    rbpower_err_t rbpower_get_unit_by_name(const char *name, rbpower_home_unit_t **unit);

    /**
     * @brief Find a unit ID by legacy ADC channel binding (deprecated, kept for compatibility).
     * @param[in] adc_channel Channel number (0-7).
     * @return Unit ID (16 bit), or RBPOWER_HOME_INVALID_ID if not found or the home network is not initialised.
     */
    rbpower_unit_id_t rbpower_find_unit_by_adc_channel(uint8_t adc_channel);

    //=============================================================================
    // NEW FUNCTIONS FOR WORKING WITH UNIT_ID
    //=============================================================================

    /**
     * @brief Create a unit_id of the local (TYPE=ADC) type.
     * @param[in] sensor_profile_id Sensor profile ID (0-63).
     * @param[in] adc_channel       Local channel number (LOCAL field).
     * @return unit_id (16 bit), or RBPOWER_HOME_INVALID_ID if @p sensor_profile_id is out of range.
     */
    rbpower_unit_id_t rbpower_create_adc_unit_id(uint8_t sensor_profile_id, uint8_t adc_channel);

    /**
     * @brief Create a unit_id for an I2C source.
     * @param[in] sensor_profile_id Sensor profile ID (0-63).
     * @param[in] i2c_bus_num       I2C bus number (0-3).
     * @param[in] i2c_address       I2C address slot (0-63).
     * @return unit_id (16 bit), or RBPOWER_HOME_INVALID_ID if a parameter is out of range.
     */
    rbpower_unit_id_t rbpower_create_i2c_unit_id(uint8_t sensor_profile_id, uint8_t i2c_bus_num, uint8_t i2c_address);

    /**
     * @brief Create a unit_id for an SPI source.
     * @param[in] sensor_profile_id Sensor profile ID (0-63).
     * @param[in] spi_device_id     SPI device ID (LOCAL field).
     * @return unit_id (16 bit), or RBPOWER_HOME_INVALID_ID if @p sensor_profile_id is out of range.
     */
    rbpower_unit_id_t rbpower_create_spi_unit_id(uint8_t sensor_profile_id, uint8_t spi_device_id);

    /**
     * @brief Create a unit_id for an ESP-NOW source.
     * @param[in] sensor_profile_id Sensor profile ID (0-63).
     * @param[in] device_mac        Device MAC address (LOCAL = mac[4] ^ mac[5]; devices can collide).
     * @return unit_id (16 bit), or RBPOWER_HOME_INVALID_ID if a parameter is invalid.
     */
    rbpower_unit_id_t rbpower_create_espnow_unit_id(uint8_t sensor_profile_id, const uint8_t device_mac[6]);

    /**
     * @brief Get the source type of a unit_id.
     * @param[in] unit_id Unit ID (16 bit).
     * @return Source type, unshifted (one of RBPOWER_UNIT_TYPE_xxx).
     */
    uint16_t rbpower_get_unit_type(rbpower_unit_id_t unit_id);

    /**
     * @brief Get the sensor profile ID of a unit_id.
     * @param[in] unit_id Unit ID (16 bit).
     * @return Sensor profile ID (0-63).
     */
    uint8_t rbpower_get_sensor_profile_id(rbpower_unit_id_t unit_id);

    /**
     * @brief Get the LOCAL field of a unit_id.
     * @param[in] unit_id Unit ID (16 bit).
     * @return Local information (8 bit).
     */
    uint8_t rbpower_get_unit_local_info(rbpower_unit_id_t unit_id);

    /**
     * @brief Check whether a unit_id has the local (TYPE=ADC) type.
     * @param[in] unit_id Unit ID (16 bit).
     * @return true if the TYPE field is RBPOWER_UNIT_TYPE_ADC.
     */
    bool rbpower_is_local_unit(rbpower_unit_id_t unit_id);

    /**
     * @brief Check whether the home network is initialised.
     * @return true if the home network is initialised.
     */
    bool rbpower_home_is_initialized(void);

    /**
     * @brief Get the result of unit creation at home-network initialisation.
     * @param[out] result Receives a copy of the creation result.
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG @p result is NULL.
     */
    rbpower_err_t rbpower_get_creation_result(rbpower_home_creation_result_t *result);

    /**
     * @brief Print the home network structure to the log.
     */
    void rbpower_home_print_network_structure(void);

    /* ================================================================================================
     * CALLBACK FUNCTION TYPES - MINIMUM
     * ================================================================================================ */

    /**
     * @brief Callback for new measurement results
     */
    // typedef void (*rbpower_results_callback_t)(const rbpower_results_t *results, void *user_data);

    /**
     * @brief Callback for system errors.
     * @param[in] error_code Error code.
     * @param[in] error_msg  Human-readable message.
     * @param[in] user_data  User pointer supplied at registration.
     */
    typedef void (*rbpower_error_callback_t)(rbpower_err_t error_code, const char *error_msg, void *user_data);

    /* ================================================================================================
     * EXTERNAL FLASH MEMORY AND OTP VALIDATION
     * ================================================================================================ */

    /**
     * @brief External flash status with OTP validation.
     */
    typedef struct
    {
        bool external_flash_available; ///< Flash initialised and available for use
        bool otp_validated;            ///< OTP validation passed
        uint32_t device_serial;        ///< Device serial number from OTP
        bool config_partition_mounted; ///< Config partition (file system) mounted
        size_t config_partition_size;  ///< Config partition size, bytes
        size_t config_used_space;      ///< Used space in the config partition, bytes
        const char *error_message;     ///< Static error message, or NULL
    } rbpower_flash_status_t;

    /**
     * @brief Initialise the external flash with OTP validation (wrapper of external_flash_init()).
     * @return ESP_OK on success, otherwise the ESP-IDF error code of external_flash_init().
     */
    esp_err_t rbpower_external_flash_init(void);

    /**
     * @brief Check external flash availability.
     * @return true if the flash is available and passed OTP validation.
     */
    bool rbpower_external_flash_is_available(void);

    /**
     * @brief Check the OTP validation status.
     * @return true if OTP validation passed.
     */
    bool rbpower_external_flash_otp_validated(void);

    /**
     * @brief Get the device serial number from OTP.
     * @return Serial number, or 0 if unavailable.
     */
    uint32_t rbpower_external_flash_get_serial(void);

    /**
     * @brief Get the external flash status.
     * @return Flash and OTP status (all-zero if the flash manager has no state).
     */
    rbpower_flash_status_t rbpower_external_flash_get_status(void);

    /* ================================================================================================
     * LIFECYCLE
     * ================================================================================================ */

    /**
     * @brief Initialise the base library: storage (when the external flash is initialised), config
     *        validation, context and mutex. Starts no measurement hardware.
     *
     * @param[in] config Configuration (copied); must not be NULL.
     * @retval RBPOWER_OK               Success.
     * @retval RBPOWER_ERR_INVALID_ARG  Invalid configuration (mains frequency not 50/60 Hz).
     * @retval RBPOWER_ERR_NOT_INIT     Storage initialisation failed.
     * @retval RBPOWER_ERR_ALREADY_INIT Already initialised.
     * @retval RBPOWER_ERR_NO_MEMORY    Mutex creation failed.
     * @retval RBPOWER_ERR_TIMEOUT      The internal mutex was not acquired within 1000 ms.
     */
    rbpower_err_t rbpower_init(const rbpower_hardware_config_t *config);

    /**
     * @brief Deinitialise the library (stops it first if running) and delete the mutex.
     * @retval RBPOWER_OK           Success.
     * @retval RBPOWER_ERR_NOT_INIT Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT  The internal mutex was not acquired within 1000 ms.
     */
    rbpower_err_t rbpower_deinit(void);

    /**
     * @brief Switch the library to the RUNNING state.
     * @retval RBPOWER_OK                  Success.
     * @retval RBPOWER_ERR_NOT_INIT        Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT         The internal mutex was not acquired within 100 ms.
     * @retval RBPOWER_ERR_ALREADY_STARTED Already running.
     */
    rbpower_err_t rbpower_start(void);

    /**
     * @brief Stop a running or paused library (state becomes CONFIGURED).
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_NOT_INIT    Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT     The internal mutex was not acquired within 100 ms.
     * @retval RBPOWER_ERR_NOT_STARTED Not running or paused.
     */
    rbpower_err_t rbpower_stop(void);

    /**
     * @brief Pause a running library.
     * @retval RBPOWER_OK                Success.
     * @retval RBPOWER_ERR_NOT_INIT      Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT       The internal mutex was not acquired within 100 ms.
     * @retval RBPOWER_ERR_INVALID_STATE Not running.
     */
    rbpower_err_t rbpower_pause(void);

    /**
     * @brief Resume a paused library.
     * @retval RBPOWER_OK                Success.
     * @retval RBPOWER_ERR_NOT_INIT      Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT       The internal mutex was not acquired within 100 ms.
     * @retval RBPOWER_ERR_INVALID_STATE Not paused.
     */
    rbpower_err_t rbpower_resume(void);

    /* ================================================================================================
     * CALIBRATION FUNCTIONS (declared only; no implementation is linked)
     * ================================================================================================ */

    /* ================================================================================================
     * UTILITY FUNCTIONS
     * ================================================================================================ */

/* ================================================================================================
 * HELPER MACROS
 * ================================================================================================ */

/** @brief True if @p err is RBPOWER_OK. */
#define RBPOWER_SUCCESS(err) ((err) == RBPOWER_OK)

/** @brief True if @p err is not RBPOWER_OK. */
#define RBPOWER_FAILED(err) ((err) != RBPOWER_OK)

    /* ================================================================================================
     * SENSOR API FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Get a display name of a sensor type (e.g. "SCT-013-10A").
     * @param[in] type Sensor type.
     * @return Static NUL-terminated name, or "Unknown"; never NULL.
     */
    const char *rbpower_get_sensor_type_name(rbsensor_type_t type);

    /**
     * @brief Check sensor type compatibility with a channel.
     * @param[in] type    Sensor type.
     * @param[in] channel Channel number (0-3).
     * @return true if @p channel < 4 and @p type is not RBSENSOR_TYPE_UNKNOWN.
     */
    bool rbpower_is_sensor_compatible_with_channel(rbsensor_type_t type, uint8_t channel);

    // static void rbpower_init_realtime_results_once(rbpower_realtime_t *results);
    // static inline void rbpower_reset_realtime_results_fast(rbpower_realtime_t *results);

    /* ================================================================================================
     * TIME SYSTEM - API FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Initialise the time system (SNTP, time zone, periodic resync).
     *
     * Call once from the application task.
     *
     * @param[in] config Time configuration (copied).
     * @retval RBPOWER_OK               Success.
     * @retval RBPOWER_ERR_INVALID_ARG  Invalid configuration.
     * @retval RBPOWER_ERR_ALREADY_INIT Already initialised.
     * @retval RBPOWER_ERR_NO_MEMORY    Mutex creation failed.
     * @retval RBPOWER_ERR_TIMEOUT      The time-system mutex was not acquired.
     * @return Other rbpower_err_t codes from the SNTP set-up.
     */
    rbpower_err_t rbtime_init(const rbtime_config_t *config);

    /**
     * @brief Deinitialise the time system.
     * @retval RBPOWER_OK           Success.
     * @retval RBPOWER_ERR_NOT_INIT Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT  The time-system mutex was not acquired.
     */
    rbpower_err_t rbtime_deinit(void);

    /**
     * @brief Get the current time status.
     * @return Current synchronization status.
     */
    rbtime_status_t rbtime_get_status(void);

    /**
     * @brief Get time-system statistics.
     * @param[out] stats Receives a copy of the statistics.
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG @p stats is NULL.
     * @retval RBPOWER_ERR_NOT_INIT    Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT     The time-system mutex was not acquired.
     */
    rbpower_err_t rbtime_get_statistics(rbtime_statistics_t *stats);

    /**
     * @brief Get the current time configuration.
     * @param[out] config Receives a copy of the configuration.
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_INVALID_ARG @p config is NULL.
     * @retval RBPOWER_ERR_NOT_INIT    Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT     The time-system mutex was not acquired.
     */
    rbpower_err_t rbtime_get_config(rbtime_config_t *config);

    /**
     * @brief Force a time synchronization and wait for it.
     *
     * Busy-waits up to @p timeout_ms; do not call from the system event task (use rbtime_kick_sync()).
     *
     * @param[in] source     Source for synchronization (RBTIME_SOURCE_NONE = the configured primary source).
     * @param[in] timeout_ms Operation timeout, ms (0 = the configured sntp_timeout_ms).
     * @retval RBPOWER_OK                     Synchronized.
     * @retval RBPOWER_ERR_NOT_INIT           Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT            Mutex not acquired, or no sync within the timeout.
     * @retval RBPOWER_ERR_WIFI_NO_CONNECTION No network for SNTP.
     * @retval RBPOWER_ERR_NOT_IMPLEMENTED    The source is not supported.
     */
    rbpower_err_t rbtime_force_sync(rbtime_source_t source, uint32_t timeout_ms);
    /**
     * @brief Start or restart the SNTP request without waiting (safe from event handlers).
     * @retval RBPOWER_OK           Request started, or SNTP is not the enabled primary source (nothing to do).
     * @retval RBPOWER_ERR_NOT_INIT Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT  The time-system mutex was not acquired within 100 ms.
     * @return Other rbpower_err_t codes from the SNTP set-up.
     */
    rbpower_err_t rbtime_kick_sync(void);

    /**
     * @brief Set the time zone.
     * @param[in] timezone POSIX TZ string (e.g. "UTC+3", "HKT-8").
     * @retval RBPOWER_OK              Success.
     * @retval RBPOWER_ERR_NOT_INIT    Not initialised.
     * @retval RBPOWER_ERR_INVALID_ARG NULL or invalid TZ string (the current TZ is left unchanged).
     * @retval RBPOWER_ERR_TIMEOUT     The time-system mutex was not acquired.
     */
    rbpower_err_t rbtime_set_timezone(const char *timezone);
    /**
     * @brief Check whether a POSIX TZ string is acceptable (name + offset, optional DST rule).
     * @param[in] timezone TZ string, e.g. "CST-8", "<UTC+9>-9", "CET-1CEST,M3.5.0,M10.5.0/3".
     * @return true if valid.
     */
    bool rbtime_timezone_is_valid(const char *timezone);

    /**
     * @brief Epoch of the last detected SNTP clock step larger than 2 s.
     *
     * Statistics finalizers mark records whose span covers it with the CLOCK_STEP quality flag.
     *
     * @return Epoch s of the last step, or 0 if none since boot.
     */
    uint32_t rbtime_last_clock_step_epoch(void);

    /**
     * @brief Set the time manually.
     * @param[in] unix_timestamp_sec Time, epoch s.
     * @retval RBPOWER_OK                 Success.
     * @retval RBPOWER_ERR_NOT_INIT       Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT        The time-system mutex was not acquired.
     * @retval RBPOWER_ERR_TIME_SET_ERROR Setting the system clock failed.
     */
    rbpower_err_t rbtime_set_manual_time(uint64_t unix_timestamp_sec);

    /**
     * @brief Register a callback for time-status events.
     * @param[in] callback  Callback function (NULL to clear).
     * @param[in] user_data User pointer passed back to the callback.
     * @retval RBPOWER_OK           Success.
     * @retval RBPOWER_ERR_NOT_INIT Not initialised.
     * @retval RBPOWER_ERR_TIMEOUT  The time-system mutex was not acquired.
     */
    rbpower_err_t rbtime_register_event_callback(rbtime_event_callback_t callback, void *user_data);

    /**
     * @brief Get the current time.
     * @return Time, epoch s, or 0 if gettimeofday() fails. Not validated: check rbtime_is_time_valid().
     */
    uint64_t rbtime_get_unix_timestamp(void);

    /**
     * @brief Get the time since boot.
     * @return Uptime, µs (esp_timer_get_time()).
     */
    uint64_t rbtime_get_uptime_us(void);

    /**
     * @brief Check system time validity.
     * @return true if the time system is initialised, the status is SYNCED or FALLBACK, and the
     *         clock is not earlier than rbtime_not_before().
     */
    bool rbtime_is_time_valid(void);
    /**
     * @brief Earliest plausible wall-clock time: the firmware build day minus one day.
     * @return Epoch s; a clock earlier than this is never treated as valid.
     */
    uint32_t rbtime_not_before(void);   /* build day − 1 d: a wall clock earlier than this is never "valid" */

/* ================================================================================================
 * TIME MACROS AND CONSTANTS
 * ================================================================================================ */

/** @brief Default time configuration (time sync disabled, SNTP primary, UTC). */
#define RBTIME_DEFAULT_CONFIG() {          \
    .enable_time_sync = false,             \
    .primary_source = RBTIME_SOURCE_SNTP,  \
    .fallback_source = RBTIME_SOURCE_NONE, \
    .sntp_server = "pool.ntp.org",         \
    .sntp_server_backup = "time.nist.gov", \
    .sntp_timeout_ms = 10000,              \
    .timezone = "UTC+0",                   \
    .enable_dst = false,                   \
    .resync_interval_sec = 3600,           \
    .max_drift_sec = 60,                   \
    .esp_now_timeout_ms = 5000,            \
    .enable_rtc_fallback = true,           \
    .enable_cpu_time_fallback = true,      \
    .manual_time_us = 0,                   \
    .force_sync_on_startup = true,         \
    .enable_time_logging = true,           \
    .sync_retry_count = 3}

/** @brief True if the time system is initialised. */
#define RBTIME_IS_INITIALIZED() (rbtime_get_status() != RBTIME_STATUS_UNINITIALIZED)

/** @brief True if the time is synchronized. */
#define RBTIME_IS_SYNCED() (rbtime_get_status() == RBTIME_STATUS_SYNCED)

#define RBTIME_SECONDS_PER_HOUR 3600         ///< Seconds per hour
#define RBTIME_SECONDS_PER_DAY 86400         ///< Seconds per day
#define RBTIME_MICROSECONDS_PER_SEC 1000000ULL ///< Microseconds per second

#ifdef __cplusplus
}
#endif

#endif /* RBPOWER_H */