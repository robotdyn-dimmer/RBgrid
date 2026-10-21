/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbjson.c
 * @brief JSON serialization and parsing for the rbpower library.
 *
 * Builds the JSON documents served by the HTTP/WebSocket/MQTT layers (system, hardware, time,
 * tariff, telemetry, WiFi, units and rbAmp fleet configs; live telemetry frames; Home Assistant
 * MQTT Discovery configs) and parses the matching POST bodies back into rbpower structures.
 * JSON keys and values emitted here are part of the public API contract (see the API reference).
 */
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include "rbjson.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"            /* esp_app_get_description — HA device sw_version */
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "rbhome_types.h"
#include "rbpower_constants.h"
#include "rbpower_enums.h"
#include "rbgrid.h"   /* RBGRID_IS_I2C_UNIT / RBGRID_I2C_GET_* — telemetry source tag */
#include "rbgrid_rbamp_config.h"    /* rbamp_bridge_config_t — unbound-sensor view */
#include "rbpoint.h"                /* RBPOINT_* sizes for the tombstone serialize */
#include "rbgrid_config_api.h"      /* rbgrid_point_tombstones_get */
#include "rbgrid_config_storage.h"  /* rbgrid_storage_load_rbamp — read the persisted fleet binding */
#include "rbmqtt.h"                 /* rbmqtt_publish — HA MQTT Discovery (retained configs) */
#include "esp_mac.h"                /* esp_read_mac — device mac = HA identity */
#include "rbpower.h"
#include "rbhome_tariff.h"
#include "rbtelemetry.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "rbhome_main_supply.h"
#include "rbhome_alt_supply.h"
#include "rbpower_lifetime_api.h"   /* rbpower_lifetime_get_totalizers — exact e_in/e_out */
#include "rbamp.h"                 /* rbamp_sensor_model_limits — CT nominal / clip limits for /api/v1/sensors */
#include "rbamp_sensor_models.h"

static const char* TAG = "RBJSON";

/* Text tables of the JSON API (strings are part of the API; indices follow the enums). */

/**
 * @brief Text representations of time sources.
 *
 * Indices correspond to rbtime_source_t values. The strings are part of the JSON API.
 */
static const char *rbtime_source_text[] = {
    "NONE",
    "SNTP",
    "ESP_NOW_HUB",
    "MANUAL",
    "RTC",
    "GPS"};

/**
 * @brief Text representations of time statuses.
 *
 * Indices correspond to rbtime_status_t values. The strings are part of the JSON API.
 */
static const char *rbtime_status_text[] = {
    "UNINITIALIZED",
    "INITIALIZING",
    "SYNCING",
    "SYNCED",
    "TIMEOUT",
    "ERROR",
    "FALLBACK"};

/**
 * @brief Text representations of sensor types.
 *
 * Indices correspond to rbsensor_type_t values. The strings are part of the JSON API.
 */
static const char *rbsensor_type_text[] = {
    "VOLTAGE_ZMPT101B",
    "VOLTAGE_ZMPT107_1",
    "VOLTAGE_DIVIDER",
    "VOLTAGE_CUSTOM",
    "CURRENT_SCT013_10A",
    "CURRENT_SCT013_20A",
    "CURRENT_SCT013_30A",
    "CURRENT_SCT013_50A",
    "CURRENT_SCT013_100A",
    "CURRENT_ACS712_5A",
    "CURRENT_ACS712_20A",
    "CURRENT_ACS712_30A",
    "CURRENT_CUSTOM",
    "UNKNOWN"};

/* Defined here, declared in rbhome_tariff.h (shared with rbhome_tariff.c and rbwebserver_api.c). */
const char *const rbpower_tariff_zone_type_text[RBPOWER_TARIFF_ZONE_COUNT] = {
    "NIGHT",
    "MORNING_PEAK",
    "DAY",
    "EVENING_PEAK",
    "LATE_EVENING",
    "WEEKEND",
    "HOLIDAY",
    "CRITICAL_PEAK"};

/* Defined here, declared in rbhome_tariff.h (shared with rbhome_tariff.c and rbwebserver_api.c). */
const char *const rbpower_tariff_plan_type_text[RBPOWER_TARIFF_PLAN_CUSTOM + 1] = {
    "SIMPLE",
    "TWO_ZONE",
    "THREE_ZONE",
    "MULTI_ZONE",
    "PROGRESSIVE",
    "SEASONAL",
    "DYNAMIC",
    "CUSTOM"};

/* Defined here, declared in rbhome_tariff.h (shared with rbhome_tariff.c and rbwebserver_api.c). */
const char *const rbpower_tariff_condition_type_text[RBPOWER_TARIFF_CONDITION_SPECIAL_EVENT + 1] = {
    "TIME_RANGE",
    "WEEKDAY",
    "WEEKEND",
    "HOLIDAY",
    "SEASON",
    "MONTH",
    "DATE_RANGE",
    "SPECIAL_EVENT"};

// RBGRID_TELEMETRY_SCHEMA_VERSION is defined in rbjson.h (shared with the /api/v1/schema descriptor).

/* Telemetry PROFILE. false = FULL (realtime + lifetime energy + economics aggregates — the original
 * payload). true = LEAN (realtime only: V/I/P/freq/pf/dir) → the heavy lifetime/economics fields are
 * dropped from the 3 s live push (clients fetch those on demand from /api/v1/summary, /economics,
 * /history which read flash) → CPU savings on every push. Runtime-settable + persisted by
 * POST /api/system/telemetry-config; main.cpp loads it at boot. The schema "v" stays the same; lean just
 * OMITS the heavy keys. */
static bool g_tele_lean = false;
void rbgrid_telemetry_set_lean(bool on) { g_tele_lean = on; }
bool rbgrid_telemetry_get_lean(void) { return g_tele_lean; }
/* Telemetry frame version. 1 = legacy keys ("main"/"alt"/"v_bus" + hex);
 * 2 = every unit under its UPPERCASE hex unit_id + type/role/caps/res/q_live, top-level
 * main_unit/alt_unit/vbus_unit references. Global (persisted /tele_frame) drives MQTT + HA discovery;
 * HTTP latest and each WS client may ask for ?v=2 independently. */
static uint8_t g_tele_frame = 2;   /* frame v2 is the default since v1.1.0; v1 only by explicit choice */
void    rbgrid_telemetry_set_frame(uint8_t f) { g_tele_frame = (f == 2) ? 2 : 1; }
uint8_t rbgrid_telemetry_get_frame(void) { return g_tele_frame; }

/* ================================================================================================
 * GLOBAL VARIABLES
 * ================================================================================================ */

static rbgrid_json_context_t* g_json_context = NULL;

/** @brief The home electrical network (units, roles, live values); defined in rbpower.c. */
extern rbpower_electrical_network_t g_home_network;

/* ================================================================================================
 * TEXT REPRESENTATIONS OF ENUMERATIONS
 * ================================================================================================ */

/**
 * @brief Text names of the metric types.
 * Indices match the rbpower_metric_type_t values.
 */
static const char* rbpower_metric_type_text[] = {
    "VOLTAGE_BUS",
    "CURRENT_LOAD",
    "POWER_LOAD",
    "ENERGY_COUNTER",
    "FREQUENCY",
    "POWER_FACTOR",
    "COMPOSITE"
};

/**
 * @brief Text names of the network roles.
 * Indices match the rbpower_load_role_t values.
 */
static const char* rbpower_load_role_text[] = {
    "MAIN_SUPPLY",
    "ALTERNATIVE_SUPPLY",
    "CIRCUIT_BREAKER",
    "DEVICE_LOAD",
    "ZONE_LOAD",
    "RENEWABLE_SOURCE",
    "STORAGE_SYSTEM",
    "EV_CHARGING",
    "HEATING_COOLING",
    "SMART_APPLIANCE"
};

/**
 * @brief Text names of the data source types.
 * Indices match the rbpower_source_type_t values.
 */
static const char* rbpower_source_type_text[] = {
    "ADC_SENSOR",
    "ESP_NOW",
    "I2C_SENSOR",
    "VIRTUAL_CALC",
    "AGGREGATED",
    "EXTERNAL_API"
};

/**
 * @brief Text names of the statistics periods.
 * Indices match the rbpower_stat_period_type_t values.
 */
static const char* rbpower_stat_period_type_text[] = {
    "3SEC",
    "15MIN",
    "1HOUR",
    "DAILY",
    "WEEKLY",
    "MONTHLY",
    "YEARLY"
};

/* ================================================================================================
 * HELPER FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Current wall-clock time as a Unix timestamp (epoch s).
 */
static uint64_t get_current_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec;
}

/** @brief Lifetime energy (Wh) for telemetry/HA (total_increasing).
 *
 * Uses the exact int64 mWh totalizer (the one v4 et_in/et_out use), not the float total_* — above
 * 2^24 Wh (16.7 MWh, a few years of a main supply) the float step is 2 Wh and night/export quarters
 * would round to nothing, drifting from history. */
static double lifetime_wh(rbpower_unit_id_t uid, bool in, float fallback)
{
    /* max(exact, float): HA total_increasing reads ANY decrease as a meter reset (a spike of the whole total in
     * the Energy dashboard). Both series are non-decreasing, so their max is too — no step down at the switch. */
    int64_t i_mwh = 0, o_mwh = 0;
    if (rbpower_lifetime_get_totalizers(uid, &i_mwh, &o_mwh) == RBPOWER_OK) {
        double exact = (double)(in ? i_mwh : o_mwh) / 1000.0;
        return exact > (double)fallback ? exact : (double)fallback;
    }
    return (double)fallback;
}

/** @brief Add the CT nominal of a channel in A, from the module's sensor class + the channel's model code
 * (rbAmp registry, rbamp-esp-idf 1.7.2-rc2). Omitted when unknown (class not read, legacy codes, virtual modules). */
static void rbjson_add_ct_rated(cJSON* s, uint8_t module, int ct_code)
{
    rbpower_module_desc_t d;
    if (ct_code <= 0 || !rbpower_get_module_desc(module, &d) || d.sensor_class == 0xFF || d.sensor_class == 0) return;
    const rbamp_sensor_model_info_t *mi = rbamp_sensor_model_lookup(d.sensor_class, (uint8_t)ct_code);
    if (mi) cJSON_AddStringToObject(s, "ct_name", mi->descriptor);
    float nom = 0, fs = 0, pk = 0;
    if (rbamp_sensor_model_limits(d.sensor_class, (uint8_t)ct_code, &nom, &fs, &pk)) {
        if (nom > 0) cJSON_AddNumberToObject(s, "ct_rated_a", round(nom * 100) / 100);   /* rated RMS (registry), not always the SKU amps */
        if (fs > 0)  cJSON_AddNumberToObject(s, "ct_fs_a", round(fs * 100) / 100);       /* RMS where a sine starts to clip */
        if (pk > 0)  cJSON_AddNumberToObject(s, "ct_peak_a", round(pk * 100) / 100);     /* instantaneous clip ceiling */
    }
}

/** @brief Add the SCT rating correction of the channel (rbamp-esp-idf 1.7.2): "corrected" + scale when the library rescales the
 * readings of a BASIC fw ≤ 1.4.19 module bound to another SCT than its 030 calibration; "uncalibrated" /
 * "gain_unexpected" / "unknown_model" mean the readings are NOT corrected — worth a warning in the app. */
static void rbjson_add_sct(cJSON* s, uint8_t module, int ch)
{
    uint8_t st = 0; float sc = 1.0f;
    if (ch < 0 || ch > 7 || !rbpower_get_channel_sct(module, (uint8_t)ch, &st, &sc)) return;
    static const char *N[] = { "none", "corrected", "uncalibrated", "gain_unexpected", "unknown_model", "pending" };
    cJSON_AddStringToObject(s, "sct_correction", st < 6 ? N[st] : "unknown");
    if (st == 1) cJSON_AddNumberToObject(s, "sct_scale", round(sc * 10000) / 10000);
}

/** @brief Add the CT load vs the sensor nominal (%): the nominal is a limit — above it the reading clips.
 * Shared by the regular unit objects and the specialized main / alt blocks. */
static void rbjson_add_ct_load(cJSON* o, rbpower_unit_id_t uid)
{
    float lp; uint8_t cs;
    if (!o || !rbpower_get_unit_ct_load(uid, &lp, &cs)) return;
    cJSON_AddNumberToObject(o, "load_pct", round(lp));
    if ((cs & 0x0F) >= 1) cJSON_AddBoolToObject(o, "ct_warn", true);
    if ((cs & 0x0F) >= 2) cJSON_AddBoolToObject(o, "clip", true);
    if (cs & 0x10) cJSON_AddBoolToObject(o, "reversed", true);   /* BASIC: negative flow, not accounted */
}

/** @brief Time since boot in ms (64-bit; a 32-bit tick*ms value would wrap after 49.7 days). */
static uint64_t get_uptime_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);   /* 32-bit tick*ms wrapped after 49.7 days */
}

/**
 * @brief Free heap size in bytes.
 */
static uint32_t get_free_heap_size(void) {
    return esp_get_free_heap_size();
}

/**
 * @brief Add a float to a JSON object, rounded.
 * @param[in,out] object JSON object
 * @param[in] name Field name
 * @param[in] value Float value
 * @param[in] decimals Number of decimal places (6 is the usual choice)
 */
static void cJSON_AddRoundedNumber(cJSON* object, const char* name, float value, int decimals) {
    if (!object || !name) return;

    // Round the value to the requested number of decimals
    double multiplier = pow(10.0, decimals);
    double rounded = round(value * multiplier) / multiplier;

    cJSON_AddNumberToObject(object, name, rounded);
}

/**
 * @brief Add a double to a JSON object, rounded.
 * @param[in,out] object JSON object
 * @param[in] name Field name
 * @param[in] value Double value
 * @param[in] decimals Number of decimal places (6 is the usual choice)
 */
static void cJSON_AddRoundedDouble(cJSON* object, const char* name, double value, int decimals) {
    if (!object || !name) return;

    // Round the value to the requested number of decimals
    double multiplier = pow(10.0, decimals);
    double rounded = round(value * multiplier) / multiplier;

    cJSON_AddNumberToObject(object, name, rounded);
}

/* ================================================================================================
 * CORE FUNCTIONS
 * ================================================================================================ */

rbgrid_json_err_t rbgrid_json_init(rbgrid_json_context_t* context, 
                                    const rbgrid_json_device_info_t* device_info) {
    if (!context || !device_info) {
        ESP_LOGE(TAG, "Invalid arguments for json init");
        return RBGRID_JSON_ERR_INVALID_ARG;
    }

    // Copy the device info
    memcpy(&context->device_info, device_info, sizeof(rbgrid_json_device_info_t));

    // Initialize the context
    context->ntp_enabled = true;
    context->boot_time_ntp = get_current_timestamp();
    context->boot_count = 1; // TODO: read from NVS
    
    g_json_context = context;
    
    ESP_LOGI(TAG, "JSON module initialized for device: %s", context->device_info.device_name);
    return RBGRID_JSON_OK;
}

cJSON* rbgrid_json_create_base_message(const char* type,
                                       const rbgrid_json_context_t* context) {
    if (!type || !context) {
        ESP_LOGE(TAG, "Invalid arguments for base message");
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root object");
        return NULL;
    }

    // Base fields
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "version", RBGRID_JSON_VERSION);
    cJSON_AddStringToObject(root, "device_id", context->device_info.device_id);

    // Timestamps
    uint64_t current_time = get_current_timestamp();
    uint64_t uptime_ms = get_uptime_ms();
    
    cJSON_AddNumberToObject(root, "timestamp_local", (double)(uptime_ms / 1000));
    if (context->ntp_enabled) {
        cJSON_AddNumberToObject(root, "timestamp_ntp", (double)current_time);
    }

    return root;
}

rbgrid_json_err_t rbgrid_json_add_timestamps(cJSON* json,
                                              const rbgrid_json_context_t* context) {
    if (!json || !context) {
        return RBGRID_JSON_ERR_INVALID_ARG;
    }

    uint64_t current_time = get_current_timestamp();
    uint64_t uptime_ms = get_uptime_ms();
    
    cJSON_AddNumberToObject(json, "timestamp_local", (double)(uptime_ms / 1000));
    if (context->ntp_enabled) {
        cJSON_AddNumberToObject(json, "timestamp_ntp", (double)current_time);
    }

    return RBGRID_JSON_OK;
}

/* ================================================================================================
 * 1. SYSTEM CONFIG
 * ================================================================================================ */

/* Universal system config JSON creation - works with local or global config.
 * Creates the compact JSON format for storage and API; caller frees the returned string. */
char* rbgrid_json_create_system_config(const rbpower_extended_config_t* extended_config) {
    if (!extended_config) {
        ESP_LOGE(TAG, "Invalid arguments for system config");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating system_config JSON");

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    
    // === SYSTEM SECTION with only required fields ===
    cJSON* system = cJSON_CreateObject();
    if (system) {
        // Basic info
        cJSON_AddStringToObject(system, "version", "1.0.0");
        cJSON_AddStringToObject(system, "device_id", "ESP32-RBGrid");
        
        // Home configuration
        cJSON_AddStringToObject(system, "home_name", 
            extended_config->home_name ? extended_config->home_name : "");
        cJSON_AddStringToObject(system, "location", 
            extended_config->location ? extended_config->location : "");
        cJSON_AddStringToObject(system, "language", 
            extended_config->language ? extended_config->language : "");  /* "" = installer has not chosen */
        
        /* Removed fields: logging_level, enable_logging, enable_telemetry,
         * max_retry_attempts, retry_delay_ms, enable_json_compression, enable_encryption, auto_start_measurements.
         * Nothing read the stored values — main.cpp sets logging_level / auto_start at boot, the rest were only
         * stored and echoed, yet the UI asked for a reboot to "apply" them. A POST that still sends them is
         * accepted and they are ignored. */

        /* Overview layout profile (declarative, UI-owned semantics) */
        {
            static const char* profile_text[] = { "home", "industrial", "ev" };
            uint8_t p = extended_config->site_profile;
            cJSON_AddStringToObject(system, "site_profile", profile_text[p < 3 ? p : 0]);
        }

        /* EV-scenario hints (flat per hub; 0 = not set, UI applies its defaults) */
        cJSON* ev = cJSON_CreateObject();
        if (ev) {
            cJSON_AddNumberToObject(ev, "charge_threshold_w", extended_config->ev_config.charge_threshold_w);
            cJSON_AddNumberToObject(ev, "idle_timeout_min", extended_config->ev_config.idle_timeout_min);
            cJSON_AddRoundedNumber(ev, "price_per_kwh", extended_config->ev_config.price_per_kwh, 6);
            cJSON_AddItemToObject(system, "ev", ev);
        }

        /* Group display names. [] = none defined. */
        {
            cJSON* groups = cJSON_AddArrayToObject(system, "groups");
            for (uint8_t g = 0; g < extended_config->groups_count && g < RBPOWER_MAX_GROUPS; g++) {
                cJSON* go = cJSON_CreateObject();
                if (!go) break;
                cJSON_AddNumberToObject(go, "id", extended_config->groups[g].id);
                cJSON_AddStringToObject(go, "name", extended_config->groups[g].name);
                cJSON_AddItemToArray(groups, go);
            }
        }

        /* Mains topology: grid.system is the stored value; nominal_v/nominal_hz are a
         * read-only mirror of hardware.network (write them via hardware-config) — omitted when the
         * hardware config is not attached (local/persisted copies). */
        {
            static const char* grid_text[] = { "", "1P", "SPLIT", "3P" };
            uint8_t gs = extended_config->grid_system;
            cJSON* grid = cJSON_CreateObject();
            if (grid) {
                cJSON_AddStringToObject(grid, "system", grid_text[gs < 4 ? gs : 0]);
                if (extended_config->base_hardware_config) {
                    /* omit unset (0) values — clients treat an absent key as "derive yourself" */
                    if (extended_config->base_hardware_config->network.nominal_voltage > 0)
                        cJSON_AddNumberToObject(grid, "nominal_v",
                            extended_config->base_hardware_config->network.nominal_voltage);
                    if (extended_config->base_hardware_config->network.nominal_frequency > 0)
                        cJSON_AddNumberToObject(grid, "nominal_hz",
                            extended_config->base_hardware_config->network.nominal_frequency);
                }
                cJSON_AddItemToObject(system, "grid", grid);
            }
        }

        cJSON_AddItemToObject(root, "system", system);
    }

    ESP_LOGI(TAG, "system_config JSON created");
    
    // Convert to unformatted string for compact storage
    char* json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return json_string;
}

/** @def IS_LIKELY_CONSTANT(ptr)
 *  @brief True when @p ptr looks like a string constant in ROM/flash (address < 0x40000000), i.e. not heap.
 *  Defined locally (and undefined again) inside rbgrid_json_parse_system_section() and rbgrid_json_free_config_strings(). */
/** @def UPDATE_STRING_FIELD(field_ptr, json_item)
 *  @brief Replace a heap string field with a strdup() of a JSON string item, freeing the old heap value;
 *  returns RBGRID_JSON_ERR_NO_MEMORY from the enclosing function on allocation failure.
 *  Local to rbgrid_json_parse_system_section(). */

/* Parse the system section from JSON (universal for global and local contexts).
 *
 * Strategy:
 * - If pointer is NULL or points to read-only memory (constants) -> allocate new
 * - If pointer is already dynamic memory -> free old and allocate new
 * - For local/temporary context: caller should set pointers to NULL before parsing
 */
rbgrid_json_err_t rbgrid_json_parse_system_section(cJSON* system_obj, rbpower_extended_config_t* config)
{
    if (!system_obj || !config) {
        return RBGRID_JSON_ERR_INVALID_ARG;
    }
    
    cJSON* item = NULL;
    
    /** @brief True when the pointer looks like a string constant (in ROM/flash).
     *  On ESP32: ROM is typically < 0x40000000, RAM is >= 0x3FF00000. */
    #define IS_LIKELY_CONSTANT(ptr) ((uintptr_t)(ptr) < 0x40000000)

    /** @brief Replace a heap string field with a strdup() of a JSON string item, freeing the old heap value. */
    #define UPDATE_STRING_FIELD(field_ptr, json_item) \
        do { \
            if ((json_item) && cJSON_IsString(json_item)) { \
                const char* old_val = (field_ptr); \
                const char* new_val = (json_item)->valuestring; \
                \
                /* Check if we need to free old value */ \
                if (old_val && !IS_LIKELY_CONSTANT(old_val)) { \
                    /* Looks like dynamic memory, free it */ \
                    ESP_LOGD(TAG, "Freeing old value: %s", old_val); \
                    free((void*)old_val); \
                } \
                \
                /* Allocate new string */ \
                (field_ptr) = strdup(new_val); \
                if (!(field_ptr)) { \
                    ESP_LOGE(TAG, "Failed to allocate memory for string"); \
                    (field_ptr) = old_val; /* Try to restore */ \
                    return RBGRID_JSON_ERR_NO_MEMORY; \
                } \
            } \
        } while(0)
    
    // Skip version and device_id - they are informational only
    
    // Parse home_name
    item = cJSON_GetObjectItem(system_obj, "home_name");
    UPDATE_STRING_FIELD(config->home_name, item);
    
    // Parse location
    item = cJSON_GetObjectItem(system_obj, "location");
    UPDATE_STRING_FIELD(config->location, item);
    
    // Parse language - special handling for default constant
    item = cJSON_GetObjectItem(system_obj, "language");
    if (item && cJSON_IsString(item) &&
        (item->valuestring[0] == '\0' || (isalpha((unsigned char)item->valuestring[0]) &&
         isalpha((unsigned char)item->valuestring[1]) && item->valuestring[2] == '\0'))) {
        /* any ISO 639-1 code (2 letters) or "" — stored upper-case like the old RU/EN; anything else keeps the
         * current value here (the HTTP POST answers 400 before parsing) */
        item->valuestring[0] = (char)toupper((unsigned char)item->valuestring[0]);
        if (item->valuestring[0]) item->valuestring[1] = (char)toupper((unsigned char)item->valuestring[1]);
        const char* old_lang = config->language;
        
        // Check if old value needs to be freed (not NULL, not constant "EN")
        if (old_lang && 
            !IS_LIKELY_CONSTANT(old_lang) && 
            strcmp(old_lang, "EN") != 0) {
            ESP_LOGD(TAG, "Freeing old language: %s", old_lang);
            free((void*)old_lang);
        }
        
        // Allocate new string
        config->language = strdup(item->valuestring);
        if (!config->language) {
            ESP_LOGE(TAG, "Failed to allocate memory for language");
            config->language = "";  // Fall back to "unset"
        }
    }
    
    #undef IS_LIKELY_CONSTANT
    #undef UPDATE_STRING_FIELD
    
    // Logging settings (now direct fields in system)
    item = cJSON_GetObjectItem(system_obj, "logging_level");
    if (item && cJSON_IsNumber(item)) {
        config->rbhome_settings.logging_level = (rbgrid_loging_level_t)item->valueint;
    }
    
    item = cJSON_GetObjectItem(system_obj, "enable_logging");
    if (item && cJSON_IsBool(item)) {
        config->rbhome_settings.enable_logging = cJSON_IsTrue(item);
    }
    
    // Telemetry settings (now direct fields in system)
    item = cJSON_GetObjectItem(system_obj, "enable_telemetry");
    if (item && cJSON_IsBool(item)) {
        config->rbhome_telemetry_settings.enable_telemetry = cJSON_IsTrue(item);
    }
    
    item = cJSON_GetObjectItem(system_obj, "max_retry_attempts");
    if (item && cJSON_IsNumber(item)) {
        config->rbhome_telemetry_settings.max_retry_attempts = (uint8_t)item->valueint;
    }
    
    item = cJSON_GetObjectItem(system_obj, "retry_delay_ms");
    if (item && cJSON_IsNumber(item)) {
        config->rbhome_telemetry_settings.retry_delay_ms = (uint32_t)item->valuedouble;
    }
    
    // JSON settings (now direct fields in system)
    item = cJSON_GetObjectItem(system_obj, "enable_json_compression");
    if (item && cJSON_IsBool(item)) {
        config->rbhome_JSON_settings.enable_JSON_compression = cJSON_IsTrue(item);
    }
    
    item = cJSON_GetObjectItem(system_obj, "enable_encryption");
    if (item && cJSON_IsBool(item)) {
        config->rbhome_JSON_settings.enable_encryption = cJSON_IsTrue(item);
    }
    // Note: encryption_key not parsed for security
    
    // Measurement settings
    item = cJSON_GetObjectItem(system_obj, "auto_start_measurements");
    if (item && cJSON_IsBool(item)) {
        config->auto_start_measurements = cJSON_IsTrue(item);
    }

    /* site_profile — strict enum; an unknown value keeps the current one (no silent reset) */
    item = cJSON_GetObjectItem(system_obj, "site_profile");
    if (item && cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "home") == 0)            config->site_profile = 0;
        else if (strcmp(item->valuestring, "industrial") == 0) config->site_profile = 1;
        else if (strcmp(item->valuestring, "ev") == 0)         config->site_profile = 2;
        else ESP_LOGW(TAG, "site_profile '%s' unknown — kept previous", item->valuestring);
    }

    /* ev block — sanity-clamped; absent fields keep current values */
    cJSON* ev_obj = cJSON_GetObjectItem(system_obj, "ev");
    if (ev_obj && cJSON_IsObject(ev_obj)) {
        item = cJSON_GetObjectItem(ev_obj, "charge_threshold_w");
        if (item && cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 1000000) {
            config->ev_config.charge_threshold_w = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(ev_obj, "idle_timeout_min");
        if (item && cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint <= 1440) {
            config->ev_config.idle_timeout_min = (uint16_t)item->valueint;
        }
        item = cJSON_GetObjectItem(ev_obj, "price_per_kwh");
        if (item && cJSON_IsNumber(item) && item->valuedouble >= 0) {
            config->ev_config.price_per_kwh = (float)item->valuedouble;
        }
    }

    /* groups — the posted array REPLACES the whole list (id > 0, name <= 31 bytes).
     * Absent key keeps the current list; present-but-invalid entries are skipped. */
    cJSON* groups_obj = cJSON_GetObjectItem(system_obj, "groups");
    if (groups_obj && cJSON_IsArray(groups_obj)) {
        config->groups_count = 0;
        cJSON* ge;
        cJSON_ArrayForEach(ge, groups_obj) {
            if (config->groups_count >= RBPOWER_MAX_GROUPS) break;
            cJSON* gid = cJSON_GetObjectItem(ge, "id");
            cJSON* gname = cJSON_GetObjectItem(ge, "name");
            if (!cJSON_IsNumber(gid) || gid->valueint <= 0 || gid->valueint > 0xFFFF ||
                !cJSON_IsString(gname) || strlen(gname->valuestring) > 31) {
                ESP_LOGW(TAG, "groups[]: invalid entry skipped");
                continue;
            }
            config->groups[config->groups_count].id = (uint16_t)gid->valueint;
            strlcpy(config->groups[config->groups_count].name, gname->valuestring,
                    sizeof(config->groups[0].name));
            config->groups_count++;
        }
    }

    /* grid.system — strict enum ""|"1P"|"SPLIT"|"3P"; unknown keeps current (no silent reset).
     * nominal_v/nominal_hz inside grid are IGNORED here (read-only mirror; write via hardware-config). */
    cJSON* grid_obj = cJSON_GetObjectItem(system_obj, "grid");
    if (grid_obj && cJSON_IsObject(grid_obj)) {
        item = cJSON_GetObjectItem(grid_obj, "system");
        if (item && cJSON_IsString(item)) {
            if (item->valuestring[0] == '\0')                 config->grid_system = 0;
            else if (strcmp(item->valuestring, "1P") == 0)    config->grid_system = 1;
            else if (strcmp(item->valuestring, "SPLIT") == 0) config->grid_system = 2;
            else if (strcmp(item->valuestring, "3P") == 0)    config->grid_system = 3;
            else ESP_LOGW(TAG, "grid.system '%s' unknown — kept previous", item->valuestring);
        }
    }

    ESP_LOGI(TAG, "System section parsed: home=%s, location=%s",
             config->home_name ? config->home_name : "NULL",
             config->location ? config->location : "NULL");
    
    return RBGRID_JSON_OK;
}

