/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_main_supply.c
 * @brief Main Supply unit: 3-second period finalize, economics and extended statistics.
 *
 * The Main Supply is the primary (grid) source with bidirectional metering. This module
 * finalizes its 3-second period (averages, cost/feed-in revenue from the active tariff zone)
 * and maintains running extended statistics. Longer period records come from the generic
 * per-unit statistics pipeline.
 */

#include "rbhome_main_supply.h"
#include "rbhome_types.h"
#include "rbpower.h"
#include "rbhome_tariff.h"
#include <string.h>
#include <math.h>
#include <float.h>
#include "esp_log.h"
#include "esp_timer.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "MAIN_SUPPLY";

// External variables
/** @brief Global home network (defined in rbhome.c). */
extern rbpower_electrical_network_t g_home_network;

// Local pointer to Main Supply (for fast access)
static rbpower_main_supply_t* g_main_supply = NULL;

// Internal function prototypes
void update_main_supply_diagnostics(rbpower_main_supply_t* main_supply);
static void update_main_supply_extended_stats(rbpower_main_supply_t* main_supply);
// There is no supply-specific 15 min / 1 h aggregation or finalize: the generic per-unit
// pipeline produces this unit's period records. Only the 3 s path
// (main_supply_start/finalize_3sec_period) is kept; it feeds statistics_3sec,
// result_base_3sec and economics_3sec, which telemetry and the lifetime accumulators read.

//=============================================================================
// INITIALIZATION AND CONFIGURATION
//=============================================================================

