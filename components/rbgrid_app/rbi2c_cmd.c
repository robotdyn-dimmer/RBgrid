/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbi2c_cmd.c
 * @brief I2C fleet actions, transport-neutral: the HTTP API (/api/v1/i2c, rbi2c_api.c) and the command channel
 *        (`get i2c` / `set i2c`: MQTT cmd, ESPHome action) both call rbi2c_action().
 *
 * @verbatim
   {"op":"state"}                              buses (pins, up), board pin profile, modules
   {"op":"scan","bus":0}                       devices answering on a bus
   {"op":"add","bus":0,"addr":80}              add a scanned rbAmp module (joins at once)
   {"op":"address","module":0,"addr":81}       change the module's address (saved in the module, UID-verified)
   {"op":"bus","module":0,"bus":1}             the module was moved to the other bus (same address)
   {"op":"drdy","module":0,"gpio":33}          the module's own DRDY line (-1 = the shared fleet line)
   {"op":"pins","bus":0,"sda":8,"scl":9}       bus pins (board-profile checked), applied after a restart; "off":true
   @endverbatim
 * Reply: the I2C state (or the scan result) with "status":"ok", or {"status":"error","code","message"}; *status is
 * the HTTP-style code (200, 400, 404, 409).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "rbamp_bridge.h"

/** Integer field @p k of @p b (number or numeric string, "0x51" accepted), else @p dflt. */
static int num(const cJSON *b, const char *k, int dflt)
{
    const cJSON *j = cJSON_GetObjectItem(b, k);
    if (j && cJSON_IsNumber(j)) return j->valueint;
    if (j && cJSON_IsString(j)) return (int)strtol(j->valuestring, NULL, 0);
    return dflt;
}

static char *err(int *status, int code, const char *ecode, const char *msg)
{
    *status = code;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "code", ecode);
    cJSON_AddStringToObject(o, "message", msg);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static char *result(int *status, esp_err_t e, const char *why)
{
    if (e == ESP_OK) {
        *status = 200;
        cJSON *o = rbamp_bridge_i2c_state();
        if (!o) return err(status, 500, "STATE", "no I2C state");
        cJSON_AddStringToObject(o, "status", "ok");
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        return s;
    }
    int code = e == ESP_ERR_NOT_FOUND ? 404 : e == ESP_ERR_INVALID_STATE ? 409 : 400;
    return err(status, code, esp_err_to_name(e), why[0] ? why : esp_err_to_name(e));
}

char *rbi2c_action(const cJSON *req, int *status)
{
    int st_dummy;
    if (!status) status = &st_dummy;
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(req, "op"));
    if (!op) op = "state";
    char why[128] = "";
    esp_err_t e;
    if (!strcmp(op, "state")) {
        return result(status, ESP_OK, why);
    } else if (!strcmp(op, "scan")) {
        int bb = num(req, "bus", 0);
        cJSON *arr = cJSON_CreateArray();
        e = (bb < 0 || bb > 1) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_scan((uint8_t)bb, arr);
        if (e == ESP_ERR_INVALID_RESPONSE) {
            cJSON_Delete(arr);
            return err(status, 409, "BUS_STUCK", "a bus line is held low — a module may be without power, set to another mode "
                                                 "(e.g. Modbus) or wired wrong: check the module connections, then scan again");
        }
        if (e != ESP_OK) { cJSON_Delete(arr); return err(status, 409, "BUS_DOWN", "bus is not running"); }
        *status = 200;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddNumberToObject(o, "bus", bb);
        cJSON_AddItemToObject(o, "devices", arr);
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        return s;
    } else if (!strcmp(op, "add")) {
        int bb = num(req, "bus", 0), a = num(req, "addr", -1), nm = -1;
        e = (bb < 0 || bb > 1 || a < 0 || a > 0x7F) ? ESP_ERR_INVALID_ARG
                                                     : rbamp_bridge_i2c_add((uint8_t)bb, (uint8_t)a, &nm, why, sizeof(why));
    } else if (!strcmp(op, "address")) {
        int m = num(req, "module", -1), a = num(req, "addr", -1);
        e = (m < 0 || a < 0 || a > 0x7F) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_set_addr((uint8_t)m, (uint8_t)a, why, sizeof(why));
    } else if (!strcmp(op, "bus")) {
        int m = num(req, "module", -1), bb = num(req, "bus", -1);
        e = (m < 0 || bb < 0 || bb > 1) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_set_bus((uint8_t)m, (uint8_t)bb, why, sizeof(why));
    } else if (!strcmp(op, "drdy")) {
        int m = num(req, "module", -1), g = num(req, "gpio", -2);
        e = (m < 0 || g < -1 || g > 48) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_set_drdy((uint8_t)m, g, why, sizeof(why));
    } else if (!strcmp(op, "pins")) {
        int bus = num(req, "bus", -1);
        bool off = cJSON_IsTrue(cJSON_GetObjectItem(req, "off"));
        int sda = off ? -1 : num(req, "sda", -2), scl = off ? -1 : num(req, "scl", -2);
        e = (bus < 0 || bus > 1 || sda == -2 || scl == -2) ? ESP_ERR_INVALID_ARG
                                                          : rbamp_bridge_i2c_set_pins((uint8_t)bus, sda, scl, why, sizeof(why));
    } else {
        return err(status, 404, "OP", "op: state | scan | add | address | bus | drdy | pins");
    }
    return result(status, e, why);
}
