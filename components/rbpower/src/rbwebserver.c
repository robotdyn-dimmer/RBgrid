/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver.c
 * @brief Web server core: HTTP/HTTPS lifecycle, URI routing per Wi-Fi mode, CORS policy, system
 *        config / reboot / time-sync / telemetry-latest handlers and the station home page.
 *
 * The REST/config handlers live in rbwebserver_api.c, OTA in rbwebserver_ota.c, the Wi-Fi portal in
 * rbwebserver_wifi.c and embedded-UI hosting in rbwebserver_ui.c (shared surface:
 * rbwebserver_internal.h). All handlers run on the single httpd task.
 */

#include "rbwebserver.h"
#include "rbwebserver_internal.h"   /* shared surface for the _ota/_wifi/_api split TUs */
#include "rbwifi.h"
#include "rbjson.h"
#include "rbhome_types.h"   /* rbpower_home_lock/unlock — guard g_home_network reads on the httpd task */
#include "rbgrid_config_storage.h"
#include "rbgrid_config_api.h"
#include "rbwebsocket.h"
// #include "rbwebsocket_test.h"  // REMOVED: Test WebSocket endpoints removed for production
#include "rbwebsocket_telemetry.h"
#include "rbtelemetry.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_ota_ops.h"      // OTA: esp_ota_begin/write/end/set_boot
#include "esp_app_desc.h"     // esp_app_get_description (version endpoint)
#include "esp_partition.h"
#include "esp_https_server.h"  /* TLS server (httpd_ssl_start) */
#include "littlefs_raw.h"      /* optional prod cert/key from storage + https-enable flag */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static const char* TAG = "RBWEBSERVER";

/* ==== HTTPS / TLS ===========================================================================
 * The server can run plain HTTP (port 80, default) or TLS (port 443). The choice is a persisted
 * runtime flag (`/https_enabled` in littlefs), read at boot — toggling requires a reboot because
 * httpd_start and httpd_ssl_start are distinct ESP-IDF entry points. Cert source, in order:
 *   1. PROD: `/https_cert.pem` + `/https_key.pem` in littlefs (installed at provisioning — a real
 *      certificate for the device's DNS name). Used when BOTH files exist.
 *   2. DEV : embedded self-signed cert (components/rbpower/certs/) — always present as fallback.
 * Self-signed is fine for LAN dev (curl -k / browser exception); prod must install a real cert. */
/// @cond EMBEDDED_PEM
/* Linker symbols of the embedded self-signed development certificate and key (EMBED_TXTFILES). */
extern const unsigned char rbgrid_dev_cert_pem_start[] asm("_binary_rbgrid_dev_cert_pem_start");
extern const unsigned char rbgrid_dev_cert_pem_end[]   asm("_binary_rbgrid_dev_cert_pem_end");
extern const unsigned char rbgrid_dev_key_pem_start[]  asm("_binary_rbgrid_dev_key_pem_start");
extern const unsigned char rbgrid_dev_key_pem_end[]    asm("_binary_rbgrid_dev_key_pem_end");
/// @endcond

#define RBWEB_HTTPS_FLAG_FILE  "/https_enabled"   ///< littlefs flag file: present = start HTTPS at boot
#define RBWEB_HTTPS_CERT_FILE  "/https_cert.pem"  ///< littlefs file: production server certificate (PEM)
#define RBWEB_HTTPS_KEY_FILE   "/https_key.pem"   ///< littlefs file: production private key (PEM)
#define RBWEB_HTTPS_PORT       443                ///< HTTPS listening port

static bool g_https_enabled = false;   /* desired mode (persisted, read at boot) */
static bool g_https_active  = false;   /* what the running server actually started as */
/* Heap copies of a storage-loaded prod cert/key — kept alive for the server's lifetime
 * (httpd_ssl_config_t does not copy the PEM buffers; they must outlive the server). */
static char* g_https_cert_buf = NULL;
static char* g_https_key_buf  = NULL;

// Configuration constants — CONFIG_COMPACT_SIZE moved to rbwebserver_internal.h (shared)

// Module global variables
static httpd_handle_t g_server_handle = NULL;
static uint16_t g_server_port = 80;
static uint16_t g_max_open_sockets = 12;   /* ~10 WS clients (LWIP_MAX_SOCKETS 16) */
static rbwebserver_redirect_config_t g_redirect_config;
static bool g_initialized = false;
static bool g_running = false;

// Global context variables
static rbpower_extended_config_t* g_extended_config = NULL;
static rbgrid_json_context_t* g_json_context = NULL;

// WebSocket callback (keep this one)
static rbwebserver_ws_message_callback_t g_ws_message_callback = NULL;
static void* g_ws_message_user_data = NULL;

/* Accessors for the split handler TUs (_ota/_wifi/_api). The globals stay static here — notably
 * g_json_context, which is a SEPARATE static in rbjson.c and would collide at link if de-static'd. */
httpd_handle_t             rbwebserver_core_server(void)   { return g_server_handle; }
rbpower_extended_config_t *rbwebserver_core_ext_cfg(void)  { return g_extended_config; }
rbgrid_json_context_t     *rbwebserver_core_json_ctx(void) { return g_json_context; }

// Forward declarations (is_request_from_ap_network / add_cors_headers → rbwebserver_internal.h)
static esp_err_t universal_root_handler(httpd_req_t *req);
static esp_err_t rbwebserver_telemetry_latest_handler(httpd_req_t *req);
static esp_err_t rbwebserver_system_reboot_handler(httpd_req_t *req);
static esp_err_t rbwebserver_system_time_sync_handler(httpd_req_t *req);
static esp_err_t rbwebserver_restore_system_config_handler(httpd_req_t *req);

// New API handlers
// version/ota handlers moved to rbwebserver_ota.c (declared in rbwebserver_internal.h)


// WebSocket handler (stub - WebSocket API requires additional configuration)
// Commented out, websocket_test_handler is used instead
/*
static esp_err_t ws_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "WebSocket support is not enabled in this build");
    httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "WebSocket not supported");
    return ESP_OK;
}
*/

// Web server initialization
rbpower_err_t rbwebserver_init(const rbwebserver_config_t* config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Web server already initialized");
        return RBPOWER_OK;
    }
    
    if (config == NULL) {
        ESP_LOGE(TAG, "config == NULL");
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (config) {
        g_server_port = config->port;
        g_max_open_sockets = config->max_open_sockets;
        
        // Set global configuration pointers
        g_extended_config = config->extended_config;
        g_json_context = config->json_context;

        if (!g_extended_config) {
            ESP_LOGW(TAG, "extended_config not set in web server configuration");
        }
        if (!g_json_context) {
            ESP_LOGW(TAG, "json_context not set in web server configuration");
        }
    }
    g_initialized = true;

    ESP_LOGI(TAG, "Web server initialized with port %d", g_server_port);
    return RBPOWER_OK;
}

// Configure redirects
rbpower_err_t rbwebserver_configure_redirects(const rbwebserver_redirect_config_t* redirect_config)
{
    if (!g_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (redirect_config == NULL) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    memcpy(&g_redirect_config, redirect_config, sizeof(rbwebserver_redirect_config_t));
    ESP_LOGI(TAG, "Redirects configured: %s:%d", g_redirect_config.frontend_host, (int)g_redirect_config.frontend_port);

    return RBPOWER_OK;
}

// Helper function to check if request came from AP network
// Checks client IP - if in 192.168.4.x subnet, it's an AP client
bool is_request_from_ap_network(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in client_addr;
    struct sockaddr_in server_addr;
    socklen_t addr_size = sizeof(client_addr);

    // First try to get server address
    if (getsockname(fd, (struct sockaddr*)&server_addr, &addr_size) == 0) {
        char server_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &server_addr.sin_addr, server_ip, sizeof(server_ip));

        // If server IP is 192.168.4.1 - this is AP
        if (strcmp(server_ip, "192.168.4.1") == 0) {
            return true;
        }
    }

    // If getsockname failed, check client IP
    addr_size = sizeof(client_addr);
    if (getpeername(fd, (struct sockaddr*)&client_addr, &addr_size) < 0) {
        return false;
    }

    // Get client IP
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

    // Check if client is in AP subnet (192.168.4.x)
    return (strncmp(client_ip, "192.168.4.", 10) == 0);
}

/* ==== CORS policy. Config persists in littlefs /sec_cfg:
 * {"dev_cors":bool, "origins":["http://host[:port]", ...≤8]}. No file (or dev_cors true, the
 * DEFAULT) → wildcard "*" — existing installs and development frontends keep working with
 * zero migration. dev_cors false → only a listed Origin is echoed back (+Vary: Origin); an
 * unlisted Origin gets NO Access-Control-Allow-Origin header, so the browser blocks the read.
 * Non-browser clients (curl/python, no Origin) are unaffected — CORS is a browser contract. */
#define RBWEB_SEC_CFG_FILE "/sec_cfg"   ///< littlefs file holding the CORS/security policy JSON
/** @brief Maximum CORS allow-list entries: cloud app + capacitor://localhost + https://localhost +
 *         http://localhost (mobile wrappers) + spares. */
#define RBWEB_CORS_MAX_ORIGINS 8
static struct {
    bool loaded;
    bool dev_cors;
    int  n_origins;
    char origins[RBWEB_CORS_MAX_ORIGINS][64];
} s_sec_cfg = { .loaded = false, .dev_cors = true, .n_origins = 0 };

void rbwebserver_security_cfg_reload(void)
{
    s_sec_cfg.dev_cors = true;
    s_sec_cfg.n_origins = 0;
    s_sec_cfg.loaded = true;
    if (!littlefs_raw_exists(RBWEB_SEC_CFG_FILE)) return;
    char buf[512] = {0};
    size_t act = 0;
    if (littlefs_raw_read(RBWEB_SEC_CFG_FILE, buf, sizeof(buf) - 1, &act) != ESP_OK || !buf[0]) return;
    cJSON *j = cJSON_Parse(buf);
    if (!j) return;
    cJSON *dc = cJSON_GetObjectItem(j, "dev_cors");
    if (cJSON_IsBool(dc)) s_sec_cfg.dev_cors = cJSON_IsTrue(dc);
    cJSON *arr = cJSON_GetObjectItem(j, "origins");
    if (cJSON_IsArray(arr)) {
        cJSON *o = NULL;
        cJSON_ArrayForEach(o, arr) {
            if (cJSON_IsString(o) && o->valuestring[0] && s_sec_cfg.n_origins < RBWEB_CORS_MAX_ORIGINS) {
                strlcpy(s_sec_cfg.origins[s_sec_cfg.n_origins++], o->valuestring, 64);
            }
        }
    }
    cJSON_Delete(j);
}

