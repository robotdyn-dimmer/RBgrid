/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_config_api.h
 * @brief RBgrid configuration API: builder-style setup of units, tariffs, time and telemetry,
 *        plus read-only accessors for realtime, snapshot and history data.
 *
 * The application creates a configuration handle with rbgrid_config_create(), adds metering
 * sources (rbAmp I2C modules, ESP-NOW nodes, persisted units), assigns the special units
 * (voltage bus, main supply, alternative supply), optionally builds a tariff plan with the
 * tariff builder, and then hands the handle to the core for initialisation. The functions in
 * the "public data access" sections read the live home network after start-up.
 *
 * Measurement comes only from rbAmp I2C modules and ESP-NOW nodes. ADC-typed unit definitions
 * (rbgrid_config_add_adc_sensor() and the ADC sensor model codes) remain as the legacy binding
 * of the default units that the rbAmp bridge feeds; no on-chip ADC is sampled.
 *
 * @par Thread safety
 * The builder functions (everything taking a ::rbgrid_config_handle_t or a
 * ::rbgrid_tariff_builder_t) are not thread-safe; call them from one task during boot.
 * Functions that modify the live network at runtime (bind/rebind/rename, point tombstones,
 * rbgrid_config_set_source_invert()) take no lock of their own and are meant to be called
 * from the HTTP server task.
 */

#ifndef RBGRID_CONFIG_API_H
#define RBGRID_CONFIG_API_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "rbpower.h"
#include "rbpower_enums.h"
#include "rbgrid.h"
#include "rbtelemetry.h"
#include "../src/rbhome_main_supply.h"
#include "../src/rbhome_alt_supply.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // ========== TYPES AND CONSTANTS ==========

    /**
     * @brief Opaque configuration handle (builder context) returned by rbgrid_config_create().
     */
    typedef struct rbgrid_config_context_s *rbgrid_config_handle_t;

/**
 * @brief Maximum number of sensors accepted by the configuration builder.
 */
#define RBGRID_CONFIG_MAX_SENSORS 16

/**
 * @brief Sentinel value for "no sensor" / invalid sensor ID.
 */
