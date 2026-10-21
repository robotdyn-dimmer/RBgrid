/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_main_supply.h
 * @brief Specialized Main Supply unit: grid connection with bidirectional metering and economics.
 *
 * The Main Supply is the primary (grid) source. On top of the generic per-unit pipeline it keeps:
 * - bidirectional energy accounting (consumed from / supplied to the grid);
 * - a 3-second accumulator that feeds telemetry and the lifetime accumulators;
 * - 3-second economics (cost, feed-in revenue, active tariff zone);
 * - running "extended" totals (per-zone daily figures, lifetime totals, simple optimization scores).
 *
 * 15-minute and longer period records for this unit are produced by the generic per-unit
 * statistics pipeline, not by this module.
 *
 * Money values are in the configured tariff currency units; tariff rates are currency units per kWh.
 * Timestamps named `*_us` are `esp_timer_get_time()` microseconds (monotonic since boot) unless noted.
 *
 * Thread safety: the structures are owned by the global home network (`g_home_network`) and are
 * updated from the rbhome measurement/period processing context. They are not internally locked.
 */

#ifndef RBHOME_MAIN_SUPPLY_H
#define RBHOME_MAIN_SUPPLY_H

#include "rbhome_types.h"
#include "rbpower_enums.h"
#include "rbhome_tariff.h"
#include "rbpower_lifetime_accumulators.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// CONSTANTS
//=============================================================================

/** @brief Size of the 15-minute period ring buffer (reserved; not used by the current pipeline). */
#define RBPOWER_MAIN_SUPPLY_15MIN_BUFFER_SIZE  4   ///< Ring buffer length for 15-minute periods
/** @brief Maximum number of tariff zones tracked per Main Supply (array dimension for per-zone stats). */
#define RBPOWER_MAIN_SUPPLY_TARIFF_ZONES      8   ///< Maximum number of tariff zones

//=============================================================================
// ECONOMICS STRUCTURES
//=============================================================================

/**
 * @brief Economics and energy of one 3-second period (basic calculations).
 */
typedef struct {
    // Instant values
    float instant_cost;                     ///< Cost of energy consumed in the 3 s period, currency units
    float instant_revenue;                  ///< Feed-in revenue for energy supplied in the 3 s period, currency units
    float current_tariff_rate;              ///< Active import tariff rate, currency units per kWh
    rbpower_tariff_zone_type_t current_zone;     ///< Active tariff zone

    // Direction
    rbpower_current_direction_t direction;  ///< Dominant current direction in the period
    float energy_consumed_wh;               ///< Energy consumed in the 3 s period, Wh
    float energy_supplied_wh;               ///< Energy supplied (exported) in the 3 s period, Wh
} rbpower_main_supply_economics_3sec_t;

/**
 * @brief Economics of one 15-minute period ring-buffer slot (reserved; not filled by the current pipeline).
 */
typedef struct {
    // Totals
    float cost_consumed;                    ///< Cost of consumed energy, currency units
    float revenue_supplied;                 ///< Feed-in revenue, currency units
    float net_cost;                         ///< Net cost (cost - revenue), currency units

    // Energy
    float energy_consumed_wh;               ///< Energy consumed, Wh
    float energy_supplied_wh;               ///< Energy supplied, Wh

    // Tariffs
    float avg_tariff_consumed;              ///< Average import tariff, currency units per kWh
    float avg_tariff_supplied;              ///< Average feed-in tariff, currency units per kWh
    uint8_t dominant_zone;                  ///< Dominant tariff zone index
    uint8_t zone_changes;                   ///< Number of tariff zone changes in the period
} rbpower_main_supply_economics_15min_t;

//=============================================================================
// REAL-TIME DATA STRUCTURES
//=============================================================================

/**
 * @brief Real-time (200 ms) data of the Main Supply.
 */
