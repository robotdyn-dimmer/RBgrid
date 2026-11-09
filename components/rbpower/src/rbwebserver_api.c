/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver_api.c
 * @brief REST/config API endpoints, token authentication and the MQTT/HTTP command dispatcher.
 *
 * Holds the /api/system and /api/v1 config/units/sensors/history/economics/summary/export handlers,
 * the two-role API-token guards (rbapi_require_auth / rbapi_require_read) and the unified command
 * dispatcher (rbgrid_command_dispatch) shared by the MQTT command channel. The shared CORS helper
 * comes from the internal header and the core module state (g_extended_config / g_json_context —
 * both static in rbwebserver.c) is reached via the rbgrid_core_ext_cfg() /
 * rbgrid_core_json_ctx() accessors.
 *
 * All HTTP handlers run on the httpd task; reads of g_home_network take rbpower_home_lock().
 */

#include "rbdynprice.h"
#include <math.h>
#include "rbwebserver_internal.h"
#include "rbstats_migrate.h"   /* history 503 during deferred format migration */
#include "rbhome_types.h"            /* g_home_network + rbpower_home_lock/unlock */
#include "rbgrid_config_storage.h"
#include "rbgrid_config_api.h"
#include "rbhome_tariff.h"           /* economics getters for GET /api/v1/economics */
#include "rbmqtt.h"                  /* rbmqtt_is_connected — GET /api/system/mqtt-config */
#include "rbjson.h"                  /* rbgrid_telemetry_set/get_lean — telemetry profile */
#include "littlefs_raw.h"            /* persist the MQTT broker URI / telemetry profile */
#include "esp_app_desc.h"            /* esp_app_get_description — fw version in /api/v1/schema */
#include "rbtelemetry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>                    /* round() — economics/summary value rounding */
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */
#include "rbamp_sensor_models.h"   /* CT model registry for schema enums.ct_model + PATCH ct_code check */

static const char* TAG = "RBWEBSERVER_API";
/** @brief Oldest record timestamp accepted as real history, epoch s (2025-01-01).
 *
 * Anything earlier is a fake-clock / Jan-2024-fallback artifact, never real history (the previous
 * 2020-09 bound let the 2024 fallback records through). */
#define RBGRID_TS_SANE_MIN 1735689600u

/* ==== TWO-ROLE token auth + 401 rate-limit.
 * Tokens persist in littlefs: /api_token = ADMIN (writes + everything), /api_token_read = READ
 * (data reads: history/economics/summary/export + WS telemetry). Arming model (migration-safe for
 * existing installs): NO admin token provisioned = onboarding state → everything open.
 * Admin token set → writes require ADMIN (a READ token on a write = 403, not 401). Read token
 * additionally set → gated data reads require READ or ADMIN. Token sources: `Authorization:
 * Bearer`, `X-API-Key`, or `?token=` in the query (WS handshake can't set headers from browsers).
 * Rate-limit: >10 FAILED auths in 60 s → further invalid attempts get 429 + Retry-After (valid
 * tokens keep working — the limiter slows guessing, it must not lock out the owner). ==== */

/** @brief Role granted to a request by the presented token. */
typedef enum {
    RBAPI_ROLE_NONE = 0,  ///< No / wrong token while auth is armed
    RBAPI_ROLE_READ = 1,  ///< READ token: gated data reads only
    RBAPI_ROLE_ADMIN = 2  ///< ADMIN token, or auth not armed (onboarding)
} rbapi_role_t;


/* The token presented by the request ("" if none). */
static void rbapi_presented_token(httpd_req_t* req, char out[96])
{
    out[0] = '\0';
    char hdr[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && hdr[0]) {
        const char* p = (strncmp(hdr, "Bearer ", 7) == 0) ? hdr + 7 : hdr;
        strlcpy(out, p, 96);
        return;
    }
    if (httpd_req_get_hdr_value_str(req, "X-API-Key", hdr, sizeof(hdr)) == ESP_OK && hdr[0]) {
        strlcpy(out, hdr, 96);
        return;
    }
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[96];
        if (httpd_query_key_value(q, "token", v, sizeof(v)) == ESP_OK) strlcpy(out, v, 96);
    }
}

/* One-time WebSocket tickets: a browser can't put an Authorization header on a WebSocket, and a long-lived
 * token in the URL ends up in logs. POST /api/v1/ws-ticket (Bearer) issues a random ticket that is valid
 * once, for 30 s, with the role of the token that asked; the WS handshake presents it as ?ticket=.
 * httpd runs every handler on one task, so the table needs no lock. */
#define RBAPI_WS_TICKETS     4
#define RBAPI_WS_TICKET_US   (30LL * 1000000LL)
typedef struct { char t[33]; int64_t exp_us; uint8_t role; } rbapi_ws_ticket_t;
static rbapi_ws_ticket_t s_ws_tickets[RBAPI_WS_TICKETS];

/* Consume a presented ?ticket=: returns its role, or RBAPI_ROLE_NONE (unknown / used / expired). */
static int rbapi_ws_ticket_consume(httpd_req_t* req)
{
    char q[160], v[40];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return 0;
    if (httpd_query_key_value(q, "ticket", v, sizeof(v)) != ESP_OK || strlen(v) != 32) return 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < RBAPI_WS_TICKETS; i++) {
        rbapi_ws_ticket_t* k = &s_ws_tickets[i];
        if (k->t[0] && now < k->exp_us && strcmp(k->t, v) == 0) {
            int role = k->role;
            memset(k, 0, sizeof(*k));   /* one-time */
            return role;
        }
    }
    return 0;
}

/* Role of this request. No admin token on flash = auth not armed → ADMIN (open/onboarding). */
static rbapi_role_t rbapi_request_role(httpd_req_t* req, bool* out_armed_read)
{
    char admin[48], rd[48];
    bool have_admin = rbgrid_token_load(RBGRID_TOKEN_FILE, admin);
    bool have_read  = rbgrid_token_load(RBGRID_TOKEN_READ_FILE, rd);
    if (out_armed_read) *out_armed_read = have_read;
    if (!have_admin) return RBAPI_ROLE_ADMIN;
    char got[96];
    rbapi_presented_token(req, got);
    if (got[0] && strcmp(got, admin) == 0) return RBAPI_ROLE_ADMIN;
    if (got[0] && have_read && strcmp(got, rd) == 0) return RBAPI_ROLE_READ;
    /* only a WebSocket handshake may present a ticket */
    if (strncmp(req->uri, "/ws-telemetry", 13) == 0) {
        int tr = rbapi_ws_ticket_consume(req);
        if (tr) return (rbapi_role_t)tr;
    }
    return RBAPI_ROLE_NONE;
}

/* Failed-auth rate limiter (global, not per client IP). */
static uint32_t s_auth_fail_count = 0;
static int64_t  s_auth_fail_window_us = 0;
static bool rbapi_auth_fail_limited(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_auth_fail_window_us > 60000000LL) {   /* 60 s window */
        s_auth_fail_window_us = now;
        s_auth_fail_count = 0;
    }
    s_auth_fail_count++;
    return s_auth_fail_count > 10;
}

static void rbapi_send_401(httpd_req_t* req)
{
    if (rbapi_auth_fail_limited()) {
        ESP_LOGW(TAG, "429 auth rate-limited: %s", req->uri);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_hdr(req, "Retry-After", "60");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"too_many_requests\"}", HTTPD_RESP_USE_STRLEN);
        return;
    }
    ESP_LOGW(TAG, "401 unauthorized: %s", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"unauthorized\",\"hint\":\"Authorization: Bearer <token>\"}", HTTPD_RESP_USE_STRLEN);
}

/* Write gate (ADMIN). True → proceed; false → 401/403/429 already sent (handler returns ESP_FAIL).
 * OPTIONS always proceeds (CORS preflight carries no credentials). */
bool rbapi_require_auth(httpd_req_t* req)
{
    if (req->method == HTTP_OPTIONS) return true;
    rbapi_role_t role = rbapi_request_role(req, NULL);
    if (role == RBAPI_ROLE_ADMIN) return true;
    if (role == RBAPI_ROLE_READ) {   /* authenticated but insufficient → 403 */
        ESP_LOGW(TAG, "403 forbidden (read-role on write): %s", req->uri);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"forbidden\",\"need\":\"admin\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    rbapi_send_401(req);
    return false;
}

/* Data-read gate (READ or ADMIN). Armed only when a READ token is provisioned — without one,
 * reads stay open even with an admin token set (write-protection-only mode). */
bool rbapi_require_read(httpd_req_t* req)
{
    if (req->method == HTTP_OPTIONS) return true;
    bool armed_read = false;
    rbapi_role_t role = rbapi_request_role(req, &armed_read);
    if (!armed_read || role >= RBAPI_ROLE_READ) return true;
    rbapi_send_401(req);
    return false;
}

/* Public wrappers so app-layer handlers (main/) + the WS handshake reuse the same guards. */
bool rbwebserver_api_check_auth(httpd_req_t* req) { return rbapi_require_auth(req); }
/**
 * @brief Report whether API auth is armed (an admin token is provisioned).
 * @return true if an admin token exists; false = onboarding state, every write is open.
 */
bool rbwebserver_api_token_armed(void) { char t[48]; return rbgrid_token_load(RBGRID_TOKEN_FILE, t); }
bool rbwebserver_api_check_read(httpd_req_t* req) { return rbapi_require_read(req); }

#ifdef RBGRID_FAULT_INJECT
#include "rbfault.h"
/* TEST BUILDS ONLY: POST /api/debug/fault?point=flash_write|lfs_sync|lfs_mount_io&count=N (admin). */
esp_err_t rbwebserver_api_fault_handler(httpd_req_t* req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    char q[96], pt[24] = {0}, cn[12] = {0}, sk[12] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "point", pt, sizeof(pt));
        httpd_query_key_value(q, "count", cn, sizeof(cn));
        httpd_query_key_value(q, "skip", sk, sizeof(sk));
    }
    int p = rbfault_from_name(pt);
    if (p < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "point: flash_write|lfs_sync|lfs_mount_io|espnow_ack_drop|gap_plus_day"); return ESP_FAIL; }
    uint32_t n = cn[0] ? (uint32_t)strtoul(cn, NULL, 10) : 1;
    rbfault_arm_skip((rbfault_point_t)p, n, sk[0] ? (uint32_t)strtoul(sk, NULL, 10) : 0);
    rbpower_diag_printf("fault: armed %s x%lu", pt, (unsigned long)n);
    char out[80]; snprintf(out, sizeof(out), "{\"status\":\"OK\",\"point\":\"%s\",\"count\":%lu}", pt, (unsigned long)n);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}
#endif

/* POST /api/v1/ws-ticket → {"ticket":"<32 hex>","expires_s":30}. Needs a READ or ADMIN token when reads are
 * gated (open otherwise, like every read). The ticket opens one /ws-telemetry handshake with the caller's role. */
esp_err_t rbwebserver_api_ws_ticket_handler(httpd_req_t* req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) { httpd_resp_send(req, NULL, 0); return ESP_OK; }
    if (!rbapi_require_read(req)) return ESP_FAIL;
    rbapi_role_t role = rbapi_request_role(req, NULL);
    if (role == RBAPI_ROLE_NONE) role = RBAPI_ROLE_READ;   /* reads not gated: the ticket carries read access */
    int64_t now = esp_timer_get_time();
    int slot = 0;   /* a free or expired slot, else the one expiring soonest */
    for (int i = 0; i < RBAPI_WS_TICKETS; i++) {
        if (!s_ws_tickets[i].t[0] || now >= s_ws_tickets[i].exp_us) { slot = i; break; }
        if (s_ws_tickets[i].exp_us < s_ws_tickets[slot].exp_us) slot = i;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    rbapi_ws_ticket_t* k = &s_ws_tickets[slot];
    for (int i = 0; i < 16; i++) snprintf(&k->t[i * 2], 3, "%02x", rnd[i]);
    k->exp_us = now + RBAPI_WS_TICKET_US;
    k->role = (uint8_t)role;
    char out[96];
    snprintf(out, sizeof(out), "{\"ticket\":\"%s\",\"expires_s\":30}", k->t);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

void rbapi_send_range_error(httpd_req_t *req, const char *field, const char *msg);

// System configuration handler

esp_err_t rbwebserver_api_system_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "   ==== GET 2 /api/system/system-config request");
    add_cors_headers(req);
    
    if (rbgrid_core_ext_cfg()) {
        ESP_LOGI(TAG, "   ==== GET 2 rbgrid_core_ext_cfg() OK");
        char* json_string = rbgrid_json_create_system_config(rbgrid_core_ext_cfg());
        if (json_string) {
            ESP_LOGI(TAG, "   ==== GET 2 json_string OK");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_string, strlen(json_string));
            free(json_string);
            return ESP_OK;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create system config");
    return ESP_FAIL;
}

