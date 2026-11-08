/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_units.c
 * @brief Point (unit) create / change / delete, shared by every transport.
 *
 * Each call returns a malloc'd JSON reply and sets *status to an HTTP-style code (200, 201, 400, 404,
 * 409, 507). The HTTP handlers in rbwebserver_api.c only parse the URI/body and send the reply.
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

static const char *TAG = "RBGRID_UNITS";

/** @brief Set *status and return a heap copy of @p s (NULL on OOM; the caller sends a 500). */
static char *rbu_reply_impl(int *status, int code, const char *s)
{
    if (status) *status = code;
    return strdup(s);
}
#define rbu_reply(code, s) rbu_reply_impl(status, (code), (s))

/* Delete a point (DELETE /api/v1/units/{hex}): 200 removed + persisted, 404 unknown, 409 has children. */
char *rbgrid_units_delete(rbpower_unit_id_t unit_id, int *status)
{
    if (unit_id == 0) {
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"invalid unit id — expect /api/v1/units/<hex>\"}");
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
    if (e == RBPOWER_OK) rbpower_home_clear_inside_refs(unit_id);   /* points measured inside it become top-level */
    char body[160];
    if (e == RBPOWER_ERR_INVALID_ARG) {            // unknown / already removed (idempotent → 404)
        snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"unit %04X not found\"}", unit_id);
        return rbu_reply(404, body);
    }
    if (e == RBPOWER_ERR_INVALID_STATE) {          // has children
        snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"unit %04X has children — delete them first\"}", unit_id);
        return rbu_reply(409, body);
    }
    // Success — persist units.json without the unit (sensor config untouched = cascade unit-only).
    if (dead_point[0]) rbgrid_point_tombstone_add(dead_point);   /* before save → persisted */
    rbgrid_json_ha_unpublish_unit(unit_id, dead_role);           /* HA: drop the ghost entities */
    esp_err_t su = rbgrid_storage_save_units(NULL);
    rbgrid_core_notify_unit_deleted(unit_id);
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"removed\":\"%04X\",\"persist\":\"%s\"}",
             unit_id, esp_err_to_name(su));
    ESP_LOGW(TAG, "DELETE unit 0x%04X → removed + persisted", unit_id);
    return rbu_reply(200, body);
}

/* POST /api/v1/units — bind a sensor to a NEW unit (the inverse of
 * DELETE). Body: {sensor:{iface,bus/module/ch|channel}, name, role?, stat_class?, location?}. Creates
 * the unit at runtime (rbpower_home_create_unit → source bind hook sets the rbAmp fleet entry) and
 * persists units.json. 201 {status,unit_id,persist}; 409 already-bound; 400 bad/sensor-not-found. */
char *rbgrid_units_create(const char *json, int *status)
{
    if (!json || !json[0] || strlen(json) > 1024) {
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"empty or oversized body\"}");
    }
    rbgrid_create_unit_req_t cu;
    rbgrid_json_err_t pe = rbgrid_json_parse_create_unit(json, &cu);
    if (pe != RBGRID_JSON_OK) {
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"invalid body — need sensor.iface + addressing + name\"}");
    }

    rbpower_unit_id_t uid = 0;
    int br = rbgrid_config_bind_unit(&cu, &uid);
    char body[224];
    if (br == RBGRID_CONFIG_ERR_DUPLICATE_SENSOR) {
        return rbu_reply(409, "{\"status\":\"error\",\"code\":\"SENSOR_BOUND\",\"message\":\"sensor already bound\"}");
    }
    if (br == RBGRID_CONFIG_ERR_POINT_TOMBSTONED) {   /* retired slug — resurrect with reuse:true */
        return rbu_reply(409, "{\"status\":\"error\",\"code\":\"POINT_TOMBSTONED\",\"message\":\"point slug retired — pass reuse:true to resurrect\"}");
    }
    if (br == RBGRID_CONFIG_ERR_POINT_TAKEN) {
        return rbu_reply(409, "{\"status\":\"error\",\"code\":\"POINT_TAKEN\",\"message\":\"point slug already in use\"}");
    }
    if (br == RBGRID_CONFIG_ERR_NO_MEMORY) {
        return rbu_reply(507, "{\"status\":\"error\",\"message\":\"unit network full\"}");
    }
    if (br != RBGRID_CONFIG_OK) {       // INVALID_PARAM → sensor not found / unsupported iface / bad fields
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"sensor not found, unsupported iface or invalid point slug\"}");
    }
    // Success — persist units.json with the new unit (the rbAmp fleet binding was persisted by the hook).
    esp_err_t su = rbgrid_storage_save_units(NULL);
    const rbpower_unit_definition_t* nd = rbpower_home_get_unit_definition(uid);
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"unit_id\":\"%04X\",\"point\":\"%s\",\"persist\":\"%s\"}",
             uid, nd ? nd->point : "", esp_err_to_name(su));
    ESP_LOGW(TAG, "units: bound unit 0x%04X + persisted", uid);
    rbgrid_ha_discovery_request();   /* HA entities for the new unit now, not at the next MQTT reconnect */
    return rbu_reply(201, body);
}

