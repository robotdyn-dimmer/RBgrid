/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file modfw.c
 * @brief HTTP surface of the hub-driven firmware update of STD rbAmp modules (the transfer itself runs in
 *        rbamp_bridge.c).
 *
 * @verbatim
   POST /api/v1/modules/<m>/firmware[?bank=<hex>][&board=<hex>]   body = vendor-PREPARED image for the NON-running
 *        bank (see /api/v1/sensors modules[].ota.{bank_size,active_bank}); write token; optional X-FW-SHA256.
        202 + {"status":"started",...}; the update runs in the background (10–15 min), metering continues.
   GET  /api/v1/modules/<m>/firmware   progress {module,state,done,total,elapsed_s,fw_before,fw_after,reason,message}
   @endverbatim
 * Every step is also an event in /api/v1/system/events ("modfw m<N>: …").
 * Both handlers run in the httpd task.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "rbwebserver.h"
#include "rbamp_bridge.h"

/*  @brief Add the web server's CORS headers to a response (defined in rbwebserver).
 *  @param[in] req  HTTP request. */
extern void add_cors_headers(httpd_req_t *req);
static const char *TAG = "modfw";
#define MODFW_MAX (512 * 1024)   ///< maximum accepted image size (bytes)

/** Parse <m> from /api/v1/modules/<m>/firmware; false if malformed or >= RBAMP_MAX_MODULES. */
static bool parse_module(httpd_req_t *req, unsigned *m)
{
    /* reject out-of-range: the (uint8_t) cast below would make /modules/256/ act on module 0 */
    return sscanf(req->uri, "/api/v1/modules/%u/firmware", m) == 1 && *m < RBAMP_MAX_MODULES;
}
static void send_err(httpd_req_t *req, const char *status, const char *code, const char *msg)
{
    httpd_resp_set_status(req, status);
    char b[224];
    snprintf(b, sizeof(b), "{\"status\":\"error\",\"code\":\"%s\",\"message\":\"%s\"}", code, msg);
    httpd_resp_sendstr(req, b);
}

/** GET /api/v1/modules/<m>/firmware: update progress JSON from rbamp_bridge_module_ota_status(). */
static esp_err_t modfw_get(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_read(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    char out[320];
    rbamp_bridge_module_ota_status(out, sizeof(out));
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/** POST /api/v1/modules/<m>/firmware: receive the image into PSRAM, check the optional SHA-256, hand it to
 *  rbamp_bridge_module_ota_start() (which owns the buffer from then on) and answer 202. */
static esp_err_t modfw_post(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_auth(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    unsigned m = 0;
    if (!parse_module(req, &m)) { send_err(req, "400 Bad Request", "BAD_PATH", "use /api/v1/modules/<m>/firmware"); return ESP_OK; }
    char q[96] = {0}, v[24];
    uint32_t bank = 0, board = 0; bool has_bank = false, has_board = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "bank", v, sizeof(v)) == ESP_OK) { bank = strtoul(v, NULL, 16); has_bank = true; }
        if (httpd_query_key_value(q, "board", v, sizeof(v)) == ESP_OK) { board = strtoul(v, NULL, 16); has_board = true; }
    }
    if (!has_bank) {   /* optional: the hub knows which bank is not running (modules[].ota.inactive_bank) */
        rbpower_module_desc_t md;
        if (rbpower_get_module_desc((uint8_t)m, &md) && md.inactive_bank) bank = md.inactive_bank;
        else { send_err(req, "400 Bad Request", "NO_BANK", "module's inactive bank unknown — pass bank=<hex>"); return ESP_OK; }
    }
    if (req->content_len <= 0 || req->content_len > MODFW_MAX) { send_err(req, "400 Bad Request", "BAD_SIZE", "missing or oversized image"); return ESP_OK; }
    char want[72] = {0};
    httpd_req_get_hdr_value_str(req, "X-FW-SHA256", want, sizeof(want));
    uint8_t *img = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!img) { send_err(req, "500 Internal Server Error", "NO_MEM", "no PSRAM for the image"); return ESP_OK; }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *)img + got, req->content_len - got);
        /* bounded retries: a stalled uploader would otherwise pin the single httpd task forever */
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls <= 3) continue;
        if (r == HTTPD_SOCK_ERR_TIMEOUT) { free(img); send_err(req, "408 Request Timeout", "RECV_TIMEOUT", "upload stalled"); return ESP_OK; }
        stalls = 0;
        if (r <= 0) { free(img); send_err(req, "400 Bad Request", "RECV", "upload broke"); return ESP_OK; }
        got += r;
    }
    if (want[0]) {
        uint8_t d[32]; char hex[65];
        mbedtls_sha256(img, got, d, 0);
        for (int i = 0; i < 32; i++) sprintf(&hex[i * 2], "%02x", d[i]);
        if (strcasecmp(hex, want) != 0) { free(img); send_err(req, "400 Bad Request", "SHA256", "sha256 mismatch — image rejected"); return ESP_OK; }
    }
    char why[96];
    esp_err_t e = rbamp_bridge_module_ota_start((uint8_t)m, img, (size_t)got, bank, has_board, board, why, sizeof(why));
    if (e != ESP_OK) {   /* img already freed by the bridge */
        const char *st = (e == ESP_ERR_INVALID_STATE) ? "409 Conflict" : (e == ESP_ERR_NOT_FOUND) ? "404 Not Found" : "400 Bad Request";
        send_err(req, st, "REFUSED", why);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "module %u firmware update started (%d B, bank 0x%08lX)", m, got, (unsigned long)bank);
    httpd_resp_set_status(req, "202 Accepted");
    char b[160];
    snprintf(b, sizeof(b), "{\"status\":\"started\",\"module\":%u,\"bytes\":%d,\"message\":\"%s\"}", m, got, why);
    httpd_resp_sendstr(req, b);
    return ESP_OK;
}

/** @brief Register GET/POST /api/v1/modules/{m}/firmware on the running web server. Call after the server is up. */
void modfw_register_http(void)
{
    httpd_handle_t srv = rbwebserver_core_server();
    if (!srv) return;
    httpd_uri_t g = { .uri = "/api/v1/modules/*", .method = HTTP_GET,  .handler = modfw_get,  .user_ctx = NULL };
    httpd_uri_t p = { .uri = "/api/v1/modules/*", .method = HTTP_POST, .handler = modfw_post, .user_ctx = NULL };
    esp_err_t eg = httpd_register_uri_handler(srv, &g), ep = httpd_register_uri_handler(srv, &p);
    if (eg != ESP_OK || ep != ESP_OK) ESP_LOGE(TAG, "URI registration FAILED (get=%s post=%s)", esp_err_to_name(eg), esp_err_to_name(ep));
}
