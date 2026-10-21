/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file    rbmqtt.c
 * @brief   RBgrid MQTT client — real esp-mqtt publisher (see rbmqtt.h).
 *
 * esp-mqtt lifecycle: init from a configurable broker URI → event handler tracks a
 * `connected` flag (esp-mqtt auto-reconnects internally) → publishes are gated on it so a
 * down broker never blocks the caller. LWT marks the device offline on an unclean drop;
 * on connect we publish "online" (retained). No secrets here — Wi-Fi creds are provisioned
 * separately; the broker URI is passed in from config.
 */
#include "rbmqtt.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "RBMQTT";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static rbmqtt_connect_cb_t s_connect_cb = NULL;   // (re)publish HA Discovery configs on connect
void rbmqtt_register_connect_cb(rbmqtt_connect_cb_t cb) { s_connect_cb = cb; }

/* Owned copies of the strings esp-mqtt needs to outlive the caller's buffers. */
static char s_lwt_topic[96] = {0};

/* Command/config channel (optional; enabled via rbmqtt_enable_commands). */
static char s_cmd_topic[96]  = {0};
static char s_resp_topic[96] = {0};
static rbmqtt_cmd_handler_t s_cmd_handler = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "connected to broker");
            // Mark online (retained) so a late subscriber sees current liveness.
            if (s_lwt_topic[0] && s_client) {
                esp_mqtt_client_publish(s_client, s_lwt_topic, "online", 0, 1, true);
            }
            // (Re)subscribe to the command topic on every connect (subscriptions don't survive
            // a reconnect with a clean session).
            if (s_cmd_topic[0] && s_client) {
                int mid = esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
                ESP_LOGI(TAG, "subscribed cmd topic '%s' (mid=%d)", s_cmd_topic, mid);
            }
            // (Re)publish HA MQTT Discovery configs (retained) so HA auto-creates entities on connect
            // and re-discovers after an HA restart.
            if (s_connect_cb) {
                s_connect_cb();
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "disconnected from broker");
            break;
        case MQTT_EVENT_DATA:
            // Command channel: a message on the subscribed cmd topic. Config payloads are small
            // and arrive in a single event (esp-mqtt buffer >> command size); ignore chunked.
            if (s_cmd_handler && event && event->topic && event->topic_len > 0 &&
                event->current_data_offset == 0 &&
                (size_t)event->topic_len == strlen(s_cmd_topic) &&
                strncmp(event->topic, s_cmd_topic, event->topic_len) == 0) {
                // Guard: a payload larger than buffer.size arrives chunked (data_len <
                // total_data_len). We only process single-shot messages, so reject an oversized
                // command with a clear error instead of parsing a truncated fragment.
                if (event->total_data_len > event->data_len) {
                    ESP_LOGW(TAG, "cmd payload too large: %d > buffer %d — rejected",
                             event->total_data_len, event->data_len);
                    if (s_resp_topic[0] && s_client && s_connected) {
                        esp_mqtt_client_publish(s_client, s_resp_topic,
                            "{\"type\":\"cmd_ack\",\"data\":{\"result\":\"error\","
                            "\"message\":\"command payload too large for MQTT buffer\"}}",
                            0, 1, false);
                    }
                    break;
                }
                char *resp = s_cmd_handler(event->data, event->data_len);
                if (resp) {
                    if (s_resp_topic[0] && s_client && s_connected) {
                        esp_mqtt_client_publish(s_client, s_resp_topic, resp, 0, 1, false);
                    }
                    free(resp);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "mqtt error event");
            break;
        default:
            break;
    }
}

