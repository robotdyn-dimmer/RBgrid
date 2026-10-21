/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_voltage_bus.c
 * @brief Voltage Bus unit: a memory-lean specialised unit for voltage-only measurement.
 *
 * A Voltage Bus measures only voltage, frequency and voltage THD (no current, power
 * or energy). It uses a dedicated small structure (about 200 bytes instead of the
 * ~3.4 KB of a full measurement unit).
 *
 * All functions here read or write the shared @c g_home_network instance; they are
 * called from the rbpower processing task and are not internally locked.
 */

#include "rbhome_types.h"
#include "rbpower.h"
#include <string.h>
#include <math.h>
#include <float.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "VOLTAGE_BUS";

/** Global electrical network model (defined in the rbhome core). */
extern rbpower_electrical_network_t g_home_network;

//=============================================================================
// INITIALISATION AND CONFIGURATION
//=============================================================================

/* 
 * @brief Initialise a Voltage Bus structure with default thresholds.
 *
 * Clears the structure, sets identity, and derives default limits from the
 * nominal voltage: under/over-voltage at -10 % / +10 %, THD warning 5 %,
 * THD alarm 8 %, frequency window 49.5-50.5 Hz.
 *
 * @param[out] voltage_bus     Structure to initialise.
 * @param[in]  unit_id         Unit identifier.
 * @param[in]  name            Display name (truncated to the name field size).
 * @param[in]  nominal_voltage Nominal RMS voltage, V.
 * @retval ESP_OK              Initialised.
 * @retval ESP_ERR_INVALID_ARG @p voltage_bus or @p name is NULL.
 */
