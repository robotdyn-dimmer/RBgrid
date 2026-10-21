/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "rbhome_types.h"
/**
 * @file rbwebserver_wifi.c
 * @brief Wi-Fi endpoints of the web server: setup portal, STA status/save, AP password, network scan.
 *
 * AP config portal (GET /wifi), STA status/save, the user's AP password (GET/POST /api/system/ap)
 * and the async network scan (GET /wifi/scan + /wifi/scan/results with the WIFI_EVENT_SCAN_DONE-cached
 * results). Handlers run on the httpd task; the scan-done handler runs on the default event-loop
 * task. The shared CORS helper comes from the internal header and the JSON context is reached via the rbwebserver_core_json_ctx() accessor (g_json_context is a
 * separate static in both rbjson.c and rbwebserver.c — see rbwebserver_internal.h).
 */

#include "rbwebserver_internal.h"
/** @brief Persist the open hour (PARTIAL record) before a planned restart (declared in rbhome_types.h). @return Number of units flushed. */
extern int rbpower_stats_flush_open_hours(void);   /* rbhome_types.h — open-hour flush on planned restart */
#include "rbwifi.h"
#include "rbgrid_config_storage.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */

/* WPA2-PSK passphrase limits for the provisioned AP setup password (ap_provision.h) */
_Static_assert(sizeof(RBGRID_AP_SETUP_PASSWORD) - 1 >= 8 && sizeof(RBGRID_AP_SETUP_PASSWORD) - 1 <= 63,
               "RBGRID_AP_SETUP_PASSWORD must be 8..63 characters (WPA2-PSK)");

static const char* TAG = "RBWEBSERVER_WIFI";

