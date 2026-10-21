/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_alt_supply.h
 * @brief Specialized "Alternative Supply" unit: bidirectional energy, period statistics and economics.
 *
 * The alternative supply is the unit configured as the system's second energy source (inverter,
 * generator, storage, ...). This header defines the per-source data model:
 * - bidirectional energy accounting (generated vs. consumed in standby);
 * - a 3 s -> 15 min -> 1 h statistics chain with a 15-minute ring buffer;
 * - tariff-based economics (revenue at the feed-in rate, cost at the current buying rate).
 *
 * @note Experimental scaffold. Only the 3 s finalize and the 15 min / 1 h roll-up are wired into
 *       the period callback. No code feeds @ref rbpower_alternative_supply_t::realtime_200ms from
 *       measurements in this release, so most values stay zero. Source-specific telemetry
 *       (solar irradiance/MPPT, battery SoC/SoH, wind) and the production-analytics, ROI and
 *       maintenance fields are reserved and not used by the current firmware.
 *
 * Money values are in the configured tariff currency (currency units). Timestamps named `*_us`
 * are `esp_timer_get_time()` values (µs since boot) unless stated otherwise.
 */

#ifndef RBHOME_ALT_SUPPLY_H
#define RBHOME_ALT_SUPPLY_H

#include "rbhome_types.h"
#include "rbpower_enums.h"
#include "rbhome_tariff.h"
#include "rbpower_lifetime_accumulators.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// CONSTANTS
//=============================================================================

#define RBPOWER_ALT_SUPPLY_15MIN_BUFFER_SIZE  4   ///< Number of finalized 15-minute periods kept in the ring buffer
#define RBPOWER_ALT_SUPPLY_TARIFF_ZONES      8   ///< Maximum number of tariff zones tracked per period

//=============================================================================
// TYPES AND ENUMERATIONS
//=============================================================================

/**
 * @brief Kind of alternative energy source.
 */
typedef enum {
    RBPOWER_ALT_SOURCE_SOLAR_INVERTER = 0,  ///< Solar (PV) inverter
    RBPOWER_ALT_SOURCE_WIND_TURBINE,        ///< Wind turbine
    RBPOWER_ALT_SOURCE_BATTERY_STORAGE,     ///< Battery storage system
    RBPOWER_ALT_SOURCE_FUEL_CELL,           ///< Fuel cell
    RBPOWER_ALT_SOURCE_MICRO_HYDRO,         ///< Micro-hydro generator
    RBPOWER_ALT_SOURCE_GENERATOR,           ///< Diesel or gas generator
    RBPOWER_ALT_SOURCE_HYBRID,              ///< Hybrid system
    RBPOWER_ALT_SOURCE_OTHER                ///< Any other source type
} rbpower_alt_source_type_t;

/**
 * @brief Operating mode of the alternative source.
 */
typedef enum {
    RBPOWER_ALT_MODE_OFF = 0,               ///< Switched off
    RBPOWER_ALT_MODE_STANDBY,               ///< Standby
    RBPOWER_ALT_MODE_STARTING,              ///< Starting up
    RBPOWER_ALT_MODE_GENERATING,            ///< Generating
    RBPOWER_ALT_MODE_PEAK_SHAVING,         ///< Peak shaving
    RBPOWER_ALT_MODE_GRID_SUPPORT,          ///< Grid support
    RBPOWER_ALT_MODE_ISLAND,                ///< Island (off-grid) operation
    RBPOWER_ALT_MODE_MAINTENANCE,           ///< Maintenance
    RBPOWER_ALT_MODE_FAULT,                 ///< Fault
    RBPOWER_ALT_MODE_DERATING               ///< Derated (reduced output power)
} rbpower_alt_supply_mode_t;

/**
 * @brief Device state of the alternative source.
 */
typedef enum {
    RBPOWER_ALT_STATE_OFFLINE = 0,          ///< Offline / not connected
    RBPOWER_ALT_STATE_INITIALIZING,         ///< Initializing
    RBPOWER_ALT_STATE_READY,                ///< Ready
    RBPOWER_ALT_STATE_RUNNING,              ///< Running
    RBPOWER_ALT_STATE_WARNING,              ///< Running with a warning
    RBPOWER_ALT_STATE_FAULT,                ///< Fault
    RBPOWER_ALT_STATE_EMERGENCY_STOP        ///< Emergency stop
} rbpower_alt_supply_state_t;

//=============================================================================
// ECONOMICS STRUCTURES
//=============================================================================

/**
 * @brief Economics of the last finalized 3-second period.
 */
typedef struct {
    // Instant (per-period) figures
    float instant_generation_cost;          ///< Estimated generation (operating) cost of the 3 s period, currency units
    float instant_generation_revenue;       ///< Generation revenue of the 3 s period, currency units
    float instant_consumption_cost;         ///< Consumption cost of the 3 s period, currency units
    float current_tariff_rate;              ///< Current buying tariff rate, currency units per kWh
    rbpower_tariff_zone_type_t current_zone;     ///< Current tariff zone

    // Direction and energy
    rbpower_current_direction_t direction;  ///< Dominant current direction in the period
    float energy_generated_wh;              ///< Energy generated in the 3 s period, Wh
    float energy_consumed_wh;               ///< Energy consumed in the 3 s period, Wh
    float net_production_wh;                ///< Generated minus consumed in the 3 s period, Wh
} rbpower_alt_supply_economics_3sec_t;