/* Free the dynamically allocated strings in a config structure.
 * Call this when destroying a config that was parsed with rbgrid_json_parse_system_section. */
void rbgrid_json_free_config_strings(rbpower_extended_config_t* config)
{
    if (!config) {
        return;
    }
    
    /** @brief True when the pointer looks like a string constant (in ROM/flash). */
    #define IS_LIKELY_CONSTANT(ptr) ((uintptr_t)(ptr) < 0x40000000)

    // Free home_name if it's dynamic memory
    if (config->home_name && !IS_LIKELY_CONSTANT(config->home_name)) {
        ESP_LOGD(TAG, "Freeing home_name: %s", config->home_name);
        free((void*)config->home_name);
        config->home_name = NULL;
    }
    
    // Free location if it's dynamic memory
    if (config->location && !IS_LIKELY_CONSTANT(config->location)) {
        ESP_LOGD(TAG, "Freeing location: %s", config->location);
        free((void*)config->location);
        config->location = NULL;
    }
    
    // Free language if it's dynamic memory and not default constant
    if (config->language && 
        !IS_LIKELY_CONSTANT(config->language) &&
        strcmp(config->language, "EN") != 0) {
        ESP_LOGD(TAG, "Freeing language: %s", config->language);
        free((void*)config->language);
        config->language = NULL;
    }
    
    #undef IS_LIKELY_CONSTANT
}

/* ================================================================================================
 * 2. HARDWARE CONFIG
 * ================================================================================================ */

/* Hardware config → JSON string. With a context: full API message ("hardware_config" + metadata);
 * without: compact "hardware" document for the config file. Caller frees the string. */
char* rbgrid_json_create_hardware_config(const rbpower_hardware_config_t* hardware_config,
                                          const rbgrid_json_context_t* context) {
    
    if (!hardware_config) {
        ESP_LOGE(TAG, "Invalid hardware_config for hardware config");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating hardware_config JSON (context %s)", context ? "provided" : "NULL");

    cJSON* root = NULL;
    cJSON* data = NULL;
    
    if (context) {
        // With a context: build the full API message with metadata
        root = rbgrid_json_create_base_message("hardware_config", context);
        if (!root) {
            return NULL;
        }
        data = cJSON_CreateObject();
        if (!data) {
            cJSON_Delete(root);
            return NULL;
        }
    } else {
        // Without a context: build a plain JSON for the config file
        root = cJSON_CreateObject();
        if (!root) {
            return NULL;
        }

        // Message type
        cJSON_AddStringToObject(root, "type", "hardware");

        // The data section that receives the hardware subsections
        data = cJSON_CreateObject();
        if (!data) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToObject(root, "data", data);
    }


    // === NETWORK: mains nominal frequency (Hz) and voltage (V) ===
    cJSON* network = cJSON_CreateObject();
    if (network) {
        cJSON_AddNumberToObject(network, "nominal_frequency", hardware_config->network.nominal_frequency);
        /* nominal_voltage lets a 120 V site be configured over the API. 0 = unset (clients derive/fall back). */
        cJSON_AddNumberToObject(network, "nominal_voltage", hardware_config->network.nominal_voltage);
        cJSON_AddItemToObject(data, "network", network);
    }

    // Attach data here only with a context (API message);
    // without a context data is already attached under the "hardware" document
    if (context) {
        cJSON_AddItemToObject(root, "data", data);
    }

    ESP_LOGI(TAG, "hardware_config JSON created successfully");
    
    // Convert to string and cleanup cJSON
    // PrintUnformatted gives the compact form used without a context (files)
    char* json_string = context ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return json_string;
}



/* ================================================================================================
 * 3. SENSOR TYPE LIST
 * ================================================================================================ */

/* List of the sensor type names ("adc_sensor_list" message). The ADC-typed names are the legacy
 * binding of the default units fed by the rbAmp bridge — there is no native ESP32-ADC path.
 * Caller frees the returned string. */
char* rbgrid_json_create_adc_sensor_list(const rbgrid_json_context_t* context) {
    if (!context) {
        ESP_LOGE(TAG, "Invalid arguments for adc sensor list");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating adc_sensor_list JSON");

    cJSON* root = rbgrid_json_create_base_message("adc_sensor_list", context);
    if (!root) {
        return NULL;
    }

    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    // === SENSOR TYPE LIST ===
    cJSON* list = cJSON_CreateArray();
    if (list) {
        // All sensor types from rbsensor_type_text,
        // excluding the last element (UNKNOWN)
        for (int i = 0; i < RBSENSOR_TYPE_UNKNOWN; i++) {
            cJSON_AddItemToArray(list, cJSON_CreateString(rbsensor_type_text[i]));
        }
        cJSON_AddItemToObject(data, "list", list);
    }

    cJSON_AddItemToObject(root, "data", data);

    ESP_LOGI(TAG, "adc_sensor_list JSON created successfully");
    
    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_string;
}

/* ================================================================================================
 * 4. TIME CONFIG
 * ================================================================================================ */

/* Time-system config + sync statistics ("time_config" message). Caller frees the returned string. */
char* rbgrid_json_create_time_config(const rbpower_hardware_config_t* hardware_config,
                                      const rbgrid_json_context_t* context) {
    if (!hardware_config || !context) {
        ESP_LOGE(TAG, "Invalid arguments for time config");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating time_config JSON");

    cJSON* root = rbgrid_json_create_base_message("time_config", context);
    if (!root) {
        return NULL;
    }

    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    // === CONFIG SUBSTRUCTURE ===
    cJSON* config = cJSON_CreateObject();
    if (config) {
        const rbtime_config_t* time_cfg = &hardware_config->time_config;

        // Main settings
        cJSON_AddBoolToObject(config, "enable_time_sync", time_cfg->enable_time_sync);

        // Time sources as text values
        const char* primary_source_str = "NONE";
        if (time_cfg->primary_source >= 0 && time_cfg->primary_source <= RBTIME_SOURCE_GPS) {
            primary_source_str = rbtime_source_text[time_cfg->primary_source];
        }
        cJSON_AddStringToObject(config, "primary_source", primary_source_str);
        
        const char* fallback_source_str = "NONE";
        if (time_cfg->fallback_source >= 0 && time_cfg->fallback_source <= RBTIME_SOURCE_GPS) {
            fallback_source_str = rbtime_source_text[time_cfg->fallback_source];
        }
        cJSON_AddStringToObject(config, "fallback_source", fallback_source_str);
        
        // SNTP settings
        cJSON_AddStringToObject(config, "sntp_server", time_cfg->sntp_server);
        cJSON_AddStringToObject(config, "sntp_server_backup", time_cfg->sntp_server_backup);
        cJSON_AddNumberToObject(config, "sntp_timeout_ms", time_cfg->sntp_timeout_ms);
        
        // Time zone
        cJSON_AddStringToObject(config, "timezone", time_cfg->timezone);
        cJSON_AddBoolToObject(config, "enable_dst", time_cfg->enable_dst);
        
        // Periodic resync
        cJSON_AddNumberToObject(config, "resync_interval_sec", time_cfg->resync_interval_sec);
        cJSON_AddNumberToObject(config, "max_drift_sec", time_cfg->max_drift_sec);
        
        // ESP-NOW time
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 time_cfg->esp_now_hub_mac[0], time_cfg->esp_now_hub_mac[1],
                 time_cfg->esp_now_hub_mac[2], time_cfg->esp_now_hub_mac[3],
                 time_cfg->esp_now_hub_mac[4], time_cfg->esp_now_hub_mac[5]);
        cJSON_AddStringToObject(config, "esp_now_hub_mac", mac_str);
        cJSON_AddNumberToObject(config, "esp_now_timeout_ms", time_cfg->esp_now_timeout_ms);
        
        // Fallback parameters
        cJSON_AddBoolToObject(config, "enable_rtc_fallback", time_cfg->enable_rtc_fallback);
        cJSON_AddBoolToObject(config, "enable_cpu_time_fallback", time_cfg->enable_cpu_time_fallback);
        cJSON_AddNumberToObject(config, "manual_time_us", (double)time_cfg->manual_time_us);
        
        // Additional parameters
        cJSON_AddBoolToObject(config, "force_sync_on_startup", time_cfg->force_sync_on_startup);
        cJSON_AddBoolToObject(config, "enable_time_logging", time_cfg->enable_time_logging);
        cJSON_AddNumberToObject(config, "sync_retry_count", time_cfg->sync_retry_count);
        
        cJSON_AddItemToObject(data, "config", config);
    }

    // === STATISTICS SUBSTRUCTURE ===
    cJSON* statistics = cJSON_CreateObject();
    if (statistics) {
        // Read the time statistics
        rbtime_statistics_t stats;
        rbpower_err_t err = rbtime_get_statistics(&stats);
        
        if (err == RBPOWER_OK) {
            // Current status
            const char* status_str = "UNINITIALIZED";
            if (stats.current_status >= 0 && stats.current_status <= RBTIME_STATUS_FALLBACK) {
                status_str = rbtime_status_text[stats.current_status];
            }
            cJSON_AddStringToObject(statistics, "current_status", status_str);
            
            // Active source
            const char* active_source_str = "NONE";
            if (stats.active_source >= 0 && stats.active_source <= RBTIME_SOURCE_GPS) {
                active_source_str = rbtime_source_text[stats.active_source];
            }
            cJSON_AddStringToObject(statistics, "active_source", active_source_str);
            
            cJSON_AddNumberToObject(statistics, "last_sync_timestamp_us", stats.last_sync_timestamp_us);
            cJSON_AddNumberToObject(statistics, "system_start_time_us", stats.system_start_time_us);
            
            // Sync statistics
            cJSON_AddNumberToObject(statistics, "successful_syncs", stats.successful_syncs);
            cJSON_AddNumberToObject(statistics, "failed_syncs", stats.failed_syncs);
            cJSON_AddNumberToObject(statistics, "fallback_activations", stats.fallback_activations);
            cJSON_AddNumberToObject(statistics, "time_corrections_count", stats.time_corrections_count);
            
            // Time accuracy
            cJSON_AddNumberToObject(statistics, "last_time_correction_sec", stats.last_time_correction_sec);
            cJSON_AddNumberToObject(statistics, "max_observed_drift_sec", stats.max_observed_drift_sec);
            cJSON_AddNumberToObject(statistics, "average_sync_duration_ms", stats.average_sync_duration_ms);
            
            // Network statistics
            cJSON_AddNumberToObject(statistics, "sntp_requests_sent", stats.sntp_requests_sent);
            cJSON_AddNumberToObject(statistics, "sntp_responses_received", stats.sntp_responses_received);
            cJSON_AddNumberToObject(statistics, "esp_now_time_requests", stats.esp_now_time_requests);
        } else {
            // Statistics unavailable: emit the default values
            cJSON_AddStringToObject(statistics, "current_status", "UNINITIALIZED");
            cJSON_AddStringToObject(statistics, "active_source", "NONE");
            cJSON_AddNumberToObject(statistics, "last_sync_timestamp_us", 0);
            cJSON_AddNumberToObject(statistics, "system_start_time_us", 0);
            cJSON_AddNumberToObject(statistics, "successful_syncs", 0);
            cJSON_AddNumberToObject(statistics, "failed_syncs", 0);
            cJSON_AddNumberToObject(statistics, "fallback_activations", 0);
            cJSON_AddNumberToObject(statistics, "time_corrections_count", 0);
            cJSON_AddNumberToObject(statistics, "last_time_correction_sec", 0);
            cJSON_AddNumberToObject(statistics, "max_observed_drift_sec", 0);
            cJSON_AddNumberToObject(statistics, "average_sync_duration_ms", 0);
            cJSON_AddNumberToObject(statistics, "sntp_requests_sent", 0);
            cJSON_AddNumberToObject(statistics, "sntp_responses_received", 0);
            cJSON_AddNumberToObject(statistics, "esp_now_time_requests", 0);
        }
        
        cJSON_AddItemToObject(data, "statistics", statistics);
    }

    cJSON_AddItemToObject(root, "data", data);

    ESP_LOGI(TAG, "time_config JSON created successfully");
    
    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_string;
}

/* ================================================================================================
 * 5. TARIFF PLAN CONFIG
 * ================================================================================================ */

/* Current tariff plan ("tariff_plan_list_config" message). Caller frees the returned string. */
char* rbgrid_json_create_rbpower_tariff_plan_config(const rbgrid_json_context_t* context) {
    if (!context) {
        ESP_LOGE(TAG, "Invalid arguments for tariff plan config");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating tariff_plan_list_config JSON");

    cJSON* root = rbgrid_json_create_base_message("tariff_plan_list_config", context);
    if (!root) {
        return NULL;
    }

    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    // === TARIFF PLAN ARRAY ===
    cJSON* list = cJSON_CreateArray();
    if (list) {
        // Object for the current tariff plan
        cJSON* plan = cJSON_CreateObject();
        if (plan) {
            const rbpower_tariff_plan_t* tariff = &g_current_tariff_plan;
            
            // === BASE INFO ===
            cJSON_AddStringToObject(plan, "plan_name", tariff->plan_name);
            cJSON_AddBoolToObject(plan, "is_placeholder", rbpower_tariff_plan_is_placeholder(tariff->plan_name, tariff->provider_name));
            cJSON_AddStringToObject(plan, "provider_name", tariff->provider_name);
            cJSON_AddStringToObject(plan, "description", tariff->description);
            
            // Plan type
            const char* plan_type_str = "SIMPLE";
            if (tariff->plan_type >= 0 && tariff->plan_type <= RBPOWER_TARIFF_PLAN_CUSTOM) {
                plan_type_str = rbpower_tariff_plan_type_text[tariff->plan_type];
            }
            cJSON_AddStringToObject(plan, "plan_type", plan_type_str);
            
            cJSON_AddNumberToObject(plan, "active_zones_count", tariff->active_zones_count);
            
            // Default zone
            const char* default_zone_str = "NIGHT";
            if (tariff->default_zone_id >= 0 && tariff->default_zone_id < RBPOWER_TARIFF_ZONE_COUNT) {
                default_zone_str = rbpower_tariff_zone_type_text[tariff->default_zone_id];
            }
            cJSON_AddStringToObject(plan, "default_zone_id", default_zone_str);
            
            // === TARIFF ZONES ===
            cJSON* zones = cJSON_CreateArray();
            if (zones) {
                for (int i = 0; i < RBPOWER_TARIFF_ZONE_COUNT; i++) {
                    const rbpower_tariff_zone_t* zone = &tariff->zones[i];
                    
                    // Skip disabled zones
                    if (!zone->is_enabled) continue;
                    
                    cJSON* zone_obj = cJSON_CreateObject();
                    if (zone_obj) {
                        // Zone ID
                        const char* zone_id_str = "NIGHT";
                        if (zone->zone_id >= 0 && zone->zone_id < RBPOWER_TARIFF_ZONE_COUNT) {
                            zone_id_str = rbpower_tariff_zone_type_text[zone->zone_id];
                        }
                        cJSON_AddStringToObject(zone_obj, "zone_id", zone_id_str);
                        
                        cJSON_AddStringToObject(zone_obj, "zone_name", zone->zone_name);
                        cJSON_AddStringToObject(zone_obj, "description", zone->description);
                        
                        // Tariff rates (money values rounded to 6 decimals)
                        cJSON_AddRoundedNumber(zone_obj, "rate_buying_per_kwh", zone->rate_buying_per_kwh, 6);
                        /* The feed-in side is serialized too: without it, revenue appeared "from nowhere"
                         * (a zone selling rate in the built-in plan while the document showed only buying).
                         * Both are what the costing actually uses. */
                        cJSON_AddRoundedNumber(zone_obj, "rate_selling_per_kwh", zone->rate_selling_per_kwh, 6);
                        cJSON_AddRoundedNumber(zone_obj, "net_metering_ratio", zone->net_metering_ratio, 4);

                        // Multipliers (rounded to 4 decimals)
                        cJSON_AddRoundedNumber(zone_obj, "base_multiplier", zone->base_multiplier, 4);
                        cJSON_AddRoundedNumber(zone_obj, "seasonal_multiplier", zone->seasonal_multiplier, 4);
                        cJSON_AddRoundedNumber(zone_obj, "critical_peak_multiplier", zone->critical_peak_multiplier, 4);
                        
                        // === ACTIVATION CONDITIONS ===
                        cJSON_AddBoolToObject(zone_obj, "conditions_and_logic", zone->conditions_and_logic);   /* round-trips through GET/POST */
                        cJSON* conditions = cJSON_CreateArray();
                        if (conditions) {
                            /* only the slots the parser counted — a slot beyond conditions_count is stale
                             * (a deleted condition keeps is_active and would reappear as a ghost in GET) */
                            for (int j = 0; j < 4 && j < zone->conditions_count; j++) {
                                const rbpower_tariff_condition_t* cond = &zone->conditions[j];
                                
                                // Skip inactive conditions
                                if (!cond->is_active) continue;
                                
                                cJSON* cond_obj = cJSON_CreateObject();
                                if (cond_obj) {
                                    // Condition type
                                    const char* cond_type_str = "TIME_RANGE";
                                    if (cond->condition_type >= 0 && cond->condition_type <= RBPOWER_TARIFF_CONDITION_SPECIAL_EVENT) {
                                        cond_type_str = rbpower_tariff_condition_type_text[cond->condition_type];
                                    }
                                    cJSON_AddStringToObject(cond_obj, "condition_type", cond_type_str);
                                    
                                    // Parameters depending on the condition type
                                    cJSON* params = cJSON_CreateObject();
                                    if (params) {
                                        switch (cond->condition_type) {
                                            case RBPOWER_TARIFF_CONDITION_TIME_RANGE:
                                                cJSON_AddNumberToObject(params, "start_hour", cond->params.time_range.start_hour);
                                                cJSON_AddNumberToObject(params, "start_minute", cond->params.time_range.start_minute);
                                                cJSON_AddNumberToObject(params, "end_hour", cond->params.time_range.end_hour);
                                                cJSON_AddNumberToObject(params, "end_minute", cond->params.time_range.end_minute);
                                                cJSON_AddBoolToObject(params, "spans_midnight", cond->params.time_range.spans_midnight);
                                                break;
                                                
                                            case RBPOWER_TARIFF_CONDITION_WEEKDAY:
                                                cJSON_AddNumberToObject(params, "weekday_mask", cond->params.weekday.weekday_mask);
                                                break;
                                                
                                            case RBPOWER_TARIFF_CONDITION_SEASON:
                                                // No such member in the params union; seasons use date_range
                                                break;
                                                
                                            case RBPOWER_TARIFF_CONDITION_DATE_RANGE:
                                                cJSON_AddNumberToObject(params, "start_month", cond->params.date_range.start_month);
                                                cJSON_AddNumberToObject(params, "start_day", cond->params.date_range.start_day);
                                                cJSON_AddNumberToObject(params, "end_month", cond->params.date_range.end_month);
                                                cJSON_AddNumberToObject(params, "end_day", cond->params.date_range.end_day);
                                                cJSON_AddBoolToObject(params, "recurring_annually", cond->params.date_range.recurring_annually);
                                                break;
                                                
                                            case RBPOWER_TARIFF_CONDITION_SPECIAL_EVENT:
                                                cJSON_AddNumberToObject(params, "event_start_timestamp", (double)cond->params.special_event.event_start_timestamp);
                                                cJSON_AddNumberToObject(params, "event_end_timestamp", (double)cond->params.special_event.event_end_timestamp);
                                                cJSON_AddNumberToObject(params, "event_id", cond->params.special_event.event_id);
                                                break;
                                                
                                            default:
                                                break;
                                        }
                                        cJSON_AddItemToObject(cond_obj, "params", params);
                                    }
                                    
                                    cJSON_AddBoolToObject(cond_obj, "is_active", cond->is_active);
                                    cJSON_AddNumberToObject(cond_obj, "priority", cond->priority);
                                    
                                    cJSON_AddItemToArray(conditions, cond_obj);
                                }
                            }
                            cJSON_AddItemToObject(zone_obj, "conditions", conditions);
                        }
                        
                        // Status
                        cJSON_AddBoolToObject(zone_obj, "is_enabled", zone->is_enabled);
                        {   /* derive from the active-zone cache (the plan's own flag lags a re-parse) */
                            bool active_now = zone->is_currently_active;
                            int cur_zid = -1;
                            rbpower_tariff_get_diag(NULL, NULL, NULL, NULL, &cur_zid, NULL, NULL, NULL);
                            if (cur_zid >= 0) active_now = ((int)zone->zone_id == cur_zid);
                            cJSON_AddBoolToObject(zone_obj, "is_currently_active", active_now);
                        }
                        cJSON_AddNumberToObject(zone_obj, "last_activation_timestamp", (double)zone->last_activation_timestamp);
                        cJSON_AddNumberToObject(zone_obj, "activations_count_today", zone->activations_count_today);
                        
                        cJSON_AddItemToArray(zones, zone_obj);
                    }
                }
                cJSON_AddItemToObject(plan, "zones", zones);
            }
            
            // === PROGRESSIVE (TIERED) CONFIG ===
            cJSON* progressive_config = cJSON_CreateObject();
            if (progressive_config) {
                const rbpower_tariff_progressive_config_t* prog = &tariff->progressive_config;
                cJSON_AddBoolToObject(progressive_config, "is_enabled", prog->is_enabled);
                cJSON_AddNumberToObject(progressive_config, "tiers_count", prog->tiers_count);
                
                cJSON* thresholds = cJSON_CreateArray();
                if (thresholds) {
                    for (int i = 0; i < prog->tiers_count && i < RBPOWER_TARIFF_PROGRESSIVE_TIERS; i++) {
                        cJSON* tier = cJSON_CreateObject();
                        if (tier) {
                            cJSON_AddNumberToObject(tier, "threshold_kwh", prog->tiers[i].threshold_kwh);
                            cJSON_AddRoundedNumber(tier, "rate_multiplier", prog->tiers[i].rate_multiplier, 4);
                            cJSON_AddRoundedNumber(tier, "fixed_rate_per_kwh", prog->tiers[i].fixed_rate_per_kwh, 6);
                            cJSON_AddBoolToObject(tier, "use_multiplier", prog->tiers[i].use_multiplier);
                            cJSON_AddStringToObject(tier, "tier_name", prog->tiers[i].tier_name);
                            cJSON_AddItemToArray(thresholds, tier);
                        }
                    }
                    cJSON_AddItemToObject(progressive_config, "thresholds", thresholds);
                }
                
                cJSON_AddStringToObject(progressive_config, "calculation_period", 
                    prog->calculation_period == RBPOWER_STAT_PERIOD_DAILY ? "DAILY" :
                    prog->calculation_period == RBPOWER_STAT_PERIOD_MONTHLY ? "MONTHLY" :
                    prog->calculation_period == RBPOWER_STAT_PERIOD_YEARLY ? "YEARLY" : "MONTHLY");
                
                cJSON_AddBoolToObject(progressive_config, "reset_monthly", prog->reset_monthly);
                cJSON_AddNumberToObject(progressive_config, "reset_day", prog->reset_day);
                
                cJSON_AddItemToObject(plan, "progressive_config", progressive_config);
            }
            
            // === FIXED PAYMENTS ===
            cJSON* fixed_payments = cJSON_CreateArray();
            if (fixed_payments) {
                for (int i = 0; i < tariff->fixed_payments_count && i < 8; i++) {
                    const rbpower_tariff_fixed_payment_t* payment = &tariff->fixed_payments[i];
                    
                    cJSON* payment_obj = cJSON_CreateObject();
                    if (payment_obj) {
                        cJSON_AddStringToObject(payment_obj, "payment_type", "FIXED");
                        cJSON_AddStringToObject(payment_obj, "payment_name", payment->payment_name);
                        cJSON_AddNumberToObject(payment_obj, "amount", payment->amount);
                        cJSON_AddStringToObject(payment_obj, "currency", payment->currency);
                        
                        cJSON_AddBoolToObject(payment_obj, "is_enabled", payment->is_enabled);
                        cJSON_AddStringToObject(payment_obj, "billing_period",
                            payment->billing_period == RBPOWER_STAT_PERIOD_DAILY ? "DAILY" :
                            payment->billing_period == RBPOWER_STAT_PERIOD_MONTHLY ? "MONTHLY" :
                            payment->billing_period == RBPOWER_STAT_PERIOD_YEARLY ? "YEARLY" : "MONTHLY");
                        cJSON_AddNumberToObject(payment_obj, "billing_day", payment->billing_day);
                        
                        cJSON_AddItemToArray(fixed_payments, payment_obj);
                    }
                }
                cJSON_AddItemToObject(plan, "fixed_payments", fixed_payments);
            }
            
            cJSON_AddNumberToObject(plan, "fixed_payments_count", tariff->fixed_payments_count);
            
            // === BASE SETTINGS ===
            cJSON_AddStringToObject(plan, "currency_code", tariff->currency_code);
            cJSON_AddStringToObject(plan, "region_code", tariff->region_code);
            
            // Taxes and levies (rounded)
            cJSON_AddRoundedNumber(plan, "vat_percent", tariff->vat_percent, 2);
            cJSON_AddRoundedNumber(plan, "energy_tax_per_kwh", tariff->energy_tax_per_kwh, 6);
            cJSON_AddRoundedNumber(plan, "grid_usage_fee_per_kwh", tariff->grid_usage_fee_per_kwh, 6);
            cJSON_AddRoundedNumber(plan, "renewable_levy_per_kwh", tariff->renewable_levy_per_kwh, 6);
            
            // Net metering
            cJSON_AddBoolToObject(plan, "net_metering_enabled", tariff->net_metering_enabled);
            cJSON_AddRoundedNumber(plan, "net_metering_default_ratio", tariff->net_metering_default_ratio, 4);

            // Default selling rate for the entire plan
            // This field should be:
            // - 0 when using ratio-based calculation (net_metering_default_ratio > 0)
            // - Fixed rate value when using fixed rate mode (net_metering_default_ratio == 0)
            float default_selling_rate = 0.0f;

            // Only set non-zero value if:
            // 1. Net metering is enabled
            // 2. We're NOT using ratio-based calculation (ratio == 0)
            // 3. Zones have a consistent fixed selling rate
            if (tariff->net_metering_enabled && tariff->net_metering_default_ratio == 0) {
                // Check all zones for a consistent fixed selling rate
                float first_rate = -1.0f;
                bool rates_consistent = true;

                for (uint8_t i = 0; i < tariff->active_zones_count; i++) {
                    if (!tariff->zones[i].is_enabled) continue;

                    if (first_rate < 0) {
                        first_rate = tariff->zones[i].rate_selling_per_kwh;
                    } else if (fabs(tariff->zones[i].rate_selling_per_kwh - first_rate) > 0.0001f) {
                        // Rates are not consistent across zones
                        rates_consistent = false;
                        break;
                    }
                }

                if (rates_consistent && first_rate > 0) {
                    default_selling_rate = first_rate;
                }
            }
            // For ratio-based calculation (net_metering_default_ratio > 0), keep default_selling_rate as 0

            cJSON_AddRoundedNumber(plan, "default_selling_rate_per_kwh", default_selling_rate, 6);

            cJSON_AddStringToObject(plan, "net_metering_period",
                tariff->net_metering_period == RBPOWER_STAT_PERIOD_DAILY ? "DAILY" :
                tariff->net_metering_period == RBPOWER_STAT_PERIOD_MONTHLY ? "MONTHLY" :
                tariff->net_metering_period == RBPOWER_STAT_PERIOD_YEARLY ? "YEARLY" : "MONTHLY");
            cJSON_AddBoolToObject(plan, "carry_over_credits", tariff->carry_over_credits);
            cJSON_AddNumberToObject(plan, "max_credit_carry_over_kwh", tariff->max_credit_carry_over_kwh);
            
            // Supported features
            cJSON_AddNumberToObject(plan, "supported_features", tariff->supported_features);
            cJSON_AddStringToObject(plan, "calculation_method", "FLAT");
            
            // Optimization
            cJSON_AddStringToObject(plan, "optimization_priority", "COST");
            cJSON_AddBoolToObject(plan, "enable_load_shifting_recommendations", tariff->enable_load_shifting_recommendations);
            cJSON_AddNumberToObject(plan, "load_shifting_threshold_percent", tariff->load_shifting_threshold_percent);
            
            // Plan validity
            cJSON_AddNumberToObject(plan, "valid_from_timestamp", (double)tariff->valid_from_timestamp);
            cJSON_AddNumberToObject(plan, "valid_until_timestamp", (double)tariff->valid_until_timestamp);
            cJSON_AddBoolToObject(plan, "is_active", tariff->is_active);
            cJSON_AddBoolToObject(plan, "is_default", tariff->is_default);
            
            // Metadata
            cJSON_AddNumberToObject(plan, "plan_version", tariff->plan_version);
            cJSON_AddNumberToObject(plan, "created_timestamp", (double)tariff->created_timestamp);
            cJSON_AddNumberToObject(plan, "last_modified_timestamp", (double)tariff->last_modified_timestamp);
            
            cJSON_AddItemToArray(list, plan);
        }
        
        cJSON_AddItemToObject(data, "list", list);
    }

    cJSON_AddItemToObject(root, "data", data);

    ESP_LOGI(TAG, "tariff_plan_list_config JSON created successfully");

    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);

    return json_string;
}




const char* rbgrid_json_status_to_string(int status) {
    // TODO: define the real status codes
    switch (status) {
        case 0: return "active";
        case 1: return "inactive";
        case 2: return "error";
        case 3: return "calibrating";
        default: return "unknown";
    }
}

const char* rbgrid_json_error_to_string(rbgrid_json_err_t error) {
    switch (error) {
        case RBGRID_JSON_OK: return "Success";
        case RBGRID_JSON_ERR_INVALID_ARG: return "Invalid argument";
        case RBGRID_JSON_ERR_NO_MEMORY: return "No memory";
        case RBGRID_JSON_ERR_INVALID_JSON: return "Invalid JSON";
        case RBGRID_JSON_ERR_MISSING_FIELD: return "Missing field";
        case RBGRID_JSON_ERR_TYPE_MISMATCH: return "Type mismatch";
        case RBGRID_JSON_ERR_INVALID_CONFIG: return "Invalid configuration";
        case RBGRID_JSON_ERR_SYSTEM_ERROR: return "System error";
        default: return "Unknown error";
    }
}

/* ================================================================================================
 * ENUM TO STRING HELPERS
 * ================================================================================================ */

/** @brief Lower-case name of a metric type. @param[in] type Metric type. @return Static string ("unknown" when out of range). */
const char* rbpower_metric_type_to_string(rbpower_metric_type_t type) {
    switch (type) {
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

/* Canonical role enum for the config surface (/api/v1/schema `enums.role` + units-config round-trip).
 * Returns the UPPER_CASE name for index idx, NULL past the end — lets the schema builder enumerate
 * without duplicating the list. */
const char* rbpower_load_role_name(unsigned idx) {
    if (idx >= sizeof(rbpower_load_role_text) / sizeof(rbpower_load_role_text[0])) return NULL;
    return rbpower_load_role_text[idx];
}

/** @brief Lower-case name of a network role. @param[in] role Role. @return Static string ("unknown" when out of range). */
const char* rbpower_load_role_to_string(rbpower_load_role_t role) {
    switch (role) {
        case RBPOWER_ROLE_MAIN_SUPPLY: return "main_supply";
        case RBPOWER_ROLE_ALTERNATIVE_SUPPLY: return "alternative_supply"; /* was missing -> "unknown" in telemetry */
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

/** @brief Lower-case name of a statistics period. @param[in] period Period type. @return Static string ("unknown" when out of range). */
const char* rbpower_stat_period_to_string(rbpower_stat_period_type_t period) {
    switch (period) {
        case RBPOWER_STAT_PERIOD_3SEC: return "3sec";
        case RBPOWER_STAT_PERIOD_15MIN: return "15min";
        case RBPOWER_STAT_PERIOD_1HOUR: return "1hour";
        case RBPOWER_STAT_PERIOD_DAILY: return "daily";
        case RBPOWER_STAT_PERIOD_WEEKLY: return "weekly";
        case RBPOWER_STAT_PERIOD_MONTHLY: return "monthly";
        case RBPOWER_STAT_PERIOD_YEARLY: return "yearly";
        default: return "unknown";
    }
}

const char* rbpower_current_direction_to_string(rbpower_current_direction_t direction) {
    switch (direction) {
        case RBPOWER_CURRENT_DIRECTION_UNKNOWN: return "unknown";
        case RBPOWER_CURRENT_DIRECTION_CONSUMING: return "consuming";
        case RBPOWER_CURRENT_DIRECTION_SUPPLYING: return "supplying";
        case RBPOWER_CURRENT_DIRECTION_BIDIRECTIONAL: return "bidirectional";
        case RBPOWER_CURRENT_DIRECTION_ZERO: return "zero";
        default: return "unknown";
    }
}

/* ================================================================================================
 * DASHBOARD STRUCTURE GENERATION
 * ================================================================================================ */

/* Dashboard structure of the home electrical network ("dashboard_structure" message).
 * Returns a heap JSON string (caller frees) or NULL on error. */
char* rbpower_create_dashboard_structure_json(void) {
    // Get the home network structure
    rbpower_electrical_network_t* network = NULL;
    rbpower_err_t err = rbpower_get_network(&network);
    if (err != RBPOWER_OK || network == NULL) {
        ESP_LOGE(TAG, "Failed to get network structure: %d", err);
        return NULL;
    }
    
    // Create base message
    cJSON* root = rbgrid_json_create_base_message("dashboard_structure", g_json_context);
    if (!root) {
        ESP_LOGE(TAG, "Failed to create base message");
        return NULL;
    }
    
    // Create data object
    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "data", data);
    
    // Calculate update age
    uint64_t current_time_us = esp_timer_get_time();
    int last_update_age_seconds = (int)((current_time_us - network->last_update_us) / 1000000);
    
    // 1. Create system_overview
    cJSON* system_overview = cJSON_CreateObject();
    if (system_overview) {
        cJSON_AddNumberToObject(system_overview, "units_total", network->units_count);
        cJSON_AddNumberToObject(system_overview, "units_active", network->aggregated.active_units_count);
        
        // Count online units
        int units_online = 0;
        for (int i = 0; i < network->units_count; i++) {
            if (network->units[i].has_realtime_data && network->units[i].has_valid_definition) {
                units_online++;
            }
        }
        cJSON_AddNumberToObject(system_overview, "units_online", units_online);
        
        cJSON_AddBoolToObject(system_overview, "system_healthy", network->system_healthy);
        cJSON_AddNumberToObject(system_overview, "last_update_age_seconds", last_update_age_seconds);
        cJSON_AddBoolToObject(system_overview, "has_voltage_bus", 
                             network->system.voltage_bus_unit_id != RBPOWER_HOME_INVALID_ID);
        cJSON_AddBoolToObject(system_overview, "has_main_supply", 
                             network->system.main_supply_unit_id != RBPOWER_HOME_INVALID_ID);
        cJSON_AddBoolToObject(system_overview, "has_alternative_supply", 
                             network->system.alternative_supply_unit_id != RBPOWER_HOME_INVALID_ID);
        
        cJSON_AddItemToObject(data, "system_overview", system_overview);
    }
    
    // 2. Create voltage_bus
    cJSON* voltage_bus = cJSON_CreateObject();
    if (voltage_bus) {
        bool available = network->system.voltage_bus_unit_id != RBPOWER_HOME_INVALID_ID;
        cJSON_AddBoolToObject(voltage_bus, "available", available);
        
        if (available) {
            uint8_t unit_id = network->system.voltage_bus_unit_id;
            cJSON_AddNumberToObject(voltage_bus, "unit_id", unit_id);
            
            // Find the unit
            for (int i = 0; i < network->units_count; i++) {
                if (network->units[i].definition.unit_id == unit_id && 
                    network->units[i].has_valid_definition) {
                    rbpower_home_unit_t* unit = &network->units[i];
                    cJSON_AddStringToObject(voltage_bus, "unit_name", unit->definition.name);
                    cJSON_AddStringToObject(voltage_bus, "unit_location", unit->definition.location);
                    cJSON_AddBoolToObject(voltage_bus, "data_valid", unit->has_realtime_data);
                    cJSON_AddBoolToObject(voltage_bus, "is_active", unit->definition.is_active);
                    
                    int unit_update_age = (int)((current_time_us - unit->last_update_us) / 1000000);
                    cJSON_AddNumberToObject(voltage_bus, "last_update_age_seconds", unit_update_age);
                    cJSON_AddNumberToObject(voltage_bus, "rated_voltage", unit->definition.rated_voltage_v);
                    
                    cJSON* thresholds = cJSON_CreateObject();
                    if (thresholds) {
                        cJSON_AddNumberToObject(thresholds, "under_voltage", 
                                              unit->definition.thresholds.under_voltage_v);
                        cJSON_AddNumberToObject(thresholds, "over_voltage", 
                                              unit->definition.thresholds.over_voltage_v);
                        cJSON_AddItemToObject(voltage_bus, "thresholds", thresholds);
                    }
                    break;
                }
            }
        }
        
        cJSON_AddItemToObject(data, "voltage_bus", voltage_bus);
    }
    
    // 3. Create main_supply
    cJSON* main_supply = cJSON_CreateObject();
    if (main_supply) {
        bool available = network->system.main_supply_unit_id != RBPOWER_HOME_INVALID_ID;
        cJSON_AddBoolToObject(main_supply, "available", available);
        
        if (available) {
            uint8_t unit_id = network->system.main_supply_unit_id;
            cJSON_AddNumberToObject(main_supply, "unit_id", unit_id);
            
            // Find the unit
            for (int i = 0; i < network->units_count; i++) {
                if (network->units[i].definition.unit_id == unit_id && 
                    network->units[i].has_valid_definition) {
                    rbpower_home_unit_t* unit = &network->units[i];
                    cJSON_AddStringToObject(main_supply, "unit_name", unit->definition.name);
                    cJSON_AddStringToObject(main_supply, "unit_location", unit->definition.location);
                    cJSON_AddBoolToObject(main_supply, "data_valid", unit->has_realtime_data);
                    cJSON_AddBoolToObject(main_supply, "is_active", unit->definition.is_active);
                    
                    int unit_update_age = (int)((current_time_us - unit->last_update_us) / 1000000);
                    cJSON_AddNumberToObject(main_supply, "last_update_age_seconds", unit_update_age);
                    cJSON_AddNumberToObject(main_supply, "rated_power", unit->definition.rated_power_w);
                    cJSON_AddNumberToObject(main_supply, "max_current", unit->definition.max_current_a);
                    cJSON_AddBoolToObject(main_supply, "bidirectional_metering", 
                                        unit->definition.bidirectional_metering);
                    cJSON_AddBoolToObject(main_supply, "statistics_enabled", 
                                        unit->has_statistics_enabled);
                    
                    cJSON* thresholds = cJSON_CreateObject();
                    if (thresholds) {
                        cJSON_AddNumberToObject(thresholds, "over_current", 
                                              unit->definition.thresholds.over_current_a);
                        cJSON_AddNumberToObject(thresholds, "over_power", 
                                              unit->definition.thresholds.over_power_w);
                        cJSON_AddItemToObject(main_supply, "thresholds", thresholds);
                    }
                    break;
                }
            }
        }
        
        cJSON_AddItemToObject(data, "main_supply", main_supply);
    }
    
    // 4. Create alternative_supply
    cJSON* alternative_supply = cJSON_CreateObject();
    if (alternative_supply) {
        bool available = network->system.alternative_supply_unit_id != RBPOWER_HOME_INVALID_ID;
        cJSON_AddBoolToObject(alternative_supply, "available", available);
        
        if (available) {
            uint8_t unit_id = network->system.alternative_supply_unit_id;
            cJSON_AddNumberToObject(alternative_supply, "unit_id", unit_id);
            
            // Find the unit
            for (int i = 0; i < network->units_count; i++) {
                if (network->units[i].definition.unit_id == unit_id && 
                    network->units[i].has_valid_definition) {
                    rbpower_home_unit_t* unit = &network->units[i];
                    cJSON_AddStringToObject(alternative_supply, "unit_name", unit->definition.name);
                    cJSON_AddStringToObject(alternative_supply, "unit_location", unit->definition.location);
                    cJSON_AddBoolToObject(alternative_supply, "data_valid", unit->has_realtime_data);
                    cJSON_AddBoolToObject(alternative_supply, "is_active", unit->definition.is_active);
                    cJSON_AddNumberToObject(alternative_supply, "rated_power", unit->definition.rated_power_w);
                    cJSON_AddStringToObject(alternative_supply, "role", 
                                          rbpower_load_role_to_string(unit->definition.role));
                    cJSON_AddBoolToObject(alternative_supply, "statistics_enabled", 
                                        unit->has_statistics_enabled);
                    break;
                }
            }
        }
        
        cJSON_AddItemToObject(data, "alternative_supply", alternative_supply);
    }
    
    // 5. Create consumers array
    cJSON* consumers = cJSON_CreateArray();
    if (consumers) {
        // Add all units that are not voltage bus or main supply
        for (int i = 0; i < network->units_count; i++) {
            rbpower_home_unit_t* unit = &network->units[i];
            
            // Skip if not valid definition
            if (!unit->has_valid_definition) continue;
            
            // Skip voltage bus and main supply
            if (unit->definition.role == RBPOWER_ROLE_MAIN_SUPPLY || 
                unit->definition.type == RBPOWER_METRIC_VOLTAGE_BUS) {
                continue;
            }
            
            cJSON* consumer = cJSON_CreateObject();
            if (consumer) {
                cJSON_AddNumberToObject(consumer, "unit_id", unit->definition.unit_id);
                cJSON_AddStringToObject(consumer, "unit_name", unit->definition.name);
                cJSON_AddStringToObject(consumer, "unit_location", unit->definition.location);
                cJSON_AddStringToObject(consumer, "role", 
                                      rbpower_load_role_to_string(unit->definition.role));
                cJSON_AddStringToObject(consumer, "type", 
                                      rbpower_metric_type_to_string(unit->definition.type));
                cJSON_AddBoolToObject(consumer, "is_active", unit->definition.is_active);
                cJSON_AddBoolToObject(consumer, "data_valid", unit->has_realtime_data);
                cJSON_AddBoolToObject(consumer, "statistics_enabled", unit->has_statistics_enabled);
                cJSON_AddNumberToObject(consumer, "parent_unit_id", unit->definition.parent_unit_id);
                cJSON_AddNumberToObject(consumer, "children_count", unit->definition.children_count);
                cJSON_AddBoolToObject(consumer, "has_children", unit->definition.children_count > 0);
                cJSON_AddNumberToObject(consumer, "rated_power", unit->definition.rated_power_w);
                cJSON_AddNumberToObject(consumer, "max_current", unit->definition.max_current_a);
                cJSON_AddBoolToObject(consumer, "bidirectional_metering", 
                                    unit->definition.bidirectional_metering);
                cJSON_AddBoolToObject(consumer, "accumulate_energy", 
                                    unit->definition.accumulate_energy);
                
                int unit_update_age = (int)((current_time_us - unit->last_update_us) / 1000000);
                cJSON_AddNumberToObject(consumer, "last_update_age_seconds", unit_update_age);
                
                cJSON_AddStringToObject(consumer, "stat_class", 
                                      rbpower_stat_period_to_string(unit->definition.stat_class));
                cJSON_AddNumberToObject(consumer, "sources_count", unit->definition.sources_count);
                
                // Add thresholds
                cJSON* thresholds = cJSON_CreateObject();
                if (thresholds) {
                    cJSON_AddNumberToObject(thresholds, "over_current", 
                                          unit->definition.thresholds.over_current_a);
                    cJSON_AddNumberToObject(thresholds, "over_power", 
                                          unit->definition.thresholds.over_power_w);
                    cJSON_AddBoolToObject(thresholds, "enable_alerts", 
                                        unit->definition.thresholds.enable_alerts);
                    cJSON_AddItemToObject(consumer, "thresholds", thresholds);
                }
                
                cJSON_AddItemToArray(consumers, consumer);
            }
        }
        
        cJSON_AddItemToObject(data, "consumers", consumers);
    }
    
    // 6. Create system_status
    cJSON* system_status = cJSON_CreateObject();
    if (system_status) {
        cJSON_AddBoolToObject(system_status, "synchronized", network->is_synchronized);
        cJSON_AddBoolToObject(system_status, "healthy", network->system_healthy);
        cJSON_AddNumberToObject(system_status, "update_counter", network->update_counter);
        cJSON_AddNumberToObject(system_status, "uptime_seconds", network->system_uptime_s);
        
        // Age of the network's ZC event timestamp (legacy dashboard field)
        int zc_age_seconds = (int)((current_time_us - network->timestamp_ZC_event) / 1000000);
        cJSON_AddNumberToObject(system_status, "last_zc_event_age_seconds", zc_age_seconds);
        
        cJSON_AddItemToObject(data, "system_status", system_status);
    }
    
    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_string;
}

/* ================================================================================================
 * 7. TELEMETRY CONFIG
 * ================================================================================================ */

/* Telemetry config ("telemetry_config" message) from extended_config->telemetry_config.
 * Caller frees the returned string. */
char* rbgrid_json_create_telemetry_config(const rbpower_extended_config_t* extended_config,
                                           const rbgrid_json_context_t* context) {
    if (!extended_config || !context) {
        ESP_LOGE(TAG, "Invalid arguments for telemetry config");
        return NULL;
    }
    
    const rbpower_telemetry_config_t* telemetry_config = &extended_config->telemetry_config;

    ESP_LOGI(TAG, "Creating telemetry_config JSON");

    cJSON* root = rbgrid_json_create_base_message("telemetry_config", context);
    if (!root) {
        return NULL;
    }

    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    // === MAIN SETTINGS ===
    cJSON_AddBoolToObject(data, "telemetry_enabled", telemetry_config->telemetry_enabled);
    cJSON_AddNumberToObject(data, "json_buffer_size", telemetry_config->json_buffer_size);
    cJSON_AddBoolToObject(data, "compact_json", telemetry_config->compact_json);

    // === TELEMETRY CHANNELS ===
    cJSON* channels = cJSON_CreateArray();
    if (channels) {
        // All channels (MQTT, HTTP, WebSocket)
        const char* protocol_names[] = {"MQTT", "HTTP", "WEBSOCKET"};
        
        for (int i = 0; i < RBPOWER_TELEMETRY_PROTOCOLS_MAX; i++) {
            const rbpower_telemetry_channel_t* channel = &telemetry_config->channels[i];
            
            cJSON* channel_obj = cJSON_CreateObject();
            if (channel_obj) {
                cJSON_AddStringToObject(channel_obj, "protocol", protocol_names[i]);
                cJSON_AddBoolToObject(channel_obj, "enabled", channel->enabled);
                cJSON_AddNumberToObject(channel_obj, "send_interval_ms", channel->send_interval_ms);
                cJSON_AddNumberToObject(channel_obj, "retry_count", channel->retry_count);
                cJSON_AddNumberToObject(channel_obj, "timeout_ms", channel->timeout_ms);
                
                // === PROTOCOL-SPECIFIC CONFIG ===
                cJSON* config = cJSON_CreateObject();
                if (config) {
                    switch (i) {
                        case RBPOWER_TELEMETRY_MQTT:
                            if (channel->config.mqtt.broker_url) {
                                cJSON_AddStringToObject(config, "broker_url", channel->config.mqtt.broker_url);
                            } else {
                                cJSON_AddStringToObject(config, "broker_url", "");
                            }
                            
                            if (channel->config.mqtt.topic) {
                                cJSON_AddStringToObject(config, "topic", channel->config.mqtt.topic);
                            } else {
                                cJSON_AddStringToObject(config, "topic", "");
                            }
                            
                            cJSON_AddNumberToObject(config, "qos", channel->config.mqtt.qos);
                            break;
                            
                        case RBPOWER_TELEMETRY_HTTP:
                            if (channel->config.http.endpoint_url) {
                                cJSON_AddStringToObject(config, "endpoint_url", channel->config.http.endpoint_url);
                            } else {
                                cJSON_AddStringToObject(config, "endpoint_url", "");
                            }
                            
                            if (channel->config.http.auth_token) {
                                cJSON_AddStringToObject(config, "auth_token", channel->config.http.auth_token);
                            } else {
                                cJSON_AddStringToObject(config, "auth_token", "");
                            }
                            break;
                            
                        case RBPOWER_TELEMETRY_WEBSOCKET:
                            if (channel->config.websocket.server_url) {
                                cJSON_AddStringToObject(config, "server_url", channel->config.websocket.server_url);
                            } else {
                                cJSON_AddStringToObject(config, "server_url", "");
                            }
                            
                            cJSON_AddNumberToObject(config, "port", channel->config.websocket.port);
                            break;
                            
                        default:
                            break;
                    }
                    
                    cJSON_AddItemToObject(channel_obj, "config", config);
                }
                
                cJSON_AddItemToArray(channels, channel_obj);
            }
        }
        
        cJSON_AddItemToObject(data, "channels", channels);
    }

    cJSON_AddItemToObject(root, "data", data);

    ESP_LOGI(TAG, "telemetry_config JSON created successfully");
    
    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_string;
}

/* WiFi AP + STA state ("wifi_config" message). Caller frees the returned string. */
char* rbgrid_json_create_wifi_config(const rbgrid_json_context_t* context) {
    if (!context) {
        ESP_LOGE(TAG, "Invalid arguments for wifi config");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating wifi_config JSON");

    cJSON* root = rbgrid_json_create_base_message("wifi_config", context);
    if (!root) {
        return NULL;
    }

    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    // === OPERATING MODE ===
    cJSON_AddStringToObject(data, "mode", "APSTA");

    // === AP (Access Point) CONFIG ===
    cJSON* ap_config = cJSON_CreateObject();
    if (ap_config) {
        // Read the WiFi AP info
        wifi_config_t ap_wifi_config;
        esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &ap_wifi_config);
        
        if (err == ESP_OK) {
            cJSON_AddStringToObject(ap_config, "ssid", (char*)ap_wifi_config.ap.ssid);
            cJSON_AddNumberToObject(ap_config, "channel", ap_wifi_config.ap.channel);
            cJSON_AddNumberToObject(ap_config, "max_connection", ap_wifi_config.ap.max_connection);
            cJSON_AddBoolToObject(ap_config, "ssid_hidden", ap_wifi_config.ap.ssid_hidden);
            
            // AP IP info
            esp_netif_ip_info_t ap_ip_info;
            esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap_netif && esp_netif_get_ip_info(ap_netif, &ap_ip_info) == ESP_OK) {
                char ip_str[16];
                esp_ip4addr_ntoa(&ap_ip_info.ip, ip_str, sizeof(ip_str));
                cJSON_AddStringToObject(ap_config, "ip_address", ip_str);
                
                esp_ip4addr_ntoa(&ap_ip_info.netmask, ip_str, sizeof(ip_str));
                cJSON_AddStringToObject(ap_config, "netmask", ip_str);
                
                esp_ip4addr_ntoa(&ap_ip_info.gw, ip_str, sizeof(ip_str));
                cJSON_AddStringToObject(ap_config, "gateway", ip_str);
            }
        } else {
            cJSON_AddStringToObject(ap_config, "status", "not configured");
        }
        
        cJSON_AddItemToObject(data, "ap", ap_config);
    }

    // === STA (Station) CONFIG ===
    cJSON* sta_config = cJSON_CreateObject();
    if (sta_config) {
        // Read the WiFi STA info
        wifi_config_t sta_wifi_config;
        esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &sta_wifi_config);
        
        if (err == ESP_OK) {
            cJSON_AddStringToObject(sta_config, "ssid", (char*)sta_wifi_config.sta.ssid);
            
            // Connection status
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                cJSON_AddBoolToObject(sta_config, "connected", true);
                cJSON_AddNumberToObject(sta_config, "rssi", ap_info.rssi);
                cJSON_AddNumberToObject(sta_config, "channel", ap_info.primary);
                
                // BSSID
                char bssid_str[18];
                snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                        ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                        ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
                cJSON_AddStringToObject(sta_config, "bssid", bssid_str);
            } else {
                cJSON_AddBoolToObject(sta_config, "connected", false);
            }
            
            // STA IP info
            esp_netif_ip_info_t sta_ip_info;
            esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta_netif && esp_netif_get_ip_info(sta_netif, &sta_ip_info) == ESP_OK) {
                char ip_str[16];
                esp_ip4addr_ntoa(&sta_ip_info.ip, ip_str, sizeof(ip_str));
                cJSON_AddStringToObject(sta_config, "ip_address", ip_str);
                
                esp_ip4addr_ntoa(&sta_ip_info.netmask, ip_str, sizeof(ip_str));
                cJSON_AddStringToObject(sta_config, "netmask", ip_str);
                
                esp_ip4addr_ntoa(&sta_ip_info.gw, ip_str, sizeof(ip_str));
                cJSON_AddStringToObject(sta_config, "gateway", ip_str);
                
                // DNS servers
                esp_netif_dns_info_t dns_info;
                if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
                    esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, ip_str, sizeof(ip_str));
                    cJSON_AddStringToObject(sta_config, "dns_main", ip_str);
                }
                if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dns_info) == ESP_OK) {
                    esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, ip_str, sizeof(ip_str));
                    cJSON_AddStringToObject(sta_config, "dns_backup", ip_str);
                }
            }
        } else {
            cJSON_AddStringToObject(sta_config, "status", "not configured");
        }
        
        cJSON_AddItemToObject(data, "sta", sta_config);
    }

    cJSON_AddItemToObject(root, "data", data);

    ESP_LOGI(TAG, "wifi_config JSON created successfully");
    
    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_string;
}