typedef struct {
    // Electrical parameters
    float current_rms;                  ///< RMS current, A
    float power_active_w;               ///< Active power, W (signed by direction)
    float power_reactive_var;           ///< Reactive power, var
    float power_apparent_va;            ///< Apparent power, VA
    float power_factor;                 ///< Power factor (0..1)

    // Direction and energy
    rbpower_current_direction_t direction;  ///< Current direction
    float energy_increment_wh;          ///< Energy increment over the 200 ms window, Wh
    float instant_power_consuming_w;    ///< Instant consumed (import) power, W
    float instant_power_supplying_w;    ///< Instant supplied (export) power, W

    // Real-time economics
    float instant_cost_rate;            ///< Instant cost rate, currency units per hour
    float current_tariff;               ///< Active tariff rate, currency units per kWh
    rbpower_tariff_zone_type_t tariff_zone;  ///< Active tariff zone

    // Metadata
    uint32_t samples_count;             ///< Number of input samples in the window
    uint64_t last_update_us;            ///< Last update time, µs (esp_timer)
    bool is_valid;                      ///< Data valid flag
} rbpower_main_supply_realtime_200ms_t;

//=============================================================================
// MAIN SUPPLY DATA SETS
//=============================================================================

/**
 * @brief Electrical quantity accumulators of a period.
 */
typedef struct {
    // RMS values (root-mean-square aggregation)
    double current_rms_final;          ///< Finalized RMS current of the period, A
    // RMS accumulators (root-mean-square aggregation)
    double current_rms_sum_squares;          ///< Sum of squared RMS current samples, A²
    uint32_t rms_samples_count;              ///< Number of RMS samples

    // Power (linear aggregation)
    double power_active_sum;                 ///< Sum of active power samples, W
    double power_reactive_sum;               ///< Sum of reactive power samples, var
    double power_apparent_sum;               ///< Sum of apparent power samples, VA
    double power_factor_sum;                 ///< Sum of power factor samples

    // Frequency
    double frequency_sum;                    ///< Sum of frequency samples, Hz
    uint32_t frequency_samples_count;        ///< Number of frequency samples

    // Finalized period averages (filled in main_supply_finalize_3sec_period(); read by
    // telemetry / API / snapshots, alongside current_rms_final above)
    double power_active_final;               ///< Average active power of the period, W
    double power_reactive_final;             ///< Average reactive power, var
    double power_apparent_final;             ///< Average apparent power, VA
    double power_factor_final;               ///< Average power factor
    double frequency_final;                  ///< Average frequency, Hz
} rbpower_main_sup_electrical_t;

/**
 * @brief Energy accumulators of a period.
 */
typedef struct {
    // Energy accumulators (direct summation)
    double consumed_wh_accumulator;          ///< Consumed (imported) energy, Wh
    double supplied_wh_accumulator;          ///< Supplied (exported) energy, Wh
    double net_wh_accumulator;               ///< Net energy (consumed - supplied), Wh

    // Sample counters per direction
    uint32_t consuming_samples;              ///< Samples with consuming direction
    uint32_t supplying_samples;              ///< Samples with supplying direction
    uint32_t zero_samples;                   ///< Samples with zero current

    // Time accumulators
    uint64_t consuming_time_us;              ///< Time spent consuming, µs
    uint64_t supplying_time_us;              ///< Time spent supplying, µs
} rbpower_main_sup_energy_t;

/**
 * @brief Extreme value accumulators of a period.
 */
typedef struct {
    // Extremes
    float current_min;                       ///< Minimum current, A (FLT_MAX when no samples)
    float current_max;                       ///< Maximum current, A (-FLT_MAX when no samples)
    float power_min;                         ///< Minimum active power, W
    float power_max;                         ///< Maximum active power, W

    // Extreme timestamps
    uint64_t power_min_time_us;              ///< Time of power minimum, µs (esp_timer)
    uint64_t power_max_time_us;              ///< Time of power maximum, µs (esp_timer)
    uint64_t power_peak_time_us;             ///< Time of power peak, µs (esp_timer)
} rbpower_main_sup_extremes_t;

/**
 * @brief Power quality accumulators of a period.
 */