/* ==== wifi_config / wifi_status / wifi_save / async scan ==== */
esp_err_t rbwebserver_wifi_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /wifi request");
    add_cors_headers(req);

    // Get WiFi status data
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    char mode_str[16] = "NULL";
    if (mode == WIFI_MODE_AP) strcpy(mode_str, "AP");
    else if (mode == WIFI_MODE_STA) strcpy(mode_str, "STA");
    else if (mode == WIFI_MODE_APSTA) strcpy(mode_str, "APSTA");

    // STA variables
    char sta_ssid[33] = "";
    int sta_rssi = 0;
    bool sta_connected = false;
    char sta_ip[16] = "0.0.0.0";
    char sta_gateway[16] = "0.0.0.0";
    char sta_netmask[16] = "0.0.0.0";

    // AP variables
    char ap_ssid[33] = "";
    int ap_clients = 0;
    char ap_ip[16] = "0.0.0.0";

    // Get STA info
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_config_t wifi_cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
            strncpy(sta_ssid, (char*)wifi_cfg.sta.ssid, sizeof(sta_ssid) - 1);
        }

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            sta_rssi = ap_info.rssi;
            sta_connected = true;

            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip_info.ip));
                    snprintf(sta_gateway, sizeof(sta_gateway), IPSTR, IP2STR(&ip_info.gw));
                    snprintf(sta_netmask, sizeof(sta_netmask), IPSTR, IP2STR(&ip_info.netmask));
                }
            }
        }
    }

    // Get AP info
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t wifi_cfg;
        if (esp_wifi_get_config(WIFI_IF_AP, &wifi_cfg) == ESP_OK) {
            strncpy(ap_ssid, (char*)wifi_cfg.ap.ssid, sizeof(ap_ssid) - 1);

            wifi_sta_list_t sta_list;
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
                ap_clients = sta_list.num;
            }

            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&ip_info.ip));
                }
            }
        }
    }

    // Start building HTML
    httpd_resp_set_type(req, "text/html");

    const char* html_header =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "    <meta charset=\"UTF-8\">"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "    <title>WiFi Configuration - RBGRID</title>"
    "    <style>"
    "        * { margin: 0; padding: 0; box-sizing: border-box; }"
    "        body { font-family: Arial, sans-serif; background: #f0f0f0; }"
    "        .nav { background: #2c3e50; color: white; padding: 15px; }"
    "        .nav-container { max-width: 800px; margin: 0 auto; display: flex; justify-content: space-between; align-items: center; }"
    "        .nav-brand { font-size: 20px; font-weight: bold; }"
    "        .nav-links { display: flex; gap: 15px; }"
    "        .nav-links a { color: white; text-decoration: none; padding: 8px 15px; border-radius: 4px; }"
    "        .nav-links a:hover { background: #34495e; }"
    "        .nav-links a.active { background: #3498db; }"
    "        .nav-links a.disabled { color: #7f8c8d; cursor: not-allowed; pointer-events: none; opacity: 0.5; }"
    "        .container { max-width: 600px; margin: 20px auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }"
    "        h1 { color: #333; margin-bottom: 20px; }"
    "        .info-box { background: #e3f2fd; border-left: 4px solid #2196F3; padding: 15px; margin-bottom: 20px; border-radius: 4px; }"
    "        .info-box h3 { color: #1976D2; margin-bottom: 10px; }"
    "        .info-row { display: flex; justify-content: space-between; padding: 5px 0; }"
    "        .info-label { color: #666; font-weight: bold; }"
    "        .info-value { color: #333; }"
    "        .connected { color: #4CAF50; }"
    "        .disconnected { color: #f44336; }"
    "        .form-group { margin-bottom: 15px; }"
    "        label { display: block; margin-bottom: 5px; color: #666; font-weight: bold; }"
    "        input[type=\"text\"], input[type=\"password\"] { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 4px; font-size: 14px; }"
    "        button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; font-weight: bold; }"
    "        button:hover { background: #45a049; }"
    "        button.secondary { background: #2196F3; }"
    "        button.secondary:hover { background: #1976D2; }"
    "        .status { margin-top: 15px; padding: 12px; border-radius: 4px; text-align: center; }"
    "        .success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }"
    "        .error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }"
    "        .scan-results { margin-top: 20px; }"
    "        .scan-results h3 { margin-bottom: 10px; color: #333; }"
    "        .network-item { padding: 12px; margin: 5px 0; background: #f8f9fa; border-radius: 4px; cursor: pointer; border: 1px solid #dee2e6; }"
    "        .network-item:hover { background: #e9ecef; border-color: #adb5bd; }"
    "    </style>"
    "</head>"
    "<body>"
    "    <div class=\"nav\">"
    "        <div class=\"nav-container\">"
    "            <div class=\"nav-brand\">RBGRID Control</div>"
    "            <div class=\"nav-links\">"
    "                <a href=\"/\">Home</a>"
    "                <a href=\"/wifi\" class=\"active\">WiFi</a>";

    // Send header
    httpd_resp_sendstr_chunk(req, html_header);

    // Build and send navigation with WEB-APP link
    char nav_webapp[256];
    bool sta_ip_valid = (sta_connected && strcmp(sta_ip, "0.0.0.0") != 0);

    if (sta_ip_valid) {
        snprintf(nav_webapp, sizeof(nav_webapp),
            "                <a href=\"https://webapp.rbgrid.io/?esp32_ip=%s&force=1\" target=\"_blank\">WEB-APP</a>", sta_ip);
    } else {
        snprintf(nav_webapp, sizeof(nav_webapp),
            "                <a href=\"#\" class=\"disabled\">WEB-APP</a>");
    }
    httpd_resp_sendstr_chunk(req, nav_webapp);

    const char* html_after_nav =
    "            </div>"
    "        </div>"
    "    </div>"
    "    <div class=\"container\">"
    "        <h1>WiFi Configuration</h1>"
    "        <div class=\"info-box\">"
    "            <h3>Current Status</h3>"
    "            <div>";

    // Send part after navigation
    httpd_resp_sendstr_chunk(req, html_after_nav);

    // Build and send status content dynamically
    char status_html[1024];
    int len = 0;

    len = rb_scat(status_html, sizeof(status_html), len,
        "<div class=\"info-row\"><span class=\"info-label\">Mode:</span><span class=\"info-value\">%s</span></div>", mode_str);

    if (strlen(ap_ssid) > 0) {
        len = rb_scat(status_html, sizeof(status_html), len,
            "<div class=\"info-row\"><span class=\"info-label\">AP SSID:</span><span class=\"info-value\">%s</span></div>", ap_ssid);
        len = rb_scat(status_html, sizeof(status_html), len,
            "<div class=\"info-row\"><span class=\"info-label\">AP IP:</span><span class=\"info-value\">%s</span></div>", ap_ip);
        len = rb_scat(status_html, sizeof(status_html), len,
            "<div class=\"info-row\"><span class=\"info-label\">AP Clients:</span><span class=\"info-value\">%d</span></div>", ap_clients);
    }

    if (strlen(sta_ssid) > 0) {
        const char* status_class = sta_connected ? "connected" : "disconnected";
        const char* status_text = sta_connected ? "Connected" : "Disconnected";
        len = rb_scat(status_html, sizeof(status_html), len,
            "<div class=\"info-row\"><span class=\"info-label\">STA Status:</span><span class=\"info-value %s\">%s</span></div>",
            status_class, status_text);
        len = rb_scat(status_html, sizeof(status_html), len,
            "<div class=\"info-row\"><span class=\"info-label\">STA SSID:</span><span class=\"info-value\">%s</span></div>", sta_ssid);

        if (sta_connected && strcmp(sta_ip, "0.0.0.0") != 0) {
            len = rb_scat(status_html, sizeof(status_html), len,
                "<div class=\"info-row\"><span class=\"info-label\">STA IP:</span><span class=\"info-value\">%s</span></div>", sta_ip);
            len = rb_scat(status_html, sizeof(status_html), len,
                "<div class=\"info-row\"><span class=\"info-label\">Gateway:</span><span class=\"info-value\">%s</span></div>", sta_gateway);
            len = rb_scat(status_html, sizeof(status_html), len,
                "<div class=\"info-row\"><span class=\"info-label\">Netmask:</span><span class=\"info-value\">%s</span></div>", sta_netmask);
        }

        if (sta_connected && sta_rssi != 0) {
            len = rb_scat(status_html, sizeof(status_html), len,
                "<div class=\"info-row\"><span class=\"info-label\">RSSI:</span><span class=\"info-value\">%d dBm</span></div>", sta_rssi);
        }
    }

    httpd_resp_sendstr_chunk(req, status_html);

    const char* html_part2 =
    "            </div>"
    "        </div>"
    "        <div class=\"form-group\">"
    "            <button onclick=\"scanNetworks()\">Scan Networks</button>"
    "        </div>"
    "        <div id=\"scanResults\" class=\"scan-results\"></div>"
    "        <form id=\"wifiForm\">"
    "            <div class=\"form-group\">"
    "                <label for=\"ssid\">SSID:</label>"
    "                <input type=\"text\" id=\"ssid\" name=\"ssid\" required>"
    "            </div>"
    "            <div class=\"form-group\">"
    "                <label for=\"password\">Password:</label>"
    "                <input type=\"password\" id=\"password\" name=\"password\">"
    "            </div>"
    "            <button type=\"submit\">Connect</button>"
    "        </form>"
    "        <div id=\"status\"></div>"
    "    </div>"
    "    <script>"
    "        function scanNetworks() {"
    "            console.log('Scanning WiFi networks...');"
    "            fetch('/wifi/scan')"
    "                .then(response => response.json())"
    "                .then(data => {"
    "                    console.log('Scan results:', data);"
    "                    var resultsDiv = document.getElementById('scanResults');"
    "                    resultsDiv.innerHTML = '<h3>Available Networks:</h3>';"
    "                    if (data.networks && data.networks.length > 0) {"
    "                        data.networks.forEach(function(network) {"
    "                            var div = document.createElement('div');"
    "                            div.className = 'network-item';"
    "                            div.textContent = network.ssid + ' (' + network.rssi + ' dBm)';"
    "                            div.onclick = function() {"
    "                                document.getElementById('ssid').value = network.ssid;"
    "                            };"
    "                            resultsDiv.appendChild(div);"
    "                        });"
    "                    } else {"
    "                        resultsDiv.innerHTML += '<div>No networks found</div>';"
    "                    }"
    "                })"
    "                .catch(error => {"
    "                    console.error('Scan error:', error);"
    "                    document.getElementById('status').innerHTML = '<div class=\"status error\">Scan failed</div>';"
    "                });"
    "        }"
    "        "
    "        document.getElementById('wifiForm').addEventListener('submit', function(e) {"
    "            e.preventDefault();"
    "            console.log('Submitting WiFi form...');"
    "            var formData = new FormData(e.target);"
    "            var data = {};"
    "            formData.forEach(function(value, key) {"
    "                data[key] = value;"
    "            });"
    "            console.log('Form data:', data);"
    "            "
    "            fetch('/wifi/save', {"
    "                method: 'POST',"
    "                headers: {"
    "                    'Content-Type': 'application/json'"
    "                },"
    "                body: JSON.stringify(data)"
    "            })"
    "            .then(function(response) { return response.json(); })"
    "            .then(function(result) {"
    "                console.log('Save result:', result);"
    "                document.getElementById('status').innerHTML = '<div class=\"status success\">Settings saved! Restarting...</div>';"
    "                setTimeout(function() {"
    "                    window.location.href = '/';"
    "                }, 3000);"
    "            })"
    "            .catch(function(error) {"
    "                console.error('Save error:', error);"
    "                document.getElementById('status').innerHTML = '<div class=\"status error\">Failed to save settings</div>';"
    "            });"
    "        });"
    "    </script>"
    "</body>"
    "</html>";

    // Send rest of HTML (forms, scripts, closing tags)
    httpd_resp_sendstr_chunk(req, html_part2);

    // End chunked response
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* AP password chosen by the USER: the hub never sets or changes it by itself. NVS rbwifi/
 * ap_pw_user; absent = the setup password from provisioning (RBGRID_AP_SETUP_PASSWORD, ap_provision.h). */