bool rbwebserver_security_cfg_get(bool *dev_cors, char origins[][64], int max, int *n)
{
    if (!s_sec_cfg.loaded) rbwebserver_security_cfg_reload();
    if (dev_cors) *dev_cors = s_sec_cfg.dev_cors;
    int k = (s_sec_cfg.n_origins < max) ? s_sec_cfg.n_origins : max;
    for (int i = 0; i < k && origins; i++) strcpy(origins[i], s_sec_cfg.origins[i]);
    if (n) *n = k;
    return true;
}

// Universal function to add CORS headers
/* CORS allow-list probe for the preflight PNA grant (same policy source as add_cors_headers). */
static bool rbwebserver_security_origin_allowed(const char *origin)
{
    if (!s_sec_cfg.loaded) rbwebserver_security_cfg_reload();
    for (int i = 0; i < s_sec_cfg.n_origins; i++) {
        if (strcmp(origin, s_sec_cfg.origins[i]) == 0) return true;
    }
    return false;
}

void add_cors_headers(httpd_req_t *req)
{
    if (!s_sec_cfg.loaded) rbwebserver_security_cfg_reload();
    if (s_sec_cfg.dev_cors) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    } else {
        /* Echo the Origin only when it's allow-listed. The header value must outlive this call
         * (httpd stores the pointer) — use a per-request static-safe approach: match against the
         * cached config and point at the CONFIG copy, not the request buffer. */
        char origin[64] = {0};
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK && origin[0]) {
            for (int i = 0; i < s_sec_cfg.n_origins; i++) {
                if (strcmp(origin, s_sec_cfg.origins[i]) == 0) {
                    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", s_sec_cfg.origins[i]);
                    httpd_resp_set_hdr(req, "Vary", "Origin");
                    break;
                }
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    // Include all headers sent by React application
    /* X-API-Key: it is an accepted auth header; without it here cross-origin browser calls using it
     * failed preflight. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key, X-API-Version, X-Request-ID, x-request-id, X-FW-SHA256, X-FW-Version");
    /* Cross-origin JS can only read "safelisted" response headers unless exposed: the web app needs
     * Retry-After on 503 (store migration / not-ready) and Content-Disposition on export.csv. */
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "Retry-After, Content-Disposition");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "3600");

    /* Chrome Private Network Access: grant on EVERY preflight (centralized here because
     * the endpoint-local OPTIONS branches call add_cors_headers but never set PNA, so only the
     * wildcard handler used to grant it). Same policy as CORS itself. */
    if (req->method == HTTP_OPTIONS) {
        bool allow_pna = s_sec_cfg.dev_cors;
        if (!allow_pna) {
            char origin2[64] = {0};
            if (httpd_req_get_hdr_value_str(req, "Origin", origin2, sizeof(origin2)) == ESP_OK && origin2[0]) {
                allow_pna = rbwebserver_security_origin_allowed(origin2);
            }
        }
        if (allow_pna) {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
        }
    }
}

// Handler for OPTIONS requests (preflight)
esp_err_t cors_options_handler(httpd_req_t *req)
{
    // Add CORS headers (incl. the PNA grant — centralized in add_cors_headers for OPTIONS)
    add_cors_headers(req);

    // Set status 204 No Content for OPTIONS
    httpd_resp_set_status(req, "204 No Content");

    // Send empty response
    httpd_resp_send(req, "", 0);

    return ESP_OK;
}

/* Routing logic:
 * - APSTA mode uses universal_root_handler
 * - Interface detection by server IP address (192.168.4.1 = AP)
 * - AP clients redirected to /wifi
 * - STA clients get home page with instructions
 */

// Universal handler for AP mode - redirects to configuration page
esp_err_t rbwebserver_ap_root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "AP root handler: %s", req->uri);

    // Always redirect to WiFi configuration page in AP mode
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/wifi");
    httpd_resp_send(req, NULL, 0);
    
    return ESP_OK;
}

// (Wi-Fi setup page handler: rbwebserver_wifi.c)

