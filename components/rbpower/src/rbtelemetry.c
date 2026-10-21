/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbtelemetry.c
 * @brief rbpower telemetry implementation (see rbtelemetry.h).
 */

#include "rbpower.h"
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbhome_types.h"
#include "rbhome_tariff.h"
#include "rbtelemetry.h"
#include "rbwebsocket.h"
#include "rbmqtt.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "RBPOWER_TELEMETRY";

/* ================================================================================================
 * HELPER FUNCTIONS: ENUM TO STRING
 * ================================================================================================ */

static const char* rbpower_metric_type_to_string(rbpower_metric_type_t type) {
    switch(type) {
        case RBPOWER_METRIC_VOLTAGE_BUS: return "voltage_bus";
        case RBPOWER_METRIC_CURRENT_LOAD: return "current_load";
        case RBPOWER_METRIC_POWER_LOAD: return "power_load";
        case RBPOWER_METRIC_ENERGY_COUNTER: return "energy_counter";
        case RBPOWER_METRIC_FREQUENCY: return "frequency";
        case RBPOWER_METRIC_POWER_FACTOR: return "power_factor";
        case RBPOWER_METRIC_COMPOSITE: return "composite";
        default: return "unknown";
    }
}

static const char* rbpower_load_role_to_string(rbpower_load_role_t role) {
    switch(role) {
        case RBPOWER_ROLE_MAIN_SUPPLY: return "main_supply";
        case RBPOWER_ROLE_CIRCUIT_BREAKER: return "circuit_breaker";
        case RBPOWER_ROLE_DEVICE_LOAD: return "device_load";
        case RBPOWER_ROLE_ZONE_LOAD: return "zone_load";
        case RBPOWER_ROLE_RENEWABLE_SOURCE: return "renewable_source";
        case RBPOWER_ROLE_STORAGE_SYSTEM: return "storage_system";
        case RBPOWER_ROLE_EV_CHARGING: return "ev_charging";
        case RBPOWER_ROLE_HEATING_COOLING: return "heating_cooling";
        case RBPOWER_ROLE_SMART_APPLIANCE: return "smart_appliance";
        default: return "unknown";
    }
}

/* ================================================================================================
 * GLOBAL VARIABLES
 * ================================================================================================ */
// No extern access to the network: the rbpower API functions are used instead
static rbpower_telemetry_config_t g_telemetry_config = {0};
static bool g_telemetry_initialized = false;
static char* g_json_buffer = NULL;

// Latest telemetry packet (legacy packet path)
static rbpower_telemetry_packet_t g_latest_telemetry = {0};
static bool g_has_telemetry_data = false;
static SemaphoreHandle_t g_telemetry_mutex = NULL;

// WebSocket telemetry enable flag (driven by client connect/disconnect)
static bool g_websocket_telemetry_enabled = false;

/* ================================================================================================
 * PROTOCOL-SPECIFIC SEND FUNCTIONS (MQTT and WebSocket are real; HTTP only logs)
 * ================================================================================================ */

static esp_err_t send_mqtt(const char* json_string) {
    const rbpower_telemetry_channel_t* ch = &g_telemetry_config.channels[RBPOWER_TELEMETRY_MQTT];
    if (!ch->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rbmqtt_is_connected()) {
        // Broker not connected yet — skip (no fake "Send" log); WS path is unaffected.
        ESP_LOGD(TAG, "MQTT not connected, skipping publish");
        return ESP_ERR_INVALID_STATE;
    }
    const char* topic = ch->config.mqtt.topic ? ch->config.mqtt.topic : "rbgrid/telemetry";
    esp_err_t err = rbmqtt_publish_nb(topic, json_string, ch->config.mqtt.qos, false);   /* esp_timer context: never block */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT publish to '%s' failed: %s", topic, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t send_http(const char* json_string) {
    if (!g_telemetry_config.channels[RBPOWER_TELEMETRY_HTTP].enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "HTTP POST to '%s': %s", 
             g_telemetry_config.channels[RBPOWER_TELEMETRY_HTTP].config.http.endpoint_url, 
             json_string);
    
    // TODO: real HTTP send
    
    return ESP_OK;
}