static char s_broker_uri[160];
const char *rbmqtt_broker_uri(void) { return s_broker_uri; }
esp_err_t rbmqtt_init(const char *broker_uri, const char *client_id, const char *lwt_topic)
{
    strlcpy(s_broker_uri, broker_uri ? broker_uri : "", sizeof(s_broker_uri));
    if (!broker_uri || broker_uri[0] == '\0') {
        ESP_LOGE(TAG, "init: empty broker_uri");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client) {
        ESP_LOGD(TAG, "init: already initialized");
        return ESP_OK;
    }

    if (lwt_topic && lwt_topic[0]) {
        strncpy(s_lwt_topic, lwt_topic, sizeof(s_lwt_topic) - 1);
    } else {
        s_lwt_topic[0] = '\0';
    }

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = broker_uri;          // esp-mqtt copies this internally on init
    cfg.credentials.client_id = client_id;        // NULL → esp-mqtt default id
    cfg.session.keepalive = 30;
    // Buffers sized for the command/config channel: inbound SET payloads (a full tariff plan or
    // units-config) run multi-KB — the 1 KB default truncated them so the JSON never parsed
    // (a 2183 B tariff SET failed with "invalid JSON"). 8 KB in covers tariff + a 6-unit
    // units-config with margin; 8 KB out lets large GET responses (units / telemetry_data) publish
    // without chunking. ~16 KB of client buffers — fine with PSRAM headroom.
    cfg.buffer.size = 8192;
    cfg.buffer.out_size = 8192;
    /* rbmqtt_publish_nb() queues in the outbox; cap it so a stalled broker cannot eat the heap (~6 frames) */
    cfg.outbox.limit = 32 * 1024;
    if (s_lwt_topic[0]) {
        cfg.session.last_will.topic = s_lwt_topic;
        cfg.session.last_will.msg = "offline";
        cfg.session.last_will.qos = 1;
        cfg.session.last_will.retain = true;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    const char *at = strrchr(broker_uri, '@');   /* no user:password in logs */
    ESP_LOGI(TAG, "client started, broker=%s%s id=%s", at ? "***@" : "", at ? at + 1 : broker_uri, client_id ? client_id : "(default)");
    return ESP_OK;
}

bool rbmqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t rbmqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;   // broker down → skip, never block the caller
    }
    if (!topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    int mid = esp_mqtt_client_publish(s_client, topic, payload, 0 /* len: strlen */, qos, retain);
    return (mid >= 0) ? ESP_OK : ESP_FAIL;
}

/* Non-blocking: the message goes to the client outbox and the MQTT task sends it. For callers that must not
 * wait on the socket — the 3-s telemetry runs in the esp_timer task (priority 22), where a blocking publish to a
 * slow broker stalled every other esp_timer callback. */
esp_err_t rbmqtt_publish_nb(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    int mid = esp_mqtt_client_enqueue(s_client, topic, payload, 0 /* len: strlen */, qos, retain, true /* store QoS 0 too */);
    return (mid >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t rbmqtt_publish_offline(void)
{
    if (!s_client || !s_connected || !s_lwt_topic[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    int mid = esp_mqtt_client_publish(s_client, s_lwt_topic, "offline", 0, 1, true);
    return (mid >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t rbmqtt_enable_commands(const char *cmd_topic, const char *resp_topic,
                                 rbmqtt_cmd_handler_t handler)
{
    if (!cmd_topic || !cmd_topic[0] || !handler) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_cmd_topic, cmd_topic, sizeof(s_cmd_topic) - 1);
    s_cmd_topic[sizeof(s_cmd_topic) - 1] = '\0';
    if (resp_topic && resp_topic[0]) {
        strncpy(s_resp_topic, resp_topic, sizeof(s_resp_topic) - 1);
        s_resp_topic[sizeof(s_resp_topic) - 1] = '\0';
    }
    s_cmd_handler = handler;
    // If already connected, subscribe now (otherwise the CONNECTED handler will).
    if (s_connected && s_client) {
        int mid = esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        ESP_LOGI(TAG, "subscribed cmd topic '%s' (mid=%d)", s_cmd_topic, mid);
    }
    ESP_LOGI(TAG, "command channel enabled: cmd='%s' resp='%s'", s_cmd_topic, s_resp_topic);
    return ESP_OK;
}

void rbmqtt_stop(void)
{
    if (!s_client) return;
    s_connected = false;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    ESP_LOGI(TAG, "client stopped");
}
