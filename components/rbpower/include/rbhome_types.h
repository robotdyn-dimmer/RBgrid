/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_types.h
 * @brief Core data model of the home electrical network (rbhome): units, roles, sources,
 *        realtime/period statistics, timers, callbacks and the rbhome/statistics API.
 *
 * The global network instance (g_home_network) and its units are shared between the
 * measurement-feed path, the statistics task, the period timers and the web/MQTT readers.
 * Readers and writers outside the statistics task must hold the home lock
 * (rbpower_home_lock() / rbpower_home_unlock()).
 *
 * Measurements come from rbAmp I2C modules and ESP-NOW nodes. Some structures still carry
 * fields designed for the removed native ESP32-ADC measurement path (zero-cross, half-cycle and
 * harmonic analysis); with rbAmp/ESP-NOW sources most of them stay unused.
 */
#ifndef RBHOME_TYPES_H
#define RBHOME_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>      /* struct tm — rbpower_stats_calc_buckets (FR-C3) */
#include <esp_err.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "rbpower_constants.h"
#include "rbpower_enums.h"
#include "rbtelemetry.h"
#include "rbpower_lifetime_accumulators.h"

/** @brief Hardware configuration (forward declaration of struct rbpower_config_s). */
typedef struct rbpower_config_s rbpower_hardware_config_t; // Forward declaration

/** @brief Unit identifier (16 bit). */
typedef uint16_t rbpower_unit_id_t;
/* ================================================================================================
 * SIMPLIFIED RESULT STRUCTURES
 * ================================================================================================ */
/**
 * @brief Channel processing type (legacy: designed for the removed native ADC path).
 */
typedef enum
{
    RBHAL_CHANNEL_TYPE_RMS = 0,   ///< RMS calculations (AC voltage, AC current)
    RBHAL_CHANNEL_TYPE_DC,        ///< DC linear measurements (DC current, temperature)
    RBHAL_CHANNEL_TYPE_FREQUENCY, ///< Frequency measurements (future)
    RBHAL_CHANNEL_TYPE_DIGITAL    ///< Digital signals (future)
} rbhal_channel_type_t;

/**
 * @brief Universal channel data depending on type
 */
typedef struct
{
    rbhal_channel_type_t channel_type; ///< Channel type
    rbsensor_class_t sensor_class;     ///< Sensor class
    bool active;                       ///< Channel activity

    // Universal fields (interpretation depends on type)
    float primary_value;   ///< Primary value
    float secondary_value; ///< Secondary value
    float range_value;     ///< Range/spread

    // Type-specific data
    union
    {
        // RMS channels (AC voltage, AC current)
        struct
        {
            float rms;           ///< RMS value
            float peak_positive; ///< Positive peak
            float peak_negative; ///< Negative peak
            float crest_factor;  ///< Crest factor
        } rms_data;

        // DC channels (DC current, temperature, pressure)
        struct
        {
            float dc_average;  ///< DC average value
            float dc_drift;    ///< Drift over period
            float noise_level; ///< Noise level
            float min_value;   ///< Minimum over period
            float max_value;   ///< Maximum over period
        } dc_data;

        // Frequency channels (future)
        struct
        {
            float frequency_hz;        ///< Frequency
            float period_us;           ///< Period
            float frequency_stability; ///< Frequency stability
            uint32_t cycles_count;     ///< Cycle count
        } frequency_data;

        // Digital channels (future)
        struct
        {
            bool digital_state;         ///< Current state
            uint32_t transitions_count; ///< Transition count
            uint32_t high_time_us;      ///< Time in high state
            uint32_t low_time_us;       ///< Time in low state
        } digital_data;
    } type_specific;   ///< Type-specific data, selected by @c channel_type

    // Metadata
    uint32_t samples_processed; ///< Samples processed
    uint8_t signal_quality;     ///< Signal quality 0-100%
    uint64_t timestamp_us;      ///< Timestamp

} rbpower_channel_data_t;

/**
 * @brief Simplified system statistics
 */
typedef struct
{
    uint64_t cycles_processed;   ///< Total cycles processed
    uint64_t samples_collected;  ///< Total samples collected
    uint32_t buffer_overruns;    ///< Buffer overflow count
    uint32_t calculation_errors; ///< Calculation error count
    float cpu_usage_percent;     ///< CPU usage in %
    uint32_t free_heap_bytes;    ///< Free memory in bytes
    uint64_t uptime_seconds;     ///< System uptime in seconds
} rbpower_statistics_t;          // !!! to delete

/**
 * @brief Simplified buffering configuration
 */
typedef struct
{
    uint8_t buffer_depth;       ///< Number of buffers
    uint16_t samples_per_cycle; ///< Samples per grid period
    uint8_t reserve_percent;    ///< Buffer reserve percentage
    bool auto_calculate;        ///< Automatic size calculation
} rbpower_buffer_config_t;

//=============================================================================
// HOME ELECTRICAL NETWORK - Integrated rbhome structures
//=============================================================================

/** @brief Metric (unit) type. */
typedef enum
{
    RBPOWER_METRIC_VOLTAGE_BUS = 0,  ///< Voltage bus (voltage reference)
    RBPOWER_METRIC_CURRENT_LOAD,     ///< Current-measured load
    RBPOWER_METRIC_POWER_LOAD,       ///< Power-measured load
    RBPOWER_METRIC_ENERGY_COUNTER,   ///< Energy counter
    RBPOWER_METRIC_FREQUENCY,        ///< Frequency
    RBPOWER_METRIC_POWER_FACTOR,     ///< Power factor
    RBPOWER_METRIC_COMPOSITE         ///< Composite (computed from several sources)
} rbpower_metric_type_t;

/** @brief Role of a unit in the home network. */
typedef enum
{
    RBPOWER_ROLE_MAIN_SUPPLY = 0,    ///< Main power supply (AC grid)
    RBPOWER_ROLE_ALTERNATIVE_SUPPLY, ///< Alternative power supply (inverter, solar, etc.)
    RBPOWER_ROLE_CIRCUIT_BREAKER,    ///< Circuit breaker / branch circuit
    RBPOWER_ROLE_DEVICE_LOAD,        ///< Single device load
    RBPOWER_ROLE_ZONE_LOAD,          ///< Zone (room/area) load
    RBPOWER_ROLE_RENEWABLE_SOURCE,   ///< Renewable source
    RBPOWER_ROLE_STORAGE_SYSTEM,     ///< Energy storage system
    RBPOWER_ROLE_EV_CHARGING,        ///< EV charging station
    RBPOWER_ROLE_HEATING_COOLING,    ///< Heating / cooling
    RBPOWER_ROLE_SMART_APPLIANCE     ///< Smart appliance
} rbpower_load_role_t;

/** @brief Data source type of a unit. */
typedef enum
{
    RBPOWER_SOURCE_ADC_SENSOR = 0,   ///< Native ESP32 ADC sensor (path removed; value kept for compatibility)
    RBPOWER_SOURCE_ESP_NOW,          ///< Remote ESP-NOW metering node
    RBPOWER_SOURCE_I2C_SENSOR,       ///< I2C metering module (rbAmp)
    RBPOWER_SOURCE_VIRTUAL_CALC,     ///< Virtual (computed) value
    RBPOWER_SOURCE_AGGREGATED,       ///< Aggregate of other units
    RBPOWER_SOURCE_EXTERNAL_API      ///< Fed through an external API
} rbpower_source_type_t;

/**
 * @brief Error flags for diagnostics
 */
typedef enum
{
    RBPOWER_ERROR_NONE = 0,                        ///< No error
    RBPOWER_ERROR_ADC_OVERLOAD = (1 << 0),         ///< ADC overload
    RBPOWER_ERROR_SENSOR_DISCONNECT = (1 << 1),    ///< Sensor disconnect
    RBPOWER_ERROR_VOLTAGE_OUT_OF_RANGE = (1 << 2), ///< Voltage out of range
    RBPOWER_ERROR_CURRENT_OUT_OF_RANGE = (1 << 3), ///< Current out of range
    RBPOWER_ERROR_THD_HIGH = (1 << 4),             ///< High THD
    RBPOWER_ERROR_FREQUENCY_DRIFT = (1 << 5),      ///< Frequency drift
    RBPOWER_ERROR_PHASE_ANOMALY = (1 << 6),        ///< Phase anomaly
    RBPOWER_ERROR_ZC_MISSED = (1 << 7)             ///< Missed ZC
} rbpower_error_flags_t;

//=============================================================================
// STRUCTURES
//=============================================================================
/**
 * @brief Data source for logical metric
 */
typedef struct
{
    rbpower_source_type_t source_type; ///< Data source type
    uint8_t source_id;                 ///< Source ID (channel number on the source)
    uint8_t sensor_profile_id;         ///< Sensor profile ID in rbsensor
    float weight_factor;               ///< Weight factor (for aggregation)
    bool is_primary;                   ///< Primary or backup source
    bool invert_sign;                  ///< Invert sign (for direction)
} rbpower_data_source_t;

/**
 * @brief Logical metric definition (configuration)
 */
typedef struct
{
    rbpower_unit_id_t unit_id; ///< Unique metric ID (16 bits)
    char name[32];             ///< Display name
    char location[16];         ///< Location

    rbpower_metric_type_t type;            ///< Metric type
    rbpower_load_role_t role;              ///< Network role
    rbpower_stat_period_type_t stat_class; ///< Statistics class (aggregation period)

    // Data sources (aggregation support)
    rbpower_data_source_t sources[RBPOWER_MAX_SOURCES_PER_UNIT]; ///< Data sources of the unit
    uint8_t sources_count; ///< Number of active sources

    // Metric hierarchy
    rbpower_unit_id_t parent_unit_id;                    ///< Parent metric ID (0xFFFF = root)
    rbpower_unit_id_t child_units[RBPOWER_MAX_CHILDREN]; ///< Child metric IDs
    uint8_t children_count;                              ///< Number of child metrics

    // Parameters and limits
    bool bidirectional_metering; ///< Bidirectional energy metering
    bool accumulate_energy;      ///< Accumulate energy
    float rated_power_w;         ///< Rated power (W)
    float max_current_a;         ///< Maximum current (A)
    float rated_voltage_v;       ///< Rated voltage (V)

    // Thresholds and notifications
    struct
    {
        float over_power_w;    ///< Over power threshold, W
        float under_voltage_v; ///< Under voltage threshold, V
        float over_current_a;  ///< Over current threshold, A
        float over_voltage_v;  ///< Over voltage threshold, V
        bool enable_alerts;    ///< Enable alerts
    } thresholds;              ///< Alert thresholds

    // Billing
    struct
    {
        float cost_per_kwh;          ///< Cost per kWh, tariff currency
        bool time_of_use_enabled;    ///< Time-of-use tariff
        uint8_t current_tariff_zone; ///< Current tariff zone
    } billing;                       ///< Per-unit billing settings

    // State
    bool is_active;                ///< Metric is active
    uint64_t created_timestamp_us; ///< Metric creation time

    /* ---- identity / physics decouple ----
     * unit_id is an OPAQUE handle (new units: allocated from 0x0100..0x3FFF; legacy units keep
     * their convention-derived ids). The physical binding lives in explicit fields, NOT in id bits:
     * device_uid = physical device identity (rbAmp: module index until module UIDs are available;
     * ESP-NOW: node MAC packed LE in the low 48 bits), channel_idx = channel on that device.
     * Sensor occupancy ("already bound?") is decided by scanning sources+these fields, never by
     * id equality. For legacy persisted units both fields are backfilled from the id bits at load. */
    char     point[33];   ///< stable external slug [a-z0-9_.-]{1,32}; "" until generated
    uint32_t caps;        ///< RBUNIT_CAP_* capability bitmask (from the source type)
    uint64_t device_uid;  ///< physical device identity (0 = not set / not applicable)
    uint8_t  channel_idx; ///< channel on the physical device
    uint8_t  phase;       ///< mains phase: 0 unknown, 1..3 L1..L3, 4 3P (all; U3I3), 5..7 L1L2/L2L3/L3L1 (two-pole; 5 = US split 240V)
    uint16_t group_id;    ///< reserved: logical grouping (0 = none)
    uint8_t  role_trunk;  ///< reserved: trunk/feeder role flag (0 = none)

    /* Short label for flow diagrams; "" = unset, the UI derives one
     * from name. Declarative only — no firmware consumer. */
    char     short_name[25]; ///< <= 24 BYTES UTF-8 (~12 Cyrillic characters) + NUL; limit published in schema limits.short_name_bytes

    /* EV station overrides (stations of different power, e.g. 3.7 kW and 22 kW, idle at different powers).
     * 0 = use system.ev. Stored only — charging sessions are computed by the web app. */
    uint32_t ev_charge_threshold_w;   ///< Charging detection threshold, 0..100000 W (0 = system default)
    uint16_t ev_idle_timeout_min;     ///< Idle timeout, 0..1440 min (0 = system default)

} rbpower_unit_definition_t;

/* Capability bits (unit_def.caps) — what the unit's SOURCE can measure/provide.
 * Exposed in the schema and telemetry. */
#define RBUNIT_CAP_VOLTAGE       0x0001u  ///< Measures voltage
#define RBUNIT_CAP_CURRENT       0x0002u  ///< Measures current
#define RBUNIT_CAP_ACTIVE_POWER  0x0004u  ///< Measures active power
#define RBUNIT_CAP_ENERGY        0x0008u  ///< Provides energy
#define RBUNIT_CAP_BIDIRECTIONAL 0x0010u  ///< Signed power/energy (export-capable)
#define RBUNIT_CAP_PF            0x0020u  ///< Measures power factor
#define RBUNIT_CAP_FREQ          0x0040u  ///< Measures frequency
#define RBUNIT_CAP_REMOTE        0x0080u  ///< Fed over the air (ESP-NOW node)
#define RBUNIT_CAP_VIRTUAL       0x0100u  ///< Computed, no physical sensor

//=============================================================================
// REAL-TIME STRUCTURE
//=============================================================================

/**
 * @brief Energy accumulators with direction separation
 */
typedef struct
{
    // Positive energy accumulators (consumption)
    double positive_energy_accumulator_wh; ///< Consumption accumulator (Wh)
    uint32_t positive_samples_count;       ///< Positive sample count
    uint64_t positive_duration_us;         ///< Consumption duration (us)

    // Negative energy accumulators (delivery)
    double negative_energy_accumulator_wh; ///< Delivery accumulator (Wh)
    uint32_t negative_samples_count;       ///< Negative sample count
    uint64_t negative_duration_us;         ///< Delivery duration (us)

    // Current energy increment per half-cycle
    float energy_increment_halfcycle_wh; ///< Half-cycle increment
    float energy_increment_200ms_wh;     ///< 200ms increment

    // Energy balance
    double net_energy_balance_wh; ///< Net balance (consumption - delivery)

} rbpower_energy_accumulators_t;

/**
 * @brief Zero-cross analysis and phase synchronization (legacy: designed for the removed
 *        native ADC path; not filled by rbAmp/ESP-NOW sources).
 */
typedef struct
{
    // ZC event counters
    uint32_t zc_transitions_count;    ///< Total ZC transitions
    uint32_t zc_missed_count;         ///< Missed ZC count
    uint32_t direction_changes_count; ///< Direction change count

    // Half-cycle phase analysis
    float voltage_phase_deg;    ///< Voltage phase (degrees)
    float current_phase_deg;    ///< Current phase (degrees)
    float phase_difference_deg; ///< U-I phase difference
    float phase_balance_factor; ///< Half-cycle balance (-1..+1)

    // ZC synchronization quality
    uint64_t last_zc_timestamp_us;  ///< Last ZC time
    uint32_t zc_interval_us;        ///< ZC interval (us)
    float zc_frequency_measured_hz; ///< Measured ZC frequency
    uint8_t zc_stability_percent;   ///< ZC stability (0-100%)

    // Current direction
    rbpower_current_direction_t current_direction; ///< Current direction
    rbpower_current_direction_t prev_direction;    ///< Previous direction
    uint32_t direction_stable_samples;             ///< Samples in stable direction

} rbpower_zc_phase_analysis_t;

/**
 * @brief Mini-extremes for short period (half-cycle/200ms)
 */
typedef struct
{
    // Half-cycle extremes
    struct
    {
        float voltage_peak;               ///< Peak voltage
        float current_peak;               ///< Peak current
        float power_peak;                 ///< Peak power, W
        uint64_t power_peak_timestamp_us; ///< Power peak time, µs since boot
    } halfcycle;                          ///< Half-cycle extremes (legacy native ADC path)

    // 200ms extremes (sliding)
    struct
    {
        float voltage_min; ///< Minimum voltage over 200ms
        float voltage_max; ///< Maximum voltage over 200ms
        float current_min; ///< Minimum current over 200ms
        float current_max; ///< Maximum current over 200ms
        float power_min;   ///< Minimum power over 200ms
        float power_max;   ///< Maximum power over 200ms

        // Extreme timestamps
        uint64_t voltage_min_time_us; ///< Time of the voltage minimum, µs since boot
        uint64_t voltage_max_time_us; ///< Time of the voltage maximum, µs since boot
        uint64_t power_min_time_us;   ///< Time of the power minimum, µs since boot
        uint64_t power_max_time_us;   ///< Time of the power maximum, µs since boot
    } window_200ms;                   ///< Sliding 200 ms window extremes

} rbpower_realtime_extremes_t;