#include "nvs.h"
#include "nvs_flash.h"
/* Documented in rbwebserver_internal.h. */
bool rbgrid_ap_user_password(char *out, size_t n)
{
    nvs_handle_t h;
    if (!out || n < 9 || nvs_open("rbwifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    bool ok = nvs_get_str(h, "ap_pw_user", out, &len) == ESP_OK && strlen(out) >= 8;
    nvs_close(h);
    return ok;
}
static bool ap_pw_valid(const char *p)
{
    size_t l = strlen(p);
    if (l < 8 || l > 63) return false;
    for (size_t i = 0; i < l; ++i) if (p[i] < 0x20 || p[i] > 0x7E) return false;   /* printable ASCII (WPA2 passphrase) */
    return true;
}

/* POST /api/system/ap {"password":"..."} — the user's own AP password (8..63 printable ASCII); "" = back to the
 * setup password. Write token (fail-open like every write). Persisted and applied to the running AP at once (its
 * clients reconnect with the new password). */
esp_err_t rbwebserver_ap_set_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    char body[160] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body) || httpd_req_recv(req, body, len) != len) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"body\",\"message\":\"expected {password: 8..63 characters}\"}");
    }
    cJSON *j = cJSON_Parse(body);
    const char *pw = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "password")) : NULL;
    if (!pw || (pw[0] && !ap_pw_valid(pw))) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"password\",\"message\":\"8..63 printable ASCII characters, or empty to return to the setup password\"}");
    }
    char use[65];
    strlcpy(use, pw[0] ? pw : RBGRID_AP_SETUP_PASSWORD, sizeof(use));
    bool user = pw[0] != 0;
    cJSON_Delete(j);
    nvs_handle_t h;
    esp_err_t e = nvs_open("rbwifi", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = user ? nvs_set_str(h, "ap_pw_user", use) : nvs_erase_key(h, "ap_pw_user");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs"); return ESP_FAIL; }
    wifi_config_t c;
    esp_err_t ae = esp_wifi_get_config(WIFI_IF_AP, &c);
    if (ae == ESP_OK) {
        strlcpy((char *)c.ap.password, use, sizeof(c.ap.password));
        c.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ae = esp_wifi_set_config(WIFI_IF_AP, &c);
    }
    rbpower_diag_printf("wifi: AP password %s by the user (applied: %s)", user ? "changed" : "reset to the setup password",
                        esp_err_to_name(ae));
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"user_set\":%s,\"applied\":%s}", user ? "true" : "false",
             ae == ESP_OK ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}

