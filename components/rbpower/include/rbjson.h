/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbjson.h
 * @brief JSON conversion module for the rbpower library
 *
 * Converts rbpower configuration, state and measurement data to and from JSON. It produces the
 * configuration documents stored on flash and served by the HTTP API (system, hardware, time,
 * tariff, telemetry, WiFi, units, rbAmp fleet), the live telemetry payload, the Home Assistant
 * MQTT Discovery messages and the flash-history record serializers, and it parses the
 * corresponding request bodies and stored files. The JSON keys are part of the public API
 * contract; see the API reference.
 *
 * Memory ownership: functions returning `char *` return a heap string allocated by cJSON
 * (malloc with the default hooks); the caller frees it with free() / cJSON_free(). Functions
 * returning `cJSON *` return a tree the caller releases with cJSON_Delete() or attaches to a
 * parent object.
 *
 * Thread safety: serializers read shared state (g_home_network, the global tariff plan) without
 * taking locks unless stated otherwise; callers that need a consistent snapshot hold the home
 * network lock (rbpower_home_lock()).
 */

#ifndef RBJSON_H
#define RBJSON_H

#include <cJSON.h>
#include "rbpower.h"
#include "rbstorage_transfer_types.h"
#include "rbstats_v4_types.h"   /* v4 record structs */
#include "rbgrid_rbamp_config.h"   /* rbamp_bridge_config_t — rbAmp fleet config serialize/parse */
#include "rbgrid_config_api.h"     /* rbgrid_create_unit_req_t — POST /api/v1/units bind parser */
#include "esp_err.h"

/** @brief Telemetry configuration (forward declaration; defined in rbtelemetry.h). */
typedef struct rbpower_telemetry_config_s rbpower_telemetry_config_t;