/**
 * @brief THD and harmonic analysis for half-cycle
 */
typedef struct
{
    // Current THD values
    float thd_voltage_percent; ///< Voltage THD (%)
    float thd_current_percent; ///< Current THD (%)

    // Main harmonics (up to 5th for real-time)
    struct
    {
        float voltage_harmonics[5]; ///< Voltage harmonics 1-5 amplitudes
        float current_harmonics[5]; ///< Current harmonics 1-5 amplitudes
        float fundamental_voltage;  ///< Fundamental voltage harmonic
        float fundamental_current;  ///< Fundamental current harmonic
    } harmonics_basic;              ///< Basic harmonics (up to the 5th)

    // Half-cycle quality indicators
    float crest_factor_voltage;     ///< Voltage crest factor
    float crest_factor_current;     ///< Current crest factor
    uint8_t harmonic_quality_index; ///< Harmonic quality index (0-100%)

    // Quality event flags
    bool thd_voltage_alarm;            ///< Voltage THD alarm
    bool thd_current_alarm;            ///< Current THD alarm
    bool harmonic_distortion_detected; ///< Distortion detected

} rbpower_realtime_harmonics_t;

/** @brief Half-cycle electrical values (protection and diagnostics; legacy native ADC path). */
typedef struct
{
    float voltage_rms;        ///< RMS voltage for half-cycle
    float current_rms;        ///< RMS current for half-cycle
    float power_active_w;     ///< Active power for half-cycle
    float power_reactive_var; ///< Reactive power for half-cycle
} rbpower_realtime_halfcycle_t;

/** @brief 200 ms averaged electrical values (control). */
typedef struct
{
    float voltage_rms_avg;        ///< Average RMS voltage over 200ms
    float current_rms_avg;        ///< Average RMS current over 200ms
    float power_active_avg_w;     ///< Average active power over 200ms
    float power_reactive_avg_var; ///< Average reactive power over 200ms
    float power_apparent_avg_va;  ///< Average apparent power over 200ms
    float power_factor_avg;       ///< Average power factor over 200ms
    uint32_t samples_count;       ///< Number of averaged half-cycles
} rbpower_realtime_average_200ms_t;

/**
 * @brief MAIN REAL-TIME STRUCTURE
 */
typedef struct
{
    rbpower_unit_id_t unit_id; ///< Metric ID (16 bits)

    // === BASIC ELECTRICAL QUANTITIES ===
    // Half-cycle (instantaneous for protection)
    rbpower_realtime_halfcycle_t halfcycle; ///< Half-cycle values

    // === ENERGY ACCUMULATORS ===
    rbpower_energy_accumulators_t energy; ///< Energy accumulators

    // === ZC AND PHASE ANALYSIS ===
    rbpower_zc_phase_analysis_t zc_analysis; ///< ZC and phase analysis

    // === EXTREMES ===
    rbpower_realtime_extremes_t extremes; ///< Mini-extremes for short periods

    // === POWER QUALITY ===
    rbpower_realtime_harmonics_t harmonics; ///< THD and harmonics for real-time

    // === DIRECTION AND FLOWS (updated) ===
    float instant_power_consuming_w; ///< Instantaneous consuming power
    float instant_power_supplying_w; ///< Instantaneous supplying power
    float instant_power_net_w;       ///< Instantaneous net power

    // === DIAGNOSTICS AND METADATA ===
    bool is_valid;                ///< Data is valid
    uint64_t last_update_us;      ///< Last update time
    uint64_t timestamp_ZC_event;  ///< Last ZC timestamp
    uint8_t data_quality_percent; ///< Data quality (0-100%)
    uint32_t error_flags;         ///< Error flags (rbpower_error_flags_t)
    uint8_t sources_online;       ///< Number of active sources

    // === COUNTERS ===
    uint32_t update_counter;          ///< Update counter
    uint32_t error_counter;           ///< Error counter
    uint32_t halfcycle_counter;       ///< Half-cycle counter
    uint32_t samples_processed_total; ///< Total samples processed

} rbpower_unit_realtime_halfcycle_t;

/**
 * @brief MAIN REAL-TIME STRUCTURE
 */
typedef struct
{
    rbpower_unit_id_t unit_id; ///< Metric ID (16 bits)

    // === BASIC ELECTRICAL QUANTITIES ===
    // 200ms averaging (for control)
    rbpower_realtime_average_200ms_t average_200ms; ///< 200 ms averaged values

    // === ENERGY ACCUMULATORS ===
    rbpower_energy_accumulators_t energy; ///< Energy accumulators

    // === ZC AND PHASE ANALYSIS ===
    rbpower_zc_phase_analysis_t zc_analysis; ///< ZC and phase analysis

    // === EXTREMES ===
    rbpower_realtime_extremes_t extremes; ///< Mini-extremes for short periods

    // === POWER QUALITY ===
    rbpower_realtime_harmonics_t harmonics; ///< THD and harmonics for real-time

    // === DIRECTION AND FLOWS (updated) ===
    float instant_power_consuming_w; ///< Instantaneous consuming power
    float instant_power_supplying_w; ///< Instantaneous supplying power
    float instant_power_net_w;       ///< Instantaneous net power

    // === DIAGNOSTICS AND METADATA ===
    bool is_valid;                ///< Data is valid
    uint64_t last_update_us;      ///< Last update time
    uint64_t timestamp_ZC_event;  ///< Last ZC timestamp
    uint8_t data_quality_percent; ///< Data quality (0-100%)
    uint32_t error_flags;         ///< Error flags (rbpower_error_flags_t)
    uint8_t sources_online;       ///< Number of active sources

    // === COUNTERS ===
    uint32_t update_counter;          ///< Update counter
    uint32_t error_counter;           ///< Error counter
    uint32_t halfcycle_counter;       ///< Half-cycle counter
    uint32_t samples_processed_total; ///< Total samples processed

} rbpower_unit_realtime_average_200ms_t;

//=============================================================================
// STATISTICS CONFIGURATION
//=============================================================================

/**
 * @brief Statistics configuration for unit
 */
typedef struct
{
    uint32_t enabled_periods_mask;      ///< Bit mask of enabled periods
    bool auto_enable_by_unit_role;      ///< Auto-enable by unit role
    bool enable_flash_storage;          ///< Flash storage enabled
    bool enable_economics;              ///< Enable economic calculations
    bool enable_power_quality_analysis; ///< Enable quality analysis
    bool enable_direction_statistics;   ///< Enable direction statistics
    bool enable_predictions;            ///< Enable predictions

    // Memory limits
    uint16_t max_memory_usage_kb; ///< Memory limit for statistics (KB)
    uint8_t memory_priority;      ///< Priority when low memory (0-10)

    // Flash storage settings
    uint32_t flash_rotation_hours;    ///< Flash data rotation (hours)
    uint16_t flash_compression_level; ///< Compression level (0=none, 1-9)

    // Data quality settings
    uint8_t min_data_completeness_percent; ///< Minimum data completeness (%)
    uint32_t data_validation_flags;        ///< Data validation flags

} rbpower_statistics_unit_config_t;

/**
 * @brief Global statistics system configuration
 */
typedef struct
{
    bool statistics_enabled; ///< Global statistics enable
    bool storage_enabled;    ///< Global storage system enable (flash/SD)

    // Task settings
    uint8_t statistics_task_priority;    ///< Statistics task priority (recommended: tskIDLE_PRIORITY + 3)
    uint8_t callback_task_priority;      ///< Callback task priority (recommended: tskIDLE_PRIORITY + 1)
    uint16_t statistics_task_stack_size; ///< Statistics task stack size (recommended: 4096)
    uint16_t callback_task_stack_size;   ///< Callback task stack size (recommended: 3072)
    uint8_t statistics_task_core;        ///< Statistics task core (recommended: 0)
    uint8_t callback_task_core;          ///< Callback task core (recommended: 0)

    // Queue settings
    uint16_t statistics_queue_size; ///< Statistics queue size (recommended: 32)
    uint16_t callback_queue_size;   ///< Callback queue size (recommended: 16)
    uint32_t queue_timeout_ms;      ///< Queue operation timeout, ms

    // Timer settings
    bool enable_timer_auto_sync;        ///< Auto-sync timers with system time
    uint32_t timer_sync_interval_hours; ///< Resync interval (recommended: 6)
    uint16_t timer_drift_threshold_ms;  ///< Drift threshold for resync (recommended: 100)
    bool enable_timer_error_recovery;   ///< Auto-recovery on timer errors

    // Time settings
    bool enable_ntp_sync;         ///< Enable NTP synchronization
    const char *ntp_server;       ///< NTP server (default: "pool.ntp.org")
    int8_t timezone_offset_hours; ///< Timezone offset (-12 to +12)
    bool enable_dst;              ///< Automatic daylight saving time

    // Global limits
    uint32_t max_total_memory_kb;      ///< Maximum memory for all statistics
    uint16_t max_flash_usage_mb;       ///< Maximum Flash for statistics
    bool enable_memory_monitoring;     ///< Memory usage monitoring
    uint16_t memory_check_interval_ms; ///< Memory check interval

    // Error handling
    bool enable_error_recovery;    ///< Auto-recovery on errors
    uint8_t max_error_retries;     ///< Maximum error retries
    uint32_t error_retry_delay_ms; ///< Delay between retries

    // System-level callback functions
    void (*on_memory_limit_exceeded)(uint32_t current_kb, uint32_t limit_kb);         ///< Memory limit exceeded (current/limit, KB)
    void (*on_flash_storage_error)(esp_err_t error_code);                             ///< Flash storage error
    void (*on_timer_drift_detected)(uint32_t drift_ms);                               ///< Timer drift detected, ms
    void (*on_statistics_system_error)(esp_err_t error_code, const char *component); ///< Statistics system error in @p component

} rbpower_statistics_global_config_t;
//=============================================================================
// BASE UNIFIED ECONOMICS STRUCTURE
//=============================================================================

/**
 * @brief Base economics structure for Unit (replacement for rbpower_accumulator_economics_t and rbpower_longterm_economics_t)
 *
 * Applied in:
 * - Accumulators: rbpower_period_accumulator_t.economics
 * - Results: rbpower_period_statistics_t.economics
 * - For all units and all periods including MAIN_SUPPLY
 */
typedef struct
{
    // === MAIN ACCUMULATORS ===
    double cost_consumed;     ///< Consumption cost, tariff currency
    double revenue_supplied;  ///< Delivery revenue, tariff currency
    double net_cost;          ///< Net cost = cost_consumed - revenue_supplied, tariff currency
    float avg_tariff_per_kwh; ///< Average tariff for the period, tariff currency per kWh

    float consumed_wh; ///< Consumed for period (Wh)
    float supplied_wh; ///< Supplied for period (Wh)
    float net_wh;      ///< Balance for period (Wh)

    // === METADATA ===
    uint32_t samples_count;  ///< Sample count for average calculation
    uint64_t last_update_us; ///< Last update time (us)

} rbpower_unit_base_economics_t;

// === 4-FLOW SOURCE-AWARE ECONOMICS (EXPERIMENTAL SCAFFOLD) ===
// A simultaneous system snapshot (grid/pv/battery/load) feeds the merit-order attribution that
// decomposes energy by SOURCE. Units: W for the on-demand power form, Wh when integrated over a
// period. This is a scaffold: the pure function and the structs are derived/observational only —
// no flash format change, no change to the 2-quadrant economics. Degenerate input (no pv/battery)
// collapses to grid-only (no-op).
/** @brief Simultaneous system power snapshot for the 4-flow balance (W, or Wh when integrated). */
typedef struct
{
    float p_grid;  ///< main supply, signed: + import (from grid), - export (to grid)
    float p_pv;    ///< solar generation, >= 0
    float p_batt;  ///< battery, signed: + charging, - discharging
    float p_load;  ///< sum of loads, >= 0
} rbpower_energy_balance_input_t;

/** @brief Source-attributed energy flows (experimental 4-flow scaffold). */
typedef struct
{
    // 7 source-attributed flows (same unit as the input: W on-demand, Wh when integrated)
    float pv_to_load;        ///< PV self-consumed by load        (avoided cost @ retail)
    float pv_to_grid;        ///< PV exported                     (revenue @ feed-in)
    float charge_from_pv;    ///< battery charged by PV surplus   (free)
    float charge_from_grid;  ///< battery charged from grid       (cost @ import)
    float discharge_to_load; ///< battery serves load             (avoided cost @ retail)
    float discharge_to_grid; ///< battery exports                 (revenue @ feed-in)
    float grid_to_load;      ///< grid import serving load        (cost @ import)
    float self_consumption_ratio; ///< (pv_to_load + discharge_to_load) / p_load, 0..1 (0 if no load)
} rbpower_energy_4flow_t;

/**
 * @brief Merit-order source attribution (PV -> load -> battery -> grid); experimental scaffold.
 *
 * Pure total function: no globals, no side effects, @p out fully written. Negative/NaN inputs
 * are clamped to the physical domain.
 *
 * @param[in]  in  System power snapshot (W, or Wh when integrated).
 * @param[out] out Source-attributed flows, same unit as @p in.
 */
void rbpower_compute_energy_balance(const rbpower_energy_balance_input_t* in, rbpower_energy_4flow_t* out);

/**
 * @brief Gather a live system snapshot (grid/pv/battery/load) from unit realtime powers, by role.
 * @param[out] out Snapshot, W.
 */
void rbpower_gather_system_balance_input(rbpower_energy_balance_input_t* out);

/** @brief Base period snapshot (economics, energy, direction) shared by period results. */
typedef struct
{
    // === ECONOMICS ===
    float cost_consumed;       ///< Period cost, tariff currency
    float revenue_supplied;    ///< Period revenue, tariff currency
    float current_tariff_rate; ///< Last tariff rate, tariff currency per kWh
    float avg_tariff_per_kwh;  ///< Average rate for the period, tariff currency per kWh
    // rbpower_tariff_zone_type_t current_zone;// Last zone

    // === ENERGY ===
    float energy_consumed_wh; ///< Consumed (import), Wh
    float energy_supplied_wh; ///< Supplied (export), Wh

    // === STATE ===
    rbpower_current_direction_t direction; ///< Last direction

    // === METADATA ===
    uint64_t period_start_us; ///< Period start, µs
    uint64_t period_end_us;   ///< Period end, µs
    uint32_t samples_count;   ///< Sample count
    bool data_valid;          ///< Data validity

} rbgrid_snapshot_base_t;
//=============================================================================
// STATISTICAL ACCUMULATORS (correspond to rbpower_period_statistics_t)
//=============================================================================

/**
 * @brief Time boundary accumulators
 */
typedef struct
{
    uint64_t period_start_us;      ///< Current period start
    uint64_t period_end_us;        ///< Planned period end
    uint32_t expected_duration_ms; ///< Expected duration
    uint32_t actual_duration_ms;   ///< Actual duration
} rbpower_accumulator_time_boundaries_t;

/**
 * @brief Electrical quantity accumulators
 */
typedef struct
{
    // RMS accumulators (root-mean-square aggregation)
    double voltage_rms_sum_squares; ///< Sum of RMS voltage squares
    double current_rms_sum_squares; ///< Sum of RMS current squares
    uint32_t rms_samples_count;     ///< RMS sample count

    // Powers (linear aggregation)
    double power_active_sum;      ///< Active power sum
    double power_reactive_sum;    ///< Reactive power sum
    double power_apparent_sum;    ///< Apparent power sum
    double power_factor_sum;      ///< Power factor sum
    uint32_t power_samples_count; ///< Power sample count
    uint32_t pf_samples_count;    ///< Inputs that carried an apparent-power envelope (q/s/pf averaged over these only, 2026-10-15)

    // Frequency
    double frequency_sum;             ///< Frequency sum
    uint32_t frequency_samples_count; ///< Frequency sample count
} rbpower_accumulator_electrical_t;

/**
 * @brief Extreme value accumulators
 */
typedef struct
{
    // Current extremes
    float voltage_min; ///< Minimum voltage
    float voltage_max; ///< Maximum voltage
    float current_min; ///< Minimum current
    float current_max; ///< Maximum current
    float power_min;   ///< Minimum power
    float power_max;   ///< Maximum power

    // Extreme timestamps
    uint64_t voltage_min_time_us; ///< Minimum voltage time
    uint64_t voltage_max_time_us; ///< Maximum voltage time
    uint64_t power_min_time_us;   ///< Minimum power time
    uint64_t power_max_time_us;   ///< Maximum power time
    uint64_t power_peak_time_us;  ///< Peak power time

    bool extremes_initialized; ///< Extremes initialization flag
} rbpower_accumulator_extremes_t;

/**
 * @brief Energy accumulators
 */
