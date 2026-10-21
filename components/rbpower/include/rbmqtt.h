/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file    rbmqtt.h
 * @brief   RBgrid MQTT client — real esp-mqtt publisher for the telemetry MQTT channel.
 *
 * Encapsulates the esp-mqtt lifecycle (init / event-handler / connected-flag / publish /
 * LWT). The telemetry layer (rbtelemetry.c send_mqtt) calls rbmqtt_publish; init/stop are
 * tied to Wi-Fi STA up/down. Broker URL is configurable (no hardcoded host). No secrets
 * live here — Wi-Fi credentials are provisioned separately (NVS / portal).
 */
#ifndef RBMQTT_H
#define RBMQTT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize + start the MQTT client (idempotent). Connects asynchronously;
 *        esp-mqtt auto-reconnects. Publishes are gated on the connected state.
 *
 * Call from a task context (typically on Wi-Fi STA up). Not thread-safe against a
 * concurrent rbmqtt_stop().
 *
 * @param[in] broker_uri  Broker URI, e.g. "mqtt://broker.local:1883" (NULL/empty → no-op, returns error).
 *                        The URI is also stored for rbmqtt_broker_uri().
 * @param[in] client_id   Unique client id, e.g. "rbgrid-\<mac\>" (NULL → esp-mqtt default).
 * @param[in] lwt_topic   Last-Will topic (e.g. "rbgrid/\<dev\>/status"); NULL to disable LWT.
 *                        On connect the client publishes "online" (retained) to this topic;
 *                        the broker publishes "offline" if the client drops uncleanly.
 * @retval ESP_OK              Client started, or already initialized.
 * @retval ESP_ERR_INVALID_ARG @p broker_uri is NULL or empty.
 * @retval ESP_FAIL            esp_mqtt_client_init() failed.
 * @return Any other error from esp_mqtt_client_register_event() / esp_mqtt_client_start().
 */
esp_err_t rbmqtt_init(const char *broker_uri, const char *client_id, const char *lwt_topic);

/**
 * @brief Report whether the MQTT session is currently connected to the broker.
 * @return true if connected (flag maintained by the esp-mqtt event handler), false otherwise.
 */
bool rbmqtt_is_connected(void);

/**
 * @brief Broker URI that rbmqtt_init() was last given.
 * @return Pointer to an internal static buffer ("" before the first init). Never NULL.
 */
const char *rbmqtt_broker_uri(void);

/**
 * @brief Publish a payload through esp_mqtt_client_publish(). No-op (returns error) if
 *        not connected, so a down broker never stalls the caller.
 *
 * For QoS 0 the call may block on the socket while sending; use rbmqtt_publish_nb()
 * from time-critical contexts (e.g. esp_timer callbacks).
 *
 * @param[in] topic    Topic string (NUL-terminated).
 * @param[in] payload  Payload string (NUL-terminated; length taken with strlen).
 * @param[in] qos      MQTT QoS level (0, 1 or 2).
 * @param[in] retain   true to set the retain flag.
 * @retval ESP_OK                Message handed to esp-mqtt.
 * @retval ESP_ERR_INVALID_STATE Client not initialized or not connected.
 * @retval ESP_ERR_INVALID_ARG   @p topic or @p payload is NULL.
 * @retval ESP_FAIL              esp-mqtt rejected the publish.
 */
esp_err_t rbmqtt_publish(const char *topic, const char *payload, int qos, bool retain);

/**
 * @brief Publish retained "offline" on the LWT/status topic now (graceful-shutdown path).
 *
 *        A planned esp_restart() kills the TCP session without FIN, so the broker only
 *        fires the LWT after its keepalive expires (minutes) — by which time the rebooted
 *        client has already republished "online". Call this before a deliberate reboot so
 *        the offline→online transition is actually observable on the status topic.
 * @return ESP_OK if enqueued; ESP_ERR_INVALID_STATE if not connected or LWT disabled;
 *         ESP_FAIL if esp-mqtt rejected the publish.
 */
esp_err_t rbmqtt_publish_offline(void);

/**
 * @brief Like rbmqtt_publish(), but never blocks: the message is queued in the client
 *        outbox (limit 32 KB) and sent by the MQTT task.
 *
 * Intended for callers that must not wait on the socket, e.g. the 3 s telemetry tick that
 * runs in the esp_timer task.
 *
 * @param[in] topic    Topic string (NUL-terminated).
 * @param[in] payload  Payload string (NUL-terminated; length taken with strlen).
 * @param[in] qos      MQTT QoS level (0, 1 or 2). QoS 0 messages are stored in the outbox too.
 * @param[in] retain   true to set the retain flag.
 * @retval ESP_OK                Message queued.
 * @retval ESP_ERR_INVALID_STATE Client not initialized or not connected.
 * @retval ESP_ERR_INVALID_ARG   @p topic or @p payload is NULL.
 * @retval ESP_FAIL              Enqueue failed (e.g. outbox full).
 */
esp_err_t rbmqtt_publish_nb(const char *topic, const char *payload, int qos, bool retain);

/**
 * @brief Command-channel handler. Receives the raw payload of a message published to the
 *        subscribed command topic; returns a malloc'd response JSON string (rbmqtt publishes it
 *        to the response topic and frees it) or NULL to send nothing.
 *
 * Called from the esp-mqtt task. @p data is not NUL-terminated; use @p data_len.
 */
typedef char* (*rbmqtt_cmd_handler_t)(const char *data, int data_len);

/**
 * @brief Enable the bidirectional command/config channel. On every (re)connect rbmqtt
 *        subscribes to @p cmd_topic; an incoming message is passed to @p handler and the
 *        returned JSON is published to @p resp_topic. Lets configuration be read/written fully
 *        over MQTT (mirror of the HTTP config API). Call after rbmqtt_init. Topics are copied
 *        (max 95 characters each).
 * @param[in] cmd_topic   Topic to subscribe, e.g. "rbgrid/\<dev\>/cmd".
 * @param[in] resp_topic  Topic for responses, e.g. "rbgrid/\<dev\>/cmd/resp" (NULL/empty → keep previous).
 * @param[in] handler     Command dispatcher (e.g. rbgrid_command_dispatch).
 * @retval ESP_OK              Channel enabled (subscribed now if already connected).
 * @retval ESP_ERR_INVALID_ARG @p cmd_topic NULL/empty or @p handler NULL.
 */
esp_err_t rbmqtt_enable_commands(const char *cmd_topic, const char *resp_topic,
                                 rbmqtt_cmd_handler_t handler);

/**
 * @brief Callback invoked on every (re)connect to the broker, after the LWT "online"
 *        publish + command resubscribe. Runs in the esp-mqtt task.
 */
typedef void (*rbmqtt_connect_cb_t)(void);

/**
 * @brief Register the connect callback (replaces any previous one).
 *
 * Used to (re)publish retained MQTT Discovery configs so Home Assistant auto-creates entities
 * on connect and re-discovers after a Home Assistant restart.
 *
 * @param[in] cb  Callback, or NULL to unregister.
 */
void rbmqtt_register_connect_cb(rbmqtt_connect_cb_t cb);

/** @brief Stop + destroy the client (Wi-Fi down / shutdown). */
void rbmqtt_stop(void);

#ifdef __cplusplus
}
#endif

#endif // RBMQTT_H