// Register URI handlers
static esp_err_t register_uri_handlers(wifi_mode_t wifi_mode)
{
    ESP_LOGD(TAG, "Registering URI handlers for WiFi mode: %d", wifi_mode);

    // Handlers for all modes
    if (wifi_mode == WIFI_MODE_AP) {
        // In AP mode redirect all requests to configuration page
        httpd_uri_t wifi_config_uri = {
            .uri = "/wifi",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &wifi_config_uri);
        
        httpd_uri_t wifi_status_uri = {
            .uri = "/wifi/status",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &wifi_status_uri);
        
        httpd_uri_t wifi_save_uri = {
            .uri = "/wifi/save",
            .method = HTTP_POST,
            .handler = rbwebserver_wifi_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &wifi_save_uri);
        
        httpd_uri_t wifi_scan_uri = {
            .uri = "/wifi/scan",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_scan_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &wifi_scan_uri);

        httpd_uri_t wifi_scan_results_uri = {
            .uri = "/wifi/scan/results",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_scan_results_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &wifi_scan_results_uri);

        httpd_uri_t system_config_uri = {
            .uri = "/api/system/config",
            .method = HTTP_GET,
            .handler = rbwebserver_system_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &system_config_uri);
        
        httpd_uri_t system_config_post_uri = {
            .uri = "/api/system/config",
            .method = HTTP_POST,
            .handler = rbwebserver_system_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &system_config_post_uri);
        
        httpd_uri_t telemetry_latest_uri = {
            .uri = "/api/telemetry/latest",
            .method = HTTP_GET,
            .handler = rbwebserver_telemetry_latest_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &telemetry_latest_uri);
        
        // WebSocket endpoint (commented out, websocket_test_handler is used instead)
        /*
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server_handle, &ws_uri);
        */
        
        // Root handler for AP mode
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = rbwebserver_ap_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &root_uri);
        
        // Captive portal handlers
        httpd_uri_t captive_uri = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = rbwebserver_ap_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &captive_uri);
        
        httpd_uri_t captive_uri2 = {
            .uri = "/hotspot-detect.html",
            .method = HTTP_GET,
            .handler = rbwebserver_ap_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &captive_uri2);
        
        // REMOVED: Test WebSocket endpoints (/ws-test, /ws-test-socket) for production

        // WebSocket telemetry handler
        httpd_uri_t ws_telemetry_uri = {
            .uri = "/ws-telemetry",
            .method = HTTP_GET,
            .handler = websocket_telemetry_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server_handle, &ws_telemetry_uri);
        
    } else if (wifi_mode == WIFI_MODE_STA) {
        // STA mode handlers
        httpd_uri_t ap_wifi_config_uri = {
            .uri = "/wifi",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_wifi_config_uri);
        
        httpd_uri_t ap_wifi_status_uri = {
            .uri = "/wifi/status",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_wifi_status_uri);
        
        httpd_uri_t ap_wifi_save_uri = {
            .uri = "/wifi/save",
            .method = HTTP_POST,
            .handler = rbwebserver_wifi_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_wifi_save_uri);
        
        httpd_uri_t ap_wifi_scan_uri = {
            .uri = "/wifi/scan",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_scan_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_wifi_scan_uri);

        httpd_uri_t ap_wifi_scan_results_uri = {
            .uri = "/wifi/scan/results",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_scan_results_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_wifi_scan_results_uri);

        httpd_uri_t ap_system_config_uri = {
            .uri = "/api/system/config",
            .method = HTTP_GET,
            .handler = rbwebserver_system_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_system_config_uri);
        
        httpd_uri_t ap_system_config_post_uri = {
            .uri = "/api/system/config",
            .method = HTTP_POST,
            .handler = rbwebserver_system_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_system_config_post_uri);
        
        httpd_uri_t ap_telemetry_latest_uri = {
            .uri = "/api/telemetry/latest",
            .method = HTTP_GET,
            .handler = rbwebserver_telemetry_latest_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_telemetry_latest_uri);
        
        // WebSocket endpoint (commented out, websocket_test_handler is used instead)
        /*
        httpd_uri_t ap_ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server_handle, &ap_ws_uri);
        */
        
        // Root handler for STA mode
        // DEPRECATED: rbwebserver_root_redirect_handler is commented out
        // Using rbwebserver_ap_root_handler for redirect to /wifi
        httpd_uri_t ap_root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = rbwebserver_ap_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &ap_root_uri);
        
        // REMOVED: Test WebSocket endpoints (/ws-test, /ws-test-socket) for production

        // WebSocket telemetry handler for AP mode
        httpd_uri_t ap_ws_telemetry_uri = {
            .uri = "/ws-telemetry",
            .method = HTTP_GET,
            .handler = websocket_telemetry_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server_handle, &ap_ws_telemetry_uri);
        
    } else if (wifi_mode == WIFI_MODE_APSTA) {
        // APSTA mode handlers
        httpd_uri_t sta_wifi_config_uri = {
            .uri = "/wifi",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_wifi_config_uri);
        
        httpd_uri_t sta_wifi_status_uri = {
            .uri = "/wifi/status",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_wifi_status_uri);
        
        httpd_uri_t sta_wifi_save_uri = {
            .uri = "/wifi/save",
            .method = HTTP_POST,
            .handler = rbwebserver_wifi_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_wifi_save_uri);
        
        httpd_uri_t sta_wifi_scan_uri = {
            .uri = "/wifi/scan",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_scan_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_wifi_scan_uri);

        httpd_uri_t sta_wifi_scan_results_uri = {
            .uri = "/wifi/scan/results",
            .method = HTTP_GET,
            .handler = rbwebserver_wifi_scan_results_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_wifi_scan_results_uri);

        httpd_uri_t sta_system_config_uri = {
            .uri = "/api/system/config",
            .method = HTTP_GET,
            .handler = rbwebserver_system_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_system_config_uri);
        
        httpd_uri_t sta_system_config_post_uri = {
            .uri = "/api/system/config",
            .method = HTTP_POST,
            .handler = rbwebserver_system_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_system_config_post_uri);
        
        httpd_uri_t sta_telemetry_latest_uri = {
            .uri = "/api/telemetry/latest",
            .method = HTTP_GET,
            .handler = rbwebserver_telemetry_latest_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_telemetry_latest_uri);
        
        // WebSocket endpoint (commented out, websocket_test_handler is used instead)
        /*
        httpd_uri_t sta_ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server_handle, &sta_ws_uri);
        */
        
        // Captive-probe handlers: the firmware ALWAYS runs APSTA,
        // but these were only registered in the pure-AP branch — phones never got the automatic
        // "sign in to network" portal. Android (/generate_204) and Apple (/hotspot-detect.html)
        // probes from the AP subnet now redirect to /wifi like in AP mode.
        httpd_uri_t apsta_captive1 = {
            .uri = "/generate_204", .method = HTTP_GET,
            .handler = rbwebserver_ap_root_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &apsta_captive1);
        httpd_uri_t apsta_captive2 = {
            .uri = "/hotspot-detect.html", .method = HTTP_GET,
            .handler = rbwebserver_ap_root_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &apsta_captive2);

        // Root handler for APSTA mode - using universal handler
        httpd_uri_t sta_root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = universal_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &sta_root_uri);
        
        // REMOVED: Test WebSocket endpoints (/ws-test, /ws-test-socket) for production

        // WebSocket telemetry handler for APSTA mode
        httpd_uri_t sta_ws_telemetry_uri = {
            .uri = "/ws-telemetry",
            .method = HTTP_GET,
            .handler = websocket_telemetry_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server_handle, &sta_ws_telemetry_uri);
    }
    
    // ====== Register new API handlers (for all modes) ======
    
    // System Config API - GET
    httpd_uri_t api_system_config_get = {
        .uri = "/api/system/system-config",
        .method = HTTP_GET,
        .handler = rbwebserver_system_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_config_get);
    
    // System Config API - POST
    httpd_uri_t api_system_config_post = {
        .uri = "/api/system/system-config",
        .method = HTTP_POST,
        .handler = rbwebserver_system_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_config_post);
    
    // OPTIONS handler for system-config
    httpd_uri_t api_system_config_options = {
        .uri = "/api/system/system-config",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_config_options);
    
    // System Reboot API - POST
    httpd_uri_t api_system_reboot = {
        .uri = "/api/system/reboot",
        .method = HTTP_POST,
        .handler = rbwebserver_system_reboot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_reboot);

    // OPTIONS handler for reboot
    httpd_uri_t api_system_reboot_options = {
        .uri = "/api/system/reboot",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_reboot_options);

    // System Time Sync API - GET (get configuration and statistics)
    httpd_uri_t api_system_time_sync_get = {
        .uri = "/api/system/time-sync",
        .method = HTTP_GET,
        .handler = rbwebserver_system_time_sync_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_time_sync_get);

    // System Time Sync API - POST (start synchronization)
    httpd_uri_t api_system_time_sync_post = {
        .uri = "/api/system/time-sync",
        .method = HTTP_POST,
        .handler = rbwebserver_system_time_sync_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_time_sync_post);

    // OPTIONS handler for time-sync
    httpd_uri_t api_system_time_sync_options = {
        .uri = "/api/system/time-sync",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_system_time_sync_options);
    
    // Restore System Config API - POST
    httpd_uri_t api_restore_system_config = {
        .uri = "/api/system/restore-system-config",
        .method = HTTP_POST,
        .handler = rbwebserver_restore_system_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_restore_system_config);
    
    // OPTIONS handler for restore-system-config
    httpd_uri_t api_restore_system_config_options = {
        .uri = "/api/system/restore-system-config",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_restore_system_config_options);
    
    // Hardware Config API - GET
    httpd_uri_t api_hardware_config_get_uri = {
        .uri = "/api/system/hardware-config",
        .method = HTTP_GET,
        .handler = rbwebserver_api_hardware_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_hardware_config_get_uri);
    
    // Hardware Config API - POST
    httpd_uri_t api_hardware_config_post_uri = {
        .uri = "/api/system/hardware-config",
        .method = HTTP_POST,
        .handler = rbwebserver_api_hardware_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_hardware_config_post_uri);
    
    // Time Config API
    httpd_uri_t api_time_config_uri = {
        .uri = "/api/system/time-config",
        .method = HTTP_GET,
        .handler = rbwebserver_api_time_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_time_config_uri);
    
    // Tariff Plan Config API - GET
    httpd_uri_t api_tariff_plan_config_get_uri = {
        .uri = "/api/system/tariff-plan-config",
        .method = HTTP_GET,
        .handler = rbwebserver_api_tariff_plan_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_tariff_plan_config_get_uri);

    // Tariff Plan Config API - POST
    httpd_uri_t api_tariff_plan_config_post_uri = {
        .uri = "/api/system/tariff-plan-config",
        .method = HTTP_POST,
        .handler = rbwebserver_api_tariff_plan_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_tariff_plan_config_post_uri);

    // Tariff Plan Config API - OPTIONS (for CORS)
    httpd_uri_t api_tariff_plan_config_options_uri = {
        .uri = "/api/system/tariff-plan-config",
        .method = HTTP_OPTIONS,
        .handler = rbwebserver_api_tariff_plan_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_tariff_plan_config_options_uri);


    // Telemetry Config API
    httpd_uri_t api_telemetry_config_uri = {
        .uri = "/api/system/telemetry-config",
        .method = HTTP_GET,
        .handler = rbwebserver_api_telemetry_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_telemetry_config_uri);
    // POST/OPTIONS for the telemetry profile (lean/full).
    httpd_uri_t api_tele_cfg_post = { .uri = "/api/system/telemetry-config", .method = HTTP_POST,    .handler = rbwebserver_api_telemetry_config_handler, .user_ctx = NULL };
    httpd_uri_t api_tele_cfg_opt  = { .uri = "/api/system/telemetry-config", .method = HTTP_OPTIONS, .handler = rbwebserver_api_telemetry_config_handler, .user_ctx = NULL };
    httpd_register_uri_handler(g_server_handle, &api_tele_cfg_post);
    httpd_register_uri_handler(g_server_handle, &api_tele_cfg_opt);

    // WiFi Config API
    httpd_uri_t api_wifi_config_uri = {
        .uri = "/api/system/wifi-config",
        .method = HTTP_GET,
        .handler = rbwebserver_api_wifi_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_wifi_config_uri);
    httpd_uri_t api_ap_uri = { .uri = "/api/system/ap", .method = HTTP_GET, .handler = rbwebserver_ap_info_handler, .user_ctx = NULL };
    httpd_uri_t api_ap_set = { .uri = "/api/system/ap", .method = HTTP_POST, .handler = rbwebserver_ap_set_handler, .user_ctx = NULL };
    httpd_register_uri_handler(g_server_handle, &api_ap_uri);
    httpd_register_uri_handler(g_server_handle, &api_ap_set);
    
    // Units Config API - GET
    httpd_uri_t api_units_config_get_uri = {
        .uri = "/api/system/units-config",
        .method = HTTP_GET,
        .handler = rbwebserver_api_units_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_units_config_get_uri);
    
    // Units Config API - POST
    httpd_uri_t api_units_config_post_uri = {
        .uri = "/api/system/units-config",
        .method = HTTP_POST,
        .handler = rbwebserver_api_units_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_units_config_post_uri);
    
    // Units Config API - OPTIONS (CORS preflight)
    httpd_uri_t api_units_config_options_uri = {
        .uri = "/api/system/units-config",
        .method = HTTP_OPTIONS,
        .handler = rbwebserver_api_units_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_units_config_options_uri);
    

    // History API
    httpd_uri_t api_history_uri = {
        .uri = "/api/v1/history",
        .method = HTTP_GET,
        .handler = rbwebserver_api_history_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_history_uri);

    // Economics API — GET /api/v1/economics?unit=<hex> (per-zone cost/revenue + net-metering
    // + progressive tiers).
    httpd_uri_t api_economics_uri = {
        .uri = "/api/v1/economics",
        .method = HTTP_GET,
        .handler = rbwebserver_api_economics_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_economics_uri);

    // CSV export — GET /api/v1/export.csv?unit=&period=&type=&from=&to= (streamed download).
    httpd_uri_t api_export_csv_uri = {
        .uri = "/api/v1/export.csv",
        .method = HTTP_GET,
        .handler = rbwebserver_api_export_csv_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_export_csv_uri);

    // Summary — GET /api/v1/summary?unit=&period=&from=&to= (range totals + by-zone).
    httpd_uri_t api_summary_uri = {
        .uri = "/api/v1/summary",
        .method = HTTP_GET,
        .handler = rbwebserver_api_summary_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_summary_uri);

    // API schema descriptor — GET /api/v1/schema (contract baseline for drift checks).
    httpd_uri_t api_schema_uri = {
        .uri = "/api/v1/schema",
        .method = HTTP_GET,
        .handler = rbwebserver_api_schema_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_schema_uri);

    // MQTT broker config — GET/POST/OPTIONS /api/system/mqtt-config (apply-on-restart).
    httpd_uri_t api_mqtt_cfg_get  = { .uri = "/api/system/mqtt-config", .method = HTTP_GET,     .handler = rbwebserver_api_mqtt_config_handler, .user_ctx = NULL };
    httpd_uri_t api_mqtt_cfg_post = { .uri = "/api/system/mqtt-config", .method = HTTP_POST,    .handler = rbwebserver_api_mqtt_config_handler, .user_ctx = NULL };
    httpd_uri_t api_mqtt_cfg_opt  = { .uri = "/api/system/mqtt-config", .method = HTTP_OPTIONS, .handler = rbwebserver_api_mqtt_config_handler, .user_ctx = NULL };
    httpd_register_uri_handler(g_server_handle, &api_mqtt_cfg_get);
    httpd_register_uri_handler(g_server_handle, &api_mqtt_cfg_post);
    httpd_register_uri_handler(g_server_handle, &api_mqtt_cfg_opt);

    // API token provisioning — GET/POST/OPTIONS /api/system/token.
    httpd_uri_t api_token_get  = { .uri = "/api/system/token", .method = HTTP_GET,     .handler = rbwebserver_api_token_handler, .user_ctx = NULL };
    httpd_uri_t api_token_post = { .uri = "/api/system/token", .method = HTTP_POST,    .handler = rbwebserver_api_token_handler, .user_ctx = NULL };
    httpd_uri_t api_token_opt  = { .uri = "/api/system/token", .method = HTTP_OPTIONS, .handler = rbwebserver_api_token_handler, .user_ctx = NULL };
    // CORS/security policy: GET/POST/OPTIONS /api/system/security.
    httpd_uri_t api_sec_get  = { .uri = "/api/system/security", .method = HTTP_GET,     .handler = rbwebserver_api_security_handler, .user_ctx = NULL };
    httpd_uri_t api_sec_post = { .uri = "/api/system/security", .method = HTTP_POST,    .handler = rbwebserver_api_security_handler, .user_ctx = NULL };
    httpd_uri_t api_sec_opt  = { .uri = "/api/system/security", .method = HTTP_OPTIONS, .handler = rbwebserver_api_security_handler, .user_ctx = NULL };
    httpd_register_uri_handler(g_server_handle, &api_sec_get);
    httpd_register_uri_handler(g_server_handle, &api_sec_post);
    httpd_register_uri_handler(g_server_handle, &api_sec_opt);
    httpd_register_uri_handler(g_server_handle, &api_token_get);
    httpd_register_uri_handler(g_server_handle, &api_token_post);
    httpd_register_uri_handler(g_server_handle, &api_token_opt);

    // Full-config backup/restore — /api/system/backup (GET/POST) + /api/system/restore (POST).
    httpd_uri_t api_backup_get  = { .uri = "/api/system/backup",  .method = HTTP_GET,     .handler = rbwebserver_api_backup_handler,  .user_ctx = NULL };
    httpd_uri_t api_backup_post = { .uri = "/api/system/backup",  .method = HTTP_POST,    .handler = rbwebserver_api_backup_handler,  .user_ctx = NULL };
    httpd_uri_t api_backup_opt  = { .uri = "/api/system/backup",  .method = HTTP_OPTIONS, .handler = rbwebserver_api_backup_handler,  .user_ctx = NULL };
    httpd_uri_t api_restore_post = { .uri = "/api/system/restore", .method = HTTP_POST,    .handler = rbwebserver_api_restore_handler, .user_ctx = NULL };
    httpd_uri_t api_restore_opt  = { .uri = "/api/system/restore", .method = HTTP_OPTIONS, .handler = rbwebserver_api_restore_handler, .user_ctx = NULL };
    httpd_register_uri_handler(g_server_handle, &api_backup_get);
    httpd_register_uri_handler(g_server_handle, &api_backup_post);
    httpd_register_uri_handler(g_server_handle, &api_backup_opt);
    httpd_register_uri_handler(g_server_handle, &api_restore_post);
    httpd_register_uri_handler(g_server_handle, &api_restore_opt);

    // Unit deletion — REST DELETE /api/v1/units/{hexid} (wildcard path param).
    httpd_uri_t api_unit_delete_uri = {
        .uri = "/api/v1/units/*",
        .method = HTTP_DELETE,
        .handler = rbwebserver_api_unit_delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_unit_delete_uri);

    // Unit bind — REST POST /api/v1/units (create a unit from an unbound sensor).
    httpd_uri_t api_unit_create_uri = {
        .uri = "/api/v1/units",
        .method = HTTP_POST,
        .handler = rbwebserver_api_unit_create_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_unit_create_uri);

    // POST /api/v1/units/{hex} (params patch) + /rebind + /rename (wildcard path param; op picked in handler).
    httpd_uri_t api_unit_mutate_uri = {
        .uri = "/api/v1/units/*",
        .method = HTTP_POST,
        .handler = rbwebserver_api_unit_mutate_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_unit_mutate_uri);

    // OTA — GET /api/system/version (running firmware) + POST /api/system/ota (flash + reboot).
    httpd_uri_t api_version_uri = {
        .uri = "/api/system/version", .method = HTTP_GET,
        .handler = rbwebserver_api_version_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_version_uri);
    httpd_uri_t api_ota_uri = {
        .uri = "/api/system/ota", .method = HTTP_POST,
        .handler = rbwebserver_api_ota_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_ota_uri);

    // Sensor discovery — GET /api/v1/sensors.
    httpd_uri_t api_sensors_uri = {
        .uri = "/api/v1/sensors",
        .method = HTTP_GET,
        .handler = rbwebserver_api_sensors_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &api_sensors_uri);

    ESP_LOGD(TAG, "Registered 9 new API endpoints");

    // Register OPTIONS handlers for all API endpoints
    const char* api_endpoints[] = {
        "/api/system/system-config",
        "/api/system/hardware-config",
        "/api/system/time-config",
        "/api/system/tariff-plan-config",
        "/api/system/telemetry-config",
        "/api/system/wifi-config",
        "/api/system/units-config",
        "/api/system/lang",
        "/api/system/ap",
        "/api/v1/history",
        "/api/v1/units",      // POST bind preflight
        "/api/v1/units/*",    // DELETE preflight (wildcard path param)
        "/api/v1/sensors",
        "/api/system/ota",    // OTA — POST firmware preflight
        "/api/system/version",
        // token + security NOT here: both register their OPTIONS explicitly (duplicate → HANDLER_EXISTS)
        "/api/telemetry/latest",
        "/api/system/config",
        "/api/system/reboot",
        "/api/system/restore-system-config",
        "/wifi",
        "/wifi/status",
        "/wifi/save",
        "/wifi/scan"
    };
    
    for (int i = 0; i < sizeof(api_endpoints) / sizeof(api_endpoints[0]); i++) {
        httpd_uri_t options_uri = {
            .uri = api_endpoints[i],
            .method = HTTP_OPTIONS,
            .handler = cors_options_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_server_handle, &options_uri);
    }
    
    // Catch-all OPTIONS preflight: a single wildcard handler so EVERY endpoint answers CORS preflight
    // (the fixed list above can miss routes, and a web client sending custom headers — X-Request-ID
    // etc. — triggers preflight on every request, GET included). Wildcard matcher is enabled, so this
    // covers any path; the specific OPTIONS above remain as exact-match fast paths. (Without it,
    // cross-origin development frontend → device calls were blocked.)
    httpd_uri_t options_wildcard = {
        .uri = "/*",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
        .user_ctx = NULL
    };
    esp_err_t opt_err = httpd_register_uri_handler(g_server_handle, &options_wildcard);
    ESP_LOGI(TAG, "Registered OPTIONS wildcard (/*) for CORS preflight: %s", esp_err_to_name(opt_err));

    /* Embedded web-UI: upload/commit + the GET catch-all. The catch-all
     * MUST be the LAST registration — every /api, /wifi, /ws handler above wins by order. */
    httpd_uri_t ui_upload_uri = {
        .uri = "/api/system/ui-upload",
        .method = HTTP_POST,
        .handler = rbwebserver_ui_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &ui_upload_uri);
    httpd_uri_t ui_commit_uri = {
        .uri = "/api/system/ui-commit",
        .method = HTTP_POST,
        .handler = rbwebserver_ui_commit_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_handle, &ui_commit_uri);
    /* one language pack on the hub */
    httpd_uri_t lang_get_uri  = { .uri = "/api/system/lang", .method = HTTP_GET,  .handler = rbwebserver_ui_lang_handler, .user_ctx = NULL };
    httpd_uri_t lang_post_uri = { .uri = "/api/system/lang", .method = HTTP_POST, .handler = rbwebserver_ui_lang_handler, .user_ctx = NULL };
    httpd_register_uri_handler(g_server_handle, &lang_get_uri);
    httpd_register_uri_handler(g_server_handle, &lang_post_uri);
    /* NOT a slash-star catch-all: espnow/provisioning (and future routes) are registered from main
     * AFTER the server starts, and the first-registered match wins — a GET catch-all here
     * swallowed them. Explicit UI prefixes instead; anything
     * new in the kit outside these must go under /static/ (or extend this list). */
    static const char *UI_GET_PATTERNS[] = {
        "/index.html", "/static/*", "/fonts/*", "/manifest.json", "/asset-manifest.json",
        "/ui-manifest.json", "/favicon.*", "/apple-icon.png", "/robots.txt", "/icons/*", "/logo*",
        "/lang/*",   /* the stored language pack (served without a UI kit too) */
    };
    esp_err_t ui_err = ESP_OK;
    for (unsigned i = 0; i < sizeof(UI_GET_PATTERNS) / sizeof(UI_GET_PATTERNS[0]); i++) {
        httpd_uri_t u = {
            .uri = UI_GET_PATTERNS[i],
            .method = HTTP_GET,
            .handler = rbwebserver_ui_file_handler,
            .user_ctx = NULL
        };
        esp_err_t e = httpd_register_uri_handler(g_server_handle, &u);
        if (e != ESP_OK) ui_err = e;
    }
    ESP_LOGI(TAG, "Registered embedded-UI handlers (%s, ui %s)",
             esp_err_to_name(ui_err), rbwebserver_ui_installed() ? "installed" : "stub");

    /* CANARY (added after silent max_uri_handlers overflows — espnow/provisioning routes
     * fell off the table): register one throwaway handler LAST; if IT fails, the table is full and
     * some earlier registration already failed silently → scream. Then free the slot again. */
    {
        httpd_uri_t canary = {
            .uri = "/__canary", .method = HTTP_GET, .handler = cors_options_handler, .user_ctx = NULL
        };
        if (httpd_register_uri_handler(g_server_handle, &canary) != ESP_OK) {
            ESP_LOGE(TAG, "max_uri_handlers EXHAUSTED — some API routes are DEAD; raise the limit!");
        } else {
            httpd_unregister_uri_handler(g_server_handle, "/__canary", HTTP_GET);
        }
    }

    return ESP_OK;
}