typedef struct
{
    // Energy accumulators (direct summation)
    double consumed_wh_accumulator; ///< Consumed energy accumulator, Wh
    double supplied_wh_accumulator; ///< Supplied energy accumulator, Wh
    double net_wh_accumulator;      ///< Net energy accumulator, Wh

    // Sample counters for different directions
    uint32_t consuming_samples; ///< Consuming samples
    uint32_t supplying_samples; ///< Supplying samples
    uint32_t zero_samples;      ///< Zero current samples

    // Time accumulators
    uint64_t consuming_time_us; ///< Time in consuming mode
    uint64_t supplying_time_us; ///< Time in supplying mode
} rbpower_accumulator_energy_t;

/* rbpower_accumulator_power_quality_t / rbpower_accumulator_direction_stats_t REMOVED from the generic
 * per-unit accumulator: they were never written/read in the generic pipeline (THD/ZC/phase metrics came
 * only from the removed native ADC + zero-crossing analysis path, which rbAmp modules don't provide). The
 * specialized main_supply struct keeps its OWN power-quality/direction stats (rbpower_main_sup_*).
 * THD itself survives in the electrical record. */

/**
 * @brief Metadata accumulators
 */
typedef struct
{
    uint32_t samples_count;       ///< Total sample count
    uint32_t valid_samples_count; ///< Valid sample count
    uint32_t error_samples_count; ///< Error sample count

    uint64_t first_sample_time_us; ///< First sample time
    uint64_t last_sample_time_us;  ///< Last sample time
    uint64_t last_update_us;       ///< Last update time
    uint64_t last_zc_timestamp_us; ///< Last ZC timestamp

    bool period_complete;            ///< Period complete
    bool data_valid;                 ///< Data is valid
    uint32_t update_counter;         ///< Update counter
    uint32_t input_periods_expected; ///< Expected input period count
    uint32_t input_periods_received; ///< Received input period count
    bool ready_for_finalization;     ///< Ready for finalization
    uint32_t aggregation_iteration;  ///< Aggregation iteration number
    uint32_t covered_ms;             ///< Σ duration of the folded child periods, ms (HOUR: quarter windows incl. reboot-gap tails) — the honest `dur`
} rbpower_accumulator_metadata_t;

/**
 * @brief Complete accumulator for one period
 */
typedef struct
{
    rbpower_stat_period_type_t period_type; ///< Period type
    bool is_active;                         ///< Accumulator is active
    bool is_enabled;                        ///< Period enabled in configuration

    // Accumulators (structure corresponds to rbpower_period_statistics_t)
    rbpower_accumulator_time_boundaries_t time_boundaries; ///< Period time boundaries
    rbpower_accumulator_electrical_t electrical;           ///< Electrical quantity sums
    rbpower_accumulator_extremes_t extremes;               ///< Period extremes
    rbpower_accumulator_energy_t energy;                   ///< Energy sums
    rbpower_unit_base_economics_t economics;               ///< Economics sums
    rbgrid_snapshot_base_t result_base; ///< Resulting statistics after period
    rbpower_accumulator_metadata_t metadata;               ///< Sample counts, coverage and state

} rbpower_period_accumulator_t;

//=============================================================================
// LONG-TERM STATISTICS
//=============================================================================

/**
 * @brief Extended ZC and phase statistics for long-term analysis (legacy native ADC path).
 */
typedef struct
{
    // ZC statistics
    uint32_t total_zc_events;       ///< Total ZC events
    uint32_t missed_zc_events;      ///< Missed ZC events
    float zc_stability_avg_percent; ///< Average ZC stability
    float frequency_deviation_rms;  ///< RMS frequency deviation

    // Phase statistics
    float phase_balance_avg;       ///< Average phase balance
    float phase_balance_rms;       ///< RMS phase balance
    uint32_t phase_anomaly_events; ///< Phase anomaly count

    // Direction transition statistics
    struct
    {
        uint32_t consuming_to_supplying; ///< Transitions: consuming → supplying
        uint32_t supplying_to_consuming; ///< Transitions: supplying → consuming
        uint32_t rapid_transitions;      ///< Rapid transitions (<100ms)
        float avg_transition_time_ms;    ///< Average transition time, ms
    } direction_transitions;             ///< Power direction transition statistics

} rbpower_longterm_zc_stats_t;

/**
 * @brief Finalized statistics of one period of one unit.
 */
typedef struct rbpower_period_statistics
{
    // === TIME BOUNDARIES ===
    uint64_t period_start_us;    ///< Period start, µs
    uint64_t period_end_us;      ///< Period end, µs
    uint32_t period_duration_ms; ///< Period duration (ms)

    // === ELECTRICAL QUANTITIES ===
    float voltage_rms;        ///< RMS voltage for the period, V
    float current_rms;        ///< RMS current for the period, A
    float power_active_avg;   ///< Average active power, W
    float power_reactive_avg; ///< Average reactive power, var
    float power_apparent_avg; ///< Average apparent power, VA
    float power_factor_avg;   ///< Average power factor
    float frequency_avg;      ///< Average frequency, Hz

    // === PERIOD EXTREMES ===
    struct
    {
        float voltage_min;           ///< Minimum voltage, V
        float voltage_max;           ///< Maximum voltage, V
        float current_min;           ///< Minimum current, A
        float current_max;           ///< Maximum current, A
        float power_min;             ///< Minimum power, W
        float power_max;             ///< Maximum power, W
        uint64_t power_peak_time_us; ///< Power peak time, µs
    } extremes;                      ///< Period extremes

    // === PERIOD ENERGY ===
    struct
    {
        float consumed_wh; ///< Consumed for period (Wh)
        float supplied_wh; ///< Supplied for period (Wh)
        float net_wh;      ///< Balance for period (Wh)
    } energy_period;       ///< Period energy

    /* Exact integer mirrors: set by the DAY+ flash recompute,
     * which sums child records in int64 — the float energy_period/result_base fields quantize at
     * YEAR scale (32 Wh / sub-cent steps). exact_valid=false → derive the v4 record from the
     * floats as before (live 15min/HOUR path, where float is exact at period size). */
    struct
    {
        int64_t ec_mwh;    ///< consumed, mWh
        int64_t es_mwh;    ///< supplied, mWh
        int64_t c_cents;   ///< cost, currency minor units
        int64_t r_cents;   ///< revenue, currency minor units
        bool    valid;     ///< true when the exact values are set
    } exact;               ///< Exact integer mirrors of energy and money

    // === POWER QUALITY (unchanged) ===
    struct
    {
        float thd_voltage_avg;         ///< Average voltage THD, %
        float thd_voltage_95p;         ///< 95th percentile voltage THD, %
        float thd_current_avg;         ///< Average current THD, %
        uint32_t voltage_events_count; ///< Voltage event count
        uint32_t quality_alerts_count; ///< Quality event count
    } power_quality;                   ///< Power quality (THD round-trip)

    // === ECONOMICS ===
    //    rbpower_unit_base_economics_t economics; // Extended economic statistics
    rbgrid_snapshot_base_t result_base; ///< Resulting statistics after period

    // === DIRECTION STATISTICS — REMOVED (data-type dropped from the active pipeline; no ZC source in
    //     rbAmp modules). power_quality above is kept only as the THD round-trip home. ===

    // === METADATA ===
    uint32_t samples_count;            ///< Averaged sample count
    uint32_t valid_samples_count;      ///< Valid sample count
    uint8_t data_completeness_percent; ///< Data completeness (0-100%)
    bool period_complete;              ///< Period complete
    uint64_t last_update_us;           ///< Last update time, µs since boot
    uint64_t timestamp_ZC_event;       ///< ZC timestamp (legacy native ADC path)

    // === v4 record metadata (RAM-only) ===
    // Filled by the finalizer that KNOWS the calendar bucket (flash-recompute DAY+); 0 = not set,
    // store_period_to_flash_sync then falls back to its naive samples×child_dur computation.
    uint32_t v4_dur_s;                 ///< covered duration, s
    uint16_t v4_q;                     ///< RBSTATS_Q_* bits computed at finalize

} rbpower_period_statistics_t;

/**
 * @brief Complete time-based statistics for metric
 */
typedef struct
{
    rbpower_unit_id_t unit_id; ///< Metric ID (16 bits)

    // === STATISTICS CONFIGURATION ===
    rbpower_statistics_unit_config_t config; ///< Statistics configuration for this unit

    // === PERIOD STATISTICS ===
    rbpower_period_statistics_t periods[RBPOWER_STAT_PERIOD_COUNT]; ///< Last finalized statistics per period

    // NOTE: the per-unit accumulators live in the .c-file global g_period_accumulators[][]
    // (rbhome_statistics.c). An embedded `accumulators[RBPOWER_STAT_PERIOD_COUNT]` field
    // here was never referenced and was removed to reclaim ~3.9 KB/unit of DRAM.
    // Do not re-add it; use the global.

    // === ACCUMULATED STATISTICS === (since system startup)
    struct
    {
        double total_consumed_kwh;        ///< Total consumption since startup, kWh
        double total_supplied_kwh;        ///< Total supply since startup, kWh
        double total_cost;                ///< Total cost since startup, tariff currency
        uint64_t system_uptime_hours;     ///< System uptime (hours)
        uint32_t total_direction_changes; ///< Total direction changes
        uint64_t first_measurement_us;    ///< First measurement time, µs since boot
    } lifetime_totals;                    ///< Totals since system startup

    // === MOVING AVERAGES ===
    struct
    {
        float power_trend_1h;       ///< Power trend for 1 hour
        float power_trend_24h;      ///< Power trend for 24 hours
        float consumption_trend_7d; ///< Consumption trend for 7 days
        bool trends_valid;          ///< Trends validity
    } trends;                       ///< Moving-average trends

    // Predictions (simple linear)
    struct
    {
        float predicted_daily_consumption; ///< Daily consumption prediction
        float predicted_monthly_cost;      ///< Monthly cost prediction
        uint64_t prediction_timestamp_us;  ///< Prediction calculation time
        uint8_t prediction_confidence;     ///< Prediction confidence (0-100%)
    } predictions;                         ///< Simple linear predictions

    // === UPDATE MANAGEMENT ===
    uint64_t last_period_update[RBPOWER_STAT_PERIOD_COUNT]; ///< Update time for each period
    bool auto_update_enabled[RBPOWER_STAT_PERIOD_COUNT];    ///< Auto-update enabled
    bool statistics_enabled;                                ///< Statistics enabled for this metric

    // === STATE AND DIAGNOSTICS ===
    struct
    {
        uint32_t memory_usage_bytes;        ///< Memory usage
        uint16_t flash_usage_kb;            ///< Flash memory usage
        uint32_t total_timer_events;        ///< Total timer events
        uint32_t missed_timer_events;       ///< Missed timer events
        uint64_t last_statistics_update_us; ///< Last statistics update time
        esp_err_t last_error;               ///< Last error
    } diagnostics;                          ///< State and diagnostics

    // === LIFETIME ACCUMULATORS ===
    rbpower_lifetime_accumulators_basic_t lifetime_accumulators; ///< Daily/lifetime energy and cost accumulators

} rbpower_unit_statistics_t;

//=============================================================================
// STATISTICS SYSTEM - Period masks and configuration
//=============================================================================

// Bit masks for statistics periods (excluding REALTIME and BASE_200MS)
#define RBPOWER_PERIOD_MASK_3SEC (1 << 0)      ///< 3-second period bit
#define RBPOWER_PERIOD_MASK_15MIN (1 << 1)     ///< 15-minute period bit
#define RBPOWER_PERIOD_MASK_1HOUR (1 << 2)     ///< 1-hour period bit
#define RBPOWER_PERIOD_MASK_DAILY (1 << 3)     ///< Daily period bit
#define RBPOWER_PERIOD_MASK_WEEKLY (1 << 4)    ///< Weekly period bit
#define RBPOWER_PERIOD_MASK_MONTHLY (1 << 5)   ///< Monthly period bit
#define RBPOWER_PERIOD_MASK_YEARLY (1 << 6)    ///< Yearly period bit

// Preset mask combinations
#define RBPOWER_PERIODS_NONE 0                 ///< No periods
/** @brief Preset: 3 s, 15 min, 1 h. */
#define RBPOWER_PERIODS_BASIC (RBPOWER_PERIOD_MASK_3SEC | RBPOWER_PERIOD_MASK_15MIN | RBPOWER_PERIOD_MASK_1HOUR)
/** @brief Preset: BASIC + daily. */
#define RBPOWER_PERIODS_EXTENDED (RBPOWER_PERIODS_BASIC | RBPOWER_PERIOD_MASK_DAILY)
/** @brief Preset: EXTENDED + monthly. */
#define RBPOWER_PERIODS_FULL (RBPOWER_PERIODS_EXTENDED | RBPOWER_PERIOD_MASK_MONTHLY)
/** @brief Preset: FULL + yearly. */
#define RBPOWER_PERIODS_COMPLETE (RBPOWER_PERIODS_FULL | RBPOWER_PERIOD_MASK_YEARLY)

// Specialized masks by unit types
/** @brief Period mask for a main supply unit (bits by rbpower_stat_period_type_t: 3 s, 15 min, 1 h, day). */
#define RBPOWER_PERIODS_MAIN_SUPPLY ((1 << RBPOWER_STAT_PERIOD_3SEC) |  \
                                     (1 << RBPOWER_STAT_PERIOD_15MIN) | \
                                     (1 << RBPOWER_STAT_PERIOD_1HOUR) | \
                                     (1 << RBPOWER_STAT_PERIOD_DAILY))

/** @brief Period mask for a zone load (3 s, 15 min, 1 h). */
#define RBPOWER_PERIODS_ZONE_LOAD ((1 << RBPOWER_STAT_PERIOD_3SEC) |  \
                                   (1 << RBPOWER_STAT_PERIOD_15MIN) | \
                                   (1 << RBPOWER_STAT_PERIOD_1HOUR))

/** @brief Period mask for a voltage bus (3 s, 15 min, 1 h, day). */
#define RBPOWER_PERIODS_VOLTAGE_BUS ((1 << RBPOWER_STAT_PERIOD_3SEC) |  \
                                     (1 << RBPOWER_STAT_PERIOD_15MIN) | \
                                     (1 << RBPOWER_STAT_PERIOD_1HOUR) | \
                                     (1 << RBPOWER_STAT_PERIOD_DAILY))

#define RBPOWER_PERIODS_DEVICE ((1 << RBPOWER_STAT_PERIOD_3SEC)) ///< Period mask for a device (3 s only)

//=============================================================================
// TIMER SYSTEM AND STATISTICS STATES
//=============================================================================

/**
 * @brief Period timer states
 */
typedef enum
{
    RBPOWER_TIMER_STATE_STOPPED = 0, ///< Timer stopped
    RBPOWER_TIMER_STATE_RUNNING,     ///< Timer running
    RBPOWER_TIMER_STATE_PAUSED,      ///< Timer paused
    RBPOWER_TIMER_STATE_EXPIRED,     ///< Timer expired
    RBPOWER_TIMER_STATE_ERROR        ///< Timer error
} rbpower_timer_state_t;

/**
 * @brief Timer events
 */
typedef enum
{
    RBPOWER_TIMER_EVENT_PERIOD_START = 0, ///< Period start
    RBPOWER_TIMER_EVENT_PERIOD_END,       ///< Period end
    RBPOWER_TIMER_EVENT_SYNC_REQUIRED,    ///< Sync required
    RBPOWER_TIMER_EVENT_ERROR             ///< Timer error
} rbpower_timer_event_t;

/**
 * @brief Statistics event flags (bit mask)
 */
typedef enum
{
    RBPOWER_STAT_EVENT_NONE = 0,
    RBPOWER_STAT_EVENT_PERIOD_START_3SEC = (1 << 0),
    RBPOWER_STAT_EVENT_PERIOD_END_3SEC = (1 << 1),
    RBPOWER_STAT_EVENT_PERIOD_START_15MIN = (1 << 2),
    RBPOWER_STAT_EVENT_PERIOD_END_15MIN = (1 << 3),
    RBPOWER_STAT_EVENT_PERIOD_START_1HOUR = (1 << 4),
    RBPOWER_STAT_EVENT_PERIOD_END_1HOUR = (1 << 5),
    RBPOWER_STAT_EVENT_PERIOD_START_DAILY = (1 << 6),
    RBPOWER_STAT_EVENT_PERIOD_END_DAILY = (1 << 7),
    RBPOWER_STAT_EVENT_PERIOD_START_WEEKLY = (1 << 8),
    RBPOWER_STAT_EVENT_PERIOD_END_WEEKLY = (1 << 9),
    RBPOWER_STAT_EVENT_PERIOD_START_MONTHLY = (1 << 10),
    RBPOWER_STAT_EVENT_PERIOD_END_MONTHLY = (1 << 11),
    RBPOWER_STAT_EVENT_PERIOD_START_YEARLY = (1 << 12),
    RBPOWER_STAT_EVENT_PERIOD_END_YEARLY = (1 << 13),
    RBPOWER_STAT_EVENT_TIME_SYNC_REQUIRED = (1 << 14),
    RBPOWER_STAT_EVENT_MEMORY_WARNING = (1 << 15),
    RBPOWER_STAT_EVENT_FLASH_ERROR = (1 << 16)
} rbpower_stat_event_flags_t;