#define RBGRID_SENSOR_INVALID_ID -1

    /**
     * @brief Voltage sensor models (legacy ADC-typed model codes).
     */
    typedef enum
    {
        RBGRID_SENSOR_VOLTAGE_ZMPT101B,  ///< ZMPT101B voltage transformer module.
        RBGRID_SENSOR_VOLTAGE_ZMPT107_1, ///< ZMPT107-1 voltage transformer module.
        RBGRID_SENSOR_VOLTAGE_ZMPT116B,  ///< ZMPT116B voltage transformer module.
        RBGRID_SENSOR_VOLTAGE_CUSTOM     ///< User-defined voltage sensor.
    } rbgrid_voltage_sensor_model_t;

    /**
     * @brief Current sensor models (legacy ADC-typed model codes).
     */
    typedef enum
    {
        RBGRID_SENSOR_CURRENT_SCT013_10A,  ///< SCT-013 split-core CT, 10 A rated.
        RBGRID_SENSOR_CURRENT_SCT013_20A,  ///< SCT-013 split-core CT, 20 A rated.
        RBGRID_SENSOR_CURRENT_SCT013_30A,  ///< SCT-013 split-core CT, 30 A rated.
        RBGRID_SENSOR_CURRENT_SCT013_50A,  ///< SCT-013 split-core CT, 50 A rated.
        RBGRID_SENSOR_CURRENT_SCT013_100A, ///< SCT-013 split-core CT, 100 A rated.
        RBGRID_SENSOR_CURRENT_ACS712_5A,   ///< ACS712 Hall-effect sensor, 5 A.
        RBGRID_SENSOR_CURRENT_ACS712_20A,  ///< ACS712 Hall-effect sensor, 20 A.
        RBGRID_SENSOR_CURRENT_ACS712_30A,  ///< ACS712 Hall-effect sensor, 30 A.
        RBGRID_SENSOR_CURRENT_CUSTOM       ///< User-defined current sensor.
    } rbgrid_current_sensor_model_t;

    /**
     * @brief General sensor model code, grouped by interface in blocks of 100.
     *
     * Used by the sensor-type helper rbgrid_get_sensor_type_from_model(). The ADC-typed
     * codes are kept for the legacy unit binding only.
     */
    typedef enum
    {
        // Voltage sensors (ADC)
        RBGRID_SENSOR_ZMPT101B = 0, ///< ZMPT101B voltage sensor (legacy ADC-typed).
        RBGRID_SENSOR_ZMPT107_1,    ///< ZMPT107-1 voltage sensor (legacy ADC-typed).
        RBGRID_SENSOR_ZMPT116B,     ///< ZMPT116B voltage sensor (legacy ADC-typed).

        // Current sensors (ADC)
        RBGRID_SENSOR_SCT013_10A = 100, ///< SCT-013 10 A CT (legacy ADC-typed).
        RBGRID_SENSOR_SCT013_20A,       ///< SCT-013 20 A CT (legacy ADC-typed).
        RBGRID_SENSOR_SCT013_30A,       ///< SCT-013 30 A CT (legacy ADC-typed).
        RBGRID_SENSOR_SCT013_50A,       ///< SCT-013 50 A CT (legacy ADC-typed).
        RBGRID_SENSOR_SCT013_100A,      ///< SCT-013 100 A CT (legacy ADC-typed).
        RBGRID_SENSOR_ACS712_5A,        ///< ACS712 5 A Hall sensor (legacy ADC-typed).
        RBGRID_SENSOR_ACS712_20A,       ///< ACS712 20 A Hall sensor (legacy ADC-typed).
        RBGRID_SENSOR_ACS712_30A,       ///< ACS712 30 A Hall sensor (legacy ADC-typed).

        // I2C sensors
        RBGRID_SENSOR_INA226 = 200, ///< INA226 I2C power monitor (model code only, no driver).
        RBGRID_SENSOR_INA219,       ///< INA219 I2C power monitor (model code only, no driver).
        RBGRID_SENSOR_ADS1115,      ///< ADS1115 I2C ADC (model code only, no driver).

        // SPI sensors
        RBGRID_SENSOR_MCP3208 = 300, ///< MCP3208 SPI ADC (model code only, no driver).
        RBGRID_SENSOR_ADE7953,       ///< ADE7953 SPI metering IC (model code only, no driver).
        RBGRID_SENSOR_ATM90E32,      ///< ATM90E32 SPI metering IC (model code only, no driver).

        // ESP-NOW sensors
        RBGRID_SENSOR_ESPNOW_VOLTAGE = 400, ///< Remote voltage measurement over ESP-NOW.
        RBGRID_SENSOR_ESPNOW_CURRENT,       ///< Remote current measurement over ESP-NOW.
        RBGRID_SENSOR_ESPNOW_POWER,         ///< Remote power measurement over ESP-NOW.

        // Custom
        RBGRID_SENSOR_CUSTOM = 999 ///< User-defined sensor.
    } rbgrid_sensor_model_t;

    /**
     * @brief Error codes returned by the configuration API (`int` / ::rbgrid_config_error_t).
     *
     * The HTTP layer maps POINT_TOMBSTONED and POINT_TAKEN to HTTP 409.
     */
    typedef enum
    {
        RBGRID_CONFIG_OK = 0,                      ///< Success.
        RBGRID_CONFIG_ERR_NO_MEMORY = -1,          ///< Allocation failed or the unit table is full.
        RBGRID_CONFIG_ERR_INVALID_PARAM = -2,      ///< NULL handle, bad argument or object not found.
        RBGRID_CONFIG_ERR_SENSOR_LIMIT = -3,       ///< Sensor/unit/zone/fee table limit reached.
        RBGRID_CONFIG_ERR_INVALID_GPIO = -4,       ///< GPIO number out of range.
        RBGRID_CONFIG_ERR_GPIO_NOT_ADC = -5,       ///< GPIO has no ADC1 channel (legacy ADC binding).
        RBGRID_CONFIG_ERR_DUPLICATE_SENSOR = -6,   ///< Sensor/source already bound to another unit.
        RBGRID_CONFIG_ERR_NOT_INITIALIZED = -7,    ///< Handle or builder not initialised.
        RBGRID_CONFIG_ERR_ALREADY_SET = -8,        ///< Singleton setting (e.g. main supply) already configured.
        /* Point (slug) identity */
        RBGRID_CONFIG_ERR_POINT_TOMBSTONED = -9,   ///< Slug is retired; pass reuse=true to resurrect it (HTTP 409).
        RBGRID_CONFIG_ERR_POINT_TAKEN = -10        ///< Slug is in use by a live unit (HTTP 409).
    } rbgrid_config_error_t;

    /**
     * @brief External flash memory status, as returned by rbgrid_get_flash_status().
     */
    typedef struct
    {
        bool external_flash_available; ///< External flash chip detected and usable.
        bool otp_validated;            ///< OTP validation passed.
        uint32_t device_serial;        ///< Device serial number read from OTP (0 if unavailable).
        bool config_partition_mounted; ///< Configuration partition is mounted.
        size_t config_partition_size;  ///< Configuration partition size, bytes.
        size_t config_used_space;      ///< Used space in the configuration partition, bytes.
        const char *error_message;     ///< Static error description, or NULL; owned by the library, do not free.
    } rbgrid_flash_status_t;

    // ========== MAIN CONFIGURATION FUNCTIONS ==========

    /**
     * @brief Create a new configuration handle.
     *
     * @param[in] home_name Home/facility name.
     * @param[in] location  Location text.
     * @return Configuration handle, or NULL on allocation failure. Release it with
     *         rbgrid_config_destroy() unless it was consumed by the core initialisation.
     */
    rbgrid_config_handle_t rbgrid_config_create(const char *home_name, const char *location);

    /**
     * @brief Release a configuration handle and its buffers.
     *
     * Call only when initialisation failed; after a successful initialisation the memory is
     * freed automatically. NULL is ignored.
     *
     * @param[in] config Configuration handle.
     */
    void rbgrid_config_destroy(rbgrid_config_handle_t config);

    /**
     * @brief Configure the electrical network parameters.
     *
     * Applies the nominal defaults (50 Hz / 230 V); the hardware-config POST endpoint can
     * override them later.
     *
     * @param[in] config Configuration handle.
     * @retval RBGRID_CONFIG_OK                 Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM  @p config is NULL.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_ALREADY_SET    Network already configured.
     */
    int rbgrid_config_set_network(rbgrid_config_handle_t config);

    // ========== SENSOR CONFIGURATION FUNCTIONS ==========

    /**
     * @brief Add an ADC-typed sensor binding (legacy unit definition).
     *
     * Converts the GPIO to an ADC1 channel number and fills the channel from the sensor
     * profile. No ADC hardware is sampled: the binding only defines the unit (for example the
     * default main supply 0x0006 or voltage bus 0x0007) that the rbAmp bridge feeds through the
     * injection API.
     *
     * @param[in] config      Configuration handle.
     * @param[in] gpio_pin    GPIO pin; must map to an ADC1 channel.
     * @param[in] sensor_type Sensor type from rbpower_enums.h (e.g. RBSENSOR_TYPE_VOLTAGE_ZMPT107_1).
     * @return The new unit UID, or RBGRID_INVALID_UNIT_UID on failure (NULL/uninitialised
     *         handle, invalid or non-ADC1 GPIO, channel limit reached, channel already in use).
     */
    rbgrid_unit_uid_t rbgrid_config_add_adc_sensor(
        rbgrid_config_handle_t config,
        gpio_num_t gpio_pin,
        rbsensor_type_t sensor_type);

    // ========== rbAmp MODULE (I2C external source) ==========

    /**
     * @brief One channel of an rbAmp module: channel index, CT/clamp type and display name.
     */
    typedef struct
    {
        uint8_t channel;             ///< rbAmp current channel index (0..2); for ESP-NOW nodes the flat channel ID (module*4 + ch).
        rbsensor_type_t sensor_type; ///< SCT-013 model; selects the CT code and rated current.
        const char *name;            ///< Unit display name (borrowed; copied by the callee).
    } rbgrid_rbamp_channel_t;

    /**
     * @brief Register an rbAmp I2C metering module as an external source.
     *
     * Creates one RBgrid unit per current channel with source_type = I2C_SENSOR and an
     * I2C unit UID (RBGRID_MAKE_I2C_UNIT_ID(0, module_index, channel)). The rbAmp bridge then
     * probes the module, provisions each channel's CT model from @c sensor_type
     * (verify-then-set) and injects readings into these units. There is no host calibration:
     * rbAmp returns calibrated physical values (per-unit gains live in module flash).
     *
     * @param[in] config        Configuration handle.
     * @param[in] i2c_address   Module I2C address (0x50/0x51/0x52); informational, the UID uses @p module_index.
     * @param[in] module_index  Module index 0..2, encoded into the unit UID and the bridge fleet config.
     * @param[in] channels      Array of @p channel_count per-channel CT mappings.
     * @param[in] channel_count Number of entries in @p channels.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle/array or out-of-range index/count.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY       Unit table full.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR A unit with the same UID already exists.
     */
    int rbgrid_config_add_rbamp_module(
        rbgrid_config_handle_t config,
        uint8_t i2c_address,
        uint8_t module_index,
        const rbgrid_rbamp_channel_t *channels,
        uint8_t channel_count);

    /**
     * @brief Register the channels of a remote (ESP-NOW) rbAmp node as standard units.
     *
     * Mirrors rbgrid_config_add_rbamp_module(), but with source_type = ESP_NOW and the unit ID
     * derived from (node MAC, channel) via rbgrid_create_espnow_unit_id(). Registering at
     * configuration time gives the units the full standard treatment at initialisation (flash
     * buffers, source mappings, accumulators), so history and billing work uniformly. The
     * ESP-NOW hub feeds them by the same unit ID.
     *
     * @param[in] config        Configuration handle.
     * @param[in] node_mac      The remote node's 6-byte MAC address.
     * @param[in] channels      Per-channel CT type and name (channel = the node's flat channel ID, module*4 + ch).
     * @param[in] channel_count Number of entries in @p channels.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or invalid count.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY       Unit table full.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR A unit with the same UID already exists.
     */
    int rbgrid_config_add_espnow_module(
        rbgrid_config_handle_t config,
        const uint8_t node_mac[6],
        const rbgrid_rbamp_channel_t *channels,
        uint8_t channel_count);

    /**
     * @brief Runtime bind request: create a unit for an unbound sensor.
     *
     * The sensor is identified by its addressing exactly as `GET /api/v1/sensors` reports it
     * (i2c: bus/module/ch; adc: channel). @c role and @c stat_class are already resolved to
     * enums by the parser, with defaults applied. Filled by rbgrid_json_parse_create_unit().
     */
    typedef struct
    {
        char     iface[8];   ///< Interface: "i2c" or "adc" (legacy ADC-typed binding).
        uint8_t  bus;        ///< I2C bus index (0).
        uint8_t  module;     ///< I2C module index.
        uint8_t  ch;         ///< I2C channel index.
        uint8_t  channel;    ///< ADC channel (legacy ADC-typed binding).
        char     name[32];   ///< Unit display name (required).
        char     location[64];                  ///< Optional location ("" if absent).
        rbpower_load_role_t        role;        ///< Resolved load role (default RBPOWER_ROLE_DEVICE_LOAD).
        rbpower_stat_period_type_t stat_class;  ///< Resolved statistics depth (default RBPOWER_STAT_PERIOD_1HOUR).
        char     point[33];  ///< Explicit point slug ("" = generate from @c name).
        bool     reuse;      ///< Resurrect a tombstoned slug instead of failing with HTTP 409.
    } rbgrid_create_unit_req_t;

    /**
     * @brief Build a unit definition from a bind request and register it in the live network.
     *
     * Calls rbpower_home_create_unit(), which fires the source bind hook (the rbAmp bridge sets
     * the fleet entry and persists /rbamp.json). For i2c the CT model is recovered from the
     * persisted fleet entry's ct_code. Operates on RAM/flash state, not on a config handle.
     *
     * @param[in]  req     Parsed and validated bind request.
     * @param[out] out_uid Assigned unit ID (derived from the addressing) on success.
     * @retval RBGRID_CONFIG_OK                   Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM    Bad iface/addressing, sensor not in the fleet, or registration failed.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR Sensor already bound.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY        Network full.
     * @retval RBGRID_CONFIG_ERR_POINT_TAKEN      Requested slug is used by a live unit.
     * @retval RBGRID_CONFIG_ERR_POINT_TOMBSTONED Requested slug is retired and @c reuse is false.
     */
    int rbgrid_config_bind_unit(const rbgrid_create_unit_req_t *req, rbpower_unit_id_t *out_uid);

    /* ---- Point tombstones: retired slugs, persisted in units.json ---- */

    /**
     * @brief Check whether a point slug is retired (tombstoned).
     * @param[in] slug Point slug; NULL or "" returns false.
     * @return true if @p slug is in the tombstone list.
     */
    bool rbgrid_point_tombstoned(const char *slug);

    /**
     * @brief Add a slug to the tombstone list.
     *
     * When the list is full (RBPOINT_TOMBSTONE_MAX entries) the oldest entry is evicted.
     * The list is persisted by the next units.json save.
     *
     * @param[in] slug Point slug to retire.
     */
    void rbgrid_point_tombstone_add(const char *slug);

    /**
     * @brief Remove a slug from the tombstone list (resurrect it).
     * @param[in] slug Point slug.
     * @return true if the slug was found and removed, false otherwise.
     */
    bool rbgrid_point_tombstone_remove(const char *slug);

    /**
     * @brief Copy the tombstone list into a caller buffer.
     * @param[out] out Array of at least @p max slug buffers of 33 chars (RBPOINT_SLUG_MAX + 1).
     * @param[in]  max Capacity of @p out, entries.
     * @return Number of slugs copied (<= @p max).
     */
    int  rbgrid_point_tombstones_get(char out[][33], int max);

    /**
     * @brief Move a live I2C unit onto another fleet sensor, keeping its unit ID, point,
     *        history and flash slot.
     *
     * Validates the target (present in the fleet, not occupied), recovers the CT profile,
     * then rbpower_home_rebind_unit_source() rebinds the source hooks. Rebinding to the
     * current sensor is a no-op that succeeds.
     *
     * @param[in] unit_id Unit to move.
     * @param[in] module  Target rbAmp module index.
     * @param[in] ch      Target channel index.
     * @retval RBGRID_CONFIG_OK                   Success (or no-op).
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM    Unit or sensor not found, unit is not a live I2C unit, or rebind failed.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR Target sensor occupied by another unit.
     */
    int rbgrid_config_rebind_unit(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch);

    /**
     * @brief Change a live unit's point slug.
     *
     * The old slug goes to the tombstone list and a POINT_RENAMED event is logged.
     * Renaming to the current slug is a no-op that succeeds.
     *
     * @param[in] unit_id   Unit to rename.
     * @param[in] new_point New slug; must pass rbpoint_slug_valid().
     * @param[in] reuse     Allow taking a tombstoned slug.
     * @retval RBGRID_CONFIG_OK                   Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM    Unit not found or invalid slug.
     * @retval RBGRID_CONFIG_ERR_POINT_TAKEN      Slug used by another live unit.
     * @retval RBGRID_CONFIG_ERR_POINT_TOMBSTONED Slug is retired and @p reuse is false.
     */
    int rbgrid_config_rename_point(rbpower_unit_id_t unit_id, const char *new_point, bool reuse);

    /**
     * @brief Append a fully formed persisted unit definition (from units.json) to the builder.
     *
     * Works like rbgrid_config_add_rbamp_module() for fleet-derived units, but keeps the
     * stored identity (unit ID, point, device_uid/channel_idx) instead of recomputing it.
     * The unit is registered in the live network later during core initialisation.
     *
     * @param[in] config Configuration handle.
     * @param[in] def    Unit definition to copy.
     * @retval RBGRID_CONFIG_OK                   Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM    NULL argument.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED  Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY        Unit table full or allocation failed.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR A unit with the same ID is already queued.
     */
    int rbgrid_config_add_persisted_unit(rbgrid_config_handle_t config,
                                         const rbpower_unit_definition_t *def);

    /**
     * @brief Boot-path occupancy probe: is a unit with this primary source already queued
     *        in the builder?
     *
     * The application uses it to skip default channels that were already restored from
     * units.json.
     *
     * @param[in] config      Configuration handle.
     * @param[in] source_type Source type of the primary source.
     * @param[in] device_uid  Source device UID.
     * @param[in] channel_idx Source channel index.
     * @return true if a matching unit is queued; false otherwise or on a NULL handle.
     */
    bool rbgrid_config_builder_has_source(rbgrid_config_handle_t config,
                                          rbpower_source_type_t source_type,
                                          uint64_t device_uid, uint8_t channel_idx);

    // ========== ADDITIONAL SENSOR CONFIGURATION ==========

    /**
     * @brief Filter types for sensors.
     */
    typedef enum
    {
        RBGRID_FILTER_NONE = 0,   ///< No filtering.
        RBGRID_FILTER_MOVING_AVG, ///< Moving average.
        RBGRID_FILTER_MEDIAN,     ///< Median filter.
        RBGRID_FILTER_KALMAN      ///< Kalman filter.
    } rbgrid_filter_type_t;

    // ========== HELPER FUNCTIONS ==========

    /**
     * @brief Measured quantity class of a sensor model.
     */
    typedef enum
    {
        RBGRID_SENSOR_TYPE_VOLTAGE,     ///< Voltage sensor.
        RBGRID_SENSOR_TYPE_CURRENT,     ///< Current sensor.
        RBGRID_SENSOR_TYPE_TEMPERATURE, ///< Temperature sensor.
        RBGRID_SENSOR_TYPE_POWER,       ///< Power sensor.
        RBGRID_SENSOR_TYPE_UNKNOWN      ///< Unknown model.
    } rbgrid_sensor_type_t;

    // ========== UNIT CONFIGURATION FUNCTIONS ==========

    /**
     * @brief Set the voltage bus unit (required for voltage measurement).
     *
     * @param[in] config    Configuration handle.
     * @param[in] sensor_id Voltage sensor ID / unit UID.
     * @param[in] name      Unit name.
     * @retval RBGRID_CONFIG_OK                   Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM    NULL handle or invalid sensor.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED  Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_ALREADY_SET      Voltage bus already configured.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR Sensor already used by another unit.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY        Unit table full.
     */
    int rbgrid_config_set_voltage_bus(
        rbgrid_config_handle_t config,
        int sensor_id,
        const char *name);

    /**
     * @brief Set the main supply unit (primary grid input).
     *
     * @param[in] config      Configuration handle.
     * @param[in] sensor_id   Current sensor ID / unit UID.
     * @param[in] name        Unit name (NULL for "Main Supply").
     * @param[in] stat_period Deepest statistics period kept for this unit.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or invalid sensor.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_ALREADY_SET     Main supply already configured.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY       Unit table full or allocation failed.
     */
    int rbgrid_config_set_main_supply(
        rbgrid_config_handle_t config,
        int sensor_id,
        const char *name,
        rbpower_stat_period_type_t stat_period);

    /**
     * @brief Set the alternative supply unit (solar, generator, battery inverter).
     *
     * @param[in] config      Configuration handle.
     * @param[in] unit_uid    Current sensor unit UID.
     * @param[in] name        Unit name (NULL for "Alternative Supply").
     * @param[in] stat_period Deepest statistics period kept for this unit.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or invalid unit UID.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     * @retval RBGRID_CONFIG_ERR_ALREADY_SET     Alternative supply already configured.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY       Unit table full or allocation failed.
     */
    int rbgrid_config_set_alternative_supply(
        rbgrid_config_handle_t config,
        rbgrid_unit_uid_t unit_uid,
        const char *name,
        rbpower_stat_period_type_t stat_period);

    /**
     * @brief Add a regular unit backed by a sensor.
     *
     * The maximum current is derived from the sensor model.
     *
     * @param[in] config          Configuration handle.
     * @param[in] sensor_unit_uid Sensor unit UID (returned by the sensor add function).
     * @param[in] name            Unit name.
     * @param[in] stat_period     Deepest statistics period kept for this unit.
     * @retval RBGRID_CONFIG_OK                   Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM    NULL handle or invalid UID.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT     Unit limit reached.
     * @retval RBGRID_CONFIG_ERR_DUPLICATE_SENSOR Unit UID already in use.
     * @retval RBGRID_CONFIG_ERR_NO_MEMORY        Allocation failed.
     */
    int rbgrid_config_add_unit(
        rbgrid_config_handle_t config,
        int sensor_unit_uid,
        const char *name,
        rbpower_stat_period_type_t stat_period);

    /**
     * @brief Set the per-install CT orientation of a unit (sources[0].invert_sign).
     *
     * Updates both the configuration definition and the live unit, if instantiated. The sign
     * is applied at the injection boundaries.
     *
     * @param[in] config   Configuration handle.
     * @param[in] unit_uid Unit UID (> 0).
     * @param[in] invert   true to invert the measured power sign.
     * @retval RBGRID_CONFIG_OK                Success (at least one copy updated).
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM NULL handle, invalid UID, or unit not found.
     */
    int rbgrid_config_set_source_invert(rbgrid_config_handle_t config, int unit_uid, bool invert);

    /**
     * @brief Get the builder's copy of a unit definition (the boot-time source of the home network).
     *
     * @param[in] config   Configuration handle.
     * @param[in] unit_uid Unit UID (> 0).
     * @return Mutable pointer into the builder's unit array (owned by the handle, valid until it
     *         is destroyed or consumed), or NULL if absent.
     */
    rbpower_unit_definition_t* rbgrid_config_unit_def_mutable(rbgrid_config_handle_t config, int unit_uid);

    // ========== TARIFF BUILDER ==========

    /**
     * @brief Opaque tariff builder handle.
     */
    typedef struct rbgrid_tariff_builder_s *rbgrid_tariff_builder_t;