/**
 * @brief Economics summary of a 15-minute period.
 * @note Reserved: not filled by the current firmware.
 */
typedef struct {
    // Totals
    float generation_revenue;               ///< Generation revenue, currency units
    float consumption_cost;                 ///< Consumption cost, currency units
    float net_benefit;                      ///< Revenue minus cost, currency units
    float self_consumption_value;           ///< Value of self-consumed energy, currency units

    // Energy
    float energy_generated_wh;              ///< Energy generated, Wh
    float energy_consumed_wh;               ///< Energy consumed, Wh
    float net_production_wh;                ///< Generated minus consumed, Wh

    // Tariffs
    float avg_tariff_generation;            ///< Average generation (feed-in) rate, currency units per kWh
    float avg_tariff_consumption;           ///< Average consumption rate, currency units per kWh
    uint8_t dominant_zone;                  ///< Tariff zone with the most time in the period
    uint8_t zone_changes;                   ///< Number of tariff zone changes in the period

    // Production metrics
    float capacity_factor;                  ///< Capacity factor, %
    float efficiency_avg;                   ///< Average conversion efficiency, %
} rbpower_alt_supply_economics_15min_t;

//=============================================================================
// REAL-TIME DATA STRUCTURES
//=============================================================================

/**
 * @brief Real-time (200 ms) values of the alternative supply.
 * @note No code fills this structure in the current firmware (see the file note).
 */
typedef struct {
    // Electrical values
    float current_rms;                      ///< RMS current, A
    float power_active_w;                   ///< Active power, W
    float power_reactive_var;               ///< Reactive power, var
    float power_apparent_va;                ///< Apparent power, VA
    float power_factor;                     ///< Power factor (0..1)

    // DC values (PV / battery side)
    float dc_voltage;                       ///< DC voltage, V
    float dc_current;                       ///< DC current, A
    float dc_power;                         ///< DC power, W

    // Direction and energy
    rbpower_current_direction_t direction;  ///< Direction: generating (SUPPLYING) or consuming
    float energy_increment_wh;              ///< Energy increment over the 200 ms window, Wh
    float instant_power_generating_w;       ///< Instant generated power, W
    float instant_power_consuming_w;        ///< Instant consumed power (standby draw), W

    // Operating mode
    rbpower_alt_supply_mode_t mode;         ///< Current operating mode
    rbpower_alt_supply_state_t state;       ///< Current device state
    float efficiency_percent;               ///< Conversion efficiency, %

    /** @brief Source-specific telemetry. Reserved: not used by the current firmware. */
    union {
        /** @brief Solar (PV) values. */
        struct {
            float irradiance_w_m2;          ///< Solar irradiance, W/m²
            float panel_temp_c;             ///< Panel temperature, °C
            float mppt_voltage;             ///< MPPT operating voltage, V
            uint8_t mppt_status;            ///< MPPT tracker status code
        } solar;                            ///< Solar (PV) values (reserved)

        /** @brief Battery storage values. */
        struct {
            float soc_percent;              ///< State of charge, %
            float soh_percent;              ///< State of health, %
            float battery_voltage;          ///< Battery voltage, V
            float battery_temp_c;           ///< Battery temperature, °C
            uint8_t charge_state;           ///< Charge state code
        } battery;                          ///< Battery values (reserved)

        /** @brief Wind turbine values. */
        struct {
            float wind_speed_ms;            ///< Wind speed, m/s
            float rotor_rpm;                ///< Rotor speed, rpm
            float pitch_angle;              ///< Blade pitch angle, degrees
            uint8_t brake_status;           ///< Brake status code
        } wind;                             ///< Wind turbine values (reserved)
    } specific;                             ///< Source-specific telemetry (reserved)

    // Metadata
    uint32_t samples_count;                 ///< Number of samples behind this value
    uint64_t last_update_us;                ///< Time of the last update, µs since boot
    bool is_valid;                          ///< True when the values are valid
} rbpower_alt_supply_realtime_200ms_t;

/**
 * @brief 3-second accumulator and finalized values for the alternative supply.
 *
 * Fields up to (not including) @c period_start_us are cleared at the start of every period.
 */