/**
 * @brief Period timer information
 */
typedef struct
{
    esp_timer_handle_t timer_handle;        ///< ESP timer handle
    rbpower_stat_period_type_t period_type; ///< Period type
    rbpower_timer_state_t state;            ///< Timer state

    // Time parameters
    uint64_t period_duration_us;   ///< Period duration (us)
    uint64_t next_trigger_time_us; ///< Next trigger time
    uint64_t last_trigger_time_us; ///< Last trigger time

    // System time synchronization
    uint64_t sync_offset_us; ///< Sync offset
    bool requires_sync;      ///< Sync required
    uint32_t sync_drift_us;  ///< Accumulated drift

    // Timer operation statistics
    uint32_t total_triggers;   ///< Total trigger count
    uint32_t missed_triggers;  ///< Missed trigger count
    uint32_t sync_corrections; ///< Sync correction count

} rbpower_timer_info_t;

/**
 * @brief Event context for tasks
 */
typedef struct
{
    rbpower_timer_event_t event_type;       ///< Event type
    rbpower_stat_period_type_t period_type; ///< Period type
    rbpower_unit_id_t unit_id;              ///< Unit ID (if applicable)
    uint64_t event_timestamp_us;            ///< Event time

    // Additional event data
    union
    {
        struct
        {
            uint64_t period_start_us;    ///< Period start
            uint64_t period_duration_us; ///< Period duration
        } period_event;

        struct
        {
            uint32_t drift_us;    ///< Timer drift
            bool sync_successful; ///< Sync successful
        } sync_event;

        struct
        {
            esp_err_t error_code;          ///< Error code
            const char *error_description; ///< Error description
        } error_event;
    } data;                                ///< Event-specific data

} rbpower_timer_event_context_t;

/**
 * @brief Global statistics timer system state
 */
typedef struct
{
    bool system_initialized; ///< System initialized
    bool timers_running;     ///< Timers running
    bool time_synchronized;  ///< Time synchronized with NTP

    // Timers for all periods
    rbpower_timer_info_t timers[RBPOWER_STAT_PERIOD_COUNT]; ///< Per-period timer state

    // FreeRTOS tasks
    TaskHandle_t statistics_task_handle;  ///< Statistics task handle
    TaskHandle_t callback_task_handle;    ///< Callback task handle
    QueueHandle_t statistics_event_queue; ///< Statistics event queue
    QueueHandle_t callback_event_queue;   ///< Callback event queue

    // Time system state
    struct
    {
        uint64_t system_start_time_us;  ///< System start time, µs
        uint64_t last_ntp_sync_time_us; ///< Last NTP sync time, µs
        uint64_t next_sync_time_us;     ///< Next sync time, µs
        uint32_t sync_interval_hours;   ///< Sync interval, h
        int32_t timezone_offset_hours;  ///< Timezone offset, h
        bool dst_active;                ///< Daylight saving time active
    } time_context;                     ///< Time system state

    // Global statistics
    struct
    {
        uint32_t total_events_processed; ///< Total events processed
        uint32_t events_dropped;         ///< Dropped events (queue overflow)
        uint32_t timer_errors;           ///< Timer errors
        uint32_t memory_warnings;        ///< Memory warnings
        uint64_t last_activity_time_us;  ///< Last activity time, µs since boot
    } global_stats;                      ///< Global pipeline counters

    // Configuration
    rbpower_statistics_global_config_t config; ///< Global statistics configuration

} rbpower_statistics_system_t;

//=============================================================================
// CALLBACK AND SUBSCRIPTION SYSTEM
//=============================================================================

/**
 * @brief Callback event types
 */
typedef enum
{
    RBPOWER_CALLBACK_PERIOD_STARTED = 0, ///< Period started
    RBPOWER_CALLBACK_PERIOD_COMPLETED,   ///< Period completed
    RBPOWER_CALLBACK_STATISTICS_UPDATED, ///< Statistics updated
    RBPOWER_CALLBACK_THRESHOLD_EXCEEDED, ///< Threshold exceeded
    RBPOWER_CALLBACK_MEMORY_WARNING,     ///< Memory warning
    RBPOWER_CALLBACK_SYSTEM_ERROR        ///< System error
} rbpower_callback_type_t;

/**
 * @brief Callback priorities
 */
typedef enum
{
    RBPOWER_CALLBACK_PRIORITY_LOW = 0, ///< Low priority
    RBPOWER_CALLBACK_PRIORITY_MEDIUM,  ///< Medium priority
    RBPOWER_CALLBACK_PRIORITY_HIGH,    ///< High priority
    RBPOWER_CALLBACK_PRIORITY_CRITICAL ///< Critical priority
} rbpower_callback_priority_t;

/**
 * @brief Callback event data
 */
typedef struct
{
    rbpower_callback_type_t callback_type;  ///< Callback type
    rbpower_unit_id_t unit_id;              ///< Unit ID
    rbpower_stat_period_type_t period_type; ///< Period type
    uint64_t timestamp_us;                  ///< Event time

    // Type-dependent data
    union
    {
        struct
        {
            uint64_t period_start_us;      ///< Period start
            uint32_t expected_duration_ms; ///< Expected duration
        } period_started;

        struct
        {
            const rbpower_period_statistics_t *statistics; ///< Pointer to statistics
            bool period_complete;                          ///< Period fully complete
            uint8_t data_completeness_percent;             ///< Data completeness
        } period_completed;

        struct
        {
            uint32_t current_usage_bytes; ///< Current usage
            uint32_t limit_bytes;         ///< Limit
            const char *component_name;   ///< Component name
        } memory_warning;

        struct
        {
            esp_err_t error_code;       ///< Error code
            const char *error_message;  ///< Error message
            const char *component_name; ///< Component with error
        } system_error;
    } data;                             ///< Type-dependent event data

} rbpower_callback_event_t;

/**
 * @brief User callback function
 *
 * @param[in] event     Event data.
 * @param[in] user_data User pointer given at subscription.
 */
typedef void (*rbpower_user_callback_t)(const rbpower_callback_event_t *event, void *user_data);

/**
 * @brief Callback subscription
 */
typedef struct
{
    rbpower_user_callback_t callback_function; ///< Callback function
    void *user_data;                           ///< User data
    rbpower_callback_type_t callback_type;     ///< Event type
    rbpower_callback_priority_t priority;      ///< Priority
    rbpower_unit_id_t unit_id_filter;          ///< Unit ID filter (0xFFFF = all)
    uint32_t period_mask_filter;               ///< Period filter
    bool is_active;                            ///< Subscription active
    uint32_t call_count;                       ///< Call count
    uint64_t last_call_time_us;                ///< Last call time
} rbpower_callback_subscription_t;

#define RBPOWER_MAX_CALLBACK_SUBSCRIPTIONS 16 ///< Maximum subscriptions

/**
 * @brief Callback subscription registry
 */
typedef struct
{
    rbpower_callback_subscription_t subscriptions[RBPOWER_MAX_CALLBACK_SUBSCRIPTIONS]; ///< Subscription slots
    uint8_t subscription_count; ///< Active subscription count
    uint32_t total_calls;       ///< Total call count
    uint32_t dropped_calls;     ///< Dropped calls
} rbpower_callback_registry_t;

//=============================================================================
// UTILITY MACROS FOR TIMERS AND EVENTS
//=============================================================================

// Period durations in microseconds
#define RBPOWER_PERIOD_3SEC_US (3ULL * 1000000)                    ///< 3 seconds
#define RBPOWER_PERIOD_15MIN_US (15ULL * 60 * 1000000)             ///< 15 minutes
#define RBPOWER_PERIOD_1HOUR_US (60ULL * 60 * 1000000)             ///< 1 hour
#define RBPOWER_PERIOD_DAILY_US (24ULL * 60 * 60 * 1000000)        ///< 24 hours
#define RBPOWER_PERIOD_WEEKLY_US (7ULL * 24 * 60 * 60 * 1000000)   ///< 7 days
#define RBPOWER_PERIOD_MONTHLY_US (30ULL * 24 * 60 * 60 * 1000000) ///< 30 days (approximate)
#define RBPOWER_PERIOD_YEARLY_US (365ULL * 24 * 60 * 60 * 1000000) ///< 365 days

/* RBGRID_STATS_TIME_SCALE (statistics test-time acceleration toggle) lives in rbpower_constants.h
 * so rbtime.c (SNTP) sees it too — this header includes rbpower_constants.h. */

/** @brief Nominal duration of a statistics period, µs (0 for unknown periods). */
#define RBPOWER_GET_PERIOD_DURATION_US(period_type)                                                                                              \
    ((period_type) == RBPOWER_STAT_PERIOD_3SEC ? RBPOWER_PERIOD_3SEC_US : (period_type) == RBPOWER_STAT_PERIOD_15MIN ? RBPOWER_PERIOD_15MIN_US   \
                                                                      : (period_type) == RBPOWER_STAT_PERIOD_1HOUR   ? RBPOWER_PERIOD_1HOUR_US   \
                                                                      : (period_type) == RBPOWER_STAT_PERIOD_DAILY   ? RBPOWER_PERIOD_DAILY_US   \
                                                                      : (period_type) == RBPOWER_STAT_PERIOD_WEEKLY  ? RBPOWER_PERIOD_WEEKLY_US  \
                                                                      : (period_type) == RBPOWER_STAT_PERIOD_MONTHLY ? RBPOWER_PERIOD_MONTHLY_US \
                                                                      : (period_type) == RBPOWER_STAT_PERIOD_YEARLY  ? RBPOWER_PERIOD_YEARLY_US  \
                                                                                                                     : 0)

/** @brief true when the timer is RUNNING. */
#define RBPOWER_IS_TIMER_ACTIVE(timer_info) \
    ((timer_info)->state == RBPOWER_TIMER_STATE_RUNNING)

// Generate event masks for period
#define RBPOWER_PERIOD_START_EVENT_MASK(period) (1 << ((period) * 2))      ///< Start-event bit of a period
#define RBPOWER_PERIOD_END_EVENT_MASK(period) (1 << ((period) * 2 + 1))    ///< End-event bit of a period

// Check events in mask
#define RBPOWER_HAS_EVENT(mask, event) ((mask) & (event))        ///< Test an event bit
#define RBPOWER_ADD_EVENT(mask, event) ((mask) |= (event))       ///< Set an event bit
#define RBPOWER_CLEAR_EVENT(mask, event) ((mask) &= ~(event))    ///< Clear an event bit

// Time conversion
#define RBPOWER_US_TO_MS(us) ((us) / 1000)                       ///< µs -> ms
#define RBPOWER_MS_TO_US(ms) ((ms) * 1000)                       ///< ms -> µs
#define RBPOWER_SEC_TO_US(sec) ((sec) * 1000000)                 ///< s -> µs
#define RBPOWER_MIN_TO_US(min) ((min) * 60 * 1000000)            ///< min -> µs
#define RBPOWER_HOUR_TO_US(hour) ((hour) * 60 * 60 * 1000000)    ///< h -> µs

/** @brief Align a time down to a multiple of the interval (same unit, e.g. µs). */
#define RBPOWER_ALIGN_TO_INTERVAL(time_us, interval_us) \
    (((time_us) / (interval_us)) * (interval_us))

/** @brief Next interval boundary after the aligned time (same unit, e.g. µs). */
#define RBPOWER_NEXT_INTERVAL_BOUNDARY(time_us, interval_us) \
    (RBPOWER_ALIGN_TO_INTERVAL((time_us), (interval_us)) + (interval_us))

//=============================================================================
// UNIFIED UNIT STRUCTURE - for storing all metric data
//=============================================================================

/**
 * @brief Static Unit - combines all data for one metric
 */
typedef struct
{
    // === MAIN COMPONENTS ===
    rbpower_unit_definition_t definition;                         ///< Metric configuration
    rbpower_unit_realtime_halfcycle_t realtime;                   ///< Real-time data
    rbpower_unit_realtime_average_200ms_t realtime_average_200ms; ///< 200ms moving average
    rbpower_unit_statistics_t statistics;                         ///< Time statistics

    // === COMPONENT MANAGEMENT ===
    bool has_valid_definition;   ///< Definition configured
    bool has_realtime_data;      ///< Real-time data available
    bool has_statistics_enabled; ///< Statistics enabled
    bool hardware_present;       ///< Bound hardware/sensor detected present (runtime). Defaults true
                                 ///< at unit creation; a hardware-detection layer (e.g. the rbAmp
                                 ///< bridge probe) sets it false when the unit's sensor is absent, and
                                 ///< the compute gate then skips the unit (no wasted compute on dormant units).
                                 ///< Separate from definition.is_active (= user intent).

    // === META INFORMATION ===
    uint64_t last_update_us;     ///< Last update time, µs since boot
    uint32_t total_updates;      ///< Total update count
    uint64_t timestamp_ZC_event; ///< ZC timestamp (legacy native ADC path)

} rbpower_home_unit_t;

//=============================================================================
// UTILITY MACROS FOR PERIOD OPERATIONS
//=============================================================================

/** @brief Test whether @p period is enabled in a rbpower_statistics_unit_config_t. */
#define RBPOWER_IS_PERIOD_ENABLED(config, period) \
    ((config)->enabled_periods_mask & (1 << (period)))

/** @brief Enable @p period in a rbpower_statistics_unit_config_t. */
#define RBPOWER_ENABLE_PERIOD(config, period) \
    ((config)->enabled_periods_mask |= (1 << (period)))

/** @brief Disable @p period in a rbpower_statistics_unit_config_t. */
#define RBPOWER_DISABLE_PERIOD(config, period) \
    ((config)->enabled_periods_mask &= ~(1 << (period)))

/** @brief Number of enabled periods in a mask. */
#define RBPOWER_COUNT_ENABLED_PERIODS(mask) \
    (__builtin_popcount(mask))

/** @brief Default period mask for a unit role. */
#define RBPOWER_AUTO_PERIODS_BY_ROLE(role)                                                                                              \
    ((role) == RBPOWER_ROLE_MAIN_SUPPLY ? RBPOWER_PERIODS_COMPLETE : (role) == RBPOWER_ROLE_RENEWABLE_SOURCE ? RBPOWER_PERIODS_COMPLETE \
                                                                 : (role) == RBPOWER_ROLE_STORAGE_SYSTEM     ? RBPOWER_PERIODS_FULL     \
                                                                 : (role) == RBPOWER_ROLE_ZONE_LOAD          ? RBPOWER_PERIODS_EXTENDED \
                                                                                                             : RBPOWER_PERIODS_BASIC)

/** @brief Convert rbpower_period_type_t to rbpower_stat_period_type_t (RBPOWER_STAT_PERIOD_COUNT if not representable). */
#define RBPOWER_TO_STAT_PERIOD(period_type) \
    ((period_type) >= RBPOWER_PERIOD_SHORT_3SEC ? (rbpower_stat_period_type_t)((period_type) - RBPOWER_PERIOD_SHORT_3SEC) : RBPOWER_STAT_PERIOD_COUNT)

//=============================================================================
// HARDWARE MAPPING STRUCTURES
//=============================================================================
/**
 * @brief Data source mapping to unit (with support for different types)
 */
typedef struct
{
    rbpower_unit_id_t unit_id;         ///< Unit ID (16 bits)
    rbpower_source_type_t source_type; ///< Source type (ADC/ESP_NOW/I2C/etc)
    bool is_active;                    ///< Mapping active
    uint8_t sensor_profile_id;         ///< Sensor profile ID (0-63)

    // Source parameters (depend on type)
    union
    {
        // ADC source (local sensors)
        struct
        {
            uint8_t adc_channel;           ///< ADC channel number (0-15)
            uint8_t sensor_profile_id;     ///< Sensor profile ID in rbsensor
            float last_update_interval_ms; ///< Last update interval
        } adc_source;

        // ESP-NOW source (remote sensors)
        struct
        {
            uint8_t device_mac[6];     ///< Remote device MAC address
            uint8_t remote_channel_id; ///< Channel ID on remote device
            uint32_t last_packet_id;   ///< Last packet ID
            uint64_t last_received_us; ///< Last data received time
        } esp_now_source;

        // I2C source
        struct
        {
            uint8_t i2c_bus_num;       ///< I2C bus number (0-3)
            uint8_t i2c_address;       ///< I2C device address (0-63)
            uint8_t register_address;  ///< Register address
            uint16_t update_period_ms; ///< Update period
            uint8_t sub_device_id;     ///< Sub-device ID (for multi-channel I2C devices)
        } i2c_source;

        // Virtual/calculated source
        struct
        {
            rbpower_unit_id_t input_units[4];                     ///< Input unit IDs for calculation
            uint8_t input_count;                                  ///< Input unit count
            float (*calc_function)(float *inputs, uint8_t count); ///< Calculation function
        } virtual_source;

        // Aggregated source
        struct
        {
            rbpower_unit_id_t child_units[8]; ///< Child unit IDs
            uint8_t children_count;           ///< Child unit count
            float weight_factors[8];          ///< Weight factors
        } aggregated_source;
    } source_data;                            ///< Source parameters, selected by @c source_type

} rbpower_source_mapping_t;