typedef struct {
    // THD accumulators (root-mean-square aggregation)
    double thd_current_sum_squares;          ///< Sum of squared current THD samples, %²
    uint32_t thd_samples_count;              ///< Number of THD samples

    uint32_t quality_alerts_count;           ///< Number of power quality events

    // Simplified percentile accumulators
    float thd_current_max;                   ///< Maximum current THD, %

    double thd_current_final;                ///< Finalized RMS-average current THD of the period, %
} rbpower_main_sup_power_quality_t;

/**
 * @brief Base economics accumulator of a unit period.
 *
 * Replaces rbpower_accumulator_economics_t and rbpower_longterm_economics_t. Used:
 * - in accumulators: rbpower_period_accumulator_t.economics;
 * - in results: rbpower_period_statistics_t.economics;
 * - for all units and all periods, including MAIN_SUPPLY.
 */
typedef struct {
    // Main accumulators
    double cost_consumed;        ///< Cost of consumed energy, currency units
    double revenue_supplied;     ///< Feed-in revenue, currency units
    double net_cost;            ///< Net cost = cost_consumed - revenue_supplied, currency units
    float avg_tariff_per_kwh;   ///< Average tariff over the period, currency units per kWh

    // Metadata
    uint32_t samples_count;     ///< Number of samples used for averages
} rbpower_main_sup_economics_t;

/**
 * @brief Direction and zero-crossing statistics accumulators of a period.
 *
 * Zero-crossing, phase-balance and frequency-deviation fields are filled only when the
 * measurement source reports them in its 200 ms data; otherwise they stay 0.
 */
typedef struct {
    // Direction transitions
    uint32_t direction_changes_count;        ///< Number of direction changes
    uint32_t consuming_to_supplying_count;   ///< Transitions consuming -> supplying
    uint32_t supplying_to_consuming_count;   ///< Transitions supplying -> consuming
    uint32_t rapid_transitions_count;        ///< Rapid transitions (reserved; detection not implemented)

    // Transition time accumulators
    double transition_time_sum;              ///< Sum of transition times (reserved; not filled)
    uint32_t transition_time_samples;        ///< Number of transition time measurements (reserved)

    // Zero-crossing statistics
    uint32_t total_zc_events;                ///< Total zero-crossing events reported by the source
    uint32_t missed_zc_events;               ///< Missed zero-crossing events
    double zc_stability_sum;                 ///< Sum of zero-crossing stability samples, %
    uint32_t zc_stability_samples;           ///< Number of stability samples

    // Phase statistics
    double phase_balance_sum;                ///< Sum of phase balance samples
    double phase_balance_sum_squares;        ///< Sum of squared phase balance samples
    uint32_t phase_anomaly_events;           ///< Number of phase anomaly events
    uint32_t phase_samples_count;            ///< Number of phase samples

    // Frequency deviation
    double frequency_deviation_sum_squares;  ///< Sum of squared frequency deviations, Hz²
    uint32_t frequency_deviation_samples;    ///< Number of frequency deviation samples

    // Finalized period averages (filled in main_supply_finalize_3sec_period())
    double zc_stability_final;               ///< Average zero-crossing stability, %
    double phase_balance_final;              ///< Average phase balance
    double phase_balance_rms_final;          ///< RMS of phase balance
    double frequency_deviation_rms_final;    ///< RMS frequency deviation, Hz
} rbpower_main_sup_direction_stats_t;

/**
 * @brief Metadata accumulators of a period.
 */