/* ================================================================================================
 * rbAmp FLEET CONFIG — serialize / parse (persisted in file /rbamp.json)
 * Mirrors the units_config round-trip. Carries the bus-level + per-module address map that the
 * persisted units (units.json) don't hold. Bounds-checked on parse against RBAMP_MAX_*.
 * ================================================================================================ */
#define RBAMP_CONFIG_JSON_VERSION 1   ///< Format version written to /rbamp.json ("version")

char* rbgrid_json_create_rbamp_config(const rbamp_bridge_config_t* cfg) {
    if (!cfg) {
        ESP_LOGE(TAG, "create_rbamp_config: cfg is NULL");
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "type", "rbamp_config");
    cJSON_AddNumberToObject(root, "version", RBAMP_CONFIG_JSON_VERSION);

    // Bus-level params
    cJSON* bus = cJSON_CreateObject();
    if (!bus) { cJSON_Delete(root); return NULL; }
    cJSON_AddNumberToObject(bus, "i2c_port", cfg->i2c_port);
    cJSON_AddNumberToObject(bus, "sda", cfg->sda_gpio);
    cJSON_AddNumberToObject(bus, "scl", cfg->scl_gpio);
    cJSON_AddBoolToObject(bus, "pullup", cfg->internal_pullup);
    cJSON_AddNumberToObject(bus, "poll_ms", cfg->poll_period_ms);
    cJSON_AddNumberToObject(bus, "drdy_gpio", cfg->drdy_gpio_p1 ? (int)cfg->drdy_gpio_p1 - 1 : -1);
    cJSON_AddBoolToObject(bus, "log", cfg->log_readings);
    cJSON_AddBoolToObject(bus, "inject", cfg->inject_enabled);
    cJSON_AddNumberToObject(bus, "i_gain", cfg->current_gain);
    cJSON_AddNumberToObject(bus, "v_gain", cfg->voltage_gain);
    cJSON_AddBoolToObject(bus, "ct_persist", cfg->ct_persist);
    cJSON_AddItemToObject(root, "bus", bus);
    if (cfg->bus1_sda_p1 && cfg->bus1_scl_p1) {
        cJSON* b1 = cJSON_CreateObject();
        if (b1) {
            cJSON_AddNumberToObject(b1, "sda", cfg->bus1_sda_p1 - 1);
            cJSON_AddNumberToObject(b1, "scl", cfg->bus1_scl_p1 - 1);
            cJSON_AddItemToObject(root, "bus1", b1);
        }
    }

    // Module/entry map
    cJSON_AddNumberToObject(root, "module_count", cfg->module_count);
    cJSON* modules = cJSON_CreateArray();
    if (!modules) { cJSON_Delete(root); return NULL; }
    uint8_t mcount = cfg->module_count <= RBAMP_MAX_MODULES ? cfg->module_count : RBAMP_MAX_MODULES;
    for (uint8_t m = 0; m < mcount; m++) {
        const rbamp_module_cfg_t* mod = &cfg->modules[m];
        cJSON* mod_obj = cJSON_CreateObject();
        if (!mod_obj) continue;
        cJSON_AddNumberToObject(mod_obj, "addr", mod->addr);
        if (mod->bus) cJSON_AddNumberToObject(mod_obj, "bus", mod->bus);
        if (mod->drdy_p1) cJSON_AddNumberToObject(mod_obj, "drdy_gpio", mod->drdy_p1 - 1);
        if (mod->phase) cJSON_AddNumberToObject(mod_obj, "phase", mod->phase);
        cJSON_AddNumberToObject(mod_obj, "entry_count", mod->entry_count);
        cJSON* entries = cJSON_CreateArray();
        if (entries) {
            uint8_t ecount = mod->entry_count <= RBAMP_MAX_ENTRIES ? mod->entry_count : RBAMP_MAX_ENTRIES;
            for (uint8_t e = 0; e < ecount; e++) {
                const rbamp_entry_cfg_t* en = &mod->entries[e];
                cJSON* en_obj = cJSON_CreateObject();
                if (!en_obj) continue;
                cJSON_AddNumberToObject(en_obj, "source", en->source);
                cJSON_AddNumberToObject(en_obj, "ct_code", en->ct_code);
                cJSON_AddNumberToObject(en_obj, "unit_id", en->unit_id);
                if (en->ct_user) cJSON_AddNumberToObject(en_obj, "ct_user", 1);
                cJSON_AddItemToArray(entries, en_obj);
            }
            cJSON_AddItemToObject(mod_obj, "entries", entries);
        }
        {   /* owner of the ct_user models (hex), only when known */
            static const uint8_t z[12] = {0};
            if (memcmp(mod->ct_uid, z, 12) != 0) {
                char hx[25];
                for (int i = 0; i < 12; i++) snprintf(hx + 2 * i, 3, "%02x", mod->ct_uid[i]);
                cJSON_AddStringToObject(mod_obj, "ct_uid", hx);
            }
        }
        cJSON_AddItemToArray(modules, mod_obj);
    }
    cJSON_AddItemToObject(root, "modules", modules);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) ESP_LOGE(TAG, "create_rbamp_config: print failed");
    return out;
}