// API hardware-config handler
esp_err_t rbwebserver_api_hardware_config_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "GET /api/system/hardware-config request");
        
        if (rbgrid_core_ext_cfg() && rbgrid_core_ext_cfg()->base_hardware_config && rbgrid_core_json_ctx()) {
            char* json_string = rbgrid_json_create_hardware_config(rbgrid_core_ext_cfg()->base_hardware_config, rbgrid_core_json_ctx());
            if (json_string) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, json_string, strlen(json_string));
                free(json_string);
                return ESP_OK;
            }
        }
        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create hardware config");
        return ESP_FAIL;
        
    } else if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // admin token required (if provisioned)
        ESP_LOGI(TAG, "POST /api/system/hardware-config request");

        // Allocate buffer for JSON data
        char* buf = malloc(4096);  // Larger buffer for hardware config
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for JSON buffer");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        int remaining = req->content_len;
        
        if (remaining > 4095) {
            ESP_LOGE(TAG, "Content too long: %d bytes", remaining);
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
            return ESP_FAIL;
        }
        
        int ret = rbapi_recv_body(req, buf, remaining);
        if (ret <= 0) {
            ESP_LOGE(TAG, "Failed to receive data");
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        
        ESP_LOGI(TAG, "Received JSON (%d bytes): %s", ret, buf);
        
        // Parse the received JSON
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            
            // Create error response JSON
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddStringToObject(error_response, "status", "ERROR");
                cJSON_AddStringToObject(error_response, "message", "Invalid JSON format");
                cJSON_AddStringToObject(error_response, "error_code", "INVALID_JSON");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
                }
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            }
            return ESP_FAIL;
        }
        
        // Create local config context
        rbpower_hardware_config_t* config = calloc(1, sizeof(rbpower_hardware_config_t));
        if (!config) {
            ESP_LOGE(TAG, "Failed to allocate memory for local config");
            cJSON_Delete(root);
            
            // Create error response JSON
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddStringToObject(error_response, "status", "ERROR");
                cJSON_AddStringToObject(error_response, "message", "Memory allocation failed");
                cJSON_AddStringToObject(error_response, "error_code", "MEMORY_ALLOCATION_FAILED");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_500);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                } else {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
                }
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            }
            return ESP_FAIL;
        }
        
        // Check if this is a hardware configuration JSON by checking the "type" field.
        // Accept BOTH envelopes: "hardware" (legacy file/save form) and "hardware_config" (what
        // our own GET emits) — otherwise a GET→POST round-trip was impossible.
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!type || !cJSON_IsString(type) ||
            (strcmp(type->valuestring, "hardware") != 0 && strcmp(type->valuestring, "hardware_config") != 0)) {
            ESP_LOGE(TAG, "Invalid JSON type - expected 'hardware'/'hardware_config', got: %s",
                     type ? (cJSON_IsString(type) ? type->valuestring : "non-string") : "null");
            cJSON_Delete(root);
            free(config);
            
            // Create error response JSON
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddStringToObject(error_response, "status", "ERROR");
                cJSON_AddStringToObject(error_response, "message", "Invalid JSON type - expected 'hardware'");
                cJSON_AddStringToObject(error_response, "error_code", "INVALID_TYPE");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON type");
                }
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON type");
            }
            return ESP_FAIL;
        }
        
        // Parse data section if it exists
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (data) {
            {   /* numeric ranges (schema limits.ranges): reject BEFORE anything is parsed or saved */
                char rf[48], rm[128];
                if (!rbgrid_config_check_ranges("hardware", data, rf, sizeof(rf), rm, sizeof(rm))) {
                    ESP_LOGE(TAG, "hardware config rejected: %s", rm);
                    cJSON_Delete(root);
                    free(config);
                    rbapi_send_range_error(req, rf, rm);
                    return ESP_FAIL;
                }
            }
            // Try to parse hardware configuration from data
            rbgrid_json_err_t json_ret = rbgrid_json_parse_hardware_section(data, config);
            if (json_ret != RBGRID_JSON_OK) {
                ESP_LOGE(TAG, "Failed to parse hardware configuration: %d", json_ret);
                cJSON_Delete(root);
                free(config);
                
                // Create error response JSON
                cJSON* error_response = cJSON_CreateObject();
                if (error_response) {
                    cJSON_AddStringToObject(error_response, "status", "ERROR");
                    cJSON_AddStringToObject(error_response, "message", "Failed to parse hardware configuration");
                    cJSON_AddStringToObject(error_response, "error_code", "PARSE_ERROR");
                    
                    char* error_str = cJSON_PrintUnformatted(error_response);
                    cJSON_Delete(error_response);
                    
                    if (error_str) {
                        httpd_resp_set_type(req, "application/json");
                        httpd_resp_set_status(req, HTTPD_400);
                        httpd_resp_send(req, error_str, strlen(error_str));
                        free(error_str);
                    } else {
                        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse hardware configuration");
                    }
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse hardware configuration");
                }
                return ESP_FAIL;
            }
        } else {
            /* saving an all-zero hardware config bricks the measurement setup at the next boot */
            ESP_LOGW(TAG, "hardware config POST without a data section — refused");
            cJSON_Delete(root);
            free(config);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hardware config: 'data' section required");
            return ESP_FAIL;
        }
        
        // Save configuration to storage
        esp_err_t save_ret = rbgrid_storage_save_hardware(config);
        
        // Clean up local config
        free(config);
        cJSON_Delete(root);
        
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save hardware configuration: %s", esp_err_to_name(save_ret));
            
            // Create error response JSON
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddStringToObject(error_response, "status", "ERROR");
                cJSON_AddStringToObject(error_response, "message", "Failed to save configuration");
                cJSON_AddStringToObject(error_response, "error_code", "SAVE_ERROR");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_500);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                } else {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
                }
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
            }
            return ESP_FAIL;
        }
        
        ESP_LOGI(TAG, "Hardware configuration saved successfully");
        
        // Create success response JSON
        cJSON* success_response = cJSON_CreateObject();
        if (success_response) {
            cJSON_AddStringToObject(success_response, "status", "OK");
            cJSON_AddStringToObject(success_response, "message", "Hardware configuration saved successfully");
            cJSON_AddBoolToObject(success_response, "requires_reboot", true);   // applies on restart (see /schema)

            char* success_str = cJSON_PrintUnformatted(success_response);
            cJSON_Delete(success_response);
            
            if (success_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, HTTPD_200);
                httpd_resp_send(req, success_str, strlen(success_str));
                free(success_str);
            } else {
                httpd_resp_send(req, "OK", 2);
            }
        } else {
            httpd_resp_send(req, "OK", 2);
        }
        
        return ESP_OK;
    }
    
    // Method not allowed
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

// API time-config handler
esp_err_t rbwebserver_api_time_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/system/time-config request");
    add_cors_headers(req);
    
    if (rbgrid_core_ext_cfg() && rbgrid_core_ext_cfg()->base_hardware_config && rbgrid_core_json_ctx()) {
        char* json_string = rbgrid_json_create_time_config(rbgrid_core_ext_cfg()->base_hardware_config, rbgrid_core_json_ctx());
        if (json_string) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_string, strlen(json_string));
            free(json_string);
            return ESP_OK;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create time config");
    return ESP_FAIL;
}

// API tariff-plan-config handler
esp_err_t rbwebserver_api_tariff_plan_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "%s /api/system/tariff-plan-config request", req->method == HTTP_GET ? "GET" : "POST");

    /* the shared CORS policy (origin allow-list, Authorization / X-FW-*, PNA) — the hand-set "*" + "Content-Type,
     * Accept" failed the preflight of a browser POST with a token */
    add_cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");

    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Handle GET request
    if (req->method == HTTP_GET) {
        if (rbgrid_core_json_ctx()) {
            char* json_string = rbgrid_json_create_rbpower_tariff_plan_config(rbgrid_core_json_ctx());
            if (json_string) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, json_string, strlen(json_string));
                free(json_string);
                return ESP_OK;
            }
        }

        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create tariff plan config");
        return ESP_FAIL;
    }

    // Handle POST request
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // admin token required (if provisioned)
        // Get request body size
        size_t content_length = req->content_len;
        if (content_length > CONFIG_COMPACT_SIZE) {
            ESP_LOGE(TAG, "Request body too large: %d bytes (max: %d)", content_length, CONFIG_COMPACT_SIZE);

            // Form JSON error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "validation_failed");
            cJSON_AddStringToObject(response, "message", "Request body too large");

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        // Allocate memory for request body
        char* content = calloc(1, content_length + 1);
        if (!content) {
            ESP_LOGE(TAG, "Failed to allocate memory for request body");

            // Form JSON error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "internal_error");
            cJSON_AddStringToObject(response, "message", "Memory allocation failed");

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        // Read request body
        int received = rbapi_recv_body(req, content, content_length);
        if (received <= 0) {
            ESP_LOGE(TAG, "Failed to receive request body");
            free(content);

            // Form JSON error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "internal_error");
            cJSON_AddStringToObject(response, "message", "Failed to read request body");

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        // Output received JSON to log for debugging
        ESP_LOGI(TAG, "Received POST data (%d bytes):", received);
        ESP_LOGI(TAG, "%s", content);

        // Parse JSON
        cJSON* json_req = cJSON_Parse(content);
        free(content);

        if (!json_req) {
            ESP_LOGE(TAG, "Failed to parse JSON request");
            const char* error_ptr = cJSON_GetErrorPtr();
            if (error_ptr) {
                ESP_LOGE(TAG, "JSON error before: %s", error_ptr);
            }

            // Form JSON error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "validation_failed");
            cJSON_AddStringToObject(response, "message", "Invalid JSON format");

            cJSON* details = cJSON_CreateArray();
            if (error_ptr) {
                cJSON_AddItemToArray(details, cJSON_CreateString("JSON parsing error"));
            }
            cJSON_AddItemToObject(response, "details", details);

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        // Check configuration type
        cJSON* type_json = cJSON_GetObjectItem(json_req, "type");
        if (!type_json || !cJSON_IsString(type_json) ||
            strcmp(cJSON_GetStringValue(type_json), "tariff_plan_list_config") != 0) {

            ESP_LOGE(TAG, "Invalid JSON type - expected 'tariff_plan_list_config', got '%s'",
                    type_json ? cJSON_GetStringValue(type_json) : "null");

            cJSON_Delete(json_req);

            // Form JSON validation error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "validation_failed");
            cJSON_AddStringToObject(response, "message", "Invalid tariff configuration");

            cJSON* details = cJSON_CreateArray();
            cJSON_AddItemToArray(details, cJSON_CreateString("Invalid type - expected 'tariff_plan_list_config'"));
            cJSON_AddItemToObject(response, "details", details);

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        // Get plan name for response
        cJSON* data_json = cJSON_GetObjectItem(json_req, "data");
        cJSON* list_json = data_json ? cJSON_GetObjectItem(data_json, "list") : NULL;
        cJSON* first_plan = cJSON_IsArray(list_json) ? cJSON_GetArrayItem(list_json, 0) : NULL;
        cJSON* plan_name_json = first_plan ? cJSON_GetObjectItem(first_plan, "plan_name") : NULL;
        /* Copy the name: the response below is built AFTER cJSON_Delete(json_req) — the old pointer
         * into the freed tree produced garbage plan_name in the 200 body. */
        char plan_name[64] = "Unknown";
        if (plan_name_json && cJSON_GetStringValue(plan_name_json)) {
            strncpy(plan_name, cJSON_GetStringValue(plan_name_json), sizeof(plan_name) - 1);
            plan_name[sizeof(plan_name) - 1] = '\0';
        }

        // Get zone count
        cJSON* zones_json = first_plan ? cJSON_GetObjectItem(first_plan, "zones") : NULL;
        int zones_count = cJSON_IsArray(zones_json) ? cJSON_GetArraySize(zones_json) : 0;

        // Convert JSON back to string for saving
        char* json_to_save = cJSON_PrintUnformatted(json_req);
        cJSON_Delete(json_req);

        if (!json_to_save) {
            ESP_LOGE(TAG, "Failed to convert JSON to string");

            // Form JSON error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "internal_error");
            cJSON_AddStringToObject(response, "message", "Failed to process tariff configuration");

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        // Save configuration to Flash
        ESP_LOGI(TAG, "Saving tariff configuration to Flash...");
        esp_err_t save_ret = rbgrid_storage_save_tariffs(json_to_save);
        /* Apply LIVE after a successful save — same path the MQTT `set tariff` command uses
         * (rbgrid_json_parse_tariff_plan_config); HTTP used to be apply-on-reboot only, which made
         * GET answer with the old plan right after a 200. */
        if (save_ret == ESP_OK) {
            rbgrid_json_err_t ae = rbgrid_json_parse_tariff_plan_config(json_to_save);
            if (ae != RBGRID_JSON_OK) ESP_LOGW(TAG, "tariff-plan-config: saved but live-apply failed (%d) — applies on reboot", (int)ae);
        }
        free(json_to_save);

        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save tariff configuration: %s", esp_err_to_name(save_ret));

            // Form JSON error response
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "error", "internal_error");
            cJSON_AddStringToObject(response, "message", "Failed to save tariff configuration");

            char* json_response = cJSON_PrintUnformatted(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, json_response, strlen(json_response));

            free(json_response);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Tariff configuration saved successfully");

        // Form success response
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Tariff plan saved successfully");

        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "plan_name", plan_name);
        cJSON_AddNumberToObject(data, "saved_at", (double)time(NULL));
        cJSON_AddNumberToObject(data, "zones_count", zones_count);
        cJSON_AddItemToObject(response, "data", data);

        char* json_response = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_response, strlen(json_response));

        free(json_response);
        cJSON_Delete(response);
        return ESP_OK;
    }

    // Method not supported
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}


// API telemetry-config handler — GET (config + `profile`), POST {profile:"lean"|"full"}, OPTIONS.
// profile lean → the 3s live push drops lifetime/economics (CPU savings; fetch via /summary|/economics).
// Persisted to littlefs /tele_profile; applied at RUNTIME (rbgrid_telemetry_set_lean) — no reboot needed.
#define RBAPI_TELE_PROFILE_FILE "/tele_profile"   ///< littlefs file: persisted telemetry profile ("lean"/"full")
esp_err_t rbwebserver_api_telemetry_config_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // write requires the admin token (if provisioned)
        char body[128] = {0};
        int len = req->content_len;
        if (len <= 0 || len >= (int)sizeof(body)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
        int got = rbapi_recv_body(req, body, len);
        if (got <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        body[got] = 0;
        cJSON *j = cJSON_Parse(body);
        const char *prof = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "profile")) : NULL;
        cJSON *fr = j ? cJSON_GetObjectItem(j, "frame") : NULL;          /* telemetry frame version: 1|2 or "v1"|"v2" */
        int frame = -1;
        if (fr && cJSON_IsNumber(fr)) frame = fr->valueint;
        else if (fr && cJSON_IsString(fr)) frame = (!strcmp(fr->valuestring, "v2") || !strcmp(fr->valuestring, "2")) ? 2 : (!strcmp(fr->valuestring, "v1") || !strcmp(fr->valuestring, "1")) ? 1 : -1;
        if (fr && frame != 1 && frame != 2) { cJSON_Delete(j); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "frame = 1|2"); return ESP_FAIL; }
        bool lean = rbgrid_telemetry_get_lean();
        if (prof && !strcmp(prof, "lean")) lean = true;
        else if (prof && !strcmp(prof, "full")) lean = false;
        else if (prof || !fr) { if (j) cJSON_Delete(j); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile = lean|full and/or frame = 1|2"); return ESP_FAIL; }
        cJSON_Delete(j);
        if (prof) {
            rbgrid_telemetry_set_lean(lean);                                  // runtime apply (immediate)
            littlefs_raw_write(RBAPI_TELE_PROFILE_FILE, lean ? "lean" : "full", lean ? 5 : 5);   // persist
        }
        if (frame > 0) {
            rbgrid_telemetry_set_frame((uint8_t)frame);                     // global: MQTT + HA discovery keys
            littlefs_raw_write("/tele_frame", frame == 2 ? "2" : "1", 1);
            rbgrid_ha_discovery_request();                                  // value_templates follow the frame keys
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "OK");
        cJSON_AddStringToObject(resp, "profile", lean ? "lean" : "full");
        cJSON_AddNumberToObject(resp, "frame", rbgrid_telemetry_get_frame());
        cJSON_AddBoolToObject(resp, "applied", true);   // runtime — takes effect on the next 3s push
        char *s = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, s, strlen(s));
        free(s);
        return ESP_OK;
    }

    // GET — the existing telemetry config + the current profile (parse the builder output, add "profile").
    ESP_LOGI(TAG, "GET /api/system/telemetry-config request");
    if (rbgrid_core_ext_cfg() && rbgrid_core_json_ctx()) {
        char* json_string = rbgrid_json_create_telemetry_config(rbgrid_core_ext_cfg(), rbgrid_core_json_ctx());
        if (json_string) {
            cJSON *root = cJSON_Parse(json_string);
            free(json_string);
            if (root) {
                cJSON_AddStringToObject(root, "profile", rbgrid_telemetry_get_lean() ? "lean" : "full");
                cJSON_AddNumberToObject(root, "frame", rbgrid_telemetry_get_frame());   /* active global telemetry frame version */
                {   /* who is connected to /ws-telemetry and which frame each one gets */
                    extern void telemetry_ws_client_list(cJSON* arr);
                    cJSON* wsc = cJSON_AddArrayToObject(root, "ws_clients");
                    telemetry_ws_client_list(wsc);
                }
                char *s = cJSON_PrintUnformatted(root);
                cJSON_Delete(root);
                if (s) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_send(req, s, strlen(s));
                    free(s);
                    return ESP_OK;
                }
            }
        }
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create telemetry config");
    return ESP_FAIL;
}