/* GET /api/system/ap — this hub's AP SSID + password. Admin token REQUIRED and the hub must have one set:
 * on an unarmed hub anyone on its networks could read it. */
esp_err_t rbwebserver_ap_info_handler(httpd_req_t *req)
{
    extern bool rbwebserver_api_token_armed(void);
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    if (!rbwebserver_api_token_armed()) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"forbidden\",\"message\":\"set an admin token first (POST /api/system/token); the AP password is also on the USB console (appw)\"}");
    }
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_AP, &c) != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "AP not configured"); return ESP_FAIL; }
    char ssid[33]; memcpy(ssid, c.ap.ssid, 32); ssid[32] = '\0';
    char pw[65];   memcpy(pw, c.ap.password, 64); pw[64] = '\0';
    cJSON *r = cJSON_CreateObject();
    if (!r) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    cJSON_AddStringToObject(r, "ssid", ssid);
    cJSON_AddStringToObject(r, "password", pw);
    bool user_set = strcmp(pw, RBGRID_AP_SETUP_PASSWORD) != 0;
    cJSON_AddBoolToObject(r, "user_set", user_set);     /* false = still the setup password — the user should change it */
    cJSON_AddBoolToObject(r, "per_device", user_set);   /* legacy name of user_set (kept for API compatibility) */
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