rbgrid_json_err_t rbgrid_json_parse_rbamp_config(const char* json_str, rbamp_bridge_config_t* out) {
    if (!json_str || !out) return RBGRID_JSON_ERR_INVALID_ARG;

    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "parse_rbamp_config: bad JSON");
        return RBGRID_JSON_ERR_INVALID_JSON;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "rbamp_config") != 0) {
        ESP_LOGE(TAG, "parse_rbamp_config: type != rbamp_config");
        cJSON_Delete(root);
        return RBGRID_JSON_ERR_INVALID_CONFIG;
    }

    // Start from a clean struct so absent fields are zero (no stale/garbage modules).
    rbamp_bridge_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cJSON* bus1 = cJSON_GetObjectItem(root, "bus1");
    if (bus1) {
        cJSON* sd = cJSON_GetObjectItem(bus1, "sda");
        cJSON* sc = cJSON_GetObjectItem(bus1, "scl");
        if (sd && sc && cJSON_IsNumber(sd) && cJSON_IsNumber(sc) && sd->valueint >= 0 && sd->valueint <= 33 &&
            sc->valueint >= 0 && sc->valueint <= 33 && sd->valueint != sc->valueint) {
            cfg.bus1_sda_p1 = (uint8_t)(sd->valueint + 1);
            cfg.bus1_scl_p1 = (uint8_t)(sc->valueint + 1);
        }
    }
    cJSON* bus = cJSON_GetObjectItem(root, "bus");
    if (bus) {
        cJSON* it;
        if ((it = cJSON_GetObjectItem(bus, "i2c_port")) && cJSON_IsNumber(it)) cfg.i2c_port = it->valueint;
        if ((it = cJSON_GetObjectItem(bus, "sda"))      && cJSON_IsNumber(it)) cfg.sda_gpio = it->valueint;
        if ((it = cJSON_GetObjectItem(bus, "scl"))      && cJSON_IsNumber(it)) cfg.scl_gpio = it->valueint;
        if ((it = cJSON_GetObjectItem(bus, "pullup")))   cfg.internal_pullup = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(bus, "poll_ms"))  && cJSON_IsNumber(it)) cfg.poll_period_ms = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(bus, "drdy_gpio")) && cJSON_IsNumber(it))
            cfg.drdy_gpio_p1 = (it->valuedouble >= 0 && it->valuedouble <= 39) ? (uint8_t)(it->valuedouble + 1) : 0;
        if ((it = cJSON_GetObjectItem(bus, "log")))      cfg.log_readings = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(bus, "inject")))   cfg.inject_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(bus, "i_gain"))   && cJSON_IsNumber(it)) cfg.current_gain = (float)it->valuedouble;
        if ((it = cJSON_GetObjectItem(bus, "v_gain"))   && cJSON_IsNumber(it)) cfg.voltage_gain = (float)it->valuedouble;
        if ((it = cJSON_GetObjectItem(bus, "ct_persist"))) cfg.ct_persist = cJSON_IsTrue(it);
    }

    cJSON* modules = cJSON_GetObjectItem(root, "modules");
    uint8_t m = 0;
    if (modules && cJSON_IsArray(modules)) {
        cJSON* mod_obj;
        cJSON_ArrayForEach(mod_obj, modules) {
            if (m >= RBAMP_MAX_MODULES) {   // never overflow the fixed array
                ESP_LOGW(TAG, "parse_rbamp_config: >%d modules, truncating", RBAMP_MAX_MODULES);
                break;
            }
            rbamp_module_cfg_t* mod = &cfg.modules[m];
            cJSON* it;
            if ((it = cJSON_GetObjectItem(mod_obj, "addr")) && cJSON_IsNumber(it)) mod->addr = (uint8_t)it->valueint;
            mod->drdy_p1 = ((it = cJSON_GetObjectItem(mod_obj, "drdy_gpio")) && cJSON_IsNumber(it) && it->valueint >= 0 &&
                            it->valueint <= 39) ? (uint8_t)(it->valueint + 1) : 0;
            mod->bus = ((it = cJSON_GetObjectItem(mod_obj, "bus")) && cJSON_IsNumber(it) && it->valueint == 1) ? 1 : 0;
            if ((it = cJSON_GetObjectItem(mod_obj, "phase")) && cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 3) mod->phase = (uint8_t)it->valueint;
            cJSON* entries = cJSON_GetObjectItem(mod_obj, "entries");
            uint8_t e = 0;
            if (entries && cJSON_IsArray(entries)) {
                cJSON* en_obj;
                cJSON_ArrayForEach(en_obj, entries) {
                    if (e >= RBAMP_MAX_ENTRIES) {
                        ESP_LOGW(TAG, "parse_rbamp_config: >%d entries, truncating", RBAMP_MAX_ENTRIES);
                        break;
                    }
                    rbamp_entry_cfg_t* en = &mod->entries[e];
                    if ((it = cJSON_GetObjectItem(en_obj, "source"))  && cJSON_IsNumber(it)) en->source  = (uint8_t)it->valueint;
                    if ((it = cJSON_GetObjectItem(en_obj, "ct_code")) && cJSON_IsNumber(it)) en->ct_code = (uint8_t)it->valueint;
                    if ((it = cJSON_GetObjectItem(en_obj, "unit_id")) && cJSON_IsNumber(it)) en->unit_id = (uint16_t)it->valueint;
                    en->ct_user = ((it = cJSON_GetObjectItem(en_obj, "ct_user")) && cJSON_IsNumber(it) && it->valueint) ? 1 : 0;
                    e++;
                }
            }
            mod->entry_count = e;   // authoritative = number actually parsed (not the stored field)
            if ((it = cJSON_GetObjectItem(mod_obj, "ct_uid")) && cJSON_IsString(it) && strlen(it->valuestring) == 24) {
                for (int i = 0; i < 12; i++) { unsigned v = 0; sscanf(it->valuestring + 2 * i, "%2x", &v); mod->ct_uid[i] = (uint8_t)v; }
            }
            m++;
        }
    }
    cfg.module_count = m;

    cJSON_Delete(root);
    *out = cfg;
    ESP_LOGI(TAG, "parse_rbamp_config: %u module(s) loaded", (unsigned)m);
    return RBGRID_JSON_OK;
}

/** @brief Add the human channel label "ch_label".
 *
 * A current channel is "I0", "I1"… (0-based, as `ch`), a PHASE is "L1"…"L3" (1-based, electrical
 * convention, as on the U3I3 housing). `ch` stays the 0-based config value; `ch_label`: "L<ch+1>" on a
 * phase-aware module (STD-U3xx), "I<ch>" otherwise, "V" for the voltage input, "L1–L3" for the
 * phase-grouped entry. */
static void rbjson_add_ch_human(cJSON* obj, int ch, int module)
{
    char lb[12];
    if (ch == 255)      { cJSON_AddStringToObject(obj, "ch_label", "V"); return; }
    if (ch == 254)      { cJSON_AddStringToObject(obj, "ch_label", "L1–L3"); return; }
    /* phase labels only on a multi-voltage (phase-aware) module — STD-U3xx: its channel c IS phase L(c+1) */
    rbpower_module_desc_t md;
    bool phase_mod = (module >= 0 && module < 255 && rbpower_get_module_desc((uint8_t)module, &md) && strstr(md.model, "U3") != NULL);
    if (phase_mod && ch >= 0 && ch <= 2) snprintf(lb, sizeof(lb), "L%d", ch + 1);
    else                                                             snprintf(lb, sizeof(lb), "I%d", ch);
    cJSON_AddStringToObject(obj, "ch_label", lb);
}

/* GET /api/v1/sensors — sensor discovery: one entry per configured unit with its interface, hardware
 * presence, addressing and bound unit_id, plus the UNBOUND rbAmp sensors as unit_id:null.
 * BaseResponse envelope (type "sensors_list"). */
char* rbgrid_json_create_sensors_list(const rbgrid_json_context_t* context) {
    if (!context) context = g_json_context;
    cJSON* root = rbgrid_json_create_base_message("sensors_list", context);
    if (!root) return NULL;
    cJSON* sensors = cJSON_CreateArray();
    if (!sensors) { cJSON_Delete(root); return NULL; }

    /* The sensor list is a REGISTRY view, not a pure unit view: it also surfaces
     * UNBOUND rbAmp sensors (fleet entry.unit_id==0, freed by a unit delete) as unit_id:null so the
     * configurator can see a re-bindable sensor. Load the persisted fleet (/rbamp.json) to (a) tag
     * bound i2c sensors with the module I2C addr and (b) enumerate the unbound ones. No /rbamp.json
     * (fresh device) → just the unit-derived view (all bound). */
    rbamp_bridge_config_t fleet;
    bool have_fleet = (rbgrid_storage_load_rbamp(&fleet) == ESP_OK);
    /* Per-module physical presence, inferred from any bound unit on that module — an unbound sibling
     * has no hardware_present carrier of its own, so it borrows the module's observed presence. */
    bool module_present[RBAMP_MAX_MODULES] = { false };
    uint64_t emitted_ch[RBAMP_MAX_MODULES] = { 0 };   /* (module, ch) already listed as bound in pass 1 */

    /* Pass 1 — BOUND sensors (one per valid unit) + addr for i2c. */
    for (int i = 0; i < g_home_network.units_count; i++) {
        const rbpower_home_unit_t* u = &g_home_network.units[i];
        if (!u->has_valid_definition) continue;
        rbpower_unit_id_t uid = u->definition.unit_id;
        cJSON* s = cJSON_CreateObject();
        if (!s) continue;
        char uid_str[8];
        snprintf(uid_str, sizeof(uid_str), "%04X", (unsigned)uid);
        cJSON_AddStringToObject(s, "unit_id", uid_str);   // bound (unbound sensors get null below)
        cJSON_AddBoolToObject(s, "present", u->hardware_present);
        uint8_t src_id = u->definition.sources[0].source_id;
        uint8_t prof   = u->definition.sources[0].sensor_profile_id;
        /* Classification + module/ch from the definition's explicit physics, not id bits
         * (ids are opaque for new units; legacy units carry backfilled fields). */
        uint8_t feed_module = 0xFF, feed_source = 0;   /* bridge-fed legacy-definition unit (0006 ← U3I3) */
        bool def_i2c = (u->definition.sources_count > 0 &&
                        u->definition.sources[0].source_type == RBPOWER_SOURCE_I2C_SENSOR);
        bool feed = !def_i2c && rbpower_get_unit_rbamp_feed(uid, &feed_module, &feed_source);
        if (def_i2c || feed) {
            uint8_t module = feed ? feed_module : (uint8_t)u->definition.device_uid;
            uint8_t ch     = feed ? feed_source : u->definition.channel_idx;   /* 255 = V, 254 = PHASES */
            cJSON_AddStringToObject(s, "iface", "i2c");
            cJSON_AddStringToObject(s, "src_class", "rbamp");   /* = fields_by_source key */
            cJSON_AddNumberToObject(s, "bus",     (have_fleet && module < fleet.module_count && module < RBAMP_MAX_MODULES) ? fleet.modules[module].bus : 0);
            cJSON_AddNumberToObject(s, "module",  (int)module);
            cJSON_AddNumberToObject(s, "ch",      (int)ch);
            rbjson_add_ch_human(s, (int)ch, (int)module);
            /* ct_code = the rbAmp hardware CT code from the fleet entry (1/2/3/6) — CONSISTENT with the
             * unbound sensors in pass 2 (which also emit the fleet ct_code). Falls back to the unit's
             * sensor_profile_id only if the fleet entry isn't found. (Previously emitted prof here,
             * which is the internal rbsensor_type enum — a different number from the unbound path.) */
            int ct_code = prof;
            if (have_fleet && module < fleet.module_count && module < RBAMP_MAX_MODULES) {
                const rbamp_module_cfg_t* fm = &fleet.modules[module];
                for (uint8_t fe = 0; fe < fm->entry_count && fe < RBAMP_MAX_ENTRIES; fe++)
                    if (fm->entries[fe].source == ch) { ct_code = fm->entries[fe].ct_code; break; }
                cJSON_AddNumberToObject(s, "addr", fm->addr);
            }
            cJSON_AddNumberToObject(s, "ct_code", ct_code);
            rbjson_add_ct_rated(s, (uint8_t)module, ct_code);
            rbjson_add_sct(s, (uint8_t)module, (int)ch);
            { rbpower_module_desc_t ld; if (module < RBAMP_MAX_MODULES && rbpower_get_module_desc((uint8_t)module, &ld)) cJSON_AddStringToObject(s, "line", ld.line == 1 ? "std" : "basic"); }
            if (u->hardware_present && module < RBAMP_MAX_MODULES) module_present[module] = true;
            if (module < RBAMP_MAX_MODULES && ch < 64) emitted_ch[module] |= (1ull << ch);
        } else {
            /* Not I2C-sourced: an ESP-NOW node unit, or a unit with a legacy ADC-typed definition
             * (there is no native ESP32-ADC measurement path; "adc"/"native_adc" are kept as API values). */
            cJSON_AddStringToObject(s, "iface", "adc");
            cJSON_AddStringToObject(s, "src_class",
                (u->definition.sources_count > 0 &&
                 u->definition.sources[0].source_type == RBPOWER_SOURCE_ESP_NOW)
                    ? "espnow" : "native_adc");         /* = fields_by_source key */
            cJSON_AddNumberToObject(s, "channel",     src_id);
            cJSON_AddNumberToObject(s, "sensor_type", prof);
        }
        cJSON_AddItemToArray(sensors, s);
    }

    /* Pass 2 — UNBOUND rbAmp sensors: current-channel fleet entries with unit_id==0 (a unit was
     * deleted; the sensor stays, re-bindable). unit_id is JSON null; present is inferred
     * from a bound sibling on the same module. Voltage feeds (RBAMP_ENTRY_VOLTAGE) aren't CT sensors. */
    if (have_fleet) {
        for (uint8_t m = 0; m < fleet.module_count && m < RBAMP_MAX_MODULES; m++) {
            const rbamp_module_cfg_t* mod = &fleet.modules[m];
            for (uint8_t e = 0; e < mod->entry_count && e < RBAMP_MAX_ENTRIES; e++) {
                const rbamp_entry_cfg_t* en = &mod->entries[e];
                if (en->source == RBAMP_ENTRY_VOLTAGE) continue;  /* voltage feed, not a CT sensor */
                if (en->unit_id != 0) {                           /* bound → already emitted in pass 1 … */
                    rbpower_home_unit_t* bu = NULL;
                    if (rbpower_get_unit_by_id((rbpower_unit_id_t)en->unit_id, &bu) == RBPOWER_OK && bu) continue;
                    /* … unless its unit is gone (a bind whose units.json save failed would otherwise make the
                     * channels vanish from the list after a reboot) — listed as unbound so it can be bound again */
                }
                if (emitted_ch[m] & (1ull << (en->source & 63))) continue;   /* channel already shown bound (live feed) — never list it twice */
                cJSON* s = cJSON_CreateObject();
                if (!s) continue;
                cJSON_AddNullToObject(s, "unit_id");              /* UNBOUND — re-bindable */
                cJSON_AddBoolToObject(s, "present", module_present[m]);
                cJSON_AddStringToObject(s, "iface", "i2c");
                cJSON_AddStringToObject(s, "src_class", "rbamp");  /* = fields_by_source key */
                cJSON_AddNumberToObject(s, "bus",     mod->bus);
                cJSON_AddNumberToObject(s, "module",  (int)m);
                cJSON_AddNumberToObject(s, "ch",      (int)en->source);
                rbjson_add_ch_human(s, (int)en->source, (int)m);
                cJSON_AddNumberToObject(s, "ct_code", en->ct_code);
                rbjson_add_ct_rated(s, m, en->ct_code);
                rbjson_add_sct(s, m, (int)en->source);
                { rbpower_module_desc_t ld; if (rbpower_get_module_desc(m, &ld)) cJSON_AddStringToObject(s, "line", ld.line == 1 ? "std" : "basic"); }
                cJSON_AddNumberToObject(s, "addr",    mod->addr);
                cJSON_AddItemToArray(sensors, s);
            }
        }
    }
    cJSON_AddItemToObject(root, "sensors", sensors);

    /* Metering MODULE descriptors — identity the bridge learned at probe
     * (model/variant/n_v/n_i/map/period mechanism/cal flag). Absent modules are listed with
     * present:false if they were ever probed; never-probed slots are skipped. */
    {
        cJSON* mods = cJSON_CreateArray();
        for (uint8_t m = 0; m < RBPOWER_MODULE_DESC_MAX && mods; m++) {
            rbpower_module_desc_t d;
            if (!rbpower_get_module_desc(m, &d)) continue;
            cJSON* o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddNumberToObject(o, "module", m);
            cJSON_AddNumberToObject(o, "addr", d.addr);
            cJSON_AddBoolToObject(o, "present", d.present);
            cJSON_AddStringToObject(o, "line", d.line ? "std" : "basic");
            cJSON_AddStringToObject(o, "model", d.model);
            /* sensor family: SCT- = user picks the SCT-013 model per channel; CT- = CT shipped with
             * the module, calibrated in pair, model fixed per channel (not settable) */
            if (d.sensor_class != 0xFF) cJSON_AddNumberToObject(o, "sensor_class", d.sensor_class);
            cJSON_AddBoolToObject(o, "ct_settable", d.sensor_class == 1);   /* 0 = uncalibrated module, 2/3 = CT- (fixed) */
            if (d.sensor_class == 0) cJSON_AddBoolToObject(o, "uncalibrated", true);
            cJSON_AddNumberToObject(o, "variant", d.variant);
            cJSON_AddNumberToObject(o, "n_v", d.n_v);
            cJSON_AddNumberToObject(o, "n_i", d.n_i);
            if (d.line) {
                cJSON_AddNumberToObject(o, "map", d.map);
                cJSON_AddStringToObject(o, "period", d.period_src == 2 ? "counters" : d.period_src == 1 ? "latch" : "none");
            }
            cJSON_AddStringToObject(o, "cal", d.line == 0 ? "n/a" : d.cal == 1 ? "ok" : d.cal == 0 ? "not_asserted" : "unknown");   /* BASIC has no cal-status register */
            cJSON_AddNumberToObject(o, "fw", d.fw);
            {   /* human-readable version: BASIC VERSION byte → release (rbamp_basic_fw_name), STD image word as hex */
                char fb[12];
                const char *fn = d.line == 0 ? rbamp_basic_fw_name((uint8_t)d.fw) : NULL;
                if (!fn) { snprintf(fb, sizeof(fb), d.line == 0 ? "0x%02lX" : "0x%08lX", (unsigned long)d.fw); fn = fb; }
                cJSON_AddStringToObject(o, "fw_name", fn);
            }
            if (d.ota_bank_size) {   /* STD module firmware update (POST /api/v1/modules/<m>/firmware) */
                cJSON *ota = cJSON_AddObjectToObject(o, "ota");
                if (ota) {
                    cJSON_AddNumberToObject(ota, "bank_size", d.ota_bank_size);
                    char hb[12]; snprintf(hb, sizeof(hb), "0x%08lX", (unsigned long)d.active_bank);
                    cJSON_AddStringToObject(ota, "active_bank", hb);
                    if (d.board_id) { char bb[12]; snprintf(bb, sizeof(bb), "0x%08lX", (unsigned long)d.board_id); cJSON_AddStringToObject(ota, "board", bb); }
                    if (d.inactive_bank) {   /* the bank an update image must be built for (vendor .manifest "bank=") */
                        char ib[12]; snprintf(ib, sizeof(ib), "0x%08lX", (unsigned long)d.inactive_bank);
                        cJSON_AddStringToObject(ota, "inactive_bank", ib);
                        cJSON *bk = cJSON_AddArrayToObject(ota, "banks");
                        if (bk) {
                            uint32_t a = d.active_bank < d.inactive_bank ? d.active_bank : d.inactive_bank;
                            uint32_t b = d.active_bank < d.inactive_bank ? d.inactive_bank : d.active_bank;
                            char x[12]; snprintf(x, sizeof(x), "0x%08lX", (unsigned long)a); cJSON_AddItemToArray(bk, cJSON_CreateString(x));
                            snprintf(x, sizeof(x), "0x%08lX", (unsigned long)b); cJSON_AddItemToArray(bk, cJSON_CreateString(x));
                        }
                    }
                }
            }
            cJSON_AddItemToArray(mods, o);
        }
        if (mods) cJSON_AddItemToObject(root, "modules", mods);
    }
    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

rbgrid_json_err_t rbgrid_json_parse_create_unit(const char* json, rbgrid_create_unit_req_t* out) {
    if (!json || !out) return RBGRID_JSON_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->role = RBPOWER_ROLE_DEVICE_LOAD;          /* defaults if omitted */
    out->stat_class = RBPOWER_STAT_PERIOD_1HOUR;

    cJSON* root = cJSON_Parse(json);
    if (!root) return RBGRID_JSON_ERR_INVALID_JSON;
    rbgrid_json_err_t rc = RBGRID_JSON_OK;
    do {
        cJSON* sensor = cJSON_GetObjectItem(root, "sensor");
        cJSON* name   = cJSON_GetObjectItem(root, "name");
        if (!cJSON_IsObject(sensor) || !cJSON_IsString(name) || !name->valuestring || !name->valuestring[0]) {
            rc = RBGRID_JSON_ERR_MISSING_FIELD; break;
        }
        cJSON* iface = cJSON_GetObjectItem(sensor, "iface");
        if (!cJSON_IsString(iface) || !iface->valuestring) { rc = RBGRID_JSON_ERR_MISSING_FIELD; break; }
        strncpy(out->iface, iface->valuestring, sizeof(out->iface) - 1);
        strncpy(out->name,  name->valuestring,  sizeof(out->name) - 1);

        if (strcmp(out->iface, "i2c") == 0) {
            cJSON* mod = cJSON_GetObjectItem(sensor, "module");
            cJSON* ch  = cJSON_GetObjectItem(sensor, "ch");
            cJSON* bus = cJSON_GetObjectItem(sensor, "bus");
            if (!cJSON_IsNumber(mod) || !cJSON_IsNumber(ch)) { rc = RBGRID_JSON_ERR_MISSING_FIELD; break; }
            out->bus    = cJSON_IsNumber(bus) ? (uint8_t)bus->valueint : 0;
            out->module = (uint8_t)mod->valueint;
            out->ch     = (uint8_t)ch->valueint;
        } else if (strcmp(out->iface, "adc") == 0) {
            cJSON* chan = cJSON_GetObjectItem(sensor, "channel");
            if (!cJSON_IsNumber(chan)) { rc = RBGRID_JSON_ERR_MISSING_FIELD; break; }
            out->channel = (uint8_t)chan->valueint;
        } else {
            rc = RBGRID_JSON_ERR_INVALID_ARG; break;   /* unknown iface */
        }

        cJSON* loc = cJSON_GetObjectItem(root, "location");
        if (cJSON_IsString(loc) && loc->valuestring)
            strncpy(out->location, loc->valuestring, sizeof(out->location) - 1);

        cJSON* role = cJSON_GetObjectItem(root, "role");
        if (cJSON_IsString(role) && role->valuestring) {
            for (int i = 0; i < (int)(sizeof(rbpower_load_role_text)/sizeof(rbpower_load_role_text[0])); i++)
                if (strcmp(role->valuestring, rbpower_load_role_text[i]) == 0) { out->role = (rbpower_load_role_t)i; break; }
        }
        cJSON* sc = cJSON_GetObjectItem(root, "stat_class");
        if (cJSON_IsString(sc) && sc->valuestring) {
            for (int i = 0; i < (int)(sizeof(rbpower_stat_period_type_text)/sizeof(rbpower_stat_period_type_text[0])); i++)
                if (strcmp(sc->valuestring, rbpower_stat_period_type_text[i]) == 0) { out->stat_class = (rbpower_stat_period_type_t)i; break; }
        }

        /* Optional explicit point slug + tombstone resurrection flag. */
        cJSON* pt = cJSON_GetObjectItem(root, "point");
        if (cJSON_IsString(pt) && pt->valuestring)
            strncpy(out->point, pt->valuestring, sizeof(out->point) - 1);
        out->reuse = cJSON_IsTrue(cJSON_GetObjectItem(root, "reuse"));
    } while (0);
    cJSON_Delete(root);
    return rc;
}

/* ================================================================================================
 * HA MQTT Discovery (entity model rev 2) — on MQTT connect, publish one retained config per
 * entity so Home Assistant auto-creates devices+entities. Identity = mac (topics/ids/unique_id),
 * display = logical name. State from rbgrid/<mac>/telemetry (compact units-dict), availability from
 * the LWT rbgrid/<mac>/status, expire_after 30 s. Energy in/out are SEPARATE total_increasing (feed
 * the HA Energy Dashboard). Called from the rbmqtt connect callback.
 * ================================================================================================ */
/** @brief Unit key used inside an HA value_template: the hex unit id under frame v2, else `key`. */
static const char* ha_tpl_key(const char* key, rbpower_unit_id_t id, char* buf, size_t cap)
{   /* frame v2: the state topic carries units under hex ids → value_template must follow; uniq_id keeps `key` */
    if (g_tele_frame == 2 && id != 0) { snprintf(buf, cap, "%04X", (unsigned)id); return buf; }
    return key;
}
/** @brief Build and publish one retained HA MQTT Discovery sensor config (QoS 0); logs a warning when the
 * topic would be truncated or the publish fails (either means a missing entity in HA). */
static void ha_pub_entity(const char* mac, const char* key, const char* metric, const char* name,
                          const char* field, bool sys_field, const char* uom, const char* dev_cla,
                          const char* stat_cla, const char* dev_id, const char* dev_name,
                          const char* via, const char* state_t, const char* avty_t, const char* fw,
                          const char* tpl_key)
{
    cJSON* c = cJSON_CreateObject();
    if (!c) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "rbgrid_%s_%s_%s", mac, key, metric);
    cJSON_AddStringToObject(c, "uniq_id", buf);
    cJSON_AddStringToObject(c, "name", name);
    cJSON_AddStringToObject(c, "stat_t", state_t);
    char vt[112];
    if (sys_field) snprintf(vt, sizeof(vt), "{{ value_json.%s }}", field);              // top-level
    else           snprintf(vt, sizeof(vt), "{{ value_json.units['%s'].%s }}", tpl_key ? tpl_key : key, field);
    cJSON_AddStringToObject(c, "val_tpl", vt);
    if (uom && uom[0])         cJSON_AddStringToObject(c, "unit_of_meas", uom);
    if (dev_cla && dev_cla[0]) cJSON_AddStringToObject(c, "dev_cla", dev_cla);
    if (stat_cla && stat_cla[0]) cJSON_AddStringToObject(c, "stat_cla", stat_cla);
    if (sys_field && tpl_key && !strcmp(tpl_key, "LAST_RESET"))
        cJSON_AddStringToObject(c, "last_reset_value_template", "{{ value_json.system.day_start }}");
    cJSON_AddStringToObject(c, "avty_t", avty_t);
    cJSON_AddStringToObject(c, "pl_avail", "online");
    cJSON_AddStringToObject(c, "pl_not_avail", "offline");
    cJSON_AddNumberToObject(c, "expire_after", 30);
    cJSON* dev = cJSON_CreateObject();
    if (dev) {
        cJSON* ids = cJSON_CreateArray();
        if (ids) { cJSON_AddItemToArray(ids, cJSON_CreateString(dev_id)); cJSON_AddItemToObject(dev, "ids", ids); }
        cJSON_AddStringToObject(dev, "name", dev_name);
        cJSON_AddStringToObject(dev, "mf", "RBGrid");
        cJSON_AddStringToObject(dev, "mdl", "RBGrid Power Monitor");
        if (via && via[0]) cJSON_AddStringToObject(dev, "via_device", via);
        if (fw && fw[0])   cJSON_AddStringToObject(dev, "sw", fw);
        cJSON_AddItemToObject(c, "dev", dev);
    }
    char* json = cJSON_PrintUnformatted(c);
    cJSON_Delete(c);
    if (json) {
        char topic[120];
        int tlen = snprintf(topic, sizeof(topic), "homeassistant/sensor/rbgrid_%s_%s/%s/config", mac, key, metric);
        if (tlen < 0 || tlen >= (int)sizeof(topic)) {
            // Silent topic truncation = HA Discovery config lands on the wrong topic → the entity
            // never appears, looking exactly like a broken feature. Make it loud
            // instead of shipping a malformed topic quietly.
            ESP_LOGW(TAG, "HA Discovery: topic TRUNCATED (%d>=%u) mac=%s key=%s metric=%s — entity will be MISSING",
                     tlen, (unsigned)sizeof(topic), mac, key, metric);
        }
        esp_err_t pub = rbmqtt_publish(topic, json, 0, true);   // retained, QoS 0 (no PUBACK backlog)
        if (pub != ESP_OK) {
            // A swallowed publish failure = a silently-missing HA entity.
            // Surface it so a discovery gap is diagnosable, not invisible.
            ESP_LOGW(TAG, "HA Discovery: publish FAILED (%s) topic=%s — entity will be MISSING in HA",
                     esp_err_to_name(pub), topic);
        }
        free(json);
    }
}