typedef struct {
    // Electrical accumulators
    double current_rms_sum_squares;         ///< Sum of squared RMS current samples, A²
    double power_active_sum;                ///< Sum of active power samples, W
    double power_reactive_sum;              ///< Sum of reactive power samples, var
    double power_factor_sum;                ///< Sum of power factor samples
    uint32_t electrical_samples_count;      ///< Number of electrical samples

    // DC accumulators
    double dc_power_sum;                    ///< Sum of DC power samples, W
    double dc_voltage_sum;                  ///< Sum of DC voltage samples, V
    double efficiency_sum;                  ///< Sum of efficiency samples, %
    uint32_t dc_samples_count;              ///< Number of DC samples (only samples with DC power > 0)

    // Energy accumulators
    double generated_wh_accumulator;        ///< Generated energy, Wh
    double consumed_wh_accumulator;         ///< Consumed energy (standby draw), Wh
    uint32_t generating_samples;            ///< Samples with direction SUPPLYING
    uint32_t consuming_samples;             ///< Samples with direction CONSUMING
    uint32_t idle_samples;                  ///< Samples with any other direction

    // Extremes
    float power_max_generated;              ///< Maximum generated power, W
    float power_max_consumed;               ///< Maximum consumed power, W
    float efficiency_min;                   ///< Minimum efficiency, %
    float efficiency_max;                   ///< Maximum efficiency, %
    uint64_t peak_generation_time_us;       ///< Time of the generation peak, µs since boot

    // Operating modes
    uint32_t mode_samples[10];              ///< Sample count per operating mode (index = rbpower_alt_supply_mode_t)
    uint32_t state_transitions;             ///< Number of state changes

    // Economics accumulators
    double generation_revenue_accumulator;  ///< Accumulated generation revenue, currency units
    double consumption_cost_accumulator;    ///< Accumulated consumption cost, currency units
    double self_consumption_value_accumulator; ///< Accumulated value of self-consumed energy, currency units
    uint32_t tariff_samples[RBPOWER_ALT_SUPPLY_TARIFF_ZONES]; ///< Sample count per tariff zone

    // Final values, set by alt_supply_finalize_3sec_period()
    float current_rms_final;                ///< RMS current of the period, A
    float power_active_avg_final;           ///< Average active power, W
    float energy_generated_final;           ///< Generated energy, Wh
    float energy_consumed_final;            ///< Consumed energy, Wh
    float efficiency_avg_final;             ///< Average efficiency, %
    float availability_percent;             ///< Share of samples with generation or consumption, %
    float generation_revenue_final;         ///< Generation revenue, currency units
    float consumption_cost_final;           ///< Consumption cost, currency units
    float net_benefit_final;                ///< Revenue minus cost, currency units

    // Period bounds and state
    uint64_t period_start_us;               ///< Period start, µs since boot
    uint64_t period_end_us;                 ///< Period end, µs since boot
    bool period_complete;                   ///< True once the period is finalized
    bool ready_for_finalization;            ///< Ready to be finalized
} rbpower_alt_supply_statistics_3sec_t;

//=============================================================================
// LONG-PERIOD ACCUMULATORS
//=============================================================================

/**
 * @brief 15-minute accumulator, fed from finalized 3-second periods.
 */
typedef struct {
    // Electrical accumulators
    double current_rms_sum_squares;         ///< Sum of squared RMS current samples, A²
    double power_active_sum;                ///< Sum of active power samples, W
    double dc_power_sum;                    ///< Sum of DC power samples, W
    double efficiency_sum;                  ///< Sum of efficiency samples, %
    uint32_t samples_count;                 ///< Number of samples

    // Energy accumulators
    double generated_wh_accumulator;        ///< Generated energy, Wh
    double consumed_wh_accumulator;         ///< Consumed energy, Wh

    // Production metrics
    double production_quality_sum;          ///< Sum of per-3 s availability values (0-100 %)
    uint32_t full_power_seconds;            ///< Seconds at or above 90 % of rated power, s
    uint32_t zero_production_seconds;       ///< Seconds without generation, s

    // Economics accumulators
    double generation_revenue_accumulator;  ///< Generation revenue, currency units
    double consumption_cost_accumulator;    ///< Consumption cost, currency units
    double self_consumption_value_accumulator; ///< Value of self-consumed energy, currency units
    double cost_by_zone[RBPOWER_ALT_SUPPLY_TARIFF_ZONES]; ///< Net value (revenue minus cost) per tariff zone, currency units
    uint32_t minutes_in_zone[RBPOWER_ALT_SUPPLY_TARIFF_ZONES]; ///< Time per tariff zone, min

    // Extremes
    /** @brief Maximum generated power, W. */
    float power_max, efficiency_max;        ///< Maximum efficiency, %
    /** @brief Minimum non-zero generated power, W. */
    float power_min, efficiency_min;        ///< Minimum efficiency, %

    // State
    uint64_t period_start_us;               ///< Period start, µs since boot
    bool is_active;                         ///< True while a period is being accumulated
    uint32_t input_3sec_periods;            ///< Number of 3 s periods aggregated (300 = 15 min)
} rbpower_alt_supply_accumulator_15min_t;

/**
 * @brief 1-hour accumulator, fed from finalized 15-minute periods.
 */
