/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_alt_supply.c
 * @brief Alternative Supply implementation: 3 s accumulation/finalize, 15 min and 1 h roll-up, economics.
 *
 * Experimental scaffold: the 3 s finalize and the 15 min / 1 h roll-up run from the period
 * callback, but no code feeds live measurements into the real-time block in this release.
 * Source-specific (solar/battery) values and the production analytics are simplified
 * estimates kept in RAM. Not thread-safe: all functions run in the period-callback context.
 */

#include "rbhome_alt_supply.h"
#include "rbhome_types.h"
#include "rbpower.h"
#include "rbhome_tariff.h"
#include <string.h>
#include <math.h>
#include <float.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ALT_SUPPLY";

/** @brief Global home network (defined in rbhome.c). */
extern rbpower_electrical_network_t g_home_network;

/** @brief Alternative supply registered by rbpower_alt_supply_init(); used by the flash-write counter. */
static rbpower_alternative_supply_t* g_alt_supply = NULL;

// Internal function prototypes
static void update_alt_supply_diagnostics(rbpower_alternative_supply_t* alt_supply);
static void update_alt_supply_production_analytics(rbpower_alternative_supply_t* alt_supply);
static void finalize_alt_supply_15min_period(rbpower_alternative_supply_t* alt_supply);
static void finalize_alt_supply_hour_period(rbpower_alternative_supply_t* alt_supply);

//=============================================================================
// INITIALIZATION AND CONFIGURATION
//=============================================================================

/*
 * Initialize an Alternative Supply structure (documented in rbhome_alt_supply.h).
 */