/* POST /api/v1/units/{hex}/rebind {module,ch}: move a live I2C unit onto another fleet sensor;
 * id/point/history/slot kept. POST /api/v1/units/{hex}/rename {point,reuse?}: change the point slug
 * (old slug → tombstone). One handler for both (URI tail picks the op); persists units.json. */
/* SCT- models the user may select: the registry's characterized codes, plus SCT-013-100 (code 5) — the registry still
 * marks it reserved, but rbAmp BASIC firmware 1.4.19 accepts it (G 2.1622, NF 12) and it is in use on real
 * modules (the registry entry is to be corrected in a later rbAmp library release). */
bool rbgrid_ct_model_selectable(const rbamp_sensor_model_info_t *mi)
{
    if (mi->status != RBAMP_MODEL_STATUS_RESERVED) return true;
    return mi->sensor_class == RBAMP_SENSOR_CLASS_SCT_013 && mi->code == RBAMP_CT_SCT013_100;
}

/* Change a point. @p op NULL/"" = params patch; "rebind" {module,ch}; "rename" {point,reuse?}. */
char *rbgrid_units_mutate(rbpower_unit_id_t uid, const char *op, const char *json, int *status)
{
    if (!op) op = "";
    if (!rbpower_home_get_unit_definition(uid)) {
        return rbu_reply(404, "{\"status\":\"error\",\"message\":\"unit not found\"}");
    }
    if (!json || !json[0] || strlen(json) > 512) {
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"empty or oversized body\"}");
    }
    cJSON* root = cJSON_Parse(json);
    if (!root) {
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"invalid json\"}");
    }

    int rc;
    bool group_phases = false;   /* rebind ch 254: three-phase grouping, applies after a reboot */
    bool split_phases = false;   /* rebind of a phase-group point to one channel: split per phase, applies after a reboot */
    char body[512];
    if (op[0] == '\0') {
        /* PARAMS PATCH — {name?, short_name?, phase?, rated_power_w?, max_current_a?, role?,
         * group_id?, role_trunk?, invert_sign?}: live-apply to the definition + persist. Supply roles are
         * topology (specialized main/alt structs built at init) — changing to/from them is
         * NOT a patch (use the wizard/rebind), 409. Load-class role changes apply live for
         * grouping/labels; the period mask recomputes on the next reboot. */
        rbpower_unit_definition_t* def = rbpower_home_get_unit_definition_mutable(uid);
        if (!def) {
            cJSON_Delete(root);
            return rbu_reply(404, "{\"status\":\"error\",\"message\":\"unit not found\"}");
        }
        cJSON* j;
        /* validate everything first — a patch is all-or-nothing */
        if ((j = cJSON_GetObjectItem(root, "short_name")) &&
            (!cJSON_IsString(j) || strlen(j->valuestring) > 24)) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"field\":\"short_name\",\"message\":\"string, max 24 bytes utf-8\"}");
        }
        if ((j = cJSON_GetObjectItem(root, "phase")) &&
            (!cJSON_IsNumber(j) || j->valueint < 0 || j->valueint > 7)) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"field\":\"phase\",\"message\":\"0..7 (0=unset, 1..3=L1..L3, 4=3P, 5..7=L1L2/L2L3/L3L1)\"}");
        }
        if (((j = cJSON_GetObjectItem(root, "rated_power_w")) && (!cJSON_IsNumber(j) || j->valuedouble < 0)) ||
            ((j = cJSON_GetObjectItem(root, "max_current_a")) && (!cJSON_IsNumber(j) || j->valuedouble < 0))) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"message\":\"rated_power_w/max_current_a: number >= 0 (0 = unset)\"}");
        }
        const rbamp_sensor_model_info_t *ctm = NULL;
        if ((j = cJSON_GetObjectItem(root, "ct_code")) && cJSON_IsNumber(j) && j->valuedouble >= 1 && j->valuedouble <= 255)
            ctm = rbamp_sensor_model_lookup(RBAMP_SENSOR_CLASS_SCT_013, (uint8_t)j->valuedouble);
        if (j && cJSON_GetObjectItem(root, "ct_code") &&
            (!ctm || !rbgrid_ct_model_selectable(ctm) || j->valuedouble != (double)(int64_t)j->valuedouble)) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"error_code\":\"OUT_OF_RANGE\",\"field\":\"ct_code\",\"message\":\"SCT-013 model code from schema enums.ct_model\"}");
        }
        if ((j = cJSON_GetObjectItem(root, "ct_code")) && cJSON_IsNumber(j)) {   /* applied on the module first: fail → 409, nothing else changed */
            esp_err_t ce = rbpower_unit_set_ct((rbpower_unit_id_t)uid, (uint8_t)j->valuedouble);
            if (ce != ESP_OK) {
                cJSON_Delete(root);
                char eb[160];
                snprintf(eb, sizeof(eb), "{\"status\":\"error\",\"field\":\"ct_code\",\"message\":\"%s\"}",
                         ce == ESP_ERR_NOT_FOUND ? "unit is not an rbAmp current channel" :
                         ce == ESP_ERR_NOT_SUPPORTED ? "CT- series module: the sensor is factory-paired and calibrated, its model is fixed" :
                         ce == ESP_ERR_INVALID_STATE ? "module has no sensor class (uncalibrated) or is busy — the model cannot be selected" :
                         esp_err_to_name(ce));
                return rbu_reply(ce == ESP_ERR_NOT_FOUND ? 400 : 409, eb);
            }
        }
        if ((j = cJSON_GetObjectItem(root, "charge_threshold_w")) && (!cJSON_IsNumber(j) || j->valuedouble < 0 || j->valuedouble > 100000 ||
                                                                     j->valuedouble != (double)(int64_t)j->valuedouble)) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"error_code\":\"OUT_OF_RANGE\",\"field\":\"charge_threshold_w\",\"message\":\"integer 0..100000 W (0 = system.ev)\"}");
        }
        if ((j = cJSON_GetObjectItem(root, "idle_timeout_min")) && (!cJSON_IsNumber(j) || j->valuedouble < 0 || j->valuedouble > 1440 ||
                                                                   j->valuedouble != (double)(int64_t)j->valuedouble)) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"error_code\":\"OUT_OF_RANGE\",\"field\":\"idle_timeout_min\",\"message\":\"integer 0..1440 min (0 = system.ev)\"}");
        }
        /* inside: the point this one is measured inside (number or hex string; 0, "" or null clears). Must exist,
         * not be this point, not be a supply point, and not make a loop. */
        int new_inside = -1;
        if ((j = cJSON_GetObjectItem(root, "inside")) != NULL) {
            unsigned v = 0; bool ok = true;
            if (cJSON_IsNull(j)) v = 0;
            else if (cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 0xFFFF) v = (unsigned)j->valueint;
            else if (cJSON_IsString(j)) { if (j->valuestring[0] && sscanf(j->valuestring, "%x", &v) != 1) ok = false; }
            else ok = false;
            const rbpower_unit_definition_t *pd = v ? rbpower_home_get_unit_definition((rbpower_unit_id_t)v) : NULL;
            if (ok && v) {
                if (v == uid || !pd) ok = false;
                else if (pd->role == RBPOWER_ROLE_MAIN_SUPPLY || pd->role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY) ok = false;
                else {   /* walk up from the new parent: reaching this point would make a loop */
                    rbpower_unit_id_t cur = (rbpower_unit_id_t)v;
                    for (int depth = 0; depth < 16 && cur; depth++) {
                        if (cur == uid) { ok = false; break; }
                        const rbpower_unit_definition_t *cd = rbpower_home_get_unit_definition(cur);
                        cur = cd ? cd->inside_unit_id : 0;
                    }
                }
            }
            if (!ok) {
                cJSON_Delete(root);
                return rbu_reply(400, "{\"status\":\"error\",\"field\":\"inside\",\"message\":\"an existing point other than this one, not the input, without a loop (0 clears)\"}");
            }
            new_inside = (int)v;
        }
        int new_role = -1;
        if ((j = cJSON_GetObjectItem(root, "role")) != NULL) {
            if (cJSON_IsString(j)) {
                for (unsigned r = 0; rbpower_load_role_name(r); r++)
                    if (strcmp(j->valuestring, rbpower_load_role_name(r)) == 0) { new_role = (int)r; break; }
            }
            if (new_role < 0) {
                cJSON_Delete(root);
                return rbu_reply(400, "{\"status\":\"error\",\"field\":\"role\",\"message\":\"unknown role — see schema enums.role\"}");
            }
            bool cur_supply = (def->role == RBPOWER_ROLE_MAIN_SUPPLY || def->role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY);
            bool new_supply = (new_role == RBPOWER_ROLE_MAIN_SUPPLY || new_role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY);
            if (cur_supply || new_supply) {
                cJSON_Delete(root);
                return rbu_reply(409, "{\"status\":\"error\",\"code\":\"ROLE_SUPPLY_LOCKED\",\"message\":\"supply roles are topology — change via wizard/rebind, not patch\"}");
            }
        }
        /* apply — on a COPY in internal RAM, then one whole-struct write-back. The definitions live in
         * PSRAM; on ESP32 silicon < v3 byte stores into a PSRAM word (name[] starts 2 bytes into the word
         * that holds unit_id) were seen to zero the neighbouring byte of unit_id (0x0101 -> 0x0001, hub .139,
         * rev v1.1) despite CONFIG_SPIRAM_CACHE_WORKAROUND — the point then vanished at the next boot.
         * Word-aligned copy-in/copy-out avoids partial-word stores into the live definition. */
        rbpower_home_lock();
        rbpower_unit_definition_t* live = def;
        rbpower_unit_definition_t nd;
        memcpy(&nd, live, sizeof(nd));
        def = &nd;
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
        if (new_inside >= 0) def->inside_unit_id = (uint16_t)new_inside;
        nd.unit_id = live->unit_id;   /* identity is never patched */
        memcpy(live, &nd, sizeof(nd));
        def = live;
        rbpower_home_unlock();
        cJSON_Delete(root);

        esp_err_t supd = rbgrid_storage_save_units(NULL);
        char en[96], esn[64];   /* a name with " or \ broke the 200 body */
        rbgrid_json_escape(def->name, en, sizeof(en));
        rbgrid_json_escape(def->short_name, esn, sizeof(esn));
        snprintf(body, sizeof(body),
                 "{\"status\":\"ok\",\"unit_id\":\"%04X\",\"name\":\"%s\",\"short_name\":\"%s\","
                 "\"phase\":%u,\"rated_power_w\":%.1f,\"max_current_a\":%.1f,\"role\":\"%s\","
                 "\"group_id\":%u,\"role_trunk\":%u,\"inside\":%u,\"charge_threshold_w\":%lu,\"idle_timeout_min\":%u,"
                 "\"requires_reboot\":false,\"persist\":\"%s\"}",
                 uid, en, esn, def->phase, (double)def->rated_power_w,
                 (double)def->max_current_a, rbpower_load_role_name((unsigned)def->role),
                 def->group_id, def->role_trunk, (unsigned)def->inside_unit_id, (unsigned long)def->ev_charge_threshold_w,
                 (unsigned)def->ev_idle_timeout_min, esp_err_to_name(supd));
        ESP_LOGW(TAG, "units: params patched (unit 0x%04X)", uid);
        return rbu_reply(200, body);
    }
    if (strcmp(op, "rebind") == 0) {
        cJSON* jm = cJSON_GetObjectItem(root, "module");
        cJSON* jc = cJSON_GetObjectItem(root, "ch");
        if (!cJSON_IsNumber(jm) || !cJSON_IsNumber(jc)) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"message\":\"need module + ch\"}");
        }
        /* ch 254 = group a three-phase module (STD-U3I3) as ONE input: only for the bridge-fed main input; the new
         * fleet layout is saved and applies after a reboot (requires_reboot:true in the reply) */
        group_phases = cJSON_IsNumber(jc) && jc->valueint == 254;
        {   /* a point on a phase group (L1–L3) moved to one channel splits the module into one point per phase:
             * saved now, applies after a reboot (requires_reboot:true, grouped:false) */
            uint8_t fm = 0xFF, fs = 0;
            const rbpower_unit_definition_t* od = rbpower_home_get_unit_definition(uid);
            bool on_group = (rbpower_get_unit_rbamp_feed(uid, &fm, &fs) && fs == 254) ||
                            (od && od->sources_count > 0 && od->sources[0].source_type == RBPOWER_SOURCE_I2C_SENSOR &&
                             od->channel_idx == 254);
            split_phases = on_group && !group_phases && jc->valueint < 254;
        }
        rc = rbgrid_config_rebind_unit(uid, (uint8_t)jm->valueint, (uint8_t)jc->valueint);
        cJSON_Delete(root);
        if (rc == RBGRID_CONFIG_ERR_DUPLICATE_SENSOR) {
            return rbu_reply(409, group_phases
                ? "{\"status\":\"error\",\"code\":\"SENSOR_BOUND\",\"message\":\"a channel of this module carries another point — delete or move it first\"}"
                : "{\"status\":\"error\",\"code\":\"SENSOR_BOUND\",\"message\":\"target sensor bound to another unit\"}");
        }
        if (rc != RBGRID_CONFIG_OK) {
            return rbu_reply(400, group_phases
                ? "{\"status\":\"error\",\"code\":\"NOT_PHASE_MODULE\",\"message\":\"only the input can be grouped, on a present three-phase module (STD-U3I3)\"}"
                : "{\"status\":\"error\",\"message\":\"not an i2c unit or target sensor not in fleet\"}");
        }
    } else if (strcmp(op, "rename") == 0) {
        cJSON* jp = cJSON_GetObjectItem(root, "point");
        bool reuse = cJSON_IsTrue(cJSON_GetObjectItem(root, "reuse"));
        if (!cJSON_IsString(jp) || !jp->valuestring[0]) {
            cJSON_Delete(root);
            return rbu_reply(400, "{\"status\":\"error\",\"message\":\"need point\"}");
        }
        rc = rbgrid_config_rename_point(uid, jp->valuestring, reuse);
        cJSON_Delete(root);
        if (rc == RBGRID_CONFIG_ERR_POINT_TAKEN) {
            return rbu_reply(409, "{\"status\":\"error\",\"code\":\"POINT_TAKEN\",\"message\":\"point slug already in use\"}");
        }
        if (rc == RBGRID_CONFIG_ERR_POINT_TOMBSTONED) {
            return rbu_reply(409, "{\"status\":\"error\",\"code\":\"POINT_TOMBSTONED\",\"message\":\"point slug retired — pass reuse:true to resurrect\"}");
        }
        if (rc != RBGRID_CONFIG_OK) {
            return rbu_reply(400, "{\"status\":\"error\",\"message\":\"invalid point slug ([a-z0-9_.-]{1,32})\"}");
        }
    } else {
        cJSON_Delete(root);
        return rbu_reply(400, "{\"status\":\"error\",\"message\":\"unknown op — rebind|rename\"}");
    }

    esp_err_t su = rbgrid_storage_save_units(NULL);
    const rbpower_unit_definition_t* d = rbpower_home_get_unit_definition(uid);
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"unit_id\":\"%04X\",\"point\":\"%s\",\"module\":%d,\"ch\":%u,\"persist\":\"%s\"%s}",
             uid, d ? d->point : "", d ? (int)d->device_uid : -1, d ? d->channel_idx : 0,
             esp_err_to_name(su), group_phases ? ",\"grouped\":true,\"requires_reboot\":true"
                                  : split_phases ? ",\"grouped\":false,\"requires_reboot\":true" : "");
    ESP_LOGW(TAG, "units: %s ok (unit 0x%04X)", op, uid);
    rbgrid_ha_discovery_request();   /* rename → HA device/entity names follow; rebind → harmless refresh */
    return rbu_reply(200, body);
}