static esp_err_t send_websocket2(const char* json_string, const char* json_v2) {
    // Check that the WebSocket channel is enabled and that clients have enabled it
    if (!g_telemetry_config.channels[RBPOWER_TELEMETRY_WEBSOCKET].enabled || !g_websocket_telemetry_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Use the dedicated telemetry broadcast function
    extern esp_err_t broadcast_to_telemetry_clients2(const char* message, const char* message2);   /* v2 → ?v=2 clients */
    esp_err_t ret = broadcast_to_telemetry_clients2(json_string, json_v2);
    
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "WebSocket telemetry broadcast successful");
    } else if (ret == ESP_FAIL) {
        ESP_LOGD(TAG, "WebSocket telemetry broadcast failed - no active clients");
    } else {
        ESP_LOGE(TAG, "WebSocket telemetry broadcast error: %s", esp_err_to_name(ret));
    }
    
    return ret;
}
static esp_err_t send_websocket(const char* json_string) { return send_websocket2(json_string, NULL); }

/* ================================================================================================
 * INTERNAL FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Send JSON through all active channels
 */
static esp_err_t send_to_active_channels(const char* json_string) {
    esp_err_t result = ESP_OK;
    esp_err_t channel_result;
    
    // MQTT
    channel_result = send_mqtt(json_string);
    if (channel_result != ESP_OK && channel_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "MQTT send failed: %s", esp_err_to_name(channel_result));
        result = channel_result;
    }
    
    // HTTP
    channel_result = send_http(json_string);
    if (channel_result != ESP_OK && channel_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "HTTP send failed: %s", esp_err_to_name(channel_result));
        result = channel_result;
    }
    
    // WebSocket
    channel_result = send_websocket(json_string);
    if (channel_result != ESP_OK && channel_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "WebSocket send failed: %s", esp_err_to_name(channel_result));
        result = channel_result;
    }
    
    return result;
}

/* ================================================================================================
 * PUBLIC FUNCTIONS
 * ================================================================================================ */

