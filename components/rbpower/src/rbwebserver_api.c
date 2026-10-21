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
 * both static in rbwebserver.c) is reached via the rbwebserver_core_ext_cfg() /
 * rbwebserver_core_json_ctx() accessors.
 *
 * All HTTP handlers run on the httpd task; reads of g_home_network take rbpower_home_lock().
 */

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
static void rbapi_json_escape(const char *in, char *out, size_t n);   /* below, next to the MQTT config */

static const char* TAG = "RBWEBSERVER_API";
/** @brief Oldest record timestamp accepted as real history, epoch s (2025-01-01).
 *
 * Anything earlier is a fake-clock / Jan-2024-fallback artifact, never real history (the previous
 * 2020-09 bound let the 2024 fallback records through). */
#define RBAPI_TS_SANE_MIN 1735689600u

/* ==== TWO-ROLE token auth + 401 rate-limit.
 * Tokens persist in littlefs: /api_token = ADMIN (writes + everything), /api_token_read = READ
 * (data reads: history/economics/summary/export + WS telemetry). Arming model (migration-safe for
 * existing installs): NO admin token provisioned = onboarding state → everything open.
 * Admin token set → writes require ADMIN (a READ token on a write = 403, not 401). Read token
 * additionally set → gated data reads require READ or ADMIN. Token sources: `Authorization:
 * Bearer`, `X-API-Key`, or `?token=` in the query (WS handshake can't set headers from browsers).
 * Rate-limit: >10 FAILED auths in 60 s → further invalid attempts get 429 + Retry-After (valid
 * tokens keep working — the limiter slows guessing, it must not lock out the owner). ==== */
#define RBAPI_TOKEN_FILE      "/api_token"      ///< littlefs file holding the ADMIN token
#define RBAPI_TOKEN_READ_FILE "/api_token_read" ///< littlefs file holding the READ token

/** @brief Role granted to a request by the presented token. */
typedef enum {
    RBAPI_ROLE_NONE = 0,  ///< No / wrong token while auth is armed
    RBAPI_ROLE_READ = 1,  ///< READ token: gated data reads only
    RBAPI_ROLE_ADMIN = 2  ///< ADMIN token, or auth not armed (onboarding)
} rbapi_role_t;

static bool rbapi_load_token(const char* file, char out[48])
{
    size_t act = 0;
    out[0] = '\0';
    return littlefs_raw_exists(file) &&
           littlefs_raw_read(file, out, 47, &act) == ESP_OK && out[0];
}

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