//=============================================================================
// SPECIALIZED VOLTAGE BUS STRUCTURE
//=============================================================================

/* Specialized Voltage Bus structure (memory-optimized): voltage and frequency only, without
 * current/power/energy. */

/** @brief Final values of a voltage-bus 3-second period. */
typedef struct
{
    float voltage_rms_final;     ///< Final RMS for 3 sec
    float frequency_avg_final;   ///< Final average frequency
    float thd_voltage_avg_final; ///< Final average THD
} statistics_3sec_final_t;

/**
 * @brief Specialized voltage bus: voltage and frequency only, without current/power/energy.
 */
typedef struct
{
    // === IDENTIFICATION ===
    rbpower_unit_id_t unit_id; ///< Metric ID (16 bits)
    char name[32];             ///< Display name
    bool is_active;            ///< Metric activity

    // === REAL-TIME 200MS DATA ===
    struct
    {
        float voltage_rms;              ///< RMS voltage (V)
        float voltage_min;              ///< Minimum voltage (V)
        float voltage_max;              ///< Maximum voltage (V)
        float frequency;                ///< Grid frequency (Hz)
        float thd_voltage_percent;      ///< Voltage THD (%)
        uint64_t last_update_us;        ///< Last update time
        uint32_t samples_count;         ///< Sample count
        uint8_t signal_quality_percent; ///< Signal quality (0-100%)
    } realtime_200ms; ///< Realtime 200 ms data

    // === 3-SEC PERIOD STATISTICS ===

    struct
    {
        // RMS accumulators (root-mean-square averaging)
        double voltage_rms_sum_squares; ///< Sum of RMS squares
        uint32_t rms_samples_count;     ///< RMS sample count

        // 3-second extremes
        float voltage_min_3sec;       ///< Minimum for 3 sec
        float voltage_max_3sec;       ///< Maximum for 3 sec
        uint64_t voltage_min_time_us; ///< Minimum time
        uint64_t voltage_max_time_us; ///< Maximum time

        // Frequency accumulators
        double frequency_sum;             ///< Sum of frequencies
        uint32_t frequency_samples_count; ///< Frequency sample count

        // THD accumulators
        double thd_voltage_sum_squares; ///< Sum of THD squares
        uint32_t thd_samples_count;     ///< THD sample count

        // Final values after finalization
        statistics_3sec_final_t final;  ///< Final values of the last finished period
        /*
                float voltage_rms_final;            ///< Final RMS for 3 sec
                float frequency_avg_final;          ///< Final average frequency
                float thd_voltage_avg_final;        ///< Final average THD
        */
        // Time boundaries
        uint64_t period_start_us;    ///< Period start
        uint64_t period_end_us;      ///< Period end
        bool period_complete;        ///< Period complete
        bool ready_for_finalization; ///< Ready for finalization
    } statistics_3sec; ///< 3-second period accumulators and result

    // === THRESHOLD CONFIGURATION ===
    struct
    {
        float nominal_voltage_v;         ///< Nominal voltage (V)
        float under_voltage_threshold_v; ///< Under voltage threshold
        float over_voltage_threshold_v;  ///< Over voltage threshold
        float thd_warning_threshold;     ///< THD warning threshold (%)
        float thd_alarm_threshold;       ///< THD alarm threshold (%)
        float frequency_min_hz;          ///< Minimum allowable frequency
        float frequency_max_hz;          ///< Maximum allowable frequency, Hz
    } thresholds;                        ///< Alarm thresholds

    // === STATE DIAGNOSTICS ===
    struct
    {
        bool voltage_normal;             ///< Voltage normal
        bool frequency_normal;           ///< Frequency normal
        bool thd_normal;                 ///< THD normal
        uint32_t voltage_events_count;   ///< Voltage event counter
        uint32_t frequency_events_count; ///< Frequency event counter
        uint32_t quality_alerts_count;   ///< Quality event counter
        uint64_t last_event_time_us;     ///< Last event time, µs since boot
        uint32_t error_flags;            ///< Error flags (rbpower_error_flags_t)
    } diagnostics; ///< State diagnostics

    // === METADATA ===
    uint64_t created_timestamp_us; ///< Creation time
    uint32_t total_updates;        ///< Total update count
    uint64_t uptime_hours;         ///< Uptime (hours)

} rbpower_voltage_bus_t;

//=============================================================================
// MAIN HOME ELECTRICAL NETWORK STRUCTURE
//=============================================================================

/**
 * @brief Home electrical network structure (Static Units architecture)
 */
typedef struct
{
    // === LOGICAL METRICS ===
    rbpower_home_unit_t units[RBPOWER_MAX_HOME_UNITS]; ///< Static units array
    uint8_t units_count;                               ///< Active metric count

    // === SPECIALIZED STRUCTURES ===
    struct
    {
        rbpower_voltage_bus_t voltage_bus; ///< Specialized Voltage Bus structure
        bool has_voltage_bus;              ///< Voltage bus present flag
        void *main_supply;                 ///< Pointer to rbpower_main_supply_t
        bool has_main_supply;              ///< Main supply present flag
        void *alternative_supply;          ///< Pointer to rbpower_alternative_supply_t
        bool has_alternative_supply;       ///< Alternative supply present flag
    } specialized; ///< Specialized structures (voltage bus, supply structures)

    // === MANAGED DEVICES (SEPARATE) ===
    // rbhome_device_t devices[RBHOME_MAX_DEVICES];    // Managed devices //!!!
    uint8_t devices_count; ///< Device count

    // Hardware source mapping
    struct
    {
        rbpower_source_mapping_t mappings[RBPOWER_MAX_HOME_UNITS]; ///< Source to units mapping
        uint8_t active_mappings_count;                             ///< Active mapping count

        // Fast lookup by source types
        uint8_t adc_mappings_count;     ///< ADC mapping count
        uint8_t esp_now_mappings_count; ///< ESP-NOW mapping count
        uint8_t virtual_mappings_count; ///< Virtual mapping count
    } source_mappings; ///< Hardware source mappings

    // === SYSTEM METRICS ===
    struct
    {
        rbpower_unit_id_t voltage_bus_unit_id;        ///< Voltage source (Voltage) metric ID in network
        rbpower_unit_id_t main_supply_unit_id;        ///< Main source (Main Supply) ID
        rbpower_unit_id_t alternative_supply_unit_id; ///< Alternative source (inverter) ID
        float system_frequency_hz;                    ///< CURRENT system frequency
        rbpower_current_direction_t net_direction;    ///< CURRENT overall direction
        float net_power_w;                            ///< CURRENT net power

    } system; ///< System-level unit IDs and state

    // === AGGREGATED STATISTICS ===
    struct
    {
        float total_consumed_power_w;    ///< Total consumed power
        float total_supplied_power_w;    ///< Total supplied power
        float total_installed_power_w;   ///< Total installed power
        uint8_t active_units_count;      ///< Active metric count
        uint8_t problematic_units_count; ///< Problematic metrics
        uint8_t offline_devices_count;   ///< Offline devices
        float system_efficiency_percent; ///< System efficiency
    } aggregated; ///< Aggregated network statistics

    // === TIME AGGREGATION SETTINGS ===
    struct
    {
        bool enable_statistics;               ///< Time statistics enabled
        uint32_t statistics_update_mask;      ///< Enabled periods mask
        uint8_t max_statistics_memory_mb;     ///< Maximum memory for statistics
        bool auto_cleanup_old_data;           ///< Auto-cleanup old data
        uint32_t keep_daily_records_days;     ///< Days to keep daily data
        uint32_t keep_monthly_records_months; ///< Months to keep monthly data
    } statistics_config; ///< Time aggregation settings

    // === TIME SYNCHRONIZATION AND STATUS ===
    uint64_t last_update_us;     ///< Last update time
    uint64_t timestamp_ZC_event; ///< ZC update timestamp
    uint32_t update_counter;     ///< Update counter
    bool is_synchronized;        ///< Data synchronized
    bool system_healthy;         ///< System healthy
    uint32_t system_uptime_s;    ///< System uptime (seconds)

    // === CALLBACKS ===
    void (*on_threshold_exceeded)(rbpower_unit_id_t unit_id, float value, float threshold); ///< A unit value exceeded its threshold
    // void (*on_device_state_changed)(uint8_t device_id, rbhome_device_state_t new_state);//!!!
    void (*on_power_direction_changed)(rbpower_current_direction_t new_direction);          ///< Net power direction changed
    void (*on_period_completed)(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type); ///< A unit finished a period

} rbpower_electrical_network_t;

//=============================================================================
// EXTENDED CONFIGURATION WITH RBHOME
//=============================================================================

/**
 * @brief Extended rbpower configuration with home network support
 */
typedef struct
{
    // Base rbpower configuration
    rbpower_hardware_config_t *base_hardware_config; ///< Base hardware configuration

    // Home network settings
    bool enable_home_network; ///< Enable logical network
    const char *home_name;    ///< Home/object name
    const char *location;     ///< Location
    const char *language;     ///< Language

    // Additional rbhome settings
    struct
    {
        bool enable_default_thresholds;      ///< Use default thresholds
        uint32_t statistics_update_mask;     ///< Statistics enabled periods mask
        rbgrid_loging_level_t logging_level; ///< Logging level
        bool enable_logging;                 ///< Enable logging
    } rbhome_settings; ///< Additional rbhome settings

    // Telemetry and JSON
    struct
    {
        bool enable_telemetry; ///< Enable telemetry
        // rbpower_json_error_strategy_t error_strategy; //< Error handling strategy
        uint8_t max_retry_attempts; ///< Maximum retry count
        uint32_t retry_delay_ms;    ///< Delay between retries, ms
    } rbhome_telemetry_settings; ///< Telemetry settings

    // JSON
    struct
    {
        bool enable_json_api;         ///< Enable JSON API
        bool enable_JSON_compression; ///< Enable JSON compression (gzip)
        bool enable_encryption;       ///< Enable JSON encryption
        char encryption_key[32];      ///< Encryption key (if needed)
    } rbhome_JSON_settings; ///< JSON API settings

    // Units configuration (predefined units array)
    struct
    {
        const rbpower_unit_definition_t *predefined_units; ///< Predefined units array
        uint8_t predefined_units_count;                    ///< Predefined units count
        bool use_predefined_only;                          ///< Use only predefined
    } units_config; ///< Predefined units configuration

    // TIME SYSTEM. MANAGED VIA base_hardware_config->time_config
    bool enable_time_synchronization; ///< Enable time synchronization

    // Telemetry configuration
    rbpower_telemetry_config_t telemetry_config; ///< Telemetry configuration

    // Automatic measurement start
    bool auto_start_measurements; ///< Automatically start measurements after initialization (default true)

    /* Overview layout profile. Declarative — only the UI interprets it.
     * Wire form: system.site_profile = "home" | "industrial" | "ev". Zero-init = home. */
    uint8_t site_profile; ///< 0=home 1=industrial 2=ev

    /* EV-scenario hints, flat per hub (per-unit overrides live in the unit definition).
     * 0 = not set -> the UI applies its documented defaults (500 W / 30 min / tariff price).
     * Declarative only — no firmware consumer; session detection is done by the UI. */
    struct
    {
        float charge_threshold_w;  ///< power above which a charge session is assumed, W
        uint16_t idle_timeout_min; ///< session ends after this long below the threshold, min
        float price_per_kwh;       ///< station selling price (not the grid tariff), currency per kWh
    } ev_config;                   ///< EV charging hints for the UI

    /* Group names: display metadata for units[].group_id.
     * Declarative only (no tree/aggregation semantics in firmware); wire form:
     * system.groups = [{"id":N,"name":"..."}] — POST replaces the whole list. id > 0 (0 = no group). */
#define RBPOWER_MAX_GROUPS 16 ///< Maximum number of unit groups
    struct
    {
        uint16_t id;     ///< Group ID (> 0)
        char name[32];   ///< <= 31 bytes utf-8 (schema limits.name_bytes)
    } groups[RBPOWER_MAX_GROUPS]; ///< Group display names
    uint8_t groups_count;         ///< Number of valid entries in @c groups

    /* Mains topology: site-level grid system. Declarative —
     * gives units[].phase its frame of reference (valid phases per system are a CLIENT contract rule).
     * Wire form: system.grid = {"system": ""|"1P"|"SPLIT"|"3P"}; nominal_v/nominal_hz in the same GET
     * object are a read-only mirror of hardware.network (single write source stays hardware-config). */
    uint8_t grid_system; ///< 0=unset 1=1P 2=SPLIT(US 120/240) 3=3P

    // Callbacks for application-level notifications
    void (*on_home_threshold_exceeded)(const char *unit_name, float value);    ///< Threshold exceeded notification
    void (*on_home_automation_triggered)(const char *device_name, bool state); ///< Automation trigger notification

} rbpower_extended_config_t;

//=============================================================================
// HOME ELECTRICAL NETWORK DIAGNOSTICS
//=============================================================================
/**
 * @brief Units creation result (for diagnostics)
 */
typedef struct
{
    uint8_t voltage_units_created; ///< Voltage units created
    uint8_t current_units_created; ///< Current units created
    uint8_t total_units_created;   ///< Total units created
    uint8_t adc_mappings_created;  ///< ADC mappings created
    bool has_voltage_bus;          ///< Voltage Bus unit exists
    bool has_main_supply;          ///< Main Supply unit exists
    char status_message[64];       ///< Status message
} rbpower_home_creation_result_t;

/* ================================================================================================
 * API FOR 200MS DATA
 * ================================================================================================ */

/**
 * @brief Get the 200 ms averaged data of a unit by ID.
 * @param[in]  unit_id Unit ID.
 * @param[out] result  Destination for the 200 ms data.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p result is NULL or the unit is unknown.
 * @retval RBPOWER_ERR_NOT_INIT    the home network is not initialized.
 */
rbpower_err_t rbpower_get_unit_200ms_data(rbpower_unit_id_t unit_id, rbpower_unit_realtime_average_200ms_t *result);

/**
 * @brief Get the 200 ms data of the voltage unit and of all current units.
 * @param[out] voltage_unit        Voltage unit data (may be NULL).
 * @param[out] current_units       Array for the current units' data (may be NULL).
 * @param[out] current_units_count Number of current units written (may be NULL).
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT the home network is not initialized.
 */
rbpower_err_t rbpower_get_aggregated_200ms_data(rbpower_unit_realtime_average_200ms_t *voltage_unit,
                                                rbpower_unit_realtime_average_200ms_t *current_units,
                                                uint8_t *current_units_count);

/**
 * @brief Log the 200 ms data of all units (debugging).
 */
void rbpower_print_200ms_diagnostics(void);

/* ================================================================================================
 * EXTERNAL-SOURCE MEASUREMENT INJECTION API
 * ------------------------------------------------------------------------------------------------
 * These let a measurement source (rbAmp I2C module, ESP-NOW node, virtual unit, ...) feed
 * measurements into the accumulation -> statistics -> flash -> telemetry pipeline. They are
 * transport-agnostic: a source driver fills the measurement struct for a unit and calls these by
 * the unit's real unit_id. All sources share the same apply logic, so accumulation, period
 * rollover and tariffs behave identically for every source.
 *
 * Cadence is the caller's responsibility:
 *   - inject_halfcycle: per AC half-cycle (~100/120 Hz) — feeds the 3-sec accumulator.
 *   - inject_200ms:     per 200 ms moving-average window — drives the 200ms accumulation control.
 * Set the measurement fields you have; unit_id inside the struct is ignored (the unit_id argument
 * is authoritative).
 * ================================================================================================ */

/**
 * @brief Inject one unit's half-cycle measurement from an external source.
 * @param[in] unit_id Real unit ID (as registered in the home network).
 * @param[in] data    Filled half-cycle measurement; its embedded unit_id field is ignored.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p data is NULL or the unit is unknown.
 * @retval RBPOWER_ERR_NOT_INIT    the home network is not initialized.
 */
rbpower_err_t rbpower_home_inject_halfcycle(rbpower_unit_id_t unit_id,
                                            const rbpower_unit_realtime_halfcycle_t *data);

/**
 * @brief Inject one unit's 200 ms averaged measurement from an external source.
 *
 * Takes the home lock internally.
 *
 * @param[in] unit_id Real unit ID (as registered in the home network).
 * @param[in] data    Filled 200 ms measurement; its embedded unit_id field is ignored.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p data is NULL or the unit is unknown.
 * @retval RBPOWER_ERR_NOT_INIT    the home network is not initialized.
 */
rbpower_err_t rbpower_home_inject_200ms(rbpower_unit_id_t unit_id,
                                        const rbpower_unit_realtime_average_200ms_t *data);

/** @brief Set the system grid frequency from an external source (rbAmp). The telemetry `freq`
 *  prefers this value. (The 200 ms inject struct has no frequency field, hence a setter.)
 *  @param[in] hz Grid frequency, Hz; ignored if <= 0. */
void rbpower_home_set_system_frequency(float hz);

/**
 * @brief One-shot minimum power for the next period result of a unit.
 * @param[in] unit_id Unit ID.
 * @param[in] p_min_w Minimum active power of the period, W (NAN = use the period average).
 */