typedef struct {
    uint32_t samples_count;                  ///< Total number of samples
    uint32_t valid_samples_count;            ///< Number of valid samples
    uint32_t error_samples_count;            ///< Number of erroneous samples

    uint64_t first_sample_time_us;           ///< First sample time, µs (esp_timer)
    uint64_t last_sample_time_us;            ///< Last sample time, µs (esp_timer)
    uint64_t last_update_us;                 ///< Last update time, µs (esp_timer)
    uint64_t last_zc_timestamp_us;           ///< Last zero-crossing timestamp, µs (0 if not reported)

    bool period_complete;                    ///< Period complete
    bool data_valid;                         ///< Data valid
    uint32_t update_counter;                 ///< Update counter
    uint32_t input_periods_expected;     ///< Expected number of input periods
    uint32_t input_periods_received;     ///< Received number of input periods
    bool ready_for_finalization;        ///< Ready for finalization
    uint32_t aggregation_iteration;      ///< Aggregation iteration number
} rbpower_main_sup_metadata_t;


/**
 * @brief 3-second period statistics of the Main Supply.
 *
 * Fields before @c period_start_us are cleared at the start of every 3 s period.
 */
typedef struct {

    rbpower_main_sup_electrical_t electrical; ///< Electrical accumulators
    rbpower_main_sup_energy_t energy;   ///< Energy accumulators
    rbpower_main_sup_extremes_t extremes; ///< Extremes
    rbpower_main_sup_power_quality_t power_quality; ///< Power quality
    rbpower_main_sup_direction_stats_t direction_stats; ///< Direction statistics
    rbpower_main_sup_economics_t economics; ///< Economics
    rbpower_main_sup_metadata_t metadata; ///< Metadata

    // Period state
    uint64_t period_start_us;           ///< Period start, µs
    uint64_t period_end_us;             ///< Period end, µs
    bool period_complete;               ///< Period complete
    bool ready_for_finalization;        ///< Ready for finalization
} rbpower_main_supply_statistics_3sec_t;

//=============================================================================
// LONG-TERM PERIOD ACCUMULATORS
//=============================================================================

/**
 * @brief 15-minute period accumulator (reserved; the generic per-unit pipeline produces these records).
 */
typedef struct {

    rbpower_main_sup_electrical_t electrical; ///< Electrical accumulators
    rbpower_main_sup_energy_t energy;   ///< Energy accumulators
    rbpower_main_sup_extremes_t extremes; ///< Extremes
    rbpower_main_sup_power_quality_t power_quality; ///< Power quality
    rbpower_main_sup_direction_stats_t direction_stats; ///< Direction statistics
    rbpower_main_sup_economics_t economics; ///< Economics
    rbpower_main_sup_metadata_t metadata; ///< Metadata

    // Tariff zones over 15 minutes
    double cost_by_zone[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES]; ///< Cost per tariff zone, currency units
    uint32_t minutes_in_zone[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES]; ///< Minutes spent in each zone

    // State
    uint64_t period_end_us;             ///< Period end, µs
    bool period_complete;               ///< Period complete
    bool ready_for_finalization;        ///< Ready for finalization
    bool is_active;                     ///< Accumulator active
    uint32_t input_3sec_periods;    ///< Number of 3 s input periods aggregated
} rbpower_main_supply_statistics_15min_t;


/**
 * @brief 1-hour period accumulator (reserved; the generic per-unit pipeline produces these records).
 */
typedef struct {

    rbpower_main_sup_electrical_t electrical; ///< Electrical accumulators
    rbpower_main_sup_energy_t energy;   ///< Energy accumulators
    rbpower_main_sup_extremes_t extremes; ///< Extremes
    rbpower_main_sup_power_quality_t power_quality; ///< Power quality
    rbpower_main_sup_direction_stats_t direction_stats; ///< Direction statistics
    rbpower_main_sup_economics_t economics; ///< Economics
    rbpower_main_sup_metadata_t metadata; ///< Metadata

    // Tariff zones over 1 hour
    double cost_by_zone[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES]; ///< Cost per tariff zone, currency units
    uint32_t minutes_in_zone[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES]; ///< Minutes spent in each zone

    // State
    uint64_t period_end_us;             ///< Period end, µs
    bool period_complete;               ///< Period complete
    bool ready_for_finalization;        ///< Ready for finalization
    bool is_active;                     ///< Accumulator active
    uint32_t input_3sec_periods;    ///< Number of 3 s input periods aggregated
} rbpower_main_supply_statistics_hour_t;