#ifdef __cplusplus
extern "C"
{
#endif

    /* ================================================================================================
     * CONSTANTS AND MACROS
     * ================================================================================================ */

#define RBGRID_JSON_VERSION "2.0"              ///< JSON message format version
#define RBGRID_JSON_MAX_DEVICE_ID_LEN 32       ///< Buffer size of a device id / device name, in bytes
#define RBGRID_JSON_MAX_UNIT_NAME_LEN 64       ///< Buffer size of a unit name, in bytes
#define RBGRID_JSON_MAX_SENSOR_NAME_LEN 64     ///< Buffer size of a sensor name, in bytes
#define RBGRID_JSON_MAX_TARIFF_NAME_LEN 64     ///< Buffer size of a tariff name, in bytes

    /* ================================================================================================
     * ERROR TYPES
     * ================================================================================================ */

    /** @brief Result codes of the JSON module. */
    typedef enum
    {
        RBGRID_JSON_OK = 0,              ///< Success
        RBGRID_JSON_ERR_INVALID_ARG,     ///< NULL or invalid argument
        RBGRID_JSON_ERR_NO_MEMORY,       ///< Allocation failed
        RBGRID_JSON_ERR_INVALID_JSON,    ///< Input is not valid JSON
        RBGRID_JSON_ERR_MISSING_FIELD,   ///< A required field is missing
        RBGRID_JSON_ERR_TYPE_MISMATCH,   ///< A field has the wrong JSON type
        RBGRID_JSON_ERR_INVALID_CONFIG,  ///< Values are out of range or inconsistent
        RBGRID_JSON_ERR_SYSTEM_ERROR     ///< Internal error
    } rbgrid_json_err_t;

    /* ================================================================================================
     * CONFIGURATION STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Basic device information for JSON messages
     */
    typedef struct
    {
        char device_id[RBGRID_JSON_MAX_DEVICE_ID_LEN];   ///< Device ID (NUL-terminated)
        char device_name[RBGRID_JSON_MAX_DEVICE_ID_LEN]; ///< Device name (NUL-terminated)
        char location[64];                               ///< Installation location (NUL-terminated)
        char timezone[32];                               ///< Timezone string (NUL-terminated)
    } rbgrid_json_device_info_t;

    /**
     * @brief JSON module context (device identity and time base added to messages)
     */
    typedef struct
    {
        rbgrid_json_device_info_t device_info; ///< Device information
        bool ntp_enabled;                      ///< NTP synchronization enabled
        uint64_t boot_time_ntp;                ///< Boot time from NTP (wall-clock)
        uint32_t boot_count;                   ///< Boot counter
    } rbgrid_json_context_t;

    /* ================================================================================================
     * MAIN JSON CONVERSION FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Initialize a JSON module context.
     *
     * Copies @p device_info into @p context, sets the boot time from the current clock and
     * increments the boot counter.
     * @param[out] context     Context to initialize.
     * @param[in]  device_info Device information to copy.
     * @retval RBGRID_JSON_OK          Success.
     * @retval RBGRID_JSON_ERR_INVALID_ARG @p context or @p device_info is NULL.
     */
    rbgrid_json_err_t rbgrid_json_init(rbgrid_json_context_t *context,
                                       const rbgrid_json_device_info_t *device_info);

    /**
     * @brief Serialize the system configuration (the "system" document stored in /config.json
     *        and served by the configuration API).
     * @param[in] config Extended configuration to serialize.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_system_config(const rbpower_extended_config_t *config);

    /**
     * @brief Serialize the hardware configuration (message type "hardware").
     *
     * The document contains the network nominal values and the legacy ADC-typed hardware
     * section that describes the default units fed by the rbAmp bridge.
     * @param[in] hardware_config Hardware configuration to serialize.
     * @param[in] context         JSON module context, or NULL for the compact on-flash form.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_hardware_config(const rbpower_hardware_config_t *hardware_config,
                                             const rbgrid_json_context_t *context);

    /**
     * @brief Create the list of known sensor types (message type "adc_sensor_list").
     *
     * The "data.list" array holds every sensor type name except UNKNOWN. The message type keeps
     * its historical name for API compatibility.
     * @param[in] context JSON module context (required).
     * @return Heap JSON string, or NULL when @p context is NULL or on allocation failure.
     *         The caller frees it with free().
     */
    char *rbgrid_json_create_adc_sensor_list(const rbgrid_json_context_t *context);

    /**
     * @brief Serialize the time configuration and time statistics (message type "time_config").
     *
     * "config" holds the time sources, SNTP servers, timezone and intervals; "statistics" holds
     * the sync status, counters and accuracy.
     * @param[in] hardware_config Hardware configuration.
     * @param[in] context         JSON module context.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_time_config(const rbpower_hardware_config_t *hardware_config,
                                         const rbgrid_json_context_t *context);

    /**
     * @brief Serialize the tariff plans (message type "tariff_plan_list_config").
     *
     * "list" holds the plans of the global tariff context; each plan has its basic information,
     * tariff zones with conditions, progressive configuration, fixed payments and settings.
     * @param[in] context JSON module context.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_rbpower_tariff_plan_config(const rbgrid_json_context_t *context);

    /**
     * @brief Serialize the telemetry configuration (message type "telemetry_config").
     *
     * Contains telemetry_enabled, json_buffer_size, compact_json and the "channels" array
     * (MQTT, HTTP, WebSocket) with per-protocol settings.
     * @param[in] extended_config Extended configuration holding the telemetry settings.
     * @param[in] context         JSON module context.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_telemetry_config(const rbpower_extended_config_t *extended_config,
                                              const rbgrid_json_context_t *context);

    /**
     * @brief Serialize the WiFi configuration (message type "wifi_config").
     *
     * Contains the operation mode (APSTA), the access point (SSID, channel, IP address) and the
     * station (SSID, connection status, IP address).
     * @param[in] context JSON module context.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_wifi_config(const rbgrid_json_context_t *context);

    /**
     * @brief Serialize the units configuration (message type "units_config"; stored as /units.json).
     *
     * Contains the "units" array built from g_home_network, units_count, active_units_count and
     * system_healthy. Reads g_home_network: callers hold rbpower_home_lock() for a consistent view.
     * @param[in] context JSON module context, or NULL to use the module's internal context.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_units_config(const rbgrid_json_context_t *context);

    /**
     * @brief Build the sensor registry list served by GET /api/v1/sensors.
     *
     * Lists bound sensors (unit_id as hex) and unbound rbAmp sensors (unit_id null), each with
     * iface, present, addressing and addr.
     * @param[in] context JSON module context.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_sensors_list(const rbgrid_json_context_t *context);

    /**
     * @brief Parse a POST /api/v1/units bind body into a create-unit request.
     *
     * Resolves the role and stat_class strings to enums (defaults DEVICE_LOAD and 1HOUR) and
     * validates the minimal fields: sensor.iface, its addressing and a non-empty name.
     * @param[in]  json Request body (NUL-terminated).
     * @param[out] out  Request structure to fill.
     * @retval RBGRID_JSON_OK              Parsed.
     * @retval RBGRID_JSON_ERR_INVALID_ARG A NULL argument, or an unknown sensor iface.
     * @retval RBGRID_JSON_ERR_INVALID_JSON The body is not valid JSON.
     * @retval RBGRID_JSON_ERR_MISSING_FIELD A required field is missing or has the wrong type.
     */
    rbgrid_json_err_t rbgrid_json_parse_create_unit(const char *json, rbgrid_create_unit_req_t *out);

    /**
     * @brief Publish the Home Assistant MQTT Discovery configs (retained) so Home Assistant
     *        creates every device and entity automatically.
     *
     * Register it with rbmqtt_register_connect_cb() so it republishes on each connect. Devices are
     * identified by the MAC address; the state topics are rbgrid/&lt;mac&gt;/telemetry and
     * rbgrid/&lt;mac&gt;/status. Runs in the dedicated discovery task, not in the MQTT event task.
     */
    void rbgrid_json_publish_ha_discovery(void);

    /**
     * @brief Remove a deleted unit's Home Assistant entities by publishing an empty retained
     *        config on each of its discovery topics. Does nothing when MQTT is not connected.
     * @param[in] unit_id Unit id.
     * @param[in] role    Unit role (an rbpower_load_role_t value); the main and alternative
     *                    supplies use the fixed keys "main" / "alt".
     */
    void rbgrid_json_ha_unpublish_unit(uint16_t unit_id, int role);

    /**
     * @brief Ask the Home Assistant discovery task to republish (idempotent, retained).
     *
     * The component provides a weak no-op default; the application defines the real function,
     * which owns the discovery task. Called after unit create/rebind/rename so new units get
     * their entities without waiting for an MQTT reconnect.
     */
    void rbgrid_ha_discovery_request(void);

    /**
     * @brief Serialize the rbAmp fleet configuration (message type "rbamp_config").
     * @param[in] cfg Fleet configuration to serialize (bus parameters and module/entry map).
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_rbamp_config(const rbamp_bridge_config_t *cfg);

    /**
     * @brief Parse an "rbamp_config" JSON string into a fleet configuration (bounds-checked).
     * @param[in]  json_str JSON produced by rbgrid_json_create_rbamp_config().
     * @param[out] out      Fleet configuration, fully overwritten on success.
     * @retval RBGRID_JSON_OK               Parsed.
     * @retval RBGRID_JSON_ERR_INVALID_ARG  A NULL argument.
     * @retval RBGRID_JSON_ERR_INVALID_JSON The input is not valid JSON.
     * @retval RBGRID_JSON_ERR_INVALID_CONFIG The document is not a valid rbAmp configuration.
     */
    rbgrid_json_err_t rbgrid_json_parse_rbamp_config(const char *json_str, rbamp_bridge_config_t *out);

    /**
     * @brief Create the live telemetry payload in the currently selected frame version
     *        (see rbgrid_telemetry_set_frame()).
     *
     * The payload carries the schema version "v", timestamps (uptime ms and epoch s), system
     * frequency (Hz), net power direction and value (W) and every unit (voltage bus, main supply,
     * alternative supply, regular units). See the API reference for the exact keys.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char *rbgrid_json_create_telemetry(void);

    /**
     * @brief Telemetry payload schema version (the "v" field).
     *
     * Bump it on any change of the telemetry shape. It is also reported by the /api/v1/schema
     * descriptor.
     */
    #define RBGRID_TELEMETRY_SCHEMA_VERSION 1

    /**
     * @brief Select the telemetry profile.
     *
     * Lean drops the lifetime-energy and economics fields from the periodic live push (clients
     * read them on demand from /api/v1/summary, /economics and /history), which saves CPU on each
     * push. The schema version does not change; lean only omits keys.
     * @param[in] on true = lean (realtime values only), false = full (default).
     */
    void rbgrid_telemetry_set_lean(bool on);

    /**
     * @brief Get the telemetry profile.
     * @return true when the lean profile is selected.
     */
    bool rbgrid_telemetry_get_lean(void);

    /**
     * @brief Select the global telemetry frame version (drives MQTT and Home Assistant discovery).
     *
     * Frame 1 uses the legacy keys ("main"/"alt"/"v_bus" plus hex ids). Frame 2 (default) keys
     * every unit by its upper-case hex unit_id and adds type/role/caps/res/q_live plus top-level
     * main_unit/alt_unit/vbus_unit references. HTTP and WebSocket clients can request frame 2
     * per request independently.
     * @param[in] f Frame version; 2 selects frame 2, any other value selects frame 1.
     */
    void    rbgrid_telemetry_set_frame(uint8_t f);

    /**
     * @brief Get the global telemetry frame version.
     * @return 1 or 2.
     */
    uint8_t rbgrid_telemetry_get_frame(void);

    /**
     * @brief Create the live telemetry payload in a given frame version.
     * @param[in] frame Frame version (1 or 2), see rbgrid_telemetry_set_frame().
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     */
    char*   rbgrid_json_create_telemetry_frame(uint8_t frame);

    /**
     * @brief Get the canonical upper-case name of a unit role, as used in the units
     *        configuration and enumerated as `enums.role` by /api/v1/schema.
     * @param[in] idx Role index (an rbpower_load_role_t value).
     * @return Static string, or NULL when @p idx is past the last role.
     */
    const char* rbpower_load_role_name(unsigned idx);

    /* Disabled: rbgrid_json_create_measurement_data() would serialize measurement results
     * (network parameters, per-unit electrical/energy data, direction, quality, economics and
     * system totals) as a "measurement_data" message. */
    // cJSON* rbgrid_json_create_measurement_data(const rbpower_frame_results_t* results,const rbgrid_json_context_t* context);

    /* ================================================================================================
     * JSON PARSING FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Add the numeric configuration ranges as a "ranges" object to the /api/v1/schema
     *        "limits" object.
     *
     * The same table drives rbgrid_config_check_ranges(). Each entry has min, max, decimals,
     * section, path and, where applicable, zero_unset (0 means "not set") and read_only.
     * @param[in,out] limits Schema "limits" object; NULL is ignored.
     */
    void rbgrid_json_add_config_ranges(cJSON* limits);

    /**
     * @brief Validate the numeric fields of a configuration POST body against the range table.
     *
     * Only fields present in @p obj are checked (an absent field keeps its current value);
     * read-only entries are skipped.
     * @param[in]  section Configuration section name ("system", "hardware", "telemetry").
     * @param[in]  obj     Section object of the request body.
     * @param[out] field   Receives the dotted path of the first invalid field; may be NULL.
     * @param[in]  fcap    Size of @p field, in bytes.
     * @param[out] msg     Receives a human-readable error for the HTTP 400 body; may be NULL.
     * @param[in]  mcap    Size of @p msg, in bytes.
     * @return true when all present fields are numbers inside their range (or @p section /
     *         @p obj is NULL); false otherwise, with @p field and @p msg filled.
     */
    bool rbgrid_config_check_ranges(const char* section, const cJSON* obj, char* field, size_t fcap, char* msg, size_t mcap);

    /**
     * @brief Parse the "system" section of the configuration into an extended configuration.
     *
     * String fields (home_name, location, language) are heap-allocated; release them with
     * rbgrid_json_free_config_strings().
     * @param[in]  system_obj cJSON object holding the system section.
     * @param[out] config     Configuration to fill.
     * @retval RBGRID_JSON_OK              Parsed.
     * @retval RBGRID_JSON_ERR_INVALID_ARG A NULL argument.
     * @retval RBGRID_JSON_ERR_NO_MEMORY   A string allocation failed.
     */
    rbgrid_json_err_t rbgrid_json_parse_system_section(
        cJSON *system_obj,
        rbpower_extended_config_t *config);

    /**
     * @brief Free the heap strings of a configuration parsed by
     *        rbgrid_json_parse_system_section().
     *
     * Frees home_name, location and language when they were allocated by the parser and leaves
     * static/constant strings untouched.
     * @param[in,out] config Configuration to clean up.
     */
    void rbgrid_json_free_config_strings(rbpower_extended_config_t *config);

    /**
     * @brief Parse the hardware section of the configuration.
     *
     * The structure contains only char arrays and primitive types, so nothing needs freeing
     * afterwards.
     * @param[in]  hw_obj cJSON object holding the hardware section.
     * @param[out] config Hardware configuration to fill.
     * @retval RBGRID_JSON_OK              Parsed (missing fields keep their values).
     * @retval RBGRID_JSON_ERR_INVALID_ARG A NULL argument.
     */
    rbgrid_json_err_t rbgrid_json_parse_hardware_section(
        cJSON *hw_obj,
        rbpower_hardware_config_t *config);

    /**
     * @brief Parse the network section of the configuration.
     * @param[in]  net_obj       cJSON object holding the network section.
     * @param[out] wifi_ssid     Buffer for the SSID (at least 32 bytes), or NULL to skip.
     * @param[out] wifi_password Buffer for the password (at least 64 bytes), or NULL to skip.
     * @param[out] ap_enabled    Receives the access-point mode flag, or NULL to skip.
     * @retval RBGRID_JSON_OK              Parsed (absent fields leave the outputs unchanged).
     * @retval RBGRID_JSON_ERR_INVALID_ARG @p net_obj is NULL.
     */
    rbgrid_json_err_t rbgrid_json_parse_network_section(
        cJSON *net_obj,
        char *wifi_ssid,
        char *wifi_password,
        bool *ap_enabled);

    /* ================================================================================================
     * HELPER FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Create a message object with the common header fields (type, version, device_id,
     *        timestamp_local in uptime s and, when NTP is enabled, timestamp_ntp).
     * @param[in] type    Message type string.
     * @param[in] context JSON module context.
     * @return New cJSON object (the caller owns it and releases it with cJSON_Delete()), or NULL
     *         when an argument is NULL or allocation fails.
     */
    cJSON *rbgrid_json_create_base_message(const char *type,
                                           const rbgrid_json_context_t *context);

    /**
     * @brief Add timestamp_local (uptime, s) and, when NTP is enabled, timestamp_ntp to an object.
     * @param[in,out] json    Object to extend.
     * @param[in]     context JSON module context.
     * @retval RBGRID_JSON_OK              Added.
     * @retval RBGRID_JSON_ERR_INVALID_ARG A NULL argument.
     */
    rbgrid_json_err_t rbgrid_json_add_timestamps(cJSON *json,
                                                 const rbgrid_json_context_t *context);

    /* Disabled: rbgrid_json_direction_to_string(); use rbpower_current_direction_to_string(). */
    // const char* rbgrid_json_direction_to_string(rbpower_current_direction_t direction);

    /**
     * @brief Convert a unit status code to a string.
     * @param[in] status Status code: 0 active, 1 inactive, 2 error, 3 calibrating.
     * @return Static string ("active", "inactive", "error", "calibrating" or "unknown").
     */
    const char *rbgrid_json_status_to_string(int status);

    /**
     * @brief Get a human-readable description of a JSON module result code.
     * @param[in] error Result code.
     * @return Static string ("Unknown error" for an unknown code).
     */
    const char *rbgrid_json_error_to_string(rbgrid_json_err_t error);

    /**
     * @brief Convert a current direction to a string.
     * @param[in] direction Current direction.
     * @return Static string representation of the direction.
     */
    const char *rbpower_current_direction_to_string(rbpower_current_direction_t direction);

    /**
     * @brief Parse a tariff plan configuration document into the global tariff plan
     *        (g_current_tariff_plan).
     * @param[in] json_str Tariff plan configuration JSON.
     * @retval RBGRID_JSON_OK                Parsed.
     * @retval RBGRID_JSON_ERR_INVALID_ARG   @p json_str is NULL.
     * @retval RBGRID_JSON_ERR_INVALID_JSON  The input is not valid JSON.
     * @retval RBGRID_JSON_ERR_MISSING_FIELD A required object is missing.
     * @retval RBGRID_JSON_ERR_TYPE_MISMATCH A field has the wrong JSON type.
     */
    rbgrid_json_err_t rbgrid_json_parse_tariff_plan_config(const char *json_str);

    /* ================================================================================================
     * DASHBOARD STRUCTURE API
     * ================================================================================================ */

    /**
     * @brief Create the home electrical network dashboard structure.
     * @return Heap JSON string, or NULL on error. The caller frees it with free().
     *
     * Creates a JSON message of type "dashboard_structure" containing:
     * - system_overview: general network information
     * - voltage_bus: voltage and network frequency block
     * - main_supply: main power supply
     * - alternative_supply: alternative power supply
     * - consumers: array of consumers
     * - system_status: system status and health
     */
    char *rbpower_create_dashboard_structure_json(void);

    /* ================================================================================================
     * FLASH HISTORY SERIALIZATION API
     * ================================================================================================ */

    /**
     * @brief Serialize a v4 ELECTRICAL history record to a JSON object.
     *
     * Keys (short names): ts (epoch s), dur (covered duration), q (quality bitmask), res (source
     * resolution class), v (V), i (A), p_act (W), p_rea (var), p_app (VA), pf, f (Hz×10 as
     * stored), thd_v and thd_i (%), and the period envelope v_min/v_max, i_min/i_max,
     * p_min/p_max. See the API reference for the exact contract.
     * @param[in] data Record to serialize.
     * @return New cJSON object (the caller owns it and releases it with cJSON_Delete() or attaches
     *         it to a parent), or NULL when @p data is NULL or allocation fails.
     */
    cJSON *rbjson_serialize_electrical(const rbstats_electrical_v4_t *data);

    /**
     * @brief Serialize a v4 ENERGY history record to a JSON object.
     *
     * Keys: ts (epoch s), dur, q, res, ec (consumed, Wh), es (supplied, Wh), en = ec - es (Wh,
     * derived, never stored) and, except on records migrated from the v3 format, the lifetime
     * totalizers at period end et_in / et_out (Wh).
     * @param[in] data Record to serialize.
     * @return New cJSON object (caller releases it with cJSON_Delete()), or NULL when @p data is
     *         NULL or allocation fails.
     */
    cJSON *rbjson_serialize_energy(const rbstats_energy_v4_t *data);

    /**
     * @brief Serialize a v4 ECONOMICS history record to a JSON object.
     *
     * Keys: ts (epoch s), dur, q, res, c (cost), r (revenue) and n = c - r (derived), all in
     * currency units converted from the stored minor units (cents ÷ 100), cur (currency code of
     * the current tariff plan), plan_id and plan_ver.
     * @param[in] data Record to serialize.
     * @return New cJSON object (caller releases it with cJSON_Delete()), or NULL when @p data is
     *         NULL or allocation fails.
     */
    cJSON *rbjson_serialize_economics(const rbstats_econ_v4_t *data);

    /* QUALITY/DIRECTION serializers removed — data-types dropped from the active pipeline. */


#ifdef __cplusplus
}
#endif

#endif // RBJSON_H