esp_err_t rbpower_voltage_bus_init(rbpower_voltage_bus_t* voltage_bus, 
                                   rbpower_unit_id_t unit_id,
                                   const char* name,
                                   float nominal_voltage)
{
    if (!voltage_bus || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear the structure
    memset(voltage_bus, 0, sizeof(rbpower_voltage_bus_t));
    
    // Identity
    voltage_bus->unit_id = unit_id;
    strncpy(voltage_bus->name, name, sizeof(voltage_bus->name) - 1);
    voltage_bus->is_active = true;
    
    // Default thresholds
    voltage_bus->thresholds.nominal_voltage_v = nominal_voltage;
    voltage_bus->thresholds.under_voltage_threshold_v = nominal_voltage * 0.9f;  // -10%
    voltage_bus->thresholds.over_voltage_threshold_v = nominal_voltage * 1.1f;   // +10%
    voltage_bus->thresholds.thd_warning_threshold = 5.0f;    // 5% THD warning
    voltage_bus->thresholds.thd_alarm_threshold = 8.0f;      // 8% THD alarm
    voltage_bus->thresholds.frequency_min_hz = 49.5f;        // Minimum frequency
    voltage_bus->thresholds.frequency_max_hz = 50.5f;        // Maximum frequency
    
    // Initialise extremes
    voltage_bus->statistics_3sec.voltage_min_3sec = FLT_MAX;
    voltage_bus->statistics_3sec.voltage_max_3sec = -FLT_MAX;
    
    // Metadata
    voltage_bus->created_timestamp_us = esp_timer_get_time();
    
    ESP_LOGI(TAG, "Voltage Bus '%s' initialized (ID: 0x%04X, Nominal: %.1fV)", 
             name, unit_id, nominal_voltage);
    
    return ESP_OK;
}

//=============================================================================
// 200 MS DATA UPDATE
//=============================================================================

//=============================================================================
// 3-SECOND PERIOD CALLBACKS
//=============================================================================

/* 
 * @brief Start a new 3-second statistics period for a Voltage Bus.
 *
 * Resets the RMS/frequency/THD accumulators and the min/max extremes and sets the
 * period bounds. Does nothing if @p voltage_bus is NULL or inactive.
 *
 * @param[in,out] voltage_bus     Voltage Bus to reset.
 * @param[in]     period_start_us Period start, esp_timer time in µs.
 */
void voltage_bus_start_3sec_period(rbpower_voltage_bus_t* voltage_bus, uint64_t period_start_us)
{
    if (!voltage_bus || !voltage_bus->is_active) {
//        ESP_LOGW(TAG, "voltage_bus_start_3sec_period: vbus=%p, active=%d", voltage_bus, voltage_bus ? voltage_bus->is_active : -1);
        return;
    }
    
//    ESP_LOGI(TAG, "Starting 3sec period: previous samples=%lu", voltage_bus->statistics_3sec.rms_samples_count);
    
    // Reset accumulators
    voltage_bus->statistics_3sec.voltage_rms_sum_squares = 0.0;
    voltage_bus->statistics_3sec.rms_samples_count = 0;
    voltage_bus->statistics_3sec.frequency_sum = 0.0;
    voltage_bus->statistics_3sec.frequency_samples_count = 0;
    voltage_bus->statistics_3sec.thd_voltage_sum_squares = 0.0;
    voltage_bus->statistics_3sec.thd_samples_count = 0;
    
    // Reset extremes
    voltage_bus->statistics_3sec.voltage_min_3sec = FLT_MAX;
    voltage_bus->statistics_3sec.voltage_max_3sec = -FLT_MAX;
    
    // Period bounds
    voltage_bus->statistics_3sec.period_start_us = period_start_us;
    voltage_bus->statistics_3sec.period_end_us = period_start_us + 3000000; // +3 seconds
    voltage_bus->statistics_3sec.period_complete = false;
    voltage_bus->statistics_3sec.ready_for_finalization = false;
}

/* 
 * @brief Finalise the current 3-second statistics period of a Voltage Bus.
 *
 * Computes the period RMS voltage (V) and THD (%) as root-mean-square of the
 * accumulated samples and the frequency (Hz) as an arithmetic mean, marks the
 * period complete and updates the uptime. Does nothing if the bus is NULL,
 * inactive, or has no samples in this period.
 *
 * @param[in,out] voltage_bus   Voltage Bus to finalise.
 * @param[in]     period_end_us Period end, esp_timer time in µs.
 */
void voltage_bus_finalize_3sec_period(rbpower_voltage_bus_t* voltage_bus, uint64_t period_end_us)
{
    if (!voltage_bus || !voltage_bus->is_active) {
//        ESP_LOGW(TAG, "voltage_bus_finalize_3sec_period: vbus=%p, active=%d", voltage_bus, voltage_bus ? voltage_bus->is_active : -1);
        return;
    }
    
//    ESP_LOGI(TAG, "Finalizing 3sec period: samples=%lu, unit_id=0x%04X", voltage_bus->statistics_3sec.rms_samples_count, voltage_bus->unit_id);
    
    // Check that samples exist
    if (voltage_bus->statistics_3sec.rms_samples_count == 0) {
        ESP_LOGD(TAG, "No samples for voltage bus finalization");
        // Extra debug output
        ESP_LOGD(TAG, "VBus state: is_active=%d, total_updates=%lu, unit_id=0x%04X", 
                 voltage_bus->is_active, voltage_bus->total_updates, voltage_bus->unit_id);
        return;
    }
    
    // Finalise RMS (root mean square)
    voltage_bus->statistics_3sec.final.voltage_rms_final = 
        sqrtf((float)(voltage_bus->statistics_3sec.voltage_rms_sum_squares / 
                     voltage_bus->statistics_3sec.rms_samples_count));
    
    // Finalise frequency (arithmetic mean)
    if (voltage_bus->statistics_3sec.frequency_samples_count > 0) {
        voltage_bus->statistics_3sec.final.frequency_avg_final = 
            (float)(voltage_bus->statistics_3sec.frequency_sum / 
                   voltage_bus->statistics_3sec.frequency_samples_count);
    }
    
    // Finalise THD (root mean square)
    if (voltage_bus->statistics_3sec.thd_samples_count > 0) {
        voltage_bus->statistics_3sec.final.thd_voltage_avg_final = 
            sqrtf((float)(voltage_bus->statistics_3sec.thd_voltage_sum_squares / 
                         voltage_bus->statistics_3sec.thd_samples_count));
    }
    
    // Update period bounds
    voltage_bus->statistics_3sec.period_end_us = period_end_us;
    voltage_bus->statistics_3sec.period_complete = true;
    voltage_bus->statistics_3sec.ready_for_finalization = false;
    
    // Update uptime
    uint64_t uptime_us = period_end_us - voltage_bus->created_timestamp_us;
    voltage_bus->uptime_hours = uptime_us / (1000000ULL * 3600ULL);
/*    
    ESP_LOGI(TAG, "Voltage Bus 3sec finalized: V=%.1f, F=%.2f, THD=%.1f%%",
             voltage_bus->statistics_3sec.final.voltage_rms_final,
             voltage_bus->statistics_3sec.final.frequency_avg_final,
             voltage_bus->statistics_3sec.final.thd_voltage_avg_final);
*/             
}

//=============================================================================
// API FUNCTIONS
//=============================================================================

/* 
 * @brief Get a pointer to the network's Voltage Bus.
 *
 * The returned pointer refers to the shared network model; it is valid for the
 * lifetime of the network and is not protected by a lock.
 *
 * @param[out] voltage_bus_ptr Receives a pointer to the Voltage Bus.
 * @retval ESP_OK              Pointer returned.
 * @retval ESP_ERR_INVALID_ARG @p voltage_bus_ptr is NULL.
 * @retval ESP_ERR_NOT_FOUND   No Voltage Bus is configured.
 */
esp_err_t rbpower_get_voltage_bus_data(rbpower_voltage_bus_t** voltage_bus_ptr)
{
    if (!voltage_bus_ptr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!g_home_network.specialized.has_voltage_bus) {
        return ESP_ERR_NOT_FOUND;
    }
    
    *voltage_bus_ptr = &g_home_network.specialized.voltage_bus;
    return ESP_OK;
}

/* 
 * @brief Get the latest 200 ms voltage-quality values of the Voltage Bus.
 *
 * Any output pointer may be NULL to skip that value.
 *
 * @param[out] voltage_rms RMS voltage, V (may be NULL).
 * @param[out] thd_percent Voltage THD, % (may be NULL).
 * @param[out] quality_ok  true if voltage, frequency and THD are all within limits (may be NULL).
 * @retval ESP_OK            Values returned.
 * @retval ESP_ERR_NOT_FOUND No Voltage Bus is configured.
 */
esp_err_t rbpower_get_voltage_quality(float* voltage_rms, float* thd_percent, bool* quality_ok)
{
    if (!g_home_network.specialized.has_voltage_bus) {
        return ESP_ERR_NOT_FOUND;
    }
    
    rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
    
    if (voltage_rms) {
        *voltage_rms = vbus->realtime_200ms.voltage_rms;
    }
    
    if (thd_percent) {
        *thd_percent = vbus->realtime_200ms.thd_voltage_percent;
    }
    
    if (quality_ok) {
        *quality_ok = vbus->diagnostics.voltage_normal && 
                     vbus->diagnostics.frequency_normal && 
                     vbus->diagnostics.thd_normal;
    }
    
    return ESP_OK;
}

/* 
 * @brief Get the last completed 3-second statistics of the Voltage Bus.
 *
 * Any output pointer may be NULL to skip that value.
 *
 * @param[out] voltage_rms_avg Period RMS voltage, V (may be NULL).
 * @param[out] frequency_avg   Period mean frequency, Hz (may be NULL).
 * @param[out] voltage_min     Period minimum voltage, V (may be NULL).
 * @param[out] voltage_max     Period maximum voltage, V (may be NULL).
 * @retval ESP_OK               Values returned.
 * @retval ESP_ERR_NOT_FOUND    No Voltage Bus is configured.
 * @retval ESP_ERR_NOT_FINISHED The current 3-second period has not been finalised yet.
 */
esp_err_t rbpower_get_voltage_bus_3sec_stats(float* voltage_rms_avg, 
                                            float* frequency_avg,
                                            float* voltage_min,
                                            float* voltage_max)
{
    if (!g_home_network.specialized.has_voltage_bus) {
        return ESP_ERR_NOT_FOUND;
    }
    
    rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
    
    if (!vbus->statistics_3sec.period_complete) {
        return ESP_ERR_NOT_FINISHED;
    }
    
    if (voltage_rms_avg) {
        *voltage_rms_avg = vbus->statistics_3sec.final.voltage_rms_final;
    }
    
    if (frequency_avg) {
        *frequency_avg = vbus->statistics_3sec.final.frequency_avg_final;
    }
    
    if (voltage_min) {
        *voltage_min = vbus->statistics_3sec.voltage_min_3sec;
    }
    
    if (voltage_max) {
        *voltage_max = vbus->statistics_3sec.voltage_max_3sec;
    }
    
    return ESP_OK;
}

/**
 * @brief Log Voltage Bus diagnostics (identity, 200 ms values, status, events, 3 s stats).
 */
void rbpower_voltage_bus_print_diagnostics(void)
{
    if (!g_home_network.specialized.has_voltage_bus) {
        ESP_LOGW(TAG, "No voltage bus configured");
        return;
    }
    
    rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
    
    ESP_LOGI(TAG, "=== VOLTAGE BUS DIAGNOSTICS ===");
    ESP_LOGI(TAG, "Name: %s, ID: 0x%04X, Active: %s", 
             vbus->name, vbus->unit_id, vbus->is_active ? "YES" : "NO");
    
    ESP_LOGI(TAG, "Real-time 200ms: V=%.1fV (%.1f-%.1f), F=%.2fHz, THD=%.1f%%",
             vbus->realtime_200ms.voltage_rms,
             vbus->realtime_200ms.voltage_min,
             vbus->realtime_200ms.voltage_max,
             vbus->realtime_200ms.frequency,
             vbus->realtime_200ms.thd_voltage_percent);
    
    ESP_LOGI(TAG, "Status: Voltage=%s, Frequency=%s, THD=%s",
             vbus->diagnostics.voltage_normal ? "OK" : "FAULT",
             vbus->diagnostics.frequency_normal ? "OK" : "FAULT",
             vbus->diagnostics.thd_normal ? "OK" : "WARNING");
    
    ESP_LOGI(TAG, "Events: Voltage=%lu, Frequency=%lu, Quality=%lu",
             vbus->diagnostics.voltage_events_count,
             vbus->diagnostics.frequency_events_count,
             vbus->diagnostics.quality_alerts_count);
    
    if (vbus->statistics_3sec.period_complete) {
        ESP_LOGI(TAG, "3-sec stats: V=%.1fV (%.1f-%.1f), F=%.2fHz, THD=%.1f%%",
                 vbus->statistics_3sec.final.voltage_rms_final,
                 vbus->statistics_3sec.voltage_min_3sec,
                 vbus->statistics_3sec.voltage_max_3sec,
                 vbus->statistics_3sec.final.frequency_avg_final,
                 vbus->statistics_3sec.final.thd_voltage_avg_final);
    }
    
    ESP_LOGI(TAG, "Updates: %lu, Uptime: %llu hours", 
             vbus->total_updates, vbus->uptime_hours);
    ESP_LOGI(TAG, "Memory usage: ~%d bytes", sizeof(rbpower_voltage_bus_t));
}