/* Load a PEM file from littlefs into a freshly malloc'd, NUL-terminated buffer.
 * Returns the byte length INCLUDING the trailing NUL (esp_https_server expects len = strlen+1 for
 * PEM), or 0 on any failure. Caller owns *out and must free it. */
static size_t rbweb_load_pem(const char* path, char** out)
{
    *out = NULL;
    if (!littlefs_raw_exists(path)) return 0;
    char* buf = malloc(4096);
    if (!buf) return 0;
    size_t act = 0;
    if (littlefs_raw_read(path, buf, 4095, &act) != ESP_OK || act == 0) { free(buf); return 0; }
    buf[act] = '\0';
    *out = buf;
    return act + 1;   /* include NUL terminator in the length, per esp_https_server PEM contract */
}

// Start web server
rbpower_err_t rbwebserver_start()
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Web server not initialized");
        return RBPOWER_ERR_NOT_INIT;
    }

    if (g_running) {
        ESP_LOGW(TAG, "Web server already running");
        return RBPOWER_OK;
    }

    /* Embedded-UI storage (idempotent; a failed mount only disables UI hosting, stub remains). */
    rbwebserver_ui_mount();

    esp_err_t ret;

    if (g_https_enabled) {
        // ---- TLS path ----------- ---------------------------------------------------------------
        httpd_ssl_config_t sslc = HTTPD_SSL_CONFIG_DEFAULT();
        sslc.port_secure = RBWEB_HTTPS_PORT;
        sslc.httpd.ctrl_port        = 32768;
        sslc.httpd.max_open_sockets = g_max_open_sockets;
        sslc.httpd.max_uri_handlers = 128;  // 72 overflowed — tail registrations (espnow GET/POST,
        // provisioning) failed SILENTLY and the CORS OPTIONS wildcard turned them into 405s. ~86 needed.
        sslc.httpd.stack_size       = 10240;   // TLS handshake needs more stack than plain HTTP
        sslc.httpd.lru_purge_enable = true;
        sslc.httpd.uri_match_fn     = httpd_uri_match_wildcard;

        // Cert source: prod cert/key from storage if BOTH present, else embedded self-signed dev cert.
        size_t cert_len = rbweb_load_pem(RBWEB_HTTPS_CERT_FILE, &g_https_cert_buf);
        size_t key_len  = rbweb_load_pem(RBWEB_HTTPS_KEY_FILE,  &g_https_key_buf);
        if (cert_len && key_len) {
            sslc.servercert     = (const unsigned char*)g_https_cert_buf;
            sslc.servercert_len = cert_len;
            sslc.prvtkey_pem    = (const unsigned char*)g_https_key_buf;
            sslc.prvtkey_len    = key_len;
            ESP_LOGI(TAG, "HTTPS: using PROD cert from storage (%s)", RBWEB_HTTPS_CERT_FILE);
        } else {
            if (g_https_cert_buf) { free(g_https_cert_buf); g_https_cert_buf = NULL; }
            if (g_https_key_buf)  { free(g_https_key_buf);  g_https_key_buf  = NULL; }
            sslc.servercert     = rbgrid_dev_cert_pem_start;
            sslc.servercert_len = rbgrid_dev_cert_pem_end - rbgrid_dev_cert_pem_start;
            sslc.prvtkey_pem    = rbgrid_dev_key_pem_start;
            sslc.prvtkey_len    = rbgrid_dev_key_pem_end - rbgrid_dev_key_pem_start;
            ESP_LOGW(TAG, "HTTPS: using embedded SELF-SIGNED dev cert (no prod cert in storage)");
        }

        ret = httpd_ssl_start(&g_server_handle, &sslc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(ret));
            if (g_https_cert_buf) { free(g_https_cert_buf); g_https_cert_buf = NULL; }
            if (g_https_key_buf)  { free(g_https_key_buf);  g_https_key_buf  = NULL; }
            return RBPOWER_ERR_HTTP_INIT_FAILED;
        }
        g_https_active = true;
        g_server_port  = RBWEB_HTTPS_PORT;
    } else {
        // ---- plain HTTP path (default) --------------------------------------------------------
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = g_server_port;
        config.ctrl_port = 32768;
        config.max_open_sockets = g_max_open_sockets;
        config.max_uri_handlers = 128; // 72 OVERFLOWED (espnow GET/POST fell off the table →
        // wildcard-OPTIONS made them 405). Count grew with history/export/backup/espnow; ~86 now, rest = headroom.
        config.stack_size = 8192;
        config.lru_purge_enable = true;
        // Wildcard URI matching so REST path params work (e.g. DELETE /api/v1/units/{hexid}
        // registered as /api/v1/units/*). The wildcard matcher matches exact URIs identically, so all
        // existing fixed routes are unaffected (none use '*'/'?').
        config.uri_match_fn = httpd_uri_match_wildcard;

        // Start HTTP server
        ret = httpd_start(&g_server_handle, &config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            return RBPOWER_ERR_HTTP_INIT_FAILED;
        }
        g_https_active = false;
    }

    // Initialize WebSocket module with server handle
    esp_err_t ws_ret = rbwebsocket_init(g_server_handle);
    if (ws_ret != ESP_OK) {
        ESP_LOGW(TAG, "WebSocket initialization failed: %s", esp_err_to_name(ws_ret));
    } else {
        ESP_LOGI(TAG, "WebSocket module initialized");
    }

    // Register URI handlers based on WiFi mode
    wifi_mode_t wifi_mode;
    esp_wifi_get_mode(&wifi_mode);
    register_uri_handlers(wifi_mode);

    g_running = true;
    ESP_LOGI(TAG, "Web server started on port %d (%s)", g_server_port, g_https_active ? "HTTPS/TLS" : "HTTP");

    return RBPOWER_OK;
}