typedef struct {
    // Electrical accumulators
    double current_rms_sum_squares;         ///< Approximate sum of squared RMS current, A²·s
    double power_active_sum;                ///< Sum of average active power weighted by seconds, W·s
    double power_reactive_sum;              ///< Sum of reactive power, var·s (not filled)
    double power_factor_sum;                ///< Sum of power factor (not filled)
    double dc_power_sum;                    ///< Sum of DC power, W·s (not filled)
    double efficiency_sum;                  ///< Sum of efficiency weighted by seconds, %·s
    uint32_t samples_count;                 ///< Number of samples (900 per 15-minute period)

    // Energy accumulators
    double generated_wh_accumulator;        ///< Generated energy, Wh
    double consumed_wh_accumulator;         ///< Consumed energy, Wh
    double net_production_wh_accumulator;   ///< Generated minus consumed, Wh

    // Production metrics
    double capacity_factor_sum;             ///< Sum of 15-minute capacity factors, %
    double availability_sum;                ///< Sum of 15-minute availability values, %
    uint32_t start_stop_cycles;             ///< Start/stop cycles (not filled)
    uint32_t fault_events;                  ///< Fault events (not filled)

    // Extended economics accumulators
    double generation_revenue_accumulator;  ///< Generation revenue, currency units
    double consumption_cost_accumulator;    ///< Consumption cost, currency units
    double self_consumption_value_accumulator; ///< Value of self-consumed energy, currency units (not filled)
    double grid_export_value_accumulator;   ///< Value of energy exported to the grid, currency units (not filled)
    double operational_cost_accumulator;    ///< Operating cost, currency units (not filled)
    double cost_by_zone[RBPOWER_ALT_SUPPLY_TARIFF_ZONES]; ///< Value per tariff zone, currency units (not filled)
    uint32_t minutes_in_zone[RBPOWER_ALT_SUPPLY_TARIFF_ZONES]; ///< Time per tariff zone, min (not filled)

    /** @brief Source-specific accumulators. Reserved: not filled by the current firmware. */
    union {
        /** @brief Solar (PV) accumulators. */
        struct {
            double irradiance_sum;          ///< Sum of irradiance samples, W/m²
            double panel_temp_sum;          ///< Sum of panel temperature samples, °C
            uint32_t mppt_tracking_time;    ///< Time with MPPT tracking, s
        } solar;                            ///< Solar (PV) accumulators (reserved)

        /** @brief Battery accumulators. */
        struct {
            double soc_sum;                 ///< Sum of state-of-charge samples, %
            double charge_cycles;           ///< Charge cycles
            double depth_of_discharge_sum;  ///< Sum of depth-of-discharge values, %
        } battery;                          ///< Battery accumulators (reserved)
    } specific;                             ///< Source-specific accumulators (reserved)

    // Operating modes
    uint32_t mode_minutes[10];              ///< Minutes per operating mode (index = rbpower_alt_supply_mode_t), min

    // Extremes
    /** @brief Maximum 15-minute average active power, W. */
    float power_max, efficiency_max;        ///< Maximum 15-minute average efficiency, %
    /** @brief Maximum DC voltage, V (not filled). */
    float dc_voltage_max, dc_current_max;   ///< Maximum DC current, A (not filled)

    // State
    uint64_t period_start_us;               ///< Period start, µs since boot
    bool is_active;                         ///< True while a period is being accumulated
    uint32_t input_15min_periods;           ///< Number of 15-minute periods aggregated (4 = 1 h)
} rbpower_alt_supply_accumulator_hour_t;

//=============================================================================
// FINALIZED STATISTICS
//=============================================================================

/**
 * @brief Finalized 15-minute period (compact, kept in RAM).
 */
typedef struct {
    float power_active_avg;                 ///< Average active power, W
    float energy_generated_wh;              ///< Generated energy, Wh
    float energy_consumed_wh;               ///< Consumed energy, Wh
    float efficiency_avg;                   ///< Average efficiency, %
    float availability_percent;             ///< Share of time with generation, %
    float capacity_factor;                  ///< Generated energy / (rated power x 0.25 h), %
    float generation_revenue;               ///< Generation revenue, currency units
    float consumption_cost;                 ///< Consumption cost, currency units
    float net_benefit;                      ///< Revenue minus cost, currency units
    uint8_t dominant_mode;                  ///< Operating mode at finalize time (rbpower_alt_supply_mode_t)
    uint8_t dominant_tariff_zone;           ///< Tariff zone with the most time in the period
    uint64_t period_end_us;                 ///< Finalize time, µs since boot
    bool valid;                             ///< True when the record holds data
} rbpower_alt_supply_result_15min_stats_t;

/**
 * @brief Finalized 1-hour period (record intended for flash).
 */