/* Per-unit metric set (current-measuring units): current/power/reactive/pf + energy in/out (+day).
 * energy_in/out are total_increasing (lifetime, monotonic) → HA Energy Dashboard. */
static const struct { const char* metric; const char* field; const char* name;
                      const char* uom; const char* dc; const char* sc; } MET[] = {
    { "current",        "i",         "Current",          "A",   "current",        "measurement" },
    { "power",          "p_act",     "Power",            "W",   "power",          "measurement" },
    { "reactive_power", "p_rea",     "Reactive Power",   "var", "reactive_power", "measurement" },
    { "power_factor",   "pf",        "Power Factor",     "",    "power_factor",   "measurement" },
    { "energy_in",      "e_in",      "Energy In",        "Wh",  "energy",         "total_increasing" },
    { "energy_out",     "e_out",     "Energy Out",       "Wh",  "energy",         "total_increasing" },
    /* day counters reset at local midnight: HA needs total_increasing (a drop = new cycle) — `total` without
     * last_reset corrupts the long-term statistics at midnight */
    { "energy_day_in",  "e_day_in",  "Energy In (Day)",  "Wh",  "energy",         "total_increasing" },
    { "energy_day_out", "e_day_out", "Energy Out (Day)", "Wh",  "energy",         "total_increasing" },
};

/** @brief Weak no-op default of rbgrid_ha_discovery_request().
 *
 * Requests a full (idempotent, retained) HA discovery republish from the dedicated discovery task —
 * the strong definition lives in main.cpp (owner of the task + semaphore); this weak no-op keeps the
 * component linkable in builds without the task. Called after unit create/rebind/rename so new
 * units get their HA entities WITHOUT waiting for an MQTT reconnect. */
void __attribute__((weak)) rbgrid_ha_discovery_request(void) {}

/* Inverse of the per-unit discovery: on unit DELETE publish an EMPTY retained payload to every
 * per-unit config topic so HA drops the entities (same rule the ESP-NOW node remover uses). Without
 * this a deleted unit would stay in HA as a ghost going `unavailable` after expire_after. Key rule
 * mirrors the discovery snapshot: main / alt / %04X. 8 publishes QoS0 — fine from the HTTP task
 * (the dedicated-task rule below is about the MQTT event task). */
