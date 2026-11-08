/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_core_cmd.c
 * @brief Transport-neutral command dispatcher (get / set / history), API tokens and the hook registries
 *        that main/ fills in (ESP-NOW status + actions, unit-deleted).
 *
 * Moved out of rbwebserver_api.c so a build without the HTTP server keeps the same config API:
 * MQTT `cmd` calls rbgrid_command_dispatch() today; an ESPHome API action can call it the same way.
 */
#include "rbgrid_core.h"
#include "rbgrid_core_priv.h"
#include "rbhome_types.h"
#include "rbgrid_config_storage.h"
#include "rbgrid_config_api.h"
#include "rbhome_tariff.h"
#include "rbjson.h"
#include "littlefs_raw.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rbstrutil.h"
#include "rbamp_sensor_models.h"

static const char *TAG = "RBGRID_CMD";

/* API tokens (littlefs). See rbwebserver_api.c for the two-role model; the files are read here so the
 * dispatcher applies the same rule without the HTTP server. */
bool rbgrid_token_load(const char* file, char out[48])
{
    size_t act = 0;
    out[0] = '\0';
    /* Config store not mounted (a failed mount is no longer formatted): the token files can't be read, which
     * must NOT read as "no token = onboarding, everything open". Report a token that nothing can match:
     * writes and gated reads are refused until the store is back (reboot). */
    if (!littlefs_raw_is_mounted()) { out[0] = '\x01'; out[1] = '\0'; return true; }
    return littlefs_raw_exists(file) &&
           littlefs_raw_read(file, out, 47, &act) == ESP_OK && out[0];
}

/* ESP-NOW status provider: esp_now_hub lives in main/ (above this component), so it registers its
 * status-JSON builder here at boot; the MQTT `cmd get espnow` path calls through it. NULL until registered. */
static char *(*s_espnow_status_provider)(void) = NULL;
void rbgrid_register_espnow_status_provider(char *(*fn)(void)) { s_espnow_status_provider = fn; }
/* Called after a point was deleted (DELETE /api/v1/units/{id}). The ESP-NOW hub drops the node channel behind it
 * from its registry: the node's points are registered from that registry at every boot, so a deleted point came
 * back after a restart (an unused node input could not be switched off). */
static void (*s_unit_deleted_hook)(uint16_t unit_id) = NULL;
/* I2C fleet actions (rbi2c_action in rbgrid_app): `get i2c` / `set i2c`. NULL until registered. */
static char *(*s_i2c_action)(const cJSON *req, int *status) = NULL;
void rbgrid_register_i2c_action_handler(char *(*fn)(const cJSON *req, int *status)) { s_i2c_action = fn; }
void rbgrid_register_unit_deleted_hook(void (*fn)(uint16_t)) { s_unit_deleted_hook = fn; }
void rbgrid_core_notify_unit_deleted(uint16_t unit_id) { if (s_unit_deleted_hook) s_unit_deleted_hook(unit_id); }
/* ESP-NOW node actions (the hub in main/ registers it): MQTT `cmd set espnow {data:{action,…}}` = POST /api/v1/espnow. */
static char *(*s_espnow_action)(const cJSON *req, int *status) = NULL;
void rbgrid_register_espnow_action_handler(char *(*fn)(const cJSON *req, int *status)) { s_espnow_action = fn; }

