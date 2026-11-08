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

/** @brief I2C fleet action (rbi2c_cmd.c). */
char *rbi2c_action(const cJSON *req, int *status);

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
/** Send an rbi2c_action() reply with its HTTP status. */
static esp_err_t send_action(httpd_req_t *req, char *body, int st)
{
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    static const char *const codes[] = { "200 OK", "400 Bad Request", "404 Not Found", "409 Conflict", "500 Internal Server Error" };
    httpd_resp_set_status(req, st == 200 ? codes[0] : st == 404 ? codes[2] : st == 409 ? codes[3] : st == 500 ? codes[4] : codes[1]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

static esp_err_t i2c_get(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_read(req)) return ESP_FAIL;
    return send_json(req, rbamp_bridge_i2c_state());
}

/** POST /api/v1/i2c/...: the path picks the op of rbi2c_action() (rbi2c_cmd.c), the body carries its fields. */
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
    unsigned m = 0, bus = 0;
    const char *op = NULL;
    if (!strncmp(tail, "scan", 4)) op = "scan";
    else if (sscanf(tail, "modules/%u/address", &m) == 1 && strstr(tail, "/address")) op = "address";
    else if (sscanf(tail, "modules/%u/bus", &m) == 1 && strstr(tail, "/bus")) op = "bus";
    else if (sscanf(tail, "modules/%u/drdy", &m) == 1 && strstr(tail, "/drdy")) op = "drdy";
    else if (!strcmp(tail, "modules")) op = "add";
    else if (sscanf(tail, "buses/%u", &bus) == 1) op = "pins";
    if (!op) {
        cJSON_Delete(b);
        return send_err(req, "404 Not Found", "ROUTE", "scan | modules | modules/<m>/address | modules/<m>/bus | buses/<b>");
    }
    cJSON_DeleteItemFromObject(b, "op");
    cJSON_AddStringToObject(b, "op", op);
    if (!strcmp(op, "address") || !strcmp(op, "bus") || !strcmp(op, "drdy")) {
        cJSON_DeleteItemFromObject(b, "module");
        cJSON_AddNumberToObject(b, "module", m);
    }
    if (!strcmp(op, "pins")) { cJSON_DeleteItemFromObject(b, "bus"); cJSON_AddNumberToObject(b, "bus", bus); }
    int st = 200;
    char *body = rbi2c_action(b, &st);
    cJSON_Delete(b);
    return send_action(req, body, st);
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