void rbgrid_json_ha_unpublish_unit(uint16_t unit_id, int role)
{
    if (!rbmqtt_is_connected()) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char m[13];
    snprintf(m, sizeof(m), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char key[8];
    if      (role == RBPOWER_ROLE_MAIN_SUPPLY)        snprintf(key, sizeof(key), "main");
    else if (role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY) snprintf(key, sizeof(key), "alt");
    else                                              snprintf(key, sizeof(key), "%04X", (unsigned)unit_id);
    const int NM = (int)(sizeof(MET) / sizeof(MET[0]));
    for (int k = 0; k < NM; k++) {
        char topic[120];
        int tlen = snprintf(topic, sizeof(topic), "homeassistant/sensor/rbgrid_%s_%s/%s/config", m, key, MET[k].metric);
        if (tlen < 0 || tlen >= (int)sizeof(topic)) continue;
        rbmqtt_publish(topic, "", 0, true);   /* empty retained → HA removes the entity */
    }
    ESP_LOGW(TAG, "HA discovery: unpublished %d entities of unit %s", NM, key);
}

/* MUST be called from a DEDICATED task (not the MQTT event handler): ~44 retained publishes +
 * cJSON builds done synchronously in the event task trip the task-WDT → boot loop.
 * Snapshots the unit list under the lock, then publishes OUTSIDE the lock, paced with a small yield
 * per unit so the event loop / outbox keep breathing. QoS 0 (no PUBACK backlog). */
void rbgrid_json_publish_ha_discovery(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char m[13];
    snprintf(m, sizeof(m), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char state_t[48], avty_t[48];
    snprintf(state_t, sizeof(state_t), "rbgrid/%s/telemetry", m);
    snprintf(avty_t, sizeof(avty_t), "rbgrid/%s/status", m);
    // HA device sw_version — read the running app descriptor so it always matches the version
    // reported by /api/system/version (a hardcoded string drifts).
    const esp_app_desc_t* app_desc = esp_app_get_description();
    const char* fw = (app_desc && app_desc->version[0]) ? app_desc->version : "0.0.0";

    const int NM = (int)(sizeof(MET) / sizeof(MET[0]));

    /* 1. Snapshot the units under the lock (cheap), so we publish without holding it. Heap-allocated
     * (a ~3.3 KB stack array on the 6 KB ha_disc task would leave a thin margin). */
    struct ha_snap_e { char key[8]; char name[40]; bool is_main; uint16_t uid; };
    struct ha_snap_e* snap = malloc(sizeof(struct ha_snap_e) * RBPOWER_MAX_HOME_UNITS);
    if (!snap) { ESP_LOGE(TAG, "HA Discovery: snapshot alloc failed"); return; }
    int n = 0;
    rbpower_home_lock();
    bool has_vbus = g_home_network.specialized.has_voltage_bus || rbpower_home_get_grid(NULL) > 0;   /* grid-fed v_bus */
    for (int i = 0; i < g_home_network.units_count && n < RBPOWER_MAX_HOME_UNITS; i++) {
        rbpower_home_unit_t* u = &g_home_network.units[i];
        if (!u->has_valid_definition || !u->definition.is_active) continue;
        if (u->definition.type == RBPOWER_METRIC_VOLTAGE_BUS) continue;   // v_bus handled separately
        rbpower_load_role_t role = u->definition.role;
        if      (role == RBPOWER_ROLE_MAIN_SUPPLY)        { snprintf(snap[n].key, 8, "main"); snprintf(snap[n].name, 40, "Main"); snap[n].is_main = true; snap[n].uid = u->definition.unit_id; }
        else if (role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY) { snprintf(snap[n].key, 8, "alt");  snprintf(snap[n].name, 40, "Alt");  snap[n].is_main = false; snap[n].uid = u->definition.unit_id; }
        else { snprintf(snap[n].key, 8, "%04X", u->definition.unit_id);
               snap[n].uid = u->definition.unit_id;
               snprintf(snap[n].name, 40, "%s", u->definition.name[0] ? u->definition.name : snap[n].key);
               snap[n].is_main = false; }
        n++;
    }
    rbpower_home_unlock();

    int published = 0;

    /* 2. System + v_bus FIRST (so the system-overview entities land even if anything truncates). */
    ha_pub_entity(m, "system", "net_power", "Net Power", "net_pwr", true, "W", "power", "measurement",
                  m, "RBGrid System", NULL, state_t, avty_t, fw, NULL);
    ha_pub_entity(m, "system", "frequency", "Frequency", "freq", true, "Hz", "frequency", "measurement",
                  m, "RBGrid System", NULL, state_t, avty_t, fw, NULL);
    published += 2;
    /* System economics entities (billing in HA). sys_field=true → val_tpl reads
     * value_json.<field>; passing "system.cost_total" gives the nested path. These fields are already in
     * the telemetry payload's system{} block. uom = configured currency → HA monetary device_class. */
    {
        const char* cur = rbpower_tariff_currency();
        ha_pub_entity(m, "system", "cost_total", "Cost (Total)", "system.cost_total", true, cur, "monetary", "total_increasing",
                      m, "RBGrid System", NULL, state_t, avty_t, fw, NULL);
        /* monetary allows only state_class `total` in HA — the daily reset needs last_reset, taken
         * from system.day_start (local midnight, ISO) in the same payload */
        ha_pub_entity(m, "system", "cost_day", "Cost (Day)", "system.cost_day", true, cur, "monetary", "total",
                      m, "RBGrid System", NULL, state_t, avty_t, fw, "LAST_RESET");
        ha_pub_entity(m, "system", "revenue_total", "Revenue (Total)", "system.rev_total", true, cur, "monetary", "total_increasing",
                      m, "RBGrid System", NULL, state_t, avty_t, fw, NULL);
        ha_pub_entity(m, "system", "revenue_day", "Revenue (Day)", "system.rev_day", true, cur, "monetary", "total",
                      m, "RBGrid System", NULL, state_t, avty_t, fw, "LAST_RESET");
        published += 4;
    }
    if (has_vbus) {
        char dev_id[24];
        snprintf(dev_id, sizeof(dev_id), "%s_vbus", m);
        char vb_tpl[8];
        ha_pub_entity(m, "v_bus", "voltage", "Bus Voltage", "v", false, "V", "voltage", "measurement",
                      dev_id, "RBGrid Voltage Bus", m, state_t, avty_t, fw,
                      ha_tpl_key("v_bus", g_home_network.system.voltage_bus_unit_id, vb_tpl, sizeof(vb_tpl)));
        published++;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 3. Per-unit entities, paced (yield per unit lets the MQTT task drain the socket). */
    for (int i = 0; i < n; i++) {
        char dev_id[24], dev_name[48];
        snprintf(dev_id, sizeof(dev_id), "%s_%s", m, snap[i].key);
        snprintf(dev_name, sizeof(dev_name), "RBGrid %s", snap[i].name);
        /* NOTE: no per-unit "voltage" entity — the rbAmp current channels (incl. the main-supply
         * current unit) don't emit units.<u>.v in telemetry (system voltage is the separate v_bus
         * entity), so a Voltage entity here would sit permanently `unavailable`. */
        for (int k = 0; k < NM; k++) {
            char u_tpl[8];
            ha_pub_entity(m, snap[i].key, MET[k].metric, MET[k].name, MET[k].field, false,
                          MET[k].uom, MET[k].dc, MET[k].sc, dev_id, dev_name, m, state_t, avty_t, fw,
                          ha_tpl_key(snap[i].key, snap[i].uid, u_tpl, sizeof(u_tpl)));
            published++;
        }
        vTaskDelay(pdMS_TO_TICKS(15));   // pace: never hog the CPU / flood the outbox
    }

    free(snap);
    ESP_LOGI(TAG, "HA Discovery: published %d retained configs (mac %s)", published, m);
}

char* rbgrid_json_create_units_config(const rbgrid_json_context_t* context) {
    // Use internal context if not provided
    if (!context) {
        context = g_json_context;
    }
    
    if (!context) {
        ESP_LOGE(TAG, "No JSON context available (neither parameter nor internal)");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating units_config JSON");

    cJSON* root = rbgrid_json_create_base_message("units_config", context);
    if (!root) {
        ESP_LOGE(TAG, "Failed to create base message for units_config");
        return NULL;
    }

    cJSON* data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    // === UNITS ARRAY ===
    cJSON* units_array = cJSON_CreateArray();
    if (units_array) {

        // Walk all units of the global network
        for (int i = 0; i < g_home_network.units_count; i++) {
            const rbpower_home_unit_t* home_unit = &g_home_network.units[i];

            // Skip units without a valid definition
            if (!home_unit->has_valid_definition) {
                ESP_LOGW(TAG, "Unit %d has no valid definition, skipping", i);
                continue;
            }
            
            const rbpower_unit_definition_t* unit_def = &home_unit->definition;
            
            cJSON* unit_obj = cJSON_CreateObject();
            if (unit_obj) {
                // === MAIN INFO ===
                cJSON_AddNumberToObject(unit_obj, "unit_id", unit_def->unit_id);
                cJSON_AddStringToObject(unit_obj, "name", unit_def->name);
                cJSON_AddStringToObject(unit_obj, "short_name", unit_def->short_name); /* "" = unset */
                cJSON_AddStringToObject(unit_obj, "location", unit_def->location);
                
                // Metric type
                const char* type_str = "COMPOSITE";
                if (unit_def->type >= 0 && unit_def->type < sizeof(rbpower_metric_type_text)/sizeof(rbpower_metric_type_text[0])) {
                    type_str = rbpower_metric_type_text[unit_def->type];
                }
                cJSON_AddStringToObject(unit_obj, "type", type_str);
                
                // Network role
                const char* role_str = "SMART_APPLIANCE";
                if (unit_def->role >= 0 && unit_def->role < sizeof(rbpower_load_role_text)/sizeof(rbpower_load_role_text[0])) {
                    role_str = rbpower_load_role_text[unit_def->role];
                }
                cJSON_AddStringToObject(unit_obj, "role", role_str);
                
                // Statistics class
                const char* stat_class_str = "YEARLY";
                if (unit_def->stat_class >= 0 && unit_def->stat_class < sizeof(rbpower_stat_period_type_text)/sizeof(rbpower_stat_period_type_text[0])) {
                    stat_class_str = rbpower_stat_period_type_text[unit_def->stat_class];
                }
                cJSON_AddStringToObject(unit_obj, "stat_class", stat_class_str);
                
                // === DATA SOURCES ===
                cJSON* sources = cJSON_CreateArray();
                if (sources) {
                    for (int j = 0; j < unit_def->sources_count && j < RBPOWER_MAX_SOURCES_PER_UNIT; j++) {
                        const rbpower_data_source_t* source = &unit_def->sources[j];
                        
                        cJSON* source_obj = cJSON_CreateObject();
                        if (source_obj) {
                            // Data source type
                            const char* source_type_str = "EXTERNAL_API";
                            if (source->source_type >= 0 && source->source_type < sizeof(rbpower_source_type_text)/sizeof(rbpower_source_type_text[0])) {
                                source_type_str = rbpower_source_type_text[source->source_type];
                            }
                            cJSON_AddStringToObject(source_obj, "source_type", source_type_str);
                            
                            cJSON_AddNumberToObject(source_obj, "source_id", source->source_id);
                            cJSON_AddNumberToObject(source_obj, "sensor_profile_id", source->sensor_profile_id);
                            cJSON_AddNumberToObject(source_obj, "weight_factor", source->weight_factor);
                            cJSON_AddBoolToObject(source_obj, "is_primary", source->is_primary);
                            cJSON_AddBoolToObject(source_obj, "invert_sign", source->invert_sign);
                            
                            cJSON_AddItemToArray(sources, source_obj);
                        }
                    }
                    cJSON_AddItemToObject(unit_obj, "sources", sources);
                }
                cJSON_AddNumberToObject(unit_obj, "sources_count", unit_def->sources_count);
                
                // === HIERARCHY ===
                cJSON_AddNumberToObject(unit_obj, "parent_unit_id", unit_def->parent_unit_id);
                
                cJSON* child_units = cJSON_CreateArray();
                if (child_units) {
                    for (int j = 0; j < unit_def->children_count && j < RBPOWER_MAX_CHILDREN; j++) {
                        cJSON_AddItemToArray(child_units, cJSON_CreateNumber(unit_def->child_units[j]));
                    }
                    cJSON_AddItemToObject(unit_obj, "child_units", child_units);
                }
                cJSON_AddNumberToObject(unit_obj, "children_count", unit_def->children_count);
                
                // === PARAMETERS AND LIMITS ===
                cJSON_AddBoolToObject(unit_obj, "bidirectional_metering", unit_def->bidirectional_metering);
                cJSON_AddBoolToObject(unit_obj, "accumulate_energy", unit_def->accumulate_energy);
                cJSON_AddNumberToObject(unit_obj, "rated_power_w", unit_def->rated_power_w);
                cJSON_AddNumberToObject(unit_obj, "max_current_a", unit_def->max_current_a);
                cJSON_AddNumberToObject(unit_obj, "rated_voltage_v", unit_def->rated_voltage_v);
                /* EV station overrides (0 = system.ev) */
                cJSON_AddNumberToObject(unit_obj, "charge_threshold_w", unit_def->ev_charge_threshold_w);
                cJSON_AddNumberToObject(unit_obj, "idle_timeout_min", unit_def->ev_idle_timeout_min);
                
                // === THRESHOLDS AND ALERTS ===
                cJSON* thresholds = cJSON_CreateObject();
                if (thresholds) {
                    cJSON_AddNumberToObject(thresholds, "over_power_w", unit_def->thresholds.over_power_w);
                    cJSON_AddNumberToObject(thresholds, "under_voltage_v", unit_def->thresholds.under_voltage_v);
                    cJSON_AddNumberToObject(thresholds, "over_current_a", unit_def->thresholds.over_current_a);
                    cJSON_AddNumberToObject(thresholds, "over_voltage_v", unit_def->thresholds.over_voltage_v);
                    cJSON_AddBoolToObject(thresholds, "enable_alerts", unit_def->thresholds.enable_alerts);
                    cJSON_AddItemToObject(unit_obj, "thresholds", thresholds);
                }
                
                // === BILLING ===
                cJSON* billing = cJSON_CreateObject();
                if (billing) {
                    cJSON_AddRoundedNumber(billing, "cost_per_kwh", unit_def->billing.cost_per_kwh, 6);
                    cJSON_AddBoolToObject(billing, "time_of_use_enabled", unit_def->billing.time_of_use_enabled);
                    cJSON_AddNumberToObject(billing, "current_tariff_zone", unit_def->billing.current_tariff_zone);
                    cJSON_AddItemToObject(unit_obj, "billing", billing);
                }
                
                // === STATE ===
                cJSON_AddBoolToObject(unit_obj, "is_active", unit_def->is_active);
                cJSON_AddNumberToObject(unit_obj, "created_timestamp_us", (double)unit_def->created_timestamp_us);

                // === Opaque identity / physics / point ===
                cJSON_AddStringToObject(unit_obj, "point", unit_def->point);
                cJSON_AddNumberToObject(unit_obj, "caps", unit_def->caps);
                cJSON_AddNumberToObject(unit_obj, "device_uid", (double)unit_def->device_uid); /* ≤48 bits, exact in double */
                cJSON_AddNumberToObject(unit_obj, "channel_idx", unit_def->channel_idx);
                cJSON_AddNumberToObject(unit_obj, "phase", unit_def->phase);           /* reserved */
                cJSON_AddNumberToObject(unit_obj, "group_id", unit_def->group_id);     /* reserved */
                cJSON_AddNumberToObject(unit_obj, "role_trunk", unit_def->role_trunk); /* reserved */

                // === ADDITIONAL STATE INFO ===
                cJSON_AddBoolToObject(unit_obj, "has_realtime_data", home_unit->has_realtime_data);
                cJSON_AddBoolToObject(unit_obj, "has_statistics_enabled", home_unit->has_statistics_enabled);
                cJSON_AddNumberToObject(unit_obj, "last_update_us", (double)home_unit->last_update_us);
                cJSON_AddNumberToObject(unit_obj, "total_updates", home_unit->total_updates);
                
                cJSON_AddItemToArray(units_array, unit_obj);
            }
        }
        
        cJSON_AddItemToObject(data, "units", units_array);
    }
    
    // === SUMMARY ===
    cJSON_AddNumberToObject(data, "units_count", g_home_network.units_count);
    cJSON_AddNumberToObject(data, "active_units_count", g_home_network.aggregated.active_units_count);
    cJSON_AddBoolToObject(data, "system_healthy", g_home_network.system_healthy);

    // === Point tombstones (retired slugs — survive reboot via units.json) ===
    {
        char ts[RBPOINT_TOMBSTONE_MAX][33];
        int tn = rbgrid_point_tombstones_get(ts, RBPOINT_TOMBSTONE_MAX);
        cJSON* tarr = cJSON_CreateArray();
        if (tarr) {
            for (int t = 0; t < tn; t++) cJSON_AddItemToArray(tarr, cJSON_CreateString(ts[t]));
            cJSON_AddItemToObject(data, "point_tombstones", tarr);
        }
    }
    
    cJSON_AddItemToObject(root, "data", data);

    ESP_LOGI(TAG, "units_config JSON created successfully");
    
    // Convert to string and cleanup cJSON
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);
    return json_string;
}

/** @brief Add p_res_w — power resolution of the unit's source (W): the smallest step the source can tell
 * from its own floor. Only where MEASURED: STD rbAmp modules (STD-UI1: floor sd 0.014 W → 3σ ≈ 0.05 W).
 * Other sources: absent (the client keeps its own estimate). */
static void rbjson_add_p_res(cJSON *obj, rbpower_unit_id_t unit)
{
    uint8_t module = 0, source = 0;
    rbpower_module_desc_t d;
    if (!obj || !unit || !rbpower_get_unit_rbamp_feed(unit, &module, &source)) return;
    if (!rbpower_get_module_desc(module, &d) || d.line != 1) return;
    cJSON_AddNumberToObject(obj, "p_res_w", 0.05);
}

/** @brief Attach the per-phase live array of a phase-grouped
 * (multi-channel U3I3) unit — "ph":[{"ph":N,"v","i","p","q","pf"},...]. Explicit phase number per
 * entry; NAN field = unknown → omitted. Stale snapshots (>10 s — several telemetry periods) are
 * dropped rather than shown frozen. Units without a snapshot get no key (additive). */
static void rbjson_add_phase_live(cJSON* obj, uint16_t unit_id) {
    rbpower_phase_live_t phl[3];
    int64_t age_us = 0;
    uint8_t phn = rbpower_home_get_phase_live((rbpower_unit_id_t)unit_id, phl, &age_us);
    if (phn == 0 || age_us >= 10 * 1000000LL) return;
    cJSON* ph_arr = cJSON_CreateArray();
    if (!ph_arr) return;
    for (uint8_t k = 0; k < phn; k++) {
        cJSON* e = cJSON_CreateObject();
        if (!e) continue;
        cJSON_AddNumberToObject(e, "ph", phl[k].ph);
        if (!isnan(phl[k].v))  cJSON_AddNumberToObject(e, "v",  round(phl[k].v * 100) / 100);
        if (!isnan(phl[k].i))  cJSON_AddNumberToObject(e, "i",  round(phl[k].i * 10000) / 10000);
        if (!isnan(phl[k].p))  cJSON_AddNumberToObject(e, "p",  round(phl[k].p * 100) / 100);
        if (!isnan(phl[k].q))  cJSON_AddNumberToObject(e, "q",  round(phl[k].q * 100) / 100);
        if (!isnan(phl[k].pf)) cJSON_AddNumberToObject(e, "pf", round(phl[k].pf * 100) / 100);
        if (!isnan(phl[k].angle)) cJSON_AddNumberToObject(e, "angle", round(phl[k].angle * 10) / 10);   /* ° vs L1 (STD 0x7A+) */
        cJSON_AddItemToArray(ph_arr, e);
    }
    cJSON_AddItemToObject(obj, "ph", ph_arr);
    {   /* phase sequence of the three-phase input (module-measured, STD 0x7A+); absent when unknown */
        static const char *const SEQ[] = { NULL, "forward", "reverse", "abnormal" };
        if (phl[0].seq >= 1 && phl[0].seq <= 3) cJSON_AddStringToObject(obj, "phase_seq", SEQ[phl[0].seq]);
    }
}

/** @brief Tag a telemetry unit object with its data source.
 *
 * Lets the frontend tell rbAmp (I2C) units from ESP-NOW ones (icons/grouping/which fields are valid).
 * For I2C units it also emits the DECODED bus/module/channel — the firmware owns the UID bit layout,
 * so the frontend never decodes it (avoids silently breaking if the encoding changes). */
static void rbgrid_telemetry_add_source(cJSON* obj, uint16_t unit_id) {
    // Hardware presence: lets the frontend distinguish a unit whose bound sensor is
    // actually present (computing) from one configured-but-absent (dormant). Always emitted.
    cJSON_AddBoolToObject(obj, "present", rbpower_get_unit_hardware_present((rbpower_unit_id_t)unit_id));
    {   /* age_s: seconds since the last live sample (null = never fed). The UI greys values out when
         * it grows (module silent / bus break) instead of showing a frozen number as live. */
        int32_t a = rbpower_home_unit_age_ms((rbpower_unit_id_t)unit_id);
        if (a >= 0) cJSON_AddNumberToObject(obj, "age_s", round(a / 100.0) / 10.0);
        else        cJSON_AddNullToObject(obj, "age_s");
    }
    /* Source classification comes from the DEFINITION (source_type + device_uid/channel_idx),
     * never from unit_id bits — ids are opaque handles now. Legacy units have these fields
     * backfilled at load; the id-bit checks remain only as a fallback for defs we can't resolve. */
    const rbpower_unit_definition_t* t7def = rbpower_home_get_unit_definition((rbpower_unit_id_t)unit_id);
    if (t7def && t7def->point[0]) cJSON_AddStringToObject(obj, "point", t7def->point);
    bool is_i2c = t7def ? (t7def->sources_count > 0 &&
                           t7def->sources[0].source_type == RBPOWER_SOURCE_I2C_SENSOR)
                        : RBGRID_IS_I2C_UNIT(unit_id);
    bool is_espnow = t7def ? (t7def->sources_count > 0 &&
                              t7def->sources[0].source_type == RBPOWER_SOURCE_ESP_NOW)
                           : RBGRID_IS_ESP_NOW_UNIT(unit_id);
    /* Bridge-fed unit with a legacy (ADC-era) definition — e.g. default main 0006 bound to a U3I3
     * PHASES entry: label it by its REAL source (same rule as v_bus). */
    uint8_t feed_module = 0xFF, feed_source = 0;
    bool feed = !is_i2c && !is_espnow &&
                rbpower_get_unit_rbamp_feed((rbpower_unit_id_t)unit_id, &feed_module, &feed_source);
    /* src = TRANSPORT (stable). src_class = field-semantics class, EQUAL to the fields_by_source
     * key in /api/v1/schema (clients gate fields on it): the I2C bus may carry both "rbamp" and
     * "rbamp_std" units, so transport alone can't drive isFieldMeasured(). Additive; today
     * i2c→rbamp, espnow→espnow, and the legacy "adc"→"native_adc" pair for units whose definition
     * is still ADC-typed (no native ESP32-ADC measurement path exists). */
    if (is_i2c || feed) {
        cJSON_AddStringToObject(obj, "src", "i2c");
        cJSON_AddStringToObject(obj, "src_class", "rbamp");
        cJSON_AddNumberToObject(obj, "bus",    0);  /* fleet is single-bus */
        /* ch: current channel 0..2; 255 = module voltage; 254 = all current channels grouped (PHASES) */
        cJSON_AddNumberToObject(obj, "module", feed  ? (int)feed_module :
                                               t7def ? (int)t7def->device_uid
                                                     : (int)RBGRID_I2C_GET_MODULE(unit_id));
        {
            int chv = feed  ? (int)feed_source : t7def ? (int)t7def->channel_idx : (int)RBGRID_I2C_GET_CHANNEL(unit_id);
            cJSON_AddNumberToObject(obj, "ch", chv);
            int modv = feed ? (int)feed_module : t7def ? (int)t7def->device_uid : (int)RBGRID_I2C_GET_MODULE(unit_id);
            rbjson_add_ch_human(obj, chv, modv);
        }
        // energy_mode (provenance): how this unit's Wh is computed — set by the rbAmp bridge at
        // probe. Omitted when unknown (e.g. module not yet provisioned).
        uint8_t em = rbpower_get_unit_energy_mode(unit_id);
        static const char* em_str[] = { "none", "on_device", "host" };
        if (em <= RBPOWER_ENERGY_MODE_HOST) {
            cJSON_AddStringToObject(obj, "energy_mode", em_str[em]);
        }
    } else if (is_espnow) {
        cJSON_AddStringToObject(obj, "src", "espnow");
        cJSON_AddStringToObject(obj, "src_class", "espnow");
        uint8_t em = rbpower_get_unit_energy_mode(unit_id);
        static const char* emn_str[] = { "none", "on_device", "host" };
        if (em <= RBPOWER_ENERGY_MODE_HOST) {
            cJSON_AddStringToObject(obj, "energy_mode", emn_str[em]);
        }
    } else {
        cJSON_AddStringToObject(obj, "src", "adc");
        cJSON_AddStringToObject(obj, "src_class", "native_adc");
    }
}

char* rbgrid_json_create_telemetry_frame(uint8_t frame);
char* rbgrid_json_create_telemetry(void) { return rbgrid_json_create_telemetry_frame(g_tele_frame); }

/** @brief Frame-v2 decoration of a unit object: identity + capability flags + quality bits (q_live). */
static void tele_v2_decorate(cJSON* obj, rbpower_unit_id_t id)
{
    const rbpower_unit_definition_t* d = rbpower_home_get_unit_definition(id);
    if (!d) return;
    if (!cJSON_GetObjectItem(obj, "unit_id")) {   /* unit_id inside EVERY unit object */
        char hx[8]; snprintf(hx, sizeof(hx), "%04X", (unsigned)id);
        cJSON_AddStringToObject(obj, "unit_id", hx);
    }
    const char* type_str = "COMPOSITE";
    if (d->type >= 0 && d->type < (int)(sizeof(rbpower_metric_type_text)/sizeof(rbpower_metric_type_text[0])))
        type_str = rbpower_metric_type_text[d->type];
    cJSON_AddStringToObject(obj, "type", type_str);
    const char* role_str = rbpower_load_role_name((unsigned)d->role);
    cJSON_AddStringToObject(obj, "role", role_str ? role_str : "UNKNOWN");
    if (!cJSON_GetObjectItem(obj, "point") && d->point[0]) cJSON_AddStringToObject(obj, "point", d->point);
    cJSON* caps = cJSON_AddArrayToObject(obj, "caps");
    if (caps) {
        uint8_t em = rbpower_get_unit_energy_mode(id);
        if (d->caps & 1)   cJSON_AddItemToArray(caps, cJSON_CreateString("VOLTAGE"));
        if (d->caps & 2)   cJSON_AddItemToArray(caps, cJSON_CreateString("CURRENT"));
        if (d->caps & 4)   cJSON_AddItemToArray(caps, cJSON_CreateString("POWER"));
        if (d->caps & 8)   cJSON_AddItemToArray(caps, cJSON_CreateString("ENERGY"));
        /* caps say what is REALLY measured: rbAmp/ESP-NOW units carry no pf/f
         * (values are 0) → no PF/FREQ there; DIRECTION = signed power is meaningful
         * (on-device energy or the bidi bit), not for host-estimated channels (always +). */
        bool injected = (d->sources_count > 0 && (d->sources[0].source_type == RBPOWER_SOURCE_I2C_SENSOR ||
                                                  d->sources[0].source_type == RBPOWER_SOURCE_ESP_NOW));
        if ((d->caps & 16) || (injected && em == RBPOWER_ENERGY_MODE_ON_DEVICE))
                           cJSON_AddItemToArray(caps, cJSON_CreateString("DIRECTION"));
        if ((d->caps & 32) && !injected) cJSON_AddItemToArray(caps, cJSON_CreateString("PF"));
        if ((d->caps & 64) && !injected) cJSON_AddItemToArray(caps, cJSON_CreateString("FREQ"));
        if (d->caps & 128) cJSON_AddItemToArray(caps, cJSON_CreateString("REMOTE"));
        if (d->caps & 256) cJSON_AddItemToArray(caps, cJSON_CreateString("VIRTUAL"));
        if (em == RBPOWER_ENERGY_MODE_ON_DEVICE) cJSON_AddItemToArray(caps, cJSON_CreateString("DEVICE_ENERGY"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("ENERGY_MWH"));
        if (d->stat_class > 0) cJSON_AddItemToArray(caps, cJSON_CreateString("PERSISTED"));
        if (d->channel_idx == 254) cJSON_AddItemToArray(caps, cJSON_CreateString("PHASES"));
    }
    uint8_t em2 = rbpower_get_unit_energy_mode(id);
    cJSON_AddNumberToObject(obj, "res", (em2 == RBPOWER_ENERGY_MODE_ON_DEVICE) ? 1 : 0);
    uint16_t ql = 0;
    { extern uint8_t rbtime_quality(uint32_t*); uint32_t age = 0; if (rbtime_quality(&age) == 0) ql |= (1u << 1); }   /* TIME_UNSYNC */
    if (!rbpower_get_unit_hardware_present(id)) ql |= (1u << 3);                                                /* SRC_GAP */
    if (em2 == RBPOWER_ENERGY_MODE_HOST) ql |= (1u << 5);                                                       /* ESTIMATED */
    { int32_t a = rbpower_home_unit_age_ms(id); if (a < 0 || a > 5000) ql |= (1u << 2); }                       /* STALE: no live sample for > 5 s */
    cJSON_AddNumberToObject(obj, "q_live", ql);
}

/** @brief Attach a specialized block (v1 key "main"/"alt"/"v_bus") — in v2 under its hex id, decorated. */
static void tele_add_special(cJSON* units_obj, uint8_t frame, const char* v1_key, rbpower_unit_id_t id, cJSON* obj)
{
    if (frame == 2 && id != 0) {
        char hex[8]; snprintf(hex, sizeof(hex), "%04X", (unsigned)id);
        if (!cJSON_GetObjectItem(obj, "unit_id")) cJSON_AddStringToObject(obj, "unit_id", hex);
        tele_v2_decorate(obj, id);
        cJSON_AddItemToObject(units_obj, hex, obj);
    } else {
        cJSON_AddItemToObject(units_obj, v1_key, obj);
    }
}

char* rbgrid_json_create_telemetry_frame(uint8_t frame) {
    if (!g_json_context) {
        ESP_LOGE(TAG, "JSON context not initialized");
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root");
        return NULL;
    }

    // Telemetry schema version (additive Tier-2 field; bump on any payload-shape change so clients can
    // detect drift). See the API reference. One builder → HTTP/WS/MQTT all carry it.
    cJSON_AddNumberToObject(root, "v", (frame == 2) ? 2 : RBGRID_TELEMETRY_SCHEMA_VERSION);

    // Telemetry profile: LEAN omits the heavy lifetime/economics fields from the live push.
    const bool lean = g_tele_lean;

    // === TIMESTAMPS ===
    // Time comes from rbtime.c
    extern uint64_t rbtime_get_unix_timestamp(void);
    extern uint64_t rbtime_get_uptime_us(void);
    
    uint64_t unix_timestamp = rbtime_get_unix_timestamp();
    uint64_t uptime_us = rbtime_get_uptime_us();
    uint64_t uptime_ms = uptime_us / 1000;
    
    cJSON_AddNumberToObject(root, "ts", (double)uptime_ms);
    cJSON_AddNumberToObject(root, "sntp", (double)unix_timestamp);
    /* Time quality: 0 unsynced · 1 synced · 2 holdover (+ sync age).
     * Additive within frame v1. time_src: sntp only today (rtc planned). */
    {
        extern uint8_t rbtime_quality(uint32_t *sync_age_s_out);
        uint32_t sync_age_s = 0;
        uint8_t tq = rbtime_quality(&sync_age_s);
        cJSON_AddNumberToObject(root, "tq", tq);
        cJSON_AddNumberToObject(root, "sync_age_s", sync_age_s);
        cJSON_AddStringToObject(root, "time_src", tq ? "sntp" : "none");
    }
    /* Single producer: monotonic frame seq + per-boot id let a consumer detect loss AND reboots
     * without parsing uptime. Additive keys. */
    {
        extern uint32_t esp_random(void);
        static uint32_t s_tele_seq;
        static uint32_t s_boot_id;
        if (s_boot_id == 0) {
            s_boot_id = esp_random();
            if (s_boot_id == 0) s_boot_id = 1;   /* 0 is the "unset" sentinel */
        }
        cJSON_AddNumberToObject(root, "seq", (double)(++s_tele_seq));
        cJSON_AddNumberToObject(root, "boot_id", (double)s_boot_id);
        cJSON_AddNumberToObject(root, "hw_rev", (double)rbpower_hw_rev());
    }
    
    // === SYSTEM PARAMETERS ===
    // Frequency: prefer the system value fed by the source (rbAmp set_system_frequency) — the
    // voltage_bus stats read 0 for injected supplies. Fall back to the legacy voltage_bus
    // statistics only when no source reports a frequency.
    float system_frequency = 50.0f; // default
    bool freq_measured = false;
    if (g_home_network.system.system_frequency_hz > 0.0f) {
        system_frequency = g_home_network.system.system_frequency_hz;
        freq_measured = rbpower_home_system_frequency_fresh();   /* init default is the nominal 50 Hz */
    } else if (g_home_network.specialized.has_voltage_bus) {
        rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
        if (vbus->statistics_3sec.period_complete && vbus->statistics_3sec.final.frequency_avg_final > 0) {
            system_frequency = vbus->statistics_3sec.final.frequency_avg_final;
            freq_measured = true;
        } else if (vbus->realtime_200ms.frequency > 0) {
            system_frequency = vbus->realtime_200ms.frequency;
            freq_measured = true;
        }
    }
    cJSON_AddNumberToObject(root, "freq", round(system_frequency * 100) / 100);
    /* Without a voltage source `freq` is the NOMINAL 50 Hz, not a measurement —
     * say so (additive; "measured" when a module / the voltage bus reports it). */
    cJSON_AddStringToObject(root, "freq_src", freq_measured ? "measured" : "nominal");
    
    /* Live system net power + direction — computed from the SUPPLY units' actual inject-fed power
     * (the SAME source as their per-unit emission below), NOT g_home_network.system.net_power_w which
     * is only ever set to 0 at init and never updated for injected supplies (net_pwr would read 0
     * while units.main.p_act is non-zero). Supply-role units (main + alt) = the system
     * boundary; load sub-meters are breakdowns, excluded (no double-count). */
    float net_power = 0.0f, net_reactive = 0.0f;
    uint8_t net_direction = g_home_network.system.net_direction;
    if (g_home_network.specialized.has_main_supply) {
        rbpower_main_supply_t* msp = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
        if (msp) {
            net_power += msp->statistics_3sec.electrical.rms_samples_count > 0 ?
                msp->statistics_3sec.electrical.power_active_sum / msp->statistics_3sec.electrical.rms_samples_count :
                msp->realtime_200ms.power_active_w;
            net_reactive += msp->statistics_3sec.electrical.rms_samples_count > 0 ?
                msp->statistics_3sec.electrical.power_reactive_sum / msp->statistics_3sec.electrical.rms_samples_count :
                msp->realtime_200ms.power_reactive_var;
            net_direction = msp->realtime_200ms.direction;   // main supply drives system direction
        }
    }
    if (g_home_network.specialized.has_alternative_supply && g_home_network.specialized.alternative_supply) {
        rbpower_alternative_supply_t* asp = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
        net_power += asp->statistics_3sec.electrical_samples_count > 0 ?
            asp->statistics_3sec.power_active_sum / asp->statistics_3sec.electrical_samples_count :
            asp->realtime_200ms.power_active_w;
        /* alt reactive too — without it system p_rea is inconsistent with p_act when an alt
         * supply carries vars. */
        net_reactive += asp->statistics_3sec.electrical_samples_count > 0 ?
            asp->statistics_3sec.power_reactive_sum / asp->statistics_3sec.electrical_samples_count :
            asp->realtime_200ms.power_reactive_var;
    }

    /* Role-based fallback: on an install without the legacy specialized setup
     * there are NO specialized main/alt structs — a supply is an ordinary (rbAmp) unit
     * carrying the MAIN_SUPPLY/ALTERNATIVE_SUPPLY role. Live system power then aggregates by
     * ROLE across units. Only when neither specialized supply exists (no double-count). */
    if (!g_home_network.specialized.has_main_supply && !g_home_network.specialized.has_alternative_supply) {
        for (int i = 0; i < g_home_network.units_count; i++) {
            rbpower_home_unit_t* u2 = &g_home_network.units[i];
            if (!u2->has_valid_definition) continue;
            if (u2->definition.role != RBPOWER_ROLE_MAIN_SUPPLY &&
                u2->definition.role != RBPOWER_ROLE_ALTERNATIVE_SUPPLY) continue;
            net_power    += u2->realtime_average_200ms.average_200ms.power_active_avg_w;
            net_reactive += u2->realtime_average_200ms.average_200ms.power_reactive_avg_var;
            if (u2->definition.role == RBPOWER_ROLE_MAIN_SUPPLY)
                net_direction = u2->realtime_average_200ms.zc_analysis.current_direction;
        }
    }

    cJSON_AddNumberToObject(root, "net_dir", net_direction);
    cJSON_AddNumberToObject(root, "net_pwr", round(net_power * 100) / 100);

    /* === SYSTEM TOTALS (dashboard "system summary" — firmware-RESOLVED so the client doesn't
     * guess topology, per the shared entity model). Aggregate at the GRID BOUNDARY = supply-role
     * units (main + alternative); LOAD sub-meters are breakdowns of the supply, so summing them would
     * double-count → excluded. p_act = the firmware net system power; e_in/e_out are SEPARATE lifetime
     * totals (never signed-net — same convention HA's Energy Dashboard needs). Field names mirror a
     * unit's so the client parses the block like a virtual "system unit". HA ignores this block (it
     * aggregates per-unit); it's for the native UI summary card. */
    {
        cJSON* sys = cJSON_CreateObject();
        if (sys) {
            uint8_t units_valid = 0, units_present = 0;
            for (int i = 0; i < g_home_network.units_count; i++) {
                if (!g_home_network.units[i].has_valid_definition) continue;
                units_valid++;
                if (g_home_network.units[i].hardware_present) units_present++;
            }
            cJSON_AddNumberToObject(sys, "p_act",     round(net_power * 100) / 100);
            cJSON_AddNumberToObject(sys, "p_rea",     round(net_reactive * 100) / 100);
            // LEAN: skip the lifetime/economics aggregation (the heavy part — lifetime-accumulator reads
            // across supplies + 8 monetary fields) every push. Clients read these from /api/v1/summary.
            if (!lean) {
                double e_in = 0, e_out = 0, e_day_in = 0, e_day_out = 0;
                double cost_total = 0, cost_day = 0, rev_total = 0, rev_day = 0;
                if (g_home_network.specialized.has_main_supply) {
                    rbpower_main_supply_t* ms = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
                    if (ms) {
                        e_in     += ms->lifetime_accumulators.total_energy_consumed_wh;
                        e_out    += ms->lifetime_accumulators.total_energy_supplied_wh;
                        e_day_in += ms->lifetime_accumulators.daily_energy_consumed_wh;
                        e_day_out+= ms->lifetime_accumulators.daily_energy_supplied_wh;
                        cost_total += ms->lifetime_accumulators.total_cost_consumed;
                        cost_day   += ms->lifetime_accumulators.daily_cost_consumed;
                        rev_total  += ms->lifetime_accumulators.total_cost_supplied;
                        rev_day    += ms->lifetime_accumulators.daily_cost_supplied;
                    }
                }
                if (g_home_network.specialized.has_alternative_supply && g_home_network.specialized.alternative_supply) {
                    rbpower_alternative_supply_t* as = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
                    e_in     += as->lifetime_accumulators.total_energy_consumed_wh;
                    e_out    += as->lifetime_accumulators.total_energy_supplied_wh;
                    e_day_in += as->lifetime_accumulators.daily_energy_consumed_wh;
                    e_day_out+= as->lifetime_accumulators.daily_energy_supplied_wh;
                    cost_total += as->lifetime_accumulators.total_cost_consumed;
                    cost_day   += as->lifetime_accumulators.daily_cost_consumed;
                    rev_total  += as->lifetime_accumulators.total_cost_supplied;
                    rev_day    += as->lifetime_accumulators.daily_cost_supplied;
                }
                cJSON_AddNumberToObject(sys, "e_in",      round(e_in * 100) / 100);
                cJSON_AddNumberToObject(sys, "e_out",     round(e_out * 100) / 100);
                cJSON_AddNumberToObject(sys, "e_day_in",  round(e_day_in * 100) / 100);
                cJSON_AddNumberToObject(sys, "e_day_out", round(e_day_out * 100) / 100);
                cJSON_AddRoundedDouble(sys, "cost_total", cost_total, 4);
                cJSON_AddRoundedDouble(sys, "cost_day",   cost_day, 4);
                cJSON_AddRoundedDouble(sys, "rev_total",  rev_total, 4);
                cJSON_AddRoundedDouble(sys, "rev_day",    rev_day, 4);
                {   /* base_w: night baseline ("home vampires") — median of 01–05 h hourly averages, 7 days */
                    extern bool rbgrid_base_w(rbpower_unit_id_t unit, float *base_w, int *hours, int *nights);
                    float bw = NAN; int bh = 0, bn = 0;
                    if (g_home_network.system.main_supply_unit_id &&
                        rbgrid_base_w(g_home_network.system.main_supply_unit_id, &bw, &bh, &bn) && isfinite(bw)) {
                        cJSON_AddNumberToObject(sys, "base_w", round(bw * 10) / 10);
                    } else {
                        cJSON_AddNullToObject(sys, "base_w");
                    }
                    cJSON_AddNumberToObject(sys, "base_w_hours", bh);
                    cJSON_AddNumberToObject(sys, "base_w_nights", bn);
                }
                {   /* day_start: local midnight of the current day counters (ISO 8601 with offset) — HA
                     * last_reset for the monetary day entities; also lets a client label "today" */
                    time_t nowt = time(NULL);
                    if (nowt > 1600000000) {
                        struct tm lt; localtime_r(&nowt, &lt);
                        lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
                        time_t mid = mktime(&lt);
                        struct tm lm; localtime_r(&mid, &lm);
                        char iso[32]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S%z", &lm);
                        size_t n = strlen(iso);   /* +0900 -> +09:00 (HA/ISO parsers) */
                        if (n >= 5 && n + 1 < sizeof(iso)) { iso[n + 1] = 0; iso[n] = iso[n - 1]; iso[n - 1] = iso[n - 2]; iso[n - 2] = ':'; }
                        cJSON_AddStringToObject(sys, "day_start", iso);
                    }
                }
                {   /* last boot: the UI labels record gaps honestly —
                     * "the device restarted at <ts> (<reason>)" instead of "the open hour is not included" */
                    uint32_t bts = 0, dts = 0; const char* why = NULL; bool pl = false;
                    rbpower_boot_info(&bts, &why, &pl, &dts);
                    /* Before boot_check ran (first frames after a boot the HTTP server answers early) the
                     * reason is not known yet — emit null instead of {ts 0, reason "unknown"}. */
                    cJSON* boot = (why && strcmp(why, "unknown") != 0) ? cJSON_CreateObject() : NULL;
                    if (!boot) cJSON_AddNullToObject(sys, "boot");
                    if (boot) {
                        cJSON_AddNumberToObject(boot, "ts", bts);                      /* unix, 0 = clock not valid */
                        cJSON_AddStringToObject(boot, "reason", why ? why : "unknown");
                        cJSON_AddBoolToObject(boot, "planned", pl);
                        cJSON_AddNumberToObject(boot, "downtime_s", dts);              /* 0 = unknown (no heartbeat ts before the reset) */
                        cJSON_AddItemToObject(sys, "boot", boot);
                    }
                }
                /* Live tariff frame — active zone, buy/sell rates and the instant currency/hour at the
                 * system net power ("cost per hour right now"). Rates 0 = no active plan. cost_rate_h sign follows net_power (negative = earning on export). */
                {
                    int zid = -1; float buy = 0, sell = 0;
                    rbpower_tariff_get_diag(NULL, NULL, NULL, NULL, &zid, &buy, &sell, NULL);
                    cJSON* tar = cJSON_CreateObject();
                    if (tar) {
                        if (zid >= 0 && zid < RBPOWER_TARIFF_ZONE_COUNT)
                            cJSON_AddStringToObject(tar, "zone", rbpower_tariff_zone_type_text[zid]);
                        cJSON_AddRoundedDouble(tar, "buy_rate", buy, 6);
                        cJSON_AddRoundedDouble(tar, "sell_rate", sell, 6);
                        /* Same basis as cost_day / record c (a rate at the bare zone price next to an all-in
                         * "Today" total does not add up): consumption is booked
                         * as (zone rate + per-kWh fees) × (1 + VAT); feed-in revenue is the plain sell rate. */
                        double buy_all = buy;
                        {
                            const rbpower_tariff_plan_t* tp = rbpower_tariff_get_current_plan();
                            if (tp && buy > 0) {
                                double fees = (double)tp->grid_usage_fee_per_kwh + (double)tp->energy_tax_per_kwh + (double)tp->renewable_levy_per_kwh;
                                buy_all = ((double)buy + fees) * (1.0 + (double)tp->vat_percent / 100.0);
                            }
                        }
                        cJSON_AddRoundedDouble(tar, "buy_rate_all_in", buy_all, 6);
                        double rate_h = (net_power / 1000.0) * (net_power >= 0 ? buy_all : sell);
                        cJSON_AddRoundedDouble(tar, "cost_rate_h", rate_h, 6);
                        cJSON_AddStringToObject(tar, "currency", rbpower_tariff_currency());
                        {   /* money is computed off the built-in demo plan until the user saves one */
                            const rbpower_tariff_plan_t* tpp = rbpower_tariff_get_current_plan();
                            if (tpp && rbpower_tariff_plan_is_placeholder(tpp->plan_name, tpp->provider_name))
                                cJSON_AddBoolToObject(tar, "placeholder", true);
                        }
                        cJSON_AddItemToObject(sys, "tariff", tar);
                    }
                }
            }
            cJSON_AddNumberToObject(sys, "units",     units_valid);
            cJSON_AddNumberToObject(sys, "present",   units_present);
            cJSON_AddItemToObject(root, "system", sys);
        }
    }

    // === UNITS SECTION ===
    cJSON* units_obj = cJSON_CreateObject();
    if (!units_obj) {
        cJSON_Delete(root);
        return NULL;
    }
    
    // === GRID VOLTAGE (v1 key "v_bus") ===
    // Voltage is measured per PHASE by the modules (rbpower_home_get_grid), not by a legacy
    // pseudo-unit. v = mean L-N of the fresh phases; ph[] = per-phase voltage with its
    // source module / voltage channel (emitted when more than one phase or a phase other than L1 is
    // measured). The legacy specialized voltage_bus struct is used only when no module measures voltage.
    {
        rbpower_grid_phase_t gp[3];
        uint8_t ng = rbpower_home_get_grid(gp);
        cJSON* vbus_obj = NULL;
        if (ng > 0) {
            vbus_obj = cJSON_CreateObject();
            if (vbus_obj) {
                cJSON_AddNumberToObject(vbus_obj, "v", round(rbpower_home_grid_voltage(0) * 100) / 100);
                cJSON_AddBoolToObject(vbus_obj, "present", true);
                cJSON_AddStringToObject(vbus_obj, "src", "i2c");
                cJSON_AddStringToObject(vbus_obj, "src_class", "rbamp");
                cJSON_AddNumberToObject(vbus_obj, "bus",    0);
                cJSON_AddNumberToObject(vbus_obj, "module", gp[0].src_module);
                cJSON_AddNumberToObject(vbus_obj, "ch",     255);   /* module voltage input(s), see ph[].vch */
                cJSON_AddNumberToObject(vbus_obj, "n_ph",   ng);
                if (ng > 1 || gp[0].ph != 1) {
                    cJSON* pha = cJSON_CreateArray();
                    for (uint8_t k = 0; k < ng && pha; k++) {
                        cJSON* e = cJSON_CreateObject();
                        if (!e) break;
                        cJSON_AddNumberToObject(e, "ph",     gp[k].ph);
                        cJSON_AddNumberToObject(e, "v",      round(gp[k].v * 100) / 100);
                        cJSON_AddNumberToObject(e, "module", gp[k].src_module);
                        cJSON_AddNumberToObject(e, "vch",    gp[k].src_vch);
                        cJSON_AddItemToArray(pha, e);
                    }
                    if (pha) cJSON_AddItemToObject(vbus_obj, "ph", pha);
                }
            }
        } else if (g_home_network.specialized.has_voltage_bus) {
            rbpower_voltage_bus_t* vbus = &g_home_network.specialized.voltage_bus;
            vbus_obj = cJSON_CreateObject();
            if (vbus_obj) {
                float voltage_rms = vbus->statistics_3sec.period_complete ?
                                   vbus->statistics_3sec.final.voltage_rms_final :
                                   vbus->realtime_200ms.voltage_rms;
                cJSON_AddNumberToObject(vbus_obj, "v", round(voltage_rms * 100) / 100);
                rbgrid_telemetry_add_source(vbus_obj, RBGRID_UNIT_VOLTAGE_BUS);
            }
        }
        if (vbus_obj) tele_add_special(units_obj, frame, "v_bus", g_home_network.system.voltage_bus_unit_id, vbus_obj);
    }
    
    // === MAIN SUPPLY ===
    if (g_home_network.specialized.has_main_supply) {
        rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
        if (main_supply) {
            cJSON* main_obj = cJSON_CreateObject();
            if (main_obj) {
                // Electrical values from the 3-s statistics or realtime.
                // Guard on rms_samples_count (the divisor) not sum_squares — consistent with the
                // power computations below; avoids a latent div-by-zero → NaN.
                float current_rms = main_supply->statistics_3sec.electrical.rms_samples_count > 0 ?
                                   sqrt(main_supply->statistics_3sec.electrical.current_rms_sum_squares /
                                        main_supply->statistics_3sec.electrical.rms_samples_count) :
                                   main_supply->realtime_200ms.current_rms;
                
                float power_active = main_supply->statistics_3sec.electrical.rms_samples_count > 0 ?
                                    main_supply->statistics_3sec.electrical.power_active_sum / 
                                    main_supply->statistics_3sec.electrical.rms_samples_count :
                                    main_supply->realtime_200ms.power_active_w;
                
                float power_reactive = main_supply->statistics_3sec.electrical.rms_samples_count > 0 ?
                                      main_supply->statistics_3sec.electrical.power_reactive_sum / 
                                      main_supply->statistics_3sec.electrical.rms_samples_count :
                                      main_supply->realtime_200ms.power_reactive_var;
                
                // Power factor — symmetry with regular/rbAmp units. From the same
                // 3-sec accumulator the current/power above use (avg = factor_sum / sample_count).
                float power_factor = main_supply->statistics_3sec.electrical.rms_samples_count > 0 ?
                                     (float)(main_supply->statistics_3sec.electrical.power_factor_sum /
                                             main_supply->statistics_3sec.electrical.rms_samples_count) :
                                     0.0f;
                /* The 3-sec accumulator is read IN PROGRESS and restarts every 3 s — right after a
                 * restart sample_count is 0 and pf would read 0 while i/p fall back to the 200 ms
                 * value, so main.pf would flicker 0/1. Fall back to the unit's 200 ms pf too. */
                if (main_supply->statistics_3sec.electrical.rms_samples_count == 0) {
                    for (uint8_t k = 0; k < g_home_network.units_count; k++) {
                        if (g_home_network.units[k].has_valid_definition &&
                            g_home_network.units[k].definition.unit_id == g_home_network.system.main_supply_unit_id) {
                            power_factor = g_home_network.units[k].realtime_average_200ms.average_200ms.power_factor_avg;
                            break;
                        }
                    }
                }

                {   /* A phase-grouped main (U3I3 PHASES) publishes ph[] from the LATEST poll; the 3-s
                     * in-progress average above lags it, so Σ ph[].p ≠ p_act by up to ~4 % on a moving
                     * load. Take p/i/q from the same
                     * 200 ms snapshot the phases came from. */
                    rbpower_phase_live_t phx[3]; int64_t pa = 0;
                    if (rbpower_home_get_phase_live((rbpower_unit_id_t)g_home_network.system.main_supply_unit_id, phx, &pa) > 0 &&
                        pa < 10 * 1000000LL) {
                        float sp = 0.0f, si = 0.0f, sq = 0.0f;
                        uint8_t phn = rbpower_home_get_phase_live((rbpower_unit_id_t)g_home_network.system.main_supply_unit_id, phx, &pa);
                        for (uint8_t k = 0; k < phn; k++) { sp += phx[k].p; si += phx[k].i; if (!isnan(phx[k].q)) sq += phx[k].q; }
                        power_active = sp; current_rms = si; power_reactive = sq;
                    }
                }
                /* A phase-grouped supply injects its mean L-N voltage → emit `v` on
                 * main. Omitted when unknown (current-only legacy main:
                 * system voltage stays in v_bus). Same >1 V rule as the regular-unit block. */
                {
                    /* The specialized struct carries no voltage; the grouped inject lands in the UNIT's
                     * 200 ms average (same place the hex-keyed block reads), so look it up by id. */
                    float mv = 0.0f;
                    for (uint8_t k = 0; k < g_home_network.units_count; k++) {
                        if (g_home_network.units[k].has_valid_definition &&
                            g_home_network.units[k].definition.unit_id == g_home_network.system.main_supply_unit_id) {
                            mv = g_home_network.units[k].realtime_average_200ms.average_200ms.voltage_rms_avg;
                            break;
                        }
                    }
                    if (mv > 1.0f) cJSON_AddNumberToObject(main_obj, "v", round(mv * 100) / 100);
                }
                /* 0.1 mA resolution: a calibrated STD-UI1 reads 1 / 2.4 / 4.9 mA within a few %, and with
                 * 1-mA steps 2.4 mA would show as 0.002 A (a client "no flow" threshold needs the digit) */
                cJSON_AddNumberToObject(main_obj, "i", round(current_rms * 10000) / 10000);
                rbjson_add_p_res(main_obj, g_home_network.system.main_supply_unit_id);
                cJSON_AddNumberToObject(main_obj, "p_act", round(power_active * 100) / 100);
                cJSON_AddNumberToObject(main_obj, "p_rea", round(power_reactive * 100) / 100);
                cJSON_AddNumberToObject(main_obj, "pf", round(power_factor * 100) / 100);
                cJSON_AddNumberToObject(main_obj, "dir", main_supply->realtime_200ms.direction);
                rbgrid_telemetry_add_source(main_obj, RBGRID_UNIT_MAIN_SUPPLY);
                rbjson_add_phase_live(main_obj, g_home_network.system.main_supply_unit_id);  /* per-phase input (dashboard main card) */

                // Energy + economics from the lifetime accumulators (LEAN: omitted → /api/v1/economics)
                if (!lean) {
                    cJSON_AddNumberToObject(main_obj, "e_in", lifetime_wh(g_home_network.system.main_supply_unit_id, true, main_supply->lifetime_accumulators.total_energy_consumed_wh));    // lifetime consumption
                    cJSON_AddNumberToObject(main_obj, "e_out", lifetime_wh(g_home_network.system.main_supply_unit_id, false, main_supply->lifetime_accumulators.total_energy_supplied_wh));
                    rbjson_add_ct_load(main_obj, g_home_network.system.main_supply_unit_id);   // CT load vs nominal
                    cJSON_AddNumberToObject(main_obj, "e_day_in", main_supply->lifetime_accumulators.daily_energy_consumed_wh);  // daily consumption
                    cJSON_AddNumberToObject(main_obj, "e_day_out", main_supply->lifetime_accumulators.daily_energy_supplied_wh); // daily export
                    cJSON_AddRoundedDouble(main_obj, "cost_day", main_supply->lifetime_accumulators.daily_cost_consumed, 4);    // daily cost
                    cJSON_AddRoundedDouble(main_obj, "rev_day", main_supply->lifetime_accumulators.daily_cost_supplied, 4);     // daily revenue
                    cJSON_AddRoundedDouble(main_obj, "cost_total", main_supply->lifetime_accumulators.total_cost_consumed, 4);  // lifetime cost
                    cJSON_AddRoundedDouble(main_obj, "rev_total", main_supply->lifetime_accumulators.total_cost_supplied, 4);   // lifetime revenue
                }

                tele_add_special(units_obj, frame, "main", g_home_network.system.main_supply_unit_id, main_obj);
            }
        }
    }
    
    // === ALTERNATIVE SUPPLY ===
    if (g_home_network.specialized.has_alternative_supply) {
        rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
        if (alt_supply) {
            cJSON* alt_obj = cJSON_CreateObject();
            if (alt_obj) {
                // Electrical values
                float current_rms = alt_supply->statistics_3sec.electrical_samples_count > 0 ?
                                   sqrt(alt_supply->statistics_3sec.current_rms_sum_squares / 
                                        alt_supply->statistics_3sec.electrical_samples_count) :
                                   alt_supply->realtime_200ms.current_rms;
                
                float power_active = alt_supply->statistics_3sec.electrical_samples_count > 0 ?
                                    alt_supply->statistics_3sec.power_active_sum / 
                                    alt_supply->statistics_3sec.electrical_samples_count :
                                    alt_supply->realtime_200ms.power_active_w;
                
                float power_reactive = alt_supply->statistics_3sec.electrical_samples_count > 0 ?
                                      alt_supply->statistics_3sec.power_reactive_sum / 
                                      alt_supply->statistics_3sec.electrical_samples_count :
                                      alt_supply->realtime_200ms.power_reactive_var;
                
                cJSON_AddNumberToObject(alt_obj, "i", round(current_rms * 10000) / 10000);
                cJSON_AddNumberToObject(alt_obj, "p_act", round(power_active * 100) / 100);
                cJSON_AddNumberToObject(alt_obj, "p_rea", round(power_reactive * 100) / 100);
                cJSON_AddNumberToObject(alt_obj, "dir", alt_supply->realtime_200ms.direction);
                // The specialized alt block uses the legacy "adc" source tag (its definition is
                // ADC-typed); its flat 3-sec accumulator carries no power_factor_sum, so pf is not emitted.
                cJSON_AddStringToObject(alt_obj, "src", "adc");

                // Energy from the lifetime accumulators (LEAN: omitted)
                if (!lean) {
                    cJSON_AddNumberToObject(alt_obj, "e_in", lifetime_wh(g_home_network.system.alternative_supply_unit_id, true, alt_supply->lifetime_accumulators.total_energy_consumed_wh));    // lifetime consumption
                    cJSON_AddNumberToObject(alt_obj, "e_out", lifetime_wh(g_home_network.system.alternative_supply_unit_id, false, alt_supply->lifetime_accumulators.total_energy_supplied_wh));
                    rbjson_add_ct_load(alt_obj, g_home_network.system.alternative_supply_unit_id);   // CT load vs nominal
                    cJSON_AddNumberToObject(alt_obj, "e_day_in", alt_supply->lifetime_accumulators.daily_energy_consumed_wh);  // daily consumption
                    cJSON_AddNumberToObject(alt_obj, "e_day_out", alt_supply->lifetime_accumulators.daily_energy_supplied_wh); // daily generation
                }

                rbjson_add_phase_live(alt_obj, g_home_network.system.alternative_supply_unit_id);
                tele_add_special(units_obj, frame, "alt", g_home_network.system.alternative_supply_unit_id, alt_obj);
            }
        }
    }
    
    // === REGULAR UNITS ===
    for (int i = 0; i < g_home_network.units_count; i++) {
        rbpower_home_unit_t* unit = &g_home_network.units[i];

        // Skip inactive units
        if (!unit->has_valid_definition || !unit->definition.is_active) {
            continue;
        }

        // Skip voltage_bus, main_supply and alt_supply ONLY when they were already emitted by the
        // specialized blocks ("main"/"alt"/"v_bus"). Without the legacy specialized structs
        // (supply = an ordinary rbAmp unit with a role) the unit is emitted under its hex key like
        // all the others.
        rbpower_load_role_t role = unit->definition.role;
        if ((role == RBPOWER_ROLE_MAIN_SUPPLY && g_home_network.specialized.has_main_supply) ||
            (role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY && g_home_network.specialized.has_alternative_supply) ||
            unit->definition.type == RBPOWER_METRIC_VOLTAGE_BUS) {
            continue;
        }
        
        // Object for a regular unit.
        // Unit-id key as 4-digit HEX (e.g. "9020") to match the history
        // API's hex unit_id + the I2C-UID native hex form — one representation across telemetry↔history.
        char unit_key[16];
        snprintf(unit_key, sizeof(unit_key), "%04X", unit->definition.unit_id);
        
        cJSON* unit_obj = cJSON_CreateObject();
        if (unit_obj) {
            // Electrical values from the realtime data
            float current_rms = unit->realtime_average_200ms.average_200ms.current_rms_avg;
            float power_active = unit->realtime_average_200ms.average_200ms.power_active_avg_w;
            float power_reactive = unit->realtime_average_200ms.average_200ms.power_reactive_avg_var;
            uint8_t direction = unit->realtime_average_200ms.zc_analysis.current_direction;
            // Voltage + power factor from the 200 ms RT average: rbAmp per-channel units expose v/pf
            // like the supply units. (Pure current channels read v≈0 — system voltage is v_bus.)
            float voltage_rms = unit->realtime_average_200ms.average_200ms.voltage_rms_avg;
            float power_factor = unit->realtime_average_200ms.average_200ms.power_factor_avg;

            // v: OMIT for current-only rbAmp channels (v≈0 reads as a dead/fault unit on the UI and
            // pollutes the min/max envelope; system voltage comes from v_bus).
            if (voltage_rms > 1.0f) {
                cJSON_AddNumberToObject(unit_obj, "v", round(voltage_rms * 100) / 100);
            }
            cJSON_AddNumberToObject(unit_obj, "i", round(current_rms * 1000) / 1000);
            cJSON_AddNumberToObject(unit_obj, "p_act", round(power_active * 100) / 100);
            cJSON_AddNumberToObject(unit_obj, "p_rea", round(power_reactive * 100) / 100);
            cJSON_AddNumberToObject(unit_obj, "pf", round(power_factor * 100) / 100);
            cJSON_AddNumberToObject(unit_obj, "dir", direction);
            rbgrid_telemetry_add_source(unit_obj, unit->definition.unit_id);
            
            // Energy from the lifetime accumulators (LEAN: omitted)
            if (!lean) {
                cJSON_AddNumberToObject(unit_obj, "e_in", lifetime_wh(unit->definition.unit_id, true, unit->statistics.lifetime_accumulators.total_energy_consumed_wh));    // lifetime consumption
                cJSON_AddNumberToObject(unit_obj, "e_day_in", unit->statistics.lifetime_accumulators.daily_energy_consumed_wh);  // daily consumption
                /* export / generation of a regular point (RENEWABLE_SOURCE, bidirectional loads) */
                cJSON_AddNumberToObject(unit_obj, "e_out", lifetime_wh(unit->definition.unit_id, false, unit->statistics.lifetime_accumulators.total_energy_supplied_wh));
                rbjson_add_ct_load(unit_obj, unit->definition.unit_id);
                cJSON_AddNumberToObject(unit_obj, "e_day_out", unit->statistics.lifetime_accumulators.daily_energy_supplied_wh);
            }

            rbjson_add_phase_live(unit_obj, unit->definition.unit_id);

            // Finalized 3-sec result averages in a `result:{}` sub-object
            // ALONGSIDE the live 200ms values above (gauge uses live, numeric card uses result). Emit
            // only when the 3-sec period is complete; omit between (clients cache last-known) so we
            // don't bloat every 200ms packet.
            const rbpower_period_statistics_t* p3 = &unit->statistics.periods[RBPOWER_STAT_PERIOD_3SEC];
            if (p3->period_complete) {
                cJSON* result_obj = cJSON_CreateObject();
                if (result_obj) {
                    if (p3->voltage_rms > 1.0f)   // current-only rbAmp: omit v (system V = v_bus)
                        cJSON_AddNumberToObject(result_obj, "v", round(p3->voltage_rms * 100) / 100);
                    cJSON_AddNumberToObject(result_obj, "i", round(p3->current_rms * 1000) / 1000);
                    cJSON_AddNumberToObject(result_obj, "p_act", round(p3->power_active_avg * 100) / 100);
                    cJSON_AddNumberToObject(result_obj, "p_rea", round(p3->power_reactive_avg * 100) / 100);
                    cJSON_AddNumberToObject(result_obj, "pf", round(p3->power_factor_avg * 100) / 100);
                    cJSON_AddItemToObject(unit_obj, "result", result_obj);
                }
            }

            if (frame == 2) tele_v2_decorate(unit_obj, unit->definition.unit_id);
            cJSON_AddItemToObject(units_obj, unit_key, unit_obj);
        }
    }
    
    cJSON_AddItemToObject(root, "units", units_obj);
    if (frame == 2) {   /* explicit references (MAIN_SUPPLY vs VOLTAGE_BUS share a role) */
        char hx[8];
        if (g_home_network.system.main_supply_unit_id)        { snprintf(hx, sizeof(hx), "%04X", (unsigned)g_home_network.system.main_supply_unit_id);        cJSON_AddStringToObject(root, "main_unit", hx); }
        if (g_home_network.system.alternative_supply_unit_id) { snprintf(hx, sizeof(hx), "%04X", (unsigned)g_home_network.system.alternative_supply_unit_id); cJSON_AddStringToObject(root, "alt_unit", hx); }
        if (g_home_network.system.voltage_bus_unit_id)        { snprintf(hx, sizeof(hx), "%04X", (unsigned)g_home_network.system.voltage_bus_unit_id);        cJSON_AddStringToObject(root, "vbus_unit", hx); }
    }
    
    // Convert to string
    char* json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (json_string) {
        ESP_LOGD(TAG, "Telemetry JSON created: %s", json_string);
    } else {
        ESP_LOGE(TAG, "Failed to print JSON");
    }
    
    return json_string;
}

/* ================================================================================================
 * JSON → STRUCTURE PARSERS
 * ================================================================================================ */

/* Parse the hardware section. The "adc"/"adc_sensors" keys are the legacy ADC-typed config that
 * still binds the default units fed by the rbAmp bridge (no native ESP32-ADC sampling exists). */
rbgrid_json_err_t rbgrid_json_parse_hardware_section(cJSON* hw_obj, rbpower_hardware_config_t* config)
{
    if (!hw_obj || !config) {
        return RBGRID_JSON_ERR_INVALID_ARG;
    }
    
    cJSON* item = NULL;
    
    // The channel count is stored in the ADC section
    // and is set while parsing it

    // Legacy ADC config
    cJSON* adc = cJSON_GetObjectItem(hw_obj, "adc");
    if (adc) {
        item = cJSON_GetObjectItem(adc, "sampling_frequency");
        if (item && cJSON_IsNumber(item)) {
            config->adc.sampling_frequency = (uint32_t)item->valuedouble;
        }

        item = cJSON_GetObjectItem(adc, "adc_resolution");
        if (item && cJSON_IsNumber(item)) {
            config->adc.adc_resolution = (uint8_t)item->valuedouble;
        }

        item = cJSON_GetObjectItem(adc, "adc_attenuation");
        if (item && cJSON_IsNumber(item)) {
            config->adc.adc_attenuation = (uint8_t)item->valuedouble;
        }

        // ADC channel list
        cJSON* adc_channels = cJSON_GetObjectItem(adc, "channels");
        if (adc_channels && cJSON_IsArray(adc_channels)) {
            int ch_count = cJSON_GetArraySize(adc_channels);
            if (ch_count > RBPOWER_MAX_CHANNELS) {
                ch_count = RBPOWER_MAX_CHANNELS;
            }
            config->adc.channels_count = ch_count;
            
            for (int i = 0; i < ch_count; i++) {
                cJSON* ch = cJSON_GetArrayItem(adc_channels, i);
                if (ch && cJSON_IsNumber(ch)) {
                    config->adc.channels[i] = (uint8_t)ch->valuedouble;
                }
            }
        }
        
        ESP_LOGI(TAG, "ADC: sampling_freq=%lu Hz, channels=%d",
                 config->adc.sampling_frequency,
                 config->adc.channels_count);
    }

    // Network section: restore the nominal mains frequency (Hz) and voltage (V). The nominal
    // frequency is the fallback while no source reports a measured one.
    cJSON* network_obj = cJSON_GetObjectItem(hw_obj, "network");
    if (network_obj && cJSON_IsObject(network_obj)) {
        item = cJSON_GetObjectItem(network_obj, "nominal_frequency");
        if (item && cJSON_IsNumber(item)) {
            config->network.nominal_frequency = (uint16_t)item->valuedouble;
        }
        /* nominal_voltage (L-N): sane mains range only; 0 = explicit unset */
        item = cJSON_GetObjectItem(network_obj, "nominal_voltage");
        if (item && cJSON_IsNumber(item) &&
            (item->valuedouble == 0 || (item->valuedouble >= 50 && item->valuedouble <= 500))) {
            config->network.nominal_voltage = (float)item->valuedouble;
        }
    }

    // adc_sensors section
    cJSON* adc_sensors_obj = cJSON_GetObjectItem(hw_obj, "adc_sensors");
    if (adc_sensors_obj && cJSON_IsObject(adc_sensors_obj)) {
        ESP_LOGI(TAG, "Found adc_sensors section");

        // Channel array inside adc_sensors
        cJSON* channels_array = cJSON_GetObjectItem(adc_sensors_obj, "channels");
        if (channels_array && cJSON_IsArray(channels_array)) {
            int sensor_count = cJSON_GetArraySize(channels_array);
            if (sensor_count > RBPOWER_MAX_CHANNELS) {
                sensor_count = RBPOWER_MAX_CHANNELS;
                ESP_LOGW(TAG, "Sensor count limited to %d", RBPOWER_MAX_CHANNELS);
            }
            
            ESP_LOGI(TAG, "Processing %d sensor channels", sensor_count);
            
            for (int i = 0; i < sensor_count; i++) {
                cJSON* sensor = cJSON_GetArrayItem(channels_array, i);
                if (!sensor) continue;
                
                rbpower_sensor_channel_config_t* channel = &config->adc_sensors.channels[i];
                
                // sensor_type - looked up in the name table
                item = cJSON_GetObjectItem(sensor, "type");
                if (item && cJSON_IsString(item)) {
                    const char* type_str = item->valuestring;
                    channel->type = RBSENSOR_TYPE_UNKNOWN;  // Default value

                    // Special cases first
                    if (strcmp(type_str, "VOLTAGE") == 0) {
                        channel->type = RBSENSOR_TYPE_VOLTAGE_CUSTOM;
                    } else if (strcmp(type_str, "CURRENT") == 0) {
                        channel->type = RBSENSOR_TYPE_CURRENT_CUSTOM;
                    } else {
                        // Match against the rbsensor_type_text table
                        for (int j = 0; j < RBSENSOR_TYPE_UNKNOWN; j++) {
                            if (strcmp(type_str, rbsensor_type_text[j]) == 0) {
                                channel->type = (rbsensor_type_t)j;
                                break;
                            }
                        }
                    }

                    ESP_LOGD(TAG, "Sensor[%d] type string '%s' mapped to enum %d",
                             i, type_str, channel->type);
                }
                
                // enabled
                item = cJSON_GetObjectItem(sensor, "enabled");
                if (item && cJSON_IsBool(item)) {
                    channel->enabled = cJSON_IsTrue(item);
                }
                
                // name
                item = cJSON_GetObjectItem(sensor, "name");
                if (item && cJSON_IsString(item)) {
                    strncpy(channel->name, item->valuestring, sizeof(channel->name) - 1);
                    channel->name[sizeof(channel->name) - 1] = '\0';
                }
                
                // scale_factor
                item = cJSON_GetObjectItem(sensor, "scale_factor");
                if (item && cJSON_IsNumber(item)) {
                    channel->scale_factor = (float)item->valuedouble;
                }
                
                // zero_offset
                item = cJSON_GetObjectItem(sensor, "zero_offset");
                if (item && cJSON_IsNumber(item)) {
                    channel->zero_offset = (float)item->valuedouble;
                }
                
                ESP_LOGI(TAG, "Sensor[%d]: type=%d, enabled=%d, name=%s, scale=%.4f, offset=%.2f",
                         i, channel->type, channel->enabled, channel->name,
                         channel->scale_factor, channel->zero_offset);
            }
        } else {
            ESP_LOGW(TAG, "No channels array found in adc_sensors section");
        }
        
        // Global filtering parameters of adc_sensors
        item = cJSON_GetObjectItem(adc_sensors_obj, "enable_filtering");
        if (item && cJSON_IsBool(item)) {
            config->adc_sensors.enable_filtering = cJSON_IsTrue(item);
            ESP_LOGI(TAG, "Filtering enabled: %d", config->adc_sensors.enable_filtering);
        }
        
        item = cJSON_GetObjectItem(adc_sensors_obj, "filter_samples");
        if (item && cJSON_IsNumber(item)) {
            config->adc_sensors.filter_samples = (uint8_t)item->valuedouble;
            ESP_LOGI(TAG, "Filter samples: %d", config->adc_sensors.filter_samples);
        }
    } else {
        ESP_LOGW(TAG, "No adc_sensors section found in hardware config");
    }
    
    return RBGRID_JSON_OK;
}

/* Parse the network section (WiFi SSID ≤31 chars, password ≤63 chars, AP flag); NULL outputs are skipped. */
rbgrid_json_err_t rbgrid_json_parse_network_section(cJSON* net_obj, 
                                                     char* wifi_ssid,
                                                     char* wifi_password, 
                                                     bool* ap_enabled)
{
    if (!net_obj) {
        return RBGRID_JSON_ERR_INVALID_ARG;
    }
    
    cJSON* item = NULL;
    
    // WiFi SSID
    if (wifi_ssid) {
        item = cJSON_GetObjectItem(net_obj, "wifi_ssid");
        if (item && cJSON_IsString(item)) {
            strncpy(wifi_ssid, item->valuestring, 31);
            wifi_ssid[31] = '\0';
            ESP_LOGI(TAG, "Network: SSID=%s", wifi_ssid);
        }
    }
    
    // WiFi Password
    if (wifi_password) {
        item = cJSON_GetObjectItem(net_obj, "wifi_password");
        if (item && cJSON_IsString(item)) {
            strncpy(wifi_password, item->valuestring, 63);
            wifi_password[63] = '\0';
            ESP_LOGD(TAG, "Network: Password configured");
        }
    }
    
    // AP mode enabled
    if (ap_enabled) {
        item = cJSON_GetObjectItem(net_obj, "ap_enabled");
        if (item && cJSON_IsBool(item)) {
            *ap_enabled = cJSON_IsTrue(item);
            ESP_LOGI(TAG, "Network: AP mode=%d", *ap_enabled);
        }
    }

    return RBGRID_JSON_OK;
}

/* Parse the tariff plan config ("tariff_plan_list_config") into g_current_tariff_plan. */
rbgrid_json_err_t rbgrid_json_parse_tariff_plan_config(const char* json_str)
{
    if (!json_str) {
        ESP_LOGE(TAG, "Invalid JSON string pointer");
        return RBGRID_JSON_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Parsing tariff plan configuration...");

    // Parse JSON
    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return RBGRID_JSON_ERR_INVALID_JSON;
    }

    // Check type
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type) ||
        strcmp(type->valuestring, "tariff_plan_list_config") != 0) {
        ESP_LOGE(TAG, "Invalid JSON type, expected 'tariff_plan_list_config'");
        cJSON_Delete(root);
        return RBGRID_JSON_ERR_TYPE_MISMATCH;
    }

    // Get data section
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) {
        ESP_LOGE(TAG, "Missing 'data' section");
        cJSON_Delete(root);
        return RBGRID_JSON_ERR_MISSING_FIELD;
    }

    // Get list of plans
    cJSON* list = cJSON_GetObjectItem(data, "list");
    if (!list || !cJSON_IsArray(list)) {
        ESP_LOGE(TAG, "Missing or invalid 'list' array");
        cJSON_Delete(root);
        return RBGRID_JSON_ERR_MISSING_FIELD;
    }

    // Get first plan (we support only one active plan)
    cJSON* plan = cJSON_GetArrayItem(list, 0);
    if (!plan) {
        ESP_LOGE(TAG, "No tariff plans in list");
        cJSON_Delete(root);
        return RBGRID_JSON_ERR_MISSING_FIELD;
    }

    // MERGE, not wipe. A POST carrying only what the UI edits would otherwise zero everything this
    // parser does not read (plan_type, carry_over_credits, max_credit_carry_over, zone descriptions,
    // condition priorities, validity/version/timestamps, load-shifting…) — e.g. a plan save would
    // switch off net-metering credit carry-over. The plan-level fields start from the
    // CURRENT plan and the POST overrides what it carries; zones/payments are rebuilt from the POST,
    // each zone pre-filled from the current zone with the same zone_id.
    rbpower_tariff_plan_t* prev_plan = (rbpower_tariff_plan_t*)malloc(sizeof(rbpower_tariff_plan_t));
    if (prev_plan) *prev_plan = g_current_tariff_plan;
    memset(&g_current_tariff_plan, 0, sizeof(rbpower_tariff_plan_t));
    if (prev_plan) {
        g_current_tariff_plan = *prev_plan;
        memset(g_current_tariff_plan.zones, 0, sizeof(g_current_tariff_plan.zones));
        memset(g_current_tariff_plan.fixed_payments, 0, sizeof(g_current_tariff_plan.fixed_payments));
        g_current_tariff_plan.active_zones_count = 0;
        g_current_tariff_plan.fixed_payments_count = 0;
    }

    // === Parse basic plan information ===
    cJSON* item = NULL;

    // Plan name
    item = cJSON_GetObjectItem(plan, "plan_name");
    if (item && cJSON_IsString(item)) {
        strncpy(g_current_tariff_plan.plan_name, item->valuestring,
                RBPOWER_TARIFF_NAME_LENGTH - 1);
    }

    // Provider name
    item = cJSON_GetObjectItem(plan, "provider_name");
    if (item && cJSON_IsString(item)) {
        strncpy(g_current_tariff_plan.provider_name, item->valuestring,
                RBPOWER_TARIFF_NAME_LENGTH - 1);
    }

    // Description
    item = cJSON_GetObjectItem(plan, "description");
    if (item && cJSON_IsString(item)) {
        strncpy(g_current_tariff_plan.description, item->valuestring,
                RBPOWER_TARIFF_DESCRIPTION_LENGTH - 1);
    }

    // Active zones count
    item = cJSON_GetObjectItem(plan, "active_zones_count");
    if (item && cJSON_IsNumber(item)) {
        g_current_tariff_plan.active_zones_count = (uint8_t)item->valueint;
    }

    // Default zone ID
    item = cJSON_GetObjectItem(plan, "default_zone_id");
    if (item && cJSON_IsString(item)) {
        // Convert string to enum
        for (int i = 0; i < RBPOWER_TARIFF_ZONE_COUNT; i++) {
            if (strcmp(item->valuestring, rbpower_tariff_zone_type_text[i]) == 0) {
                g_current_tariff_plan.default_zone_id = (rbpower_tariff_zone_type_t)i;
                break;
            }
        }
    }

    // Currency code
    item = cJSON_GetObjectItem(plan, "currency_code");
    if (item && cJSON_IsString(item)) {
        strncpy(g_current_tariff_plan.currency_code, item->valuestring,
                RBPOWER_TARIFF_CURRENCY_LENGTH - 1);
    }

    // VAT percent
    item = cJSON_GetObjectItem(plan, "vat_percent");
    if (item && cJSON_IsNumber(item)) {
        g_current_tariff_plan.vat_percent = (float)item->valuedouble;
    }

    // Energy tax per kWh
    item = cJSON_GetObjectItem(plan, "energy_tax_per_kwh");
    if (item && cJSON_IsNumber(item)) {
        g_current_tariff_plan.energy_tax_per_kwh = (float)item->valuedouble;
    }

    // Grid usage fee per kWh
    item = cJSON_GetObjectItem(plan, "grid_usage_fee_per_kwh");
    if (item && cJSON_IsNumber(item)) {
        g_current_tariff_plan.grid_usage_fee_per_kwh = (float)item->valuedouble;
    }

    // Renewable levy per kWh
    item = cJSON_GetObjectItem(plan, "renewable_levy_per_kwh");
    if (item && cJSON_IsNumber(item)) {
        g_current_tariff_plan.renewable_levy_per_kwh = (float)item->valuedouble;
    }

    // === Parse net metering settings ===
    item = cJSON_GetObjectItem(plan, "net_metering_enabled");
    if (item && cJSON_IsBool(item)) {
        g_current_tariff_plan.net_metering_enabled = cJSON_IsTrue(item);
    }

    float default_selling_rate = 0;
    item = cJSON_GetObjectItem(plan, "default_selling_rate_per_kwh");
    if (item && cJSON_IsNumber(item)) {
        default_selling_rate = (float)item->valuedouble;
    }

    item = cJSON_GetObjectItem(plan, "net_metering_default_ratio");
    if (item && cJSON_IsNumber(item)) {
        g_current_tariff_plan.net_metering_default_ratio = (float)item->valuedouble;
    }

    item = cJSON_GetObjectItem(plan, "net_metering_period");
    if (item && cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "DAILY") == 0) {
            g_current_tariff_plan.net_metering_period = RBPOWER_STAT_PERIOD_DAILY;
        } else if (strcmp(item->valuestring, "MONTHLY") == 0) {
            g_current_tariff_plan.net_metering_period = RBPOWER_STAT_PERIOD_MONTHLY;
        } else if (strcmp(item->valuestring, "YEARLY") == 0) {
            g_current_tariff_plan.net_metering_period = RBPOWER_STAT_PERIOD_YEARLY;
        }
    }

    // Active/default flags
    item = cJSON_GetObjectItem(plan, "is_active");
    if (item && cJSON_IsBool(item)) {
        g_current_tariff_plan.is_active = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItem(plan, "is_default");
    if (item && cJSON_IsBool(item)) {
        g_current_tariff_plan.is_default = cJSON_IsTrue(item);
    }

    // Plan-level fields the UI round-trips (present → override; absent →
    // kept from the current plan).
    item = cJSON_GetObjectItem(plan, "plan_type");
    if (item && cJSON_IsString(item)) {
        for (int i = 0; i <= (int)RBPOWER_TARIFF_PLAN_CUSTOM; i++) {
            if (strcmp(item->valuestring, rbpower_tariff_plan_type_text[i]) == 0) {
                g_current_tariff_plan.plan_type = (rbpower_tariff_plan_type_t)i; break;
            }
        }
    }
    item = cJSON_GetObjectItem(plan, "region_code");
    if (item && cJSON_IsString(item)) {
        strncpy(g_current_tariff_plan.region_code, item->valuestring, sizeof(g_current_tariff_plan.region_code) - 1);
        g_current_tariff_plan.region_code[sizeof(g_current_tariff_plan.region_code) - 1] = '\0';
    }
    item = cJSON_GetObjectItem(plan, "carry_over_credits");
    if (item && cJSON_IsBool(item)) g_current_tariff_plan.carry_over_credits = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(plan, "max_credit_carry_over_kwh");
    if (item && cJSON_IsNumber(item)) g_current_tariff_plan.max_credit_carry_over_kwh = (float)item->valuedouble;
    item = cJSON_GetObjectItem(plan, "supported_features");
    if (item && cJSON_IsNumber(item)) g_current_tariff_plan.supported_features = (uint32_t)item->valuedouble;
    item = cJSON_GetObjectItem(plan, "enable_load_shifting_recommendations");
    if (item && cJSON_IsBool(item)) g_current_tariff_plan.enable_load_shifting_recommendations = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(plan, "load_shifting_threshold_percent");
    if (item && cJSON_IsNumber(item)) g_current_tariff_plan.load_shifting_threshold_percent = (float)item->valuedouble;
    item = cJSON_GetObjectItem(plan, "valid_from_timestamp");
    if (item && cJSON_IsNumber(item)) g_current_tariff_plan.valid_from_timestamp = (uint64_t)item->valuedouble;
    item = cJSON_GetObjectItem(plan, "valid_until_timestamp");
    if (item && cJSON_IsNumber(item)) g_current_tariff_plan.valid_until_timestamp = (uint64_t)item->valuedouble;
    item = cJSON_GetObjectItem(plan, "plan_version");
    if (item && cJSON_IsNumber(item)) g_current_tariff_plan.plan_version = (uint32_t)item->valuedouble;
    {   /* audit trail: modified = now (if the clock is valid), created kept (seeded on first save) */
        time_t now = time(NULL);
        uint64_t ts = (now > 1600000000) ? (uint64_t)now : 0;
        if (ts) {
            g_current_tariff_plan.last_modified_timestamp = ts;
            if (g_current_tariff_plan.created_timestamp == 0) g_current_tariff_plan.created_timestamp = ts;
        }
    }

    // === Parse tariff zones ===
    cJSON* zones = cJSON_GetObjectItem(plan, "zones");
    if (zones && cJSON_IsArray(zones)) {
        int zone_index = 0;
        cJSON* zone = NULL;

        cJSON_ArrayForEach(zone, zones) {
            if (zone_index >= RBPOWER_TARIFF_ZONE_COUNT) {
                ESP_LOGW(TAG, "Too many zones, max is %d", RBPOWER_TARIFF_ZONE_COUNT);
                break;
            }

            rbpower_tariff_zone_t* tariff_zone = &g_current_tariff_plan.zones[zone_index];

            // Pre-fill from the current plan's zone with the same zone_id, so the fields this
            // parser doesn't read (description, limits, condition priorities) survive a partial POST.
            {
                bool prefilled = false;
                cJSON* zid = cJSON_GetObjectItem(zone, "zone_id");
                if (prev_plan && zid && cJSON_IsString(zid)) {
                    for (uint8_t pz = 0; pz < prev_plan->active_zones_count && pz < RBPOWER_TARIFF_ZONE_COUNT; pz++) {
                        int pzid = (int)prev_plan->zones[pz].zone_id;
                        if (pzid >= 0 && pzid < RBPOWER_TARIFF_ZONE_COUNT &&
                            strcmp(zid->valuestring, rbpower_tariff_zone_type_text[pzid]) == 0) {
                            *tariff_zone = prev_plan->zones[pz];
                            prefilled = true;
                            break;
                        }
                    }
                }
                if (!prefilled) {
                    /* A zone the previous plan did not have: start from ZERO (the slot held whatever zone
                     * sat there before) and with AND logic like the hub's own builder — otherwise
                     * TIME_RANGE + WEEKDAY would be OR-ed ("peak Mon–Fri 18–21" = every weekday all day
                     * and every evening). */
                    memset(tariff_zone, 0, sizeof(*tariff_zone));
                    tariff_zone->conditions_and_logic = true;
                }
                cJSON* jand = cJSON_GetObjectItem(zone, "conditions_and_logic");
                if (jand && cJSON_IsBool(jand)) tariff_zone->conditions_and_logic = cJSON_IsTrue(jand);
                cJSON* zdesc = cJSON_GetObjectItem(zone, "description");
                if (zdesc && cJSON_IsString(zdesc)) {
                    strncpy(tariff_zone->description, zdesc->valuestring, sizeof(tariff_zone->description) - 1);
                    tariff_zone->description[sizeof(tariff_zone->description) - 1] = '\0';
                }
            }

            // Zone ID
            item = cJSON_GetObjectItem(zone, "zone_id");
            if (item && cJSON_IsString(item)) {
                for (int i = 0; i < RBPOWER_TARIFF_ZONE_COUNT; i++) {
                    if (strcmp(item->valuestring, rbpower_tariff_zone_type_text[i]) == 0) {
                        tariff_zone->zone_id = (rbpower_tariff_zone_type_t)i;
                        break;
                    }
                }
            }

            // Zone name
            item = cJSON_GetObjectItem(zone, "zone_name");
            if (item && cJSON_IsString(item)) {
                strncpy(tariff_zone->zone_name, item->valuestring,
                        RBPOWER_TARIFF_NAME_LENGTH - 1);
            }

            // Rate buying per kWh
            item = cJSON_GetObjectItem(zone, "rate_buying_per_kwh");
            if (item && cJSON_IsNumber(item)) {
                tariff_zone->rate_buying_per_kwh = (float)item->valuedouble;
            }

            // Explicit feed-in fields win (round-trip of what GET serializes); otherwise derive
            // the selling rate from the plan's net-metering settings.
            cJSON* sell_j  = cJSON_GetObjectItem(zone, "rate_selling_per_kwh");
            cJSON* ratio_j = cJSON_GetObjectItem(zone, "net_metering_ratio");
            bool sell_explicit  = (sell_j  && cJSON_IsNumber(sell_j)  && sell_j->valuedouble  >= 0.0);
            bool ratio_explicit = (ratio_j && cJSON_IsNumber(ratio_j) && ratio_j->valuedouble >= 0.0);
            if (sell_explicit)  tariff_zone->rate_selling_per_kwh = (float)sell_j->valuedouble;
            if (ratio_explicit) tariff_zone->net_metering_ratio   = (float)ratio_j->valuedouble;
            // Calculate selling rate based on net metering settings
            if (g_current_tariff_plan.net_metering_enabled) {
                if (!sell_explicit) {
                    if (g_current_tariff_plan.net_metering_default_ratio > 0) {
                        // Use ratio to calculate selling rate
                        tariff_zone->rate_selling_per_kwh =
                            tariff_zone->rate_buying_per_kwh * g_current_tariff_plan.net_metering_default_ratio;
                    } else if (default_selling_rate > 0) {
                        // Use fixed selling rate
                        tariff_zone->rate_selling_per_kwh = default_selling_rate;
                    }
                }
                if (!ratio_explicit) tariff_zone->net_metering_ratio = g_current_tariff_plan.net_metering_default_ratio;
            }

            // Set default multipliers
            tariff_zone->base_multiplier = 1.0f;
            tariff_zone->seasonal_multiplier = 1.0f;
            tariff_zone->critical_peak_multiplier = 1.0f;

            // Enable zone
            tariff_zone->is_enabled = true;

            // === Parse conditions ===
            cJSON* conditions = cJSON_GetObjectItem(zone, "conditions");
            if (conditions && cJSON_IsArray(conditions)) {
                int cond_index = 0;
                cJSON* condition = NULL;

                cJSON_ArrayForEach(condition, conditions) {
                    if (cond_index >= 4) {
                        ESP_LOGW(TAG, "Too many conditions for zone, max is 4");
                        break;
                    }

                    rbpower_tariff_condition_t* zone_cond = &tariff_zone->conditions[cond_index];

                    // priority / is_active round-trip with GET (otherwise they would reset to 0)
                    {
                        cJSON* pr = cJSON_GetObjectItem(condition, "priority");
                        if (pr && cJSON_IsNumber(pr)) zone_cond->priority = (uint8_t)pr->valueint;
                        cJSON* ia = cJSON_GetObjectItem(condition, "is_active");
                        if (ia && cJSON_IsBool(ia)) zone_cond->is_active = cJSON_IsTrue(ia);
                    }

                    // Condition type
                    item = cJSON_GetObjectItem(condition, "condition_type");
                    if (item && cJSON_IsString(item)) {
                        if (strcmp(item->valuestring, "TIME_RANGE") == 0) {
                            zone_cond->condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;

                            // Parse time range params
                            cJSON* params = cJSON_GetObjectItem(condition, "params");
                            if (params) {
                                cJSON* param = cJSON_GetObjectItem(params, "start_hour");
                                if (param) zone_cond->params.time_range.start_hour = (uint8_t)param->valueint;

                                param = cJSON_GetObjectItem(params, "start_minute");
                                if (param) zone_cond->params.time_range.start_minute = (uint8_t)param->valueint;

                                param = cJSON_GetObjectItem(params, "end_hour");
                                if (param) zone_cond->params.time_range.end_hour = (uint8_t)param->valueint;

                                param = cJSON_GetObjectItem(params, "end_minute");
                                if (param) zone_cond->params.time_range.end_minute = (uint8_t)param->valueint;

                                param = cJSON_GetObjectItem(params, "spans_midnight");
                                if (param) zone_cond->params.time_range.spans_midnight = cJSON_IsTrue(param);
                            }
                        } else if (strcmp(item->valuestring, "WEEKDAY") == 0) {
                            zone_cond->condition_type = RBPOWER_TARIFF_CONDITION_WEEKDAY;

                            // Parse weekday params
                            cJSON* params = cJSON_GetObjectItem(condition, "params");
                            if (params) {
                                cJSON* param = cJSON_GetObjectItem(params, "weekday_mask");
                                if (param) zone_cond->params.weekday.weekday_mask = (uint8_t)param->valueint;
                            }
                        } else if (strcmp(item->valuestring, "WEEKEND") == 0) {
                            zone_cond->condition_type = RBPOWER_TARIFF_CONDITION_WEEKEND;      /* no params (Sat/Sun) */
                        } else if (strcmp(item->valuestring, "HOLIDAY") == 0) {
                            zone_cond->condition_type = RBPOWER_TARIFF_CONDITION_HOLIDAY;      /* no params (holiday list) */
                        } else if (strcmp(item->valuestring, "DATE_RANGE") == 0) {
                            zone_cond->condition_type = RBPOWER_TARIFF_CONDITION_DATE_RANGE;
                            cJSON* params = cJSON_GetObjectItem(condition, "params");
                            if (params) {
                                cJSON* param;
                                if ((param = cJSON_GetObjectItem(params, "start_month")) && cJSON_IsNumber(param)) zone_cond->params.date_range.start_month = (uint8_t)param->valueint;
                                if ((param = cJSON_GetObjectItem(params, "start_day"))   && cJSON_IsNumber(param)) zone_cond->params.date_range.start_day   = (uint8_t)param->valueint;
                                if ((param = cJSON_GetObjectItem(params, "end_month"))   && cJSON_IsNumber(param)) zone_cond->params.date_range.end_month   = (uint8_t)param->valueint;
                                if ((param = cJSON_GetObjectItem(params, "end_day"))     && cJSON_IsNumber(param)) zone_cond->params.date_range.end_day     = (uint8_t)param->valueint;
                                if ((param = cJSON_GetObjectItem(params, "recurring_annually"))) zone_cond->params.date_range.recurring_annually = cJSON_IsTrue(param);
                            }
                        } else if (strcmp(item->valuestring, "SPECIAL_EVENT") == 0) {
                            zone_cond->condition_type = RBPOWER_TARIFF_CONDITION_SPECIAL_EVENT; /* round-trips; the evaluator treats it as never active */
                            cJSON* params = cJSON_GetObjectItem(condition, "params");
                            if (params) {
                                cJSON* param;
                                if ((param = cJSON_GetObjectItem(params, "event_start_timestamp")) && cJSON_IsNumber(param)) zone_cond->params.special_event.event_start_timestamp = (uint64_t)param->valuedouble;
                                if ((param = cJSON_GetObjectItem(params, "event_end_timestamp"))   && cJSON_IsNumber(param)) zone_cond->params.special_event.event_end_timestamp   = (uint64_t)param->valuedouble;
                                if ((param = cJSON_GetObjectItem(params, "event_id"))              && cJSON_IsNumber(param)) zone_cond->params.special_event.event_id              = (uint32_t)param->valuedouble;
                            }
                        } else {
                            /* SEASON / MONTH / unknown: nothing evaluates them — skip the slot instead of
                             * counting a condition whose type is whatever the slot held before. */
                            ESP_LOGW(TAG, "tariff zone %d: condition_type '%s' not supported — skipped", zone_index, item->valuestring);
                            continue;
                        }

                        zone_cond->is_active = true;
                        cond_index++;
                    }
                }

                /* slots beyond the parsed list are stale (a deleted condition kept is_active and
                 * showed up in GET while the evaluator ignored it) — clear them */
                for (int k = cond_index; k < 4; k++) {
                    memset(&tariff_zone->conditions[k], 0, sizeof(tariff_zone->conditions[k]));
                }
                tariff_zone->conditions_count = cond_index;
            }

            zone_index++;
        }

        // Update actual zones count
        g_current_tariff_plan.active_zones_count = zone_index;
    }

    // === Parse fixed payments ===
    cJSON* fixed_payments = cJSON_GetObjectItem(plan, "fixed_payments");
    if (fixed_payments && cJSON_IsArray(fixed_payments)) {
        int payment_index = 0;
        cJSON* payment = NULL;

        cJSON_ArrayForEach(payment, fixed_payments) {
            if (payment_index >= 8) {
                ESP_LOGW(TAG, "Too many fixed payments, max is 8");
                break;
            }

            rbpower_tariff_fixed_payment_t* fixed_payment =
                &g_current_tariff_plan.fixed_payments[payment_index];

            // Payment type
            item = cJSON_GetObjectItem(payment, "payment_type");
            if (item && cJSON_IsString(item)) {
                if (strcmp(item->valuestring, "FIXED") == 0) {
                    fixed_payment->payment_type = RBPOWER_TARIFF_FIXED_MONTHLY;
                } else if (strcmp(item->valuestring, "DEMAND") == 0) {
                    fixed_payment->payment_type = RBPOWER_TARIFF_FIXED_POWER_BASED;
                }
            }

            // Payment name
            item = cJSON_GetObjectItem(payment, "payment_name");
            if (item && cJSON_IsString(item)) {
                strncpy(fixed_payment->payment_name, item->valuestring,
                        RBPOWER_TARIFF_NAME_LENGTH - 1);
            }

            // Amount
            item = cJSON_GetObjectItem(payment, "amount");
            if (item && cJSON_IsNumber(item)) {
                fixed_payment->amount = (float)item->valuedouble;
            }

            // Is enabled
            item = cJSON_GetObjectItem(payment, "is_enabled");
            if (item && cJSON_IsBool(item)) {
                fixed_payment->is_enabled = cJSON_IsTrue(item);
            }

            // Billing period
            item = cJSON_GetObjectItem(payment, "billing_period");
            if (item && cJSON_IsString(item)) {
                if (strcmp(item->valuestring, "MONTHLY") == 0) {
                    fixed_payment->billing_period = RBPOWER_STAT_PERIOD_MONTHLY;
                } else if (strcmp(item->valuestring, "YEARLY") == 0) {
                    fixed_payment->billing_period = RBPOWER_STAT_PERIOD_YEARLY;
                }
            }

            payment_index++;
        }

        g_current_tariff_plan.fixed_payments_count = payment_index;
    }

    cJSON_Delete(root);
    if (prev_plan) free(prev_plan);

    ESP_LOGI(TAG, "Tariff plan parsed successfully:");
    ESP_LOGI(TAG, "  - Plan: %s", g_current_tariff_plan.plan_name);
    ESP_LOGI(TAG, "  - Provider: %s", g_current_tariff_plan.provider_name);
    ESP_LOGI(TAG, "  - Zones: %d", g_current_tariff_plan.active_zones_count);
    ESP_LOGI(TAG, "  - Fixed payments: %d", g_current_tariff_plan.fixed_payments_count);
    ESP_LOGI(TAG, "  - Net metering: %s",
             g_current_tariff_plan.net_metering_enabled ? "enabled" : "disabled");

    return RBGRID_JSON_OK;
}

