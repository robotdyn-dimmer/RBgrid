/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbtelemetry.h
 * @brief rbpower telemetry fan-out: one telemetry JSON per 3 s period, sent to WebSocket, MQTT and HTTP channels.
 *
 * At the end of every 3 s statistics period rbpower_telemetry_on_3sec_period_end() builds the
 * telemetry JSON once (under the home-network lock) and sends it, lock-free, to every enabled
 * channel: WebSocket clients (rbwebsocket_telemetry.h), the MQTT broker (rbmqtt.h, non-blocking
 * publish) and the HTTP channel (currently logs only, no HTTP POST is performed).
 * The WebSocket channel is additionally gated by client presence: the WebSocket handler calls
 * rbpower_telemetry_start_websocket() / rbpower_telemetry_stop_websocket().
 */

#ifndef RBTELEMETRY_H
#define RBTELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#include "rbpower_constants.h"
#include "rbpower_enums.h"

/** @brief Per-period statistics of one unit (forward declaration to avoid a circular include; defined in rbhome_types.h). */
typedef struct rbpower_period_statistics rbpower_period_statistics_t;

#ifdef __cplusplus
extern "C"
{
#endif

    /* ================================================================================================
     * TELEMETRY PROTOCOLS
     * ================================================================================================ */

    /**
     * @brief Telemetry protocol types (index into rbpower_telemetry_config_t::channels)
     */
    typedef enum
    {
        RBPOWER_TELEMETRY_MQTT = 0,     ///< MQTT publish via rbmqtt
        RBPOWER_TELEMETRY_HTTP,         ///< HTTP channel (logs only; no POST is performed)
        RBPOWER_TELEMETRY_WEBSOCKET,    ///< WebSocket broadcast to `/ws-telemetry` clients
        RBPOWER_TELEMETRY_PROTOCOLS_MAX ///< Number of channels (array size, not a protocol)
    } rbpower_telemetry_protocol_t;

    /**
     * @brief Configuration of one telemetry channel
     */
    typedef struct
    {
        bool enabled;              ///< Channel is active
        uint32_t send_interval_ms; ///< Send interval, ms (informational; sends follow the 3 s period)
        uint32_t retry_count;      ///< Number of retries on error
        uint32_t timeout_ms;       ///< Send timeout, ms

        /** @brief Protocol-specific settings (the member matching the channel index is used) */
        union
        {
            /** @brief MQTT channel settings */
            struct
            {
                const char *broker_url; ///< MQTT broker URL
                const char *topic;      ///< Topic for publishing (NULL → "rbgrid/telemetry")
                uint8_t qos;            ///< QoS level
            } mqtt;                     ///< MQTT settings

            /** @brief HTTP channel settings */
            struct
            {
                const char *endpoint_url; ///< HTTP endpoint URL
                const char *auth_token;   ///< Authorization token
            } http;                       ///< HTTP settings

            /** @brief WebSocket channel settings */
            struct
            {
                const char *server_url; ///< WebSocket server URL
                uint16_t port;          ///< Port
            } websocket;                ///< WebSocket settings
        } config;                       ///< Protocol-specific settings

    } rbpower_telemetry_channel_t;

    /**
     * @brief Main telemetry configuration
     */
    typedef struct rbpower_telemetry_config_s
    {
        bool telemetry_enabled;    ///< Global telemetry enable
        uint32_t json_buffer_size; ///< Size of the JSON work buffer allocated at init, bytes
        bool compact_json;         ///< Compact JSON (without spaces)

        /// Telemetry channels, indexed by rbpower_telemetry_protocol_t
        rbpower_telemetry_channel_t channels[RBPOWER_TELEMETRY_PROTOCOLS_MAX];

    } rbpower_telemetry_config_t;

    /**
     * @brief Data of one unit for telemetry
     */
    typedef struct
    {
        uint8_t unit_id;                          ///< Measurement unit ID
        const rbpower_period_statistics_t *stats; ///< Pointer to the unit's period statistics (NULL when not valid; not owned)
        bool data_valid;                          ///< Data is valid
        bool accumulation_enabled;                ///< Accumulation was active
    } rbpower_telemetry_unit_data_t;

    /**
     * @brief Data packet for telemetry (all units for period)
     */
    typedef struct
    {
        uint64_t timestamp_us;                                       ///< Period end timestamp, µs (esp_timer time base)
        rbpower_stat_period_type_t period_type;                      ///< Period type (3sec, 15min, etc.)
        uint8_t units_count;                                         ///< Number of units in packet
        rbpower_telemetry_unit_data_t units[RBPOWER_MAX_HOME_UNITS]; ///< Array of unit data (max RBPOWER_MAX_HOME_UNITS)
    } rbpower_telemetry_packet_t;

    /* ================================================================================================
     * API FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Initialize telemetry module (call once at boot, from a task).
     * @param[in] config  Telemetry configuration; copied (pointed-to strings must stay valid).
     * @retval ESP_OK                Initialized.
     * @retval ESP_ERR_INVALID_ARG   @p config is NULL.
     * @retval ESP_ERR_INVALID_STATE Already initialized.
     * @retval ESP_ERR_NO_MEM        JSON buffer or mutex allocation failed.
     */
    esp_err_t rbpower_telemetry_init(const rbpower_telemetry_config_t *config);

    /**
     * @brief Deinitialize telemetry module (frees the buffer and mutex, clears the configuration).
     */
    void rbpower_telemetry_deinit(void);

    /**
     * @brief Build JSON from a telemetry packet and send it to all active channels.
     * @param[in] packet  Data packet.
     * @retval ESP_OK                Sent (channels that are disabled/not connected are skipped).
     * @retval ESP_ERR_INVALID_ARG   @p packet is NULL or the module is not initialized.
     * @retval ESP_ERR_INVALID_STATE Telemetry globally disabled, or the network is unavailable.
     * @retval ESP_ERR_NO_MEM        JSON allocation failed.
     * @return Otherwise the error of the last failing channel.
     */
    esp_err_t rbpower_telemetry_send_packet(const rbpower_telemetry_packet_t *packet);

    /**
     * @brief Create JSON from a telemetry packet (legacy "measurement_data" schema).
     * @param[in]  packet    Data packet.
     * @param[out] json_out  Created JSON object (must be freed with cJSON_Delete).
     * @retval ESP_OK                Created.
     * @retval ESP_ERR_INVALID_ARG   NULL argument or module not initialized.
     * @retval ESP_ERR_INVALID_STATE The home network is unavailable.
     * @retval ESP_ERR_NO_MEM        cJSON allocation failed.
     */
    esp_err_t rbpower_telemetry_create_json(const rbpower_telemetry_packet_t *packet, cJSON **json_out);

    /**
     * @brief Enable/disable a telemetry channel.
     * @param[in] protocol  Protocol type.
     * @param[in] enabled   true to enable, false to disable.
     * @retval ESP_OK              Updated.
     * @retval ESP_ERR_INVALID_ARG @p protocol out of range or module not initialized.
     */
    esp_err_t rbpower_telemetry_set_channel_enabled(rbpower_telemetry_protocol_t protocol, bool enabled);

    /**
     * @brief Handler for 3-second period completion (called by rbhome at the end of each 3 s cycle).
     *
     * Builds the telemetry JSON once under the home-network lock (plus the v2 frame when a
     * `?v=2` WebSocket client is connected), releases the lock and sends it to every enabled
     * channel. Must be called with the home-network lock NOT held. MQTT uses the non-blocking
     * publish because the caller runs in the esp_timer task.
     *
     * @param[in] period_end_time_us  End of the completed period, µs (esp_timer time base).
     */
    void rbpower_telemetry_on_3sec_period_end(uint64_t period_end_time_us);

    /**
     * @brief Get the latest stored telemetry packet (legacy packet path).
     * @param[out] packet  Structure that receives a copy of the packet.
     * @retval RBPOWER_OK           Copied.
     * @retval RBPOWER_ERR_NO_DATA  @p packet is NULL, module not initialized, or no packet stored yet.
     * @retval RBPOWER_ERR_TIMEOUT  The telemetry mutex could not be taken within 100 ms.
     */
    rbpower_err_t rbpower_get_latest_telemetry(rbpower_telemetry_packet_t *packet);

    /**
     * @brief Enable telemetry sending via WebSocket (frames then go out every 3 s period).
     *
     * Only sets a flag; no task is created. Called by the WebSocket handler when the first
     * client connects (httpd task).
     *
     * @retval ESP_OK                Enabled.
     * @retval ESP_ERR_INVALID_STATE Telemetry module not initialized.
     */
    esp_err_t rbpower_telemetry_start_websocket(void);

    /**
     * @brief Disable telemetry sending via WebSocket (clears the flag).
     *
     * Called by the WebSocket handler when the last client disconnects.
     *
     * @return ESP_OK always.
     */
    esp_err_t rbpower_telemetry_stop_websocket(void);

    /**
     * @brief Send a single telemetry frame via WebSocket now (current state, outside the 3 s cycle).
     *
     * Used for the first frame after a client connects and for the `get_telemetry` command.
     * Takes the home-network lock while building the JSON.
     *
     * @retval ESP_OK                Queued to at least one client.
     * @retval ESP_ERR_INVALID_STATE Module not initialized, or the WebSocket channel is disabled.
     * @retval ESP_ERR_NO_MEM        Telemetry JSON could not be built.
     * @retval ESP_ERR_INVALID_ARG   No WebSocket client has connected yet.
     * @retval ESP_FAIL              No client received the frame.
     */
    esp_err_t rbpower_telemetry_send_websocket_once(void);

#ifdef __cplusplus
}
#endif

#endif // RBTELEMETRY_H