esp_err_t rbwebserver_wifi_status_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /wifi/status request");
    add_cors_headers(req);
    
    cJSON *root = cJSON_CreateObject();
    
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    cJSON_AddStringToObject(root, "mode", 
        mode == WIFI_MODE_AP ? "AP" : 
        mode == WIFI_MODE_STA ? "STA" : 
        mode == WIFI_MODE_APSTA ? "APSTA" : "NULL");
    
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_config_t wifi_cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
            char ssid[33]; memcpy(ssid, wifi_cfg.sta.ssid, 32); ssid[32] = '\0';   /* a 32-char SSID has no NUL: the password follows */
            cJSON_AddStringToObject(root, "ssid", ssid);
        }

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
            cJSON_AddBoolToObject(root, "connected", true);

            // Get STA IP address
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "sta_ip", ip_str);

                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.gw));
                    cJSON_AddStringToObject(root, "gateway", ip_str);

                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.netmask));
                    cJSON_AddStringToObject(root, "netmask", ip_str);
                }
            }
        } else {
            cJSON_AddBoolToObject(root, "connected", false);
        }
    }
    
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t wifi_cfg;
        if (esp_wifi_get_config(WIFI_IF_AP, &wifi_cfg) == ESP_OK) {
            char ap_ssid[33]; memcpy(ap_ssid, wifi_cfg.ap.ssid, 32); ap_ssid[32] = '\0';   /* same: a 32-char SSID has no NUL */
            cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);

            wifi_sta_list_t sta_list;
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
                cJSON_AddNumberToObject(root, "connected_stations", sta_list.num);
            }

            // Get AP IP address
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "ap_ip", ip_str);
                }
            }
        }
    }

    char *json_str = cJSON_Print(root);
    if (json_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
    } else {
        ESP_LOGE(TAG, "Failed to create JSON response");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON creation failed");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// WiFi settings save handler
esp_err_t rbwebserver_wifi_save_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /wifi/save request");
    add_cors_headers(req);
    /* This write used to be the ONLY unauthenticated POST — with an admin
     * token set, anyone on the LAN could repoint the hub's Wi-Fi. Same fail-open rule as every
     * other write: no token configured (provisioning) → open; token set → admin required. */
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    
    char buf[256];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }
    
    ret = rbapi_recv_body(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *password = cJSON_GetObjectItem(json, "password");
    
    if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    
    // Save and apply WiFi settings
    rbpower_err_t err = rbwifi_update_sta_config(ssid->valuestring,
                                                cJSON_IsString(password) ? password->valuestring : "",
                                                true);

    cJSON_Delete(json);
    if (err != RBPOWER_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi config");
        return ESP_FAIL;
    }

    // Send successful response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "WiFi settings saved. Restarting...");
    
    char *resp_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    free(resp_str);
    cJSON_Delete(response);

    // Restart after 1 second
    vTaskDelay(pdMS_TO_TICKS(1000));
    (void)rbpower_stats_flush_open_hours();   /* planned restart: keep the open hour */
    { rbpower_mark_planned_restart(); esp_restart(); }

    return ESP_OK;
}

