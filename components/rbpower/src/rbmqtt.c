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
#include "rbgrid_core.h"
#include "rbpower.h"   /* rbtime_is_time_valid */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rbmqtt.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/x509.h"
#include "littlefs_raw.h"   /* the user's broker CA (/mqtt_ca.pem) */
#include "esp_heap_caps.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "rbhome_types.h"   /* rbpower_diag_printf */

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

/* TLS verify trace: the bundle's verify callback, wrapped so a rejected certificate leaves a line in the
 * event log (release builds have no console). Records the last failing depth / issuer / flags. */
static char s_vrfy_note[160];
static volatile uint32_t s_vrfy_flags = 0;   /* X.509 flags from the bundle's verify step (esp-tls reports 0 there) */
/* TLS state for the app: off | waiting_clock | connecting | ok | expired | not_yet_valid | unknown_ca | name_mismatch
 * | tls_error. The CA in use: "bundle" (public CAs) or "custom" (/mqtt_ca.pem, uploaded by the user). */
static const char *volatile s_tls_state = "off";
static bool s_tls_custom_ca = false;
static char *s_ca_pem = NULL;   /* kept for the client's lifetime (esp-tls reads it at every connect) */
const char *rbmqtt_tls_state(void) { return s_tls_state; }
bool rbmqtt_tls_custom_ca(void) { return s_tls_custom_ca; }

/* X.509 verify flags → the state code the app shows */
static const char *tls_state_from_flags(uint32_t f)
{
    if (f & MBEDTLS_X509_BADCERT_EXPIRED) return "expired";
    if (f & MBEDTLS_X509_BADCERT_FUTURE) return "not_yet_valid";
    if (f & MBEDTLS_X509_BADCERT_CN_MISMATCH) return "name_mismatch";
    if (f & MBEDTLS_X509_BADCERT_NOT_TRUSTED) return "unknown_ca";
    return f ? "tls_error" : NULL;
}
int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *const crt, const int depth, uint32_t *const flags);  /* esp_crt_bundle.c (not in its header) */
static int rbmqtt_verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    uint32_t in = *flags;
    int r = esp_crt_verify_callback(ctx, crt, depth, flags);
    /* the bundle refuses an issuer it doesn't hold with an error but no flag: that is an unknown CA */
    if (r != 0 && *flags == 0) s_vrfy_flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    s_vrfy_flags |= *flags;
    if (r != 0 || *flags != 0) {
        char iss[96] = {0};
        mbedtls_x509_dn_gets(iss, sizeof(iss), &crt->issuer);
        snprintf(s_vrfy_note, sizeof(s_vrfy_note), "depth=%d flags_in=0x%x out=0x%x ret=-0x%x issuer=%.80s",
                 depth, (unsigned)in, (unsigned)*flags, (unsigned)(-r), iss);
    }
    return r;
}
static esp_err_t rbmqtt_crt_bundle_attach(void *conf)
{
    esp_err_t e = esp_crt_bundle_attach(conf);
    if (e == ESP_OK && conf) mbedtls_ssl_conf_verify((mbedtls_ssl_config *)conf, rbmqtt_verify_cb, NULL);
    return e;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            if (strcmp(s_tls_state, "off")) s_tls_state = "ok";
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
            /* "ok" no longer holds; a certificate reason set by the ERROR event before this one is kept */
            if (!strcmp(s_tls_state, "ok")) s_tls_state = "connecting";
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
            /* Release builds have no console: put the cause in the event log, at most once a minute
             * (esp-mqtt retries every few seconds). TLS failures carry the mbedTLS code + X.509 flags. */
            if (event && event->error_handle) {
                const esp_mqtt_error_codes_t *e = event->error_handle;
                /* TLS state for the app: on every failed attempt (the log below is rate-limited) */
                if (strcmp(s_tls_state, "off")) {
                    uint32_t vf = (uint32_t)e->esp_tls_cert_verify_flags;
                    if (vf == 0 || vf == 0xFFFFFFFFu) vf = s_vrfy_flags;   /* 0 / -1 = not reported (bundle path): ours */
                    s_vrfy_flags = 0;
                    const char *st = tls_state_from_flags(vf);
                    if (st) {
                        if (strcmp(st, s_tls_state)) rbpower_diag_printf("FAIL mqtt: broker certificate refused: %s (CA: %s)", st,
                                                                         s_tls_custom_ca ? "custom" : "public bundle");
                        s_tls_state = st;
                    } else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT && e->esp_tls_stack_err &&
                               (!strcmp(s_tls_state, "connecting") || !strcmp(s_tls_state, "ok"))) {
                        s_tls_state = "tls_error";   /* no certificate reason: keep a known one (expired …) */
                    }
                }
                static int64_t s_last_err_us = 0;
                int64_t now = esp_timer_get_time();
                if (s_last_err_us == 0 || now - s_last_err_us > 60000000LL) {
                    s_last_err_us = now;
                    rbpower_diag_printf("mqtt: connect failed: type=%d tls_err=0x%x tls_stack=-0x%x cert_flags=0x%x sock_errno=%d refused=%d",
                                        (int)e->error_type, (unsigned)e->esp_tls_last_esp_err,
                                        (unsigned)(-e->esp_tls_stack_err), (unsigned)e->esp_tls_cert_verify_flags,
                                        e->esp_transport_sock_errno, (int)e->connect_return_code);
                    if (s_vrfy_note[0]) { rbpower_diag_printf("mqtt: tls verify: %s", s_vrfy_note); s_vrfy_note[0] = 0; }
                }
            }
            break;
        default:
            break;
    }
}