void rbpower_home_set_next_power_min(rbpower_unit_id_t unit_id, float p_min_w);   /* one-shot p_min for the next period result (NAN = period avg) */
/**
 * @brief One-shot window END of the next 15MIN result of a unit.
 *
 * The quarter boundary the result was collected for: the quarter lands in the hour of that
 * boundary (not of the collect moment) and the hour is closed right after it.
 *
 * @param[in] unit_id Unit ID.
 * @param[in] end_ts  Window end, epoch s; 0 = unknown (on-demand collect, flush): attribution by collect time.
 */
void rbpower_home_set_next_period_end(rbpower_unit_id_t unit_id, uint32_t end_ts);
/**
 * @brief Whether a unit is fed by finished external period results.
 *
 * True for units whose energy mode is ON_DEVICE or HOST (rbAmp/ESP-NOW); their native
 * 3sec->15MIN chain is skipped, otherwise a zero native quarter could close the hour before the
 * injected one arrives.
 *
 * @param[in] unit_index Index in units[].
 * @return true if the unit's periods are injected.
 */
bool rbpower_home_unit_is_period_injected(uint8_t unit_index);
/**
 * @brief Inject a FINISHED period result from a source that accumulates on-device
 *        (e.g. rbAmp via latch_period/read_period_snapshot).
 *
 * Source-aware path: writes the period slot directly (avg power, peak, energy), BYPASSING the
 * 200 ms sum-of-squares accumulator. Used for rbAmp at its latch period (15 min); higher periods
 * aggregate from the 15-min results through the normal hierarchy. Takes the home lock.
 *
 * @param[in] unit_id       Real unit ID (as registered in the home network).
 * @param[in] period        Target period slot (e.g. RBPOWER_STAT_PERIOD_15MIN).
 * @param[in] avg_power_w   Average active power over the window, W.
 * @param[in] max_power_w   Peak active power over the window, W.
 * @param[in] energy_wh     Energy over the window, Wh (signed: + consumed, - supplied).
 * @param[in] period_ms     Actual wall-clock window length, ms (rbAmp master_dt_ms).
 * @param[in] voltage_v     Representative period voltage, V (0 = unknown).
 * @param[in] current_rms_a Representative period current, A (0 = unknown).
 * @param[in] current_min_a Period current minimum, A (history envelope).
 * @param[in] current_max_a Period current maximum, A (history envelope).
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG bad period or unknown unit.
 * @retval RBPOWER_ERR_NOT_INIT    the home network is not initialized.
 */
rbpower_err_t rbpower_home_inject_period_result(rbpower_unit_id_t unit_id,
                                                rbpower_stat_period_type_t period,
                                                float avg_power_w,
                                                float max_power_w,
                                                float energy_wh,
                                                uint32_t period_ms,
                                                float voltage_v,       /* representative period voltage (v_bus); 0=unknown */
                                                float current_rms_a,   /* representative period current; 0=unknown */
                                                float current_min_a,   /* period current minimum (history envelope) */
                                                float current_max_a);  /* period current maximum (history envelope) */

/**
 * @brief Feed bookkeeping of the et_in/et_out totalizer inject path (etstat diagnostics).
 * @param[in]  unit_index   Index in units[].
 * @param[out] feeds        Number of feeds since boot.
 * @param[out] last_feed_ts Time of the last feed, epoch s.
 * @param[out] seed_in_mwh  Import totalizer before the first post-boot feed, mWh.
 * @param[out] seed_out_mwh Export totalizer before the first post-boot feed, mWh.
 * @return false if @p unit_index is out of range.
 */
bool rbpower_home_get_et_diag(uint8_t unit_index, uint32_t* feeds, time_t* last_feed_ts,
                              int64_t* seed_in_mwh, int64_t* seed_out_mwh);

/**
 * @brief List the IDs of valid units (for C++ callers that cannot see g_home_network).
 * @param[out] ids Destination array.
 * @param[in]  cap Capacity of @p ids.
 * @return Number of IDs written.
 */
uint8_t rbpower_home_list_unit_ids(uint16_t* ids, uint8_t cap);

/* Period-bucket indices + the pure bucket calculator. DAY/WEEK/MONTH/YEAR buckets are
 * LOCAL-calendar based (TZ POSIX incl. DST): day flips at local midnight, week is Monday-aligned
 * local, month/year from the local calendar. HOUR stays the UTC hour (equal to the local hour for
 * whole-hour TZs; :30/:45 TZs documented). Pure function of (now, localtime(now)) — unit-tested
 * across 5 time zones in rbpower_logic_test.c. */
/** @brief Calendar bucket index. */
typedef enum {
    RBSTATS_BK_HOUR = 0,  ///< UTC hour
    RBSTATS_BK_DAY,       ///< Local day
    RBSTATS_BK_WEEK,      ///< Local week (Monday-aligned)
    RBSTATS_BK_MONTH,     ///< Local month
    RBSTATS_BK_YEAR,      ///< Local year
    RBSTATS_BK_COUNT      ///< Number of bucket kinds
} rbstats_bucket_idx_t;
/**
 * @brief The statistics clock: wall time, or the uptime-anchored virtual clock in accelerated builds.
 *
 * Bucket dispatch, record timestamps and child-bucket filtering all share this timebase.
 *
 * @return Current statistics time, epoch s.
 */
time_t rbpower_stats_clock_now(void);

/**
 * @brief Exact epoch span [start, end) of a calendar bucket under the CURRENT time zone.
 *
 * Reference for PARTIAL detection and expected children (DST days are 23/25 h, months 28-31 d).
 *
 * @param[in]  idx       Bucket kind.
 * @param[in]  bucket    Bucket number (as produced by rbpower_stats_calc_buckets()).
 * @param[out] start_out Span start, epoch s.
 * @param[out] end_out   Span end (exclusive), epoch s.
 * @return true if a valid span was computed.
 */
bool rbpower_stats_bucket_span(rbstats_bucket_idx_t idx, uint64_t bucket,
                               int64_t* start_out, int64_t* end_out);

/**
 * @brief Compute the calendar bucket numbers of a moment (pure function).
 * @param[in]  now_epoch Moment, epoch s.
 * @param[in]  local     localtime() of @p now_epoch.
 * @param[out] bk_out    Bucket number per rbstats_bucket_idx_t.
 */
void rbpower_stats_calc_buckets(int64_t now_epoch, const struct tm* local,
                                uint64_t bk_out[RBSTATS_BK_COUNT]);

/**
 * @brief 3-second tick: baseline the calendar buckets once the wall clock is valid and arm DAY+
 *        closes for buckets that ended while the hub was down (lifetime snapshot = last-alive mark).
 */
void rbpower_stats_boot_clock_tick(void);
/**
 * @brief Whether the wall clock can be used for calendar/lifetime decisions.
 * @param[in] now Time to check, epoch s.
 * @return true when the clock is synced (or fallback) AND @p now is not before the build date.
 */
bool rbpower_stats_clock_trusted(time_t now);

/**
 * @brief Flow-conservation diagnostics of the quarter -> HOUR path (statflow diagnostic verb).
 *
 * Asserts that no finished period result is silently dropped between inject and the stored HOUR record.
 *
 * @param[in]  unit_id       Unit ID.
 * @param[out] injected      Quarters injected (may be NULL).
 * @param[out] agg15         Quarters aggregated into an hour (may be NULL).
 * @param[out] hour_fin      Hours finalized (may be NULL).
 * @param[out] hour_quarters Quarters contained in stored hours (may be NULL).
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG the unit is unknown.
 */
rbpower_err_t rbpower_statflow_get(rbpower_unit_id_t unit_id, uint32_t* injected,
                                   uint32_t* agg15, uint32_t* hour_fin, uint32_t* hour_quarters);
/**
 * @brief Internal: statistics-side flow counters of a unit (by INDEX, not ID).
 * @param[in]  unit_index    Index in units[].
 * @param[out] agg15         Quarters aggregated into an hour (may be NULL).
 * @param[out] hour_fin      Hours finalized (may be NULL).
 * @param[out] hour_quarters Quarters contained in stored hours (may be NULL).
 */
void rbpower_statflow_read_by_index(uint8_t unit_index, uint32_t* agg15,
                                    uint32_t* hour_fin, uint32_t* hour_quarters);

/**
 * @brief Persist a FINISHED period from an external source directly to the flash history.
 *
 * For a source that latched the period on-device (e.g. an ESP-NOW node) and is not wired into
 * the accumulator: the finished avg/max/Wh is written straight to the flash ring.
 *
 * @param[in] unit_id       Unit ID.
 * @param[in] period        Period type.
 * @param[in] avg_power_w   Average active power, W.
 * @param[in] max_power_w   Peak active power, W.
 * @param[in] energy_wh     Energy, Wh (signed: + consumed, - supplied).
 * @param[in] period_ms     Window length, ms.
 * @param[in] voltage_v     Representative voltage, V.
 * @param[in] current_a     Representative current, A.
 * @param[in] current_min_a Current minimum, A.
 * @param[in] current_max_a Current maximum, A.
 */
void rbpower_home_persist_external_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period,
                                          float avg_power_w, float max_power_w, float energy_wh,
                                          uint32_t period_ms, float voltage_v, float current_a,
                                          float current_min_a, float current_max_a);

/* Per-unit energy-source provenance for telemetry. An external source (the rbAmp bridge)
 * registers, at probe, HOW each injected unit's energy is computed. The telemetry serializer emits
 * this as `energy_mode` on i2c units so the frontend knows whether to trust period-Wh as
 * device-computed. Tiny static map. */
#define RBPOWER_ENERGY_MODE_NONE      0   ///< Energy not provided
#define RBPOWER_ENERGY_MODE_ON_DEVICE 1   ///< Module integrates Wh on its own wall clock
#define RBPOWER_ENERGY_MODE_HOST      2   ///< RBgrid integrates P = V_bus x I
/**
 * @brief Register the energy mode of a unit (silently dropped when the table is full).
 * @param[in] unit_id Unit ID.
 * @param[in] mode    RBPOWER_ENERGY_MODE_*.
 */
void    rbpower_set_unit_energy_mode(rbpower_unit_id_t unit_id, uint8_t mode);
/**
 * @brief Energy mode of a unit.
 * @param[in] unit_id Unit ID.
 * @return RBPOWER_ENERGY_MODE_*, or 0xFF if not set.
 */
uint8_t rbpower_get_unit_energy_mode(rbpower_unit_id_t unit_id);

/**
 * @brief Hardware-presence gating.
 *
 * A detection layer (rbAmp bridge probe, ESP-NOW link) marks whether a unit's bound sensor is
 * physically present. present=false makes the compute gate skip the unit, while its definition
 * stays in the configuration (dormant). Defaults to true at unit creation.
 *
 * @param[in] unit_id Unit ID.
 * @param[in] present Sensor present.
 */
void rbpower_set_unit_hardware_present(rbpower_unit_id_t unit_id, bool present);

/* ---- Metering MODULE descriptor — what the bridge learned at probe ----
 * Published in /api/v1/sensors as `modules[]` so the UI can describe a module (model, channels,
 * calibration state) without decoding anything. Filled by main/rbamp_bridge.c (rbamp_std_info) on
 * every probe/join; cleared on module loss. Pure data, no bus traffic on read. */
/** @brief Descriptor of one rbAmp metering module. */
typedef struct {
    bool     present;      /**< module answered on the bus */
    uint8_t  addr;         /**< 7-bit I2C address */
    uint8_t  line;         /**< 0 = BASIC, 1 = STD */
    uint8_t  variant;      /**< raw VARIANT byte (STD: 0x11 UI1, 0x63 U3I3 …; BASIC: lib enum) */
    uint8_t  n_v;          /**< voltage channels */
    uint8_t  n_i;          /**< current channels */
    uint8_t  map;          /**< STD register-map version (0 = BASIC) */
    uint8_t  period_src;   /**< 0 none · 1 latch · 2 counters (STD period/energy mechanism) */
    int8_t   cal;          /**< 1 = cal_ok, 0 = not asserted, -1 = unknown (BASIC / not read) */
    uint32_t fw;           /**< firmware version word (STD FW_VER u32; BASIC fw byte) */
    uint32_t ota_bank_size;/**< STD module firmware update: bank size in bytes (0 = no OTA capability) */
    uint32_t active_bank;  /**< STD: running firmware bank base address (the update goes to the OTHER bank) */
    uint32_t inactive_bank;/**< STD: base of the other bank = the one an update image must be built for (0 = unknown) */
    uint32_t board_id;     /**< STD map 0x36+: board word of the running image (an update image must carry the same; 0 = unknown) */
    uint8_t  sensor_class; /**< REG_SENSOR_CLASS: 0 unset, 1 SCT-013 (user-chosen CT, generic calibration), 2/3 CT- series
                              (CT shipped with the module, channel calibrated in pair — the hub never rewrites it); 0xFF unknown */
    char     model[16];    /**< "STD-U3I3", "STD-UI1", "BASIC-UI1" … */
} rbpower_module_desc_t;
#define RBPOWER_MODULE_DESC_MAX 8 ///< Maximum number of module descriptors
/**
 * @brief Set or clear the descriptor of a module (bumps the hardware revision on change).
 * @param[in] module Module index (< RBPOWER_MODULE_DESC_MAX; others are ignored).
 * @param[in] d      Descriptor, or NULL to clear.
 */
void rbpower_set_module_desc(uint8_t module, const rbpower_module_desc_t* d);   /* NULL = clear */
/**
 * @brief Read the descriptor of a module.
 * @param[in]  module Module index.
 * @param[out] out    Destination.
 * @return true if a module is (or was) known at this index; false for a bad index, NULL @p out or an empty slot.
 */
bool rbpower_get_module_desc(uint8_t module, rbpower_module_desc_t* out);
/** @brief Hardware/config revision (telemetry `hw_rev`, with `boot_id`).
 *
 *  Bumped when a module joins/is replaced/disappears, a point's presence flips, or the fleet
 *  config (/rbamp.json: CT model, ct_user, binding) or the units config is saved. Clients re-read
 *  /api/v1/sensors when it changes. Starts at 1 each boot.
 *  @return Current revision. */
uint32_t rbpower_hw_rev(void);
/** @brief SCT-013 rating correction per module channel.
 *
 *  A BASIC module with firmware <= 1.4.19 keeps its factory (030) calibration after another SCT
 *  model is bound; the rbAmp library then scales the readings (rbamp-esp-idf 1.7.2+). Set by the
 *  bridge from the library's cache; read by the sensors list.
 *  @param[in] module Module index.
 *  @param[in] ch     Channel index.
 *  @param[in] status rbamp_sct_status_t (0 none · 1 corrected · 2 uncalibrated · 3 gain unexpected ·
 *                    4 unknown model · 5 pending), 0xFF = unknown.
 *  @param[in] scale  Applied scale factor. */
void rbpower_set_channel_sct(uint8_t module, uint8_t ch, uint8_t status, float scale);
/**
 * @brief Read the SCT correction of a module channel.
 * @param[in]  module Module index.
 * @param[in]  ch     Channel index.
 * @param[out] status rbamp_sct_status_t, 0xFF = unknown.
 * @param[out] scale  Applied scale factor.
 * @return false if the module/channel index is out of range.
 */
bool rbpower_get_channel_sct(uint8_t module, uint8_t ch, uint8_t *status, float *scale);
/** @brief Increment the hardware/config revision (see rbpower_hw_rev()). */
void     rbpower_hw_rev_bump(void);
/** @brief System voltage-bus unit ID; the bridge routes module mains voltage into it.
 *  @return Voltage-bus unit ID, 0 = none. */
rbpower_unit_id_t rbpower_home_voltage_bus_unit_id(void);
/** @brief Set the rbAmp module feeding the voltage bus (read by telemetry for the v_bus source label).
 *  @param[in] module Module index, 0xFF = none. */
void    rbpower_home_set_voltage_bus_feed(uint8_t module);
/** @brief rbAmp module feeding the voltage bus.
 *  @return Module index, 0xFF = none. */
uint8_t rbpower_home_get_voltage_bus_feed(void);
/**
 * @brief Record which rbAmp fleet module/source actually feeds a unit.
 *
 * Needed for units whose DEFINITION is still a legacy ADC-era one (e.g. the default main unit
 * 0x0006 bound to a U3I3 PHASES entry via /rbamp.json): telemetry and /api/v1/sensors then label
 * the unit with its REAL source (i2c/rbamp, module, ch) instead of "adc"/"native_adc", so the web
 * app's fields_by_source gating sees the right class. Set by the bridge on every presence publish;
 * cleared on unbind.
 *
 * @param[in] unit_id Unit ID.
 * @param[in] module  Fleet module index.
 * @param[in] source  Fleet entry source: current channel 0..2, 0xFF = module voltage,
 *                    0xFE = all current channels grouped (PHASES).
 */
void rbpower_set_unit_rbamp_feed(rbpower_unit_id_t unit_id, uint8_t module, uint8_t source);
/**
 * @brief Set the CT load of the channel feeding a unit (BASIC/STD rbAmp).
 * @param[in] unit_id  Unit ID.
 * @param[in] load_pct I_rms / CT nominal x 100, %; < 0 = unknown.
 * @param[in] state    0 ok, 1 near the limit (I_rms >= 0.9 x full scale), 2 clipping (I_rms >= full
 *                     scale, or I_peak >= 0.97 x peak in >= 3 of the last 5 polls).
 */