// WiFi network scan handler
// ============================ Async wifi scan ==========================================================
// GET /wifi/scan         → 202 {data:{status:"scanning"}} — starts a NON-blocking scan (idempotent).
// GET /wifi/scan/results → 200 {data:{status:"scanning"|"done"|"error"[,count,networks]}} — poll.
// The old esp_wifi_scan_start(...,true) stalled the httpd task 1.5-4s (delaying WS frames + other
// requests). Now WIFI_EVENT_SCAN_DONE caches the records on the event task; handlers never block.
#define WIFI_SCAN_MAX_AP 24   ///< Capacity of the cached scan-result table (AP records)
static SemaphoreHandle_t s_wifi_scan_mutex = NULL;
static bool      s_wifi_scan_in_progress = false;
static bool      s_wifi_scan_done        = false;
static bool      s_wifi_scan_error       = false;
static uint16_t  s_wifi_scan_count       = 0;
static wifi_ap_record_t s_wifi_scan_results[WIFI_SCAN_MAX_AP];

static const char* wifi_auth_mode_str(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa_psk";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2_psk";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa_wpa2_psk";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3_psk";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2_wpa3_psk";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
        default:                        return "unknown";
    }
}

// Runs on the default event-loop task when a scan completes: snapshot the AP records into the cache.
static void wifi_scan_done_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg; (void)data;
    if (base != WIFI_EVENT || id != WIFI_EVENT_SCAN_DONE) return;
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (s_wifi_scan_mutex) xSemaphoreTake(s_wifi_scan_mutex, portMAX_DELAY);
    if (num > 0) {
        uint16_t got = WIFI_SCAN_MAX_AP;   // capacity in; actual count out
        esp_err_t e = esp_wifi_scan_get_ap_records(&got, s_wifi_scan_results);
        s_wifi_scan_count = (e == ESP_OK) ? got : 0;
        s_wifi_scan_error = (e != ESP_OK);
    } else {
        s_wifi_scan_count = 0;
        s_wifi_scan_error = false;
    }
    s_wifi_scan_done = true;
    s_wifi_scan_in_progress = false;
    if (s_wifi_scan_mutex) xSemaphoreGive(s_wifi_scan_mutex);
    ESP_LOGI(TAG, "wifi scan done: %u AP(s)", (unsigned)s_wifi_scan_count);
}

static void wifi_scan_lazy_init(void) {
    if (!s_wifi_scan_mutex) {
        s_wifi_scan_mutex = xSemaphoreCreateMutex();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                   wifi_scan_done_event_handler, NULL);
    }
}