//=============================================================================
// FINALIZED STATISTICS
//=============================================================================

/**
 * @brief Completed-hour statistics record layout for flash (reserved; not written by this module).
 */
typedef struct {
    // Electrical parameters (28 bytes)
    float voltage_rms;              ///< RMS voltage from the voltage bus, V
    float current_rms;              ///< RMS current, A
    float power_active_avg;         ///< Average active power, W
    float power_reactive_avg;       ///< Average reactive power, var
    float power_apparent_avg;       ///< Average apparent power, VA
    float power_factor_avg;         ///< Average power factor
    float frequency_avg;            ///< Average frequency, Hz

    // Energy (12 bytes)
    float consumed_wh;              ///< Consumed energy, Wh
    float supplied_wh;              ///< Supplied energy, Wh
    float net_wh;                   ///< Net energy, Wh

    // Economics (28 bytes), extended for the Main Supply
    float cost_consumed;            ///< Cost of consumed energy, currency units
    float revenue_supplied;         ///< Feed-in revenue, currency units
    float net_cost;                 ///< Net cost, currency units
    float avg_tariff_consumed;      ///< Average import tariff, currency units per kWh
    float avg_tariff_supplied;      ///< Average feed-in tariff, currency units per kWh
    uint8_t dominant_tariff_zone;   ///< Dominant tariff zone index
    uint8_t zone_changes_count;     ///< Number of tariff zone changes

    // Quality (28 bytes)
    float thd_voltage_avg;          ///< Average voltage THD, %
    float thd_voltage_max;          ///< Maximum voltage THD, %
    float thd_current_avg;          ///< Average current THD, %
    uint16_t voltage_dips;          ///< Number of voltage dips
    uint16_t voltage_swells;        ///< Number of voltage swells
    uint16_t interruptions;         ///< Number of interruptions
    float power_factor_min;         ///< Minimum power factor
    float frequency_deviation_max;  ///< Maximum frequency deviation, Hz

    // Direction (24 bytes)
    uint16_t consuming_minutes;     ///< Minutes consuming
    uint16_t supplying_minutes;     ///< Minutes supplying
    uint16_t direction_changes;     ///< Number of direction changes
    uint16_t zero_power_minutes;    ///< Minutes at zero power
    float max_consuming_power;      ///< Maximum consumed power, W
    float max_supplying_power;      ///< Maximum supplied power, W

    // Net metering (12 bytes), Main Supply only
    float energy_credits_earned_wh; ///< Net-metering energy credits earned, Wh
    float energy_credits_used_wh;   ///< Net-metering energy credits used, Wh
    float net_metering_efficiency;  ///< Net-metering efficiency ratio

    // Metadata
    uint64_t period_end_us;         ///< Period end, µs
    bool valid;                     ///< Record valid
    bool written_to_flash;          ///< Record already written to flash
} rbpower_main_supply_result_hour_stats_t;


//=============================================================================
// EXTENDED STATISTICS
//=============================================================================

/**
 * @brief Extended statistics (running totals), updated on every 3 s finalize.
 */