/* ==== command dispatch (MQTT `cmd` channel today; any host transport later) ==== */
static char* cmd_ack_json(const char* cmd, const char* section,
                          const char* result, const char* message, const char* id)
{
    if (!rbgrid_core_json_ctx()) return NULL;
    cJSON* root = rbgrid_json_create_base_message("cmd_ack", rbgrid_core_json_ctx());
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
    if (strcmp(section, "sensors") == 0) return rbgrid_json_create_sensors_list(rbgrid_core_json_ctx());
    if (strcmp(section, "espnow") == 0)  return s_espnow_status_provider ? s_espnow_status_provider() : NULL;
    if (strcmp(section, "price") == 0) {   /* external energy prices in force / ahead */
        static rbpower_ext_price_t ep[48];
        int n = rbpower_tariff_external_prices(ep, 48);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "price");
        cJSON_AddNumberToObject(o, "now", (double)time(NULL));
        float b = -1, sl = -1;
        bool on = rbpower_tariff_external_price_at((uint32_t)time(NULL), &b, &sl);
        cJSON_AddBoolToObject(o, "active", on);
        cJSON* a = cJSON_AddArrayToObject(o, "prices");
        for (int i = 0; i < n; i++) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "from", ep[i].from);
            cJSON_AddNumberToObject(e, "until", ep[i].until);
            cJSON_AddNumberToObject(e, "buy", round((double)ep[i].buy * 1e6) / 1e6);
            if (ep[i].sell >= 0) cJSON_AddNumberToObject(e, "sell", round((double)ep[i].sell * 1e6) / 1e6);
            cJSON_AddItemToArray(a, e);
        }
        char* r = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        return r;
    }
    if (strcmp(section, "i2c") == 0) {
        if (!s_i2c_action) return NULL;
        cJSON *q = cJSON_CreateObject(); cJSON_AddStringToObject(q, "op", "state");
        int st = 200; char *r = s_i2c_action(q, &st); cJSON_Delete(q);
        return r;
    }
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
        int sp = rbgrid_period_str_to_stat(ps);
        if (sp < 0) { ps = "hour"; sp = rbgrid_period_str_to_stat(ps); }
        const cJSON* jf = cJSON_GetObjectItem(root, "from");
        const cJSON* jt = cJSON_GetObjectItem(root, "to");
        uint32_t from = cJSON_IsNumber(jf) ? (uint32_t)jf->valuedouble : 0;
        uint32_t to   = cJSON_IsNumber(jt) ? (uint32_t)jt->valuedouble : 0xFFFFFFFFu;
        return rbgrid_build_summary_json(unit, sp, ps, from, to);
    }
    if (strcmp(section, "system") == 0 && rbgrid_core_ext_cfg()) {
        return rbgrid_json_create_system_config(rbgrid_core_ext_cfg());
    }
    if (strcmp(section, "telemetry") == 0 && rbgrid_core_ext_cfg() && rbgrid_core_json_ctx()) {
        return rbgrid_json_create_telemetry_config(rbgrid_core_ext_cfg(), rbgrid_core_json_ctx());
    }
    if (strcmp(section, "wifi") == 0 && rbgrid_core_json_ctx()) {
        return rbgrid_json_create_wifi_config(rbgrid_core_json_ctx());
    }
    if (strcmp(section, "tariff") == 0 && rbgrid_core_json_ctx()) {
        return rbgrid_json_create_rbpower_tariff_plan_config(rbgrid_core_json_ctx());
    }
    if (strcmp(section, "adc_sensors") == 0 && rbgrid_core_json_ctx()) {
        return rbgrid_json_create_adc_sensor_list(rbgrid_core_json_ctx());
    }
    if (strcmp(section, "units") == 0 && rbgrid_core_json_ctx()) {
        // Reads g_home_network → lock vs the core-0/esp_timer mutators (we run on the MQTT task).
        rbpower_home_lock();
        char* s = rbgrid_json_create_units_config(rbgrid_core_json_ctx());
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
        if (rbgrid_token_load(RBGRID_TOKEN_FILE, admin)) {
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
    if (strcmp(section, "price") == 0) {
        /* external energy price: {"buy","sell"?,"from","until"} | {"prices":[...]} | {"clear":true} */
        if (cJSON_IsTrue(cJSON_GetObjectItem(data, "clear"))) {
            rbpower_tariff_clear_external_price();
            ack = cmd_ack_json("set", section, "ok", "external prices cleared", id);
        } else {
            const cJSON* arr = cJSON_GetObjectItem(data, "prices");
            const cJSON* one = data;
            int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 1, okc = 0;
            const char* why = NULL;
            for (int k = 0; k < n && !why; k++) {
                const cJSON* e = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, k) : one;
                const cJSON *jb = cJSON_GetObjectItem(e, "buy"), *js = cJSON_GetObjectItem(e, "sell");
                const cJSON *jf = cJSON_GetObjectItem(e, "from"), *ju = cJSON_GetObjectItem(e, "until");
                if (!cJSON_IsNumber(jb) || !cJSON_IsNumber(jf) || !cJSON_IsNumber(ju)) { why = "each price needs buy, from, until (unix s)"; break; }
                if (js && !cJSON_IsNumber(js)) { why = "sell must be a number"; break; }
                if (rbpower_tariff_set_external_price((float)jb->valuedouble, js ? (float)js->valuedouble : -1.0f,
                                                      (uint32_t)jf->valuedouble, (uint32_t)ju->valuedouble) != ESP_OK)
                    { why = "bad price (buy 0..100) or until <= from"; break; }
                okc++;
            }
            if (why) { snprintf(msg, sizeof(msg), "%s (%d stored before it)", why, okc); ack = cmd_ack_json("set", section, "error", msg, id); }
            else { snprintf(msg, sizeof(msg), "%d price(s) stored", okc); ack = cmd_ack_json("set", section, "ok", msg, id); }
        }
        free(data_str);
        return ack;
    }
    if (strcmp(section, "i2c") == 0) {
        if (!s_i2c_action) {
            ack = cmd_ack_json("set", section, "error", "I2C actions not available in this build", id);
        } else {
            int st = 200;
            char *rep = s_i2c_action(data, &st);
            cJSON *r = rep ? cJSON_Parse(rep) : NULL;
            const char *m = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "message")) : NULL;
            ack = cmd_ack_json("set", section, st == 200 ? "ok" : "error", st == 200 ? "i2c action done" : (m ? m : "i2c action failed"), id);
            cJSON *a = ack ? cJSON_Parse(ack) : NULL, *ad = a ? cJSON_GetObjectItem(a, "data") : NULL;
            if (ad && r) { cJSON_AddItemToObject(ad, "reply", r); r = NULL; char *re = cJSON_PrintUnformatted(a); if (re) { free(ack); ack = re; } }
            cJSON_Delete(a); cJSON_Delete(r); free(rep);
        }
    } else
    if (strcmp(section, "espnow") == 0) {
        if (!s_espnow_action) {
            ack = cmd_ack_json("set", section, "error", "ESP-NOW not available in this build", id);
        } else {
            int st = 200;
            char *rep = s_espnow_action(data, &st);
            const char *why = "espnow action failed";   /* the action's own message, if it gave one */
            cJSON *rj = (st != 200 && rep) ? cJSON_Parse(rep) : NULL;
            const char *m = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "message")) : NULL;
            if (m) { snprintf(msg, sizeof(msg), "%s", m); why = msg; }
            ack = cmd_ack_json("set", section, st == 200 ? "ok" : "error", st == 200 ? "espnow action done" : why, id);
            cJSON_Delete(rj);
            /* carry the action's own reply (same JSON as the HTTP answer) */
            cJSON *a = ack ? cJSON_Parse(ack) : NULL, *r = rep ? cJSON_Parse(rep) : NULL;
            cJSON *ad = a ? cJSON_GetObjectItem(a, "data") : NULL;
            if (ad && r) { cJSON_AddItemToObject(ad, "reply", r); r = NULL; char *re = cJSON_PrintUnformatted(a); if (re) { free(ack); ack = re; } }
            cJSON_Delete(a); cJSON_Delete(r); free(rep);
        }
    } else
    if (strcmp(section, "tariff") == 0) {
        /* `data` may be the whole `get tariff` document ({"type":"tariff_plan_list_config","data":{"list":[plan]}}),
         * just {"list":[plan]}, or a bare plan object: wrap the short forms into the document the parser reads. */
        if (!cJSON_GetObjectItem(data, "type")) {
            cJSON* doc = cJSON_CreateObject();
            cJSON* dd = cJSON_AddObjectToObject(doc, "data");
            const cJSON* lst = cJSON_GetObjectItem(data, "list");
            cJSON_AddStringToObject(doc, "type", "tariff_plan_list_config");
            if (cJSON_IsArray(lst)) cJSON_AddItemToObject(dd, "list", cJSON_Duplicate(lst, 1));
            else { cJSON* l = cJSON_AddArrayToObject(dd, "list"); cJSON_AddItemToArray(l, cJSON_Duplicate(data, 1)); }
            char* w = cJSON_PrintUnformatted(doc);
            cJSON_Delete(doc);
            if (w) { free(data_str); data_str = w; }
        }
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

/* Chart series for hosts: energy + money per stored period, joined on the device.
 *   {"cmd":"series","unit":"0100","period":"hour|day|week|month|year","from":unix,"to":unix,"limit":168}
 * Reply {"type":"series","unit","period","currency","cols":["ts","dur","wh_in","wh_out","cost","revenue","q"],"rows":[[...],...]}
 * Rows are the stored periods (hour is the finest in flash; 15-min periods are live only), newest first, filtered to
 * [from, to). */
static char* cmd_series(const cJSON* root)
{
    const char* us = cJSON_GetStringValue(cJSON_GetObjectItem(root, "unit"));
    if (!us) return cmd_ack_json("series", "-", "error", "missing 'unit' (hex string)", NULL);
    rbpower_unit_id_t unit = (rbpower_unit_id_t)strtoul(us, NULL, 16);
    const char* ps = cJSON_GetStringValue(cJSON_GetObjectItem(root, "period"));
    if (!ps) ps = "hour";
    int period = !strcmp(ps,"hour")?0 : !strcmp(ps,"day")?1 : !strcmp(ps,"week")?2 : !strcmp(ps,"month")?3 : !strcmp(ps,"year")?4 : -1;
    if (period < 0) return cmd_ack_json("series", ps, "error", "period: hour | day | week | month | year", NULL);
    const cJSON* jf = cJSON_GetObjectItem(root, "from");
    const cJSON* jt = cJSON_GetObjectItem(root, "to");
    const cJSON* jl = cJSON_GetObjectItem(root, "limit");
    double from = cJSON_IsNumber(jf) ? jf->valuedouble : 0, to = cJSON_IsNumber(jt) ? jt->valuedouble : 4e9;
    uint32_t limit = cJSON_IsNumber(jl) ? (uint32_t)jl->valueint : 168;
    if (limit == 0 || limit > 720) limit = 168;
    char* je = rbgrid_history_get_data(unit, (rbpower_period_t)period, (rbpower_data_type_t)1, 0, limit);   /* energy */
    char* jc = rbgrid_history_get_data(unit, (rbpower_period_t)period, (rbpower_data_type_t)2, 0, limit);   /* economics */
    cJSON* e = je ? cJSON_Parse(je) : NULL;
    cJSON* c = jc ? cJSON_Parse(jc) : NULL;
    free(je); free(jc);
    if (!e) { cJSON_Delete(c); return cmd_ack_json("series", ps, "error", "history read failed (no records or flash unavailable)", NULL); }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", "series");
    cJSON_AddStringToObject(out, "unit", us);
    cJSON_AddStringToObject(out, "period", ps);
    cJSON_AddStringToObject(out, "currency", rbpower_tariff_currency());
    cJSON* cols = cJSON_AddArrayToObject(out, "cols");
    static const char* const names[] = { "ts", "dur", "wh_in", "wh_out", "cost", "revenue", "q" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) cJSON_AddItemToArray(cols, cJSON_CreateString(names[i]));
    cJSON* rows = cJSON_AddArrayToObject(out, "rows");
    cJSON* ea = cJSON_GetObjectItem(e, "data");
    cJSON* ca = c ? cJSON_GetObjectItem(c, "data") : NULL;
    cJSON* r = NULL;
    cJSON_ArrayForEach(r, ea) {
        double ts = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "ts"));
        if (ts < from || ts >= to) continue;
        double cost = 0, rev = 0;
        cJSON* k = NULL;
        cJSON_ArrayForEach(k, ca) {
            if (cJSON_GetNumberValue(cJSON_GetObjectItem(k, "ts")) == ts) {
                cost = cJSON_GetNumberValue(cJSON_GetObjectItem(k, "c"));
                rev = cJSON_GetNumberValue(cJSON_GetObjectItem(k, "r"));
                if (isnan(cost)) cost = 0;
                if (isnan(rev)) rev = 0;
                break;
            }
        }
        double win = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "ec")), wout = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "es"));
        cJSON* row = cJSON_CreateArray();
        cJSON_AddItemToArray(row, cJSON_CreateNumber(ts));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItem(r, "dur"))));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(isnan(win) ? 0 : round(win * 1000) / 1000));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(isnan(wout) ? 0 : round(wout * 1000) / 1000));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(round(cost * 1e6) / 1e6));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(round(rev * 1e6) / 1e6));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItem(r, "q"))));
        cJSON_AddItemToArray(rows, row);
    }
    cJSON_Delete(e); cJSON_Delete(c);
    char* sres = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return sres;
}