// Build the standard BaseResponse envelope around data:{status,...}. networks_count < 0 → omit the
// networks array (scanning/ack). Returns a printed string the caller must free, or NULL on error.
static char* wifi_scan_build_envelope(const char* type, const char* status,
                                      const wifi_ap_record_t* aps, int networks_count) {
    if (!rbwebserver_core_json_ctx()) return NULL;
    cJSON* root = rbgrid_json_create_base_message(type, rbwebserver_core_json_ctx());
    if (!root) return NULL;
    cJSON* dataobj = cJSON_CreateObject();
    if (!dataobj) { cJSON_Delete(root); return NULL; }
    cJSON_AddStringToObject(dataobj, "status", status);
    if (networks_count >= 0) {
        cJSON_AddNumberToObject(dataobj, "count", networks_count);
        cJSON* arr = cJSON_CreateArray();
        for (int i = 0; i < networks_count && aps; i++) {
            cJSON* n = cJSON_CreateObject();
            char bssid[18];
            snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                     aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                     aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
            cJSON_AddStringToObject(n, "ssid", (char*)aps[i].ssid);
            cJSON_AddStringToObject(n, "bssid", bssid);
            cJSON_AddNumberToObject(n, "rssi", aps[i].rssi);
            cJSON_AddNumberToObject(n, "channel", aps[i].primary);
            cJSON_AddStringToObject(n, "auth_mode", wifi_auth_mode_str(aps[i].authmode));
            cJSON_AddItemToArray(arr, n);
        }
        cJSON_AddItemToObject(dataobj, "networks", arr);
    }
    cJSON_AddItemToObject(root, "data", dataobj);
    char* s = cJSON_Print(root);
    cJSON_Delete(root);
    return s;
}

esp_err_t rbwebserver_wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /wifi/scan request (async)");
    add_cors_headers(req);
    wifi_scan_lazy_init();

    if (s_wifi_scan_mutex) xSemaphoreTake(s_wifi_scan_mutex, portMAX_DELAY);
    if (!s_wifi_scan_in_progress) {
        esp_err_t e = esp_wifi_scan_start(NULL, false);   // NON-blocking; result via SCAN_DONE
        if (e == ESP_OK) {
            s_wifi_scan_in_progress = true;
            s_wifi_scan_done = false;
            s_wifi_scan_error = false;
        } else {
            ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(e));
            s_wifi_scan_in_progress = false;
            s_wifi_scan_done = true;    // /results returns error on the very next poll
            s_wifi_scan_error = true;
            s_wifi_scan_count = 0;
        }
    }
    // else: a scan is already running → idempotent, just report "scanning" (no second start).
    if (s_wifi_scan_mutex) xSemaphoreGive(s_wifi_scan_mutex);

    char* json = wifi_scan_build_envelope("wifi_scan", "scanning", NULL, -1);
    if (!json) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json context unavailable"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

esp_err_t rbwebserver_wifi_scan_results_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /wifi/scan/results request");
    add_cors_headers(req);

    // httpd serves requests on a single task → this static snapshot is not re-entrant-hazardous
    // and keeps ~2KB of AP records off the limited httpd stack.
    static wifi_ap_record_t snapshot[WIFI_SCAN_MAX_AP];
    bool done = false, err = false;
    uint16_t count = 0;
    if (s_wifi_scan_mutex) xSemaphoreTake(s_wifi_scan_mutex, portMAX_DELAY);
    done = s_wifi_scan_done;
    err  = s_wifi_scan_error;
    count = s_wifi_scan_count;
    if (done && !err && count > 0) {
        if (count > WIFI_SCAN_MAX_AP) count = WIFI_SCAN_MAX_AP;
        memcpy(snapshot, s_wifi_scan_results, (size_t)count * sizeof(wifi_ap_record_t));
    }
    if (s_wifi_scan_mutex) xSemaphoreGive(s_wifi_scan_mutex);

    char* json;
    if (err) {
        json = wifi_scan_build_envelope("wifi_scan_results", "error", NULL, 0);
    } else if (done) {
        json = wifi_scan_build_envelope("wifi_scan_results", "done", snapshot, (int)count);
    } else {
        json = wifi_scan_build_envelope("wifi_scan_results", "scanning", NULL, -1);
    }
    if (!json) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json context unavailable"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}