esp_err_t rbpower_alt_supply_init(rbpower_alternative_supply_t* alt_supply,
                                  rbpower_unit_id_t unit_id,
                                  const char* name,
                                  rbpower_alt_source_type_t type,
                                  float rated_power_kw)
{
    if (!alt_supply || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear the structure
    memset(alt_supply, 0, sizeof(rbpower_alternative_supply_t));
    
    // Identification
    alt_supply->unit_id = unit_id;
    strncpy(alt_supply->name, name, sizeof(alt_supply->name) - 1);
    alt_supply->is_active = true;
    alt_supply->type = type;
    
    // Basic configuration
    alt_supply->config.rated_power_kw = rated_power_kw;
    alt_supply->config.max_current_a = rated_power_kw * 1000.0f / 230.0f; // Approximate value for 230 V
    alt_supply->config.min_operating_voltage = 180.0f;
    alt_supply->config.max_operating_voltage = 270.0f;
    
    // Operating modes
    alt_supply->config.grid_tie_enabled = true;
    alt_supply->config.island_mode_capable = false;
    alt_supply->config.anti_islanding_enabled = true;
    alt_supply->config.export_limitation_enabled = false;
    alt_supply->config.export_limit_kw = rated_power_kw;
    
    // Economic parameters
    alt_supply->config.self_consumption_priority = 80.0f; // 80 % of generation counted as self-consumed
    alt_supply->config.feed_in_tariff = 0.05f; // Default 0.05 currency units per kWh
    alt_supply->config.time_of_use_optimization = true;
    alt_supply->config.save_to_flash = true;
    
    // Type-specific defaults
    switch (type) {
        case RBPOWER_ALT_SOURCE_SOLAR_INVERTER:
            alt_supply->config.specific_config.solar.panel_capacity_kwp = rated_power_kw * 1.1f; // Panels slightly larger than the inverter
            alt_supply->config.specific_config.solar.temperature_coeff = -0.35f; // -0.35%/°C
            alt_supply->config.specific_config.solar.inverter_efficiency = 97.5f; // 97.5%
            alt_supply->config.specific_config.solar.mppt_channels = 2;
            break;
            
        case RBPOWER_ALT_SOURCE_BATTERY_STORAGE:
            alt_supply->config.specific_config.battery.battery_capacity_kwh = rated_power_kw * 2.0f; // 2 hours at rated power
            alt_supply->config.specific_config.battery.max_charge_rate_kw = rated_power_kw;
            alt_supply->config.specific_config.battery.max_discharge_rate_kw = rated_power_kw;
            alt_supply->config.specific_config.battery.min_soc_percent = 20.0f;
            alt_supply->config.specific_config.battery.max_soc_percent = 95.0f;
            break;
            
        default:
            // Other types: basic settings only
            break;
    }
    
    // Initialize extremes
    alt_supply->statistics_3sec.power_max_generated = -FLT_MAX;
    alt_supply->statistics_3sec.power_max_consumed = -FLT_MAX;
    alt_supply->statistics_3sec.efficiency_min = FLT_MAX;
    alt_supply->statistics_3sec.efficiency_max = -FLT_MAX;
    
    // Initialize accumulators
    alt_supply->period_accumulators.accumulator_15min.power_min = FLT_MAX;
    alt_supply->period_accumulators.accumulator_15min.power_max = -FLT_MAX;
    alt_supply->period_accumulators.accumulator_15min.efficiency_min = FLT_MAX;
    alt_supply->period_accumulators.accumulator_15min.efficiency_max = -FLT_MAX;
    
    alt_supply->period_accumulators.accumulator_hour.power_max = -FLT_MAX;
    alt_supply->period_accumulators.accumulator_hour.efficiency_max = -FLT_MAX;
    alt_supply->period_accumulators.accumulator_hour.dc_voltage_max = -FLT_MAX;
    alt_supply->period_accumulators.accumulator_hour.dc_current_max = -FLT_MAX;
    
    // Initialize production analytics
    alt_supply->production_analytics.initial_efficiency = 100.0f; // Initial efficiency 100 %
    alt_supply->production_analytics.current_efficiency = 100.0f;
    alt_supply->production_analytics.annual_degradation_rate = 0.5f; // 0.5 % per year
    alt_supply->production_analytics.capacity_retention_percent = 100.0f;
    
    // Metadata
    alt_supply->created_timestamp_us = esp_timer_get_time();
    alt_supply->installation_date_us = esp_timer_get_time();
    alt_supply->commissioning_date_us = esp_timer_get_time();
    strncpy(alt_supply->serial_number, "N/A", sizeof(alt_supply->serial_number) - 1);
    strncpy(alt_supply->firmware_version, "1.0.0", sizeof(alt_supply->firmware_version) - 1);
    
    // Register the module-level pointer
    g_alt_supply = alt_supply;
    
    ESP_LOGI(TAG, "Alternative Supply initialized: %s, Type=%d, Power=%.1fkW", 
             name, type, rated_power_kw);
    
    return ESP_OK;
}


/*
 * Add the current 200 ms values to the 3 s accumulator (documented in rbhome_alt_supply.h).
 */
void accumulate_alt_supply_3sec_statistics(rbpower_alternative_supply_t* alt_supply)
{
    if (!alt_supply || !alt_supply->is_active || !alt_supply->realtime_200ms.is_valid) {
        return;
    }
    
    rbpower_alt_supply_statistics_3sec_t* stats = &alt_supply->statistics_3sec;
    
    // Accumulate electrical values
    float current = alt_supply->realtime_200ms.current_rms;
    stats->current_rms_sum_squares += (current * current);
    stats->power_active_sum += alt_supply->realtime_200ms.power_active_w;
    stats->power_reactive_sum += alt_supply->realtime_200ms.power_reactive_var;
    stats->power_factor_sum += alt_supply->realtime_200ms.power_factor;
    stats->electrical_samples_count++;
    
    // Accumulate DC values
    if (alt_supply->realtime_200ms.dc_power > 0) {
        stats->dc_power_sum += alt_supply->realtime_200ms.dc_power;
        stats->dc_voltage_sum += alt_supply->realtime_200ms.dc_voltage;
        stats->efficiency_sum += alt_supply->realtime_200ms.efficiency_percent;
        stats->dc_samples_count++;
    }
    
    // Accumulate energy
    if (alt_supply->realtime_200ms.direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING) {
        stats->generated_wh_accumulator += alt_supply->realtime_200ms.energy_increment_wh;
        stats->generating_samples++;
    } else if (alt_supply->realtime_200ms.direction == RBPOWER_CURRENT_DIRECTION_CONSUMING) {
        stats->consumed_wh_accumulator += alt_supply->realtime_200ms.energy_increment_wh;
        stats->consuming_samples++;
    } else {
        stats->idle_samples++;
    }
    
    // Update extremes
    if (alt_supply->realtime_200ms.instant_power_generating_w > stats->power_max_generated) {
        stats->power_max_generated = alt_supply->realtime_200ms.instant_power_generating_w;
        stats->peak_generation_time_us = esp_timer_get_time();
    }
    
    if (alt_supply->realtime_200ms.instant_power_consuming_w > stats->power_max_consumed) {
        stats->power_max_consumed = alt_supply->realtime_200ms.instant_power_consuming_w;
    }
    
    if (alt_supply->realtime_200ms.efficiency_percent > 0) {
        if (alt_supply->realtime_200ms.efficiency_percent < stats->efficiency_min) {
            stats->efficiency_min = alt_supply->realtime_200ms.efficiency_percent;
        }
        if (alt_supply->realtime_200ms.efficiency_percent > stats->efficiency_max) {
            stats->efficiency_max = alt_supply->realtime_200ms.efficiency_percent;
        }
    }
    
    // Operating-mode counters
    if (alt_supply->realtime_200ms.mode < 10) {
        stats->mode_samples[alt_supply->realtime_200ms.mode]++;
    }
    
    // Track state changes (function-local static: single instance only)
    static rbpower_alt_supply_state_t last_state = RBPOWER_ALT_STATE_OFFLINE;
    if (alt_supply->realtime_200ms.state != last_state) {
        stats->state_transitions++;
        last_state = alt_supply->realtime_200ms.state;
    }
    
    // Accumulate economics at the current tariff rate
    float tariff_rate = rbpower_get_current_tariff_rate_simple();

    if (stats->generated_wh_accumulator > 0) {
        // Generation revenue = energy x feed-in rate (buying rate if no feed-in rate is set)
        float feed_in_rate = alt_supply->config.feed_in_tariff > 0 ? 
                            alt_supply->config.feed_in_tariff : tariff_rate;
        stats->generation_revenue_accumulator += 
            (alt_supply->realtime_200ms.energy_increment_wh / 1000.0f) * feed_in_rate;
    }
    
    if (stats->consumed_wh_accumulator > 0) {
        // Consumption cost = energy x current buying rate
        stats->consumption_cost_accumulator += 
            (alt_supply->realtime_200ms.energy_increment_wh / 1000.0f) * tariff_rate;
    }
    
    // Value of self-consumed energy
    if (alt_supply->realtime_200ms.direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING) {
        // Value = avoided purchase cost for the self-consumed share
        float self_consumption_ratio = alt_supply->config.self_consumption_priority / 100.0f;
        stats->self_consumption_value_accumulator += 
            (alt_supply->realtime_200ms.energy_increment_wh / 1000.0f) * 
            tariff_rate * self_consumption_ratio;
    }
    
    // Tariff zone counters
    rbpower_tariff_zone_type_t current_zone;
    if (rbpower_tariff_get_current_zone(&current_zone, NULL, NULL) == RBPOWER_OK) {
        if (current_zone < RBPOWER_ALT_SUPPLY_TARIFF_ZONES) {
            stats->tariff_samples[current_zone]++;
        }
    }
}

/**
 * @brief Update the diagnostics block from the real-time values (simplified estimates).
 * @param[in,out] alt_supply  Structure to update (ignored if NULL).
 * @note Currently has no caller.
 */
static void update_alt_supply_diagnostics(rbpower_alternative_supply_t* alt_supply)
{
    if (!alt_supply) {
        return;
    }
    
    // Generating if output exceeds 50 W
    alt_supply->diagnostics.is_generating = 
        (alt_supply->realtime_200ms.instant_power_generating_w > 50.0f);
    
    // Grid connection (simplified: any state except OFFLINE)
    alt_supply->diagnostics.grid_connected = 
        (alt_supply->realtime_200ms.state != RBPOWER_ALT_STATE_OFFLINE);
    
    // Fault detection is not implemented
    alt_supply->diagnostics.has_active_fault = false;
    
    // Data quality
    alt_supply->diagnostics.data_quality_percent = 
        (alt_supply->realtime_200ms.samples_count >= 10) ? 100.0f : 
        (alt_supply->realtime_200ms.samples_count * 10.0f);
    
    // Production health (simplified estimate)
    if (alt_supply->type == RBPOWER_ALT_SOURCE_SOLAR_INVERTER) {
        // Solar: score based on efficiency
        if (alt_supply->realtime_200ms.efficiency_percent > 80.0f) {
            alt_supply->diagnostics.production_health_score = 100.0f;
        } else if (alt_supply->realtime_200ms.efficiency_percent > 60.0f) {
            alt_supply->diagnostics.production_health_score = 80.0f;
        } else {
            alt_supply->diagnostics.production_health_score = 60.0f;
        }
    } else {
        // Other sources: basic score
        alt_supply->diagnostics.production_health_score = 
            alt_supply->diagnostics.is_generating ? 100.0f : 50.0f;
    }
    
    // Annual yield forecast (simplified)
    if (alt_supply->production_analytics.lifetime_generating_hours > 0) {
        float avg_power = alt_supply->production_analytics.lifetime_generated_kwh / 
                         alt_supply->production_analytics.lifetime_generating_hours;
        // Assume 1500 sun hours per year for solar, 4000 h for other sources
        float annual_hours = (alt_supply->type == RBPOWER_ALT_SOURCE_SOLAR_INVERTER) ? 
                            1500.0f : 4000.0f;
        alt_supply->diagnostics.predicted_annual_yield_kwh = avg_power * annual_hours;
    }
}

//=============================================================================
// 3-SECOND PERIOD CALLBACKS
//=============================================================================

/*
 * Start a 3 s period (documented in rbhome_alt_supply.h).
 */
void alt_supply_start_3sec_period(rbpower_alternative_supply_t* alt_supply, uint64_t period_start_us)
{
    if (!alt_supply || !alt_supply->is_active) {
        return;
    }
    
    // Clear the accumulators (all fields before period_start_us)
    memset(&alt_supply->statistics_3sec, 0, 
           offsetof(rbpower_alt_supply_statistics_3sec_t, period_start_us));
    
    // Reset extremes
    alt_supply->statistics_3sec.power_max_generated = -FLT_MAX;
    alt_supply->statistics_3sec.power_max_consumed = -FLT_MAX;
    alt_supply->statistics_3sec.efficiency_min = FLT_MAX;
    alt_supply->statistics_3sec.efficiency_max = -FLT_MAX;
    
    // Period bounds
    alt_supply->statistics_3sec.period_start_us = period_start_us;
    alt_supply->statistics_3sec.period_end_us = period_start_us + 3000000; // +3 s
    alt_supply->statistics_3sec.period_complete = false;
    alt_supply->statistics_3sec.ready_for_finalization = false;
}

/*
 * Finalize a 3 s period (documented in rbhome_alt_supply.h).
 */
void alt_supply_finalize_3sec_period(rbpower_alternative_supply_t* alt_supply, uint64_t period_end_us)
{
    if (!alt_supply || !alt_supply->is_active) {
        return;
    }
    
    rbpower_alt_supply_statistics_3sec_t* stats = &alt_supply->statistics_3sec;
    rbgrid_snapshot_base_t* result3sec = &alt_supply->result_base_3sec;

    // Require at least one sample
    if (stats->electrical_samples_count == 0) {
        ESP_LOGW(TAG, "No samples for alt supply finalization");
        return;
    }
    
    // Finalize electrical values
    stats->current_rms_final = 
        sqrtf((float)(stats->current_rms_sum_squares / stats->electrical_samples_count));
    
    stats->power_active_avg_final = 
        (float)(stats->power_active_sum / stats->electrical_samples_count);
    
    // Finalize energy
    stats->energy_generated_final = (float)stats->generated_wh_accumulator;
    stats->energy_consumed_final = (float)stats->consumed_wh_accumulator;
    
    // Finalize efficiency
    if (stats->dc_samples_count > 0) {
        stats->efficiency_avg_final = 
            (float)(stats->efficiency_sum / stats->dc_samples_count);
    } else {
        stats->efficiency_avg_final = 0.0f;
    }
    
    // Availability over the period
    uint32_t total_samples = stats->generating_samples + stats->consuming_samples + stats->idle_samples;
    if (total_samples > 0) {
        stats->availability_percent = 
            ((float)(stats->generating_samples + stats->consuming_samples) / total_samples) * 100.0f;
    }
    
    // Finalize economics
    stats->generation_revenue_final = (float)stats->generation_revenue_accumulator;
    stats->consumption_cost_final = (float)stats->consumption_cost_accumulator;
    stats->net_benefit_final = stats->generation_revenue_final - stats->consumption_cost_final;
    
    // Update period bounds
    stats->period_end_us = period_end_us;
    stats->period_complete = true;
    
    // Update production analytics
    update_alt_supply_production_analytics(alt_supply);
    
    // === 3-SECOND ECONOMICS ===
    rbpower_alt_supply_economics_3sec_t* eco = &alt_supply->period_economics.economics_3sec;
    
    // Energy of the period
    eco->energy_generated_wh = stats->energy_generated_final;
    eco->energy_consumed_wh = stats->energy_consumed_final;
    eco->net_production_wh = eco->energy_generated_wh - eco->energy_consumed_wh;
    
    // Direction = the direction with the most samples
    if (stats->generating_samples > stats->consuming_samples) {
        eco->direction = RBPOWER_CURRENT_DIRECTION_SUPPLYING;
    } else if (stats->consuming_samples > stats->generating_samples) {
        eco->direction = RBPOWER_CURRENT_DIRECTION_CONSUMING;
    } else if (stats->idle_samples > 0) {
        eco->direction = RBPOWER_CURRENT_DIRECTION_ZERO;
    } else {
        eco->direction = RBPOWER_CURRENT_DIRECTION_UNKNOWN;
    }
    
    // Current tariff zone and rate
    rbpower_tariff_zone_type_t current_zone;
    float current_rate;
    if (rbpower_tariff_get_current_zone(&current_zone, NULL, &current_rate) == RBPOWER_OK) {
        eco->current_zone = current_zone;
        eco->current_tariff_rate = current_rate;
    }
    
    // Economics of the 3 s period
    eco->instant_generation_revenue = stats->generation_revenue_final;
    eco->instant_consumption_cost = stats->consumption_cost_final;
    
    // Generation (operating) cost
    if (stats->energy_generated_final > 0) {
        // Rough per-type generation cost
        float generation_cost_per_kwh = 0.01f; // Default 0.01 currency units per kWh
        if (alt_supply->type == RBPOWER_ALT_SOURCE_SOLAR_INVERTER) {
            generation_cost_per_kwh = 0.005f; // Solar: lower cost
        } else if (alt_supply->type == RBPOWER_ALT_SOURCE_GENERATOR) {
            generation_cost_per_kwh = 0.15f; // Fuel generator: higher cost
        }
        eco->instant_generation_cost = 
            (stats->energy_generated_final / 1000.0f) * generation_cost_per_kwh;
    } else {
        eco->instant_generation_cost = 0.0f;
    }

    result3sec->cost_consumed = eco->instant_consumption_cost;
    result3sec->revenue_supplied = eco->instant_generation_revenue;
    result3sec->avg_tariff_per_kwh = eco->current_tariff_rate;
    result3sec->energy_consumed_wh = eco->energy_consumed_wh;
    result3sec->energy_supplied_wh = eco->energy_generated_wh;
    result3sec->direction = eco->direction;
    result3sec->period_end_us = period_end_us;
    //result3sec->result_base_3sec.period_start_us = main_supply->created_timestamp_us;
    result3sec->samples_count = stats->electrical_samples_count;
    result3sec->data_valid = true;

    /*
    ESP_LOGI(TAG, "Alt Supply 3sec finalized: I=%.2fA, P=%.1fW, Gen=%.2fWh, Cons=%.2fWh, Eff=%.1f%%, Rev=%.4f€, Tariff=%.4f€/kWh",
             stats->current_rms_final,
             stats->power_active_avg_final,
             stats->energy_generated_final,
             stats->energy_consumed_final,
             stats->efficiency_avg_final,
             stats->generation_revenue_final,
             eco->current_tariff_rate);
    
    // Debug log of the Alt Supply economics
    if (stats->energy_generated_final > 0 || stats->energy_consumed_final > 0) {
        ESP_LOGI(TAG, "Alt Supply Economics: Gen=%.3fWh, GenRev=%.6f€, Cons=%.3fWh, ConsCost=%.6f€, NetBenefit=%.6f€",
                 stats->energy_generated_final,
                 stats->generation_revenue_final,
                 stats->energy_consumed_final, 
                 stats->consumption_cost_final,
                 stats->net_benefit_final);
    }
    */     
}

/**
 * @brief Update the cumulative production analytics from the finalized 3 s period.
 *
 * Lifetime energy, operating hours, a smoothed efficiency, a degradation estimate and the
 * solar specific yield / performance ratio (simplified, assumes a 20 % capacity factor).
 *
 * @param[in,out] alt_supply  Structure to update; must not be NULL.
 */
static void update_alt_supply_production_analytics(rbpower_alternative_supply_t* alt_supply)
{
    rbpower_alt_supply_production_analytics_t* analytics = &alt_supply->production_analytics;
    rbpower_alt_supply_statistics_3sec_t* stats = &alt_supply->statistics_3sec;
    
    // Lifetime production
    analytics->lifetime_generated_kwh += stats->energy_generated_final / 1000.0f;
    analytics->lifetime_consumed_kwh += stats->energy_consumed_final / 1000.0f;
    analytics->lifetime_net_production_kwh = 
        analytics->lifetime_generated_kwh - analytics->lifetime_consumed_kwh;
    
    // Operating hours
    if (stats->generating_samples > 0 || stats->consuming_samples > 0) {
        analytics->lifetime_operating_hours += 3.0f / 3600.0f; // 3 s in hours
    }
    if (stats->generating_samples > 0) {
        analytics->lifetime_generating_hours += 3.0f / 3600.0f;
    }
    
    // Current efficiency
    if (stats->efficiency_avg_final > 0) {
        // Exponential moving average for smoothing
        analytics->current_efficiency = 
            analytics->current_efficiency * 0.95f + stats->efficiency_avg_final * 0.05f;
    }
    
    // Degradation estimate (simplified)
    if (analytics->lifetime_operating_hours > 100) { // After 100 operating hours
        float hours_per_year = 8760.0f;
        float years_operated = analytics->lifetime_operating_hours / hours_per_year;
        if (years_operated > 0.1f) { // After 0.1 year
            float efficiency_loss = analytics->initial_efficiency - analytics->current_efficiency;
            analytics->annual_degradation_rate = efficiency_loss / years_operated;
            analytics->capacity_retention_percent = 
                (analytics->current_efficiency / analytics->initial_efficiency) * 100.0f;
        }
    }
    
    // YTD/MTD production metrics are not updated here
    
    // Best daily production
    // TODO: implement when a day-period finalize exists
    
    // Source-specific analytics
    if (alt_supply->type == RBPOWER_ALT_SOURCE_SOLAR_INVERTER) {
        // Specific yield
        if (alt_supply->config.specific_config.solar.panel_capacity_kwp > 0) {
            analytics->specific_analytics.solar.specific_yield_kwh_kwp = 
                analytics->lifetime_generated_kwh / 
                alt_supply->config.specific_config.solar.panel_capacity_kwp;
        }
        
        // Performance ratio (simplified)
        if (analytics->lifetime_operating_hours > 0) {
            float expected_production = 
                alt_supply->config.specific_config.solar.panel_capacity_kwp * 
                analytics->lifetime_operating_hours * 0.2f; // Assume a 20 % capacity factor
            
            analytics->specific_analytics.solar.performance_ratio_lifetime = 
                (analytics->lifetime_generated_kwh / expected_production) * 100.0f;
        }
    }
}

//=============================================================================
// PERIOD AGGREGATION
//=============================================================================

/*
 * Aggregate 3 s -> 15 min (documented in rbhome_alt_supply.h).
 */
void aggregate_alt_supply_15min_from_3sec(rbpower_alternative_supply_t* alt_supply)
{
    if (!alt_supply || !alt_supply->statistics_3sec.period_complete) {
        return;
    }
    
    rbpower_alt_supply_accumulator_15min_t* acc = &alt_supply->period_accumulators.accumulator_15min;
    rbpower_alt_supply_statistics_3sec_t* src = &alt_supply->statistics_3sec;
    
    // First 3 s period of a new 15 min period
    if (!acc->is_active) {
        acc->is_active = true;
        acc->period_start_us = src->period_start_us;
        acc->input_3sec_periods = 0;
    }
    
    // Aggregate electrical values
    acc->current_rms_sum_squares += src->current_rms_sum_squares;
    acc->power_active_sum += src->power_active_sum;
    acc->dc_power_sum += src->dc_power_sum;
    acc->efficiency_sum += src->efficiency_sum;
    acc->samples_count += src->electrical_samples_count;
    
    // Aggregate energy
    acc->generated_wh_accumulator += src->generated_wh_accumulator;
    acc->consumed_wh_accumulator += src->consumed_wh_accumulator;
    
    // Production metrics
    if (src->availability_percent > 0) {
        acc->production_quality_sum += src->availability_percent;
    }
    if (src->power_max_generated > alt_supply->config.rated_power_kw * 900.0f) { // 90 % of rated power (W)
        acc->full_power_seconds += 3; // 3 s
    }
    if (src->generating_samples == 0) {
        acc->zero_production_seconds += 3;
    }
    
    // Economics accumulators
    acc->generation_revenue_accumulator += src->generation_revenue_accumulator;
    acc->consumption_cost_accumulator += src->consumption_cost_accumulator;
    acc->self_consumption_value_accumulator += src->self_consumption_value_accumulator;
    
    // Per-tariff-zone accumulation
    for (int i = 0; i < RBPOWER_ALT_SUPPLY_TARIFF_ZONES; i++) {
        if (src->tariff_samples[i] > 0) {
            // Split the net value across zones by sample share
            float zone_ratio = (float)src->tariff_samples[i] / src->electrical_samples_count;
            acc->cost_by_zone[i] += (src->generation_revenue_accumulator - 
                                     src->consumption_cost_accumulator) * zone_ratio;
            
            // Approximate time in the zone
            acc->minutes_in_zone[i] += (3.0f / 60.0f) * zone_ratio; // 3 s in minutes
        }
    }
    
    // Update extremes
    if (src->power_max_generated > acc->power_max) {
        acc->power_max = src->power_max_generated;
    }
    if (src->power_max_generated < acc->power_min && src->power_max_generated > 0) {
        acc->power_min = src->power_max_generated;
    }
    if (src->efficiency_max > acc->efficiency_max) {
        acc->efficiency_max = src->efficiency_max;
    }
    if (src->efficiency_min < acc->efficiency_min && src->efficiency_min < FLT_MAX) {
        acc->efficiency_min = src->efficiency_min;
    }
    
    // Count the period
    acc->input_3sec_periods++;
    
    // 15 min complete after 300 periods of 3 s (900 s)
    if (acc->input_3sec_periods >= 300) {
        finalize_alt_supply_15min_period(alt_supply);
    }
}

/**
 * @brief Finalize the 15-minute period into the ring buffer and roll it into the hour accumulator.
 * @param[in,out] alt_supply  Structure to update; must not be NULL.
 */
static void finalize_alt_supply_15min_period(rbpower_alternative_supply_t* alt_supply)
{
    rbpower_alt_supply_accumulator_15min_t* acc = &alt_supply->period_accumulators.accumulator_15min;
    
    if (!acc->is_active || acc->samples_count == 0) {
        return;
    }
    
    // Next ring buffer slot
    uint8_t next_index = (alt_supply->period_statistics.current_15min_index + 1) % 
                         RBPOWER_ALT_SUPPLY_15MIN_BUFFER_SIZE;
    
    rbpower_alt_supply_result_15min_stats_t* result = 
        &alt_supply->period_statistics.result_15min_periods[next_index];
    
    // Finalize electrical values
    result->power_active_avg = (float)(acc->power_active_sum / acc->samples_count);
    
    // Energy
    result->energy_generated_wh = (float)acc->generated_wh_accumulator;
    result->energy_consumed_wh = (float)acc->consumed_wh_accumulator;
    
    // Efficiency
    if (acc->samples_count > 0 && acc->efficiency_sum > 0) {
        result->efficiency_avg = (float)(acc->efficiency_sum / acc->samples_count);
    } else {
        result->efficiency_avg = 0.0f;
    }
    
    // Availability
    float total_seconds = acc->input_3sec_periods * 3.0f;
    if (total_seconds > 0) {
        result->availability_percent = 
            ((total_seconds - acc->zero_production_seconds) / total_seconds) * 100.0f;
    }
    
    // Capacity factor
    if (alt_supply->config.rated_power_kw > 0) {
        float theoretical_max_energy = alt_supply->config.rated_power_kw * 1000.0f * 0.25f; // 15 min = 0.25 h
        result->capacity_factor = (result->energy_generated_wh / theoretical_max_energy) * 100.0f;
    }
    
    // Economics
    result->generation_revenue = (float)acc->generation_revenue_accumulator;
    result->consumption_cost = (float)acc->consumption_cost_accumulator;
    result->net_benefit = result->generation_revenue - result->consumption_cost;
    
    // Dominant operating mode
    uint32_t max_mode_samples = 0;
    result->dominant_mode = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t mode_count = 0;
        // Per-mode counts over the 3 s periods are not kept;
        // simplified: use the current mode
        result->dominant_mode = alt_supply->realtime_200ms.mode;
    }
    
    // Dominant tariff zone
    float max_minutes = 0;
    result->dominant_tariff_zone = 0;
    for (int i = 0; i < RBPOWER_ALT_SUPPLY_TARIFF_ZONES; i++) {
        if (acc->minutes_in_zone[i] > max_minutes) {
            max_minutes = acc->minutes_in_zone[i];
            result->dominant_tariff_zone = i;
        }
    }
    
    // Metadata
    result->period_end_us = esp_timer_get_time();
    result->valid = true;
    
    // Advance the ring buffer index
    alt_supply->period_statistics.current_15min_index = next_index;
    
    // Roll into the hour accumulator
    aggregate_alt_supply_hour_from_15min(alt_supply);
    
    ESP_LOGI(TAG, "Alt Supply 15min finalized: P=%.1fW, Gen=%.1fWh, CF=%.1f%%, Eff=%.1f%%, Rev=%.3f€",
             result->power_active_avg,
             result->energy_generated_wh,
             result->capacity_factor,
             result->efficiency_avg,
             result->generation_revenue);
    
    // Reset the accumulator for the next period
    memset(acc, 0, sizeof(rbpower_alt_supply_accumulator_15min_t));
    acc->power_min = FLT_MAX;
    acc->power_max = -FLT_MAX;
    acc->efficiency_min = FLT_MAX;
    acc->efficiency_max = -FLT_MAX;
}