// Stop web server
rbpower_err_t rbwebserver_stop()
{
    if (!g_running) {
        ESP_LOGW(TAG, "Web server not running");
        return RBPOWER_OK;
    }

    esp_err_t ret = g_https_active ? httpd_ssl_stop(g_server_handle) : httpd_stop(g_server_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop web server: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_HTTP_INIT_FAILED;
    }

    g_server_handle = NULL;
    g_running = false;
    g_https_active = false;
    if (g_https_cert_buf) { free(g_https_cert_buf); g_https_cert_buf = NULL; }
    if (g_https_key_buf)  { free(g_https_key_buf);  g_https_key_buf  = NULL; }
    ESP_LOGI(TAG, "Web server stopped");

    return RBPOWER_OK;
}

/* ==== HTTPS enable/disable API (persisted; takes effect on next boot) ======================== */
void rbwebserver_set_https(bool enabled)
{
    g_https_enabled = enabled;
    if (enabled) littlefs_raw_write(RBWEB_HTTPS_FLAG_FILE, "1", 2);
    else         littlefs_raw_remove(RBWEB_HTTPS_FLAG_FILE);
}

bool rbwebserver_get_https(void)
{
    return g_https_enabled;
}

bool rbwebserver_https_active(void)
{
    return g_https_active;
}

void rbwebserver_load_https_flag(void)
{
    g_https_enabled = littlefs_raw_exists(RBWEB_HTTPS_FLAG_FILE);
    ESP_LOGI(TAG, "HTTPS flag at boot: %s", g_https_enabled ? "ENABLED (port 443)" : "disabled (HTTP port 80)");
}