/* Role of this request. No admin token on flash = auth not armed → ADMIN (open/onboarding). */
static rbapi_role_t rbapi_request_role(httpd_req_t* req, bool* out_armed_read)
{
    char admin[48], rd[48];
    bool have_admin = rbapi_load_token(RBAPI_TOKEN_FILE, admin);
    bool have_read  = rbapi_load_token(RBAPI_TOKEN_READ_FILE, rd);
    if (out_armed_read) *out_armed_read = have_read;
    if (!have_admin) return RBAPI_ROLE_ADMIN;
    char got[96];
    rbapi_presented_token(req, got);
    if (got[0] && strcmp(got, admin) == 0) return RBAPI_ROLE_ADMIN;
    if (got[0] && have_read && strcmp(got, rd) == 0) return RBAPI_ROLE_READ;
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
bool rbwebserver_api_token_armed(void) { char t[48]; return rbapi_load_token(RBAPI_TOKEN_FILE, t); }
bool rbwebserver_api_check_read(httpd_req_t* req) { return rbapi_require_read(req); }

/* Reusable JSON builders (defined later; shared by the HTTP handlers + the MQTT cmd-get path). */
char *rbgrid_build_schema_json(void);
/* 400 body for a numeric field outside schema limits.ranges: {"status":"ERROR","error_code":"OUT_OF_RANGE","field":…,"message":…} */
void rbapi_send_range_error(httpd_req_t *req, const char *field, const char *msg);
char *rbgrid_build_economics_json(rbpower_unit_id_t unit);
char *rbgrid_build_summary_json(rbpower_unit_id_t unit, int sp, const char *period_str, uint32_t from, uint32_t to);
static int rbapi_period_str_to_stat(const char *s);
/* ESP-NOW status provider: esp_now_hub lives in main/ (above this component), so it registers its
 * status-JSON builder here at boot; the MQTT `cmd get espnow` path calls through it. NULL until registered. */
static char *(*s_espnow_status_provider)(void) = NULL;
void rbgrid_register_espnow_status_provider(char *(*fn)(void)) { s_espnow_status_provider = fn; }

/* ==== moved verbatim from rbwebserver.c (command dispatch + api config/units/sensors/history) ==== */
static char* cmd_ack_json(const char* cmd, const char* section,
                          const char* result, const char* message, const char* id)
{
    if (!rbwebserver_core_json_ctx()) return NULL;
    cJSON* root = rbgrid_json_create_base_message("cmd_ack", rbwebserver_core_json_ctx());
    if (!root) return NULL;
    cJSON* data = cJSON_CreateObject();
    if (!data) { cJSON_Delete(root); return NULL; }
    cJSON_AddStringToObject(data, "cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(data, "section", section ? section : "");
    cJSON_AddStringToObject(data, "result", result ? result : "");
    if (message) cJSON_AddStringToObject(data, "message", message);
    (void)id;   // id is echoed at the TOP LEVEL of the envelope by the dispatcher, not inside data
    cJSON_AddItemToObject(root, "data", data);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

// Build the GET response for a section by delegating to the same builder the HTTP handler uses.
// `root` carries optional params (unit/period/from/to) for the data sections (summary/economics).
static char* cmd_get_section(const cJSON* root, const char* section)
{
    if (!section) return NULL;
    // v1 API sections over MQTT (same JSON as the HTTP endpoints; the dispatcher injects top-level id).
    if (strcmp(section, "schema") == 0)  return rbgrid_build_schema_json();
    if (strcmp(section, "sensors") == 0) return rbgrid_json_create_sensors_list(rbwebserver_core_json_ctx());
    if (strcmp(section, "espnow") == 0)  return s_espnow_status_provider ? s_espnow_status_provider() : NULL;
    if (strcmp(section, "economics") == 0) {
        const cJSON* ju = cJSON_GetObjectItem(root, "unit");
        if (!cJSON_IsString(ju)) return NULL;
        return rbgrid_build_economics_json((rbpower_unit_id_t)strtoul(ju->valuestring, NULL, 16));
    }
    if (strcmp(section, "summary") == 0) {
        const cJSON* ju = cJSON_GetObjectItem(root, "unit");
        if (!cJSON_IsString(ju)) return NULL;
        rbpower_unit_id_t unit = (rbpower_unit_id_t)strtoul(ju->valuestring, NULL, 16);
        const cJSON* jp = cJSON_GetObjectItem(root, "period");
        const char* ps = (cJSON_IsString(jp) && jp->valuestring[0]) ? jp->valuestring : "hour";
        int sp = rbapi_period_str_to_stat(ps);
        if (sp < 0) { ps = "hour"; sp = rbapi_period_str_to_stat(ps); }
        const cJSON* jf = cJSON_GetObjectItem(root, "from");
        const cJSON* jt = cJSON_GetObjectItem(root, "to");
        uint32_t from = cJSON_IsNumber(jf) ? (uint32_t)jf->valuedouble : 0;
        uint32_t to   = cJSON_IsNumber(jt) ? (uint32_t)jt->valuedouble : 0xFFFFFFFFu;
        return rbgrid_build_summary_json(unit, sp, ps, from, to);
    }
    if (strcmp(section, "system") == 0 && rbwebserver_core_ext_cfg()) {
        return rbgrid_json_create_system_config(rbwebserver_core_ext_cfg());
    }
    if (strcmp(section, "telemetry") == 0 && rbwebserver_core_ext_cfg() && rbwebserver_core_json_ctx()) {
        return rbgrid_json_create_telemetry_config(rbwebserver_core_ext_cfg(), rbwebserver_core_json_ctx());
    }
    if (strcmp(section, "wifi") == 0 && rbwebserver_core_json_ctx()) {
        return rbgrid_json_create_wifi_config(rbwebserver_core_json_ctx());
    }
    if (strcmp(section, "tariff") == 0 && rbwebserver_core_json_ctx()) {
        return rbgrid_json_create_rbpower_tariff_plan_config(rbwebserver_core_json_ctx());
    }
    if (strcmp(section, "adc_sensors") == 0 && rbwebserver_core_json_ctx()) {
        return rbgrid_json_create_adc_sensor_list(rbwebserver_core_json_ctx());
    }
    if (strcmp(section, "units") == 0 && rbwebserver_core_json_ctx()) {
        // Reads g_home_network → lock vs the core-0/esp_timer mutators (we run on the MQTT task).
        rbpower_home_lock();
        char* s = rbgrid_json_create_units_config(rbwebserver_core_json_ctx());
        rbpower_home_unlock();
        return s;
    }
    if (strcmp(section, "telemetry_data") == 0) {
        extern char* rbgrid_json_create_telemetry(void);
        rbpower_home_lock();
        char* s = rbgrid_json_create_telemetry();
        rbpower_home_unlock();
        return s;
    }
    return NULL;   // unknown / context unavailable
}

// Apply a SET for a section, returning a cmd_ack envelope.
static char* cmd_set_section(const cJSON* root, const char* section, const char* id)
{
    /* the MQTT cmd channel bypassed the HTTP write gate: with an admin token provisioned, a SET must carry it
     * in the envelope ({"cmd":"set",…,"token":"…"}) — same rule as HTTP writes */
    {
        char admin[48];
        if (rbapi_load_token(RBAPI_TOKEN_FILE, admin)) {
            const char* tok = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
            if (!tok || strcmp(tok, admin) != 0) return cmd_ack_json("set", section, "error", "unauthorized: admin token required", id);
        }
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) {
        return cmd_ack_json("set", section, "error", "missing 'data'", id);
    }
    // The string-based parsers want the section JSON as a string.
    char* data_str = cJSON_PrintUnformatted(data);
    if (!data_str) {
        return cmd_ack_json("set", section, "error", "serialize failed", id);
    }

    char* ack = NULL;
    char msg[96];
    if (strcmp(section, "tariff") == 0) {
        rbgrid_json_err_t e = rbgrid_json_parse_tariff_plan_config(data_str);
        if (e == RBGRID_JSON_OK) {
            ack = cmd_ack_json("set", section, "ok", "tariff applied", id);
        } else {
            snprintf(msg, sizeof(msg), "tariff parse failed (err %d)", (int)e);
            ack = cmd_ack_json("set", section, "error", msg, id);
        }
    } else
    if (strcmp(section, "units") == 0) {
        esp_err_t e = rbgrid_storage_save_units(data_str);
        if (e == ESP_OK) {
            ack = cmd_ack_json("set", section, "ok", "units saved", id);
        } else {
            // Surface the real reason: ESP_ERR_INVALID_STATE = external config flash not mounted
            // (storage backend absent on this board), not a parse/buffer issue.
            snprintf(msg, sizeof(msg), "units save failed: %s", esp_err_to_name(e));
            ack = cmd_ack_json("set", section, "error", msg, id);
        }
    } else {
        ack = cmd_ack_json("set", section, "error", "section not settable over cmd channel", id);
    }
    free(data_str);
    return ack;
}

// MQTT history query — parity with HTTP GET /api/v1/history. Reuses rbgrid_history_get_data()
// (→ the validated flash read path). Returns the SAME flat {status,request,meta,data} JSON as the
// HTTP endpoint so clients reuse one parser. Request:
//   {"cmd":"history","unit":"0x9020","period":0,"type":0,"offset":0,"limit":50}
//   period = STORAGE period (0=HOUR 1=DAY 2=WEEK 3=MON 4=YEAR); type = 0=electrical 1=energy ...
static char* cmd_history(const cJSON* root)
{
    const cJSON* ju = cJSON_GetObjectItem(root, "unit");
    const cJSON* jp = cJSON_GetObjectItem(root, "period");
    const cJSON* jt = cJSON_GetObjectItem(root, "type");
    const cJSON* jo = cJSON_GetObjectItem(root, "offset");
    const cJSON* jl = cJSON_GetObjectItem(root, "limit");
    if (!cJSON_IsString(ju)) {
        return cmd_ack_json("history", "-", "error", "missing 'unit' (hex string e.g. \"0x9020\")", NULL);
    }
    uint32_t unit   = (uint32_t)strtoul(ju->valuestring, NULL, 16);
    // period/type accept the SAME strings as HTTP /api/v1/history (hour/day/week/month/year,
    // electrical/energy/economics/quality/direction), or a numeric enum value as fallback.
    int period = 0, type = 0;
    if (cJSON_IsString(jp)) {
        const char* p = jp->valuestring;
        period = !strcmp(p,"hour")?0 : !strcmp(p,"day")?1 : !strcmp(p,"week")?2 : !strcmp(p,"month")?3 : !strcmp(p,"year")?4 : 0;
    } else if (cJSON_IsNumber(jp)) { period = jp->valueint; }
    if (cJSON_IsString(jt)) {
        const char* t = jt->valuestring;
        type = !strcmp(t,"electrical")?0 : !strcmp(t,"energy")?1 : !strcmp(t,"economics")?2 : !strcmp(t,"quality")?3 : !strcmp(t,"direction")?4 : 0;
    } else if (cJSON_IsNumber(jt)) { type = jt->valueint; }
    uint32_t offset = cJSON_IsNumber(jo) ? (uint32_t)jo->valueint : 0;
    uint32_t limit  = cJSON_IsNumber(jl) ? (uint32_t)jl->valueint : 50;
    if (limit == 0 || limit > 720) limit = 50;
    // Reads external flash (not g_home_network) → no g_home_network lock; the flash read path is
    // itself write-priority-gated so it defers to in-flight stats writes.
    char* js = rbgrid_history_get_data((rbpower_unit_id_t)unit, (rbpower_period_t)period,
                                       (rbpower_data_type_t)type, offset, limit);
    if (!js) {
        return cmd_ack_json("history", "-", "error", "history read failed (no buffer/records or flash unavailable)", NULL);
    }
    return js;   // flat {status,request,meta,data} — identical to HTTP /api/v1/history
}

// Public: parse a command JSON and return a malloc'd response JSON (caller frees), or NULL.
char* rbgrid_command_dispatch(const char* in_json, int in_len)
{
    if (!in_json || in_len <= 0) return NULL;
    cJSON* root = cJSON_ParseWithLength(in_json, (size_t)in_len);
    if (!root) {
        ESP_LOGW(TAG, "cmd: invalid JSON (%d bytes)", in_len);
        return cmd_ack_json("?", "?", "error", "invalid JSON", NULL);
    }
    const cJSON* cmd = cJSON_GetObjectItem(root, "cmd");
    const cJSON* sec = cJSON_GetObjectItem(root, "section");
    const cJSON* idj = cJSON_GetObjectItem(root, "id");
    const char* cmd_s = cJSON_IsString(cmd) ? cmd->valuestring : "";
    const char* sec_s = cJSON_IsString(sec) ? sec->valuestring : "";
    const char* id_s  = cJSON_IsString(idj) ? idj->valuestring : NULL;

    char* out = NULL;
    if (strcmp(cmd_s, "get") == 0) {
        out = cmd_get_section(root, sec_s);
        if (!out) out = cmd_ack_json("get", sec_s, "error", "unknown section or context unavailable", id_s);
    } else if (strcmp(cmd_s, "set") == 0) {
        out = cmd_set_section(root, sec_s, id_s);
    } else if (strcmp(cmd_s, "history") == 0) {
        out = cmd_history(root);
    } else {
        out = cmd_ack_json(cmd_s, sec_s, "error", "unknown cmd (use 'get' / 'set' / 'history')", id_s);
    }

    // Echo the request id as a TOP-LEVEL envelope field on EVERY response (get + set). MQTT
    // cmd/resp is a shared pub-sub topic (unlike HTTP's 1:1 socket), so correlation by `type`
    // breaks with concurrent in-flight requests or multiple listeners — the client matches on
    // top-level `id`, falling back to `type` when absent. `data` stays the section verbatim.
    // Re-print is cheap; config commands are infrequent.
    if (id_s && out) {
        cJSON* o = cJSON_Parse(out);
        if (o) {
            cJSON_DeleteItemFromObject(o, "id");   // de-dup if a builder ever adds one
            cJSON_AddStringToObject(o, "id", id_s);
            char* re = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (re) { free(out); out = re; }
        }
    }

    ESP_LOGI(TAG, "cmd dispatch: cmd='%s' section='%s' id='%s' → %s",
             cmd_s, sec_s, id_s ? id_s : "(none)", out ? "response" : "(null)");
    cJSON_Delete(root);
    return out;
}

// System configuration handler

esp_err_t rbwebserver_api_system_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "   ==== GET 2 /api/system/system-config request");
    add_cors_headers(req);
    
    if (rbwebserver_core_ext_cfg()) {
        ESP_LOGI(TAG, "   ==== GET 2 rbwebserver_core_ext_cfg() OK");
        char* json_string = rbgrid_json_create_system_config(rbwebserver_core_ext_cfg());
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
        
        if (rbwebserver_core_ext_cfg() && rbwebserver_core_ext_cfg()->base_hardware_config && rbwebserver_core_json_ctx()) {
            char* json_string = rbgrid_json_create_hardware_config(rbwebserver_core_ext_cfg()->base_hardware_config, rbwebserver_core_json_ctx());
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
    
    if (rbwebserver_core_ext_cfg() && rbwebserver_core_ext_cfg()->base_hardware_config && rbwebserver_core_json_ctx()) {
        char* json_string = rbgrid_json_create_time_config(rbwebserver_core_ext_cfg()->base_hardware_config, rbwebserver_core_json_ctx());
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
        if (rbwebserver_core_json_ctx()) {
            char* json_string = rbgrid_json_create_rbpower_tariff_plan_config(rbwebserver_core_json_ctx());
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
    if (rbwebserver_core_ext_cfg() && rbwebserver_core_json_ctx()) {
        char* json_string = rbgrid_json_create_telemetry_config(rbwebserver_core_ext_cfg(), rbwebserver_core_json_ctx());
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
    
    if (rbwebserver_core_json_ctx()) {
        char* json_string = rbgrid_json_create_wifi_config(rbwebserver_core_json_ctx());
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
        
        if (rbwebserver_core_json_ctx()) {
            // Build under the g_home_network lock — this runs on the httpd task while the
            // core-0 / esp_timer tasks mutate the units array.
            rbpower_home_lock();
            char* json_string = rbgrid_json_create_units_config(rbwebserver_core_json_ctx());
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

// DELETE /api/v1/units/{hexid} (documented below)

esp_err_t rbwebserver_api_unit_delete_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;   // DELETE is a write: admin token required
    httpd_resp_set_type(req, "application/json");

    // Path is /api/v1/units/<hexid> — the id is the segment after the last '/'.
    const char* last = strrchr(req->uri, '/');
    rbpower_unit_id_t unit_id = 0;
    if (!last || last[1] == '\0' || sscanf(last + 1, "%hx", &unit_id) != 1 || unit_id == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid unit id — expect /api/v1/units/<hex>\"}");
        return ESP_OK;
    }

    /* Capture the point slug before removal — it gets tombstoned so a future bind can't
     * silently adopt this identity (resurrect explicitly with reuse:true). */
    char dead_point[33] = {0};
    int dead_role = -1;
    {
        const rbpower_unit_definition_t* ddef = rbpower_home_get_unit_definition(unit_id);
        if (ddef && ddef->point[0]) strncpy(dead_point, ddef->point, sizeof(dead_point) - 1);
        if (ddef) dead_role = (int)ddef->role;   /* HA discovery key rule (main/alt/%04X) */
    }

    rbpower_err_t e = rbpower_home_remove_unit(unit_id);
    char body[160];
    if (e == RBPOWER_ERR_INVALID_ARG) {            // unknown / already removed (idempotent → 404)
        httpd_resp_set_status(req, "404 Not Found");
        snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"unit %04X not found\"}", unit_id);
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }
    if (e == RBPOWER_ERR_INVALID_STATE) {          // has children
        httpd_resp_set_status(req, "409 Conflict");
        snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"unit %04X has children — delete them first\"}", unit_id);
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }
    // Success — persist units.json without the unit (sensor config untouched = cascade unit-only).
    if (dead_point[0]) rbgrid_point_tombstone_add(dead_point);   /* before save → persisted */
    rbgrid_json_ha_unpublish_unit(unit_id, dead_role);           /* HA: drop the ghost entities */
    esp_err_t su = rbgrid_storage_save_units(NULL);
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"removed\":\"%04X\",\"persist\":\"%s\"}",
             unit_id, esp_err_to_name(su));
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, body);
    ESP_LOGW(TAG, "DELETE unit 0x%04X → removed + persisted", unit_id);
    return ESP_OK;
}

/* POST /api/v1/units — bind a sensor to a NEW unit (the inverse of
 * DELETE). Body: {sensor:{iface,bus/module/ch|channel}, name, role?, stat_class?, location?}. Creates
 * the unit at runtime (rbpower_home_create_unit → source bind hook sets the rbAmp fleet entry) and
 * persists units.json. 201 {status,unit_id,persist}; 409 already-bound; 400 bad/sensor-not-found. */
esp_err_t rbwebserver_api_unit_create_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;   // POST is a write: admin token required
    httpd_resp_set_type(req, "application/json");

    if (req->content_len <= 0 || req->content_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"empty or oversized body\"}");
        return ESP_OK;
    }
    char* buf = malloc(req->content_len + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int total = 0, remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf + total, remaining);
        if (r <= 0) {
            free(buf);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"body recv failed\"}");
            return ESP_OK;
        }
        total += r; remaining -= r;
    }
    buf[total] = '\0';

    rbgrid_create_unit_req_t cu;
    rbgrid_json_err_t pe = rbgrid_json_parse_create_unit(buf, &cu);
    free(buf);
    if (pe != RBGRID_JSON_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid body — need sensor.iface + addressing + name\"}");
        return ESP_OK;
    }

    rbpower_unit_id_t uid = 0;
    int br = rbgrid_config_bind_unit(&cu, &uid);
    char body[224];
    if (br == RBGRID_CONFIG_ERR_DUPLICATE_SENSOR) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"SENSOR_BOUND\",\"message\":\"sensor already bound\"}");
        return ESP_OK;
    }
    if (br == RBGRID_CONFIG_ERR_POINT_TOMBSTONED) {   /* retired slug — resurrect with reuse:true */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"POINT_TOMBSTONED\",\"message\":\"point slug retired — pass reuse:true to resurrect\"}");
        return ESP_OK;
    }
    if (br == RBGRID_CONFIG_ERR_POINT_TAKEN) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"POINT_TAKEN\",\"message\":\"point slug already in use\"}");
        return ESP_OK;
    }
    if (br == RBGRID_CONFIG_ERR_NO_MEMORY) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"unit network full\"}");
        return ESP_OK;
    }
    if (br != RBGRID_CONFIG_OK) {       // INVALID_PARAM → sensor not found / unsupported iface / bad fields
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"sensor not found, unsupported iface or invalid point slug\"}");
        return ESP_OK;
    }
    // Success — persist units.json with the new unit (the rbAmp fleet binding was persisted by the hook).
    esp_err_t su = rbgrid_storage_save_units(NULL);
    const rbpower_unit_definition_t* nd = rbpower_home_get_unit_definition(uid);
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"unit_id\":\"%04X\",\"point\":\"%s\",\"persist\":\"%s\"}",
             uid, nd ? nd->point : "", esp_err_to_name(su));
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_sendstr(req, body);
    ESP_LOGW(TAG, "POST /api/v1/units → bound unit 0x%04X + persisted", uid);
    rbgrid_ha_discovery_request();   /* HA entities for the new unit now, not at the next MQTT reconnect */
    return ESP_OK;
}