/*
 * Aggregate 15 min -> 1 h (documented in rbhome_alt_supply.h).
 */
void aggregate_alt_supply_hour_from_15min(rbpower_alternative_supply_t* alt_supply)
{
    // Newest finalized 15 min period
    rbpower_alt_supply_result_15min_stats_t* src = 
        &alt_supply->period_statistics.result_15min_periods[
            alt_supply->period_statistics.current_15min_index];
    
    if (!src->valid) {
        return;
    }
    
    rbpower_alt_supply_accumulator_hour_t* acc = &alt_supply->period_accumulators.accumulator_hour;
    
    // First 15 min period of a new hour
    if (!acc->is_active) {
        acc->is_active = true;
        acc->period_start_us = esp_timer_get_time() - 900000000; // -15 min
        acc->input_15min_periods = 0;
    }
    
    // Aggregate the main values
    // RMS values aggregate as squares
    acc->current_rms_sum_squares += powf(src->power_active_avg / 230.0f, 2) * 900; // Approximation: I = P / 230 V
    acc->power_active_sum += src->power_active_avg * 900; // 900 s in 15 min
    acc->efficiency_sum += src->efficiency_avg * 900;
    acc->samples_count += 900; // Samples (seconds) per 15 min
    
    // Aggregate energy
    acc->generated_wh_accumulator += src->energy_generated_wh;
    acc->consumed_wh_accumulator += src->energy_consumed_wh;
    acc->net_production_wh_accumulator += (src->energy_generated_wh - src->energy_consumed_wh);
    
    // Production metrics
    acc->capacity_factor_sum += src->capacity_factor;
    acc->availability_sum += src->availability_percent;
    
    // Economics accumulators
    acc->generation_revenue_accumulator += src->generation_revenue;
    acc->consumption_cost_accumulator += src->consumption_cost;
    
    // Operating modes
    if (src->dominant_mode < 10) {
        acc->mode_minutes[src->dominant_mode] += 15; // 15 min
    }
    
    // Update extremes
    if (src->power_active_avg > acc->power_max) {
        acc->power_max = src->power_active_avg;
    }
    if (src->efficiency_avg > acc->efficiency_max) {
        acc->efficiency_max = src->efficiency_avg;
    }
    
    // Count the period
    acc->input_15min_periods++;
    
    // Hour complete after 4 periods of 15 min
    if (acc->input_15min_periods >= 4) {
        finalize_alt_supply_hour_period(alt_supply);
    }
}