// API wifi-config handler
esp_err_t rbwebserver_api_wifi_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/system/wifi-config request");
    add_cors_headers(req);
    
    if (rbgrid_core_json_ctx()) {
        char* json_string = rbgrid_json_create_wifi_config(rbgrid_core_json_ctx());
        if (json_string) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_string, strlen(json_string));
            free(json_string);
            return ESP_OK;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create wifi config");
    return ESP_FAIL;
}

// API units-config handler
esp_err_t rbwebserver_api_units_config_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    
    // Handle OPTIONS request for CORS preflight
    if (req->method == HTTP_OPTIONS) {
        ESP_LOGI(TAG, "OPTIONS /api/system/units-config request (CORS preflight)");
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "GET /api/system/units-config request");
        
        if (rbgrid_core_json_ctx()) {
            // Build under the g_home_network lock — this runs on the httpd task while the
            // core-0 / esp_timer tasks mutate the units array.
            rbpower_home_lock();
            char* json_string = rbgrid_json_create_units_config(rbgrid_core_json_ctx());
            rbpower_home_unlock();
            if (json_string) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, json_string, strlen(json_string));
                free(json_string);
                return ESP_OK;
            }
        }
        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create units config");
        return ESP_FAIL;
        
    } else if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // admin token required (if provisioned)
        ESP_LOGI(TAG, "POST /api/system/units-config request");

        // Allocate buffer for JSON data (units.json ceiling, PSRAM — 8 KB held only 6 units)
        char* buf = rbgrid_cfg_big_alloc(CONFIG_UNITS_SIZE);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for JSON buffer");
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Memory allocation failed");
                cJSON_AddStringToObject(error_response, "message", "Server out of memory");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_500);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        int remaining = req->content_len;
        
        if (remaining > CONFIG_UNITS_SIZE - 1) {
            ESP_LOGE(TAG, "Content too long: %d bytes", remaining);
            free(buf);
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Content too large");
                cJSON_AddStringToObject(error_response, "message", "Request body exceeds maximum size");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        int ret = rbapi_recv_body(req, buf, remaining);
        if (ret <= 0) {
            ESP_LOGE(TAG, "Failed to receive data");
            free(buf);
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Failed to receive data");
                cJSON_AddStringToObject(error_response, "message", "Could not read request body");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        
        ESP_LOGI(TAG, "Received JSON (%d bytes): %s", ret, buf);
        
        // Parse the received JSON
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Invalid JSON");
                cJSON_AddStringToObject(error_response, "message", "Request body is not valid JSON");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        // Validate JSON type
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "units_config") != 0) {
            ESP_LOGE(TAG, "Invalid JSON type - expected 'units_config'");
            cJSON_Delete(root);
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Validation error");
                cJSON_AddStringToObject(error_response, "message", "Invalid JSON type - expected 'units_config'");
                
                cJSON* details = cJSON_CreateArray();
                cJSON* detail = cJSON_CreateObject();
                cJSON_AddStringToObject(detail, "field", "type");
                cJSON_AddStringToObject(detail, "error", "Must be 'units_config'");
                cJSON_AddItemToArray(details, detail);
                cJSON_AddItemToObject(error_response, "details", details);
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        // Get data section
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (!data) {
            ESP_LOGE(TAG, "Missing 'data' section");
            cJSON_Delete(root);
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Validation error");
                cJSON_AddStringToObject(error_response, "message", "Missing 'data' section");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        // Get units array
        cJSON* units = cJSON_GetObjectItem(data, "units");
        if (!units || !cJSON_IsArray(units)) {
            ESP_LOGE(TAG, "Missing or invalid 'units' array");
            cJSON_Delete(root);
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Validation error");
                cJSON_AddStringToObject(error_response, "message", "Missing or invalid 'units' array");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        // Create validation errors array
        cJSON* validation_errors = cJSON_CreateArray();
        int units_count = cJSON_GetArraySize(units);
        bool has_errors = false;
        
        // Process and validate each unit
        cJSON* unit = NULL;
        int unit_index = 0;
        cJSON_ArrayForEach(unit, units) {
            unit_index++;
            
            // Validate name (required, max 32 chars)
            cJSON* name = cJSON_GetObjectItem(unit, "name");
            if (!name || !cJSON_IsString(name)) {
                cJSON* error = cJSON_CreateObject();
                cJSON_AddNumberToObject(error, "unit_id", unit_index);
                cJSON_AddStringToObject(error, "field", "name");
                cJSON_AddStringToObject(error, "error", "Name is required");
                cJSON_AddItemToArray(validation_errors, error);
                has_errors = true;
            } else if (strlen(name->valuestring) > 32) {
                cJSON* error = cJSON_CreateObject();
                cJSON_AddNumberToObject(error, "unit_id", unit_index);
                cJSON_AddStringToObject(error, "field", "name");
                cJSON_AddStringToObject(error, "error", "Name exceeds 32 characters");
                cJSON_AddItemToArray(validation_errors, error);
                has_errors = true;
            }
            
            // Validate location (max 16 chars)
            cJSON* location = cJSON_GetObjectItem(unit, "location");
            if (location && cJSON_IsString(location) && strlen(location->valuestring) > 16) {
                cJSON* error = cJSON_CreateObject();
                cJSON_AddNumberToObject(error, "unit_id", unit_index);
                cJSON_AddStringToObject(error, "field", "location");
                cJSON_AddStringToObject(error, "error", "Location exceeds 16 characters");
                cJSON_AddItemToArray(validation_errors, error);
                has_errors = true;
            }

            // short_name (optional, max 24 BYTES utf-8 — raised from 10)
            cJSON* short_name = cJSON_GetObjectItem(unit, "short_name");
            if (short_name && cJSON_IsString(short_name) && strlen(short_name->valuestring) > 24) {
                cJSON* error = cJSON_CreateObject();
                cJSON_AddNumberToObject(error, "unit_id", unit_index);
                cJSON_AddStringToObject(error, "field", "short_name");
                cJSON_AddStringToObject(error, "error", "short_name exceeds 24 bytes (utf-8)");
                cJSON_AddItemToArray(validation_errors, error);
                has_errors = true;
            }

            // phase (optional numeric: 0=unset, 1..3=L1..L3, 4=3P, 5..7=L1L2/L2L3/L3L1)
            cJSON* phase = cJSON_GetObjectItem(unit, "phase");
            if (phase && (!cJSON_IsNumber(phase) || phase->valueint < 0 || phase->valueint > 7)) {
                cJSON* error = cJSON_CreateObject();
                cJSON_AddNumberToObject(error, "unit_id", unit_index);
                cJSON_AddStringToObject(error, "field", "phase");
                cJSON_AddStringToObject(error, "error", "phase must be 0..7 (0=unset, 1..3=L1..L3, 4=3P, 5..7=L1L2/L2L3/L3L1)");
                cJSON_AddItemToArray(validation_errors, error);
                has_errors = true;
            }
            
            // Validate sources
            cJSON* sources = cJSON_GetObjectItem(unit, "sources");
            if (sources && cJSON_IsArray(sources)) {
                cJSON* source = NULL;
                cJSON_ArrayForEach(source, sources) {
                    cJSON* source_type = cJSON_GetObjectItem(source, "source_type");
                    cJSON* source_id = cJSON_GetObjectItem(source, "source_id");
                    
                    if (source_type && cJSON_IsString(source_type) && 
                        strcmp(source_type->valuestring, "ADC_SENSOR") == 0) {
                        if (source_id && cJSON_IsNumber(source_id)) {
                            int id = source_id->valueint;
                            if (id < 0 || id > 7) {
                                cJSON* error = cJSON_CreateObject();
                                cJSON_AddNumberToObject(error, "unit_id", unit_index);
                                cJSON_AddStringToObject(error, "field", "source_id");
                                char err_msg[64];
                                snprintf(err_msg, sizeof(err_msg), "ADC channel %d is out of range (0-7)", id);
                                cJSON_AddStringToObject(error, "error", err_msg);
                                cJSON_AddItemToArray(validation_errors, error);
                                has_errors = true;
                            }
                        }
                    }
                }
            }
            
            // Add default fields if validation passed for this unit
            if (!has_errors) {
                // Add sources_count
                cJSON* sources_count = cJSON_GetObjectItem(unit, "sources_count");
                if (!sources_count) {
                    int count = sources ? cJSON_GetArraySize(sources) : 0;
                    cJSON_AddNumberToObject(unit, "sources_count", count);
                }
                
                // Add parent_unit_id
                cJSON* parent_id = cJSON_GetObjectItem(unit, "parent_unit_id");
                if (!parent_id) {
                    cJSON_AddNumberToObject(unit, "parent_unit_id", 65535);
                }
                
                // Add child_units array
                cJSON* child_units = cJSON_GetObjectItem(unit, "child_units");
                if (!child_units) {
                    cJSON_AddItemToObject(unit, "child_units", cJSON_CreateArray());
                    cJSON_AddNumberToObject(unit, "children_count", 0);
                }
                
                // Add bidirectional_metering based on type and role
                cJSON* type_obj = cJSON_GetObjectItem(unit, "type");
                cJSON* role_obj = cJSON_GetObjectItem(unit, "role");
                bool bidirectional = false;
                
                if (type_obj && cJSON_IsString(type_obj) && 
                    role_obj && cJSON_IsString(role_obj)) {
                    if (strcmp(type_obj->valuestring, "CURRENT_LOAD") == 0 &&
                        (strcmp(role_obj->valuestring, "MAIN_SUPPLY") == 0 ||
                         strcmp(role_obj->valuestring, "ALTERNATIVE_SUPPLY") == 0)) {
                        bidirectional = true;
                    }
                }
                
                cJSON* bidir = cJSON_GetObjectItem(unit, "bidirectional_metering");
                if (!bidir) {
                    cJSON_AddBoolToObject(unit, "bidirectional_metering", bidirectional);
                }
                
                // Add is_active
                cJSON* is_active = cJSON_GetObjectItem(unit, "is_active");
                if (!is_active) {
                    cJSON_AddBoolToObject(unit, "is_active", true);
                }
            }
        }
        
        // Check if there were validation errors
        if (has_errors) {
            cJSON_Delete(root);
            
            // Send validation error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Validation error");
                cJSON_AddStringToObject(error_response, "message", "Invalid unit configuration");
                cJSON_AddItemToObject(error_response, "details", validation_errors);
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        cJSON_Delete(validation_errors);
        
        // Add units_count to root
        cJSON_AddNumberToObject(root, "units_count", units_count);
        
        /* LIVE-apply list for invert_sign (CT orientation): the full document is persisted below and
         * otherwise applies on reboot, but the sign flag is consumed at the inject boundary and must
         * follow the POST immediately (otherwise: POST 200, GET still false, and the rest of the
         * hour ran as import). Collected here (root is freed next), applied after a
         * successful save so live never runs ahead of the file. */
        struct { uint16_t uid; bool inv; } inv_list[RBPOWER_MAX_HOME_UNITS];
        int inv_n = 0;
        {
            cJSON* u_it = NULL;
            cJSON_ArrayForEach(u_it, units) {
                cJSON* uid_j = cJSON_GetObjectItem(u_it, "unit_id");
                cJSON* src_a = cJSON_GetObjectItem(u_it, "sources");
                if (!cJSON_IsNumber(uid_j) || !cJSON_IsArray(src_a) || cJSON_GetArraySize(src_a) == 0) continue;
                cJSON* inv_j = cJSON_GetObjectItem(cJSON_GetArrayItem(src_a, 0), "invert_sign");
                if (!cJSON_IsBool(inv_j) || inv_n >= RBPOWER_MAX_HOME_UNITS) continue;
                inv_list[inv_n].uid = (uint16_t)uid_j->valueint;
                inv_list[inv_n].inv = cJSON_IsTrue(inv_j);
                inv_n++;
            }
        }

        // Convert modified JSON to string for saving
        char* json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to create JSON string");
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Internal error");
                cJSON_AddStringToObject(error_response, "message", "Failed to process configuration");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_500);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        // Save configuration to storage
        ESP_LOGI(TAG, "Saving units configuration to storage...");
        esp_err_t save_ret = rbgrid_storage_save_units(json_str);
        free(json_str);
        
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save units configuration: %s", esp_err_to_name(save_ret));
            
            // Send error response
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddBoolToObject(error_response, "success", false);
                cJSON_AddStringToObject(error_response, "error", "Save failed");
                cJSON_AddStringToObject(error_response, "message", "Failed to save configuration to storage");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_500);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                }
            }
            return ESP_FAIL;
        }
        
        ESP_LOGI(TAG, "Units configuration saved successfully");

        /* Live-apply invert_sign (see inv_list above). Other fields keep apply-on-reboot semantics. */
        if (inv_n > 0) {
            int applied = 0;
            rbpower_home_lock();
            for (int k = 0; k < inv_n; k++) {
                rbpower_unit_definition_t* d = rbpower_home_get_unit_definition_mutable((rbpower_unit_id_t)inv_list[k].uid);
                if (!d) continue;
                if (d->sources_count == 0) d->sources_count = 1;
                if (d->sources[0].invert_sign != inv_list[k].inv) applied++;
                d->sources[0].invert_sign = inv_list[k].inv;
            }
            rbpower_home_unlock();
            ESP_LOGW(TAG, "units-config POST: invert_sign live-applied (%d changed of %d)", applied, inv_n);
        }
        
        // Create success response
        cJSON* success_response = cJSON_CreateObject();
        if (success_response) {
            cJSON_AddBoolToObject(success_response, "success", true);
            cJSON_AddStringToObject(success_response, "message", "Units configuration saved successfully");
            
            cJSON* response_data = cJSON_CreateObject();
            cJSON_AddNumberToObject(response_data, "units_saved", units_count);
            cJSON_AddNumberToObject(response_data, "active_units", units_count);
            cJSON_AddBoolToObject(response_data, "requires_reboot", true);
            
            // Add current timestamp
            uint64_t timestamp = esp_timer_get_time() / 1000000; // Convert to seconds
            cJSON_AddNumberToObject(response_data, "timestamp", timestamp);
            
            cJSON_AddItemToObject(success_response, "data", response_data);
            
            char* success_str = cJSON_PrintUnformatted(success_response);
            cJSON_Delete(success_response);
            
            if (success_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, HTTPD_200);
                httpd_resp_send(req, success_str, strlen(success_str));
                free(success_str);
            } else {
                httpd_resp_send(req, "{\"success\":true}", 16);
            }
        } else {
            httpd_resp_send(req, "{\"success\":true}", 16);
        }
        
        return ESP_OK;
    }
    
    // Method not allowed
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