/* ================================================================================================
 * FLASH HISTORY SERIALIZATION FUNCTIONS
 * ================================================================================================ */

/* Serialize an ELECTRICAL history record into a JSON object (caller owns it; cJSON_Delete). */
cJSON* rbjson_serialize_electrical(const rbstats_electrical_v4_t* data) {
    if (!data) {
        ESP_LOGE(TAG, "rbjson_serialize_electrical: data is NULL");
        return NULL;
    }

    cJSON* obj = cJSON_CreateObject();
    if (!obj) {
        ESP_LOGE(TAG, "Failed to create JSON object for ELECTRICAL");
        return NULL;
    }

    // Timestamp (v4 header; same value as the stored record)
    cJSON_AddNumberToObject(obj, "ts", data->h.ts);
    // v4 header: covered duration, quality bitmask, source resolution class
    cJSON_AddNumberToObject(obj, "dur", data->h.dur);
    cJSON_AddNumberToObject(obj, "q",   data->h.q);
    cJSON_AddNumberToObject(obj, "res", data->h.res);

    // Electrical parameters (converted from the packed formats: V×10, mA, W×10, var×10, VA×10, pf×100)
    cJSON_AddNumberToObject(obj, "v", data->voltage_rms_v10 / 10.0);
    cJSON_AddNumberToObject(obj, "i", data->current_rms_ma / 1000.0);
    cJSON_AddNumberToObject(obj, "p_act", data->power_active_w10 / 10.0);  // same key as telemetry
    cJSON_AddNumberToObject(obj, "p_rea", data->power_reactive_var10 / 10.0);   /* p_act/p_rea/p_app triad; "q" is the quality bitmask */
    cJSON_AddNumberToObject(obj, "p_app", data->power_apparent_va10 / 10.0);
    cJSON_AddNumberToObject(obj, "pf", data->power_factor_100 / 100.0);
    cJSON_AddNumberToObject(obj, "f", data->frequency_hz10);
    cJSON_AddNumberToObject(obj, "thd_v", data->thd_voltage_100 / 100.0);
    cJSON_AddNumberToObject(obj, "thd_i", data->thd_current_100 / 100.0);

    // Per-period envelope min/max (charts draw bands around the avg lines).
    cJSON_AddNumberToObject(obj, "v_min", data->voltage_min_v10 / 10.0);
    cJSON_AddNumberToObject(obj, "v_max", data->voltage_max_v10 / 10.0);
    cJSON_AddNumberToObject(obj, "i_min", data->current_min_ma / 1000.0);
    cJSON_AddNumberToObject(obj, "i_max", data->current_max_ma / 1000.0);
    cJSON_AddNumberToObject(obj, "p_min", data->power_min_w10 / 10.0);
    cJSON_AddNumberToObject(obj, "p_max", data->power_max_w10 / 10.0);

    return obj;
}

