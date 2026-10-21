/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver.h
 * @brief Web server of the RBgrid hub: HTTP/HTTPS server lifecycle, REST/config API, Wi-Fi setup
 *        portal, WebSocket telemetry and the MQTT/HTTP command dispatcher.
 *
 * The server runs on the ESP-IDF httpd task (plain HTTP on port 80 by default, or TLS on 443 when
 * the persisted HTTPS flag is set). All URI handlers run on that single httpd task. Write endpoints
 * are guarded by the admin API token once one is provisioned; data reads by the read token once one
 * is provisioned (see rbwebserver_api_check_auth() / rbwebserver_api_check_read()).
 */

#ifndef RBWEBSERVER_H
#define RBWEBSERVER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbhome_types.h"
#include "rbjson.h"
#include "esp_http_server.h"
#include "cJSON.h"

    /**
     * @brief Web server configuration
     */
    typedef struct
    {
        uint16_t port;                              ///< HTTP server port (HTTPS always uses 443)
        uint16_t max_open_sockets;                  ///< Maximum open sockets
        uint16_t max_uri_handlers;                  ///< Maximum URI handlers (informational; the server uses a fixed 128)
        uint16_t max_resp_headers;                  ///< Maximum response headers (informational)
        uint32_t recv_wait_timeout;                 ///< Receive wait timeout, s (informational)
        uint32_t send_wait_timeout;                 ///< Send wait timeout, s (informational)
        bool enable_websocket;                      ///< Enable WebSocket support (informational; `/ws-telemetry` is always registered)
        const char *ws_path;                        ///< Path for WebSocket (informational)
        rbpower_extended_config_t *extended_config; ///< Extended configuration used by the config handlers (not owned)
        rbgrid_json_context_t *json_context;        ///< JSON module context (not owned)
    } rbwebserver_config_t;

    /**
     * @brief Redirect settings
     */
    typedef struct
    {
        const char *frontend_host; ///< Frontend host (host name or IP address)
        uint16_t frontend_port;    ///< Frontend port (e.g., 3000)
        bool enable_root_redirect; ///< Enable root redirect (stored only; the current router does not redirect)
    } rbwebserver_redirect_config_t;

/**
 * @brief Default web server configuration
 */
#define RBWEBSERVER_DEFAULT_CONFIG() { \
    .port = 80,                        \
    .max_open_sockets = 12,            \
    .max_uri_handlers = 16,            \
    .max_resp_headers = 8,             \
    .recv_wait_timeout = 5,            \
    .send_wait_timeout = 5,            \
    .enable_websocket = true,          \
    .ws_path = "/ws",                  \
    .extended_config = NULL,           \
    .json_context = NULL}

/**
 * @brief Default redirect configuration (no frontend host; the values are stored only).
 */