/* ---- Points: create / change / delete. The logic lives in the core (rbgrid_units.c); these handlers only
 * check the token, read the URI and body, and send the core's reply with its status. ---- */

/** @brief Send a core reply (JSON body + HTTP-style status), then free the body. */
static esp_err_t rbapi_send_core_reply(httpd_req_t *req, int status, char *body)
{
    const char *st = status == 200 ? "200 OK" : status == 201 ? "201 Created" : status == 400 ? "400 Bad Request" :
                     status == 404 ? "404 Not Found" : status == 409 ? "409 Conflict" :
                     status == 507 ? "507 Insufficient Storage" : "500 Internal Server Error";
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_status(req, st);
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

/** @brief Read the whole request body into a malloc'd NUL-terminated buffer (max @p cap bytes). */
static char *rbapi_read_body(httpd_req_t *req, int cap, const char **err)
{
    *err = NULL;
    if (req->content_len <= 0 || req->content_len > cap) { *err = "{\"status\":\"error\",\"message\":\"empty or oversized body\"}"; return NULL; }
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int total = 0, remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf + total, remaining);
        if (r <= 0) { free(buf); *err = "{\"status\":\"error\",\"message\":\"body recv failed\"}"; return NULL; }
        total += r; remaining -= r;
    }
    buf[total] = '\0';
    return buf;
}

/* DELETE /api/v1/units/{hexid} */
esp_err_t rbwebserver_api_unit_delete_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;   // DELETE is a write: admin token required
    httpd_resp_set_type(req, "application/json");
    // Path is /api/v1/units/<hexid> — the id is the segment after the last '/'.
    const char* last = strrchr(req->uri, '/');
    rbpower_unit_id_t unit_id = 0;
    if (!last || last[1] == '\0' || sscanf(last + 1, "%hx", &unit_id) != 1) unit_id = 0;
    int st = 500;
    char *body = rbgrid_units_delete(unit_id, &st);
    return rbapi_send_core_reply(req, st, body);
}

/* POST /api/v1/units — bind a sensor to a NEW point. Body: {sensor:{iface,bus/module/ch|channel}, name, role?,
 * stat_class?, location?}. 201 {status,unit_id,point,persist}; 409 already bound; 400 bad body / sensor not found. */
esp_err_t rbwebserver_api_unit_create_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;   // POST is a write: admin token required
    httpd_resp_set_type(req, "application/json");
    const char *err = NULL;
    char *buf = rbapi_read_body(req, 1024, &err);
    if (!buf) {
        if (!err) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, err);
        return ESP_OK;
    }
    int st = 500;
    char *body = rbgrid_units_create(buf, &st);
    free(buf);
    return rbapi_send_core_reply(req, st, body);
}

/* POST /api/v1/units/{hex} (params patch), /api/v1/units/{hex}/rebind {module,ch}, /api/v1/units/{hex}/rename
 * {point,reuse?}: one handler, the URI tail picks the op; persists units.json. */
esp_err_t rbwebserver_api_unit_mutate_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    unsigned uid_u = 0;
    char op[12] = {0};
    char upath[96];   /* without the query: "/rename?token=…" parsed as op "rename?toke" */
    snprintf(upath, sizeof(upath), "%.*s", (int)strcspn(req->uri, "?"), req->uri);
    int nmatch = sscanf(upath, "/api/v1/units/%4x/%11s", &uid_u, op);
    if (nmatch < 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"bad uri — /api/v1/units/{hex}[/rebind|/rename]\"}");
        return ESP_OK;
    }
    rbpower_unit_id_t uid = (rbpower_unit_id_t)uid_u;
    if (!rbpower_home_get_unit_definition(uid)) {   /* unknown point first, as before the body checks */
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"unit not found\"}");
        return ESP_OK;
    }
    const char *err = NULL;
    char *buf = rbapi_read_body(req, 512, &err);
    if (!buf) {
        if (!err) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, err);
        return ESP_OK;
    }
    int st = 500;
    char *body = rbgrid_units_mutate(uid, nmatch >= 2 ? op : "", buf, &st);
    free(buf);
    return rbapi_send_core_reply(req, st, body);
}


/* GET /api/v1/sensors — bound-sensor discovery: list configured sensors
 * with iface/present/unit_id + addressing, so the frontend shows what hardware is detected. */
esp_err_t rbwebserver_api_sensors_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    /* Readiness gate: before the home network finishes init (~16 s after reboot) the
     * unit list is empty — an empty `sensors` array is indistinguishable from "no sensors". Return
     * 503 so the client retries instead of rendering an empty device list. */
    if (!rbpower_home_is_initialized()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "5");   /* machine-readable retry hint */
        httpd_resp_sendstr(req, "{\"status\":\"initializing\",\"message\":\"home network not ready — retry\"}");
        return ESP_OK;
    }
    rbpower_home_lock();   // consistent read of g_home_network vs the core-0/esp_timer mutators
    char* json = rbgrid_json_create_sensors_list(NULL);
    rbpower_home_unlock();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sensors list build failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* fwd decl — defined later (used by the type=all / from-to history branch). */
static cJSON *rbapi_history_filtered_array(rbpower_unit_id_t unit, rbpower_period_t period,
                                           rbpower_data_type_t type, uint32_t from, uint32_t to, uint32_t limit);
static esp_err_t rbapi_input_history_reply(httpd_req_t *req, const char *period_str, rbpower_period_t period,
                                           const char *type_str, bool type_all, rbpower_data_type_t data_type,
                                           uint32_t offset, uint32_t limit, const char *from_str, const char *to_str,
                                           const char *since_str);