typedef struct {
    double electrical_avg3sec_current_rms;          ///< RMS current of the last 3 s period, A
    // Power (linear aggregation)
    double power_avg3sec_active_sum;                 ///< Active power sum, W (not updated)
    double power_avg3sec_reactive_sum;               ///< Reactive power sum, var (not updated)
    double power_avg3sec_apparent_sum;               ///< Apparent power sum, VA (not updated)
    double power_avg3sec_factor_sum;                 ///< Power factor sum (not updated)
    // Energy (direct summation)
    double energy_avg3sec_consumed_wh;          ///< Energy consumed in the last 3 s period, Wh
    double energy_avg3sec_supplied_wh;          ///< Energy supplied in the last 3 s period, Wh
    double energy_avg3sec_net_wh;               ///< Net energy of the last 3 s period, Wh
    // Extremes
    float extremes_avg3sec_current_min;         ///< Minimum current in the last 3 s period, A
    float extremes_avg3sec_current_max;         ///< Maximum current in the last 3 s period, A
    float extremes_avg3sec_power_min;           ///< Minimum power in the last 3 s period, W
    float extremes_avg3sec_power_max;           ///< Maximum power in the last 3 s period, W
    // THD (root-mean-square aggregation)
    double quality_avg3sec_thd_current;          ///< RMS current THD of the last 3 s period, %

    // Direction statistics (since power-on)
    uint32_t direction_changes_total;   ///< Total direction changes since power-on
    uint64_t last_direction_change_us;  ///< Time of the last direction change, µs (esp_timer)
    float time_consuming_percent;       ///< Share of samples consuming in the last 3 s period, %
    float time_supplying_percent;       ///< Share of samples supplying in the last 3 s period, %

    // Per-tariff-zone statistics (reset every 24 h)
    float energy_by_zone_kwh[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES]; ///< Consumed energy per zone today, kWh
    float cost_by_zone[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES];       ///< Cost per zone today, currency units
    float time_in_zone_percent[RBPOWER_MAIN_SUPPLY_TARIFF_ZONES]; ///< Share of time per zone today, % (renormalized each 3 s)
    uint64_t daily_stats_reset_time;   ///< Time of the last daily reset, µs (esp_timer)

    // Accumulated totals (since initialization)
    double lifetime_consumed_kwh;       ///< Total consumed energy, kWh
    double lifetime_supplied_kwh;       ///< Total supplied energy, kWh
    double lifetime_cost_total;         ///< Total cost, currency units
    double lifetime_revenue_total;      ///< Total feed-in revenue, currency units
    double lifetime_net_balance;        ///< Net balance (cost - revenue), currency units

    // Optimization (reset every ~30 days; simplified estimates)
    double potential_savings_total;     ///< Estimated potential savings this month, currency units
    double achieved_savings;            ///< Achieved savings this month, currency units (not computed; stays 0)
    uint32_t optimal_shifts_executed;   ///< Number of 3 s periods consuming in the night zone this month
    float tariff_efficiency_score;      ///< Tariff efficiency this month, 0-100 %
    uint64_t monthly_stats_reset_time;  ///< Time of the last monthly reset, µs (esp_timer)
} rbpower_main_supply_extended_stats_t;

//=============================================================================
// MAIN SUPPLY STRUCTURE
//=============================================================================

/**
 * @brief Specialized Main Supply unit.
 */