typedef struct {
    // Electrical (20 bytes)
    float current_rms;                      ///< Approximate RMS current, A; replaced by V x I (VA) when voltage bus data is available
    float power_active_avg;                 ///< Average active power, W
    float power_reactive_avg;               ///< Average reactive power, var
    float power_factor_avg;                 ///< Average power factor
    float dc_power_avg;                     ///< Average DC power, W

    // Energy (12 bytes)
    float generated_wh;                     ///< Generated energy, Wh
    float consumed_wh;                      ///< Consumed energy, Wh
    float net_production_wh;                ///< Generated minus consumed, Wh

    // Production metrics (20 bytes)
    float efficiency_avg;                   ///< Average efficiency, %
    float capacity_factor;                  ///< Average capacity factor, %
    float availability_percent;             ///< Average availability, %
    uint16_t start_stop_cycles;             ///< Start/stop cycles
    uint16_t fault_events;                  ///< Fault events

    // Operating modes (12 bytes)
    uint8_t minutes_generating;             ///< Minutes in GENERATING mode, min
    uint8_t minutes_standby;                ///< Minutes in STANDBY mode, min
    uint8_t minutes_offline;                ///< Minutes in OFF mode, min
    uint8_t dominant_mode;                  ///< Mode with the most minutes (rbpower_alt_supply_mode_t)
    float peak_power;                       ///< Peak 15-minute average power, W

    // Economics (28 bytes)
    float generation_revenue;               ///< Generation revenue, currency units
    float consumption_cost;                 ///< Consumption cost, currency units
    float net_benefit;                      ///< Revenue minus cost, currency units
    float self_consumption_value;           ///< Value of self-consumed energy, currency units
    float grid_export_value;                ///< Value of exported energy, currency units
    float operational_cost;                 ///< Operating cost, currency units
    uint8_t dominant_tariff_zone;           ///< Dominant tariff zone (not filled)

    /** @brief Source-specific hourly values (16 bytes). Reserved: no source data is fed. */
    union {
        /** @brief Solar (PV) hourly values. */
        struct {
            float avg_irradiance;           ///< Average irradiance, W/m²
            float energy_yield_ratio;       ///< Energy yield, kWh/kWp
            float performance_ratio;        ///< Performance ratio, %
            float avg_panel_temp;           ///< Average panel temperature, °C
        } solar;                            ///< Solar (PV) values (reserved)

        /** @brief Battery hourly values. */
        struct {
            float avg_soc;                  ///< Average state of charge, %
            float charge_cycles;            ///< Charge cycles
            float round_trip_efficiency;    ///< Round-trip efficiency, %
            float depth_of_discharge;       ///< Average depth of discharge, %
        } battery;                          ///< Battery values (reserved)
    } specific;                             ///< Source-specific values (reserved)

    // Metadata
    uint64_t period_end_us;                 ///< Finalize time, µs since boot
    bool valid;                             ///< True when the record holds data
    bool written_to_flash;                  ///< True once handed to rbpower_alt_supply_save_hour_to_flash()
} rbpower_alt_supply_result_hour_stats_t;

/**
 * @brief Finalized day record.
 * @note Reserved: not produced by the current firmware (day and longer periods are recomputed from flash history).
 */
typedef struct {
    // Production (24 bytes)
    float total_generated_kwh;              ///< Generated energy, kWh
    float total_consumed_kwh;               ///< Consumed energy, kWh
    float net_production_kwh;               ///< Generated minus consumed, kWh
    float peak_power_kw;                    ///< Peak power, kW
    float avg_power_kw;                     ///< Average power, kW
    float capacity_factor_daily;            ///< Daily capacity factor, %

    // Efficiency (16 bytes)
    float avg_efficiency;                   ///< Average efficiency, %
    float performance_ratio_daily;          ///< Daily performance ratio, %
    float availability_percent;             ///< Availability, %
    float production_quality_score;         ///< Production quality score (0-100)

    // Operating modes (12 bytes)
    float hours_generating;                 ///< Hours generating, h
    float hours_standby;                    ///< Hours in standby, h
    float hours_offline;                    ///< Hours offline, h
    uint16_t start_stop_cycles;             ///< Start/stop cycles
    uint16_t fault_events_total;            ///< Fault events

    // Economic value (16 bytes)
    float self_consumption_kwh;         ///< Self-consumed energy, kWh
    float grid_export_kwh;              ///< Energy exported to the grid, kWh
    float value_generated;              ///< Value of the generated energy, currency units
    float operational_cost;             ///< Operating cost, currency units

    /** @brief Source-specific daily values (20 bytes). Reserved. */
    union {
        /** @brief Solar (PV) daily values. */
        struct {
            float total_irradiation_kwh_m2; ///< Irradiation, kWh/m²
            float avg_panel_efficiency;     ///< Average panel efficiency, %
            float soiling_loss_percent;     ///< Soiling loss, %
            float temperature_loss_percent; ///< Temperature loss, %
            float shading_loss_percent;     ///< Shading loss, %
        } solar;                            ///< Solar (PV) values (reserved)

        /** @brief Battery daily values. */
        struct {
            float total_charge_kwh;         ///< Energy charged, kWh
            float total_discharge_kwh;      ///< Energy discharged, kWh
            float round_trip_efficiency;    ///< Round-trip efficiency, %
            float avg_soc_daily;            ///< Average state of charge, %
            float battery_health_change;    ///< Change in state of health, %
        } battery;                          ///< Battery values (reserved)
    } specific;                             ///< Source-specific values (reserved)

    // Metadata
    uint64_t period_end_us;                 ///< Period end, µs since boot
    bool valid;                             ///< True when the record holds data
    bool written_to_flash;                  ///< True once written to flash
} rbpower_alt_supply_result_day_stats_t;

/**
 * @brief Finalized month record.
 * @note Reserved: not produced by the current firmware.
 */