esp_err_t rbpower_telemetry_init(const rbpower_telemetry_config_t* config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_telemetry_initialized) {
        ESP_LOGW(TAG, "Telemetry already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Copy the configuration
    memcpy(&g_telemetry_config, config, sizeof(rbpower_telemetry_config_t));
    
    // Allocate the JSON buffer
    g_json_buffer = malloc(g_telemetry_config.json_buffer_size);
    if (!g_json_buffer) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        return ESP_ERR_NO_MEM;
    }
    
    // Create the mutex that protects the telemetry packet
    g_telemetry_mutex = xSemaphoreCreateMutex();
    if (!g_telemetry_mutex) {
        ESP_LOGE(TAG, "Failed to create telemetry mutex");
        free(g_json_buffer);
        g_json_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    g_telemetry_initialized = true;
    
    ESP_LOGI(TAG, "Telemetry initialized. Buffer size: %lu bytes", 
             g_telemetry_config.json_buffer_size);
    
    // Log the channel states
    for (int i = 0; i < RBPOWER_TELEMETRY_PROTOCOLS_MAX; i++) {
        const char* protocol_names[] = {"MQTT", "HTTP", "WebSocket"};
        ESP_LOGI(TAG, "%s channel: %s", 
                 protocol_names[i], 
                 g_telemetry_config.channels[i].enabled ? "ENABLED" : "DISABLED");
    }
    
    return ESP_OK;
}

void rbpower_telemetry_deinit(void) {
    if (g_json_buffer) {
        free(g_json_buffer);
        g_json_buffer = NULL;
    }
    
    if (g_telemetry_mutex) {
        vSemaphoreDelete(g_telemetry_mutex);
        g_telemetry_mutex = NULL;
    }
    
    memset(&g_telemetry_config, 0, sizeof(rbpower_telemetry_config_t));
    g_telemetry_initialized = false;
    g_has_telemetry_data = false;
    
    ESP_LOGI(TAG, "Telemetry deinitialized");
}

esp_err_t rbpower_telemetry_create_json(const rbpower_telemetry_packet_t* packet, cJSON** json_out) {
    if (!packet || !json_out || !g_telemetry_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Access the home network
    rbpower_electrical_network_t* network;
    rbpower_err_t err = rbpower_get_network(&network);
    if (err != RBPOWER_OK || network == NULL) {
        ESP_LOGE(TAG, "Cannot get network for telemetry: %d", err);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Create the base JSON structure
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    
    // Base message fields
    cJSON_AddStringToObject(root, "type", "measurement_data");
    cJSON_AddNumberToObject(root, "timestamp_local", (double)esp_timer_get_time());
    
    // NTP timestamp if available
    uint64_t ntp_timestamp = rbtime_get_unix_timestamp();
    if (ntp_timestamp > 0) {
        cJSON_AddNumberToObject(root, "timestamp_ntp", (double)(ntp_timestamp * 1000000ULL));
    }
    
    cJSON_AddStringToObject(root, "version", "2.0");
    cJSON_AddStringToObject(root, "device_id", "rbgrid_main_001");
    
    // Create the data object
    cJSON* data_obj = cJSON_CreateObject();
    if (!data_obj) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    // === PERIOD_INFO ===
    cJSON* period_info = cJSON_CreateObject();
    if (!period_info) {
        cJSON_Delete(data_obj);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(period_info, "period_type", "3sec");
    
    // Find the first valid unit for the period timestamps
    uint64_t period_start_us = 0;
    uint64_t period_end_us = 0;
    uint32_t period_duration_ms = 0;
    uint8_t valid_units_count = 0;
    
    for (uint8_t i = 0; i < packet->units_count; i++) {
        if (packet->units[i].data_valid && packet->units[i].stats) {
            if (period_start_us == 0) {
                period_start_us = packet->units[i].stats->period_start_us;
                period_end_us = packet->units[i].stats->period_end_us;
                period_duration_ms = packet->units[i].stats->period_duration_ms;
            }
            valid_units_count++;
        }
    }
    
    cJSON_AddNumberToObject(period_info, "period_duration_ms", period_duration_ms);
    cJSON_AddNumberToObject(period_info, "period_start_us", (double)period_start_us);
    cJSON_AddNumberToObject(period_info, "period_end_us", (double)period_end_us);
    cJSON_AddNumberToObject(period_info, "units_count", packet->units_count);
    cJSON_AddNumberToObject(period_info, "valid_units_count", valid_units_count);
    
    cJSON_AddItemToObject(data_obj, "period_info", period_info);
    
    // === SYSTEM_TOTALS ===
    cJSON* system_totals = cJSON_CreateObject();
    if (!system_totals) {
        cJSON_Delete(data_obj);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    // Accumulators for the system totals
    float total_power_active = 0.0f;
    float total_energy_consumed = 0.0f;
    float total_energy_supplied = 0.0f;
    double total_cost_consumed = 0.0;
    double total_revenue_supplied = 0.0;
    double system_net_cost = 0.0;
    float frequency_sum = 0.0f;
    uint32_t frequency_count = 0;
    uint32_t completeness_sum = 0;
    uint32_t completeness_count = 0;
    
    // === UNITS ARRAY ===
    cJSON* units_array = cJSON_CreateArray();
    if (!units_array) {
        cJSON_Delete(system_totals);
        cJSON_Delete(data_obj);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    // Process each unit
    for (uint8_t i = 0; i < packet->units_count; i++) {
        const rbpower_telemetry_unit_data_t* unit_data = &packet->units[i];
        uint8_t unit_id = unit_data->unit_id;
        
        // Check that the unit is valid in the network
        if (unit_id >= network->units_count || !network->units[unit_id].has_valid_definition) {
            continue;
        }
        
        const rbpower_home_unit_t* home_unit = &network->units[unit_id];
        const rbpower_period_statistics_t* stats = unit_data->stats;
        
        cJSON* unit_obj = cJSON_CreateObject();
        if (!unit_obj) {
            continue;
        }
        
        // Main unit data
        cJSON_AddNumberToObject(unit_obj, "unit_id", unit_id);
        cJSON_AddStringToObject(unit_obj, "unit_name", home_unit->definition.name);
        cJSON_AddStringToObject(unit_obj, "unit_location", home_unit->definition.location);
        cJSON_AddStringToObject(unit_obj, "unit_type", rbpower_metric_type_to_string(home_unit->definition.type));
        cJSON_AddStringToObject(unit_obj, "unit_role", rbpower_load_role_to_string(home_unit->definition.role));
        cJSON_AddBoolToObject(unit_obj, "is_active", home_unit->definition.is_active);
        cJSON_AddBoolToObject(unit_obj, "data_valid", unit_data->data_valid);
        cJSON_AddBoolToObject(unit_obj, "statistics_enabled", home_unit->has_statistics_enabled);
        cJSON_AddBoolToObject(unit_obj, "period_complete", stats ? stats->period_complete : false);
        
        if (stats && unit_data->data_valid) {
            // === ELECTRICAL ===
            cJSON* electrical = cJSON_CreateObject();
            cJSON_AddNumberToObject(electrical, "voltage_rms", stats->voltage_rms);
            cJSON_AddNumberToObject(electrical, "current_rms", stats->current_rms);
            cJSON_AddNumberToObject(electrical, "power_active", stats->power_active_avg);
            cJSON_AddNumberToObject(electrical, "power_reactive", stats->power_reactive_avg);
            cJSON_AddNumberToObject(electrical, "power_apparent", stats->power_apparent_avg);
            cJSON_AddNumberToObject(electrical, "power_factor", stats->power_factor_avg);
            cJSON_AddNumberToObject(electrical, "frequency", stats->frequency_avg);
            cJSON_AddItemToObject(unit_obj, "electrical", electrical);
            
            // === ENERGY ===
            cJSON* energy = cJSON_CreateObject();
            cJSON_AddNumberToObject(energy, "consumed_wh", stats->energy_period.consumed_wh);
            cJSON_AddNumberToObject(energy, "supplied_wh", stats->energy_period.supplied_wh);
            cJSON_AddNumberToObject(energy, "net_wh", stats->energy_period.net_wh);
            cJSON_AddNumberToObject(energy, "consumed_kwh", stats->energy_period.consumed_wh / 1000.0f);
            cJSON_AddNumberToObject(energy, "supplied_kwh", stats->energy_period.supplied_wh / 1000.0f);
            cJSON_AddNumberToObject(energy, "net_kwh", stats->energy_period.net_wh / 1000.0f);
            cJSON_AddItemToObject(unit_obj, "energy", energy);
            
            // === EXTREMES ===
            cJSON* extremes = cJSON_CreateObject();
            cJSON_AddNumberToObject(extremes, "voltage_min", stats->extremes.voltage_min);
            cJSON_AddNumberToObject(extremes, "voltage_max", stats->extremes.voltage_max);
            cJSON_AddNumberToObject(extremes, "current_min", stats->extremes.current_min);
            cJSON_AddNumberToObject(extremes, "current_max", stats->extremes.current_max);
            cJSON_AddNumberToObject(extremes, "power_min", stats->extremes.power_min);
            cJSON_AddNumberToObject(extremes, "power_max", stats->extremes.power_max);
            cJSON_AddNumberToObject(extremes, "power_peak_time_us", (double)stats->extremes.power_peak_time_us);
            cJSON_AddItemToObject(unit_obj, "extremes", extremes);
            
            // === ECONOMICS ===
            cJSON* economics = cJSON_CreateObject();
            cJSON_AddNumberToObject(economics, "cost_consumed", stats->result_base.cost_consumed);
            cJSON_AddNumberToObject(economics, "revenue_supplied", stats->result_base.revenue_supplied);
            cJSON_AddNumberToObject(economics, "net_cost", (stats->result_base.cost_consumed - stats->result_base.revenue_supplied));
            cJSON_AddNumberToObject(economics, "avg_tariff_rate", stats->result_base.avg_tariff_per_kwh);

            // Current tariff rate
            float current_tariff_rate = rbpower_get_current_tariff_rate_simple();
            cJSON_AddNumberToObject(economics, "current_tariff_rate", current_tariff_rate);

            // Cost for this period
            double period_cost = 0.0;
            if (stats->energy_period.consumed_wh > 0) {
                period_cost = (stats->energy_period.consumed_wh / 1000.0f) * current_tariff_rate;
            }
            cJSON_AddNumberToObject(economics, "cost_per_period", period_cost);
            cJSON_AddNumberToObject(economics, "samples_count", stats->result_base.samples_count);
            cJSON_AddItemToObject(unit_obj, "economics", economics);

            // === QUALITY ===
            cJSON* quality = cJSON_CreateObject();
            cJSON_AddNumberToObject(quality, "samples_total", stats->samples_count);
            cJSON_AddNumberToObject(quality, "samples_valid", stats->valid_samples_count);
            cJSON_AddNumberToObject(quality, "completeness_percent", stats->data_completeness_percent);
            
            // Data age
            uint64_t current_time = esp_timer_get_time();
            uint32_t data_age_ms = (current_time - stats->last_update_us) / 1000;
            cJSON_AddNumberToObject(quality, "last_update_age_ms", data_age_ms);
            cJSON_AddItemToObject(unit_obj, "quality", quality);
            
            // Accumulate into system_totals
            total_power_active += stats->power_active_avg;
            total_energy_consumed += stats->energy_period.consumed_wh;
            total_energy_supplied += stats->energy_period.supplied_wh;
            total_cost_consumed += stats->result_base.cost_consumed;
            total_revenue_supplied += stats->result_base.revenue_supplied;

            if (stats->frequency_avg > 0) {
                frequency_sum += stats->frequency_avg;
                frequency_count++;
            }
            
            completeness_sum += stats->data_completeness_percent;
            completeness_count++;
        }
        
        cJSON_AddItemToArray(units_array, unit_obj);
    }
    
    // Fill system_totals
    cJSON_AddNumberToObject(system_totals, "total_power_active", total_power_active);
    cJSON_AddNumberToObject(system_totals, "total_energy_consumed", total_energy_consumed);
    cJSON_AddNumberToObject(system_totals, "total_energy_supplied", total_energy_supplied);
    cJSON_AddNumberToObject(system_totals, "total_cost_consumed", total_cost_consumed);
    cJSON_AddNumberToObject(system_totals, "total_revenue_supplied", total_revenue_supplied);
    system_net_cost = total_cost_consumed - total_revenue_supplied;
    cJSON_AddNumberToObject(system_totals, "system_net_cost", system_net_cost);
    
    float avg_frequency = (frequency_count > 0) ? (frequency_sum / frequency_count) : 0.0f;
    cJSON_AddNumberToObject(system_totals, "avg_system_frequency", avg_frequency);
    
    uint8_t avg_completeness = (completeness_count > 0) ? (completeness_sum / completeness_count) : 0;
    cJSON_AddNumberToObject(system_totals, "data_completeness_avg", avg_completeness);
    
    cJSON_AddItemToObject(data_obj, "system_totals", system_totals);
    cJSON_AddItemToObject(data_obj, "units", units_array);
    cJSON_AddItemToObject(root, "data", data_obj);
    
    *json_out = root;
    return ESP_OK;
}

esp_err_t rbpower_telemetry_send_packet(const rbpower_telemetry_packet_t* packet) {
    if (!packet || !g_telemetry_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!g_telemetry_config.telemetry_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Build the JSON
    cJSON* json_obj = NULL;
    esp_err_t result = rbpower_telemetry_create_json(packet, &json_obj);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JSON: %s", esp_err_to_name(result));
        return result;
    }
    
    // Convert to a string
    char* json_string;
    if (g_telemetry_config.compact_json) {
        json_string = cJSON_PrintUnformatted(json_obj);
    } else {
        json_string = cJSON_Print(json_obj);
    }
    
    if (!json_string) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    // Send through the active channels
    result = send_to_active_channels(json_string);
    
    // Free the memory
    cJSON_free(json_string);
    cJSON_Delete(json_obj);
    
    return result;
}

esp_err_t rbpower_telemetry_set_channel_enabled(rbpower_telemetry_protocol_t protocol, bool enabled) {
    if (protocol >= RBPOWER_TELEMETRY_PROTOCOLS_MAX || !g_telemetry_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    g_telemetry_config.channels[protocol].enabled = enabled;
    
    const char* protocol_names[] = {"MQTT", "HTTP", "WebSocket"};
    ESP_LOGI(TAG, "%s channel %s", protocol_names[protocol], enabled ? "ENABLED" : "DISABLED");
    
    return ESP_OK;
}

void rbpower_telemetry_on_3sec_period_end(uint64_t period_end_time_us) {
    if (!g_telemetry_initialized || !g_telemetry_config.telemetry_enabled) {
        return;
    }
    
    // Build the rich telemetry JSON ONCE and send it to ALL enabled channels (unified schema).
    // Previously MQTT/HTTP got a SEPARATE old packet that only carried generic
    // units[].statistics.periods[3SEC] — so the specialized main_supply/voltage_bus units
    // (rbAmp 0x50 → 0x0006/0x0007) showed 0 because their data lives in the specialized
    // structs, which only this rich JSON reads. Unifying also removes the double JSON build
    // and the two-incompatible-schemas issue.
    {
        extern char* rbgrid_json_create_telemetry(void);
        // Build the snapshot under the g_home_network lock (consistent read vs the core-0
        // update tasks / inject path), then release BEFORE the network sends below — a
        // publish must never hold the lock across the wire.
        rbpower_home_lock();
        char* json_telemetry = rbgrid_json_create_telemetry();
        extern bool telemetry_ws_has_frame2(void);
        extern char* rbgrid_json_create_telemetry_frame(uint8_t);
        char* json_v2 = telemetry_ws_has_frame2() ? rbgrid_json_create_telemetry_frame(2) : NULL;   /* v2 frame for ?v=2 WS clients */
        rbpower_home_unlock();
        if (!json_telemetry) {
            ESP_LOGW(TAG, "Failed to create telemetry JSON on 3sec period end");
            free(json_v2);
            return;
        }
        if (g_telemetry_config.channels[RBPOWER_TELEMETRY_WEBSOCKET].enabled && g_websocket_telemetry_enabled) {
            send_websocket2(json_telemetry, json_v2);
        }
        free(json_v2);   /* also when WS is off: a registered v2 client made it (leaked ~5 KB / 3 s) */
        if (g_telemetry_config.channels[RBPOWER_TELEMETRY_MQTT].enabled) {
            send_mqtt(json_telemetry);
        }
        if (g_telemetry_config.channels[RBPOWER_TELEMETRY_HTTP].enabled) {
            send_http(json_telemetry);
        }
        free(json_telemetry);
        return;  // unified path — replaces the legacy per-channel packet build below
    }
    
    // Legacy packet path below is unreachable while the unified block above returns; kept for the
    // packet-based API (rbpower_get_latest_telemetry / rbpower_telemetry_send_packet).
    // Get the network through the API for the old format
    rbpower_electrical_network_t* network;
    rbpower_err_t err = rbpower_get_network(&network);
    if (err != RBPOWER_OK || network == NULL) {
        ESP_LOGW(TAG, "Cannot get network for telemetry: %d", err);
        return;
    }
    
    // Build the summary telemetry packet for all units (old format for MQTT/HTTP)
    rbpower_telemetry_packet_t packet = {
        .timestamp_us = period_end_time_us,
        .period_type = RBPOWER_STAT_PERIOD_3SEC,
        .units_count = 0
    };
    
    // Collect data from all active units
    for (uint8_t unit_id = 0; unit_id < network->units_count && packet.units_count < RBPOWER_MAX_HOME_UNITS; unit_id++) {
        if (network->units[unit_id].has_valid_definition) {
            rbpower_telemetry_unit_data_t* unit_data = &packet.units[packet.units_count];
            
            unit_data->unit_id = unit_id;
            unit_data->data_valid = network->units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_3SEC].period_complete;
            
            if (unit_data->data_valid) {
                unit_data->stats = &network->units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_3SEC];
            } else {
                unit_data->stats = NULL;
            }
            
            packet.units_count++;
        }
    }
    
    // Send the summary packet if there is data (for MQTT/HTTP)
    if (packet.units_count > 0) {
        // Store the latest telemetry packet
        if (g_telemetry_mutex && xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&g_latest_telemetry, &packet, sizeof(rbpower_telemetry_packet_t));
            g_has_telemetry_data = true;
            xSemaphoreGive(g_telemetry_mutex);
        }
        
        // Send through MQTT/HTTP if they are enabled
        if (g_telemetry_config.channels[RBPOWER_TELEMETRY_MQTT].enabled || 
            g_telemetry_config.channels[RBPOWER_TELEMETRY_HTTP].enabled) {
            esp_err_t result = rbpower_telemetry_send_packet(&packet);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send telemetry packet with %d units: %s", 
                         packet.units_count, esp_err_to_name(result));
            } else {
                ESP_LOGD(TAG, "Telemetry sent for %d units", packet.units_count);
            }
        }
    } else {
        ESP_LOGD(TAG, "No active units for telemetry");
    }
}

rbpower_err_t rbpower_get_latest_telemetry(rbpower_telemetry_packet_t* packet) {
    if (!packet || !g_telemetry_initialized) {
        return RBPOWER_ERR_NO_DATA;
    }
    
    if (!g_has_telemetry_data) {
        return RBPOWER_ERR_NO_DATA;
    }
    
    if (g_telemetry_mutex && xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(packet, &g_latest_telemetry, sizeof(rbpower_telemetry_packet_t));
        xSemaphoreGive(g_telemetry_mutex);
        return RBPOWER_OK;
    }
    
    return RBPOWER_ERR_TIMEOUT;
}

/* ================================================================================================
 * WEBSOCKET TELEMETRY CONTROL
 * ================================================================================================ */

/* Enable WebSocket telemetry (documented in rbtelemetry.h). */
esp_err_t rbpower_telemetry_start_websocket(void) {
    if (!g_telemetry_initialized) {
        ESP_LOGE(TAG, "Telemetry not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    g_websocket_telemetry_enabled = true;
    ESP_LOGI(TAG, "WebSocket telemetry enabled");
    return ESP_OK;
}

/* Disable WebSocket telemetry (documented in rbtelemetry.h). */
esp_err_t rbpower_telemetry_stop_websocket(void) {
    g_websocket_telemetry_enabled = false;
    ESP_LOGI(TAG, "WebSocket telemetry disabled");
    return ESP_OK;
}

/**
 * @brief Check whether WebSocket telemetry is enabled (at least one client connected).
 * @return true if the WebSocket telemetry flag is set.
 */
bool rbpower_telemetry_is_websocket_enabled(void) {
    return g_websocket_telemetry_enabled;
}

/* Send one WebSocket telemetry frame now (documented in rbtelemetry.h). */
esp_err_t rbpower_telemetry_send_websocket_once(void) {
    if (!g_telemetry_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Build the telemetry JSON (snapshot under lock, then send lock-free)
    extern char* rbgrid_json_create_telemetry(void);
    rbpower_home_lock();
    char* json_telemetry = rbgrid_json_create_telemetry();
    extern bool telemetry_ws_has_frame2(void);
    extern char* rbgrid_json_create_telemetry_frame(uint8_t);
    char* json_v2 = telemetry_ws_has_frame2() ? rbgrid_json_create_telemetry_frame(2) : NULL;   /* v2 frame for ?v=2 WS clients */
    rbpower_home_unlock();

    if (!json_telemetry) {
        ESP_LOGE(TAG, "Failed to create telemetry JSON");
        free(json_v2);
        return ESP_ERR_NO_MEM;
    }

    // Send through WebSocket
    esp_err_t ret = send_websocket2(json_telemetry, json_v2);
    if (json_v2) free(json_v2);
    
    // Free the JSON string
    free(json_telemetry);
    
    return ret;
}