/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbi2c_api.c
 * @brief I2C fleet management over HTTP: rbAmp modules on one or two I2C buses; bus, pins and module address
 *        are chosen in the web app.
 *
 * The second I2C bus and the /api/v1/i2c management API are available in the code but are not part of the
 * v1.1.0 release feature set.
 *
 * @verbatim
   GET  /api/v1/i2c                        buses (pins, up), board pin profile (pins_ok / pins_reserved), modules
   POST /api/v1/i2c/scan          {bus}     devices answering on a bus: {addr, module|null, kind rbamp|rbdimmer|unknown|device, fw, uid}
   POST /api/v1/i2c/modules       {bus,addr} add a scanned rbAmp module (joins at once)
   POST /api/v1/i2c/modules/<m>/address {addr}  change the module's address (saved in the module, UID-verified)
   POST /api/v1/i2c/modules/<m>/bus     {bus}   the module was moved to the other bus (same address)
   POST /api/v1/i2c/modules/<m>/drdy    {gpio}  the module's own DRDY line (-1 = the shared fleet line)
   POST /api/v1/i2c/buses/<b>     {sda,scl} | {"off":true}  pins (board-profile checked), applied after a restart
   @endverbatim
 * Writes need the admin token when one is set. Events: "m<N> added / address / moved", "I2C bus {b} pins".
 * The handlers run in the httpd task; the bus work is done by rbamp_bridge_i2c_*().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "rbwebserver.h"
#include "rbamp_bridge.h"

/*  @brief Add the web server's CORS headers to a response (defined in rbwebserver).
 *  @param[in] req  HTTP request. */
extern void add_cors_headers(httpd_req_t *req);
static const char *TAG = "rbi2c";

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}
static esp_err_t send_err(httpd_req_t *req, const char *status, const char *code, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", msg);
    return send_json(req, o);
}
/** Reply to a write: the new I2C state with "status":"ok" on success, else an error mapped from @p e
 *  (NOT_FOUND → 404, INVALID_STATE → 409, other → 400). */
static esp_err_t send_result(httpd_req_t *req, esp_err_t e, const char *why)
{
    if (e == ESP_OK) {
        cJSON *o = rbamp_bridge_i2c_state();
        if (o) cJSON_AddStringToObject(o, "status", "ok");
        return send_json(req, o);
    }
    const char *st = e == ESP_ERR_NOT_FOUND ? "404 Not Found" : e == ESP_ERR_INVALID_STATE ? "409 Conflict" : "400 Bad Request";
    return send_err(req, st, esp_err_to_name(e), why[0] ? why : esp_err_to_name(e));
}

static esp_err_t i2c_get(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_read(req)) return ESP_FAIL;
    return send_json(req, rbamp_bridge_i2c_state());
}

/** Integer field @p k of @p b (number or numeric string, "0x51" accepted), else @p dflt. */
static int num(cJSON *b, const char *k, int dflt)
{
    cJSON *j = cJSON_GetObjectItem(b, k);
    if (j && cJSON_IsNumber(j)) return j->valueint;
    if (j && cJSON_IsString(j)) return (int)strtol(j->valuestring, NULL, 0);   /* "0x51" accepted */
    return dflt;
}