// Set global context
rbpower_err_t rbwebserver_set_global_context(rbpower_extended_config_t* extended_config, 
                                            rbgrid_json_context_t* json_context)
{
    if (!g_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    g_extended_config = extended_config;
    g_json_context = json_context;
    
    return RBPOWER_OK;
}

/**
 * @brief Set the callback for WebSocket messages (stored only; no current handler invokes it).
 * @param[in] callback   Callback function.
 * @param[in] user_data  User data passed back to the callback.
 * @retval RBPOWER_OK            Stored.
 * @retval RBPOWER_ERR_NOT_INIT  rbwebserver_init() not called.
 */
rbpower_err_t rbwebserver_set_ws_message_callback(rbwebserver_ws_message_callback_t callback, void* user_data)
{
    if (!g_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    g_ws_message_callback = callback;
    g_ws_message_user_data = user_data;
    
    return RBPOWER_OK;
}

// WiFi status handler

// ===================== Unified command dispatcher (config via MQTT + HTTP) =====================
// One entry point that maps a command JSON → a response JSON, REUSING the exact same builders and
// parsers the per-section HTTP handlers use. The MQTT command channel (rbgrid/<mac>/cmd) routes
// through here so configuration is fully manageable over MQTT, identical to HTTP.
// (Implementation: rbgrid_command_dispatch() in rbwebserver_api.c.)
//
// Request  (→ rbgrid/<mac>/cmd):  {"id":"<opt>","cmd":"get","section":"<sec>"}
//                                 {"id":"<opt>","cmd":"set","section":"<sec>","data":{...}}
// Response (← rbgrid/<mac>/cmd/resp):
//   get → the section builder's enveloped JSON verbatim (same as the HTTP GET body).
//   set → a "cmd_ack" envelope: data:{cmd,section,result:"ok|error",message,id}.
// Sections (get): system, telemetry, wifi, units, tariff, adc_sensors, telemetry_data (live).
// Sections (set): units (rbgrid_storage_save_units), tariff (rbgrid_json_parse_tariff_plan_config).

esp_err_t rbwebserver_system_config_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "   ==== GET /api/system/system-config request");

        if (g_extended_config) {
            ESP_LOGI(TAG, "   ==== GET g_extended_config OK");
            char* json_string = rbgrid_json_create_system_config(g_extended_config);
            if (json_string) {
                ESP_LOGI(TAG, "   ==== GET json_string OK");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, json_string, strlen(json_string));
                free(json_string);
                return ESP_OK;
            }
        }
        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get config");
        return ESP_FAIL;
        
    } else if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // admin token required (if provisioned)
        ESP_LOGI(TAG, "POST /api/system/system-config request");

        // Allocate buffer for JSON data (config can be larger)
        char* buf = malloc(2048);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for JSON buffer");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        int remaining = req->content_len;
        
        if (remaining > 2047) {
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
        
        // Extract system section from JSON
        cJSON* system = cJSON_GetObjectItem(root, "system");
        if (!system) {
            ESP_LOGE(TAG, "No 'system' section in JSON");
            cJSON_Delete(root);
            
            // Create error response JSON
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddStringToObject(error_response, "status", "ERROR");
                cJSON_AddStringToObject(error_response, "message", "Missing 'system' section in request");
                cJSON_AddStringToObject(error_response, "error_code", "MISSING_SYSTEM_SECTION");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'system' section");
                }
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'system' section");
            }
            return ESP_FAIL;
        }
        
        // Create local config context
        rbpower_extended_config_t* local_config = calloc(1, sizeof(rbpower_extended_config_t));
        if (!local_config) {
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
        
        // Initialize string pointers to NULL for proper memory management
        local_config->home_name = NULL;
        local_config->location = NULL;
        local_config->language = NULL;
        
        // Load existing config from Flash first (to preserve other fields)
        esp_err_t load_result = rbgrid_storage_load_system(local_config);
        if (load_result != ESP_OK) {
            ESP_LOGW(TAG, "Could not load existing config from Flash, using defaults");
        }
        
        {   /* numeric ranges (schema limits.ranges): reject BEFORE parse/save — 400 names the field */
            extern void rbapi_send_range_error(httpd_req_t *req, const char *field, const char *msg);
            char rf[48], rm[128];
            /* language = any ISO 639-1 code (2 letters) or "" (language packs load on demand,
             * a new language must not need a firmware release). Was a silent keep on anything but RU/EN. */
            const cJSON* jl = cJSON_GetObjectItem(system, "language");
            if (jl && (!cJSON_IsString(jl) || !(jl->valuestring[0] == '\0' ||
                       (isalpha((unsigned char)jl->valuestring[0]) && isalpha((unsigned char)jl->valuestring[1]) &&
                        jl->valuestring[2] == '\0')))) {
                cJSON_Delete(root);
                rbgrid_json_free_config_strings(local_config);
                free(local_config);
                rbapi_send_range_error(req, "language", "language must be an ISO 639-1 code (2 letters) or empty");
                return ESP_FAIL;
            }
            if (!rbgrid_config_check_ranges("system", system, rf, sizeof(rf), rm, sizeof(rm))) {
                ESP_LOGE(TAG, "system config rejected: %s", rm);
                cJSON_Delete(root);
                rbgrid_json_free_config_strings(local_config);
                free(local_config);
                rbapi_send_range_error(req, rf, rm);
                return ESP_FAIL;
            }
        }
        // Parse JSON into local config structure
        /* Only these fields really apply on restart; snapshot them to tell the client the truth */
        __typeof__(local_config->rbhome_settings) prev_settings = local_config->rbhome_settings;
        __typeof__(local_config->rbhome_telemetry_settings) prev_tel = local_config->rbhome_telemetry_settings;
        bool prev_auto = local_config->auto_start_measurements;
        rbgrid_json_err_t parse_result = rbgrid_json_parse_system_section(system, local_config);
        
        if (parse_result != RBGRID_JSON_OK) {
            ESP_LOGE(TAG, "Failed to parse system config: %d", parse_result);
            cJSON_Delete(root);
            
            // Clean up local config
            rbgrid_json_free_config_strings(local_config);
            free(local_config);
            
            // Create error response JSON
            cJSON* error_response = cJSON_CreateObject();
            if (error_response) {
                cJSON_AddStringToObject(error_response, "status", "ERROR");
                cJSON_AddStringToObject(error_response, "message", "Failed to parse configuration");
                cJSON_AddStringToObject(error_response, "error_code", "CONFIG_PARSE_FAILED");
                
                char* error_str = cJSON_PrintUnformatted(error_response);
                cJSON_Delete(error_response);
                
                if (error_str) {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, error_str, strlen(error_str));
                    free(error_str);
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse configuration");
                }
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse configuration");
            }
            return ESP_FAIL;
        }
        
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Configuration parsed successfully, saving to Flash...");
        
        // Save updated config to Flash using local config
        esp_err_t save_result = rbgrid_storage_save_system(local_config);

        /* These fields are declarative (no subsystem reads them) — mirror them into the
         * live config so GET reflects the change without the reboot the response asks for. */
        if (save_result == ESP_OK && g_extended_config) {
            g_extended_config->site_profile = local_config->site_profile;
            g_extended_config->ev_config = local_config->ev_config;
            /* language live-apply too: GET reads the LIVE config — without
             * this mirror a saved language only showed after reboot. strdup: local_config's strings
             * are freed right below. (The old live pointer may be a flash-constant — leak-free skip.) */
            if (local_config->language) {
                char* lang_copy = strdup(local_config->language);
                if (lang_copy) g_extended_config->language = lang_copy;
            }
            /* groups live-apply: plain array copy */
            memcpy(g_extended_config->groups, local_config->groups, sizeof(g_extended_config->groups));
            g_extended_config->groups_count = local_config->groups_count;
            /* grid.system live-apply (phase topology) */
            g_extended_config->grid_system = local_config->grid_system;
            /* home_name / location are labels — live too (the UI used to ask for a reboot to rename) */
            if (local_config->home_name) { char* c = strdup(local_config->home_name); if (c) g_extended_config->home_name = c; }
            if (local_config->location)  { char* c = strdup(local_config->location);  if (c) g_extended_config->location = c; }
        }
        /* every system-config field the API exposes now applies live (the restart-only ones were never read and
         * were removed from the API) */
        (void)prev_settings; (void)prev_tel; (void)prev_auto;
        bool needs_reboot = false;

        // Clean up local config after save
        rbgrid_json_free_config_strings(local_config);
        free(local_config);
        
        // Create response JSON
        cJSON* response = cJSON_CreateObject();
        if (!response) {
            ESP_LOGE(TAG, "Failed to create response JSON");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        if (save_result == ESP_OK) {
            ESP_LOGI(TAG, "Configuration saved successfully");
            
            // Success response
            cJSON_AddStringToObject(response, "status", "OK");
            cJSON_AddStringToObject(response, "message", "Configuration saved successfully");
            /* true only when a restart-applied field changed (logging / telemetry retry / auto-start);
             * names, language, profile, EV, groups and grid.system are live */
            cJSON_AddBoolToObject(response, "requires_reboot", needs_reboot);
        } else {
            ESP_LOGE(TAG, "Failed to save config to Flash: %s", esp_err_to_name(save_result));
            
            // Error response
            cJSON_AddStringToObject(response, "status", "ERROR");
            cJSON_AddStringToObject(response, "message", "Failed to save configuration");
            cJSON_AddStringToObject(response, "error_code", "CONFIG_SAVE_FAILED");
        }
        
        // Send response
        char* response_str = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        
        if (response_str) {
            httpd_resp_set_type(req, "application/json");
            esp_err_t send_result = httpd_resp_send(req, response_str, strlen(response_str));
            free(response_str);
            return (save_result == ESP_OK) ? send_result : ESP_FAIL;
        } else {
            ESP_LOGE(TAG, "Failed to serialize response JSON");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create response");
            return ESP_FAIL;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

// Task to perform reboot in separate context
static void reboot_task(void *pvParameters) {
    uint32_t delay_ms = (uint32_t)(uintptr_t)pvParameters;

    ESP_LOGI(TAG, "Reboot task started, delay: %lu ms", (unsigned long)delay_ms);

    // Small delay to ensure HTTP response is sent
    vTaskDelay(pdMS_TO_TICKS(100));

    // Perform reboot
    rbgrid_system_reboot(delay_ms);

    // This task should terminate after esp_restart() call
    vTaskDelete(NULL);
}

// Backup configuration restore handler
static esp_err_t rbwebserver_restore_system_config_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    
    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // admin token required (if provisioned)
        ESP_LOGI(TAG, "POST /api/system/restore-system-config request");
        
        // Create response JSON
        cJSON* response = cJSON_CreateObject();
        if (!response) {
            ESP_LOGE(TAG, "Failed to create response JSON");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        // First check if backup exists
        esp_err_t check_result = rbgrid_storage_check_backup(NULL);
        if (check_result != ESP_OK) {
            ESP_LOGE(TAG, "Backup not found");
            
            // Error response - backup not found
            cJSON_AddStringToObject(response, "status", "ERROR");
            cJSON_AddStringToObject(response, "message", "Backup not found");
            cJSON_AddStringToObject(response, "error_code", "BACKUP_NOT_FOUND");
            
            char* response_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
            response = NULL;   /* OOM print fell through to a second delete */
            
            if (response_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, HTTPD_404);
                httpd_resp_send(req, response_str, strlen(response_str));
                free(response_str);
            } else {
                httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Backup not found");
            }
            return ESP_FAIL;
        }
        
        // Create backup of current config before restore (safety)
        esp_err_t backup_result = rbgrid_storage_create_backup("/backup/config_before_restore.json");
        if (backup_result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create safety backup before restore");
            // Not critical, continue with restore
        }
        
        // Perform restore
        esp_err_t restore_result = rbgrid_storage_restore_from_backup(NULL);
        
        if (restore_result == ESP_OK) {
            ESP_LOGI(TAG, "Configuration restored successfully from backup");
            
            // Success response
            cJSON_AddStringToObject(response, "status", "OK");
            cJSON_AddStringToObject(response, "message", "Configuration restored successfully");
            cJSON_AddBoolToObject(response, "requires_reboot", true);
            
            char* response_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
            response = NULL;   /* OOM print fell through to a second delete */
            
            if (response_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, response_str, strlen(response_str));
                free(response_str);
                return ESP_OK;
            }
        } else if (restore_result == ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "Invalid JSON in backup file");
            
            // Error response - corrupted backup
            cJSON_AddStringToObject(response, "status", "ERROR");
            cJSON_AddStringToObject(response, "message", "Backup file is corrupted");
            cJSON_AddStringToObject(response, "error_code", "BACKUP_CORRUPTED");
            
            char* response_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
            response = NULL;   /* OOM print fell through to a second delete */
            
            if (response_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, HTTPD_400);
                httpd_resp_send(req, response_str, strlen(response_str));
                free(response_str);
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Backup file is corrupted");
            }
            return ESP_FAIL;
        } else {
            ESP_LOGE(TAG, "Failed to restore configuration: %s", esp_err_to_name(restore_result));
            
            // Error response - restore failed
            cJSON_AddStringToObject(response, "status", "ERROR");
            cJSON_AddStringToObject(response, "message", "Failed to restore configuration");
            cJSON_AddStringToObject(response, "error_code", "RESTORE_FAILED");
            
            char* response_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
            response = NULL;   /* OOM print fell through to a second delete */
            
            if (response_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, HTTPD_500);
                httpd_resp_send(req, response_str, strlen(response_str));
                free(response_str);
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restore configuration");
            }
            return ESP_FAIL;
        }
        
        // Shouldn't reach here
        cJSON_Delete(response);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal error");
        return ESP_FAIL;
    }
    
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}