/**
 * @brief Finalize the hour into period_statistics.result_last_hour and pass it to the flash stub.
 * @param[in,out] alt_supply  Structure to update; must not be NULL.
 */
static void finalize_alt_supply_hour_period(rbpower_alternative_supply_t* alt_supply)
{
    rbpower_alt_supply_accumulator_hour_t* acc = &alt_supply->period_accumulators.accumulator_hour;
    rbpower_alt_supply_result_hour_stats_t* result = &alt_supply->period_statistics.result_last_hour;
    
    if (!acc->is_active || acc->samples_count == 0) {
        return;
    }
    
    // Finalize electrical values
    result->current_rms = sqrtf((float)(acc->current_rms_sum_squares / acc->samples_count));
    result->power_active_avg = (float)(acc->power_active_sum / acc->samples_count);
    
    // Use the voltage bus to derive the apparent power S = U x I (current_rms keeps the current, A)
    rbpower_voltage_bus_t* voltage_bus = NULL;
    rbpower_get_voltage_bus_data(&voltage_bus);
    
    if (voltage_bus && voltage_bus->statistics_3sec.period_complete) {
        float voltage_rms = voltage_bus->statistics_3sec.final.voltage_rms_final;
        float apparent_va = voltage_rms * result->current_rms;
        float q2 = apparent_va * apparent_va - result->power_active_avg * result->power_active_avg;
        result->power_reactive_avg = q2 > 0.0f ? sqrtf(q2) : 0.0f;
        result->power_factor_avg = apparent_va > 0.0f ? result->power_active_avg / apparent_va : 0.0f;
    }
    
    // DC power
    if (acc->dc_power_sum > 0) {
        result->dc_power_avg = (float)(acc->dc_power_sum / acc->samples_count);
    }
    
    // Energy
    result->generated_wh = (float)acc->generated_wh_accumulator;
    result->consumed_wh = (float)acc->consumed_wh_accumulator;
    result->net_production_wh = result->generated_wh - result->consumed_wh;
    
    // Production metrics
    result->efficiency_avg = (float)(acc->efficiency_sum / acc->samples_count);
    result->capacity_factor = (float)(acc->capacity_factor_sum / acc->input_15min_periods);
    result->availability_percent = (float)(acc->availability_sum / acc->input_15min_periods);
    result->start_stop_cycles = acc->start_stop_cycles;
    result->fault_events = acc->fault_events;
    
    // Operating modes
    result->minutes_generating = acc->mode_minutes[RBPOWER_ALT_MODE_GENERATING];
    result->minutes_standby = acc->mode_minutes[RBPOWER_ALT_MODE_STANDBY];
    result->minutes_offline = acc->mode_minutes[RBPOWER_ALT_MODE_OFF];
    
    // Dominant operating mode
    uint32_t max_minutes = 0;
    for (int i = 0; i < 10; i++) {
        if (acc->mode_minutes[i] > max_minutes) {
            max_minutes = acc->mode_minutes[i];
            result->dominant_mode = i;
        }
    }
    
    result->peak_power = acc->power_max;
    
    // Economics
    result->generation_revenue = (float)acc->generation_revenue_accumulator;
    result->consumption_cost = (float)acc->consumption_cost_accumulator;
    result->net_benefit = result->generation_revenue - result->consumption_cost;
    result->self_consumption_value = (float)acc->self_consumption_value_accumulator;
    result->grid_export_value = (float)acc->grid_export_value_accumulator;
    result->operational_cost = (float)acc->operational_cost_accumulator;
    
    // Source-specific values (the source accumulators are not fed, so these stay zero)
    if (alt_supply->type == RBPOWER_ALT_SOURCE_SOLAR_INVERTER) {
        // Solar values
        if (acc->specific.solar.irradiance_sum > 0 && acc->samples_count > 0) {
            result->specific.solar.avg_irradiance = 
                (float)(acc->specific.solar.irradiance_sum / acc->samples_count);
        }
        
        // Energy yield ratio
        if (alt_supply->config.specific_config.solar.panel_capacity_kwp > 0) {
            result->specific.solar.energy_yield_ratio = 
                result->generated_wh / 1000.0f / 
                alt_supply->config.specific_config.solar.panel_capacity_kwp;
        }
        
        // Performance ratio
        result->specific.solar.performance_ratio = result->efficiency_avg;
        
        if (acc->specific.solar.panel_temp_sum > 0 && acc->samples_count > 0) {
            result->specific.solar.avg_panel_temp = 
                (float)(acc->specific.solar.panel_temp_sum / acc->samples_count);
        }
    } else if (alt_supply->type == RBPOWER_ALT_SOURCE_BATTERY_STORAGE) {
        // Battery values
        if (acc->specific.battery.soc_sum > 0 && acc->samples_count > 0) {
            result->specific.battery.avg_soc = 
                (float)(acc->specific.battery.soc_sum / acc->samples_count);
        }
        result->specific.battery.charge_cycles = (float)acc->specific.battery.charge_cycles;
        
        // Round trip efficiency
        if (result->consumed_wh > 0) {
            result->specific.battery.round_trip_efficiency = 
                (result->generated_wh / result->consumed_wh) * 100.0f;
        }
        
        if (acc->specific.battery.depth_of_discharge_sum > 0 && 
            acc->specific.battery.charge_cycles > 0) {
            result->specific.battery.depth_of_discharge = 
                (float)(acc->specific.battery.depth_of_discharge_sum / 
                       acc->specific.battery.charge_cycles);
        }
    }
    
    // Metadata
    result->period_end_us = esp_timer_get_time();
    result->valid = true;
    result->written_to_flash = false;
    
    // Store in flash (stub)
    rbpower_alt_supply_save_hour_to_flash(result);
    
    ESP_LOGI(TAG, "Alt Supply hour finalized: Gen=%.1fWh, Eff=%.1f%%, CF=%.1f%%, Avail=%.1f%%, NetBenefit=%.3f€",
             result->generated_wh,
             result->efficiency_avg,
             result->capacity_factor,
             result->availability_percent,
             result->net_benefit);
    
    // Reset the accumulator for the next period
    memset(acc, 0, sizeof(rbpower_alt_supply_accumulator_hour_t));
    acc->power_max = -FLT_MAX;
    acc->efficiency_max = -FLT_MAX;
    acc->dc_voltage_max = -FLT_MAX;
    acc->dc_current_max = -FLT_MAX;
}

// The 15 min and 1 h finalize are triggered only by the 3 s counter path
// (aggregate_alt_supply_15min_from_3sec() at input_3sec_periods >= 300). A separate
// period-event dispatcher would finalize twice, so there is none.

//=============================================================================
// FLASH STORAGE STUB (hour; called from the live hour finalize)
//=============================================================================

/*
 * Store hourly statistics in flash - stub (documented in rbhome_alt_supply.h).
 */
esp_err_t rbpower_alt_supply_save_hour_to_flash(const rbpower_alt_supply_result_hour_stats_t* hour_stats)
{
    if (!hour_stats || !hour_stats->valid) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // TODO: implement the flash write
    ESP_LOGD(TAG, "Saving hour stats to flash (stub)");
    
    // Report the record as written
    ((rbpower_alt_supply_result_hour_stats_t*)hour_stats)->written_to_flash = true;
    
    if (g_alt_supply) {
        g_alt_supply->flash_write_cycles++;
    }
    
    return ESP_OK;
}

// Day/month flash save and hour load are not implemented: day and longer periods are
// recomputed from flash history.