typedef struct {
    // Production summary (24 bytes)
    float total_generated_kwh;              ///< Generated energy, kWh
    float total_consumed_kwh;               ///< Consumed energy, kWh
    float net_production_kwh;               ///< Generated minus consumed, kWh
    float capacity_factor_monthly;          ///< Monthly capacity factor, %
    float availability_percent;             ///< Availability, %
    float performance_ratio_monthly;        ///< Monthly performance ratio, %

    // Efficiency and degradation (16 bytes)
    float avg_efficiency;                   ///< Average efficiency, %
    float efficiency_degradation;       ///< Efficiency loss, %
    float expected_production_kwh;      ///< Expected production, kWh
    float actual_vs_expected_ratio;     ///< Actual / expected production ratio

    // Economics (24 bytes)
    float total_value_generated;        ///< Total value of the generated energy, currency units
    float self_consumption_value;       ///< Value of self-consumed energy, currency units
    float grid_export_value;            ///< Value of exported energy, currency units
    float operational_costs;            ///< Operating costs, currency units
    float maintenance_costs;            ///< Maintenance costs, currency units
    float net_benefit;                  ///< Net benefit, currency units

    // Reliability (12 bytes)
    uint32_t total_operating_hours;         ///< Operating hours, h
    uint32_t total_generating_hours;        ///< Generating hours, h
    uint16_t total_faults;                  ///< Fault count
    uint16_t unplanned_stops;               ///< Unplanned stop count

    // Metadata
    uint64_t period_end_us;                 ///< Period end, µs since boot
    uint8_t days_in_month;                  ///< Number of days in the month
    bool valid;                             ///< True when the record holds data
    bool written_to_flash;                  ///< True once written to flash
} rbpower_alt_supply_result_month_stats_t;

//=============================================================================
// PRODUCTION ANALYTICS
//=============================================================================

/**
 * @brief Cumulative production analytics.
 *
 * Only the lifetime energy totals, operating hours, current efficiency/degradation estimate and
 * the solar specific yield / performance ratio are updated (simplified estimates, in RAM only).
 * The year/month totals, best/worst day, ROI and cost fields are reserved and not used by the
 * current firmware.
 */
typedef struct {
    // Lifetime totals (since initialization)
    double lifetime_generated_kwh;          ///< Total generated energy, kWh
    double lifetime_consumed_kwh;           ///< Total consumed energy, kWh
    double lifetime_net_production_kwh;     ///< Generated minus consumed, kWh
    float    lifetime_operating_hours;      ///< Operating time, h (incremented in 3 s steps)
    float    lifetime_generating_hours;     ///< Generating time, h (incremented in 3 s steps)

    // Production metrics (reserved)
    float ytd_production_kwh;               ///< Production year to date, kWh (reserved)
    float mtd_production_kwh;               ///< Production month to date, kWh (reserved)
    float best_daily_production_kwh;        ///< Best daily production, kWh (reserved)
    float worst_daily_production_kwh;       ///< Worst daily production, kWh (reserved)
    uint64_t best_production_date;          ///< Date of the best daily production (reserved)

    // Efficiency and degradation
    float initial_efficiency;               ///< Initial efficiency, %
    float current_efficiency;               ///< Smoothed current efficiency, %
    float annual_degradation_rate;          ///< Estimated degradation, % per year
    float capacity_retention_percent;       ///< Current / initial efficiency, %

    // Economic analytics, reset yearly (reserved)
    double annual_revenue_total;            ///< Annual revenue, currency units (reserved)
    double annual_operational_costs;        ///< Annual operating costs, currency units (reserved)
    double annual_maintenance_costs;        ///< Annual maintenance costs, currency units (reserved)
    double annual_net_benefit;              ///< Annual net benefit, currency units (reserved)
    float annual_roi_percent;               ///< Annual return on investment, % (reserved)
    uint64_t annual_stats_reset_time;       ///< Time of the last yearly reset (reserved)

    /** @brief Source-specific analytics. */
    union {
        /** @brief Solar (PV) analytics. */
        struct {
            float total_irradiation_received;   ///< Received irradiation, MWh/m² (reserved)
            float specific_yield_kwh_kwp;       ///< Specific yield, kWh/kWp
            float performance_ratio_lifetime;   ///< Lifetime performance ratio estimate, %
            uint32_t cleaning_events;           ///< Panel cleaning count (reserved)
        } solar;                                ///< Solar (PV) analytics

        /** @brief Battery analytics. */
        struct {
            float total_charge_cycles;          ///< Total charge cycles (reserved)
            float avg_depth_of_discharge;       ///< Average depth of discharge, % (reserved)
            float battery_wear_percent;         ///< Battery wear, % (reserved)
            uint32_t full_cycles_equivalent;    ///< Equivalent full cycles (reserved)
        } battery;                              ///< Battery analytics (reserved)
    } specific_analytics;                       ///< Source-specific analytics
} rbpower_alt_supply_production_analytics_t;

//=============================================================================
// MAIN ALTERNATIVE SUPPLY STRUCTURE
//=============================================================================

/**
 * @brief Alternative Supply unit: identity, live data, period statistics, economics and configuration.
 *
 * Allocated and initialized by the home-network setup when an alternative supply unit is
 * configured; reachable through the network's specialized-unit pointer.
 */