esp_err_t rbwebserver_api_history_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/v1/history request");
    add_cors_headers(req);
    if (!rbapi_require_read(req)) return ESP_FAIL;   /* data read — READ role when armed */

    // Deferred format migration in progress: the archive is temporarily offline —
    // answer 503 + Retry-After, NOT an empty array (an empty array reads as "no history exists";
    // the client shows "updating the archive").
    if (rbstats_migrate_state() != RBSTATS_STORE_READY) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "30");
        httpd_resp_set_type(req, "application/json");
        char b[96];
        snprintf(b, sizeof(b), "{\"error\":\"not_ready\",\"storage_state\":\"%s\",\"progress\":%u}",
                 rbstats_store_state_str(), (unsigned)rbstats_migrate_progress());
        httpd_resp_sendstr(req, b);
        return ESP_OK;
    }

    // 1. Parse query string
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len <= 1) {
        ESP_LOGW(TAG, "No query parameters");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query parameters");
        return ESP_FAIL;
    }

    char* query_buf = malloc(buf_len);
    if (!query_buf) {
        ESP_LOGE(TAG, "Failed to allocate query buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, query_buf, buf_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get query string");
        free(query_buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Query string: %s", query_buf);

    // 2. Extract parameters
    char unit_id_str[16] = {0};
    char period_str[16] = {0};
    char type_str[16] = {0};
    char offset_str[16] = {0};
    char limit_str[16] = {0};
    char from_str[16] = {0};   // optional unix-sec time window
    char to_str[16] = {0};
    char since_str[16] = {0};  // since-cursor incremental sync (API contract v4)

    httpd_query_key_value(query_buf, "unit_id", unit_id_str, sizeof(unit_id_str));
    // Uniform v1-read surface: economics/summary accept both `unit` and `unit_id`; history now too
    // (harmonized — `unit_id` stays primary, existing clients are unaffected).
    if (!unit_id_str[0]) httpd_query_key_value(query_buf, "unit", unit_id_str, sizeof(unit_id_str));
    httpd_query_key_value(query_buf, "period", period_str, sizeof(period_str));
    httpd_query_key_value(query_buf, "type", type_str, sizeof(type_str));
    httpd_query_key_value(query_buf, "offset", offset_str, sizeof(offset_str));
    httpd_query_key_value(query_buf, "limit", limit_str, sizeof(limit_str));
    httpd_query_key_value(query_buf, "from", from_str, sizeof(from_str));
    httpd_query_key_value(query_buf, "to", to_str, sizeof(to_str));
    httpd_query_key_value(query_buf, "since", since_str, sizeof(since_str));

    free(query_buf);

    // 3. Validate required parameters
    if (strlen(unit_id_str) == 0 || strlen(period_str) == 0 || strlen(type_str) == 0) {
        ESP_LOGW(TAG, "Missing required parameters");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Required parameters: unit_id, period, type");
        return ESP_FAIL;
    }

    // 4. Convert unit_id from hex string ("input" = the grid input as one series, handled after the period/type parse)
    rbpower_unit_id_t unit_id = 0;
    bool unit_input = (strcmp(unit_id_str, "input") == 0);
    if (!unit_input && sscanf(unit_id_str, "%hx", &unit_id) != 1) {
        ESP_LOGW(TAG, "Invalid unit_id format: %s", unit_id_str);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid unit_id format (expected hex)");
        return ESP_FAIL;
    }

    // 5. Convert period
    rbpower_period_t period;
    if (strcmp(period_str, "hour") == 0) {
        period = RBPOWER_PERIOD_HOUR;
    } else if (strcmp(period_str, "day") == 0) {
        period = RBPOWER_PERIOD_DAY;
    } else if (strcmp(period_str, "week") == 0) {
        period = RBPOWER_PERIOD_WEEK;
    } else if (strcmp(period_str, "month") == 0) {
        period = RBPOWER_PERIOD_MONTH;
    } else if (strcmp(period_str, "year") == 0) {
        period = RBPOWER_PERIOD_YEAR;
    } else {
        ESP_LOGW(TAG, "Invalid period: %s", period_str);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid period (hour, day, week, month, year)");
        return ESP_FAIL;
    }

    // 6. Convert type ("all" = multi-type combine, handled in the branch below)
    rbpower_data_type_t data_type = RBPOWER_DATA_TYPE_ELECTRICAL;
    bool type_all = (strcmp(type_str, "all") == 0);
    if (type_all) {
        /* combined below */
    } else if (strcmp(type_str, "electrical") == 0) {
        data_type = RBPOWER_DATA_TYPE_ELECTRICAL;
    } else if (strcmp(type_str, "energy") == 0) {
        data_type = RBPOWER_DATA_TYPE_ENERGY;
    } else if (strcmp(type_str, "economics") == 0) {
        data_type = RBPOWER_DATA_TYPE_ECONOMICS;
    } else {
        // QUALITY/DIRECTION removed — no ZC/THD source in the rbAmp fleet (THD lives in electrical).
        ESP_LOGW(TAG, "Invalid type: %s", type_str);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type (electrical, energy, economics, all)");
        return ESP_FAIL;
    }

    // 7. Convert offset and limit (with defaults)
    uint32_t offset = (strlen(offset_str) > 0) ? (uint32_t)atoi(offset_str) : 0;
    uint32_t limit = (strlen(limit_str) > 0) ? (uint32_t)atoi(limit_str) : 100;

    // Limit bounds check
    if (limit > 720) {
        limit = 720;
    }
    if (limit == 0) {
        limit = 100;
    }

    ESP_LOGI(TAG, "Parsed: unit=0x%04X, period=%s, type=%s, offset=%lu, limit=%lu",
             unit_id, period_str, type_str, (unsigned long)offset, (unsigned long)limit);

    if (unit_input)
        return rbapi_input_history_reply(req, period_str, period, type_str, type_all, data_type, offset, limit,
                                         from_str, to_str, since_str);

    // 8b. since-cursor (API contract v4): strictly ts > since, ASCENDING, up to `limit`,
    // + next_since/has_more. Keyed on ts (not offsets) → stable across ring wrap. ts==0 records
    // (unsynced shelf, v4) are NEVER part of the since-stream. Single-type only (contract).
    // Scan: page the validated per-type JSON newest-first; a ring of `limit` kept duplicates ends
    // holding the OLDEST matched window — exactly the ascending page the client asked for.
    if (since_str[0] && !type_all) {
        uint32_t since = (uint32_t)strtoul(since_str, NULL, 10);
        cJSON **kept = calloc(limit, sizeof(cJSON *));
        if (!kept) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_FAIL; }
        uint32_t matched = 0;
        const uint32_t PAGE = 128;
        for (uint32_t off = 0;; off += PAGE) {
            char *js = rbgrid_history_get_data(unit_id, period, data_type, off, PAGE);
            if (!js) break;
            cJSON *r = cJSON_Parse(js);
            free(js);
            if (!r) break;
            cJSON *data = cJSON_GetObjectItem(r, "data");
            int page_n = 0;
            cJSON *rec = NULL;
            if (cJSON_IsArray(data)) {
                cJSON_ArrayForEach(rec, data) {
                    page_n++;
                    cJSON *tsj = cJSON_GetObjectItem(rec, "ts");
                    uint32_t ts = tsj ? (uint32_t)cJSON_GetNumberValue(tsj) : 0;
                    if (ts == 0 || ts <= since || ts < RBGRID_TS_SANE_MIN) continue;
                    uint32_t slot = matched % limit;
                    if (kept[slot]) cJSON_Delete(kept[slot]);
                    kept[slot] = cJSON_Duplicate(rec, 1);
                    matched++;
                }
            }
            cJSON_Delete(r);
            if ((uint32_t)page_n < PAGE) break;   /* ring exhausted */
        }
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        uint32_t emitted = (matched < limit) ? matched : limit;
        uint32_t next_since = since;
        /* insertion order is newest→oldest; the kept window = insertions [matched-emitted .. matched-1];
         * ascending output walks insertions DOWN from matched-1 (oldest) to matched-emitted (newest). */
        for (uint32_t j = 0; j < emitted; j++) {
            uint32_t ins = matched - 1 - j;
            uint32_t slot = ins % limit;
            if (!kept[slot]) continue;
            cJSON *tsj = cJSON_GetObjectItem(kept[slot], "ts");
            if (tsj) next_since = (uint32_t)cJSON_GetNumberValue(tsj);
            cJSON_AddItemToArray(arr, kept[slot]);
            kept[slot] = NULL;
        }
        for (uint32_t j = 0; j < limit; j++) {
            if (kept[j]) cJSON_Delete(kept[j]);
        }
        free(kept);
        char uidhex[8];
        snprintf(uidhex, sizeof(uidhex), "%04X", (unsigned)unit_id);
        cJSON_AddStringToObject(root, "unit", uidhex);
        cJSON_AddStringToObject(root, "period", period_str);
        cJSON_AddStringToObject(root, "type", type_str);
        cJSON_AddNumberToObject(root, "schema_ver", 4);   /* v4 record forms (dur, q, res, et_in/et_out, plan_id/plan_ver) */
        cJSON_AddItemToObject(root, "data", arr);
        cJSON_AddNumberToObject(root, "next_since", next_since);
        cJSON_AddBoolToObject(root, "has_more", matched > limit);
        cJSON_AddNumberToObject(root, "total_available", matched);
        char *out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, strlen(out));
        free(out);
        return ESP_OK;
    }

    // 8a. Additive path for type=all and/or from/to. Reuses the validated per-type builder
    // (electrical envelope preserved), then windows by ts. Default (single type, no window) is UNCHANGED.
    if (type_all || from_str[0] || to_str[0]) {
        uint32_t from = from_str[0] ? (uint32_t)strtoul(from_str, NULL, 10) : 0;
        uint32_t to   = to_str[0]   ? (uint32_t)strtoul(to_str,   NULL, 10) : 0xFFFFFFFFu;
        uint32_t lim  = (from_str[0] || to_str[0]) ? 720 : limit;   // window: scan max, then filter
        cJSON *root = cJSON_CreateObject();
        if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
        cJSON_AddNumberToObject(root, "v", 1);
        char uidhex[8]; snprintf(uidhex, sizeof(uidhex), "%04X", (unsigned)unit_id);
        cJSON_AddStringToObject(root, "unit", uidhex);
        cJSON_AddStringToObject(root, "period", period_str);
        if (type_all) {
            cJSON *d = cJSON_AddObjectToObject(root, "data");
            cJSON_AddItemToObject(d, "electrical", rbapi_history_filtered_array(unit_id, period, RBPOWER_DATA_TYPE_ELECTRICAL, from, to, lim));
            cJSON_AddItemToObject(d, "energy",     rbapi_history_filtered_array(unit_id, period, RBPOWER_DATA_TYPE_ENERGY, from, to, lim));
            cJSON_AddItemToObject(d, "economics",  rbapi_history_filtered_array(unit_id, period, RBPOWER_DATA_TYPE_ECONOMICS, from, to, lim));
        } else {
            cJSON_AddItemToObject(root, "data", rbapi_history_filtered_array(unit_id, period, data_type, from, to, lim));
        }
        char *out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, strlen(out));
        free(out);
        return ESP_OK;
    }

    // 8. Call API function (default path — unchanged)
    if (rbpower_home_get_unit_definition(unit_id) == NULL) {   /* unknown unit → 404, not 500 */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown unit");
        return ESP_FAIL;
    }
    char* json_response = rbgrid_history_get_data(unit_id, period, data_type, offset, limit);

    if (!json_response) {
        ESP_LOGE(TAG, "Failed to get history data");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read history data");
        return ESP_FAIL;
    }
    /* fake-clock records (0 < ts < 2025: Jan-2024 fallback) are not history — drop them here too; this
     * default path used to return them (e.g. ts=1704063689 at the top of history).
     * ts == 0 (unsynced shelf) keeps its documented meaning and stays. */
    if (strstr(json_response, "\"ts\":")) {
        cJSON *r = cJSON_Parse(json_response);
        cJSON *data = r ? cJSON_GetObjectItem(r, "data") : NULL;
        if (cJSON_IsArray(data)) {
            bool dropped = false;
            for (int i = cJSON_GetArraySize(data) - 1; i >= 0; --i) {
                cJSON *tsj = cJSON_GetObjectItem(cJSON_GetArrayItem(data, i), "ts");
                uint32_t ts = tsj ? (uint32_t)cJSON_GetNumberValue(tsj) : 0;
                if (ts != 0 && ts < RBGRID_TS_SANE_MIN) { cJSON_DeleteItemFromArray(data, i); dropped = true; }
            }
            if (dropped) {
                char *re = cJSON_PrintUnformatted(r);
                if (re) { free(json_response); json_response = re; }
            }
        }
        cJSON_Delete(r);
    }

    // 9. Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));

    // 10. Cleanup
    free(json_response);

    ESP_LOGI(TAG, "History API request completed successfully");
    return ESP_OK;
}


#define RBAPI_INPUT_MAX 8   ///< Max points of the grid input (main + one per further phase)
static void rbapi_add_input_units(cJSON *root, const rbpower_unit_id_t *ids, uint8_t n);

/* Add the numbers of object @p src into @p dst by key (counts and money of the per-point economics JSON). */
static void rbapi_obj_sum(cJSON *dst, const cJSON *src)
{
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, src) {
        if (!f->string || !cJSON_IsNumber(f)) continue;
        if (!strcmp(f->string, "tier") || !strcmp(f->string, "rate_mult") || !strcmp(f->string, "v")) continue;
        cJSON *d = cJSON_GetObjectItem(dst, f->string);
        if (d && cJSON_IsNumber(d)) cJSON_SetNumberValue(d, round((d->valuedouble + f->valuedouble) * 10000) / 10000);
    }
}