void rbpower_set_unit_ct_load(rbpower_unit_id_t unit_id, float load_pct, uint8_t state);
/**
 * @brief Name the statistics task's current stage (reported in the boot line after a task-watchdog reset).
 * @param[in] stage Static string (NULL = empty).
 */
void rbpower_stats_stage(const char* stage);
/**
 * @brief CT load of the channel feeding a unit.
 * @param[in]  unit_id  Unit ID.
 * @param[out] load_pct Load, % (< 0 = unknown).
 * @param[out] state    0 ok, 1 near the limit, 2 clipping.
 * @return true if a value is known for the unit.
 */
bool rbpower_get_unit_ct_load(rbpower_unit_id_t unit_id, float *load_pct, uint8_t *state);
/**
 * @brief rbAmp fleet module/source feeding a unit.
 * @param[in]  unit_id Unit ID.
 * @param[out] module  Fleet module index.
 * @param[out] source  Fleet entry source (see rbpower_set_unit_rbamp_feed()).
 * @return false when the unit is not fed by the bridge.
 */
bool rbpower_get_unit_rbamp_feed(rbpower_unit_id_t unit_id, uint8_t* module, uint8_t* source);

/* ---- Diagnostics trace (catch errors DURING accumulation, not hours later) ----
 * rbpower_diag_printf() formats one line and hands it to the registered sink (the diagnostic
 * console prints it as "#DIAG <line>"); without a sink, or disabled, it is a no-op. Lines that start
 * with "FAIL " are invariant violations — a long run can be aborted on the first one. period_s =
 * cadence of the periodic reconciliation lines (module counters vs hub-integrated power); 0 = events only. */
/** @brief Diagnostics line sink. @param[in] line One formatted line (no newline). */
typedef void (*rbpower_diag_sink_t)(const char* line);
/** @brief Set the console sink of the diagnostics trace. @param[in] sink Sink, or NULL. */
void     rbpower_diag_set_sink(rbpower_diag_sink_t sink);
/**
 * @brief Set the second sink, for the on-hub event log.
 *
 * Receives EVERY event line even with the console trace off; while it is set
 * rbpower_diag_enabled() is true. Periodic lines (period_s) are produced only while the console
 * trace is on.
 *
 * @param[in] sink Sink, or NULL.
 */
void     rbpower_diag_set_log_sink(rbpower_diag_sink_t sink);
/**
 * @brief Enable the console trace and set the periodic cadence.
 * @param[in] enabled  Console trace on/off.
 * @param[in] period_s Periodic reconciliation cadence, s (0 = events only).
 */
void     rbpower_diag_set(bool enabled, uint32_t period_s);
/** @brief Whether any diagnostics listener is active (console trace or log sink). @return true if active. */
bool     rbpower_diag_enabled(void);
/** @brief Effective periodic cadence. @return Cadence, s (900 with only the log sink; 0 = events only). */
uint32_t rbpower_diag_period_s(void);
/** @brief Configured periodic cadence, even while the console trace is off. @return Cadence, s. */
uint32_t rbpower_diag_period_cfg_s(void);   /* configured cadence, even while the console trace is off */
/** @brief Whether the console trace (`diag on`) is active. @return true if on. */
bool     rbpower_diag_console_on(void);     /* `diag on` (console sink) — vs rbpower_diag_enabled() = any listener */
/**
 * @brief Format one diagnostics line and pass it to the sinks (no-op without a listener).
 * @param[in] fmt printf-style format, followed by its arguments.
 */
void     rbpower_diag_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
/** @brief Whether an external source refreshed the system frequency in the last 10 s.
 *  @return true if fresh. */
bool     rbpower_home_system_frequency_fresh(void);

/* ---- Boot forensics: the console is CONSOLE_NONE, so a panic leaves no trace. A marker
 * in RTC memory records a PLANNED restart (every deliberate esp_restart() sets it) and the last
 * heartbeat uptime; at boot rbpower_boot_check() classifies the reset (power-on / planned / panic /
 * watchdog / unexplained software reset) and reports it — "FAIL " prefixed when unexpected. */
/** @brief Mark the next restart as planned (call right before every deliberate esp_restart()); runs the pre-restart hook once. */
void rbpower_mark_planned_restart(void);
/** @brief Update the heartbeat uptime in the RTC marker (cheap; called from the 200 ms inject path). */
void rbpower_boot_heartbeat(void);                     /* cheap; called from the 200 ms inject path */
/** @brief Rate-limited (2 s) periodic housekeeping (heartbeat, day rollover, NVS flush); callable from any task. */
void rbpower_home_housekeeping_tick(void);            /* rate-limited (2 s) periodic housekeeping; any task */
/**
 * @brief Task-watchdog ISR hook: record the running task per core.
 * @param[in] t0 Task name on core 0.
 * @param[in] t1 Task name on core 1.
 */
void rbpower_boot_note_wdt(const char* t0, const char* t1); /* TWDT ISR hook: running task per core */
/** @brief Task-watchdog ISR hook: record the starved-entry list (IDF message callback). @param[in] msg Message. */
void rbpower_boot_note_wdt_msg(const char* msg);          /* TWDT ISR hook: starved-entry list (IDF msg callback) */
/** @brief Hook run once inside rbpower_mark_planned_restart(). @param[in] fn Hook, or NULL. */
void rbpower_set_pre_restart_hook(void (*fn)(void));  /* run once inside rbpower_mark_planned_restart() */
/**
 * @brief Classify the last reset and format a one-line report.
 * @param[out] out Destination buffer.
 * @param[in]  cap Size of @p out, bytes.
 * @return Length of the line (0 if @p out is NULL or @p cap is 0).
 */
int  rbpower_boot_check(char* out, size_t cap);        /* one line; returns its length */
/**
 * @brief Boot information for telemetry (system.boot).
 * @param[out] boot_ts    Boot time, epoch s (may be NULL).
 * @param[out] reason     Reset reason string (may be NULL).
 * @param[out] planned    true if the restart was planned (may be NULL).
 * @param[out] downtime_s Downtime before this boot, s (may be NULL).
 */
void rbpower_boot_info(uint32_t* boot_ts, const char** reason, bool* planned, uint32_t* downtime_s); /* telemetry system.boot */
/**
 * @brief Age of the last live (200 ms) feed of a unit.
 * @param[in] unit_id Unit ID.
 * @return Age, ms; -1 = never fed.
 */
int32_t rbpower_home_unit_age_ms(rbpower_unit_id_t unit_id);   /* ms since the last live (200 ms) feed; -1 = never */
/**
 * @brief Hardware-presence flag of a unit (see rbpower_set_unit_hardware_present()).
 * @param[in] unit_id Unit ID.
 * @return Presence flag; false for an unknown unit.
 */
bool rbpower_get_unit_hardware_present(rbpower_unit_id_t unit_id);

/**
 * @brief Remove a unit at runtime.
 *
 * Stops compute, drops the unit from the ID->index map, disables its accumulators and reclaims and
 * ERASES its flash STATS slot. Leaves a hole in the array (rebuilt clean on restart). The caller
 * persists the updated units.json.
 *
 * @param[in] unit_id Unit ID.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT      the home network is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG   the unit is unknown.
 * @retval RBPOWER_ERR_INVALID_STATE the unit still has children (delete them first).
 */
rbpower_err_t rbpower_home_remove_unit(rbpower_unit_id_t unit_id);

/**
 * @brief Force-finalize an accumulated period of a unit now (test/diagnostic trigger).
 *
 * Persists e.g. a HOUR record without waiting for the wall-clock boundary, even below the
 * automatic input threshold.
 *
 * @param[in] unit_id Real unit ID.
 * @param[in] period  Period to finalize.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT      the home network is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG   bad period or unknown unit.
 * @retval RBPOWER_ERR_INVALID_STATE no inputs accumulated — nothing to store.
 */
rbpower_err_t rbpower_force_finalize_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period);

/**
 * @brief Flush every unit's OPEN HOUR accumulator to flash as a PARTIAL record.
 *
 * Planned-restart path (reboot API, MQTT command, OTA). The open hour lives only in RAM, so a reboot
 * would otherwise drop its already-aggregated quarters, and a boot that contributes fewer than 3
 * quarters before the next boundary would produce no record at all. The flushed record is
 * q=PARTIAL (dur < nominal); the post-reboot remainder of the same hour closes as its own partial
 * record, and the DAY+ recompute sums both children, so day totals stay exact.
 *
 * @return Number of units flushed.
 */
int rbpower_stats_flush_open_hours(void);

/**
 * @brief Forget the per-unit open-HOUR NVS image and pending quality bits of units[unit_idx].
 *
 * Used on unit removal, so a unit later re-created with the same ID starts with an empty open hour.
 * Call with the home lock held.
 *
 * @param[in] unit_idx Index in units[].
 */
void rbpower_stats_forget_unit(uint8_t unit_idx);

/* ---- Per-phase LIVE data of a phase-grouped (multi-channel) unit ----
 * Telemetry carries "ph":[{"ph":<1..3>,"v","i","p","q","pf"}, ...] with the EXPLICIT phase number
 * per entry (supports 2-of-3) and unknown fields omitted. Top-level unit v/i/p stay the sum/mean
 * forms. Source: the three-phase module poll. NAN in a float field = unknown -> omitted from JSON. */
/** @brief Live values of one phase of a phase-grouped unit. */
typedef struct {
    uint8_t ph;      /**< 1..3 = L1..L3 */
    float   v;       /**< V L-N */
    float   i;       /**< A */
    float   p;       /**< W, signed */
    float   q;       /**< var, signed */
    float   pf;      /**< power factor */
    float   angle;   /**< voltage phase angle vs L1, degrees (L1 = 0; forward: L2 ≈ −120, L3 ≈ +120); NAN = unknown (STD 0x7A+) */
    uint8_t seq;     /**< phase sequence of the whole group: 0 n/a · 1 forward · 2 reverse · 3 abnormal (same in every entry) */
} rbpower_phase_live_t;

/**
 * @brief Upsert the per-phase live snapshot of a unit.
 *
 * Pool-backed (sparse — only multi-channel units have this); evicts the stalest entry when the
 * pool is full. Takes the home lock.
 *
 * @param[in] unit_id Unit ID (non-zero).
 * @param[in] entries Phase entries.
 * @param[in] count   Number of entries, 1..3 (0 = clear).
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG unit_id 0, count > 3, or NULL @p entries with count > 0.
 */
rbpower_err_t rbpower_home_set_phase_live(rbpower_unit_id_t unit_id,
                                          const rbpower_phase_live_t* entries, uint8_t count);

/**
 * @brief Read back the per-phase live snapshot of a unit.
 *
 * Emitters should drop stale data (age beyond a few telemetry periods).
 *
 * @param[in]  unit_id Unit ID.
 * @param[out] out     Entries 0..count-1.
 * @param[out] age_us  Snapshot age, µs.
 * @return Entry count (0 = none).
 */
uint8_t rbpower_home_get_phase_live(rbpower_unit_id_t unit_id,
                                    rbpower_phase_live_t out[3], int64_t* age_us);

/* ---- Grid (mains) voltage per phase ----
 * Voltage is a property of the GRID, measured per phase by the metering modules (three-phase
 * module = L1..L3, single-phase/BASIC = one phase), not of the legacy pseudo-unit 0x0007
 * "voltage bus" fed by injects. Fed by the rbAmp bridge on every poll; consumed by telemetry
 * (v_bus + ph[]), by host-side P = V x I of current-only channels (by the unit's phase) and by HA.
 * Entries go stale after RBPOWER_GRID_STALE_US (module lost) and are then omitted. */
/** @brief Grid voltage/frequency of one phase. */
typedef struct {
    uint8_t ph;          /**< 1..3 = L1..L3 */
    float   v;           /**< V L-N (NAN = unknown) */
    float   f;           /**< Hz (NAN = the source reports no frequency) */
    uint8_t src_module;  /**< feeding rbAmp module index (0xFF = none) */
    uint8_t src_vch;     /**< module voltage channel index */
    int64_t ts_us;       /**< last feed (esp_timer) */
} rbpower_grid_phase_t;
#define RBPOWER_GRID_STALE_US (10LL * 1000000LL) ///< Grid phase entry staleness limit, µs (10 s)
/**
 * @brief Store the grid voltage/frequency of one phase (takes the home lock).
 * @param[in] ph     Phase 1..3.
 * @param[in] v      Voltage L-N, V (NAN = unknown).
 * @param[in] f      Frequency, Hz (NAN = not reported).
 * @param[in] module Feeding rbAmp module index.
 * @param[in] vch    Module voltage channel index.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p ph out of 1..3.
 */
rbpower_err_t rbpower_home_set_grid_phase(uint8_t ph, float v, float f, uint8_t module, uint8_t vch);
/**
 * @brief Fresh grid phases.
 * @param[out] out Destination (may be NULL to just count).
 * @return Number of fresh phases (0 = no module measures voltage).
 */
uint8_t rbpower_home_get_grid(rbpower_grid_phase_t out[3]);
/**
 * @brief Voltage of a grid phase.
 * @param[in] ph Phase 1..3; 0 (or a phase nobody feeds) = mean of the fresh phases.
 * @return Voltage, V; NAN when no phase is fresh.
 */
float rbpower_home_grid_voltage(uint8_t ph);

/**
 * @brief Lock the recursive mutex guarding g_home_network for cross-task access.
 *
 * g_home_network is mutated by the update path, the esp_timer 3-sec period task and the rbAmp
 * inject path, and read by the HTTP JSON builders on the webserver task. Any code reading or
 * writing g_home_network from a task OTHER than the one performing the mutation (notably the
 * HTTP handlers and the JSON builders) must wrap its access in lock/unlock.
 * The mutex is RECURSIVE: a locked write transaction may call a self-locking builder safely.
 * No-op before rbpower_home_init() creates the mutex (no other task runs that early).
 */
void rbpower_home_lock(void);
/**
 * @brief Try to take the home lock with a timeout (for watchdog-supervised callers).
 * @param[in] timeout_ms Timeout, ms.
 * @return true if the lock was taken (always true before the mutex exists), false otherwise.
 */
bool rbpower_home_lock_timeout(uint32_t timeout_ms);   /* false = not taken (watchdog-supervised callers) */
/** @brief Release the home lock taken by rbpower_home_lock() / rbpower_home_lock_timeout(). */
void rbpower_home_unlock(void);
/** @brief Number of active metering units (excluding the voltage bus). @return Unit count. */
uint8_t rbpower_home_active_unit_count(void);   /* count of active metering units (excl. v-bus) */

/**
 * @brief Period-boundary callback.
 *
 * Invoked from the statistics task whenever a period longer than 3 s (i.e. the 15-min boundary)
 * closes, so an external source that owns its own period (e.g. rbAmp) can collect/inject its
 * finished period AT THE SAME boundary, staying synchronized with the native hierarchy. Runs in the
 * statistics task context; keep the handler short (it shares that task's watchdog window).
 *
 * @param[in] period       Period that closed.
 * @param[in] timestamp_us Boundary time, µs.
 */
typedef void (*rbpower_period_boundary_cb_t)(rbpower_stat_period_type_t period,
                                             uint64_t timestamp_us);
/** @brief Register the period-boundary callback. @param[in] cb Handler, or NULL to clear. */
void rbpower_register_period_boundary_cb(rbpower_period_boundary_cb_t cb);
/**
 * @brief Wall-clock quarter boundary nearest to now, for the boundary callback.
 * @return Boundary, epoch s (multiple of 900); 0 when the clock is not trusted or in accelerated-clock builds.
 */
uint32_t rbpower_stats_quarter_boundary_now(void);

/* External-source unbind hook. When a unit fed by an external source (rbAmp/I2C) is removed, the
 * source layer must clear its binding so the unit does NOT respawn on reboot (e.g. the rbAmp
 * bridge sets the fleet entry's unit_id=0 and persists /rbamp.json). rbpower_home_remove_unit
 * invokes this for I2C units. Cross-component hook (the bridge lives in main/), like the period cb. */
/** @brief Unbind hook. @param[in] unit_id Removed unit ID. */
typedef void (*rbpower_unit_unbind_cb_t)(rbpower_unit_id_t unit_id);
/** @brief Register the unbind hook. @param[in] cb Hook, or NULL. */
void rbpower_register_unit_unbind_cb(rbpower_unit_unbind_cb_t cb);
/* External-source BIND hook (symmetric to unbind). When a unit is created at runtime for an
 * external-source sensor (the bind API), the source layer must set its binding so the source starts
 * injecting and the unit survives reboot (the rbAmp bridge sets the fleet entry's unit_id and
 * persists /rbamp.json). rbpower_home_create_unit invokes this. No-op for sources with no binding.
 * The hook receives the full definition, not just the ID — with opaque unit IDs the source layer
 * cannot recover module/channel from ID bits; it reads device_uid/channel_idx. */