typedef struct {
    // === IDENTIFICATION ===
    rbpower_unit_id_t unit_id;              ///< Unit ID of the source
    char name[32];                          ///< Display name (for example "Solar Inverter")
    bool is_active;                         ///< True when the source is processed
    rbpower_unit_id_t voltage_bus_id;       ///< ID of the associated voltage bus
    rbpower_alt_source_type_t type;         ///< Source type

    // === REAL-TIME DATA ===
    rbpower_alt_supply_realtime_200ms_t realtime_200ms;     ///< Latest 200 ms values (not fed in this release)
    rbpower_alt_supply_statistics_3sec_t statistics_3sec;   ///< Current 3 s accumulator and final values
    rbgrid_snapshot_base_t result_base_3sec;                ///< Common 3 s snapshot (energy, cost, direction)

    // === LONG-PERIOD ACCUMULATORS ===
    /** @brief Accumulators for the 15-minute and 1-hour roll-up. */
    struct {
        rbpower_alt_supply_accumulator_15min_t accumulator_15min;  ///< 15-minute accumulator
        rbpower_alt_supply_accumulator_hour_t accumulator_hour;    ///< 1-hour accumulator
    } period_accumulators;                  ///< Long-period accumulators

    // === FINALIZED PERIOD STATISTICS ===
    /** @brief Finalized period results. */
    struct {
        rbpower_alt_supply_result_15min_stats_t result_15min_periods[RBPOWER_ALT_SUPPLY_15MIN_BUFFER_SIZE]; ///< Ring buffer of the last 15-minute periods
        uint8_t current_15min_index;        ///< Ring buffer index of the newest 15-minute period

        rbpower_alt_supply_result_hour_stats_t result_last_hour; ///< Last completed hour
        // Day and month results are not kept here: day and longer periods are recomputed from
        // flash history.
    } period_statistics;                    ///< Finalized period statistics

    // === PRODUCTION ANALYTICS ===
    rbpower_alt_supply_production_analytics_t production_analytics; ///< Cumulative production analytics

    // === PERIOD ECONOMICS ===
    /** @brief Economics of the latest periods. */
    struct {
        rbpower_alt_supply_economics_3sec_t economics_3sec;    ///< Last 3 s period
        rbpower_alt_supply_economics_15min_t economics_15min;  ///< 15-minute summary (reserved)
    } period_economics;                     ///< Period economics

    // === CONFIGURATION ===
    /** @brief Source configuration (defaults set by rbpower_alt_supply_init()). */
    struct {
        // Basic parameters
        float rated_power_kw;               ///< Rated power, kW
        float max_current_a;                ///< Maximum current, A
        float min_operating_voltage;        ///< Minimum operating voltage, V
        float max_operating_voltage;        ///< Maximum operating voltage, V

        // Operating modes (configuration flags; not acted on by the firmware)
        bool grid_tie_enabled;              ///< Grid-tied operation
        bool island_mode_capable;           ///< Island (off-grid) operation supported
        bool anti_islanding_enabled;        ///< Anti-islanding protection enabled
        bool export_limitation_enabled;     ///< Export limitation enabled
        float export_limit_kw;              ///< Export limit, kW

        /** @brief Source-specific configuration. Reserved except panel_capacity_kwp. */
        union {
            /** @brief Solar (PV) configuration. */
            struct {
                float panel_capacity_kwp;    ///< Installed panel capacity, kWp (used for specific yield)
                float temperature_coeff;     ///< Temperature coefficient, % per °C
                float inverter_efficiency;   ///< Inverter efficiency, %
                uint8_t mppt_channels;       ///< Number of MPPT channels (reserved)
            } solar;                         ///< Solar (PV) configuration

            /** @brief Battery configuration. */
            struct {
                float battery_capacity_kwh;  ///< Battery capacity, kWh
                float max_charge_rate_kw;    ///< Maximum charge rate, kW
                float max_discharge_rate_kw; ///< Maximum discharge rate, kW
                float min_soc_percent;       ///< Minimum state of charge, %
                float max_soc_percent;       ///< Maximum state of charge, %
            } battery;                       ///< Battery configuration (reserved)
        } specific_config;                   ///< Source-specific configuration

        // Economic parameters
        float self_consumption_priority;     ///< Share of generation counted as self-consumed, % (0-100)
        float feed_in_tariff;               ///< Feed-in (selling) rate, currency units per kWh; 0 = use the buying rate
        bool time_of_use_optimization;      ///< Time-of-use optimization flag (reserved)
        bool save_to_flash;                 ///< Store history in flash
    } config;                               ///< Configuration

    // === DIAGNOSTICS AND STATE ===
    /** @brief Diagnostics and state. Most counters are reserved and not updated. */
    struct {
        // Current state
        bool is_generating;                 ///< Currently generating
        bool grid_connected;                ///< Connected to the grid
        bool has_active_fault;              ///< An active fault is present
        uint32_t active_warnings;           ///< Active warnings, bit mask
        uint32_t active_faults;             ///< Active faults, bit mask

        // Event counters
        uint32_t total_fault_events;        ///< Fault events
        uint32_t total_warning_events;      ///< Warning events
        uint32_t grid_disconnections;       ///< Grid disconnections
        uint32_t emergency_stops;           ///< Emergency stops

        // Last events
        uint64_t last_fault_time_us;        ///< Time of the last fault, µs since boot
        uint64_t last_maintenance_us;       ///< Time of the last maintenance, µs since boot
        uint32_t last_fault_code;           ///< Code of the last fault

        // Production health (simplified estimates)
        float production_health_score;       ///< Production health score (0-100 %)
        float predicted_annual_yield_kwh;    ///< Predicted annual yield, kWh
        bool maintenance_required;           ///< Maintenance required (reserved)
        float data_quality_percent;         ///< Data quality (0-100 %)
    } diagnostics;                          ///< Diagnostics

    // === METADATA ===
    uint64_t installation_date_us;          ///< Installation time, µs since boot (set at init)
    uint64_t commissioning_date_us;         ///< Commissioning time, µs since boot (set at init)
    uint64_t created_timestamp_us;          ///< Structure creation time, µs since boot
    uint32_t total_updates;                 ///< Number of updates
    uint32_t flash_write_cycles;            ///< Number of hourly records handed to the flash writer
    char serial_number[32];                 ///< Equipment serial number
    char firmware_version[16];              ///< Equipment firmware version

    // === LIFETIME ACCUMULATORS ===
    rbpower_lifetime_accumulators_supply_t lifetime_accumulators;  ///< Lifetime energy accumulators (40 bytes)
} rbpower_alternative_supply_t;