/* GET /api/v1/economics?unit=input — the grid input's economics summed over its points (per-zone cost/revenue,
 * net-metering kWh). The progressive block is the main point's. */
static esp_err_t rbapi_input_economics_reply(httpd_req_t *req)
{
    rbpower_unit_id_t ids[RBAPI_INPUT_MAX];
    uint8_t n = rbpower_home_input_supply_units(ids, RBAPI_INPUT_MAX);
    if (n == 0) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No input point"); return ESP_FAIL; }
    cJSON *root = NULL;
    for (uint8_t k = 0; k < n; k++) {
        char *js = rbgrid_build_economics_json(ids[k]);
        if (!js) continue;
        cJSON *e = cJSON_Parse(js);
        free(js);
        if (!e) continue;
        if (!root) { root = e; continue; }
        cJSON *zr = cJSON_GetObjectItem(root, "zones"), *ze = cJSON_GetObjectItem(e, "zones"), *z = NULL;
        cJSON_ArrayForEach(z, ze) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(z, "zone"));
            cJSON *t = NULL;
            cJSON_ArrayForEach(t, zr) {
                const char *tn = cJSON_GetStringValue(cJSON_GetObjectItem(t, "zone"));
                if (name && tn && !strcmp(name, tn)) { rbapi_obj_sum(t, z); break; }
            }
        }
        cJSON *nr = cJSON_GetObjectItem(root, "net_metering"), *ne = cJSON_GetObjectItem(e, "net_metering");
        if (nr && ne) rbapi_obj_sum(nr, ne);
        cJSON_Delete(e);
    }
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    cJSON_ReplaceItemInObject(root, "unit", cJSON_CreateString("input"));
    rbapi_add_input_units(root, ids, n);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

/* HTTP GET /api/v1/economics?unit=<hex> — thin wrapper over rbgrid_build_economics_json. */
esp_err_t rbwebserver_api_economics_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_read(req)) return ESP_FAIL;   /* data read — READ role when armed */
    char unit_str[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *q = malloc(qlen);
        if (q && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
            if (httpd_query_key_value(q, "unit", unit_str, sizeof(unit_str)) != ESP_OK)
                httpd_query_key_value(q, "unit_id", unit_str, sizeof(unit_str));   // accept both keys
        }
        free(q);
    }
    rbpower_unit_id_t unit = 0;
    if (strcmp(unit_str, "input") == 0) return rbapi_input_economics_reply(req);
    if (strlen(unit_str) == 0 || sscanf(unit_str, "%hx", &unit) != 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Required: unit=<hex> or unit=input");
        return ESP_FAIL;
    }
    char *out = rbgrid_build_economics_json(unit);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}


/** @brief Context of the CSV row emitter (callback for rbgrid_foreach_valid_record). */
struct rbapi_csv_ctx {
    httpd_req_t *req;   ///< Request the CSV chunks are streamed to
    bool elec   /**< Emit electrical columns */,
         energy /**< Emit energy columns */,
         econ;  ///< Emit economics columns
};
static void rbapi_csv_emit(const rbpower_period_statistics_t *s, void *vctx)
{
    struct rbapi_csv_ctx *c = (struct rbapi_csv_ctx *)vctx;
    char line[320];
    uint32_t ts = (uint32_t)(s->period_start_us / 1000000LL);
    int p = snprintf(line, sizeof(line), "%lu", (unsigned long)ts);
    if (c->elec)
        p = rb_scat(line, sizeof(line), p, ",%.2f,%.3f,%.1f,%.1f,%.1f,%.2f,%.1f,%.2f,%.2f",
                      s->voltage_rms, s->current_rms, s->power_active_avg, s->power_reactive_avg,
                      s->power_apparent_avg, s->power_factor_avg, s->frequency_avg,
                      s->power_quality.thd_voltage_avg, s->power_quality.thd_current_avg);
    if (c->energy)
        p = rb_scat(line, sizeof(line), p, ",%.1f,%.1f,%.1f",
                      s->energy_period.consumed_wh, s->energy_period.supplied_wh, s->energy_period.net_wh);
    if (c->econ)
        p = rb_scat(line, sizeof(line), p, ",%.4f,%.4f,%.4f",
                      s->result_base.cost_consumed, s->result_base.revenue_supplied,
                      s->result_base.cost_consumed - s->result_base.revenue_supplied);
    p = rb_scat(line, sizeof(line), p, "\n");
    httpd_resp_send_chunk(c->req, line, p);
}



/* Return a NEW cJSON array of history records (the validated rbgrid_history_get_data `data[]`,
 * so the rich electrical min/max envelope is preserved) for ONE type, filtered to the unix [from,to]
 * window. Caller owns the array. Used by the history handler for `from`/`to` + `type=all`.
 * Depends only on rbgrid_history_get_data + cJSON. */
static cJSON *rbapi_history_filtered_array(rbpower_unit_id_t unit, rbpower_period_t period,
                                           rbpower_data_type_t type, uint32_t from, uint32_t to, uint32_t limit)
{
    cJSON *arr = cJSON_CreateArray();
    char *js = rbgrid_history_get_data(unit, period, type, 0, limit);
    if (!js) return arr;
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (root) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *rec = NULL;
        if (cJSON_IsArray(data)) {
            cJSON_ArrayForEach(rec, data) {
                cJSON *tsj = cJSON_GetObjectItem(rec, "ts");
                uint32_t ts = tsj ? (uint32_t)cJSON_GetNumberValue(tsj) : 0;
                if (ts < RBGRID_TS_SANE_MIN) continue;        // fake-clock garbage (see above)
                if (ts < from || ts > to) continue;
                cJSON_AddItemToArray(arr, cJSON_Duplicate(rec, 1));
            }
        }
        cJSON_Delete(root);
    }
    return arr;
}

/* ---- Grid input aggregate (unit=input) ---------------------------------------------------------------------
 * A three-phase input metered per phase is several supply points (main + one per further phase). The app's input
 * views need one series: records of all input points summed by ts. Energy/money fields add, dur = max, q = OR,
 * res = max, the rest from the first record seen (the main point's). */

static bool rbapi_sum_key(const char *k)
{
    static const char *const keys[] = { "ec", "es", "en", "et_in", "et_out", "c", "r", "n" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (!strcmp(k, keys[i])) return true;
    return false;
}

static void rbapi_rec_add(cJSON *dst, const cJSON *src)
{
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, src) {
        if (!f->string || !cJSON_IsNumber(f)) continue;
        cJSON *d = cJSON_GetObjectItem(dst, f->string);
        if (!d) { cJSON_AddNumberToObject(dst, f->string, f->valuedouble); continue; }
        if (!cJSON_IsNumber(d)) continue;
        double a = d->valuedouble, b = f->valuedouble, v;
        if (rbapi_sum_key(f->string))      v = a + b;
        else if (!strcmp(f->string, "q"))  v = (double)((uint32_t)a | (uint32_t)b);
        else if (!strcmp(f->string, "dur") || !strcmp(f->string, "res")) v = (a > b) ? a : b;
        else continue;   /* ts and the rest: first record wins */
        cJSON_SetNumberValue(d, v);
    }
}

static int rbapi_ts_desc(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON * const *)a, *y = *(const cJSON * const *)b;
    double tx = cJSON_GetNumberValue(cJSON_GetObjectItem(x, "ts")), ty = cJSON_GetNumberValue(cJSON_GetObjectItem(y, "ts"));
    return (tx < ty) ? 1 : (tx > ty) ? -1 : 0;
}

/* Summed records of the grid input, newest first, within [from, to]; at most @p limit per point are read.
 * @p ids/@p n_ids receive the points used. */
static cJSON *rbapi_input_history_array(rbpower_period_t period, rbpower_data_type_t type, uint32_t from, uint32_t to,
                                        uint32_t limit, rbpower_unit_id_t *ids, uint8_t *n_ids)
{
    uint8_t n = rbpower_home_input_supply_units(ids, RBAPI_INPUT_MAX);
    if (n_ids) *n_ids = n;
    cJSON *acc = cJSON_CreateArray();
    if (!acc) return NULL;
    for (uint8_t k = 0; k < n; k++) {
        cJSON *a = rbapi_history_filtered_array(ids[k], period, type, from, to, limit);
        if (!a) continue;
        cJSON *rec = a->child;
        while (rec) {
            cJSON *next = rec->next;
            double ts = cJSON_GetNumberValue(cJSON_GetObjectItem(rec, "ts"));
            cJSON *dst = NULL, *it = NULL;
            cJSON_ArrayForEach(it, acc) {
                if (cJSON_GetNumberValue(cJSON_GetObjectItem(it, "ts")) == ts) { dst = it; break; }
            }
            if (dst) rbapi_rec_add(dst, rec);
            else cJSON_AddItemToArray(acc, cJSON_DetachItemViaPointer(a, rec));
            rec = next;
        }
        cJSON_Delete(a);
    }
    /* sort newest first, round the sums (records carry 3 dp energy, cents money) */
    int cnt = cJSON_GetArraySize(acc);
    if (cnt > 1) {
        cJSON **v = malloc(sizeof(cJSON *) * (size_t)cnt);
        if (v) {
            int i = 0;
            while (acc->child) v[i++] = cJSON_DetachItemViaPointer(acc, acc->child);
            qsort(v, (size_t)cnt, sizeof(cJSON *), rbapi_ts_desc);
            for (i = 0; i < cnt; i++) cJSON_AddItemToArray(acc, v[i]);
            free(v);
        }
    }
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, acc) {
        cJSON *f = NULL;
        cJSON_ArrayForEach(f, it) {
            if (!f->string || !cJSON_IsNumber(f) || !rbapi_sum_key(f->string)) continue;
            bool money = !strcmp(f->string, "c") || !strcmp(f->string, "r") || !strcmp(f->string, "n");
            cJSON_SetNumberValue(f, money ? round(f->valuedouble * 100) / 100 : round(f->valuedouble * 1000) / 1000);
        }
    }
    return acc;
}

static void rbapi_add_input_units(cJSON *root, const rbpower_unit_id_t *ids, uint8_t n)
{
    cJSON *u = cJSON_AddArrayToObject(root, "units");
    for (uint8_t k = 0; u && k < n; k++) {
        char hx[8];
        snprintf(hx, sizeof(hx), "%04X", (unsigned)ids[k]);
        cJSON_AddItemToArray(u, cJSON_CreateString(hx));
    }
}