/** @brief Bind hook. @param[in] def Definition of the created unit. */
typedef void (*rbpower_unit_bind_cb_t)(const rbpower_unit_definition_t* def);
/** @brief Register the bind hook. @param[in] cb Hook, or NULL. */
void rbpower_register_unit_bind_cb(rbpower_unit_bind_cb_t cb);
/**
 * @brief Feed-rebind hook: move a bridge-FED unit onto another fleet channel.
 *
 * For a legacy definition fed through a fleet entry (e.g. the main input 0x0006): the source layer
 * re-points its entry; ID/point/history stay. The input can sit on any channel (users may wire
 * channels in any order).
 *
 * @param[in] unit_id Unit ID.
 * @param[in] module  Target module index.
 * @param[in] ch      Target channel.
 * @return 0 ok, -1 not fed / no such channel, -2 busy.
 */
typedef int (*rbpower_unit_feed_rebind_cb_t)(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch);
/** @brief Register the feed-rebind hook. @param[in] cb Hook, or NULL. */
void rbpower_register_unit_feed_rebind_cb(rbpower_unit_feed_rebind_cb_t cb);
/**
 * @brief Move a bridge-fed unit onto another fleet channel (calls the feed-rebind hook).
 * @param[in] unit_id Unit ID.
 * @param[in] module  Target module index.
 * @param[in] ch      Target channel.
 * @return 0 ok, -1 not fed / no such channel / no hook registered, -2 busy.
 */
int  rbpower_unit_feed_rebind(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch);
/**
 * @brief CT-model hook: set the CT model of the channel a unit is bound to (rbAmp fleet entry).
 *
 * Set live on the module and persisted in the module and in /rbamp.json.
 *
 * @param[in] unit_id Unit ID.
 * @param[in] ct_code CT model code.
 * @return ESP_OK, or ESP_ERR_NOT_FOUND for a unit that is not an rbAmp current channel.
 */
typedef esp_err_t (*rbpower_unit_ct_cb_t)(rbpower_unit_id_t unit_id, uint8_t ct_code);
/** @brief Register the CT-model hook. @param[in] cb Hook, or NULL. */
void rbpower_register_unit_ct_cb(rbpower_unit_ct_cb_t cb);
/**
 * @brief Set the CT model of the channel a unit is bound to (calls the CT-model hook).
 * @param[in] unit_id Unit ID.
 * @param[in] ct_code CT model code.
 * @return Hook result; ESP_ERR_NOT_SUPPORTED when no hook is registered.
 */
esp_err_t rbpower_unit_set_ct(rbpower_unit_id_t unit_id, uint8_t ct_code);

/* ---- Opaque-handle identity API ---- */

/**
 * @brief Allocate a free opaque unit ID from 0x0100..0x3FFF.
 *
 * The range is generated by no legacy convention (native ADC IDs are 0x00XX, ESP-NOW
 * 0x4000..0x7FFF, external bus 0x8000..0xBFFF, virtual 0xC000+). Skips IDs of live units AND IDs
 * present in the flash slot map, so a new unit can never adopt a deleted unit's history slot.
 *
 * @return New unit ID, or RBPOWER_HOME_INVALID_ID when exhausted.
 */
rbpower_unit_id_t rbpower_home_alloc_unit_id(void);

/**
 * @brief Sensor-occupancy lookup: the unit whose primary source matches this physical sensor.
 *
 * Replaces ID equality as the duplicate-bind guard.
 *
 * @param[in] source_type Source type.
 * @param[in] device_uid  Physical device identity.
 * @param[in] channel_idx Channel on the device.
 * @param[in] exclude_uid Unit to skip (rebind of itself).
 * @return Unit ID, or RBPOWER_HOME_INVALID_ID if the sensor is free.
 */
rbpower_unit_id_t rbpower_home_find_unit_by_source(rbpower_source_type_t source_type,
                                                   uint64_t device_uid, uint8_t channel_idx,
                                                   rbpower_unit_id_t exclude_uid);

/**
 * @brief Point slug -> unit ID (live units only).
 * @param[in] point Point slug.
 * @return Unit ID, or RBPOWER_HOME_INVALID_ID if not found.
 */
rbpower_unit_id_t rbpower_home_find_unit_by_point(const char* point);

/**
 * @brief Mutable access to a live unit's definition.
 *
 * For single-writer configuration paths only (rebind/rename, point backfill).
 *
 * @param[in] unit_id Unit ID.
 * @return Definition, or NULL if the unit is not found.
 */
rbpower_unit_definition_t* rbpower_home_get_unit_definition_mutable(rbpower_unit_id_t unit_id);
/**
 * @brief Read-only access to a live unit's definition.
 * @param[in] unit_id Unit ID.
 * @return Definition, or NULL if the unit is not found.
 */
const rbpower_unit_definition_t* rbpower_home_get_unit_definition(rbpower_unit_id_t unit_id);

/**
 * @brief Move a live I2C unit onto another physical sensor, keeping ID/point/history/slot.
 *
 * Fires the unbind hook (clears the OLD fleet entry by unit_id), updates the definition, then fires
 * the bind hook (sets the NEW fleet entry). The caller has already validated target occupancy and
 * recovered the sensor profile.
 *
 * @param[in] unit_id         Unit ID.
 * @param[in] new_source      New data source.
 * @param[in] device_uid      New physical device identity.
 * @param[in] channel_idx     New channel on the device.
 * @param[in] rated_current_a Rated current of the new sensor, A.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT    the home network is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG @p new_source is NULL or the unit is unknown.
 */
rbpower_err_t rbpower_home_rebind_unit_source(rbpower_unit_id_t unit_id,
                                              const rbpower_data_source_t* new_source,
                                              uint64_t device_uid, uint8_t channel_idx,
                                              float rated_current_a);

/**
 * @brief Create a unit at runtime (counterpart of rbpower_home_remove_unit()).
 *
 * Used by the bind API to bind a sensor to a fresh unit. Validates and registers the unit into the
 * live home network (ID map, source mapping, realtime and statistics), reusing a removed slot when
 * available, then invokes the bind hook.
 *
 * @param[in] def Unit definition.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT      the home network is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG   @p def is NULL or invalid.
 * @retval RBPOWER_ERR_INVALID_STATE the unit ID already exists.
 * @retval RBPOWER_ERR_NO_MEMORY     the network is full.
 */
rbpower_err_t rbpower_home_create_unit(const rbpower_unit_definition_t* def);

/**
 * @brief Roll a completed 15-min RESULT up the hierarchy (1 h -> day -> ...).
 *
 * Result-based and source-agnostic: native finalize and rbAmp inject share it, so external-source
 * units aggregate to higher periods through the same path. Call after a unit's 15-min period
 * statistics are populated. Higher finalizes fire only at real wall-clock boundaries.
 *
 * @param[in] unit_id     Unit ID.
 * @param[in] stats_15min Completed 15-min statistics.
 * @return true if the quarter was aggregated into the 1 h period.
 */
bool rbpower_rollup_after_15min(rbpower_unit_id_t unit_id,
                                const rbpower_period_statistics_t* stats_15min);
/**
 * @brief Same as rbpower_rollup_after_15min(), for a quarter whose window ends at @p q_end_ts.
 *
 * The quarter is attributed to the hour of (q_end_ts - 1) even when collected seconds after the boundary.
 *
 * @param[in] unit_id     Unit ID.
 * @param[in] stats_15min Completed 15-min statistics.
 * @param[in] q_end_ts    Window end, epoch s (0 = unknown).
 * @return true if the quarter was aggregated into the 1 h period.
 */
bool rbpower_rollup_after_15min_at(rbpower_unit_id_t unit_id,
                                   const rbpower_period_statistics_t* stats_15min, uint32_t q_end_ts);

//=============================================================================
// STATISTICS SYSTEM API FUNCTIONS (declarations)
//=============================================================================

// System initialization and management
/**
 * @brief Initialize the statistics system (accumulators, task, queue, timers).
 * @param[in] config Global configuration.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG   @p config is NULL.
 * @retval RBPOWER_ERR_ALREADY_INIT  already initialized.
 * @retval RBPOWER_ERR_NO_MEMORY     allocation of the accumulators, queue or task failed.
 */
rbpower_err_t rbpower_statistics_init(const rbpower_statistics_global_config_t *config);

/**
 * @brief Get a pointer to the statistics system state.
 * @param[out] system_state Pointer to the internal state.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p system_state is NULL.
 * @retval RBPOWER_ERR_NOT_INIT    the statistics system is not initialized.
 */
rbpower_err_t rbpower_statistics_get_system_state(rbpower_statistics_system_t **system_state);
/**
 * @brief Log the statistics system diagnostics.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT the statistics system is not initialized.
 */
rbpower_err_t rbpower_statistics_print_diagnostics(void);
//=============================================================================
// STATISTICS INTEGRATION API FUNCTIONS
//=============================================================================

/**
 * @brief Callback for the start of a 3-second statistics period (esp_timer task).
 * @param[in] period_start_time_us Period start time, µs.
 * @param[in] period_end_time_us   Planned period end time, µs.
 */
void rbpower_home_start_3sec_period_callback(uint64_t period_start_time_us, uint64_t period_end_time_us);

/**
 * @brief Callback for the completion of a 3-second statistics period (esp_timer task).
 * @param[in] period_end_time_us Period end time, µs.
 */
void rbpower_home_end_3sec_period_callback(uint64_t period_end_time_us);

/**
 * @brief Get the 3-second statistics of a unit by ID.
 * @param[in]  unit_id Unit ID.
 * @param[out] stats   Destination.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p stats is NULL or the unit is unknown.
 * @retval RBPOWER_ERR_NOT_INIT    the home network is not initialized.
 */
rbpower_err_t rbpower_get_unit_3sec_statistics(rbpower_unit_id_t unit_id, rbpower_period_statistics_t *stats);

/**
 * @brief Log the statistics accumulation diagnostics.
 */
void rbpower_print_accumulation_diagnostics(void);

/**
 * @brief Start the statistics accumulation system.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT      the home network or the statistics system is not initialized.
 * @retval RBPOWER_ERR_INVALID_STATE the 3-second period timer is not running.
 */
rbpower_err_t rbpower_start_accumulation_system(void);

/**
 * @brief Copy the state of one period timer.
 * @param[in]  period_type Period.
 * @param[out] timer_info  Destination.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p timer_info is NULL or @p period_type is out of range.
 * @retval RBPOWER_ERR_NOT_INIT    the statistics system is not initialized.
 */
rbpower_err_t rbpower_statistics_timers_get_info(rbpower_stat_period_type_t period_type, rbpower_timer_info_t *timer_info);

/**
 * @brief Finalize any statistics period of a unit (store + rollup), if it is ready.
 * @param[in] unit_id            Unit index (the finalize path is index-based).
 * @param[in] period_type        Period to finalize.
 * @param[in] period_end_time_us Period end time, µs.
 */
void rbpower_finalize_period_callback(rbpower_unit_id_t unit_id,
                                      rbpower_stat_period_type_t period_type,
                                      uint64_t period_end_time_us);

/**
 * @brief FORCED finalize for tests/diagnostics (statforce1h / forceperiod verbs).
 *
 * Arms the period's readiness so the finalize stores to flash even with fewer than the automatic
 * threshold of input periods, instead of the automatic path's silent NOT-READY early return.
 * Production auto-finalize stays threshold-gated.
 *
 * @param[in] unit_id            Unit index.
 * @param[in] period_type        Period to finalize.
 * @param[in] period_end_time_us Period end time, µs.
 * @return 1 = finalized, 0 = refused (no inputs, nothing to store).
 */
int rbpower_finalize_period_callback_forced(rbpower_unit_id_t unit_id,          /* 1 = finalized, 0 = refused: no inputs (nothing to store) */
                                            rbpower_stat_period_type_t period_type,
                                            uint64_t period_end_time_us);

/**
 * @brief Get the statistics of any period of a unit.
 * @param[in]  unit_id     Unit INDEX in units[] (despite the type).
 * @param[in]  period_type Period type.
 * @param[out] stats       Destination.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p stats is NULL, bad period/index or undefined unit.
 * @retval RBPOWER_ERR_NOT_INIT    the statistics system is not initialized.
 */
rbpower_err_t rbpower_get_unit_period_statistics(rbpower_unit_id_t unit_id,
                                                 rbpower_stat_period_type_t period_type,
                                                 rbpower_period_statistics_t *stats);

/**
 * @brief Check whether a period is ready for use.
 * @param[in] unit_id     Unit INDEX in units[] (despite the type).
 * @param[in] period_type Period type.
 * @return true if the period is complete with at least 50 % data completeness.
 */
bool rbpower_is_period_ready(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type);

/**
 * @brief Completeness of the period being accumulated.
 * @param[in] unit_id     Unit INDEX in units[] (despite the type).
 * @param[in] period_type Period type.
 * @return Received / expected input periods, % (0.0-100.0); 0 if unknown.
 */
float rbpower_get_period_completeness(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type);

/**
 * @brief Log the aggregation diagnostics of a unit.
 * @param[in] unit_id Unit INDEX in units[] (despite the type).
 */
void rbpower_print_aggregation_diagnostics(rbpower_unit_id_t unit_id);

/**
 * @brief Log the aggregation diagnostics of all units.
 */
void rbpower_print_all_aggregation_diagnostics(void);

/**
 * @brief Aggregation API self-test (debugging).
 */
void rbpower_test_aggregation_api(void);

//=============================================================================
// VOLTAGE BUS API FUNCTIONS
//=============================================================================

/**
 * @brief Initialize a voltage bus structure.
 * @param[out] voltage_bus     Voltage bus structure.
 * @param[in]  unit_id         Unit ID.
 * @param[in]  name            Unit name.
 * @param[in]  nominal_voltage Nominal voltage, V.
 * @return ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG @p voltage_bus or @p name is NULL.
 */
esp_err_t rbpower_voltage_bus_init(rbpower_voltage_bus_t *voltage_bus,
                                   rbpower_unit_id_t unit_id,
                                   const char *name,
                                   float nominal_voltage);

/**
 * @brief Start a 3-second period of the voltage bus.
 * @param[in,out] voltage_bus     Voltage bus structure.
 * @param[in]     period_start_us Period start time, µs.
 */
void voltage_bus_start_3sec_period(rbpower_voltage_bus_t *voltage_bus, uint64_t period_start_us);

/**
 * @brief Finalize a 3-second period of the voltage bus.
 * @param[in,out] voltage_bus   Voltage bus structure.
 * @param[in]     period_end_us Period end time, µs.
 */
void voltage_bus_finalize_3sec_period(rbpower_voltage_bus_t *voltage_bus, uint64_t period_end_us);

/**
 * @brief Get the voltage bus structure.
 * @param[out] voltage_bus_ptr Pointer to the voltage bus.
 * @return ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG @p voltage_bus_ptr is NULL.
 * @retval ESP_ERR_NOT_FOUND   no voltage bus is configured.
 */
esp_err_t rbpower_get_voltage_bus_data(rbpower_voltage_bus_t **voltage_bus_ptr);

/**
 * @brief Get the voltage quality.
 * @param[out] voltage_rms RMS voltage, V.
 * @param[out] thd_percent Voltage THD, %.
 * @param[out] quality_ok  true when voltage and THD are within thresholds.
 * @return ESP_OK on success.
 * @retval ESP_ERR_NOT_FOUND no voltage bus is configured.
 */
esp_err_t rbpower_get_voltage_quality(float *voltage_rms, float *thd_percent, bool *quality_ok);

/**
 * @brief Get the 3-second statistics of the voltage bus.
 * @param[out] voltage_rms_avg Average RMS voltage, V.
 * @param[out] frequency_avg   Average frequency, Hz.
 * @param[out] voltage_min     Minimum voltage, V.
 * @param[out] voltage_max     Maximum voltage, V.
 * @return ESP_OK on success.
 * @retval ESP_ERR_NOT_FOUND    no voltage bus is configured.
 * @retval ESP_ERR_NOT_FINISHED no completed 3-second period yet.
 */
esp_err_t rbpower_get_voltage_bus_3sec_stats(float *voltage_rms_avg,
                                             float *frequency_avg,
                                             float *voltage_min,
                                             float *voltage_max);

/**
 * @brief Log the voltage bus diagnostics.
 */
void rbpower_voltage_bus_print_diagnostics(void);

//=============================================================================
// UNITS UTILITIES
//=============================================================================

/**
 * @brief Find the index in units[] by unit ID.
 * @param[in] unit_id Unit ID (16 bits).
 * @return Index in the array, or 0xFF if not found.
 */
uint8_t find_unit_index_by_id(rbpower_unit_id_t unit_id);

/**
 * @brief Raise v4 quality bits on the HOUR record of a unit that is open now; applied when it closes.
 *
 * Used by the source side, e.g. RBSTATS_Q_SRC_RESTART for a module firmware update within the hour.
 *
 * @param[in] unit_def_id Unit ID.
 * @param[in] q_bits      RBSTATS_Q_* bits to raise.
 */
void rbpower_stats_flag_open_hour(rbpower_unit_id_t unit_def_id, uint16_t q_bits);

#endif // RBHOME_TYPES_H