//=============================================================================
// API FUNCTIONS
//
// Calling context: all functions below operate on the caller's structure without locking.
// They are called from the home-network period callback (single task); do not call them
// concurrently for the same structure.
//=============================================================================

/**
 * @brief Initialize an Alternative Supply structure with defaults.
 *
 * Clears the structure, sets identity and type-specific default configuration and registers
 * the structure as the module's current alternative supply (used by the flash-write counter).
 *
 * @param[out] alt_supply      Structure to initialize.
 * @param[in]  unit_id         Unit ID of the source.
 * @param[in]  name            Display name (truncated to 31 characters).
 * @param[in]  type            Source type.
 * @param[in]  rated_power_kw  Rated power, kW.
 * @retval ESP_OK               Initialized.
 * @retval ESP_ERR_INVALID_ARG  @p alt_supply or @p name is NULL.
 */
esp_err_t rbpower_alt_supply_init(rbpower_alternative_supply_t* alt_supply,
                                  rbpower_unit_id_t unit_id,
                                  const char* name,
                                  rbpower_alt_source_type_t type,
                                  float rated_power_kw);

/**
 * @brief Add the current 200 ms values to the 3-second accumulator.
 *
 * Does nothing if the structure is NULL, inactive, or the real-time data is invalid.
 * Uses a function-local static for state-change tracking, so it supports one instance only
 * and is not reentrant.
 *
 * @param[in,out] alt_supply  Structure to update.
 */
void accumulate_alt_supply_3sec_statistics(rbpower_alternative_supply_t* alt_supply);

/**
 * @brief Start a new 3-second period: clear the accumulators and set the period bounds.
 * @param[in,out] alt_supply       Structure to update (ignored if NULL or inactive).
 * @param[in]     period_start_us  Period start, µs since boot.
 */
void alt_supply_start_3sec_period(rbpower_alternative_supply_t* alt_supply, uint64_t period_start_us);

/**
 * @brief Finalize the 3-second period: compute final values, economics and the 3 s snapshot.
 *
 * Returns early (with a warning log) when no samples were accumulated.
 *
 * @param[in,out] alt_supply     Structure to update (ignored if NULL or inactive).
 * @param[in]     period_end_us  Period end, µs since boot.
 */
void alt_supply_finalize_3sec_period(rbpower_alternative_supply_t* alt_supply, uint64_t period_end_us);

/**
 * @brief Add the finalized 3-second period to the 15-minute accumulator.
 *
 * After 300 periods (15 min) the 15-minute period is finalized into the ring buffer and
 * rolled into the hourly accumulator.
 *
 * @param[in,out] alt_supply  Structure to update (ignored if NULL or the 3 s period is not complete).
 */
void aggregate_alt_supply_15min_from_3sec(rbpower_alternative_supply_t* alt_supply);

/**
 * @brief Add the newest finalized 15-minute period to the hourly accumulator.
 *
 * After 4 periods the hour is finalized into @c period_statistics.result_last_hour.
 *
 * @param[in,out] alt_supply  Structure to update; must not be NULL.
 */
void aggregate_alt_supply_hour_from_15min(rbpower_alternative_supply_t* alt_supply);

//=============================================================================
// FLASH STORAGE STUB (hour; called from the live hour finalize)
//=============================================================================

/**
 * @brief Store hourly statistics in flash (stub).
 *
 * Does not write to flash yet: marks the record as written and increments the
 * flash-write counter of the registered alternative supply.
 *
 * @param[in] hour_stats  Finalized hourly record (its @c written_to_flash flag is set).
 * @retval ESP_OK               Record accepted.
 * @retval ESP_ERR_INVALID_ARG  @p hour_stats is NULL or not valid.
 */
esp_err_t rbpower_alt_supply_save_hour_to_flash(const rbpower_alt_supply_result_hour_stats_t* hour_stats);

#endif // RBHOME_ALT_SUPPLY_H