/* GET /api/v1/history?unit=input&… — the grid input as one series (energy | economics | all). */
static esp_err_t rbapi_input_history_reply(httpd_req_t *req, const char *period_str, rbpower_period_t period,
                                           const char *type_str, bool type_all, rbpower_data_type_t data_type,
                                           uint32_t offset, uint32_t limit, const char *from_str, const char *to_str,
                                           const char *since_str)
{
    if (!type_all && data_type == RBPOWER_DATA_TYPE_ELECTRICAL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unit=input: type energy, economics or all");
        return ESP_FAIL;
    }
    rbpower_unit_id_t ids[RBAPI_INPUT_MAX];
    uint8_t n = 0;
    uint32_t from = from_str[0] ? (uint32_t)strtoul(from_str, NULL, 10) : 0;
    uint32_t to   = to_str[0]   ? (uint32_t)strtoul(to_str,   NULL, 10) : 0xFFFFFFFFu;
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    if (type_all) {
        cJSON_AddNumberToObject(root, "v", 1);
        cJSON_AddStringToObject(root, "unit", "input");
        cJSON_AddStringToObject(root, "period", period_str);
        cJSON *d = cJSON_AddObjectToObject(root, "data");
        cJSON_AddItemToObject(d, "electrical", cJSON_CreateArray());   /* not aggregated (per phase: read the points) */
        cJSON_AddItemToObject(d, "energy",    rbapi_input_history_array(period, RBPOWER_DATA_TYPE_ENERGY, from, to, 720, ids, &n));
        cJSON_AddItemToObject(d, "economics", rbapi_input_history_array(period, RBPOWER_DATA_TYPE_ECONOMICS, from, to, 720, ids, &n));
    } else if (since_str[0]) {
        uint32_t since = (uint32_t)strtoul(since_str, NULL, 10);
        cJSON *all = rbapi_input_history_array(period, data_type, since + 1, 0xFFFFFFFFu, 720, ids, &n);
        cJSON *arr = cJSON_CreateArray();
        int cnt = all ? cJSON_GetArraySize(all) : 0;
        uint32_t emitted = 0, next_since = since;
        for (int i = cnt - 1; i >= 0 && emitted < limit; i--) {   /* ascending, oldest first */
            cJSON *r = cJSON_GetArrayItem(all, i);
            next_since = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(r, "ts"));
            cJSON_AddItemToArray(arr, cJSON_Duplicate(r, 1));
            emitted++;
        }
        cJSON_Delete(all);
        cJSON_AddStringToObject(root, "unit", "input");
        cJSON_AddStringToObject(root, "period", period_str);
        cJSON_AddStringToObject(root, "type", type_str);
        cJSON_AddNumberToObject(root, "schema_ver", 4);
        cJSON_AddItemToObject(root, "data", arr);
        cJSON_AddNumberToObject(root, "next_since", next_since);
        cJSON_AddBoolToObject(root, "has_more", (uint32_t)cnt > emitted);
        cJSON_AddNumberToObject(root, "total_available", cnt);
    } else if (from_str[0] || to_str[0]) {
        cJSON_AddNumberToObject(root, "v", 1);
        cJSON_AddStringToObject(root, "unit", "input");
        cJSON_AddStringToObject(root, "period", period_str);
        cJSON_AddItemToObject(root, "data", rbapi_input_history_array(period, data_type, from, to, 720, ids, &n));
    } else {
        uint32_t want = offset + limit; if (want > 720) want = 720;
        cJSON *all = rbapi_input_history_array(period, data_type, 0, 0xFFFFFFFFu, want, ids, &n);
        int cnt = all ? cJSON_GetArraySize(all) : 0;
        cJSON *arr = cJSON_CreateArray();
        uint32_t ret = 0;
        for (int i = (int)offset; i < cnt && ret < limit; i++, ret++) cJSON_AddItemToArray(arr, cJSON_Duplicate(cJSON_GetArrayItem(all, i), 1));
        cJSON_Delete(all);
        cJSON_AddStringToObject(root, "status", "ok");
        cJSON_AddNumberToObject(root, "schema_ver", 4);
        cJSON *rq = cJSON_AddObjectToObject(root, "request");
        cJSON_AddStringToObject(rq, "period", period_str);
        cJSON_AddStringToObject(rq, "type", type_str);
        cJSON_AddNumberToObject(rq, "offset", offset);
        cJSON_AddNumberToObject(rq, "limit", limit);
        cJSON_AddStringToObject(rq, "unit_id", "input");
        /* total = the largest record count among the input points (their records share the period grid) */
        uint32_t total = (uint32_t)cnt;
        for (uint8_t k = 0; k < n; k++) {
            uint32_t t = 0;
            if (rbgrid_history_get_buffer_info(ids[k], period, data_type, &t) == RBGRID_OK && t > total) total = t;
        }
        cJSON *meta = cJSON_AddObjectToObject(root, "meta");
        cJSON_AddNumberToObject(meta, "total_available", total);
        cJSON_AddNumberToObject(meta, "returned", ret);
        cJSON_AddBoolToObject(meta, "has_more", total > offset + ret);
        cJSON_AddItemToObject(root, "data", arr);
    }
    rbapi_add_input_units(root, ids, n);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

/* GET /api/v1/export.csv?unit=<hex>&period=hour&type=electrical|energy|economics|all&from=&to=
 * Streamed CSV download (Content-Disposition). Reads descaled period records in small batches (memory-safe;
 * never holds the whole history) via rbgrid_get_period_history, optionally windowed by unix-sec from/to.
 * Tier-1 data (the same descaled values as /api/v1/history); units are stated in the header row. */
esp_err_t rbwebserver_api_export_csv_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_read(req)) return ESP_FAIL;   /* data read — READ role when armed */
    char unit_str[16] = {0}, period_str[16] = {0}, type_str[16] = {0}, from_str[16] = {0}, to_str[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *q = malloc(qlen);
        if (q && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
            httpd_query_key_value(q, "unit", unit_str, sizeof(unit_str));
            if (!unit_str[0]) httpd_query_key_value(q, "unit_id", unit_str, sizeof(unit_str));
            httpd_query_key_value(q, "period", period_str, sizeof(period_str));
            httpd_query_key_value(q, "type", type_str, sizeof(type_str));
            httpd_query_key_value(q, "from", from_str, sizeof(from_str));
            httpd_query_key_value(q, "to", to_str, sizeof(to_str));
        }
        free(q);
    }
    rbpower_unit_id_t unit = 0;
    int sp = rbgrid_period_str_to_stat(period_str);
    if (!unit_str[0] || sscanf(unit_str, "%hx", &unit) != 1 || sp < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Required: unit=<hex>&period=hour|day|week|month|year");
        return ESP_FAIL;
    }
    if (!type_str[0]) strcpy(type_str, "all");
    bool t_elec = !strcmp(type_str, "electrical") || !strcmp(type_str, "all");
    bool t_energy = !strcmp(type_str, "energy") || !strcmp(type_str, "all");
    bool t_econ = !strcmp(type_str, "economics") || !strcmp(type_str, "all");
    if (!t_elec && !t_energy && !t_econ) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "type = electrical|energy|economics|all");
        return ESP_FAIL;
    }
    uint32_t from = from_str[0] ? (uint32_t)strtoul(from_str, NULL, 10) : 0;
    uint32_t to   = to_str[0]   ? (uint32_t)strtoul(to_str,   NULL, 10) : 0xFFFFFFFFu;

    httpd_resp_set_type(req, "text/csv");
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"rbgrid_%04X_%s_%s.csv\"", (unsigned)unit, period_str, type_str);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    // Header row (units stated)
    char line[320];
    int n = snprintf(line, sizeof(line), "timestamp_unix%s%s%s\n",
                     t_elec ? ",v_rms_V,i_rms_A,p_active_W,p_reactive_var,p_apparent_VA,pf,freq_Hz,thd_v_pct,thd_i_pct" : "",
                     t_energy ? ",energy_consumed_Wh,energy_supplied_Wh,energy_net_Wh" : "",
                     t_econ ? ",cost,revenue,net_cost" : "");
    httpd_resp_send_chunk(req, line, n);

    int fp = rbgrid_period_str_to_flash(period_str);
    struct rbapi_csv_ctx cctx = { req, t_elec, t_energy, t_econ };
    rbgrid_foreach_valid_record(unit, sp, fp, from, to, rbapi_csv_emit, &cctx);  // bounded + skips empties

    httpd_resp_send_chunk(req, NULL, 0);   // end stream
    return ESP_OK;
}


/* HTTP GET /api/v1/summary?unit=&period=&from=&to= — thin wrapper over rbgrid_build_summary_json. */
esp_err_t rbwebserver_api_summary_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_read(req)) return ESP_FAIL;   /* data read — READ role when armed */
    char unit_str[16] = {0}, period_str[16] = {0}, from_str[16] = {0}, to_str[16] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *q = malloc(qlen);
        if (q && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
            httpd_query_key_value(q, "unit", unit_str, sizeof(unit_str));
            if (!unit_str[0]) httpd_query_key_value(q, "unit_id", unit_str, sizeof(unit_str));
            httpd_query_key_value(q, "period", period_str, sizeof(period_str));
            httpd_query_key_value(q, "from", from_str, sizeof(from_str));
            httpd_query_key_value(q, "to", to_str, sizeof(to_str));
        }
        free(q);
    }
    if (!period_str[0]) strcpy(period_str, "hour");
    rbpower_unit_id_t unit = 0;
    int sp = rbgrid_period_str_to_stat(period_str);
    if (!unit_str[0] || sscanf(unit_str, "%hx", &unit) != 1 || sp < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Required: unit=<hex> (period default hour)");
        return ESP_FAIL;
    }
    uint32_t from = from_str[0] ? (uint32_t)strtoul(from_str, NULL, 10) : 0;
    uint32_t to   = to_str[0]   ? (uint32_t)strtoul(to_str,   NULL, 10) : 0xFFFFFFFFu;
    char *out = rbgrid_build_summary_json(unit, sp, period_str, from, to);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

/* GET/POST /api/system/mqtt-config — configure the MQTT broker at runtime (was compile-time
 * RBGRID_MQTT_BROKER_URI). Persists the broker URI to littlefs (/mqtt_broker); main.cpp loads it at boot.
 * Auth + TLS go via the URI scheme: mqtts://user:pass@host:8883. APPLY-ON-RESTART (matches the other
 * config endpoints) — POST saves + sets requires_reboot; the new broker is used on next boot. */
#define RBAPI_MQTT_BROKER_FILE "/mqtt_broker"   ///< littlefs file: persisted MQTT broker URI
/* mqtts://user:pass@host:8883 → mqtts://<masked>@host:8883 — the GET is unauthenticated and CORS-readable, so the
 * broker credentials never leave the hub. */
static void mqtt_uri_mask(const char *in, char *out, size_t n)
{
    const char *sch = strstr(in, "://");
    const char *at  = sch ? strchr(sch + 3, '@') : NULL;
    if (!at) { strlcpy(out, in, n); return; }
    snprintf(out, n, "%.*s***%s", (int)(sch + 3 - in), in, at);
}
esp_err_t rbwebserver_api_mqtt_config_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (req->method == HTTP_GET) {
        char uri[160] = {0};
        size_t act = 0;
        bool persisted = littlefs_raw_exists(RBAPI_MQTT_BROKER_FILE) &&
                         littlefs_raw_read(RBAPI_MQTT_BROKER_FILE, uri, sizeof(uri) - 1, &act) == ESP_OK && uri[0];
        char mu[176], me[176];
        mqtt_uri_mask(persisted ? uri : "", mu, sizeof(mu));
        mqtt_uri_mask(rbmqtt_broker_uri(), me, sizeof(me));
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "broker_uri", mu);
        cJSON_AddBoolToObject(root, "persisted", persisted);   // false → using the compile-time default
        cJSON_AddStringToObject(root, "effective_uri", me);   /* what the client actually runs with (connected:true with an empty broker_uri meant the default broker) */
        cJSON_AddBoolToObject(root, "connected", rbmqtt_is_connected());
        cJSON_AddStringToObject(root, "note", "auth/TLS via URI scheme (mqtts://user:pass@host:port). Apply-on-restart.");
        char *s = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, s, strlen(s));
        free(s);
        return ESP_OK;
    }

    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // write requires the admin token (if provisioned)
        char body[256] = {0};
        int len = req->content_len;
        if (len <= 0 || len >= (int)sizeof(body)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large / empty");
            return ESP_FAIL;
        }
        int got = rbapi_recv_body(req, body, len);
        if (got <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        body[got] = 0;
        cJSON *j = cJSON_Parse(body);
        const char *uri = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "broker_uri")) : NULL;
        // Validate the scheme (mqtt:// or mqtts://) so a bad value can't brick the broker connection.
        bool valid = uri && (strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "mqtts://", 8) == 0) && strlen(uri) < 160;
        if (valid && strstr(uri, "://***@")) {   /* the masked GET value sent back would store "***" as the password */
            cJSON_Delete(j);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "broker_uri carries the masked credentials (***) — send the full user:password or a URI without them");
            return ESP_FAIL;
        }
        if (!valid) {
            if (j) cJSON_Delete(j);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "broker_uri required (mqtt://host:port or mqtts://user:pass@host:port)");
            return ESP_FAIL;
        }
        esp_err_t we = littlefs_raw_write(RBAPI_MQTT_BROKER_FILE, uri, strlen(uri) + 1);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", we == ESP_OK ? "OK" : "ERROR");
        char mu[176]; mqtt_uri_mask(uri, mu, sizeof(mu));
        cJSON_AddStringToObject(resp, "broker_uri", mu);
        cJSON_AddBoolToObject(resp, "requires_reboot", true);
        cJSON_AddStringToObject(resp, "message", we == ESP_OK ? "Saved. Reboot to apply the new broker." : "persist failed");
        cJSON_Delete(j);
        char *s = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, s, strlen(s));
        free(s);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET/POST only");
    return ESP_FAIL;
}

/* API-token provisioning over HTTP. GET → {token_set} (never reveals the token). POST {token}
 * sets/rotates it: the initial set works in the fail-open window (no token yet → open); rotating an existing
 * token requires the current Bearer (enforced by rbapi_require_auth). POST {token:""} clears → auth OFF. */