/* Initialize the Main Supply structure (API documented in rbhome_main_supply.h). */
esp_err_t rbpower_main_supply_init(rbpower_main_supply_t* main_supply,
                                   rbpower_unit_id_t unit_id,
                                   const char* name,
                                   float max_current,
                                   float max_power)
{
    if (!main_supply || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear structure
    memset(main_supply, 0, sizeof(rbpower_main_supply_t));

    // Identification
    main_supply->unit_id = unit_id;
    strncpy(main_supply->name, name, sizeof(main_supply->name) - 1);
    main_supply->is_active = true;
    
    // Configuration
    main_supply->config.max_current_a = max_current;
    main_supply->config.max_power_w = max_power;
    main_supply->config.bidirectional_enabled = true;
    main_supply->config.net_metering_enabled = true;
    main_supply->config.feed_in_tariff_default = 0.08f; // 8 cents/kWh default
    main_supply->config.enable_optimization = true;
    main_supply->config.save_to_flash = true;
    
    // Initialize extremes
    main_supply->statistics_3sec.extremes.current_min = FLT_MAX;
    main_supply->statistics_3sec.extremes.current_max = -FLT_MAX;
    main_supply->statistics_3sec.extremes.power_min = FLT_MAX;
    main_supply->statistics_3sec.extremes.power_max = -FLT_MAX;

    // Metadata
    main_supply->created_timestamp_us = esp_timer_get_time();
    
    // Save global pointer
    g_main_supply = main_supply;
    
    
    return ESP_OK;
}

//=============================================================================
// 200MS DATA UPDATE
//=============================================================================



/**
 * @brief Update Main Supply diagnostics from the latest 200 ms data.
 *
 * Tracks overcurrent and overpower transitions, counts reverse-flow events
 * (consuming -> supplying) and estimates data quality from the sample count.
 * Keeps the previous direction in a function-local static, so it assumes a single
 * Main Supply instance and a single calling context.
 *
 * @param[in,out] main_supply Main Supply (ignored if NULL).
 */
void update_main_supply_diagnostics(rbpower_main_supply_t* main_supply)
{
    if (!main_supply) {
        return;
    }
    
    float current = main_supply->realtime_200ms.current_rms;
    float power = main_supply->realtime_200ms.power_active_w;
    
    // Check current
    bool current_was_normal = main_supply->diagnostics.current_normal;
    main_supply->diagnostics.current_normal = (current <= main_supply->config.max_current_a);
    
    if (current_was_normal && !main_supply->diagnostics.current_normal) {
        main_supply->diagnostics.overload_events_count++;
        main_supply->diagnostics.last_event_time_us = esp_timer_get_time();
        main_supply->diagnostics.error_flags |= RBPOWER_ERROR_CURRENT_OUT_OF_RANGE;
    } else if (!current_was_normal && main_supply->diagnostics.current_normal) {
        main_supply->diagnostics.error_flags &= ~RBPOWER_ERROR_CURRENT_OUT_OF_RANGE;
    }
    
    // Check power
    bool power_was_normal = main_supply->diagnostics.power_normal;
    main_supply->diagnostics.power_normal = (fabsf(power) <= main_supply->config.max_power_w);
    
    if (power_was_normal && !main_supply->diagnostics.power_normal) {
        main_supply->diagnostics.last_event_time_us = esp_timer_get_time();
    }
    
    // Reverse flow counter
    static rbpower_current_direction_t last_direction = RBPOWER_CURRENT_DIRECTION_UNKNOWN;
    if (main_supply->realtime_200ms.direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING &&
        last_direction == RBPOWER_CURRENT_DIRECTION_CONSUMING) {
        main_supply->diagnostics.reverse_flow_events++;
        main_supply->extended_stats.direction_changes_total++;
        main_supply->extended_stats.last_direction_change_us = esp_timer_get_time();
    }
    last_direction = main_supply->realtime_200ms.direction;
    
    // Data quality
    main_supply->diagnostics.data_quality_percent = 
        (main_supply->realtime_200ms.samples_count >= 10) ? 100.0f : 
        (main_supply->realtime_200ms.samples_count * 10.0f);
}

//=============================================================================
// 3-SECOND PERIOD CALLBACK FUNCTIONS
//=============================================================================

/* Start of the 3-second period (API documented in rbhome_main_supply.h). */
void main_supply_start_3sec_period(rbpower_main_supply_t* main_supply, uint64_t period_start_us)
{
    if (!main_supply || !main_supply->is_active) {
        return;
    }
    
    
    // Reset accumulators
    memset(&main_supply->statistics_3sec, 0, 
           offsetof(rbpower_main_supply_statistics_3sec_t, period_start_us));
    
    // Reset extremes
    main_supply->statistics_3sec.extremes.current_min = FLT_MAX;
    main_supply->statistics_3sec.extremes.current_max = -FLT_MAX;
    main_supply->statistics_3sec.extremes.power_min = FLT_MAX;
    main_supply->statistics_3sec.extremes.power_max = -FLT_MAX;

    // Time boundaries
    main_supply->statistics_3sec.period_start_us = period_start_us;
    main_supply->statistics_3sec.period_end_us = period_start_us + 3000000; // +3 seconds
    main_supply->statistics_3sec.period_complete = false;
    main_supply->statistics_3sec.ready_for_finalization = false;
}

/* Finalize the 3-second period (API documented in rbhome_main_supply.h). */
void main_supply_finalize_3sec_period(rbpower_main_supply_t* main_supply, uint64_t period_end_us)
{
    if (!main_supply || !main_supply->is_active) {
        ESP_LOGI(TAG, "main_supply_finalize_3sec_period: msupply=%p, active=%d", 
                 main_supply, main_supply ? main_supply->is_active : -1);
        return;
    }
    
    // Check data availability
    if (main_supply->statistics_3sec.electrical.rms_samples_count == 0) {
        ESP_LOGD(TAG, "No samples for main supply finalization (electrical)");
        ESP_LOGD(TAG, "Main supply debug: is_active=%d, total_updates=%lu, unit_id=0x%04X", 
                 main_supply->is_active, main_supply->total_updates, main_supply->unit_id);
        ESP_LOGD(TAG, "System main_supply_unit_id=0x%04X, has_main_supply=%d", 
                 g_home_network.system.main_supply_unit_id, g_home_network.specialized.has_main_supply);
        return;
    }
    
    // === FINALIZE ELECTRICAL VALUES ===
    
    // RMS current (root mean square aggregation)
    main_supply->statistics_3sec.electrical.current_rms_final = 0.0f;
    if (main_supply->statistics_3sec.electrical.rms_samples_count > 0) {
        main_supply->statistics_3sec.electrical.current_rms_final = sqrtf((float)(main_supply->statistics_3sec.electrical.current_rms_sum_squares / 
                                         main_supply->statistics_3sec.electrical.rms_samples_count));
    }

    // Average power (linear aggregation) 
    float power_active_avg_final = 0.0f;
    float power_reactive_avg_final = 0.0f;
    float power_apparent_avg_final = 0.0f;
    float power_factor_avg_final = 0.0f;
    if (main_supply->statistics_3sec.electrical.rms_samples_count > 0) {
        power_active_avg_final = (float)(main_supply->statistics_3sec.electrical.power_active_sum / 
                                        main_supply->statistics_3sec.electrical.rms_samples_count);
        power_reactive_avg_final = (float)(main_supply->statistics_3sec.electrical.power_reactive_sum / 
                                          main_supply->statistics_3sec.electrical.rms_samples_count);
        power_apparent_avg_final = (float)(main_supply->statistics_3sec.electrical.power_apparent_sum / 
                                          main_supply->statistics_3sec.electrical.rms_samples_count);
        power_factor_avg_final = (float)(main_supply->statistics_3sec.electrical.power_factor_sum / 
                                        main_supply->statistics_3sec.electrical.rms_samples_count);
    }
    
    // Average frequency
    float frequency_avg_final = 50.0f; // Default when no frequency samples, Hz
    if (main_supply->statistics_3sec.electrical.frequency_samples_count > 0) {
        frequency_avg_final = (float)(main_supply->statistics_3sec.electrical.frequency_sum / 
                                     main_supply->statistics_3sec.electrical.frequency_samples_count);
    }
    
    // === FINALIZE ENERGY VALUES ===
    
    // Energy (direct copy of the accumulators), Wh
    float energy_consumed_final = (float)main_supply->statistics_3sec.energy.consumed_wh_accumulator;
    float energy_supplied_final = (float)main_supply->statistics_3sec.energy.supplied_wh_accumulator;
    /* net energy already held in energy.net_wh_accumulator — no separate local needed */
    
    // === FINALIZE POWER QUALITY ===
    
    // Current THD (root-mean-square aggregation)
    float thd_current_avg_final = 0.0f;
    if (main_supply->statistics_3sec.power_quality.thd_samples_count > 0) {
        thd_current_avg_final = sqrtf((float)(main_supply->statistics_3sec.power_quality.thd_current_sum_squares / 
                                             main_supply->statistics_3sec.power_quality.thd_samples_count));
    }
    
    // === FINALIZE DIRECTION STATS ===
    
    // Average zero-crossing stability
    float zc_stability_avg_final = 0.0f;
    if (main_supply->statistics_3sec.direction_stats.zc_stability_samples > 0) {
        zc_stability_avg_final = (float)(main_supply->statistics_3sec.direction_stats.zc_stability_sum / 
                                        main_supply->statistics_3sec.direction_stats.zc_stability_samples);
    }
    
    // Average phase balance
    float phase_balance_avg_final = 0.0f;
    if (main_supply->statistics_3sec.direction_stats.phase_samples_count > 0) {
        phase_balance_avg_final = (float)(main_supply->statistics_3sec.direction_stats.phase_balance_sum / 
                                         main_supply->statistics_3sec.direction_stats.phase_samples_count);
    }
    
    // RMS of phase balance
    float phase_balance_rms_final = 0.0f;
    if (main_supply->statistics_3sec.direction_stats.phase_samples_count > 0) {
        phase_balance_rms_final = sqrtf((float)(main_supply->statistics_3sec.direction_stats.phase_balance_sum_squares / 
                                               main_supply->statistics_3sec.direction_stats.phase_samples_count));
    }
    
    // RMS frequency deviation
    float frequency_deviation_rms_final = 0.0f;
    if (main_supply->statistics_3sec.direction_stats.frequency_deviation_samples > 0) {
        frequency_deviation_rms_final = sqrtf((float)(main_supply->statistics_3sec.direction_stats.frequency_deviation_sum_squares / 
                                                      main_supply->statistics_3sec.direction_stats.frequency_deviation_samples));
    }
    
    // === FINALIZE ECONOMICS ===
    
    // Base economics
    /* Period cost/revenue/net/tariff are computed below from energy x tariff and
     * written into period_economics.economics_3sec + statistics_3sec.economics —
     * no pre-overwrite locals needed here. */

    // === SURFACE 3-SEC RESULT AVERAGES ===
    // Store the finalized per-period averages computed above (previously dropped)
    // so telemetry / API / snapshots can read them, parallel to current_rms_final.
    main_supply->statistics_3sec.electrical.power_active_final   = power_active_avg_final;
    main_supply->statistics_3sec.electrical.power_reactive_final = power_reactive_avg_final;
    main_supply->statistics_3sec.electrical.power_apparent_final = power_apparent_avg_final;
    main_supply->statistics_3sec.electrical.power_factor_final   = power_factor_avg_final;
    main_supply->statistics_3sec.electrical.frequency_final      = frequency_avg_final;
    main_supply->statistics_3sec.power_quality.thd_current_final = thd_current_avg_final;
    main_supply->statistics_3sec.direction_stats.zc_stability_final            = zc_stability_avg_final;
    main_supply->statistics_3sec.direction_stats.phase_balance_final           = phase_balance_avg_final;
    main_supply->statistics_3sec.direction_stats.phase_balance_rms_final       = phase_balance_rms_final;
    main_supply->statistics_3sec.direction_stats.frequency_deviation_rms_final = frequency_deviation_rms_final;

    // === UPDATE TIME BOUNDARIES AND FLAGS ===
    main_supply->statistics_3sec.period_end_us = period_end_us;
    main_supply->statistics_3sec.period_complete = true;
    main_supply->statistics_3sec.ready_for_finalization = false; // Clear the ready flag
    
    // Update metadata
    main_supply->statistics_3sec.metadata.period_complete = true;
    main_supply->statistics_3sec.metadata.data_valid = true;
    main_supply->statistics_3sec.metadata.last_update_us = period_end_us;
    
    // === UPDATE RUNTIME ===
    uint64_t uptime_us = period_end_us - main_supply->created_timestamp_us;
    main_supply->uptime_hours = uptime_us / (1000000ULL * 3600ULL);
    
    // === UPDATE EXTENDED STATISTICS ===
    update_main_supply_extended_stats(main_supply);
    
    // === CALCULATE 3-SECOND ECONOMICS (PERIOD ECONOMICS) ===
    
    // Period energy
    main_supply->period_economics.economics_3sec.energy_consumed_wh = energy_consumed_final;
    main_supply->period_economics.economics_3sec.energy_supplied_wh = energy_supplied_final;
    
    // Dominant current direction from the sample counts
    if (main_supply->statistics_3sec.energy.consuming_samples > main_supply->statistics_3sec.energy.supplying_samples) {
        main_supply->period_economics.economics_3sec.direction = RBPOWER_CURRENT_DIRECTION_CONSUMING;
    } else if (main_supply->statistics_3sec.energy.supplying_samples > main_supply->statistics_3sec.energy.consuming_samples) {
        main_supply->period_economics.economics_3sec.direction = RBPOWER_CURRENT_DIRECTION_SUPPLYING;
    } else if (main_supply->statistics_3sec.energy.zero_samples > 0) {
        main_supply->period_economics.economics_3sec.direction = RBPOWER_CURRENT_DIRECTION_ZERO;
    } else {
        main_supply->period_economics.economics_3sec.direction = RBPOWER_CURRENT_DIRECTION_UNKNOWN;
    }
    
    // Active tariff zone and rate
    rbpower_tariff_zone_type_t current_zone;
    char zone_name[64];
    float current_rate;
    if (rbpower_tariff_get_current_zone(&current_zone, zone_name, &current_rate) == RBPOWER_OK) {
        main_supply->period_economics.economics_3sec.current_zone = current_zone;
        main_supply->period_economics.economics_3sec.current_tariff_rate = current_rate;
    } else {
        ESP_LOGD("RBHOME_TARIFF", "ERROR getting tariff zone");
        main_supply->period_economics.economics_3sec.current_zone = 0;
        main_supply->period_economics.economics_3sec.current_tariff_rate = rbpower_get_current_tariff_rate_simple();
    }

    // Cost and revenue of the 3 s period
    if (energy_consumed_final > 0) {
        // Cost = energy (kWh) * import tariff (currency units per kWh)
        main_supply->period_economics.economics_3sec.instant_cost = 
            (energy_consumed_final / 1000.0f) * main_supply->period_economics.economics_3sec.current_tariff_rate;
    } else {
        main_supply->period_economics.economics_3sec.instant_cost = 0.0f;
    }
    
    if (energy_supplied_final > 0) {
        // Revenue = energy (kWh) * feed-in tariff (currency units per kWh);
        // use the configured feed-in tariff, otherwise the current import tariff
        float feed_in_rate = main_supply->config.feed_in_tariff_default > 0 ? 
                            main_supply->config.feed_in_tariff_default : 
                            main_supply->period_economics.economics_3sec.current_tariff_rate;
        main_supply->period_economics.economics_3sec.instant_revenue = 
            (energy_supplied_final / 1000.0f) * feed_in_rate;
    } else {
        main_supply->period_economics.economics_3sec.instant_revenue = 0.0f;
    }
    
    // === COPY CALCULATED ECONOMIC INDICATORS TO STATISTICS ===
    
    // Copy the computed values from period_economics into statistics_3sec for aggregation
    main_supply->statistics_3sec.economics.cost_consumed = main_supply->period_economics.economics_3sec.instant_cost;
    main_supply->statistics_3sec.economics.revenue_supplied = main_supply->period_economics.economics_3sec.instant_revenue;
    main_supply->statistics_3sec.economics.net_cost = main_supply->period_economics.economics_3sec.instant_cost - 
                                                       main_supply->period_economics.economics_3sec.instant_revenue;
    main_supply->statistics_3sec.economics.avg_tariff_per_kwh = main_supply->period_economics.economics_3sec.current_tariff_rate;
    main_supply->statistics_3sec.economics.samples_count = main_supply->statistics_3sec.metadata.samples_count;
    
    main_supply->result_base_3sec.cost_consumed = main_supply->period_economics.economics_3sec.instant_cost;
    main_supply->result_base_3sec.revenue_supplied = main_supply->period_economics.economics_3sec.instant_revenue;
    main_supply->result_base_3sec.avg_tariff_per_kwh = main_supply->period_economics.economics_3sec.current_tariff_rate;
    main_supply->result_base_3sec.energy_consumed_wh = main_supply->period_economics.economics_3sec.energy_consumed_wh;
    main_supply->result_base_3sec.energy_supplied_wh = main_supply->period_economics.economics_3sec.energy_supplied_wh;
    main_supply->result_base_3sec.direction = main_supply->period_economics.economics_3sec.direction;
    main_supply->result_base_3sec.period_end_us = main_supply->statistics_3sec.period_end_us;
    main_supply->result_base_3sec.period_start_us = main_supply->created_timestamp_us;
    main_supply->result_base_3sec.samples_count = main_supply->statistics_3sec.metadata.samples_count;
    main_supply->result_base_3sec.data_valid = main_supply->statistics_3sec.metadata.data_valid;

    /*
    // Debug log of the economics calculation
    ESP_LOGI(TAG, "Economics 3sec: Energy_consumed=%.3fWh, Tariff=%.4f€/kWh, Cost=%.6f€", 
             energy_consumed_final, main_supply->period_economics.economics_3sec.current_tariff_rate,
             main_supply->statistics_3sec.economics.cost_consumed);
    
    // === LOG RESULTS ===
    
    ESP_LOGI(TAG, "Main Supply 3sec finalized:");
    ESP_LOGI(TAG, "  Electrical: I=%.2fA, Pact=%.1fW, Preact=%.1fVAR, PF=%.3f, F=%.2fHz", 
             main_supply->statistics_3sec.electrical.current_rms_final, power_active_avg_final, power_reactive_avg_final, 
             power_factor_avg_final, frequency_avg_final);
    s*/
}

/**
 * @brief Update Main Supply extended statistics after a 3 s finalize.
 *
 * Updates direction shares, per-zone daily figures, lifetime totals, the daily (24 h) and
 * monthly (~30 days) resets, simplified optimization estimates and the last-3 s copies.
 *
 * @param[in,out] main_supply Main Supply (ignored if NULL or inactive).
 */
static void update_main_supply_extended_stats(rbpower_main_supply_t* main_supply)
{
    if (!main_supply || !main_supply->is_active) {
        return;
    }
    
    // === UPDATE DIRECTION STATISTICS ===
    
    // Total number of samples
    uint32_t total_samples = main_supply->statistics_3sec.energy.consuming_samples + 
                            main_supply->statistics_3sec.energy.supplying_samples + 
                            main_supply->statistics_3sec.energy.zero_samples;
    
    if (total_samples > 0) {
        // Share of time in each direction, %
        main_supply->extended_stats.time_consuming_percent = 
            (100.0f * main_supply->statistics_3sec.energy.consuming_samples) / total_samples;
        main_supply->extended_stats.time_supplying_percent = 
            (100.0f * main_supply->statistics_3sec.energy.supplying_samples) / total_samples;
    }
    
    // === UPDATE ACCUMULATED DIRECTION STATISTICS ===
    
    // Total direction changes since power-on
    main_supply->extended_stats.direction_changes_total += 
        main_supply->statistics_3sec.direction_stats.direction_changes_count;
    
    // Time of the last direction change (if any)
    if (main_supply->statistics_3sec.direction_stats.direction_changes_count > 0) {
        main_supply->extended_stats.last_direction_change_us = esp_timer_get_time();
    }
    
    // === UPDATE DAILY STATISTICS BY TARIFF ZONES ===
    // Active tariff zone
    rbpower_tariff_zone_type_t current_zone;
    char zone_name[64];
    float current_rate;
    if (rbpower_tariff_get_current_zone(&current_zone, zone_name, &current_rate) == RBPOWER_OK) {
        // Validate the zone index
        if (current_zone < RBPOWER_MAIN_SUPPLY_TARIFF_ZONES) {
            // Energy per zone, kWh
            main_supply->extended_stats.energy_by_zone_kwh[current_zone] += 
                (main_supply->statistics_3sec.energy.consumed_wh_accumulator / 1000.0f);
            
            // Cost per zone
            main_supply->extended_stats.cost_by_zone[current_zone] += 
                main_supply->period_economics.economics_3sec.instant_cost;
            
            // Time in zone (3 s = 0.05 min)
            float time_in_zone_minutes = 3.0f / 60.0f; // 3 s in minutes
            main_supply->extended_stats.time_in_zone_percent[current_zone] += time_in_zone_minutes;
        }
    }

    // === UPDATE ACCUMULATED TOTALS (LIFETIME) ===
    
    // Total consumption since initialization
    main_supply->extended_stats.lifetime_consumed_kwh += 
        (main_supply->statistics_3sec.energy.consumed_wh_accumulator / 1000.0);
        
    // Total supply since initialization
    main_supply->extended_stats.lifetime_supplied_kwh += 
        (main_supply->statistics_3sec.energy.supplied_wh_accumulator / 1000.0);
        
    // Total cost
    main_supply->extended_stats.lifetime_cost_total += 
        main_supply->period_economics.economics_3sec.instant_cost;
        
    // Total revenue
    main_supply->extended_stats.lifetime_revenue_total += 
        main_supply->period_economics.economics_3sec.instant_revenue;
        
    // Net balance
    main_supply->extended_stats.lifetime_net_balance = 
        main_supply->extended_stats.lifetime_cost_total - 
        main_supply->extended_stats.lifetime_revenue_total;
    
    // === CHECK AND RESET DAILY STATISTICS ===
    
    // Current local time in seconds since midnight
    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    uint32_t seconds_since_midnight = timeinfo->tm_hour * 3600 + 
                                     timeinfo->tm_min * 60 + 
                                     timeinfo->tm_sec;
    
    // Reset the daily statistics once 24 h have passed since the last reset
    uint32_t hours_since_reset = (esp_timer_get_time() - main_supply->extended_stats.daily_stats_reset_time) / 
                                (1000000ULL * 3600ULL);
    if (hours_since_reset >= 24) {
        // Reset daily data
        for (int i = 0; i < RBPOWER_MAIN_SUPPLY_TARIFF_ZONES; i++) {
            main_supply->extended_stats.energy_by_zone_kwh[i] = 0.0f;
            main_supply->extended_stats.cost_by_zone[i] = 0.0f;
            main_supply->extended_stats.time_in_zone_percent[i] = 0.0f;
        }
        main_supply->extended_stats.daily_stats_reset_time = esp_timer_get_time();
        
        ESP_LOGI("MSUPPLY_EXT", "Daily statistics reset for Main Supply");
    }
    
    // === CHECK AND RESET MONTHLY OPTIMIZATION STATISTICS ===
    
    uint32_t hours_since_monthly_reset = (esp_timer_get_time() - main_supply->extended_stats.monthly_stats_reset_time) / 
                                        (1000000ULL * 3600ULL);
    if (hours_since_monthly_reset >= (24 * 30)) { // Approximately 30 days
        // Reset monthly optimization data
        main_supply->extended_stats.potential_savings_total = 0.0;
        main_supply->extended_stats.achieved_savings = 0.0;
        main_supply->extended_stats.optimal_shifts_executed = 0;
        main_supply->extended_stats.tariff_efficiency_score = 0.0f;
        main_supply->extended_stats.monthly_stats_reset_time = esp_timer_get_time();
        
        ESP_LOGI("MSUPPLY_EXT", "Monthly optimization statistics reset for Main Supply");
    }
    
    // === CALCULATE TIME PERCENTAGES IN ZONES ===
    
    // Recompute the per-zone time shares from the accumulated time
    float total_time_minutes = 0.0f;
    for (int i = 0; i < RBPOWER_MAIN_SUPPLY_TARIFF_ZONES; i++) {
        total_time_minutes += main_supply->extended_stats.time_in_zone_percent[i];
    }
    
    if (total_time_minutes > 0.1f) { // Avoid division by zero
        for (int i = 0; i < RBPOWER_MAIN_SUPPLY_TARIFF_ZONES; i++) {
            main_supply->extended_stats.time_in_zone_percent[i] = 
                (main_supply->extended_stats.time_in_zone_percent[i] / total_time_minutes) * 100.0f;
        }
    }
    
    // === OPTIMIZATION AND EFFICIENCY (SIMPLIFIED CALCULATIONS) ===
    
    // Simple potential-savings estimate based on off-peak zone usage
    float night_zone_usage = (current_zone == RBPOWER_TARIFF_ZONE_NIGHT) ?
                             main_supply->period_economics.economics_3sec.instant_cost : 0.0f;
    float day_zone_usage = (current_zone == RBPOWER_TARIFF_ZONE_DAY) ?
                          main_supply->period_economics.economics_3sec.instant_cost : 0.0f;

    if (night_zone_usage > 0 && day_zone_usage == 0) {
        // Night tariff in use: count as potential savings
        main_supply->extended_stats.potential_savings_total += night_zone_usage * 0.3f; // assumed 30% saving
    }
    
    // Simple tariff efficiency estimate (0-100%)
    if (main_supply->extended_stats.lifetime_cost_total > 0) {
        float efficiency_ratio = main_supply->extended_stats.achieved_savings / main_supply->extended_stats.lifetime_cost_total;
        main_supply->extended_stats.tariff_efficiency_score = 
            fminf(100.0f, fmaxf(0.0f, efficiency_ratio * 100.0f));
    }
    
    // Count optimal load shifts (simplified)
    if (current_zone == RBPOWER_TARIFF_ZONE_NIGHT &&
        main_supply->statistics_3sec.energy.consuming_samples > 0) {
        main_supply->extended_stats.optimal_shifts_executed++;
    }
    
    // === FILL AVERAGE VALUES FOR 3-SECOND PERIOD ===
    
    // Electrical: RMS current
    main_supply->extended_stats.electrical_avg3sec_current_rms = 
        main_supply->statistics_3sec.electrical.current_rms_final;
    
    // Energy
    main_supply->extended_stats.energy_avg3sec_consumed_wh = 
        main_supply->statistics_3sec.energy.consumed_wh_accumulator;
    main_supply->extended_stats.energy_avg3sec_supplied_wh = 
        main_supply->statistics_3sec.energy.supplied_wh_accumulator;
    main_supply->extended_stats.energy_avg3sec_net_wh = 
        main_supply->statistics_3sec.energy.net_wh_accumulator;
    
    // Extremes
    main_supply->extended_stats.extremes_avg3sec_current_min = 
        main_supply->statistics_3sec.extremes.current_min;
    main_supply->extended_stats.extremes_avg3sec_current_max = 
        main_supply->statistics_3sec.extremes.current_max;
    main_supply->extended_stats.extremes_avg3sec_power_min = 
        main_supply->statistics_3sec.extremes.power_min;
    main_supply->extended_stats.extremes_avg3sec_power_max = 
        main_supply->statistics_3sec.extremes.power_max;
    
    // Power quality: RMS current THD
    if (main_supply->statistics_3sec.power_quality.thd_samples_count > 0) {
        main_supply->extended_stats.quality_avg3sec_thd_current = 
            sqrtf((float)(main_supply->statistics_3sec.power_quality.thd_current_sum_squares / 
                         main_supply->statistics_3sec.power_quality.thd_samples_count));
    } else {
        main_supply->extended_stats.quality_avg3sec_thd_current = 0.0;
    }

}