/* Point operations over the command channel (the HTTP /api/v1/units routes, for builds without the web server):
 *   {"cmd":"unit","op":"create","data":{sensor:{iface,...},name,role?,...}}
 *   {"cmd":"unit","op":"patch|rebind|rename","unit":"0006","data":{...}}
 *   {"cmd":"unit","op":"delete","unit":"0006"}
 * Reply: cmd_ack with result ok|error, the HTTP-style code in data.code and the route's own reply in data.reply. */
static char* cmd_unit(const cJSON* root, const char* id)
{
    char admin[48];
    if (rbgrid_token_load(RBGRID_TOKEN_FILE, admin)) {
        const char* tok = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
        if (!tok || strcmp(tok, admin) != 0) return cmd_ack_json("unit", "-", "error", "unauthorized: admin token required", id);
    }
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(root, "op"));
    const char* us = cJSON_GetStringValue(cJSON_GetObjectItem(root, "unit"));
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!op) return cmd_ack_json("unit", "-", "error", "missing 'op' (create | patch | rebind | rename | delete)", id);
    rbpower_unit_id_t uid = us ? (rbpower_unit_id_t)strtoul(us, NULL, 16) : 0;
    char* body = data ? cJSON_PrintUnformatted(data) : NULL;
    int st = 400;
    char* rep = NULL;
    if (!strcmp(op, "create")) {
        rep = body ? rbgrid_units_create(body, &st) : NULL;
    } else if (!strcmp(op, "patch") || !strcmp(op, "rebind") || !strcmp(op, "rename")) {
        rep = (uid && body) ? rbgrid_units_mutate(uid, !strcmp(op, "patch") ? "" : op, body, &st) : NULL;
    } else if (!strcmp(op, "delete")) {
        rep = uid ? rbgrid_units_delete(uid, &st) : NULL;
    } else {
        free(body);
        return cmd_ack_json("unit", op, "error", "unknown op (create | patch | rebind | rename | delete)", id);
    }
    free(body);
    if (!rep) return cmd_ack_json("unit", op, "error", us || !strcmp(op, "create") ? "missing 'data'" : "missing 'unit' (hex id)", id);
    cJSON* r = cJSON_Parse(rep);
    const char* m = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "message")) : NULL;
    bool ok = st >= 200 && st < 300;
    char* ack = cmd_ack_json("unit", op, ok ? "ok" : "error", ok ? "done" : (m ? m : "failed"), id);
    cJSON* a = ack ? cJSON_Parse(ack) : NULL;
    cJSON* ad = a ? cJSON_GetObjectItem(a, "data") : NULL;
    if (ad) {
        cJSON_AddNumberToObject(ad, "code", st);
        if (r) { cJSON_AddItemToObject(ad, "reply", r); r = NULL; }
        char* re = cJSON_PrintUnformatted(a);
        if (re) { free(ack); ack = re; }
    }
    cJSON_Delete(a); cJSON_Delete(r); free(rep);
    return ack;
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
    /* Reads over MQTT follow the HTTP data-read rule (rbapi_require_read): with an ADMIN and a READ token
     * provisioned, `get` and `history` must carry one of them in the envelope ({"cmd":"get",…,"token":"…"}).
     * Otherwise they stay open, as HTTP reads do (onboarding / write-protection-only mode). */
    if (strcmp(cmd_s, "get") == 0 || strcmp(cmd_s, "history") == 0) {
        char rd[48], admin[48];
        if (rbgrid_token_load(RBGRID_TOKEN_READ_FILE, rd) && rbgrid_token_load(RBGRID_TOKEN_FILE, admin)) {
            const char* tok = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
            bool ok = tok && (strcmp(tok, rd) == 0 || strcmp(tok, admin) == 0);
            if (!ok) {
                out = cmd_ack_json(cmd_s, sec_s, "error", "unauthorized: read token required", id_s);
                goto envelope;
            }
        }
    }
    if (strcmp(cmd_s, "get") == 0) {
        out = cmd_get_section(root, sec_s);
        if (!out) out = cmd_ack_json("get", sec_s, "error", "unknown section or context unavailable", id_s);
    } else if (strcmp(cmd_s, "set") == 0) {
        out = cmd_set_section(root, sec_s, id_s);
    } else if (strcmp(cmd_s, "history") == 0) {
        out = cmd_history(root);
    } else if (strcmp(cmd_s, "unit") == 0) {
        out = cmd_unit(root, id_s);
    } else if (strcmp(cmd_s, "series") == 0) {
        out = cmd_series(root);
    } else {
        out = cmd_ack_json(cmd_s, sec_s, "error", "unknown cmd (use 'get' / 'set' / 'history' / 'series' / 'unit')", id_s);
    }

envelope:
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