static esp_err_t rbwebserver_system_reboot_handler(httpd_req_t *req)
{
    add_cors_headers(req);

    if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // reboot is a write: admin token required
        ESP_LOGI(TAG, "POST /api/system/reboot request");
        
        // Allocate buffer for optional JSON data
        char* buf = NULL;
        uint32_t delay_ms = 1000; // Default delay 1 second
        
        if (req->content_len > 0) {
            buf = malloc(512); // Small buffer for reboot request
            if (!buf) {
                ESP_LOGE(TAG, "Failed to allocate memory for JSON buffer");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
                return ESP_FAIL;
            }
            
            int remaining = req->content_len;
            if (remaining > 511) {
                remaining = 511;
            }
            
            int ret = rbapi_recv_body(req, buf, remaining);
            if (ret <= 0) {
                ESP_LOGE(TAG, "Failed to receive data");
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
                return ESP_FAIL;
            }
            buf[ret] = '\0';
            
            // Parse optional JSON to get delay
            cJSON *root = cJSON_Parse(buf);
            free(buf);
            
            if (root != NULL) {
                cJSON* delay_item = cJSON_GetObjectItem(root, "delay_ms");
                if (delay_item && cJSON_IsNumber(delay_item)) {
                    delay_ms = (uint32_t)delay_item->valuedouble;
                    if (delay_ms > 3000) delay_ms = 3000;   /* same clamp rbgrid_system_reboot applies — echo the REAL value */
                    ESP_LOGI(TAG, "Reboot delay from request: %lu ms", (unsigned long)delay_ms);
                }
                cJSON_Delete(root);
            }
        }
        
        // Create response JSON
        cJSON* response = cJSON_CreateObject();
        if (!response) {
            ESP_LOGE(TAG, "Failed to create response JSON");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        // Add response fields
        cJSON_AddStringToObject(response, "status", "OK");
        cJSON_AddStringToObject(response, "message", "System will reboot");
        cJSON_AddNumberToObject(response, "delay_ms", delay_ms);
        
        // Send response
        char* response_str = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        
        if (response_str) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, response_str, strlen(response_str));
            free(response_str);
            
            // Create a task to perform reboot in separate context
            ESP_LOGI(TAG, "Creating reboot task with delay %lu ms", (unsigned long)delay_ms);
            /* Priority raised idle+1 → +6: after a WS client storm the idle+1 task never
             * progressed (API reboot answered 200, hub stayed up, no LWT offline); the orderly path is
             * also guarded by a hard esp_restart fallback inside rbgrid_system_reboot(). */
            BaseType_t ret = xTaskCreate(reboot_task, "reboot_task", 4096, 
                                        (void*)(uintptr_t)delay_ms, 
                                        tskIDLE_PRIORITY + 6, NULL);
            if (ret != pdPASS) {
                /* The 200 is already on the wire — a planned reboot must still happen. Seen after a
                 * WebSocket client storm (internal heap exhausted by socket buffers): task creation
                 * failed silently and the hub stayed up. Degrade gracefully: publish the LWT offline
                 * (cheap), give the response a moment to flush, then hard-restart from here. */
                ESP_LOGE(TAG, "Failed to create reboot task (internal heap %u) — hard restart from the HTTP task",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                extern esp_err_t rbmqtt_publish_offline(void);
                (void)rbmqtt_publish_offline();
                vTaskDelay(pdMS_TO_TICKS(delay_ms > 3000 ? 3000 : delay_ms) + pdMS_TO_TICKS(300));
                { rbpower_mark_planned_restart(); esp_restart(); }
            }
            
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to serialize response JSON");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create response");
            return ESP_FAIL;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

// Time synchronization handler
static esp_err_t rbwebserver_system_time_sync_handler(httpd_req_t *req)
{
    add_cors_headers(req);

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "GET /api/system/time-sync request - returning time configuration");

        // Create temporary hardware_config structure for function call
        rbpower_hardware_config_t hw_config = {0};
        rbpower_err_t err = rbtime_get_config(&hw_config.time_config);

        if (err != RBPOWER_OK) {
            ESP_LOGE(TAG, "Failed to get time config: %d", err);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get time configuration");
            return ESP_FAIL;
        }

        // Check JSON context
        if (!g_json_context) {
            ESP_LOGE(TAG, "JSON context not initialized");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON module not initialized");
            return ESP_FAIL;
        }

        // Create JSON with time configuration and statistics
        char* json_str = rbgrid_json_create_time_config(&hw_config, g_json_context);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to create time config JSON");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create response");
            return ESP_FAIL;
        }

        // Send JSON response
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);

        return ESP_OK;
    }
    else if (req->method == HTTP_POST) {
        if (!rbapi_require_auth(req)) return ESP_FAIL;   // admin token required (if provisioned)
        ESP_LOGI(TAG, "POST /api/system/time-sync request");

        // Read request body (increase buffer for large JSON)
        size_t buf_len = req->content_len + 1;
        if (buf_len > 4096) {  // Maximum size limit
            ESP_LOGE(TAG, "Request too large: %d bytes", req->content_len);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
            return ESP_FAIL;
        }

        char* buf = malloc(buf_len);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for request buffer");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        int total_received = 0;
        int remaining = req->content_len;

        // Read all content
        while (remaining > 0) {
            int to_read = remaining;
            if (to_read > 512) {
                to_read = 512;  // Read in 512 byte chunks
            }

            int ret = httpd_req_recv(req, buf + total_received, to_read);
            if (ret <= 0) {
                ESP_LOGE(TAG, "Failed to receive request data (received %d of %d)",
                         total_received, req->content_len);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
                return ESP_FAIL;
            }

            total_received += ret;
            remaining -= ret;
        }

        buf[total_received] = '\0';

        // Parse JSON request
        cJSON* request = cJSON_Parse(buf);

        if (!request) {
            ESP_LOGE(TAG, "Failed to parse JSON request");
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        free(buf);

        // Get action
        cJSON* action_item = cJSON_GetObjectItem(request, "action");
        if (!action_item || !cJSON_IsString(action_item)) {
            ESP_LOGE(TAG, "Missing or invalid 'action' field");
            cJSON_Delete(request);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'action' field");
            return ESP_FAIL;
        }

        const char* action = action_item->valuestring;
        ESP_LOGI(TAG, "Time sync action: %s", action);

        // Handle action "sync_time"
        if (strcmp(action, "sync_time") == 0) {
            cJSON* data = cJSON_GetObjectItem(request, "data");
            if (!data) {
                ESP_LOGE(TAG, "Missing 'data' field for sync_time");
                cJSON_Delete(request);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'data' field");
                return ESP_FAIL;
            }

            // Get parameters
            cJSON* timestamp_item = cJSON_GetObjectItem(data, "timestamp");
            cJSON* timezone_item = cJSON_GetObjectItem(data, "timezone");
            cJSON* source_item = cJSON_GetObjectItem(data, "source");

            uint64_t previous_time = rbtime_get_unix_timestamp();
            uint64_t new_timestamp = 0;
            const char* timezone = NULL;
            const char* source = "MANUAL_WEB";
            bool sync_success = false;
            const char* error_message = NULL;

            // Check source
            if (source_item && cJSON_IsString(source_item)) {
                source = source_item->valuestring;
            }

            // If source == "MANUAL_WEB" - set time manually then start SNTP
            if (strcmp(source, "MANUAL_WEB") == 0) {
                if (timestamp_item && cJSON_IsNumber(timestamp_item)) {
                    new_timestamp = (uint64_t)timestamp_item->valuedouble;

                    // Validate timestamp
                    if (new_timestamp < 1000000000 || new_timestamp > 2147483647) {
                        error_message = "Invalid timestamp value";
                        ESP_LOGE(TAG, "%s: %llu", error_message, new_timestamp);
                    } else {
                        // Step 1: Set time manually for immediate correction
                        rbpower_err_t err = rbtime_set_manual_time(new_timestamp);
                        if (err == RBPOWER_OK) {
                            sync_success = true;
                            ESP_LOGI(TAG, "Manual time set to: %llu", new_timestamp);

                            // Step 2: Start SNTP sync for fine-tuning
                            ESP_LOGI(TAG, "Starting SNTP sync after manual time set...");
                            rbpower_err_t sntp_err = rbtime_force_sync(RBTIME_SOURCE_SNTP, 5000);
                            if (sntp_err == RBPOWER_OK) {
                                ESP_LOGI(TAG, "SNTP sync started successfully after manual time set");
                                // Get updated time after SNTP
                                new_timestamp = rbtime_get_unix_timestamp();
                            } else if (sntp_err == RBPOWER_ERR_TIMEOUT) {
                                ESP_LOGI(TAG, "SNTP sync continuing in background");
                            } else {
                                ESP_LOGW(TAG, "SNTP sync failed after manual time set: %d", sntp_err);
                                // Not critical - time already set manually
                            }
                        } else {
                            error_message = "Failed to set manual time";
                            ESP_LOGE(TAG, "%s: %d", error_message, err);
                        }
                    }
                } else {
                    error_message = "Missing timestamp for manual sync";
                    ESP_LOGE(TAG, "%s", error_message);
                }
            }
            // If source == "SNTP" - only start SNTP synchronization
            else if (strcmp(source, "SNTP") == 0) {
                ESP_LOGI(TAG, "Direct SNTP sync requested");
                rbpower_err_t err = rbtime_force_sync(RBTIME_SOURCE_SNTP, 10000);
                if (err == RBPOWER_OK || err == RBPOWER_ERR_TIMEOUT) {
                    sync_success = true;
                    new_timestamp = rbtime_get_unix_timestamp();
                    ESP_LOGI(TAG, "SNTP sync initiated");
                } else {
                    error_message = "SNTP sync failed";
                    ESP_LOGE(TAG, "%s: %d", error_message, err);
                }
            }
            else {
                error_message = "Unknown source type";
                ESP_LOGE(TAG, "Unknown source: %s", source);
            }

            // Update timezone if specified
            if (timezone_item && cJSON_IsString(timezone_item)) {
                timezone = timezone_item->valuestring;
                rbpower_err_t tz_err = rbtime_set_timezone(timezone);
                if (tz_err == RBPOWER_OK) {
                    ESP_LOGI(TAG, "Timezone updated to: %s", timezone);
                } else if (tz_err == RBPOWER_ERR_INVALID_ARG) {
                    // invalid POSIX TZ must be a 400, not a silent UTC fallback (e.g. "<UTC+9>" without an offset).
                    cJSON_Delete(request);
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_sendstr(req,
                        "{\"status\":\"error\",\"message\":\"invalid POSIX TZ string (would fall back to UTC) — e.g. CST-8 or CET-1CEST,M3.5.0,M10.5.0/3\"}");
                    return ESP_OK;
                } else {
                    ESP_LOGW(TAG, "Failed to update timezone: %d", tz_err);
                }
            }

            // Form response according to documentation
            cJSON* response = cJSON_CreateObject();
            if (sync_success) {
                cJSON_AddStringToObject(response, "status", "OK");
                cJSON_AddStringToObject(response, "message", "Time synchronized successfully");
                cJSON_AddNumberToObject(response, "timestamp_set", new_timestamp);
                if (timezone) {
                    cJSON_AddStringToObject(response, "timezone_set", timezone);
                }
                cJSON_AddNumberToObject(response, "previous_time", previous_time);
                cJSON_AddNumberToObject(response, "time_difference", (int64_t)(new_timestamp - previous_time));
                cJSON_AddBoolToObject(response, "rtc_updated", false); // RTC not yet supported
            } else {
                cJSON_AddStringToObject(response, "status", "ERROR");
                cJSON_AddStringToObject(response, "error_code", "SYNC_FAILED");
                cJSON_AddStringToObject(response, "message", error_message ? error_message : "Unknown error");
                if (new_timestamp > 0) {
                    char details[128];
                    snprintf(details, sizeof(details), "Timestamp must be between 1000000000 and 2147483647");
                    cJSON_AddStringToObject(response, "details", details);
                }
            }

            cJSON_Delete(request);

            // Send response
            char* json_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);

            if (json_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, json_str);
                free(json_str);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to serialize response");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create response");
                return ESP_FAIL;
            }
        }
        // Handle action "save_config"
        else if (strcmp(action, "save_config") == 0) {
            ESP_LOGI(TAG, "Processing save_config action");

            cJSON* data = cJSON_GetObjectItem(request, "data");
            if (!data) {
                ESP_LOGE(TAG, "Missing 'data' field for save_config");
                cJSON_Delete(request);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'data' field");
                return ESP_FAIL;
            }

            // Create JSON object for saving (config section only)
            cJSON* save_json = cJSON_CreateObject();
            cJSON* config = cJSON_GetObjectItem(data, "config");

            if (!config) {
                ESP_LOGE(TAG, "Missing 'config' field in data");
                cJSON_Delete(request);
                cJSON_Delete(save_json);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'config' field");
                return ESP_FAIL;
            }

            // Copy config to save_json
            cJSON_AddItemToObject(save_json, "config", cJSON_Duplicate(config, true));

            // Convert to string for saving
            char* config_str = cJSON_PrintUnformatted(save_json);
            cJSON_Delete(save_json);

            bool save_success = false;
            bool requires_reboot = false;
            const char* error_message = NULL;

            {   /* A TZ without an offset ("<UTC+9>") would run the device in UTC —
                 * reject the request (400) BEFORE anything is saved; the file and runtime stay as they are. */
                cJSON* tzj = cJSON_GetObjectItem(config, "timezone");
                if (tzj && cJSON_IsString(tzj) && tzj->valuestring[0] && !rbtime_timezone_is_valid(tzj->valuestring)) {
                    ESP_LOGE(TAG, "time-sync save_config: timezone '%s' rejected (not a POSIX TZ with an offset)", tzj->valuestring);
                    if (config_str) free(config_str);
                    cJSON_Delete(request);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Invalid timezone: use a POSIX TZ with an offset, e.g. \"<UTC+9>-9\", \"CST-8\", \"CET-1CEST,M3.5.0,M10.5.0/3\"");
                    return ESP_FAIL;
                }
            }
            if (config_str) {
                // Save configuration to Flash
                esp_err_t save_err = rbgrid_storage_save_time(config_str);

                if (save_err == ESP_OK) {
                    save_success = true;
                    ESP_LOGI(TAG, "✅ Time configuration saved to Flash");

                    // Optional: apply some settings immediately without reboot
                    // For example, timezone can be updated immediately
                    cJSON* timezone = cJSON_GetObjectItem(config, "timezone");
                    if (timezone && cJSON_IsString(timezone)) {
                        rbtime_set_timezone(timezone->valuestring);
                        ESP_LOGI(TAG, "Timezone updated immediately to: %s", timezone->valuestring);
                    }

                    // Full application of all settings requires reboot
                    cJSON* enable_time_sync = cJSON_GetObjectItem(config, "enable_time_sync");
                    cJSON* primary_source = cJSON_GetObjectItem(config, "primary_source");
                    cJSON* sntp_server = cJSON_GetObjectItem(config, "sntp_server");

                    // If critical parameters changed - reboot required
                    if (enable_time_sync || primary_source || sntp_server) {
                        requires_reboot = true;
                        ESP_LOGI(TAG, "Critical time settings changed, reboot required");
                    }
                } else {
                    error_message = "Failed to save configuration to Flash";
                    ESP_LOGE(TAG, "%s: %s", error_message, esp_err_to_name(save_err));
                }

                free(config_str);
            } else {
                error_message = "Failed to create configuration JSON";
                ESP_LOGE(TAG, "%s", error_message);
            }

            cJSON_Delete(request);

            // Form response according to documentation
            cJSON* response = cJSON_CreateObject();
            if (save_success) {
                cJSON_AddStringToObject(response, "status", "OK");
                cJSON_AddStringToObject(response, "message", "Configuration saved successfully");
                cJSON_AddBoolToObject(response, "requires_reboot", requires_reboot);

                if (requires_reboot) {
                    ESP_LOGI(TAG, "Configuration saved. Reboot required to apply all changes.");
                } else {
                    ESP_LOGI(TAG, "Configuration saved and partially applied.");
                }
            } else {
                cJSON_AddStringToObject(response, "status", "ERROR");
                cJSON_AddStringToObject(response, "error_code", "SAVE_FAILED");
                cJSON_AddStringToObject(response, "message", error_message ? error_message : "Unknown error");
            }

            // Send response
            char* json_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);

            if (json_str) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, json_str);
                free(json_str);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to serialize response");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create response");
                return ESP_FAIL;
            }
        }
        else {
            ESP_LOGE(TAG, "Unknown action: %s", action);
            cJSON_Delete(request);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
            return ESP_FAIL;
        }
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