typedef struct {
    // === IDENTIFICATION ===
    rbpower_unit_id_t unit_id;              ///< Unit ID
    char name[32];                          ///< Name (e.g. "Main Supply"), NUL-terminated
    bool is_active;                         ///< Unit active
    rbpower_unit_id_t voltage_bus_id;       ///< ID of the associated voltage bus

    // === REAL-TIME DATA ===
    rbpower_main_supply_realtime_200ms_t realtime_200ms;    ///< Latest 200 ms real-time data
    rbpower_main_supply_statistics_3sec_t statistics_3sec;  ///< 3 s period accumulation
    rbgrid_snapshot_base_t result_base_3sec;                ///< Finalized 3 s result snapshot

    // Per-period accumulators and results (15 min / 1 h) are not stored here: the generic
    // per-unit pipeline produces this unit's period records. Only the 3 s economics remain,
    // because the lifetime accumulators read them.

    // === EXTENDED STATISTICS ===
    rbpower_main_supply_extended_stats_t extended_stats;  ///< Running totals

    /** @brief Period economics (3 s; read by the lifetime accumulators). */
    struct {
        rbpower_main_supply_economics_3sec_t economics_3sec;  ///< Economics and energy of the last 3 s period
    } period_economics;  ///< Period economics

    /** @brief Main Supply configuration. */
    struct {
        float max_current_a;                ///< Maximum current, A
        float max_power_w;                  ///< Maximum power, W
        bool bidirectional_enabled;         ///< Bidirectional metering enabled
        bool net_metering_enabled;          ///< Net metering enabled
        float feed_in_tariff_default;       ///< Default feed-in tariff, currency units per kWh (0 = use import tariff)
        uint8_t tariff_plan_id;             ///< Tariff plan ID
        bool enable_optimization;           ///< Optimization statistics enabled
        bool save_to_flash;                 ///< Save history to flash
    } config;  ///< Configuration

    /** @brief Main Supply diagnostics. */
    struct {
        bool current_normal;                ///< Current within max_current_a
        bool power_normal;                  ///< |power| within max_power_w
        uint32_t overload_events_count;     ///< Overcurrent event counter
        uint32_t reverse_flow_events;       ///< Reverse-flow (consuming -> supplying) event counter
        uint64_t last_event_time_us;        ///< Time of the last event, µs (esp_timer)
        uint32_t error_flags;               ///< Error flags (RBPOWER_ERROR_*)
        float data_quality_percent;         ///< Data quality, 0-100 %
    } diagnostics;  ///< Diagnostics

    // === METADATA ===
    uint64_t created_timestamp_us;          ///< Creation time, µs (esp_timer)
    uint32_t total_updates;                 ///< Total number of updates
    uint64_t uptime_hours;                  ///< Uptime since creation, hours
    uint32_t flash_write_cycles;            ///< Flash write cycles

    // === LIFETIME ACCUMULATORS ===
    rbpower_lifetime_accumulators_supply_t lifetime_accumulators;  ///< Lifetime accumulators (40 bytes)

} rbpower_main_supply_t;

//=============================================================================
// API FUNCTIONS
//=============================================================================

/**
 * @brief Initialize a Main Supply structure and register it as the module's active instance.
 *
 * Clears the structure, sets identification and default configuration (bidirectional and
 * net metering enabled, default feed-in tariff 0.08 currency units per kWh) and resets extremes.
 *
 * @param[out] main_supply Structure to initialize.
 * @param[in]  unit_id     Unit ID.
 * @param[in]  name        Unit name (truncated to 31 characters).
 * @param[in]  max_current Maximum current, A.
 * @param[in]  max_power   Maximum power, W.
 * @return
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_ARG if @p main_supply or @p name is NULL
 * @note Call once during network setup, before the period processing starts.
 */
esp_err_t rbpower_main_supply_init(rbpower_main_supply_t* main_supply,
                                   rbpower_unit_id_t unit_id,
                                   const char* name,
                                   float max_current,
                                   float max_power);

/**
 * @brief Start a 3-second period: clear the 3 s accumulators and set the period bounds.
 *
 * @param[in,out] main_supply     Main Supply (ignored if NULL or inactive).
 * @param[in]     period_start_us Period start, µs (esp_timer); period end is set to start + 3 s.
 * @note Called from the rbhome 3-second period handler; not thread-safe.
 */
void main_supply_start_3sec_period(rbpower_main_supply_t* main_supply, uint64_t period_start_us);

/**
 * @brief Finalize a 3-second period: compute averages, economics and extended statistics.
 *
 * Computes period averages, cost (consumed energy x active tariff) and feed-in revenue
 * (supplied energy x feed-in tariff), fills @c result_base_3sec and updates @c extended_stats.
 * Does nothing if there are no RMS samples in the period.
 *
 * @param[in,out] main_supply   Main Supply (ignored if NULL or inactive).
 * @param[in]     period_end_us Period end, µs (esp_timer).
 * @note Called from the rbhome 3-second period handler; not thread-safe. Reads the active
 *       tariff zone through rbpower_tariff_get_current_zone().
 */
void main_supply_finalize_3sec_period(rbpower_main_supply_t* main_supply, uint64_t period_end_us);

// 15-minute / 1-hour aggregation, finalize and flash save/load are not part of this module:
// the generic per-unit pipeline produces this unit's period records. The 3 s path above feeds
// telemetry and the lifetime accumulators.

#endif // RBHOME_MAIN_SUPPLY_H