/* POST /api/v1/units/{hex}/rebind {module,ch}: move a live I2C unit onto another fleet sensor;
 * id/point/history/slot kept. POST /api/v1/units/{hex}/rename {point,reuse?}: change the point slug
 * (old slug → tombstone). One handler for both (URI tail picks the op); persists units.json. */
/* SCT- models the user may select: the registry's characterized codes, plus SCT-013-100 (code 5) — the registry still
 * marks it reserved, but rbAmp BASIC firmware 1.4.19 accepts it (G 2.1622, NF 12) and it is in use on real
 * modules (the registry entry is to be corrected in a later rbAmp library release). */
static bool ct_model_selectable(const rbamp_sensor_model_info_t *mi)
{
    if (mi->status != RBAMP_MODEL_STATUS_RESERVED) return true;
    return mi->sensor_class == RBAMP_SENSOR_CLASS_SCT_013 && mi->code == RBAMP_CT_SCT013_100;
}

esp_err_t rbwebserver_api_unit_mutate_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");

    /* /api/v1/units/{hex}            → params patch (unit edit)
     * /api/v1/units/{hex}/(rebind|rename) → rebind / rename ops */
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
    if (!rbpower_home_get_unit_definition(uid)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"unit not found\"}");
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"empty or oversized body\"}");
        return ESP_OK;
    }
    char buf[513];
    int total = 0, remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf + total, remaining);
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"body recv failed\"}");
            return ESP_OK;
        }
        total += r; remaining -= r;
    }
    buf[total] = '\0';
    cJSON* root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid json\"}");
        return ESP_OK;
    }

    int rc;
    char body[400];
    if (nmatch == 1) {
        /* PARAMS PATCH — {name?, short_name?, phase?, rated_power_w?, max_current_a?, role?,
         * group_id?, role_trunk?, invert_sign?}: live-apply to the definition + persist. Supply roles are
         * topology (specialized main/alt structs built at init) — changing to/from them is
         * NOT a patch (use the wizard/rebind), 409. Load-class role changes apply live for
         * grouping/labels; the period mask recomputes on the next reboot. */
        rbpower_unit_definition_t* def = rbpower_home_get_unit_definition_mutable(uid);
        if (!def) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"unit not found\"}");
            return ESP_OK;
        }
        cJSON* j;
        /* validate everything first — a patch is all-or-nothing */
        if ((j = cJSON_GetObjectItem(root, "short_name")) &&
            (!cJSON_IsString(j) || strlen(j->valuestring) > 24)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"field\":\"short_name\",\"message\":\"string, max 24 bytes utf-8\"}");
            return ESP_OK;
        }
        if ((j = cJSON_GetObjectItem(root, "phase")) &&
            (!cJSON_IsNumber(j) || j->valueint < 0 || j->valueint > 7)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"field\":\"phase\",\"message\":\"0..7 (0=unset, 1..3=L1..L3, 4=3P, 5..7=L1L2/L2L3/L3L1)\"}");
            return ESP_OK;
        }
        if (((j = cJSON_GetObjectItem(root, "rated_power_w")) && (!cJSON_IsNumber(j) || j->valuedouble < 0)) ||
            ((j = cJSON_GetObjectItem(root, "max_current_a")) && (!cJSON_IsNumber(j) || j->valuedouble < 0))) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"rated_power_w/max_current_a: number >= 0 (0 = unset)\"}");
            return ESP_OK;
        }
        const rbamp_sensor_model_info_t *ctm = NULL;
        if ((j = cJSON_GetObjectItem(root, "ct_code")) && cJSON_IsNumber(j) && j->valuedouble >= 1 && j->valuedouble <= 255)
            ctm = rbamp_sensor_model_lookup(RBAMP_SENSOR_CLASS_SCT_013, (uint8_t)j->valuedouble);
        if (j && cJSON_GetObjectItem(root, "ct_code") &&
            (!ctm || !ct_model_selectable(ctm) || j->valuedouble != (double)(int64_t)j->valuedouble)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"error_code\":\"OUT_OF_RANGE\",\"field\":\"ct_code\",\"message\":\"SCT-013 model code from schema enums.ct_model\"}");
            return ESP_OK;
        }
        if ((j = cJSON_GetObjectItem(root, "ct_code")) && cJSON_IsNumber(j)) {   /* applied on the module first: fail → 409, nothing else changed */
            esp_err_t ce = rbpower_unit_set_ct((rbpower_unit_id_t)uid, (uint8_t)j->valuedouble);
            if (ce != ESP_OK) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, ce == ESP_ERR_NOT_FOUND ? "400 Bad Request" : "409 Conflict");
                char eb[160];
                snprintf(eb, sizeof(eb), "{\"status\":\"error\",\"field\":\"ct_code\",\"message\":\"%s\"}",
                         ce == ESP_ERR_NOT_FOUND ? "unit is not an rbAmp current channel" :
                         ce == ESP_ERR_NOT_SUPPORTED ? "CT- series module: the sensor is factory-paired and calibrated, its model is fixed" :
                         ce == ESP_ERR_INVALID_STATE ? "module has no sensor class (uncalibrated) or is busy — the model cannot be selected" :
                         esp_err_to_name(ce));
                httpd_resp_sendstr(req, eb);
                return ESP_OK;
            }
        }
        if ((j = cJSON_GetObjectItem(root, "charge_threshold_w")) && (!cJSON_IsNumber(j) || j->valuedouble < 0 || j->valuedouble > 100000 ||
                                                                     j->valuedouble != (double)(int64_t)j->valuedouble)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"error_code\":\"OUT_OF_RANGE\",\"field\":\"charge_threshold_w\",\"message\":\"integer 0..100000 W (0 = system.ev)\"}");
            return ESP_OK;
        }
        if ((j = cJSON_GetObjectItem(root, "idle_timeout_min")) && (!cJSON_IsNumber(j) || j->valuedouble < 0 || j->valuedouble > 1440 ||
                                                                   j->valuedouble != (double)(int64_t)j->valuedouble)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"error_code\":\"OUT_OF_RANGE\",\"field\":\"idle_timeout_min\",\"message\":\"integer 0..1440 min (0 = system.ev)\"}");
            return ESP_OK;
        }
        int new_role = -1;
        if ((j = cJSON_GetObjectItem(root, "role")) != NULL) {
            if (cJSON_IsString(j)) {
                for (unsigned r = 0; rbpower_load_role_name(r); r++)
                    if (strcmp(j->valuestring, rbpower_load_role_name(r)) == 0) { new_role = (int)r; break; }
            }
            if (new_role < 0) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_sendstr(req, "{\"status\":\"error\",\"field\":\"role\",\"message\":\"unknown role — see schema enums.role\"}");
                return ESP_OK;
            }
            bool cur_supply = (def->role == RBPOWER_ROLE_MAIN_SUPPLY || def->role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY);
            bool new_supply = (new_role == RBPOWER_ROLE_MAIN_SUPPLY || new_role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY);
            if (cur_supply || new_supply) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"ROLE_SUPPLY_LOCKED\",\"message\":\"supply roles are topology — change via wizard/rebind, not patch\"}");
                return ESP_OK;
            }
        }
        /* apply */
        rbpower_home_lock();
        if ((j = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(j) && j->valuestring[0]) {
            strncpy(def->name, j->valuestring, sizeof(def->name) - 1);
            def->name[sizeof(def->name) - 1] = '\0';
        }
        if ((j = cJSON_GetObjectItem(root, "short_name")) && cJSON_IsString(j)) {
            strncpy(def->short_name, j->valuestring, sizeof(def->short_name) - 1);
            def->short_name[sizeof(def->short_name) - 1] = '\0';
        }
        if ((j = cJSON_GetObjectItem(root, "phase")) && cJSON_IsNumber(j))         def->phase = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(root, "rated_power_w")) && cJSON_IsNumber(j)) def->rated_power_w = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(root, "max_current_a")) && cJSON_IsNumber(j)) def->max_current_a = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(root, "group_id")) && cJSON_IsNumber(j))      def->group_id = (uint16_t)j->valueint;
        if ((j = cJSON_GetObjectItem(root, "role_trunk")) && cJSON_IsNumber(j))    def->role_trunk = (uint8_t)(j->valueint ? 1 : 0);
        if ((j = cJSON_GetObjectItem(root, "charge_threshold_w")) && cJSON_IsNumber(j)) def->ev_charge_threshold_w = (uint32_t)j->valuedouble;
        if ((j = cJSON_GetObjectItem(root, "idle_timeout_min")) && cJSON_IsNumber(j))   def->ev_idle_timeout_min = (uint16_t)j->valuedouble;
        /* invert_sign (CT orientation) — LIVE apply: consumed at the next inject boundary. The full
         * units-config POST only persists it (apply-on-reboot); this is the live path. Mirrors into sources[0] which every loader/serializer reads. */
        if ((j = cJSON_GetObjectItem(root, "invert_sign")) && cJSON_IsBool(j)) {
            if (def->sources_count == 0) def->sources_count = 1;
            def->sources[0].invert_sign = cJSON_IsTrue(j);
        }
        if (new_role >= 0) def->role = (rbpower_load_role_t)new_role;
        rbpower_home_unlock();
        cJSON_Delete(root);

        esp_err_t supd = rbgrid_storage_save_units(NULL);
        char en[96], esn[64];   /* a name with " or \ broke the 200 body */
        rbapi_json_escape(def->name, en, sizeof(en));
        rbapi_json_escape(def->short_name, esn, sizeof(esn));
        snprintf(body, sizeof(body),
                 "{\"status\":\"ok\",\"unit_id\":\"%04X\",\"name\":\"%s\",\"short_name\":\"%s\","
                 "\"phase\":%u,\"rated_power_w\":%.1f,\"max_current_a\":%.1f,\"role\":\"%s\","
                 "\"group_id\":%u,\"role_trunk\":%u,\"charge_threshold_w\":%lu,\"idle_timeout_min\":%u,"
                 "\"requires_reboot\":false,\"persist\":\"%s\"}",
                 uid, en, esn, def->phase, (double)def->rated_power_w,
                 (double)def->max_current_a, rbpower_load_role_name((unsigned)def->role),
                 def->group_id, def->role_trunk, (unsigned long)def->ev_charge_threshold_w,
                 (unsigned)def->ev_idle_timeout_min, esp_err_to_name(supd));
        httpd_resp_sendstr(req, body);
        ESP_LOGW(TAG, "POST %s → params patched (unit 0x%04X)", req->uri, uid);
        return ESP_OK;
    }
    if (strcmp(op, "rebind") == 0) {
        cJSON* jm = cJSON_GetObjectItem(root, "module");
        cJSON* jc = cJSON_GetObjectItem(root, "ch");
        if (!cJSON_IsNumber(jm) || !cJSON_IsNumber(jc)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"need module + ch\"}");
            return ESP_OK;
        }
        rc = rbgrid_config_rebind_unit(uid, (uint8_t)jm->valueint, (uint8_t)jc->valueint);
        cJSON_Delete(root);
        if (rc == RBGRID_CONFIG_ERR_DUPLICATE_SENSOR) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"SENSOR_BOUND\",\"message\":\"target sensor bound to another unit\"}");
            return ESP_OK;
        }
        if (rc != RBGRID_CONFIG_OK) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"not an i2c unit or target sensor not in fleet\"}");
            return ESP_OK;
        }
    } else if (strcmp(op, "rename") == 0) {
        cJSON* jp = cJSON_GetObjectItem(root, "point");
        bool reuse = cJSON_IsTrue(cJSON_GetObjectItem(root, "reuse"));
        if (!cJSON_IsString(jp) || !jp->valuestring[0]) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"need point\"}");
            return ESP_OK;
        }
        rc = rbgrid_config_rename_point(uid, jp->valuestring, reuse);
        cJSON_Delete(root);
        if (rc == RBGRID_CONFIG_ERR_POINT_TAKEN) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"POINT_TAKEN\",\"message\":\"point slug already in use\"}");
            return ESP_OK;
        }
        if (rc == RBGRID_CONFIG_ERR_POINT_TOMBSTONED) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"POINT_TOMBSTONED\",\"message\":\"point slug retired — pass reuse:true to resurrect\"}");
            return ESP_OK;
        }
        if (rc != RBGRID_CONFIG_OK) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid point slug ([a-z0-9_.-]{1,32})\"}");
            return ESP_OK;
        }
    } else {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"unknown op — rebind|rename\"}");
        return ESP_OK;
    }

    esp_err_t su = rbgrid_storage_save_units(NULL);
    const rbpower_unit_definition_t* d = rbpower_home_get_unit_definition(uid);
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"unit_id\":\"%04X\",\"point\":\"%s\",\"module\":%d,\"ch\":%u,\"persist\":\"%s\"}",
             uid, d ? d->point : "", d ? (int)d->device_uid : -1, d ? d->channel_idx : 0,
             esp_err_to_name(su));
    httpd_resp_sendstr(req, body);
    ESP_LOGW(TAG, "POST %s → ok (unit 0x%04X)", req->uri, uid);
    rbgrid_ha_discovery_request();   /* rename → HA device/entity names follow; rebind → harmless refresh */
    return ESP_OK;
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

    // 4. Convert unit_id from hex string
    rbpower_unit_id_t unit_id = 0;
    if (sscanf(unit_id_str, "%hx", &unit_id) != 1) {
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
                    if (ts == 0 || ts <= since || ts < RBAPI_TS_SANE_MIN) continue;
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
                if (ts != 0 && ts < RBAPI_TS_SANE_MIN) { cJSON_DeleteItemFromArray(data, i); dropped = true; }
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

/* GET /api/v1/economics?unit=<hex> — the tariff/economics state that is otherwise only visible through
 * console diagnostics (zonestats/netmeter/progtier): per-zone cost+revenue, net-metering credit
 * bank, progressive-tier state. Reads the already-computed getters in rbhome_tariff.c (no new compute).
 * Tier-2 composite. See documents/development/API_EVOLUTION_GUIDE.md. */
/**
 * @brief Build the /api/v1/economics JSON for a unit.
 *
 * Shared by the HTTP handler and the MQTT `cmd get economics` path. Reads the tariff getters
 * (read-only); call from task context.
 *
 * @param[in] unit  Unit id.
 * @return malloc'd JSON string (caller frees), or NULL on OOM.
 */
char *rbgrid_build_economics_json(rbpower_unit_id_t unit)
{
    // Read the computed economics state (all getters are read-only, NULL-safe).
    float cost_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0}, rev_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0};
    rbpower_tariff_get_zone_stats(unit, cost_zone, rev_zone);
    double nm_in = 0, nm_out = 0, nm_bal = 0, nm_earn = 0, nm_used = 0, nm_bill = 0;
    rbpower_tariff_get_net_metering(unit, &nm_in, &nm_out, &nm_bal, &nm_earn, &nm_used, &nm_bill);
    bool pg_en = false; double pg_cum = 0; int pg_tier = 0; float pg_mult = 1.0f;
    rbpower_tariff_get_progressive(unit, &pg_en, &pg_cum, &pg_tier, &pg_mult);

    static const char *ZNAME[RBPOWER_TARIFF_ZONE_COUNT] = {
        "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak"
    };

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "v", 1);
    char uidhex[8]; snprintf(uidhex, sizeof(uidhex), "%04X", (unsigned)unit);
    cJSON_AddStringToObject(root, "unit", uidhex);
    cJSON_AddStringToObject(root, "currency", rbpower_tariff_currency());

    cJSON *zones = cJSON_AddArrayToObject(root, "zones");
    for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        cJSON *zo = cJSON_CreateObject();
        cJSON_AddStringToObject(zo, "zone", ZNAME[z]);
        cJSON_AddNumberToObject(zo, "cost", round(cost_zone[z] * 10000) / 10000);
        cJSON_AddNumberToObject(zo, "revenue", round(rev_zone[z] * 10000) / 10000);
        cJSON_AddItemToArray(zones, zo);
    }
    cJSON *nm = cJSON_AddObjectToObject(root, "net_metering");
    cJSON_AddNumberToObject(nm, "period_import_kwh", round(nm_in * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "period_export_kwh", round(nm_out * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "credit_balance_kwh", round(nm_bal * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "lifetime_earned_kwh", round(nm_earn * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "lifetime_used_kwh", round(nm_used * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "last_billable_net_kwh", round(nm_bill * 10000) / 10000);
    cJSON *pg = cJSON_AddObjectToObject(root, "progressive");
    cJSON_AddBoolToObject(pg, "enabled", pg_en);
    cJSON_AddNumberToObject(pg, "cumulative_import_kwh", round(pg_cum * 10000) / 10000);
    cJSON_AddNumberToObject(pg, "tier", pg_tier);
    cJSON_AddNumberToObject(pg, "rate_mult", round(pg_mult * 100) / 100);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
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
    if (strlen(unit_str) == 0 || sscanf(unit_str, "%hx", &unit) != 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Required: unit=<hex>");
        return ESP_FAIL;
    }
    char *out = rbgrid_build_economics_json(unit);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

/* map "hour".."year" → rbpower_stat_period_type_t (the period rgrid_get_period_history wants). -1 = bad. */
static int rbapi_period_str_to_stat(const char *s)
{
    if (!strcmp(s, "hour"))  return RBPOWER_STAT_PERIOD_1HOUR;
    if (!strcmp(s, "day"))   return RBPOWER_STAT_PERIOD_DAILY;
    if (!strcmp(s, "week"))  return RBPOWER_STAT_PERIOD_WEEKLY;
    if (!strcmp(s, "month")) return RBPOWER_STAT_PERIOD_MONTHLY;
    if (!strcmp(s, "year"))  return RBPOWER_STAT_PERIOD_YEARLY;
    return -1;
}
/* map "hour".."year" → rbpower_period_t (flash period, for the valid-record-count query). -1 = bad. */
static int rbapi_period_str_to_flash(const char *s)
{
    if (!strcmp(s, "hour"))  return RBPOWER_PERIOD_HOUR;
    if (!strcmp(s, "day"))   return RBPOWER_PERIOD_DAY;
    if (!strcmp(s, "week"))  return RBPOWER_PERIOD_WEEK;
    if (!strcmp(s, "month")) return RBPOWER_PERIOD_MONTH;
    if (!strcmp(s, "year"))  return RBPOWER_PERIOD_YEAR;
    return -1;
}

/* Iterate VALID period records (skipping empty 0xFFFFFFFF flash slots), optionally windowed by unix
 * from/to, invoking cb per record. Memory-safe (reads in small batches). Bounded by the VALID record
 * count (rbgrid_history_get_buffer_info) so it never decodes the unwritten 0xFF ring tail — the bug the
 * raw rbpower_flash_read_records would cause (it returns physical slots incl. empties). Returns valid
 * record count visited (pre-window). Shared by export.csv + summary. */
#define RBAPI_REC_BATCH 20            ///< Records read from flash per batch
#define RBAPI_TS_EMPTY  0xFFFFFFFFu   ///< Timestamp of an unwritten (erased) flash slot
static uint32_t rbapi_foreach_valid_record(rbpower_unit_id_t unit, int stat_period, int flash_period,
                                           uint32_t from, uint32_t to,
                                           void (*cb)(const rbpower_period_statistics_t *, void *), void *ctx)
{
    uint32_t valid_total = 0;
    rbgrid_history_get_buffer_info(unit, (rbpower_period_t)flash_period, RBPOWER_DATA_TYPE_ELECTRICAL, &valid_total);
    if (valid_total == 0) return 0;

    rbpower_period_statistics_t *st = malloc(RBAPI_REC_BATCH * sizeof(rbpower_period_statistics_t));
    if (!st) return 0;

    uint32_t emitted = 0, scanned = 0, idx = 0;
    uint32_t last_ts = UINT32_MAX;                  // rows come newest-first: emit strictly decreasing ts only
    const uint32_t scan_cap = valid_total + 4096;   // safety: never loop past the physical ring
    while (emitted < valid_total && scanned < scan_cap) {
        uint32_t got = 0;
        if (rbgrid_get_period_history(unit, (rbpower_stat_period_type_t)stat_period, idx, RBAPI_REC_BATCH, st, &got) != RBGRID_OK)
            break;
        if (got == 0) break;
        for (uint32_t i = 0; i < got && emitted < valid_total; i++) {
            scanned++;
            uint32_t ts = (uint32_t)(st[i].period_start_us / 1000000LL);
            if (ts == 0 || ts == RBAPI_TS_EMPTY) continue;   // unwritten/empty slot — skip (the bug fix)
            if (ts < RBAPI_TS_SANE_MIN) continue;            // garbage: fake-clock / 2024-fallback records
            /* offset paging: a record written between two batches shifts every offset by one, so the next batch
             * repeats the last row — CSV/summary counted that period twice */
            if (ts >= last_ts) continue;
            last_ts = ts;
            emitted++;
            if (ts < from || ts > to) continue;              // outside the requested window
            cb(&st[i], ctx);
        }
        if (got < RBAPI_REC_BATCH) break;                    // physical ring exhausted
        idx += got;
        /* a background (priority-1) caller would otherwise run this flash scan back to back and starve
         * the IDLE task of its core → task watchdog (observed during an OTA upload: starved IDLE0) */
        if (uxTaskPriorityGet(NULL) < 5) vTaskDelay(1);
    }
    free(st);
    return emitted;
}

/** @brief Context of the CSV row emitter (callback for rbapi_foreach_valid_record). */
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

/** @brief Summary accumulator (callback context for rbapi_foreach_valid_record). */
struct rbapi_sum_ctx {
    double e_in  /**< Imported (consumed) energy, Wh */,
           e_out /**< Exported (supplied) energy, Wh */,
           cost  /**< Cost of consumed energy, major currency units */,
           rev;  ///< Revenue for supplied energy, major currency units
    uint32_t records;   ///< Number of records summed
};
static void rbapi_sum_emit(const rbpower_period_statistics_t *s, void *vctx)
{
    struct rbapi_sum_ctx *c = (struct rbapi_sum_ctx *)vctx;
    c->e_in += s->energy_period.consumed_wh;
    c->e_out += s->energy_period.supplied_wh;
    c->cost += s->result_base.cost_consumed;
    c->rev += s->result_base.revenue_supplied;
    c->records++;
}


/* "Home vampires" baseline (standby load): median of the hourly average power of the
 * night hours 01:00–05:00 local (record midpoint) over the last 7 days, full hours only (dur ≥ 3540 s). One
 * value for the app and HA. Recomputed at most once an hour (flash scan of ≤168 HOUR records). */
/** @brief Collector for the night-baseline scan (callback context for rbapi_foreach_valid_record). */
struct rbapi_base_ctx {
    float v[40];            ///< Hourly average powers of the qualifying night hours, W
    int n;                  ///< Number of values in @c v
    uint8_t nights_seen[8]; ///< Day-of-year (low byte) of each distinct night seen
    int nights;             ///< Number of distinct nights
};
static void rbapi_base_emit(const rbpower_period_statistics_t *s, void *vctx)
{
    struct rbapi_base_ctx *c = (struct rbapi_base_ctx *)vctx;
    uint32_t ts = (uint32_t)(s->period_start_us / 1000000LL), dur = s->v4_dur_s ? s->v4_dur_s : 3600;
    if (dur < 3540 || c->n >= 40) return;
    time_t mid = (time_t)(ts - dur / 2); struct tm lt; localtime_r(&mid, &lt);
    if (lt.tm_hour < 1 || lt.tm_hour >= 5) return;
    c->v[c->n++] = (float)(s->energy_period.consumed_wh * 3600.0 / dur);
    uint8_t day = (uint8_t)lt.tm_yday;   /* distinct nights (yday is enough inside a 7-day window) */
    for (int i = 0; i < c->nights && i < 8; ++i) if (c->nights_seen[i] == day) return;
    if (c->nights < 8) c->nights_seen[c->nights++] = day;
}
static int rbapi_cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
/* The scan walks every valid HOUR record of the unit (whole history, then windows to 7 days): 13–18 s of
 * flash reads on a hub with a long history. It used to run INLINE in the telemetry builder, i.e. under the
 * home lock — the 3-s stats chain and every HTTP request stalled for that long, every 2 min while the result
 * was empty (period events paused 10–13 s; a telemetry GET took 18 s).
 * Now a low-priority task owns the scan; the telemetry path only reads the cached result. */
static volatile float s_bw_w = NAN;
static volatile int s_bw_h = 0, s_bw_n = 0;
static volatile rbpower_unit_id_t s_bw_unit = 0, s_bw_done_unit = 0xFFFF;
static TaskHandle_t s_bw_task = NULL;
static void rbapi_base_w_task(void *arg)
{
    (void)arg;
    for (;;) {
        rbpower_unit_id_t unit = s_bw_unit;
        time_t now = time(NULL);
        if (unit == 0 || now < 1600000000) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        struct rbapi_base_ctx *c = calloc(1, sizeof(*c));
        if (!c) { vTaskDelay(pdMS_TO_TICKS(60000)); continue; }
        int64_t t0 = esp_timer_get_time();
        uint32_t seen = rbapi_foreach_valid_record(unit, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR,
                                                   (uint32_t)now - 7u * 86400u, (uint32_t)now, rbapi_base_emit, c);
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        if (ms > 2000) rbpower_diag_printf("base_w scan %lu records %lldms (background)", (unsigned long)seen, (long long)ms);
        float w = NAN;
        if (c->n > 0) { qsort(c->v, c->n, sizeof(float), rbapi_cmp_f);
                        w = (c->n & 1) ? c->v[c->n / 2] : 0.5f * (c->v[c->n / 2 - 1] + c->v[c->n / 2]); }
        s_bw_w = w; s_bw_h = c->n; s_bw_n = c->nights; s_bw_done_unit = unit;
        int h = c->n;
        free(c);
        /* An EMPTY result (right after boot the stats store / clock may not be ready yet) is retried after
         * 2 min; a non-empty result is refreshed hourly (otherwise base_w stayed null for an hour after boot). */
        vTaskDelay(pdMS_TO_TICKS(h > 0 ? 3600000 : 120000));
    }
}
/**
 * @brief Night-time standby baseline ("home vampires") of a unit, from the cached background scan.
 *
 * Non-blocking: the first call starts a low-priority task that scans the HOUR records; until it has a
 * result for @p unit the outputs are NAN / 0. Called from the telemetry builder.
 *
 * @param[in]  unit    Unit id to compute the baseline for (becomes the scan target).
 * @param[out] base_w  Median night-hour average power, W (NAN when not available; may be NULL).
 * @param[out] hours   Number of night hours used (may be NULL).
 * @param[out] nights  Number of distinct nights used (may be NULL).
 * @return true if a non-empty result for @p unit is available.
 */
bool rbgrid_base_w(rbpower_unit_id_t unit, float *base_w, int *hours, int *nights)
{
    s_bw_unit = unit;
    if (!s_bw_task) xTaskCreate(rbapi_base_w_task, "base_w", 4096, NULL, 1, &s_bw_task);
    bool mine = (s_bw_done_unit == unit);
    if (base_w) *base_w = mine ? s_bw_w : NAN;
    if (hours)  *hours = mine ? s_bw_h : 0;
    if (nights) *nights = mine ? s_bw_n : 0;
    return mine && s_bw_h > 0;
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
                if (ts < RBAPI_TS_SANE_MIN) continue;        // fake-clock garbage (see above)
                if (ts < from || ts > to) continue;
                cJSON_AddItemToArray(arr, cJSON_Duplicate(rec, 1));
            }
        }
        cJSON_Delete(root);
    }
    return arr;
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
    int sp = rbapi_period_str_to_stat(period_str);
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

    int fp = rbapi_period_str_to_flash(period_str);
    struct rbapi_csv_ctx cctx = { req, t_elec, t_energy, t_econ };
    rbapi_foreach_valid_record(unit, sp, fp, from, to, rbapi_csv_emit, &cctx);  // bounded + skips empties

    httpd_resp_send_chunk(req, NULL, 0);   // end stream
    return ESP_OK;
}

/* GET /api/v1/summary?unit=<hex>&period=hour&from=&to= — the "real-bill" roll-up:
 * range totals (energy in/out/net + cost/revenue/net summed over the period records in the window) +
 * per-zone cost/revenue (current cumulative, from the tariff getter). Tier-2 composite (additive). */
/**
 * @brief Build the /api/v1/summary JSON (range totals + per-zone cost/revenue).
 *
 * Shared by the HTTP handler and the MQTT `cmd get summary` path. Scans flash records; call from
 * task context.
 *
 * @param[in] unit        Unit id.
 * @param[in] sp          Stat-period enum (from rbapi_period_str_to_stat).
 * @param[in] period_str  Period name ("hour".."year"); echoed and mapped to the flash period.
 * @param[in] from        Window start, epoch s (inclusive).
 * @param[in] to          Window end, epoch s (inclusive).
 * @return malloc'd JSON string (caller frees), or NULL on OOM.
 */
char *rbgrid_build_summary_json(rbpower_unit_id_t unit, int sp, const char *period_str, uint32_t from, uint32_t to)
{
    // Range totals: sum the VALID period records in [from,to] (bounded + skips empties — same helper as CSV).
    struct rbapi_sum_ctx sc = {0};
    rbapi_foreach_valid_record(unit, sp, rbapi_period_str_to_flash(period_str), from, to, rbapi_sum_emit, &sc);

    float cost_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0}, rev_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0};
    rbpower_tariff_get_zone_stats(unit, cost_zone, rev_zone);
    static const char *ZN[RBPOWER_TARIFF_ZONE_COUNT] = {
        "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak" };

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "v", 1);
    char uidhex[8]; snprintf(uidhex, sizeof(uidhex), "%04X", (unsigned)unit);
    cJSON_AddStringToObject(root, "unit", uidhex);
    cJSON_AddStringToObject(root, "period", period_str);
    cJSON_AddStringToObject(root, "currency", rbpower_tariff_currency());
    cJSON *tot = cJSON_AddObjectToObject(root, "totals");
    cJSON_AddNumberToObject(tot, "records", sc.records);
    cJSON_AddNumberToObject(tot, "energy_in_kwh", round(sc.e_in / 1000.0 * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "energy_out_kwh", round(sc.e_out / 1000.0 * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "net_kwh", round((sc.e_in - sc.e_out) / 1000.0 * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "cost", round(sc.cost * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "revenue", round(sc.rev * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "net_cost", round((sc.cost - sc.rev) * 10000) / 10000);
    // by-zone = current cumulative tariff-zone distribution (NOT windowed by from/to).
    cJSON *bz = cJSON_AddArrayToObject(root, "by_zone");
    for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        cJSON *zo = cJSON_CreateObject();
        cJSON_AddStringToObject(zo, "zone", ZN[z]);
        cJSON_AddNumberToObject(zo, "cost", round(cost_zone[z] * 10000) / 10000);
        cJSON_AddNumberToObject(zo, "revenue", round(rev_zone[z] * 10000) / 10000);
        cJSON_AddItemToArray(bz, zo);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
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
    int sp = rbapi_period_str_to_stat(period_str);
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
/* JSON string body escape for hand-built responses (" \ and control characters) */
static void rbapi_json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)(in ? in : ""); *c && o + 7 < n; ++c) {
        if (*c == '"' || *c == '\\') { out[o++] = '\\'; out[o++] = (char)*c; }
        else if (*c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", *c);
        else out[o++] = (char)*c;
    }
    out[o] = '\0';
}
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
        bool set  = rbapi_load_token(RBAPI_TOKEN_FILE, cur);
        bool rset = rbapi_load_token(RBAPI_TOKEN_READ_FILE, cur);   /* read role */
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
        const char *file = (role && strcmp(role, "read") == 0) ? RBAPI_TOKEN_READ_FILE : RBAPI_TOKEN_FILE;
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
                 rbapi_load_token(RBAPI_TOKEN_FILE, cur) ? "true" : "false",
                 rbapi_load_token(RBAPI_TOKEN_READ_FILE, cur) ? "true" : "false");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET/POST only");
    return ESP_FAIL;
}

/* GET/POST /api/system/security: CORS policy (dev_cors flag + origin allow-list).
 * Both methods admin-gated (unarmed auth = open, as everywhere). POST body:
 * {"dev_cors":bool, "origins":["http://host:port", ...≤4]} → persisted to /sec_cfg + live reload. */
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
                if (cJSON_IsString(o) && o->valuestring[0] && strlen(o->valuestring) < 64 &&
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
        char out[224];
        if (ml > 0)
            snprintf(out, sizeof(out), "{\"exists\":true,\"files\":%d,\"meta\":%s}", n, meta);
        else
            snprintf(out, sizeof(out), "{\"exists\":%s,\"files\":%d}", n > 0 ? "true" : "false", n);
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;
        int n = rbgrid_storage_backup_all();
        httpd_resp_set_type(req, "application/json");
        char out[80]; snprintf(out, sizeof(out), "{\"status\":\"%s\",\"files_backed_up\":%d}", n > 0 ? "OK" : "ERROR", n);
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
    char out[96];
    snprintf(out, sizeof(out), "{\"status\":\"%s\",\"files_restored\":%d,\"requires_reboot\":true}", n > 0 ? "OK" : "ERROR", n);
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/v1/schema: machine-readable contract descriptor. Lists the API
 * version + the endpoint surface (path, methods, tier, params/keys) + the on-flash/telemetry schema versions.
 * This is the drift baseline: a client (or a contract test) fetches /schema + each endpoint and
 * diffs against a recorded golden. Tier 1 = 1:1 struct mirror (immutable); composite = aggregated.
 * See documents/development/API_EVOLUTION_GUIDE.md. Bump RBGRID_API_VERSION on any breaking surface change. */
/* RBGRID_API_VERSION lives in rbwebserver_internal.h (shared with the ui-commit guard) */

/* ISO-4217 minor-unit digits for money display formatting (`money.money_decimals` in /schema).
 * Default 2; the 3-digit dinar/rial group and the common 0-digit currencies are enumerated. */
static int iso4217_minor_digits(const char *code)
{
    static const char *THREE[] = { "BHD", "IQD", "JOD", "KWD", "LYD", "OMR", "TND" };
    static const char *ZERO[]  = { "BIF", "CLP", "DJF", "GNF", "ISK", "JPY", "KMF", "KRW",
                                   "PYG", "RWF", "UGX", "VND", "VUV", "XAF", "XOF", "XPF" };
    for (unsigned i = 0; i < sizeof(THREE) / sizeof(THREE[0]); i++)
        if (strcmp(code, THREE[i]) == 0) return 3;
    for (unsigned i = 0; i < sizeof(ZERO) / sizeof(ZERO[0]); i++)
        if (strcmp(code, ZERO[i]) == 0) return 0;
    return 2;
}

/**
 * @brief Build the /api/v1/schema JSON (API contract descriptor).
 *
 * Shared by the HTTP handler and the MQTT `cmd get schema` path.
 *
 * @return malloc'd JSON string (caller frees), or NULL on OOM.
 */
char *rbgrid_build_schema_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "api_version", RBGRID_API_VERSION);
    cJSON_AddNumberToObject(root, "telemetry_schema", RBGRID_TELEMETRY_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "telemetry_profile", rbgrid_telemetry_get_lean() ? "lean" : "full");
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw", (ad && ad->version[0]) ? ad->version : "0.0.0");

    /* API contract v4 (additive): build flavor, hardware profile, storage state, the
     * per-source electrical key sets and the quality-bit dictionary. */
    cJSON_AddStringToObject(root, "build", "full");
    cJSON_AddStringToObject(root, "hw_profile", "esp32-wrover-psram");
    cJSON_AddStringToObject(root, "storage_state", rbstats_store_state_str());
    cJSON_AddNumberToObject(root, "history_schema_ver", 4);   /* history record JSON forms */
    {   /* time_quality — same source as the telemetry frame */
        extern uint8_t rbtime_quality(uint32_t *sync_age_s_out);
        uint32_t sync_age_s = 0; uint8_t tq = rbtime_quality(&sync_age_s);
        cJSON *tqo = cJSON_AddObjectToObject(root, "time_quality");
        if (tqo) { cJSON_AddNumberToObject(tqo, "tq", tq); cJSON_AddStringToObject(tqo, "time_src", tq ? "sntp" : "none"); cJSON_AddNumberToObject(tqo, "sync_age_s", sync_age_s); }
    }
    {
        /* fields_by_source (per-source electrical key sets; keys will not change) */
        cJSON *fbs = cJSON_AddObjectToObject(root, "fields_by_source");
        cJSON *rbamp = cJSON_AddArrayToObject(fbs, "rbamp");
        static const char *RBAMP_KEYS[] = { "v", "i", "p_act", "v_min", "v_max", "i_min", "i_max", "p_min", "p_max" };
        for (unsigned k = 0; k < sizeof(RBAMP_KEYS) / sizeof(RBAMP_KEYS[0]); k++)
            cJSON_AddItemToArray(rbamp, cJSON_CreateString(RBAMP_KEYS[k]));
        cJSON *esn = cJSON_AddArrayToObject(fbs, "espnow");
        static const char *ESN_KEYS[] = { "v", "i", "p_act" };
        for (unsigned k = 0; k < sizeof(ESN_KEYS) / sizeof(ESN_KEYS[0]); k++)
            cJSON_AddItemToArray(esn, cJSON_CreateString(ESN_KEYS[k]));
        /* quality_bits (contract §1): bit index by name; q=0 = GOOD */
        cJSON *qb = cJSON_AddObjectToObject(root, "quality_bits");
        cJSON_AddNumberToObject(qb, "PARTIAL", 0);
        cJSON_AddNumberToObject(qb, "TIME_UNSYNC", 1);
        cJSON_AddNumberToObject(qb, "CLOCK_STEP", 2);
        cJSON_AddNumberToObject(qb, "SRC_GAP", 3);
        cJSON_AddNumberToObject(qb, "RECOMPUTED", 4);
        cJSON_AddNumberToObject(qb, "ESTIMATED", 5);
        cJSON_AddNumberToObject(qb, "UNSIGNED_SRC", 6);
        cJSON_AddNumberToObject(qb, "OVERRANGE", 7);
        cJSON_AddNumberToObject(qb, "MIGRATED", 8);
    }

    // Explicit capability flags (schema-driven UI; more reliable than inferring from endpoints[]).
    cJSON *cap = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddBoolToObject(cap, "espnow", true);
    cJSON_AddBoolToObject(cap, "economics", true);
    cJSON_AddBoolToObject(cap, "summary", true);
    cJSON_AddBoolToObject(cap, "exportCsv", true);
    cJSON_AddBoolToObject(cap, "telemetryConfigPost", true);
    cJSON_AddBoolToObject(cap, "mqttConfig", true);
    cJSON_AddBoolToObject(cap, "adcUnboundDiscovery", false);   // known gap: /api/v1/sensors lists i2c-unbound only
    cJSON_AddBoolToObject(cap, "historyTimeRange", true);
    cJSON_AddBoolToObject(cap, "auth", true);
    cJSON_AddBoolToObject(cap, "https", true);
    cJSON_AddBoolToObject(cap, "provisioning", true);
    cJSON_AddBoolToObject(cap, "configBackup", true);

    /* Configurable-surface descriptor: units of measure, key format,
     * canonical enums and money formatting — so the UI renders from data instead of hardcoding.
     * Wire invariants these declare: energy = float Wh on EVERY endpoint (mWh exists only inside
     * the v4 flash records); money = float in MAJOR currency units on EVERY endpoint (cents exist
     * only inside the v4 flash records). */
    {
        cJSON *uom = cJSON_AddObjectToObject(root, "units_of_measure");
        cJSON_AddStringToObject(uom, "energy", "Wh");
        cJSON_AddStringToObject(uom, "power", "W");
        cJSON_AddStringToObject(uom, "voltage", "V");
        cJSON_AddStringToObject(uom, "current", "A");
        cJSON_AddStringToObject(uom, "frequency", "Hz");
        cJSON_AddStringToObject(uom, "money", "major");         /* float, major currency units */
        cJSON_AddStringToObject(uom, "rate", "major_per_kwh");  /* tariff rates: currency per kWh */

        cJSON *kf = cJSON_AddObjectToObject(root, "key_format");
        cJSON_AddStringToObject(kf, "unit_key", "HEX4");        /* %04X upper-case, e.g. "0006" */
        /* telemetry still emits legacy "main"/"alt" supply keys; flips to hex at the v2 frame */
        cJSON_AddBoolToObject(kf, "legacy_supply_keys", true);

        cJSON *en = cJSON_AddObjectToObject(root, "enums");
        cJSON *roles = cJSON_AddArrayToObject(en, "role");
        for (unsigned r = 0; rbpower_load_role_name(r); r++)
            cJSON_AddItemToArray(roles, cJSON_CreateString(rbpower_load_role_name(r)));
        cJSON *sp = cJSON_AddArrayToObject(en, "site_profile");
        cJSON_AddItemToArray(sp, cJSON_CreateString("home"));
        cJSON_AddItemToArray(sp, cJSON_CreateString("industrial"));
        cJSON_AddItemToArray(sp, cJSON_CreateString("ev"));
        /* UI language chosen by the installer (system.language; "" = unset → UI falls back to the
         * browser language). The web app applies the device language only when this enum exists. */
        /* empty: any ISO 639-1 code is accepted (language packs on demand) — the web app greys
         * out codes only when this list is non-empty */
        (void)cJSON_AddArrayToObject(en, "language");
        {   /* CT models from the rbAmp model registry (rbamp_sensor_models.h):
             *  ct_model     — SCT- series, user-selectable per channel (class 1, characterized codes only — reserved ones the
             *                 module refuses with ERR_PARAM); PATCH /api/v1/units/{hex} ct_code
             *  ct_model_ct  — CT- series names (class 2, CT shipped with the module, calibrated in pair) — display only */
            cJSON *ct  = cJSON_AddObjectToObject(en, "ct_model");
            cJSON *ct2 = cJSON_AddObjectToObject(en, "ct_model_ct");
            char k[4];
            for (unsigned i = 0; i < RBAMP_SENSOR_MODEL_COUNT; i++) {
                const rbamp_sensor_model_info_t *mi = &RBAMP_SENSOR_MODELS[i];
                if (!ct_model_selectable(mi)) continue;
                snprintf(k, sizeof(k), "%u", (unsigned)mi->code);
                if (mi->sensor_class == RBAMP_SENSOR_CLASS_SCT_013) cJSON_AddStringToObject(ct, k, mi->descriptor);
                else if (mi->sensor_class == RBAMP_SENSOR_CLASS_WIRED_CT) cJSON_AddStringToObject(ct2, k, mi->descriptor);
            }
        }
        cJSON *ph = cJSON_AddObjectToObject(en, "phase");
        cJSON_AddStringToObject(ph, "0", "UNSET");
        cJSON_AddStringToObject(ph, "1", "L1");
        cJSON_AddStringToObject(ph, "2", "L2");
        cJSON_AddStringToObject(ph, "3", "L3");
        cJSON_AddStringToObject(ph, "4", "3P");
        /* phase topology: two-pole / line-to-line loads. 5 = L1L2 (also the US
         * split-phase 240 V load), 6/7 = the other 3P pairs. Valid set per grid.system is a client
         * contract rule (1P: 0,1 · SPLIT: 0,1,2,5 · 3P: all) — firmware validates range only. */
        cJSON_AddStringToObject(ph, "5", "L1L2");
        cJSON_AddStringToObject(ph, "6", "L2L3");
        cJSON_AddStringToObject(ph, "7", "L3L1");
        /* site mains system (system.grid.system; "" = unset → client derives 1P) */
        cJSON *gs = cJSON_AddArrayToObject(en, "grid_system");
        cJSON_AddItemToArray(gs, cJSON_CreateString("1P"));
        cJSON_AddItemToArray(gs, cJSON_CreateString("SPLIT"));
        cJSON_AddItemToArray(gs, cJSON_CreateString("3P"));
        /* Which grid systems were validated on hardware — the UI labels the others "not validated"
         * from HERE, not from a hardcoded list. */
        cJSON *gv = cJSON_AddArrayToObject(en, "grid_system_validated");
        cJSON_AddItemToArray(gv, cJSON_CreateString("1P"));
        /* 3P validated on hardware (an rbAmp STD-U3I3 module on a live three-phase installation):
         * module check (U per phase, CT↔phase pairing, CT direction, Q sign on a real inductive
         * load) + hub: per-phase ph[] = module, Σ phases = total, quarter/hour/day records, planned reboot,
         * poll pause, cold downtime across local midnight. */
        cJSON_AddItemToArray(gv, cJSON_CreateString("3P"));

        /* input-field limits (BYTES, utf-8) — UI renders counters from here, no hardcode */
        cJSON *lim = cJSON_AddObjectToObject(root, "limits");
        cJSON_AddNumberToObject(lim, "short_name_bytes", 24);
        cJSON_AddNumberToObject(lim, "name_bytes", 31);
        cJSON_AddNumberToObject(lim, "origins_max", 8);
        rbgrid_json_add_config_ranges(lim);          /* limits.ranges — numeric config bounds */
        cJSON *tz = cJSON_AddArrayToObject(en, "tariff_zone");
        for (unsigned z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++)
            cJSON_AddItemToArray(tz, cJSON_CreateString(rbpower_tariff_zone_type_text[z]));
        cJSON *pt = cJSON_AddArrayToObject(en, "tariff_plan_type");
        for (unsigned t = 0; t < sizeof(rbpower_tariff_plan_type_text) / sizeof(rbpower_tariff_plan_type_text[0]); t++)
            cJSON_AddItemToArray(pt, cJSON_CreateString(rbpower_tariff_plan_type_text[t]));
        cJSON *ct = cJSON_AddArrayToObject(en, "tariff_condition_type");
        for (unsigned c = 0; c < sizeof(rbpower_tariff_condition_type_text) / sizeof(rbpower_tariff_condition_type_text[0]); c++)
            cJSON_AddItemToArray(ct, cJSON_CreateString(rbpower_tariff_condition_type_text[c]));

        /* money formatting: currency comes from the ACTIVE tariff plan (display label, no conversion);
         * decimals follow ISO-4217 minor units; rates keep up to 4 decimals. */
        cJSON *money = cJSON_AddObjectToObject(root, "money");
        const char *cur = rbpower_tariff_currency();
        cJSON_AddStringToObject(money, "currency_code", cur);
        cJSON_AddNumberToObject(money, "money_decimals", iso4217_minor_digits(cur));
        cJSON_AddNumberToObject(money, "rate_decimals", 4);
    }

    // Curated endpoint surface (the contract baseline). tier: "1"=1:1 struct mirror, "composite"=aggregated.
    // auth = the write (POST/DELETE) is token-guarded; reboot = a POST to it needs a device restart to apply.
    // reboot_fields (mixed endpoints): only these fields apply at restart, the rest are live — the POST answer's
    // requires_reboot is true only when one of them changed (system-config: rbwebserver.c needs_reboot;
    // time-sync: enable_time_sync/primary_source/sntp_server, timezone is live).
    static const struct { const char *path; const char *methods; const char *tier; bool auth; bool reboot; const char *params; const char *reboot_fields; } EP[] = {
        { "/api/v1/history",            "GET",      "1+env",     false, false, "unit_id,period,type(electrical|energy|economics|all),offset,limit,from,to,since" },
        { "/api/v1/economics",          "GET",      "composite", false, false, "unit" },
        { "/api/v1/summary",            "GET",      "composite", false, false, "unit,period,from,to" },
        { "/api/v1/export.csv",         "GET",      "1-csv",     false, false, "unit,period,type,from,to" },
        { "/api/v1/espnow",             "GET,POST", "composite", true,  false, "action(pair|add|remove|disable|enable|latch),mac,seconds,channels" },
        { "/api/v1/units",              "POST",     "n/a",       true,  false, "sensor,name,role,stat_class,point,reuse" },
        { "/api/v1/units/{hex}",        "POST,DELETE","n/a",     true,  false, "name,short_name,phase,rated_power_w,max_current_a,role,group_id,role_trunk,invert_sign,charge_threshold_w,idle_timeout_min,ct_code (POST=params patch; invert_sign + ct_code apply live; ct_code = enums.ct_model, rbAmp current channels; EV overrides 0 = system.ev)" },
        { "/api/v1/units/{hex}/rebind", "POST",     "n/a",       true,  false, "module,ch" },
        { "/api/v1/units/{hex}/rename", "POST",     "n/a",       true,  false, "point,reuse" },
        { "/api/v1/sensors",            "GET",      "composite", false, false, "" },
        { "/api/telemetry/latest",      "GET",      "composite", false, false, "" },
        { "/api/system/provisioning",   "GET",      "composite", false, false, "" },
        { "/api/system/token",          "GET,POST", "n/a",       true,  false, "token,role(admin|read)" },
        { "/api/system/security",       "GET,POST", "n/a",       true,  false, "dev_cors,origins" },
        { "/api/system/backup",         "GET,POST", "n/a",       true,  false, "" },
        { "/api/system/restore",        "POST",     "n/a",       true,  true,  "" },
        { "/api/system/system-config",  "GET,POST", "composite", true,  false, "" },   /* all fields live */
        { "/api/system/hardware-config","GET,POST", "composite", true,  true,  "" },
        { "/api/system/units-config",   "GET,POST", "composite", true,  false, "" },
        { "/api/system/tariff-plan-config","GET,POST","1",       true,  false, "" },
        { "/api/system/telemetry-config","GET,POST", "composite", true,  false, "profile(lean|full)" },
        { "/api/system/mqtt-config",    "GET,POST", "composite", true,  true,  "broker_uri" },
        { "/api/system/time-sync",      "GET,POST", "composite", true,  true,  "", "enable_time_sync,primary_source,sntp_server" },
        { "/api/system/reboot",         "POST",     "n/a",       true,  false, "delay_ms" },
        { "/api/system/version",        "GET",      "1",         false, false, "" },
        { "/api/system/ota",            "POST",     "n/a",       true,  true,  "(binary)" },
        { "/api/system/ui-upload",      "POST",     "n/a",       true,  false, "path (query); body=raw file" },
        { "/api/system/ui-commit",      "POST",     "n/a",       true,  false, "force (query); body=ui-manifest.json" },
        { "/api/system/ap",             "GET,POST", "n/a",       true,  false, "GET: AP ssid/password (admin token, armed hub only); POST {password}: the user sets the AP password (\"\" = setup password)" },
        { "/api/system/lang",           "GET,POST", "n/a",       true,  false, "code (query, ISO 639-1); body=language pack JSON ≤ 200 KB; one pack kept; served at /lang/<code>.json" },
    };
    cJSON *eps = cJSON_AddArrayToObject(root, "endpoints");
    for (unsigned i = 0; i < sizeof(EP) / sizeof(EP[0]); i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "path", EP[i].path);
        cJSON_AddStringToObject(e, "methods", EP[i].methods);
        cJSON_AddStringToObject(e, "tier", EP[i].tier);
        cJSON_AddBoolToObject(e, "auth_required", EP[i].auth);
        cJSON_AddBoolToObject(e, "requires_reboot", EP[i].reboot);
        if (EP[i].params[0]) cJSON_AddStringToObject(e, "params", EP[i].params);
        if (EP[i].reboot_fields) {
            cJSON *rf = cJSON_AddArrayToObject(e, "reboot_fields");
            char buf[160]; strlcpy(buf, EP[i].reboot_fields, sizeof(buf));
            for (char *sv = NULL, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                cJSON_AddItemToArray(rf, cJSON_CreateString(t));
        }
        cJSON_AddItemToArray(eps, e);
    }
    cJSON *tr = cJSON_AddObjectToObject(root, "transports");
    cJSON_AddStringToObject(tr, "ws", "/ws-telemetry (push + get/start/stop_telemetry)");
    cJSON_AddStringToObject(tr, "mqtt", "rbgrid/<mac>/{telemetry,status,cmd,cmd/resp} + HA discovery");

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
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