/* TLS broker: start the client once the clock is real (certificate validity dates are checked). */
static void rbmqtt_tls_wait_task(void *arg)
{
    (void)arg;
    while (s_client && !rbtime_is_time_valid()) vTaskDelay(pdMS_TO_TICKS(1000));
    if (s_client) {
        s_tls_state = "connecting";
        esp_err_t e = esp_mqtt_client_start(s_client);
        rbpower_diag_printf("mqtt: clock set — TLS connection started: %s", esp_err_to_name(e));
    }
    vTaskDelete(NULL);
}

static char s_broker_uri[160];
const char *rbmqtt_broker_uri(void) { return s_broker_uri; }
/* Planned reboot: publish the retained "offline" first (the core runs its reboot notifiers). */
static void rbmqtt_reboot_notify(void) { (void)rbmqtt_publish_offline(); }
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

    /* mqtts:// / wss://: verify the broker against the ESP-IDF CA bundle (public CAs). Without it esp-mqtt
     * had no CA at all and every TLS connect failed. A broker with a self-signed / private-CA certificate
     * still fails verification — that needs a user-supplied CA (not in this release). */
    const bool tls = strncmp(broker_uri, "mqtts://", 8) == 0 || strncmp(broker_uri, "wss://", 6) == 0;
    s_tls_state = tls ? "connecting" : "off";
    s_tls_custom_ca = false;
    if (tls) {
        /* The user's own CA (a home broker with a private CA / self-signed certificate) replaces the public bundle:
         * uploaded with POST /api/system/mqtt-ca, read at MQTT start. Dates and the host name are checked either way. */
        size_t sz = 0;
        if (littlefs_raw_size(RBMQTT_CA_FILE, &sz) == ESP_OK && sz > 0 && sz <= RBMQTT_CA_MAX) {
            free(s_ca_pem);
            s_ca_pem = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_ca_pem) s_ca_pem = malloc(sz + 1);
            size_t got = 0;
            if (s_ca_pem && littlefs_raw_read(RBMQTT_CA_FILE, s_ca_pem, sz, &got) == ESP_OK && got == sz) {
                s_ca_pem[sz] = 0;
                cfg.broker.verification.certificate = s_ca_pem;
                cfg.broker.verification.certificate_len = sz + 1;   /* PEM: length includes the NUL */
                s_tls_custom_ca = true;
            } else { free(s_ca_pem); s_ca_pem = NULL; }
        }
        if (!s_tls_custom_ca) cfg.broker.verification.crt_bundle_attach = rbmqtt_crt_bundle_attach;
        ESP_LOGI(TAG, "TLS broker: verifying against %s", s_tls_custom_ca ? "the uploaded CA (/mqtt_ca.pem)" : "the CA bundle");
        rbpower_diag_printf("mqtt: TLS broker, CA: %s", s_tls_custom_ca ? "custom (uploaded)" : "public bundle");
    }
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    (void)rbgrid_core_register_reboot_notify(rbmqtt_reboot_notify);
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err == ESP_OK) {
        if (tls && !rbtime_is_time_valid()) {
            /* The broker certificate's dates are checked (CONFIG_MBEDTLS_HAVE_TIME_DATE): before the clock is real
             * every certificate reads "not yet valid". Start the TLS connection once the clock is set (NTP or by
             * hand); plain mqtt:// is not delayed. */
            s_tls_state = "waiting_clock";
            if (xTaskCreate(rbmqtt_tls_wait_task, "mqtt_tls_wait", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS) {
                rbpower_diag_printf("mqtt: TLS broker — waiting for the clock before connecting (certificate dates)");
            } else {
                err = esp_mqtt_client_start(s_client);   /* no task: try now, the client keeps retrying */
            }
        } else {
            err = esp_mqtt_client_start(s_client);
        }
    }
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