/** POST /api/v1/i2c/...: route scan / modules / modules/<m>/{address,bus,drdy} / buses/<b>. */
static esp_err_t i2c_post(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_auth(req)) return ESP_FAIL;
    char buf[160] = {0};
    int len = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    if (len > 0 && httpd_req_recv(req, buf, len) <= 0) return send_err(req, "400 Bad Request", "BODY", "body read failed");
    cJSON *b = cJSON_Parse(len > 0 ? buf : "{}");
    if (!b) return send_err(req, "400 Bad Request", "JSON", "bad JSON");
    const char *tail = req->uri + strlen("/api/v1/i2c/");
    char why[128] = "";
    esp_err_t e;
    unsigned m = 0, bus = 0;
    if (!strncmp(tail, "scan", 4)) {
        int bb = num(b, "bus", 0);
        cJSON *o = cJSON_CreateObject(), *arr = cJSON_CreateArray();
        e = (bb < 0 || bb > 1) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_scan((uint8_t)bb, arr);
        cJSON_Delete(b);
        if (e == ESP_ERR_INVALID_RESPONSE) {
            cJSON_Delete(o); cJSON_Delete(arr);
            return send_err(req, "409 Conflict", "BUS_STUCK", "every address answers: SDA is held low — check pull-ups (4.7 kOhm to 3.3 V) and wiring, then scan again");
        }
        if (e != ESP_OK) { cJSON_Delete(o); cJSON_Delete(arr); return send_err(req, "409 Conflict", "BUS_DOWN", "bus is not running"); }
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddNumberToObject(o, "bus", bb);
        cJSON_AddItemToObject(o, "devices", arr);
        return send_json(req, o);
    } else if (sscanf(tail, "modules/%u/address", &m) == 1 && strstr(tail, "/address")) {
        int a = num(b, "addr", -1);
        e = (a < 0 || a > 0x7F) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_set_addr((uint8_t)m, (uint8_t)a, why, sizeof(why));
    } else if (sscanf(tail, "modules/%u/bus", &m) == 1 && strstr(tail, "/bus")) {
        int bb = num(b, "bus", -1);
        e = (bb < 0 || bb > 1) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_set_bus((uint8_t)m, (uint8_t)bb, why, sizeof(why));
    } else if (sscanf(tail, "modules/%u/drdy", &m) == 1 && strstr(tail, "/drdy")) {
        int g = num(b, "gpio", -2);
        e = (g < -1 || g > 39) ? ESP_ERR_INVALID_ARG : rbamp_bridge_i2c_set_drdy((uint8_t)m, g, why, sizeof(why));
    } else if (!strcmp(tail, "modules")) {
        int bb = num(b, "bus", 0), a = num(b, "addr", -1);
        int nm = -1;
        e = (bb < 0 || bb > 1 || a < 0 || a > 0x7F) ? ESP_ERR_INVALID_ARG
                                                     : rbamp_bridge_i2c_add((uint8_t)bb, (uint8_t)a, &nm, why, sizeof(why));
    } else if (sscanf(tail, "buses/%u", &bus) == 1) {
        cJSON *off = cJSON_GetObjectItem(b, "off");
        int sda = cJSON_IsTrue(off) ? -1 : num(b, "sda", -2), scl = cJSON_IsTrue(off) ? -1 : num(b, "scl", -2);
        e = (bus > 1 || (sda == -2 || scl == -2)) ? ESP_ERR_INVALID_ARG
                                                  : rbamp_bridge_i2c_set_pins((uint8_t)bus, sda, scl, why, sizeof(why));
    } else {
        cJSON_Delete(b);
        return send_err(req, "404 Not Found", "ROUTE", "scan | modules | modules/<m>/address | modules/<m>/bus | buses/<b>");
    }
    cJSON_Delete(b);
    return send_result(req, e, why);
}

/** @brief Register GET /api/v1/i2c and POST /api/v1/i2c/... on the running web server. Call after the server is up. */
void rbi2c_register_http(void)
{
    httpd_handle_t srv = rbwebserver_core_server();
    if (!srv) return;
    httpd_uri_t g = { .uri = "/api/v1/i2c",   .method = HTTP_GET,  .handler = i2c_get,  .user_ctx = NULL };
    httpd_uri_t p = { .uri = "/api/v1/i2c/*", .method = HTTP_POST, .handler = i2c_post, .user_ctx = NULL };
    esp_err_t eg = httpd_register_uri_handler(srv, &g), ep = httpd_register_uri_handler(srv, &p);
    if (eg != ESP_OK || ep != ESP_OK) ESP_LOGE(TAG, "URI registration FAILED (get=%s post=%s)", esp_err_to_name(eg), esp_err_to_name(ep));
}