esp_err_t rbwebserver_api_token_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) { httpd_resp_send(req, NULL, 0); return ESP_OK; }
    if (req->method == HTTP_GET) {
        char cur[48];
        bool set  = rbgrid_token_load(RBGRID_TOKEN_FILE, cur);
        bool rset = rbgrid_token_load(RBGRID_TOKEN_READ_FILE, cur);   /* read role */
        httpd_resp_set_type(req, "application/json");
        char out[64];
        snprintf(out, sizeof(out), "{\"token_set\":%s,\"read_token_set\":%s}",
                 set ? "true" : "false", rset ? "true" : "false");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   /* fail-open initial set / Bearer-gated rotate */
        char body[192] = {0};
        int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
        if (len > 0 && rbapi_recv_body(req, body, len) <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
        if (len > 0) body[len] = 0;
        cJSON *j = cJSON_Parse(body);
        const char *tok  = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "token")) : NULL;
        const char *role = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "role"))  : NULL;
        if (!tok) { if (j) cJSON_Delete(j); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "token required"); return ESP_FAIL; }
        /* role picks the slot — "admin" (default) or "read". A read token without an admin
         * token is pointless (auth not armed) but allowed; GET reports both flags. */
        const char *file = (role && strcmp(role, "read") == 0) ? RBGRID_TOKEN_READ_FILE : RBGRID_TOKEN_FILE;
        bool clearing = (tok[0] == 0);
        if (!clearing && strlen(tok) >= 47) { cJSON_Delete(j); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "token too long (<47)"); return ESP_FAIL; }
        esp_err_t we = clearing ? littlefs_raw_remove(file)
                                : littlefs_raw_write(file, tok, strlen(tok) + 1);
        cJSON_Delete(j);
        httpd_resp_set_type(req, "application/json");
        char cur[48];
        char out[96];
        snprintf(out, sizeof(out), "{\"status\":\"%s\",\"token_set\":%s,\"read_token_set\":%s}",
                 we == ESP_OK ? "OK" : "ERROR",
                 rbgrid_token_load(RBGRID_TOKEN_FILE, cur) ? "true" : "false",
                 rbgrid_token_load(RBGRID_TOKEN_READ_FILE, cur) ? "true" : "false");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET/POST only");
    return ESP_FAIL;
}

/* GET/POST /api/system/security: CORS policy (dev_cors flag + origin allow-list).
 * Both methods admin-gated (unarmed auth = open, as everywhere). POST body:
 * {"dev_cors":bool, "origins":["http://host:port", ...≤8]} → persisted to /sec_cfg + live reload.
 * GET also returns "builtin_origins" (always allowed, read-only) and "origins_max". A POST without
 * "dev_cors" keeps allow-list mode (false). Built-in origins in a POSTed list are dropped (no slot used). */
esp_err_t rbwebserver_api_security_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) { httpd_resp_send(req, NULL, 0); return ESP_OK; }
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        bool dc = true;
        char orgs[RBWEB_CORS_MAX_ORIGINS_PUB][64];
        int n = 0;
        rbwebserver_security_cfg_get(&dc, orgs, RBWEB_CORS_MAX_ORIGINS_PUB, &n);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "dev_cors", dc);
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(orgs[i]));
        cJSON_AddItemToObject(j, "origins", arr);
        cJSON *bi = cJSON_CreateArray();
        for (int i = 0; i < rbwebserver_security_builtin_count(); i++)
            cJSON_AddItemToArray(bi, cJSON_CreateString(rbwebserver_security_builtin_origin(i)));
        cJSON_AddItemToObject(j, "builtin_origins", bi);
        cJSON_AddNumberToObject(j, "origins_max", RBWEB_CORS_MAX_ORIGINS_PUB);
        char *out = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
        httpd_resp_sendstr(req, out);
        free(out);
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        char body[512] = {0};
        int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
        if (len <= 0 || rbapi_recv_body(req, body, len) <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
            return ESP_FAIL;
        }
        body[len] = 0;
        cJSON *j = cJSON_Parse(body);
        if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json"); return ESP_FAIL; }
        /* Re-serialize only the known keys — never persist arbitrary client JSON. */
        cJSON *outj = cJSON_CreateObject();
        cJSON_AddBoolToObject(outj, "dev_cors", cJSON_IsTrue(cJSON_GetObjectItem(j, "dev_cors")));
        cJSON *arr_in = cJSON_GetObjectItem(j, "origins");
        cJSON *arr = cJSON_CreateArray();
        int kept = 0;
        if (cJSON_IsArray(arr_in)) {
            cJSON *o = NULL;
            cJSON_ArrayForEach(o, arr_in) {
                bool is_builtin = false;
                for (int b = 0; cJSON_IsString(o) && b < rbwebserver_security_builtin_count(); b++)
                    if (strcmp(o->valuestring, rbwebserver_security_builtin_origin(b)) == 0) is_builtin = true;
                if (cJSON_IsString(o) && o->valuestring[0] && strlen(o->valuestring) < 64 && !is_builtin &&
                    kept < RBWEB_CORS_MAX_ORIGINS_PUB) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(o->valuestring));
                    kept++;
                }
            }
        }
        cJSON_AddItemToObject(outj, "origins", arr);
        cJSON_Delete(j);
        char *ser = cJSON_PrintUnformatted(outj);
        cJSON_Delete(outj);
        if (!ser) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
        esp_err_t we = littlefs_raw_write("/sec_cfg", ser, strlen(ser) + 1);
        free(ser);
        rbwebserver_security_cfg_reload();
        char out[64];
        snprintf(out, sizeof(out), "{\"status\":\"%s\",\"origins\":%d}",
                 we == ESP_OK ? "OK" : "ERROR", kept);
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET/POST only");
    return ESP_FAIL;
}

/* Full-config backup — modular, single slot. GET → {exists, files}. POST → snapshot every module
 * config file to /backup/ (overwrites the slot). The "save current config as the rollback point" action. */
esp_err_t rbwebserver_api_backup_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) { httpd_resp_send(req, NULL, 0); return ESP_OK; }
    if (req->method == HTTP_GET) {
        int n = rbgrid_storage_backup_count();
        httpd_resp_set_type(req, "application/json");
        /* meta = {ts,fw,files} stamped at POST time (additive; absent on pre-meta slots). */
        char meta[128]; int ml = (n > 0) ? rbgrid_storage_backup_meta(meta, sizeof(meta)) : 0;
        /* sealed: present only for a 1.1.7+ slot — false = restore will refuse it (a save failed part-way) */
        int sv = (n > 0) ? rbgrid_storage_backup_sealed() : -1;
        char seal[24] = "";
        if (sv >= 0) snprintf(seal, sizeof(seal), ",\"sealed\":%s", sv == 1 ? "true" : "false");
        char out[256];
        if (ml > 0)
            snprintf(out, sizeof(out), "{\"exists\":true,\"files\":%d%s,\"meta\":%s}", n, seal, meta);
        else
            snprintf(out, sizeof(out), "{\"exists\":%s,\"files\":%d%s}", n > 0 ? "true" : "false", n, seal);
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;
        int n = rbgrid_storage_backup_all();
        httpd_resp_set_type(req, "application/json");
        if (n <= 0) httpd_resp_set_status(req, "500 Internal Server Error");   /* any failed copy = no restore point */
        char out[96]; snprintf(out, sizeof(out), "{\"status\":\"%s\",\"files_backed_up\":%d}", n > 0 ? "OK" : "ERROR", n > 0 ? n : 0);
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET/POST only");
    return ESP_FAIL;
}

/* Full-config restore — POST → restore every module config file from the backup slot, then reboot to
 * apply (the running config is in RAM). 404 if no backup. Single slot (last backup only). */
esp_err_t rbwebserver_api_restore_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) { httpd_resp_send(req, NULL, 0); return ESP_OK; }
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    if (rbgrid_storage_backup_count() == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"status\":\"ERROR\",\"message\":\"no backup to restore\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int n = rbgrid_storage_restore_all();
    if (n == -2) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "{\"status\":\"ERROR\",\"error_code\":\"BACKUP_INCOMPLETE\",\"message\":\"The restore point is incomplete (a save failed part-way). Nothing was changed; save a new restore point.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (n <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"status\":\"ERROR\",\"error_code\":\"RESTORE_FAILED\",\"message\":\"Restoring failed; the settings in use before were put back. No restart needed.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"status\":\"OK\",\"files_restored\":%d,\"requires_reboot\":true}", n);
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


/**
 * @brief Send a 400 response for a numeric field outside the schema `limits.ranges`.
 *
 * Body: {"status":"ERROR","error_code":"OUT_OF_RANGE","field":…,"message":…}. Adds the CORS headers.
 *
 * @param[in] req    Request to answer.
 * @param[in] field  Offending field name (NULL → "").
 * @param[in] msg    Human-readable message (NULL → "value out of range").
 */
void rbapi_send_range_error(httpd_req_t *req, const char *field, const char *msg)
{
    cJSON *e = cJSON_CreateObject();
    char *s = NULL;
    if (e) {
        cJSON_AddStringToObject(e, "status", "ERROR");
        cJSON_AddStringToObject(e, "error_code", "OUT_OF_RANGE");
        cJSON_AddStringToObject(e, "field", field ? field : "");
        cJSON_AddStringToObject(e, "message", msg ? msg : "value out of range");
        s = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
    }
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, HTTPD_400);
    if (s) { httpd_resp_send(req, s, strlen(s)); free(s); }
    else httpd_resp_send(req, "{\"status\":\"ERROR\",\"error_code\":\"OUT_OF_RANGE\"}", HTTPD_RESP_USE_STRLEN);
}

/* HTTP GET /api/v1/schema — thin wrapper over rbgrid_build_schema_json. */
esp_err_t rbwebserver_api_schema_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    char *s = rbgrid_build_schema_json();
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s, strlen(s));
    free(s);
    return ESP_OK;
}


/* ==== Dynamic prices (DYNAMIC_TARIFFS_DESIGN §6): the same core functions as `cmd get/set dynprice|price` ==== */

static esp_err_t dp_send_json(httpd_req_t *req, cJSON *o, const char *status)
{
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s, strlen(s));
    free(s);
    return ESP_OK;
}

static cJSON *dp_read_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 24 * 1024) return NULL;
    char *body = malloc(len + 1);
    if (!body) return NULL;
    int got = rbapi_recv_body(req, body, len);
    cJSON *j = got > 0 ? cJSON_ParseWithLength(body, got) : NULL;
    free(body);
    return j;
}

static esp_err_t dp_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t dp_error(httpd_req_t *req, const char *field, const char *msg)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", msg);
    cJSON_AddStringToObject(e, "field", field ? field : "");
    return dp_send_json(req, e, "400 Bad Request");
}

/* GET /api/v1/dynprice → config + status + providers[]; POST → merge config (400 {error, field} on a bad field). */
esp_err_t rbwebserver_api_dynprice_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) return dp_options(req);
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;
        cJSON *j = dp_read_body(req);
        if (!j) return dp_error(req, "", "body must be a JSON object");
        char ef[24] = "", em[128] = "";
        esp_err_t r = rbdynprice_set_config(j, ef, sizeof(ef), em, sizeof(em));
        cJSON_Delete(j);
        if (r != ESP_OK) return dp_error(req, ef, em);
        return dp_send_json(req, rbdynprice_config_json(), NULL);
    }
    if (!rbapi_require_read(req)) return ESP_FAIL;
    return dp_send_json(req, rbdynprice_config_json(), NULL);
}

/* GET /api/v1/price[?from=&until=] → state + HA store + resolved prices; POST {prices:[…]} | {buy,from,until} |
 * {clear:true} → the Home Assistant / external store (same rules as `cmd set price`). */
esp_err_t rbwebserver_api_price_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) return dp_options(req);
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;
        cJSON *j = dp_read_body(req);
        if (!j) return dp_error(req, "", "body must be a JSON object");
        if (cJSON_IsTrue(cJSON_GetObjectItem(j, "clear"))) {
            rbpower_tariff_clear_external_price();
            cJSON_Delete(j);
            return dp_send_json(req, rbdynprice_price_json(0, 0), NULL);
        }
        const cJSON *arr = cJSON_GetObjectItem(j, "prices");
        int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 1;
        for (int k = 0; k < n; k++) {
            const cJSON *e = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, k) : j;
            const cJSON *jb = cJSON_GetObjectItem(e, "buy"), *js = cJSON_GetObjectItem(e, "sell");
            const cJSON *jf = cJSON_GetObjectItem(e, "from"), *ju = cJSON_GetObjectItem(e, "until");
            if (!cJSON_IsNumber(jb) || !cJSON_IsNumber(jf) || !cJSON_IsNumber(ju) || (js && !cJSON_IsNumber(js) && !cJSON_IsNull(js)) ||
                rbpower_tariff_set_external_price((float)jb->valuedouble, cJSON_IsNumber(js) ? (float)js->valuedouble : NAN,
                                                  (uint32_t)jf->valuedouble, (uint32_t)ju->valuedouble) != ESP_OK) {
                char m[128];
                snprintf(m, sizeof(m), "price %d: needs buy/sell -100..100 and from < until (unix s); %d stored before it", k, k);
                cJSON_Delete(j);
                return dp_error(req, "prices", m);
            }
        }
        cJSON_Delete(j);
        return dp_send_json(req, rbdynprice_price_json(0, 0), NULL);
    }
    if (!rbapi_require_read(req)) return ESP_FAIL;
    uint32_t from = 0, until = 0;
    char q[96], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "from", v, sizeof(v)) == ESP_OK) from = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "until", v, sizeof(v)) == ESP_OK) until = (uint32_t)strtoul(v, NULL, 10);
    }
    bool log = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "log", v, sizeof(v)) == ESP_OK)
        log = (v[0] == '1' || v[0] == 't');
    return dp_send_json(req, log ? rbdynprice_price_log_json(from, until) : rbdynprice_price_json(from, until), NULL);
}