// Latest telemetry data handler
static esp_err_t rbwebserver_telemetry_latest_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/telemetry/latest request");
    add_cors_headers(req);
    /* Read gate: this used to be the one data read WITHOUT the gate —
     * armed read-token installs leaked live telemetry. No-op until a read token exists. */
    if (!rbapi_require_read(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");

    /* Real snapshot — the SAME `measurement_data` shape the WS/MQTT push every 3 s, so a non-WS
     * client (or the dashboard's initial render before the socket connects) gets live data in one
     * GET. Lock g_home_network vs the core-0/esp_timer mutators (recursive mutex). */
    extern char* rbgrid_json_create_telemetry_frame(uint8_t);
    extern uint8_t rbgrid_telemetry_get_frame(void);
    uint8_t frame = rbgrid_telemetry_get_frame();
    {   /* per-request ?v=2 — independent of the global telemetry-config.frame */
        char qs[64];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
            char vv[4];
            if (httpd_query_key_value(qs, "v", vv, sizeof(vv)) == ESP_OK) frame = (vv[0] == '2') ? 2 : 1;
        }
    }
    rbpower_home_lock();   /* the frame must be built under the lock (consistent snapshot) */
    char* json = rbgrid_json_create_telemetry_frame(frame);
    rbpower_home_unlock();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "telemetry snapshot build failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// ====== NEW API HANDLERS ======

// API system-config handler
// Handler for STA clients - shows network switching instructions
static esp_err_t rbwebserver_sta_home_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "STA home page handler");
    add_cors_headers(req);

    // Get STA IP for WEB-APP link
    char sta_ip[16] = "0.0.0.0";
    bool sta_connected = false;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            sta_connected = true;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip_info.ip));
                }
            }
        }
    }

    httpd_resp_set_type(req, "text/html");

    const char* home_html_part1 =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "    <meta charset=\"UTF-8\">"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "    <title>RBGRID - Home</title>"
    "    <style>"
    "        * { margin: 0; padding: 0; box-sizing: border-box; }"
    "        body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding-top: 60px; }"
    "        .nav { background: #2c3e50; color: white; padding: 15px; position: fixed; top: 0; left: 0; right: 0; z-index: 1000; }"
    "        .nav-container { max-width: 800px; margin: 0 auto; display: flex; justify-content: space-between; align-items: center; }"
    "        .nav-brand { font-size: 20px; font-weight: bold; }"
    "        .nav-links { display: flex; gap: 15px; }"
    "        .nav-links a { color: white; text-decoration: none; padding: 8px 15px; border-radius: 4px; }"
    "        .nav-links a:hover { background: #34495e; }"
    "        .nav-links a.active { background: #3498db; }"
    "        .nav-links a.disabled { color: #7f8c8d; cursor: not-allowed; pointer-events: none; opacity: 0.5; }"
    "        .content { display: flex; align-items: center; justify-content: center; min-height: calc(100vh - 60px); }"
    "        .card { background: white; max-width: 600px; margin: 20px; padding: 40px; border-radius: 15px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }"
    "        h1 { color: #333; margin-bottom: 10px; font-size: 28px; }"
    "        .subtitle { color: #666; margin-bottom: 30px; font-size: 16px; }"
    "        .info-box { background: #e3f2fd; border-left: 4px solid #2196F3; padding: 20px; margin-bottom: 25px; border-radius: 4px; }"
    "        .info-box h2 { color: #1976D2; margin-bottom: 15px; font-size: 20px; }"
    "        .info-box p { color: #333; line-height: 1.6; margin-bottom: 10px; }"
    "        .network-name { background: #f5f5f5; padding: 8px 12px; border-radius: 4px; font-family: monospace; display: inline-block; margin: 5px 0; }"
    "        .btn-container { display: flex; gap: 15px; margin-top: 25px; }"
    "        .btn { flex: 1; padding: 15px; border: none; border-radius: 8px; cursor: pointer; font-size: 16px; font-weight: bold; text-decoration: none; text-align: center; transition: all 0.3s; }"
    "        .btn-primary { background: #4CAF50; color: white; }"
    "        .btn-primary:hover { background: #45a049; transform: translateY(-2px); box-shadow: 0 5px 15px rgba(76,175,80,0.3); }"
    "        .btn-secondary { background: #2196F3; color: white; }"
    "        .btn-secondary:hover { background: #1976D2; transform: translateY(-2px); box-shadow: 0 5px 15px rgba(33,150,243,0.3); }"
    "        .btn-outline { background: transparent; color: #666; border: 2px solid #ddd; }"
    "        .btn-outline:hover { background: #f5f5f5; border-color: #999; }"
    "        .status-dot { width: 12px; height: 12px; border-radius: 50%; display: inline-block; margin-right: 8px; }"
    "        .connected { background: #4CAF50; }"
    "        .disconnected { background: #f44336; }"
    "    </style>"
    "</head>"
    "<body>"
    "    <div class=\"nav\">"
    "        <div class=\"nav-container\">"
    "            <div class=\"nav-brand\">RBGRID Control</div>"
    "            <div class=\"nav-links\">"
    "                <a href=\"/\" class=\"active\">Home</a>"
    "                <a href=\"/wifi\">WiFi</a>";

    // Send first part
    httpd_resp_sendstr_chunk(req, home_html_part1);

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

    const char* home_html_part2 =
    "            </div>"
    "        </div>"
    "    </div>"
    "    <div class=\"content\">"
    "        <div class=\"card\">"
    "            <h1>RBGRID Control Panel</h1>"
    "            <p class=\"subtitle\">ESP32 Power Monitor & Control System</p>"
    "            <div class=\"info-box\">"
    "                <h2>Welcome!</h2>"
    "                <p>You are connected via the device's WiFi network:</p>"
    "                <div class=\"network-name\"><span class=\"status-dot connected\"></span>rbgrid-xxxx (this hub)</div>"
    "                <p style=\"margin-top: 15px;\">To access the full web application:</p>"
    "                <ol style=\"margin-left: 20px; color: #333; line-height: 1.8;\">"
    "                    <li>Disconnect from the hub's network <strong>rbgrid-xxxx</strong></li>"
    "                    <li>Connect to your <strong>home WiFi network</strong></li>"
    "                    <li>Open the web application in your browser</li>"
    "                </ol>"
    "                <p style=\"margin-top: 15px; color: #666; font-size: 14px;\">💡 The device must be configured to connect to your home WiFi first. Use the WiFi Settings page to configure the connection.</p>"
    "            </div>"
    "            <div style=\"margin-top: 25px;\">"
    "                <a href=\"/wifi\" class=\"btn btn-primary\" style=\"display: block;\">WiFi Settings</a>"
    "            </div>"
    "        </div>"
    "    </div>"
    "</body>"
    "</html>";

    // Send rest of page
    httpd_resp_sendstr_chunk(req, home_html_part2);

    // End chunked response
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

static esp_err_t universal_root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Universal root handler: %s", req->uri);

    // Determine request source
    if (is_request_from_ap_network(req)) {
        // Request from AP network - redirect to WiFi configuration page
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        httpd_resp_send(req, NULL, 0);
    } else {
        // STA network: serve the installed embedded UI; otherwise the firmware stub page
        // ("UI not installed → upload the package / open the cloud app").
        if (rbwebserver_ui_installed()) {
            return rbwebserver_ui_index_handler(req);
        }
        return rbwebserver_sta_home_handler(req);
    }

    return ESP_OK;
}

/**
 * @brief Re-register the URI handlers for the current Wi-Fi mode by restarting the server.
 *
 * No-op when the server is not running. Blocks ~100 ms; call from a task, never from an httpd handler.
 *
 * @retval RBPOWER_OK  Server restarted (or not running).
 * @return Otherwise the error of rbwebserver_start().
 */
rbpower_err_t rbwebserver_update_wifi_mode(void)
{
    if (!g_running) {
        return RBPOWER_OK;
    }

    ESP_LOGI(TAG, "Updating web server handlers for new WiFi mode");

    // Stop server
    rbwebserver_stop();

    // Small delay
    vTaskDelay(pdMS_TO_TICKS(100));

    // Restart server
    return rbwebserver_start();
}

// ========== HISTORY API HANDLER ==========

/* The history handler (GET /api/v1/history) lives in rbwebserver_api.c. It parses the query and calls
 * rbgrid_history_get_data(). Query parameters:
 * - unit_id: hex string (e.g. "0004")
 * - period: string (hour, day, week, month, year)
 * - type: string (electrical, energy, economics, all)
 * - offset: number (default 0)
 * - limit: number (default 100, max 720)
 */
/* DELETE /api/v1/units/{hexid} (rbwebserver_api.c) — remove a unit. Stops its compute,
 * reclaims+ERASES its flash slot, persists units.json without it. Cascade = UNIT-ONLY: the bound
 * sensor's config is untouched (it reappears in the sensor-list with unit_id=null, re-bindable).
 * 200 {status,removed,units_remaining}; 404 if unknown; 409 if it has children (delete them first). */