#define RBWEBSERVER_DEFAULT_REDIRECT_CONFIG() { \
    .frontend_host = "",                        \
    .frontend_port = 3000,                      \
    .enable_root_redirect = true}

    /**
     * @brief Initialize web server (stores the port, socket limit and context pointers; idempotent).
     *
     * @param[in] config  Server configuration (the context pointers are kept, not copied).
     * @retval RBPOWER_OK               Initialized (or already initialized).
     * @retval RBPOWER_ERR_INVALID_ARG  @p config is NULL.
     */
    rbpower_err_t rbwebserver_init(const rbwebserver_config_t *config);

    /**
     * @brief Configure redirects (the structure is copied; @c frontend_host must stay valid).
     *
     * @param[in] redirect_config  Redirect configuration.
     * @retval RBPOWER_OK               Stored.
     * @retval RBPOWER_ERR_NOT_INIT     rbwebserver_init() not called.
     * @retval RBPOWER_ERR_INVALID_ARG  @p redirect_config is NULL.
     */
    rbpower_err_t rbwebserver_configure_redirects(const rbwebserver_redirect_config_t *redirect_config);

    /**
     * @brief Start the web server (HTTP or HTTPS per the persisted flag) and register all URI handlers
     *        for the current Wi-Fi mode.
     *
     * Call from a task after Wi-Fi is up; rbwebserver_load_https_flag() must have run first.
     *
     * @retval RBPOWER_OK                    Started (or already running).
     * @retval RBPOWER_ERR_NOT_INIT          rbwebserver_init() not called.
     * @retval RBPOWER_ERR_HTTP_INIT_FAILED  httpd_start() / httpd_ssl_start() failed.
     */
    rbpower_err_t rbwebserver_start(void);

    /**
     * @brief Stop the web server and free the TLS certificate buffers.
     *
     * @retval RBPOWER_OK                    Stopped (or was not running).
     * @retval RBPOWER_ERR_HTTP_INIT_FAILED  httpd_stop() / httpd_ssl_stop() failed.
     */
    rbpower_err_t rbwebserver_stop(void);

    /* HTTPS/TLS. The mode is a persisted flag read at boot; toggling needs a reboot.
     * set_https persists the choice; load_https_flag must run before rbwebserver_start at boot. */

    /**
     * @brief Persist the desired server mode (HTTPS on port 443 or HTTP); effective at the next boot.
     * @param[in] enabled  true → HTTPS (littlefs flag /https_enabled written), false → HTTP (flag removed).
     */
    void rbwebserver_set_https(bool enabled);

    /**
     * @brief Desired (persisted) server mode.
     * @return true if HTTPS is requested.
     */
    bool rbwebserver_get_https(void);

    /**
     * @brief Mode the running server actually started in.
     * @return true if the running server uses TLS.
     */
    bool rbwebserver_https_active(void);

    /**
     * @brief The running httpd handle.
     *
     * Lets the application (main/) register additional URI handlers (e.g. the ESP-NOW management API,
     * which lives in main/ above this component).
     *
     * @return Server handle, or NULL until rbwebserver_start().
     */
    httpd_handle_t rbwebserver_core_server(void);

    /**
     * @brief MD5 of the running firmware image exactly as built (= md5sum of the application .bin).
     *
     * Identifies the exact build remotely (version/build date may be identical between builds).
     * Computed once on first call (~1 s of flash reads), then cached.
     *
     * @return Pointer to a static 32-hex-digit string, or "" if it could not be computed.
     */
    const char* rbwebserver_running_image_md5(void);

    /**
     * @brief Token-on-writes guard for application-registered handlers (wrapper over the internal
     *        rbapi_require_auth()).
     *
     * Passes everything while no admin token is provisioned; OPTIONS always passes.
     *
     * @param[in] req  HTTP request.
     * @return true if authorized; false if a 401/403/429 response has ALREADY been sent.
     */
    bool rbwebserver_api_check_auth(httpd_req_t *req);

    /**
     * @brief READ-role gate for data reads and the WebSocket handshake (wrapper over rbapi_require_read()).
     *
     * Passes everything while no read token is provisioned.
     *
     * @param[in] req  HTTP request.
     * @return true if authorized; false if a 401/429 response has already been sent.
     */
    bool rbwebserver_api_check_read(httpd_req_t *req);

    /**
     * @brief Register the ESP-NOW status-JSON builder of the application (main/ owns the ESP-NOW hub
     *        above this component) so the MQTT `cmd get espnow` path can serve it.
     * @param[in] fn  Builder returning a malloc'd JSON string (caller frees), or NULL to unregister.
     */
    void rbgrid_register_espnow_status_provider(char *(*fn)(void));

    /**
     * @brief Read the persisted HTTPS flag (/https_enabled in littlefs) into RAM. Call at boot before
     *        rbwebserver_start().
     */
    void rbwebserver_load_https_flag(void);

    /**
     * @brief GET/POST /api/system/system-config and /api/system/config — system configuration.
     *        GET: no auth; POST: admin token required (validated, saved, applied live).
     *
     * @param[in] req  HTTP request.
     * @return ESP_OK on success; ESP_FAIL after an error response (400/401/403/405/429/500).
     */
    esp_err_t rbwebserver_system_config_handler(httpd_req_t *req);

    /**
     * @brief Set global context for JSON generation (replaces the pointers given to rbwebserver_init()).
     *
     * @param[in] extended_config  Extended rbpower configuration (not owned).
     * @param[in] json_context     JSON module context (not owned).
     * @retval RBPOWER_OK            Set.
     * @retval RBPOWER_ERR_NOT_INIT  rbwebserver_init() not called.
     */
    rbpower_err_t rbwebserver_set_global_context(rbpower_extended_config_t *extended_config,
                                                 rbgrid_json_context_t *json_context);

    /**
     * @brief Callback for WebSocket messages
     */
    typedef void (*rbwebserver_ws_message_callback_t)(const char *message, size_t length, void *user_data);

    /**
     * @brief GET /wifi — Wi-Fi setup page (HTML, chunked). No auth.
     *
     * @param[in] req  HTTP request.
     * @return esp_err_t ESP error code.
     */
    esp_err_t rbwebserver_wifi_config_handler(httpd_req_t *req);

    /**
     * @brief POST /wifi/save {"ssid":"…","password":"…"} — save the station credentials to NVS and
     *        restart. Admin token required (when provisioned).
     *
     * @param[in] req  HTTP request (JSON body, max 255 bytes).
     * @return esp_err_t ESP error code.
     */
    esp_err_t rbwebserver_wifi_save_handler(httpd_req_t *req);

    /**
     * @brief GET /wifi/status — STA/AP status as JSON. No auth.
     *
     * @param[in] req  HTTP request.
     * @return esp_err_t ESP error code.
     */
    esp_err_t rbwebserver_wifi_status_handler(httpd_req_t *req);

    /**
     * @brief GET /wifi/scan — start a non-blocking network scan (idempotent); answers 202
     *        {data:{status:"scanning"}}. No auth.
     *
     * @param[in] req  HTTP request.
     * @return esp_err_t ESP error code.
     */
    esp_err_t rbwebserver_wifi_scan_handler(httpd_req_t *req);

    /**
     * @brief GET /wifi/scan/results — poll the async scan started by rbwebserver_wifi_scan_handler().
     *        Answers 200 {data:{status:"scanning"|"done"|"error"[,count,networks]}}. No auth.
     *
     * @param[in] req  HTTP request.
     * @return esp_err_t ESP error code.
     */
    esp_err_t rbwebserver_wifi_scan_results_handler(httpd_req_t *req);

    /**
     * @brief Unified command dispatcher — maps a command JSON to a response JSON, reusing the
     *        same config builders/parsers as the HTTP handlers. Used by the MQTT command channel
     *        (rbgrid/\<mac\>/cmd) so configuration is fully manageable over MQTT as well as HTTP.
     *
     * Commands: `get` (sections system, telemetry, wifi, units, tariff, adc_sensors, telemetry_data,
     * schema, sensors, espnow, economics, summary), `set` (sections units, tariff — requires the admin
     * token in the envelope once one is provisioned) and `history`. The request `id` is echoed as a
     * top-level field of the response. Runs on the caller's task (the MQTT task); takes the
     * home-network lock where needed.
     *
     * @param[in] in_json  Command JSON (not necessarily NUL-terminated), e.g. {"cmd":"get","section":"system"}.
     * @param[in] in_len   Length of @p in_json, bytes.
     * @return malloc'd response JSON (caller frees), or NULL on hard failure.
     */
    char* rbgrid_command_dispatch(const char* in_json, int in_len);

    /**
     * @brief GET /, /generate_204, /hotspot-detect.html (AP side) — 302 redirect to /wifi (captive portal). No auth.
     *
     * @param[in] req  HTTP request.
     * @return ESP_OK always.
     */
    esp_err_t rbwebserver_ap_root_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // RBWEBSERVER_H