/* Serialize an ENERGY history record into a JSON object (caller owns it; cJSON_Delete). */
cJSON* rbjson_serialize_energy(const rbstats_energy_v4_t* data) {
    if (!data) {
        ESP_LOGE(TAG, "rbjson_serialize_energy: data is NULL");
        return NULL;
    }

    cJSON* obj = cJSON_CreateObject();
    if (!obj) {
        ESP_LOGE(TAG, "Failed to create JSON object for ENERGY");
        return NULL;
    }

    // Timestamp (v4 header)
    cJSON_AddNumberToObject(obj, "ts", data->h.ts);
    // v4 header: covered duration, quality bitmask, source resolution class
    cJSON_AddNumberToObject(obj, "dur", data->h.dur);
    cJSON_AddNumberToObject(obj, "q",   data->h.q);
    cJSON_AddNumberToObject(obj, "res", data->h.res);

    // Energy in Wh (stored as mWh).
    // en is DERIVED = ec - es (net is never stored).
    double ec_wh = (double)data->ec_mwh / 1000.0;
    double es_wh = (double)data->es_mwh / 1000.0;
    cJSON_AddNumberToObject(obj, "ec", ec_wh);
    cJSON_AddNumberToObject(obj, "es", es_wh);
    cJSON_AddNumberToObject(obj, "en", ec_wh - es_wh);
    /* Lifetime totalizers at period end (Wh); absent on records migrated from v3 (they carry 0). */
    if (!(data->h.q & RBSTATS_Q_MIGRATED)) {
        cJSON_AddNumberToObject(obj, "et_in",  (double)data->et_in_mwh  / 1000.0);
        cJSON_AddNumberToObject(obj, "et_out", (double)data->et_out_mwh / 1000.0);
    }

    return obj;
}

/* Serialize an ECONOMICS history record into a JSON object (caller owns it; cJSON_Delete). */
cJSON* rbjson_serialize_economics(const rbstats_econ_v4_t* data) {
    if (!data) {
        ESP_LOGE(TAG, "rbjson_serialize_economics: data is NULL");
        return NULL;
    }

    cJSON* obj = cJSON_CreateObject();
    if (!obj) {
        ESP_LOGE(TAG, "Failed to create JSON object for ECONOMICS");
        return NULL;
    }

    // Timestamp (v4 header)
    cJSON_AddNumberToObject(obj, "ts", data->h.ts);
    // v4 header: covered duration, quality bitmask, source resolution class
    cJSON_AddNumberToObject(obj, "dur", data->h.dur);
    cJSON_AddNumberToObject(obj, "q",   data->h.q);
    cJSON_AddNumberToObject(obj, "res", data->h.res);

    // Minor units (cents) -> currency; n is DERIVED = c - r (net never stored).
    double c = (double)data->c_cents / 100.0;
    double r = (double)data->r_cents / 100.0;
    cJSON_AddNumberToObject(obj, "c", c);
    cJSON_AddNumberToObject(obj, "r", r);
    cJSON_AddNumberToObject(obj, "n", c - r);
    cJSON_AddStringToObject(obj, "cur", g_current_tariff_plan.currency_code);
    cJSON_AddNumberToObject(obj, "plan_id",  data->plan_id);
    cJSON_AddNumberToObject(obj, "plan_ver", data->plan_ver);

    return obj;
}

/* There are no QUALITY/DIRECTION JSON serializers — those period data types are not part of the
 * pipeline (the rbAmp modules provide no zero-cross/THD source; THD stays in the electrical record). */

/* ===== Numeric config ranges — ONE table for `/api/v1/schema limits.ranges` and the POST guard =====
 * Without it a client could post out-of-range values (e.g. a negative EV threshold, or a cleared field
 * arriving as 0) and narrow fields would silently truncate (uint8).
 * Keys are the leaf field names as they appear in the POST body (unique across sections); `path` is the
 * dotted location inside the section object; aliases (grid.nominal_v/nominal_hz = read-only mirror of
 * hardware network.*) carry the same range. zero_unset: 0 = "not set" is accepted besides [min..max].
 * The adc.* rows guard the legacy ADC-typed hardware config. */
/** @struct rbgrid_cfg_range_t
 *  @brief One numeric config range: drives `/api/v1/schema limits.ranges` and the POST guard. */
/** @var rbgrid_cfg_range_t::section
 *  Config section the field belongs to ("system", "hardware", "telemetry"). */
/** @var rbgrid_cfg_range_t::path
 *  Dotted location of the field inside the section object. */
/** @var rbgrid_cfg_range_t::key
 *  Leaf field name as it appears in the POST body (unique across sections). */
/** @var rbgrid_cfg_range_t::min
 *  Lowest accepted value (in the field's own unit). */
/** @var rbgrid_cfg_range_t::max
 *  Highest accepted value (in the field's own unit). */
/** @var rbgrid_cfg_range_t::decimals
 *  Number of decimals a client should offer / show. */
/** @var rbgrid_cfg_range_t::zero_unset
 *  true: 0 means "not set" and is accepted besides [min..max]. */
/** @var rbgrid_cfg_range_t::read_only
 *  true: published in the schema only, not checked on POST (mirror or not POSTable). */
typedef struct { const char* section; const char* path; const char* key; double min, max; uint8_t decimals; bool zero_unset; bool read_only; } rbgrid_cfg_range_t;
static const rbgrid_cfg_range_t s_cfg_ranges[] = {
    { "system",   "logging_level",          "logging_level",       0,      3,     0, false, false },
    { "system",   "max_retry_attempts",     "max_retry_attempts",  0,      255,   0, false, false },
    { "system",   "retry_delay_ms",         "retry_delay_ms",      0,      600000,0, false, false },
    { "system",   "ev.charge_threshold_w",  "charge_threshold_w",  0,      100000,0, false, false },
    { "system",   "ev.idle_timeout_min",    "idle_timeout_min",    0,      1440,  0, false, false },
    { "system",   "ev.price_per_kwh",       "price_per_kwh",       0,      1000,  4, false, false },
    { "hardware", "adc.channels_count",     "channels_count",      0,      8,     0, false, false },
    { "hardware", "adc.sampling_frequency", "sampling_frequency",  1000,   50000, 0, false, false },
    { "hardware", "adc.adc_resolution",     "adc_resolution",      9,      12,    0, false, false },
    { "hardware", "adc.adc_attenuation",    "adc_attenuation",     0,      3,     0, false, false },
    { "hardware", "network.nominal_voltage","nominal_voltage",     90,     480,   0, true,  false },
    { "hardware", "network.nominal_frequency","nominal_frequency", 45,     65,    0, true,  false },
    { "system",   "grid.nominal_v",         "nominal_v",           90,     480,   0, true,  true  },   /* read-only mirror */
    { "system",   "grid.nominal_hz",        "nominal_hz",          45,     65,    0, true,  true  },   /* read-only mirror */
    { "telemetry","json_buffer_size",       "json_buffer_size",    1024,   65536, 0, false, true  },   /* not POSTable today */
};

void rbgrid_json_add_config_ranges(cJSON* limits)
{
    if (!limits) return;
    cJSON* r = cJSON_AddObjectToObject(limits, "ranges");
    if (!r) return;
    for (size_t i = 0; i < sizeof(s_cfg_ranges) / sizeof(s_cfg_ranges[0]); i++) {
        const rbgrid_cfg_range_t* e = &s_cfg_ranges[i];
        cJSON* o = cJSON_AddObjectToObject(r, e->key);
        if (!o) continue;
        cJSON_AddNumberToObject(o, "min", e->min);
        cJSON_AddNumberToObject(o, "max", e->max);
        cJSON_AddNumberToObject(o, "decimals", e->decimals);
        cJSON_AddStringToObject(o, "section", e->section);
        cJSON_AddStringToObject(o, "path", e->path);
        if (e->zero_unset) cJSON_AddBoolToObject(o, "zero_unset", true);
        if (e->read_only)  cJSON_AddBoolToObject(o, "read_only", true);
    }
}

/** @brief Resolve a dotted path (max 47 chars) inside a JSON object; NULL when any step is missing. */
static const cJSON* cfg_range_resolve(const cJSON* obj, const char* path)
{
    char buf[48]; strlcpy(buf, path, sizeof(buf));
    const cJSON* cur = obj;
    char* save = NULL;
    for (char* tok = strtok_r(buf, ".", &save); tok && cur; tok = strtok_r(NULL, ".", &save))
        cur = cJSON_GetObjectItem(cur, tok);
    return cur;
}

/* POST guard: every table entry of `section` present in `obj` must be a number inside its range.
 * Returns true when all good; otherwise fills field (dotted path) + msg for the 400 body. Only
 * fields that are PRESENT are checked (absent = keep current, as before). */
bool rbgrid_config_check_ranges(const char* section, const cJSON* obj, char* field, size_t fcap, char* msg, size_t mcap)
{
    if (!section || !obj) return true;
    for (size_t i = 0; i < sizeof(s_cfg_ranges) / sizeof(s_cfg_ranges[0]); i++) {
        const rbgrid_cfg_range_t* e = &s_cfg_ranges[i];
        if (strcmp(e->section, section) != 0 || e->read_only) continue;
        const cJSON* it = cfg_range_resolve(obj, e->path);
        if (!it) continue;
        if (!cJSON_IsNumber(it)) {
            if (field) strlcpy(field, e->path, fcap);
            if (msg) snprintf(msg, mcap, "%s must be a number", e->path);
            return false;
        }
        double v = it->valuedouble;
        if (e->zero_unset && v == 0.0) continue;
        if (v < e->min || v > e->max) {
            if (field) strlcpy(field, e->path, fcap);
            if (msg) snprintf(msg, mcap, "%s out of range [%.*f..%.*f]%s", e->path, e->decimals, e->min, e->decimals, e->max,
                              e->zero_unset ? " (0 = unset)" : "");
            return false;
        }
    }
    return true;
}