/**
 * @brief Maximum number of tariff zones in the builder.
 */
#define RBGRID_TARIFF_BUILDER_MAX_ZONES 16

/**
 * @brief Maximum number of holidays in the builder.
 */
#define RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS 32

/**
 * @brief Maximum number of fixed fees in the builder.
 */
#define RBGRID_TARIFF_BUILDER_MAX_FIXED_FEES 8

    /**
     * @brief Create a tariff builder with basic parameters.
     *
     * @param[in] currency Currency code ("EUR", "USD", ...).
     * @param[in] region   Region code ("DE", "US", ...).
     * @return Builder handle (release with rbgrid_tariff_builder_destroy()), or NULL on error.
     */
    rbgrid_tariff_builder_t rbgrid_tariff_builder_create(const char *currency, const char *region);

    /**
     * @brief Create an empty tariff builder.
     *
     * @return Builder handle (release with rbgrid_tariff_builder_destroy()), or NULL on error.
     */
    rbgrid_tariff_builder_t rbgrid_tariff_builder_create_empty(void);

    /**
     * @brief Set regional settings.
     *
     * @param[in] builder  Builder handle.
     * @param[in] currency Currency code ("EUR", "USD", ...).
     * @param[in] region   Region code ("DE", "US", ...).
     * @param[in] timezone Time zone ("Europe/Berlin", "UTC+3"); may be NULL.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL builder or argument.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_region_settings(
        rbgrid_tariff_builder_t builder,
        const char *currency,
        const char *region,
        const char *timezone);

    /**
     * @brief Add a time-of-day tariff zone.
     *
     * @param[in] builder      Builder handle.
     * @param[in] zone_name    Zone name ("Night", "Day", "Peak").
     * @param[in] rate_per_kwh Rate, currency per kWh.
     * @param[in] time_range   Time range ("23:00-07:00" or "23:00-07:00+1").
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or unparsable range.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT    RBGRID_TARIFF_BUILDER_MAX_ZONES reached.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_add_time_zone(
        rbgrid_tariff_builder_t builder,
        const char *zone_name,
        float rate_per_kwh,
        const char *time_range);

    /**
     * @brief Add a weekday-based tariff zone.
     *
     * @param[in] builder      Builder handle.
     * @param[in] zone_name    Zone name ("Weekend", "Workday").
     * @param[in] rate_per_kwh Rate, currency per kWh.
     * @param[in] weekday_mask Day bitmask (0x01 = Mon, 0x02 = Tue, ..., 0x40 = Sun).
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or empty mask.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT    RBGRID_TARIFF_BUILDER_MAX_ZONES reached.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_add_weekday_zone(
        rbgrid_tariff_builder_t builder,
        const char *zone_name,
        float rate_per_kwh,
        uint8_t weekday_mask);

    /**
     * @brief Add a seasonal tariff zone.
     *
     * @param[in] builder      Builder handle.
     * @param[in] zone_name    Zone name ("Winter", "Summer").
     * @param[in] rate_per_kwh Rate, currency per kWh.
     * @param[in] start_month  First month of the season (1..12).
     * @param[in] end_month    Last month of the season (1..12).
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or month out of range.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT    RBGRID_TARIFF_BUILDER_MAX_ZONES reached.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_add_seasonal_zone(
        rbgrid_tariff_builder_t builder,
        const char *zone_name,
        float rate_per_kwh,
        uint8_t start_month,
        uint8_t end_month);

    /**
     * @brief Add a list of holidays sharing one rate.
     *
     * @param[in] builder              Builder handle.
     * @param[in] holiday_days         Array of day-of-year numbers (1..366).
     * @param[in] holidays_count       Number of entries in @p holiday_days.
     * @param[in] holiday_rate_per_kwh Holiday rate, currency per kWh.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or day out of range.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT    RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS reached.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_add_holiday_list(
        rbgrid_tariff_builder_t builder,
        const uint16_t *holiday_days,
        uint8_t holidays_count,
        float holiday_rate_per_kwh);

    /**
     * @brief Add a single holiday.
     *
     * @param[in] builder      Builder handle.
     * @param[in] day_of_year  Day of year (1..366).
     * @param[in] holiday_name Holiday name (may be NULL).
     * @param[in] rate_per_kwh Rate for this day, currency per kWh.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL builder or day out of range.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT    RBGRID_TARIFF_BUILDER_MAX_HOLIDAYS reached.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_add_single_holiday(
        rbgrid_tariff_builder_t builder,
        uint16_t day_of_year,
        const char *holiday_name,
        float rate_per_kwh);

    /**
     * @brief Add the preset public holidays of a country.
     *
     * @param[in] builder              Builder handle.
     * @param[in] country_code         Country code ("DE", "US", ...).
     * @param[in] holiday_rate_per_kwh Rate applied to all holidays, currency per kWh.
     * @return RBGRID_CONFIG_ERR_INVALID_PARAM for a NULL argument or unknown country,
     *         otherwise the result of rbgrid_tariff_builder_add_holiday_list().
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_country_holidays(
        rbgrid_tariff_builder_t builder,
        const char *country_code,
        float holiday_rate_per_kwh);

    /**
     * @brief Set the monthly subscription fee (added as a "Monthly Fee" fixed fee).
     *
     * @param[in] builder        Builder handle.
     * @param[in] monthly_amount Monthly fee, currency units.
     * @return RBGRID_CONFIG_ERR_INVALID_PARAM for a NULL builder or negative amount,
     *         otherwise the result of rbgrid_tariff_builder_add_custom_fee().
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_monthly_fee(
        rbgrid_tariff_builder_t builder,
        float monthly_amount);

    /**
     * @brief Set the installed-capacity fee (added as a "Power Fee" fixed fee).
     *
     * @param[in] builder           Builder handle.
     * @param[in] rate_per_kw_month Fee, currency per kW per month.
     * @return RBGRID_CONFIG_ERR_INVALID_PARAM for a NULL builder or negative rate,
     *         otherwise the result of rbgrid_tariff_builder_add_custom_fee().
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_power_fee(
        rbgrid_tariff_builder_t builder,
        float rate_per_kw_month);

    /**
     * @brief Add a custom fixed fee.
     *
     * @param[in] builder  Builder handle.
     * @param[in] fee_name Fee name ("Grid fee", "Tax", "Green levy").
     * @param[in] amount   Fee amount, currency units (per kW for power-based fees).
     * @param[in] fee_type Fee type (MONTHLY, DAILY, POWER_BASED).
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     * @retval RBGRID_CONFIG_ERR_SENSOR_LIMIT    RBGRID_TARIFF_BUILDER_MAX_FIXED_FEES reached.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_add_custom_fee(
        rbgrid_tariff_builder_t builder,
        const char *fee_name,
        float amount,
        rbpower_tariff_fixed_type_t fee_type);

    /**
     * @brief Configure net metering (selling exported energy).
     *
     * @param[in] builder              Builder handle.
     * @param[in] selling_rate_per_kwh Selling rate, currency per kWh.
     * @param[in] net_ratio            Net ratio coefficient (0.0..1.0, typically 0.7).
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL builder or value out of range.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_net_metering(
        rbgrid_tariff_builder_t builder,
        float selling_rate_per_kwh,
        float net_ratio);

    /**
     * @brief Set the selling rate of an existing zone.
     *
     * @param[in] builder              Builder handle.
     * @param[in] zone_name            Zone name.
     * @param[in] selling_rate_per_kwh Selling rate for this zone, currency per kWh.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or zone not found.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_zone_selling_rate(
        rbgrid_tariff_builder_t builder,
        const char *zone_name,
        float selling_rate_per_kwh);

    /**
     * @brief Set progressive (tiered) pricing.
     *
     * @param[in] builder             Builder handle.
     * @param[in] tier_thresholds_kwh Array of tier thresholds, kWh.
     * @param[in] tier_rates_per_kwh  Array of tier rates, currency per kWh.
     * @param[in] tiers_count         Number of entries in both arrays.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or invalid count.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_set_progressive_tiers(
        rbgrid_tariff_builder_t builder,
        const float *tier_thresholds_kwh,
        const float *tier_rates_per_kwh,
        uint8_t tiers_count);

    /**
     * @brief Validate a builder before applying it.
     *
     * @param[in] builder Builder handle.
     * @retval RBGRID_CONFIG_OK                  Builder is valid.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL builder or inconsistent plan.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Builder not initialised.
     */
    rbgrid_config_error_t rbgrid_tariff_builder_validate(rbgrid_tariff_builder_t builder);

    /**
     * @brief Apply the tariff plan from a builder to a configuration.
     *
     * The builder is copied; the caller still owns it and releases it with
     * rbgrid_tariff_builder_destroy().
     *
     * @param[in] config  Configuration handle.
     * @param[in] builder Builder handle.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL argument or validation failed.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle or builder not initialised.
     */
    rbgrid_config_error_t rbgrid_config_apply_tariff_plan(
        rbgrid_config_handle_t config,
        rbgrid_tariff_builder_t builder);

    /**
     * @brief Release builder memory.
     *
     * @param[in] builder Builder handle (NULL is ignored).
     */
    void rbgrid_tariff_builder_destroy(rbgrid_tariff_builder_t builder);

    // ========== MAIN INITIALIZATION FUNCTIONS ==========

    /**
     * @brief Configure time synchronisation via SNTP.
     *
     * @param[in] config         Configuration handle.
     * @param[in] primary_server Primary SNTP server (e.g. "pool.ntp.org").
     * @param[in] backup_server  Backup SNTP server (e.g. "time.nist.gov").
     * @param[in] timezone       Time zone (e.g. "UTC+3" or "Europe/Berlin").
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or server.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_sntp_time(
        rbgrid_config_handle_t config,
        const char *primary_server,
        const char *backup_server,
        const char *timezone);

    /**
     * @brief Configure the time fallback used when SNTP is unavailable.
     *
     * @param[in] config          Configuration handle.
     * @param[in] enable_rtc      Use the RTC as fallback.
     * @param[in] enable_manual   Allow a manual time setting as fallback.
     * @param[in] manual_time_sec Manual time, Unix epoch seconds (0 = current time).
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_time_fallback(
        rbgrid_config_handle_t config,
        bool enable_rtc,
        bool enable_manual,
        uint64_t manual_time_sec);

    /**
     * @brief Advanced time synchronisation settings.
     *
     * @param[in] config            Configuration handle.
     * @param[in] sync_interval_sec Resynchronisation interval, s (default 3600).
     * @param[in] timeout_ms        SNTP request timeout, ms (default 10000).
     * @param[in] retry_count       Number of synchronisation attempts (default 3).
     * @param[in] enable_dst        Enable daylight saving time.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or value out of range.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_time_advanced(
        rbgrid_config_handle_t config,
        uint32_t sync_interval_sec,
        uint32_t timeout_ms,
        uint8_t retry_count,
        bool enable_dst);

    // ========== PUBLIC DATA ACCESS FUNCTIONS ==========

    /**
     * @brief Get the voltage statistics of the last completed 3-second period.
     *
     * Copies the final voltage statistics (RMS voltage, average frequency, THD) of the
     * voltage bus. Reads shared state without a lock; values are consistent per field.
     *
     * @param[out] voltage_stats Destination structure.
     * @retval RBGRID_OK                  Success.
     * @retval RBGRID_ERR_INVALID_ARG     @p voltage_stats is NULL.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND  No voltage bus configured.
     */
    rbgrid_err_t rbgrid_realtime_get_voltage(statistics_3sec_final_t *voltage_stats);

    /**
     * @brief Get the current main supply state from the 200 ms realtime structure.
     *
     * Every output pointer may be NULL to skip that value. Reads shared state without a lock.
     *
     * @param[out] current_rms      RMS current, A.
     * @param[out] power_active     Active power, W.
     * @param[out] direction        Current direction (import/export).
     * @param[out] power_reactive   Reactive power, var.
     * @param[out] power_apparent   Apparent power, VA.
     * @param[out] power_factor     Power factor (-1..1).
     * @param[out] samples_count    Number of samples in the 200 ms window.
     * @param[out] energy_increment Energy increment over the 200 ms window, Wh.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND No main supply configured.
     */
    rbgrid_err_t rbgrid_realtime_get_main_supply(float *current_rms,
                                                 float *power_active,
                                                 rbpower_current_direction_t *direction,
                                                 float *power_reactive,
                                                 float *power_apparent,
                                                 float *power_factor,
                                                 uint32_t *samples_count,
                                                 float *energy_increment);

    /**
     * @brief Get the current alternative supply state from the 200 ms realtime structure.
     *
     * Every output pointer may be NULL to skip that value. Reads shared state without a lock.
     *
     * @param[out] current_rms      RMS current, A.
     * @param[out] power_active     Active power, W.
     * @param[out] direction        Current direction (import/export).
     * @param[out] power_reactive   Reactive power, var.
     * @param[out] power_apparent   Apparent power, VA.
     * @param[out] power_factor     Power factor (-1..1).
     * @param[out] samples_count    Number of samples in the 200 ms window.
     * @param[out] energy_increment Energy increment over the 200 ms window, Wh.
     * @param[out] efficiency       Conversion efficiency, %.
     * @param[out] dc_power         DC-side power, W.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND No alternative supply configured.
     */
    rbgrid_err_t rbgrid_realtime_get_alt_supply(float *current_rms,
                                                float *power_active,
                                                rbpower_current_direction_t *direction,
                                                float *power_reactive,
                                                float *power_apparent,
                                                float *power_factor,
                                                uint32_t *samples_count,
                                                float *energy_increment,
                                                float *efficiency,
                                                float *dc_power);

    /**
     * @brief Get the 200 ms realtime data of a unit by unit UID.
     *
     * Every output pointer may be NULL to skip that value. Reads shared state without a lock.
     *
     * @param[in]  unit_uid       Unit UID.
     * @param[out] current_rms    RMS current, A.
     * @param[out] power_active   Active power, W.
     * @param[out] power_reactive Reactive power, var.
     * @param[out] power_apparent Apparent power, VA.
     * @param[out] power_factor   Power factor (-1..1).
     * @param[out] samples_count  Number of samples in the 200 ms window.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND Unknown unit UID.
     */
    rbgrid_err_t rbgrid_realtime_get_unit(rbgrid_unit_uid_t unit_uid,
                                          float *current_rms,
                                          float *power_active,
                                          float *power_reactive,
                                          float *power_apparent,
                                          float *power_factor,
                                          uint32_t *samples_count);

    //=============================================================================
    // PUBLIC SNAPSHOT DATA ACCESS FUNCTIONS - 3sec Statistics
    //=============================================================================

    /**
     * @brief Get the 3-second voltage snapshot (statistics_3sec_final_t: final RMS voltage,
     *        average frequency and average THD of the last 3-second period).
     *
     * @param[out] voltage_stats Destination structure.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_INVALID_ARG    @p voltage_stats is NULL.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND No voltage bus configured.
     */
    rbgrid_err_t rbgrid_snapshot_get_voltage(statistics_3sec_final_t *voltage_stats);

    /**
     * @brief Get the 3-second economic/energy snapshot of the main supply.
     *
     * The ::rbgrid_snapshot_base_t carries period cost and revenue, the current and average
     * tariff rate and zone, consumed (import) and supplied (export) energy in Wh, the last
     * direction, and the period start/end (µs) with a validity flag.
     *
     * @param[out] m_sup_snapshot Destination structure.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_INVALID_ARG    @p m_sup_snapshot is NULL.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND No main supply configured.
     */
    rbgrid_err_t rbgrid_snapshot_get_main_supply(rbgrid_snapshot_base_t *m_sup_snapshot);

    /**
     * @brief Get the 3-second economic/energy snapshot of the alternative supply.
     *
     * See rbgrid_snapshot_get_main_supply() for the snapshot contents.
     *
     * @param[out] alt_sup_snapshot Destination structure.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_INVALID_ARG    @p alt_sup_snapshot is NULL.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND No alternative supply configured.
     */
    rbgrid_err_t rbgrid_snapshot_get_alt_supply(rbgrid_snapshot_base_t *alt_sup_snapshot);

    /**
     * @brief Get the 3-second economic/energy snapshot of a unit by unit UID.
     *
     * See rbgrid_snapshot_get_main_supply() for the snapshot contents.
     *
     * @param[in]  unit_uid           Unit UID.
     * @param[out] unit_snapshot_base Destination structure.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_INVALID_ARG    @p unit_snapshot_base is NULL.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND Unknown unit UID.
     */
    rbgrid_err_t rbgrid_snapshot_get_unit(rbgrid_unit_uid_t unit_uid, rbgrid_snapshot_base_t *unit_snapshot_base);

    //=============================================================================
    // ========== TELEMETRY CONFIGURATION FUNCTIONS ==========
    //=============================================================================
    /**
     * @brief Basic telemetry configuration.
     *
     * @param[in] config           Configuration handle.
     * @param[in] enabled          Global telemetry enable.
     * @param[in] json_buffer_size JSON buffer size, bytes (default 4096).
     * @param[in] compact_json     Emit compact JSON without whitespace.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_telemetry_basic(
        rbgrid_config_handle_t config,
        bool enabled,
        uint32_t json_buffer_size,
        bool compact_json);

    /**
     * @brief Configure the MQTT telemetry channel.
     *
     * @param[in] config           Configuration handle.
     * @param[in] enabled          Enable the MQTT channel.
     * @param[in] broker_url       MQTT broker URL (e.g. "mqtt://broker.local:1883").
     * @param[in] topic            Publishing topic (e.g. "rbpower/measurements").
     * @param[in] qos              QoS level (0..2).
     * @param[in] send_interval_ms Send interval, ms.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or invalid argument.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_telemetry_mqtt(
        rbgrid_config_handle_t config,
        bool enabled,
        const char *broker_url,
        const char *topic,
        uint8_t qos,
        uint32_t send_interval_ms);

    /**
     * @brief Configure the HTTP telemetry channel.
     *
     * @param[in] config           Configuration handle.
     * @param[in] enabled          Enable the HTTP channel.
     * @param[in] endpoint_url     HTTP endpoint URL (e.g. "https://api.example.com/data").
     * @param[in] auth_token       Authorization token (may be NULL).
     * @param[in] send_interval_ms Send interval, ms.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or invalid argument.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_telemetry_http(
        rbgrid_config_handle_t config,
        bool enabled,
        const char *endpoint_url,
        const char *auth_token,
        uint32_t send_interval_ms);

    /**
     * @brief Configure the WebSocket telemetry channel.
     *
     * @param[in] config           Configuration handle.
     * @param[in] enabled          Enable the WebSocket channel.
     * @param[in] server_url       WebSocket server URL (e.g. "ws://rbgrid.local").
     * @param[in] port             Server port (e.g. 8080).
     * @param[in] send_interval_ms Send interval, ms.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or invalid argument.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_telemetry_websocket(
        rbgrid_config_handle_t config,
        bool enabled,
        const char *server_url,
        uint16_t port,
        uint32_t send_interval_ms);

    /**
     * @brief Advanced telemetry channel configuration.
     *
     * @param[in] config      Configuration handle.
     * @param[in] protocol    Telemetry protocol (MQTT/HTTP/WebSocket).
     * @param[in] retry_count Number of retries on error.
     * @param[in] timeout_ms  Send timeout, ms.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle or unknown protocol.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_telemetry_channel_advanced(
        rbgrid_config_handle_t config,
        rbpower_telemetry_protocol_t protocol,
        uint32_t retry_count,
        uint32_t timeout_ms);

    /**
     * @brief Reset all telemetry parameters to defaults (WebSocket channel only, 3000 ms interval).
     *
     * @param[in] config Configuration handle.
     * @retval RBGRID_CONFIG_OK                  Success.
     * @retval RBGRID_CONFIG_ERR_INVALID_PARAM   NULL handle.
     * @retval RBGRID_CONFIG_ERR_NOT_INITIALIZED Handle not initialised.
     */
    int rbgrid_config_set_telemetry_defaults(rbgrid_config_handle_t config);

    // ========== EXTERNAL FLASH FUNCTIONS ==========

    /**
     * @brief Check external flash availability.
     * @return true if the external flash is available, false otherwise.
     */
    bool rbgrid_flash_is_available(void);

    /**
     * @brief Check the OTP validation status.
     * @return true if OTP validation passed, false otherwise.
     */
    bool rbgrid_otp_is_validated(void);

    /**
     * @brief Get the device serial number.
     * @return Device serial number from OTP, or 0 on error.
     */
    uint32_t rbgrid_get_device_serial(void);

    /**
     * @brief Get the extended external flash status.
     * @return Status structure (by value).
     */
    rbgrid_flash_status_t rbgrid_get_flash_status(void);

    /**
     * @brief Reboot the system after an orderly shutdown of the library services.
     *
     * Performs, in order: publish of the retained MQTT "offline" status, flush of the open
     * hour as a partial record, persistence of the lifetime energy totals, deinitialisation of
     * rbpower and rbgrid, the delay, then esp_restart(). A one-shot esp_timer guarantees the
     * restart even if the orderly path blocks.
     *
     * @param[in] delay_ms Delay before the restart, ms (values above 3000 are capped to 3000).
     * @return ESP_OK; in normal operation the chip restarts before the caller resumes.
     */
    esp_err_t rbgrid_system_reboot(uint32_t delay_ms);

    // ========== FLASH HISTORICAL DATA FUNCTIONS ==========

    /**
     * @brief Get records from a flash history buffer as a JSON string.
     *
     * Reads the requested range of records and returns a ready JSON response in the format
     * described in the HTTP API reference (status, request, meta, data[]). A window starting at
     * or beyond the end of the ring yields a valid response with an empty data array.
     *
     * @param[in] unit_id Unit ID (e.g. 0x0004).
     * @param[in] period  Data period (HOUR, DAY, WEEK, MONTH, YEAR).
     * @param[in] type    Data type (ELECTRICAL, ENERGY, ECONOMICS, QUALITY, DIRECTION).
     * @param[in] offset  Offset from the oldest record (0 = oldest).
     * @param[in] limit   Number of records to read (1..720).
     * @return Heap-allocated JSON string, or NULL on error (invalid limit, unknown unit/buffer,
     *         allocation failure). The caller frees it with free().
     *
     * Usage example:
     * @code
     * char* json = rbgrid_history_get_data(0x0004, RBPOWER_PERIOD_HOUR,
     *                                      RBPOWER_DATA_TYPE_ELECTRICAL, 0, 100);
     * if (json) {
     *     printf("%s\n", json);
     *     free(json);
     * }
     * @endcode
     */
    char *rbgrid_history_get_data(
        rbpower_unit_id_t unit_id,
        rbpower_period_t period,
        rbpower_data_type_t type,
        uint32_t offset,
        uint32_t limit);

    /**
     * @brief Get history buffer information (total number of stored records).
     *
     * @param[in]  unit_id         Unit ID.
     * @param[in]  period          Data period.
     * @param[in]  type            Data type.
     * @param[out] total_available Total record count.
     * @retval RBGRID_OK                 Success.
     * @retval RBGRID_ERR_INVALID_ARG    @p total_available is NULL.
     * @retval RBGRID_ERR_UNIT_NOT_FOUND No buffer for this unit/period/type.
     */
    rbgrid_err_t rbgrid_history_get_buffer_info(
        rbpower_unit_id_t unit_id,
        rbpower_period_t period,
        rbpower_data_type_t type,
        uint32_t *total_available);

    /* ================================================================================================
     * API FOR GETTING AGGREGATED PERIOD STATISTICS
     * ================================================================================================ */

    /**
     * @brief Get completed period statistics from the flash ring buffer.
     *
     * Indexing: 0 = oldest record in the buffer, N-1 = newest. To get the last N records use
     * start_index = total_count - N and count = N. All data types (ELECTRICAL, ENERGY,
     * ECONOMICS) are read and merged into one structure per period; fields of a missing data
     * type are zeroed.
     *
     * @param[in]  unit_id          Unit ID (e.g. 0x0006).
     * @param[in]  period           Statistics period (RBPOWER_STAT_PERIOD_DAILY, RBPOWER_STAT_PERIOD_WEEKLY, ...).
     * @param[in]  start_index      Index of the first record (0 = oldest).
     * @param[in]  count            Number of records to read.
     * @param[out] stats_out        Caller-allocated array of at least @p count elements.
     * @param[out] actual_count_out Number of records actually read (may be NULL).
     * @retval RBGRID_OK              Success (0 records when @p count is 0).
     * @retval RBGRID_ERR_INVALID_ARG @p stats_out is NULL.
     * @retval RBGRID_ERR_NO_MEMORY   Temporary buffer allocation failed.
     *
     * Example: read the last 7 daily records.
     * @code
     * rbpower_period_statistics_t stats[7];
     * uint32_t total_count = 0;
     * rbgrid_history_get_buffer_info(0x0006, RBPOWER_PERIOD_DAY, RBPOWER_DATA_TYPE_ELECTRICAL, &total_count);
     *
     * uint32_t start = (total_count >= 7) ? (total_count - 7) : 0;
     * uint32_t actual_read = 0;
     * rbgrid_err_t err = rbgrid_get_period_history(0x0006, RBPOWER_STAT_PERIOD_DAILY,
     *                                                start, 7, stats, &actual_read);
     * if (err == RBGRID_OK) {
     *     printf("Read %lu daily records\n", (unsigned long)actual_read);
     *     for (uint32_t i = 0; i < actual_read; i++) {
     *         printf("Day %lu: Energy=%fWh\n", (unsigned long)i, stats[i].energy_period.consumed_wh);
     *     }
     * }
     * @endcode
     */
    rbgrid_err_t rbgrid_get_period_history(
        rbpower_unit_id_t unit_id,
        rbpower_stat_period_type_t period,
        uint32_t start_index,
        uint32_t count,
        rbpower_period_statistics_t *stats_out,
        uint32_t *actual_count_out);

#ifdef __cplusplus
}
#endif

#endif // RBGRID_CONFIG_API_H
