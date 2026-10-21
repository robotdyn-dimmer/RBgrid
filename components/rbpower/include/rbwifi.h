/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwifi.h
 * @brief Wi-Fi station/access-point management for the rbpower library.
 *
 * Provides functions for initializing and managing the Wi-Fi connection (STA, AP or AP+STA),
 * storing STA credentials in NVS, and a minimal standalone HTTP server helper. The full web
 * UI/API server lives in rbwebserver.h; the WebSocket helpers in this file are stubs (the live
 * telemetry stream is rbwebsocket_telemetry.h).
 *
 * STA behaviour: on boot the station retries up to rbwifi_config_t::max_retry times, then
 * signals failure (the caller falls back to AP mode) while continuing to retry in the
 * background. After the first successful connection a later drop is retried indefinitely.
 *
 * Unless noted otherwise, functions are meant to be called from task context and are not
 * thread-safe against each other; the Wi-Fi event handler runs in the default event-loop task.
 *
 * @version 1.0.0
 * @date 2025-07-23
 * @author RobotBroker
 */

#ifndef RBWIFI_H
#define RBWIFI_H

#ifdef __cplusplus
extern "C"
{
#endif

/* ================================================================================================
 * INCLUDES
 * ================================================================================================ */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"

#include "rbpower_enums.h"
//#include "rbpower_types.h"

/* ================================================================================================
 * CONSTANTS
 * ================================================================================================ */

#define RBWIFI_MAX_SSID_LEN     32 ///< SSID buffer size, bytes
#define RBWIFI_MAX_PASSWORD_LEN 64 ///< Password buffer size, bytes
#define RBWIFI_MAX_WS_CLIENTS   4  ///< Size of the (unused) WebSocket client table
#define RBWIFI_DEFAULT_RETRY    5  ///< Default number of initial STA connection attempts
#define RBWIFI_DEFAULT_PORT     80 ///< Default HTTP server port

/* ================================================================================================
 * TYPES AND STRUCTURES
 * ================================================================================================ */

/**
 * @brief WiFi connection status
 */
typedef enum {
    RBWIFI_STATUS_NOT_INITIALIZED = 0, ///< rbwifi_init() not called yet
    RBWIFI_STATUS_DISCONNECTED,        ///< Initialized, STA not connected
    RBWIFI_STATUS_CONNECTING,          ///< STA connection in progress
    RBWIFI_STATUS_CONNECTED,           ///< STA connected and has an IP address
    RBWIFI_STATUS_FAILED               ///< Initial STA connection attempts exhausted (background retry continues)
} rbwifi_status_t;

/**
 * @brief WiFi configuration
 */
typedef struct {
    // Station mode config
    char ssid[RBWIFI_MAX_SSID_LEN];         ///< SSID of access point to connect to (empty → load from NVS)
    char password[RBWIFI_MAX_PASSWORD_LEN]; ///< Password of access point to connect to
    uint8_t max_retry;                      ///< Maximum initial connection attempts before reporting failure
    bool enable_sta;                        ///< Enable station mode (STA)

    // Access Point mode config
    bool enable_ap;                         ///< Enable access point mode (AP)
    char ap_ssid[RBWIFI_MAX_SSID_LEN];     ///< SSID of own access point
    char ap_password[RBWIFI_MAX_PASSWORD_LEN]; ///< Password of own access point (empty → open AP)
    uint8_t ap_channel;                     ///< AP channel (1-13)
    uint8_t ap_max_connections;             ///< Maximum connections to AP
} rbwifi_config_t;

/**
 * @brief WiFi connection information
 */
typedef struct {
    rbwifi_status_t status;     ///< Current connection status
    wifi_ap_record_t ap_info;   ///< Information about connected AP (valid when connected)
    esp_netif_ip_info_t ip_info; ///< STA IP information (valid when connected)
    int8_t rssi;                ///< Signal level, dBm
    uint8_t channel;            ///< Wi-Fi channel
} rbwifi_info_t;

/**
 * @brief Web server configuration
 */
typedef struct {
    uint16_t port;          ///< HTTP server port
    bool enable_websocket;  ///< Register the WebSocket path (stub handler answers 501)
    const char* ws_path;    ///< Path for WebSocket (e.g. "/ws")
} rbwifi_server_config_t;

/**
 * @brief WebSocket client
 */
typedef struct {
    httpd_handle_t server; ///< HTTP server handle
    int fd;                ///< Client socket descriptor
    bool connected;        ///< Slot in use
} rbwifi_ws_client_t;

/**
 * @brief Callback for Wi-Fi events; called from the default event-loop task after every handled
 *        Wi-Fi/IP event with the current status.
 */
typedef void (*rbwifi_event_callback_t)(rbwifi_status_t status, void* user_data);

/**
 * @brief Callback for WebSocket messages
 */
typedef void (*rbwifi_ws_message_callback_t)(const char* message, size_t length, void* user_data);

/* ================================================================================================
 * DEFAULT CONFIGURATION MACROS
 * ================================================================================================ */

/* Access-point setup password: a provisioning value, kept out of the source tree.
 * ap_provision.h is git-ignored; ap_provision.example.h is the template (see it for the rules).
 * The setup password applies until the user sets their own via POST /api/system/ap; the hub
 * never generates or changes it by itself. */
#if __has_include("ap_provision.h")
#  include "ap_provision.h"
#else
#  include "ap_provision.example.h"
#  if !defined(RBGRID_AP_PROVISION_WARNED)
#    define RBGRID_AP_PROVISION_WARNED 1 ///< Guard so the missing-ap_provision.h warning is emitted once per translation unit
#    warning "ap_provision.h not found: the AP setup password is the public template value — create components/rbpower/include/ap_provision.h for real devices"
#  endif
#endif
#ifndef RBGRID_AP_SETUP_PASSWORD
#  error "RBGRID_AP_SETUP_PASSWORD is not defined (ap_provision.h)"
#endif

/**
 * @brief Default rbwifi_config_t initializer: STA enabled, AP "RBGRID-AP" with the provisioned
 *        setup password (RBGRID_AP_SETUP_PASSWORD), channel 1, up to 4 stations.
 */
#define RBWIFI_DEFAULT_CONFIG() { \
    .ssid = "",                   \
    .password = "",               \
    .max_retry = RBWIFI_DEFAULT_RETRY, \
    .enable_sta = true,           \
    .enable_ap = false,           \
    .ap_ssid = "RBGRID-AP",       \
    .ap_password = RBGRID_AP_SETUP_PASSWORD, \
    .ap_channel = 1,              \
    .ap_max_connections = 4       \
}

/** @brief Default rbwifi_server_config_t initializer: port 80, WebSocket path "/ws". */
#define RBWIFI_DEFAULT_SERVER_CONFIG() { \
    .port = RBWIFI_DEFAULT_PORT,         \
    .enable_websocket = true,            \
    .ws_path = "/ws"                     \
}

/* ================================================================================================
 * API FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Initialize Wi-Fi module: NVS, event group, netif(s), esp_wifi and event handlers.
 *
 * Aborts (ESP_ERROR_CHECK) if the TCP/IP stack, the default event loop or the event handlers
 * cannot be set up.
 *
 * @param[in] config  Wi-Fi configuration (copied).
 * @retval RBPOWER_OK                   Initialized.
 * @retval RBPOWER_ERR_INVALID_ARG      @p config is NULL.
 * @retval RBPOWER_ERR_INVALID_STATE    Already initialized.
 * @retval RBPOWER_ERR_NVS_INIT_FAILED  NVS could not be initialized.
 * @retval RBPOWER_ERR_NO_MEMORY        Event group could not be created.
 * @retval RBPOWER_ERR_WIFI_INIT_FAILED esp_wifi_init() failed.
 */
rbpower_err_t rbwifi_init(const rbwifi_config_t* config);

/**
 * @brief Deinitialize Wi-Fi module (stops Wi-Fi and the helper server, clears the context).
 *
 * @retval RBPOWER_OK            Deinitialized.
 * @retval RBPOWER_ERR_NOT_INIT  Module not initialized.
 */
rbpower_err_t rbwifi_deinit(void);

/**
 * @brief Configure the Wi-Fi mode (STA / AP / AP+STA) and start Wi-Fi.
 *
 * STA credentials come from the configuration, or from NVS when the configured SSID is empty.
 * In STA-only mode with an SSID the call blocks up to 30 s waiting for the connection.
 *
 * @retval RBPOWER_OK                      Started (and connected, in STA-only mode).
 * @retval RBPOWER_ERR_NOT_INIT            Module not initialized.
 * @retval RBPOWER_ERR_INVALID_STATE       Neither STA nor AP enabled.
 * @retval RBPOWER_ERR_WIFI_INIT_FAILED    Setting the mode/STA config or starting Wi-Fi failed.
 * @retval RBPOWER_ERR_WIFI_AP_INIT_FAILED Setting the AP config failed.
 * @retval RBPOWER_ERR_TIMEOUT             STA-only: initial connection attempts exhausted.
 */
rbpower_err_t rbwifi_start(void);

/**
 * @brief Stop Wi-Fi.
 *
 * @retval RBPOWER_OK                       Stopped.
 * @retval RBPOWER_ERR_WIFI_NOT_INITIALIZED Module not initialized.
 * @retval RBPOWER_ERR_WIFI_INIT_FAILED     esp_wifi_stop() failed.
 */
rbpower_err_t rbwifi_stop(void);

/**
 * @brief Get current Wi-Fi status.
 *
 * @param[out] info  Structure to fill; AP record, RSSI, channel and IP are filled only when connected.
 * @retval RBPOWER_OK                       Filled.
 * @retval RBPOWER_ERR_INVALID_ARG          @p info is NULL.
 * @retval RBPOWER_ERR_WIFI_NOT_INITIALIZED Module not initialized.
 */
rbpower_err_t rbwifi_get_status(rbwifi_info_t* info);

/**
 * @brief Register callback for Wi-Fi events (replaces any previous one).
 *
 * @param[in] callback   Callback function (NULL to unregister).
 * @param[in] user_data  User data passed back to the callback.
 * @retval RBPOWER_OK                       Registered.
 * @retval RBPOWER_ERR_WIFI_NOT_INITIALIZED Module not initialized.
 */
rbpower_err_t rbwifi_register_event_callback(rbwifi_event_callback_t callback, void* user_data);

/* ================================================================================================
 * WEB SERVER FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Initialize and start the helper HTTP server (not the main web UI server, see rbwebserver.h).
 *
 * @param[in] config  Server configuration.
 * @retval RBPOWER_OK                    Started.
 * @retval RBPOWER_ERR_INVALID_ARG       @p config is NULL.
 * @retval RBPOWER_ERR_NOT_STARTED       Module not initialized, or the server is already running.
 * @retval RBPOWER_ERR_HTTP_INIT_FAILED  httpd_start() failed.
 */
rbpower_err_t rbwifi_server_start(const rbwifi_server_config_t* config);

/**
 * @brief Stop the helper HTTP server.
 *
 * @retval RBPOWER_OK                       Stopped (or was not running).
 * @retval RBPOWER_ERR_HTTP_NOT_INITIALIZED Module not initialized.
 */
rbpower_err_t rbwifi_server_stop(void);

/**
 * @brief Register a URI handler on the helper HTTP server.
 *
 * @param[in] uri  Structure with URI description.
 * @retval RBPOWER_OK                          Registered.
 * @retval RBPOWER_ERR_INVALID_ARG             @p uri is NULL.
 * @retval RBPOWER_ERR_HTTP_NOT_INITIALIZED    Module not initialized or server not running.
 * @retval RBPOWER_ERR_HTTP_URI_REGISTER_FAILED httpd_register_uri_handler() failed.
 */
rbpower_err_t rbwifi_server_register_uri_handler(const httpd_uri_t* uri);

/* ================================================================================================
 * WEBSOCKET FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Send message to all connected WebSocket clients (not implemented: always fails).
 *
 * @param[in] message  Message to send (JSON string).
 * @param[in] length   Message length, bytes.
 * @retval RBPOWER_ERR_INVALID_ARG           @p message is NULL or @p length is 0.
 * @retval RBPOWER_ERR_WIFI_NOT_INITIALIZED  Module not initialized or server not running.
 * @retval RBPOWER_ERR_WEBSOCKET_INIT_FAILED Otherwise (stub).
 */
rbpower_err_t rbwifi_ws_send_to_all_clients(const char* message, size_t length);

/**
 * @brief Register callback for incoming WebSocket messages (stored only; never invoked by the stub server).
 *
 * @param[in] callback   Callback function.
 * @param[in] user_data  User data passed back to the callback.
 * @retval RBPOWER_OK                     Registered.
 * @retval RBPOWER_ERR_WIFI_CONNECT_FAILED Module not initialized.
 */
rbpower_err_t rbwifi_ws_register_message_callback(rbwifi_ws_message_callback_t callback, void* user_data);

/**
 * @brief Get number of connected WebSocket clients of the helper server.
 *
 * @return Number of connected clients (0 with the stub implementation).
 */
uint8_t rbwifi_ws_get_client_count(void);

/* ================================================================================================
 * HELPER FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Check if Wi-Fi module is initialized
 *
 * @return true if initialized
 */
bool rbwifi_is_initialized(void);

/**
 * @brief Check if the Wi-Fi station is connected (has an IP address).
 *
 * @return true if connected
 */
bool rbwifi_is_connected(void);

/**
 * @brief Check if the helper HTTP server is running
 *
 * @return true if running
 */
bool rbwifi_server_is_running(void);

/**
 * @brief Get string representation of Wi-Fi status.
 *
 * @param[in] status  Wi-Fi status.
 * @return Static string with the status description ("Unknown" for out-of-range values).
 */
const char* rbwifi_status_to_string(rbwifi_status_t status);

/* ================================================================================================
 * NVS FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Save Wi-Fi STA credentials to NVS.
 *
 * @param[in] ssid      SSID to save.
 * @param[in] password  Password to save.
 * @retval RBPOWER_OK                    Saved and committed.
 * @retval RBPOWER_ERR_INVALID_ARG       @p ssid or @p password is NULL.
 * @retval RBPOWER_ERR_NVS_OPEN_FAILED   NVS namespace could not be opened.
 * @retval RBPOWER_ERR_NVS_WRITE_FAILED  A key could not be written.
 * @retval RBPOWER_ERR_NVS_COMMIT_FAILED Commit failed.
 */
rbpower_err_t rbwifi_nvs_save_credentials(const char* ssid, const char* password);

/**
 * @brief Load Wi-Fi STA credentials from NVS.
 *
 * @param[out] ssid      Buffer for SSID (minimum RBWIFI_MAX_SSID_LEN bytes).
 * @param[out] password  Buffer for password (minimum RBWIFI_MAX_PASSWORD_LEN bytes).
 * @retval RBPOWER_OK                Loaded.
 * @retval RBPOWER_ERR_INVALID_ARG   @p ssid or @p password is NULL.
 * @retval RBPOWER_ERR_NVS_NOT_FOUND Namespace or a key not found (or unreadable).
 */
rbpower_err_t rbwifi_nvs_load_credentials(char* ssid, char* password);

/**
 * @brief Clear saved Wi-Fi STA credentials from NVS.
 *
 * @retval RBPOWER_OK                    Cleared.
 * @retval RBPOWER_ERR_NVS_OPEN_FAILED   NVS namespace could not be opened.
 * @retval RBPOWER_ERR_NVS_COMMIT_FAILED Commit failed.
 */
rbpower_err_t rbwifi_nvs_clear_credentials(void);

/**
 * @brief Check if STA credentials are saved in NVS
 *
 * @return true if credentials are saved
 */
bool rbwifi_nvs_has_credentials(void);

/**
 * @brief Update the STA configuration with new credentials and reconnect if Wi-Fi is running.
 *
 * @param[in] ssid         New SSID.
 * @param[in] password     New password.
 * @param[in] save_to_nvs  true to also persist the credentials in NVS.
 * @retval RBPOWER_OK                       Updated (reconnect started if Wi-Fi is running).
 * @retval RBPOWER_ERR_INVALID_ARG          @p ssid or @p password is NULL.
 * @retval RBPOWER_ERR_WIFI_NOT_INITIALIZED Module not initialized.
 * @retval RBPOWER_ERR_WIFI_INIT_FAILED     esp_wifi_set_config() failed.
 * @return Any error of rbwifi_nvs_save_credentials() when @p save_to_nvs is true.
 */
rbpower_err_t rbwifi_update_sta_config(const char* ssid, const char* password, bool save_to_nvs);

/**
 * @brief Get the IP information of the access-point interface.
 *
 * @param[out] ap_info  Structure to fill.
 * @retval RBPOWER_OK                          Filled.
 * @retval RBPOWER_ERR_INVALID_ARG             @p ap_info is NULL.
 * @retval RBPOWER_ERR_WIFI_AP_NOT_INITIALIZED Module not initialized or AP interface not created.
 * @retval RBPOWER_ERR_HAL_FAILED              esp_netif_get_ip_info() failed.
 */
rbpower_err_t rbwifi_get_ap_info(esp_netif_ip_info_t* ap_info);

/**
 * @brief Get number of stations connected to the access point (counted from AP events).
 *
 * @return Number of connected stations
 */
uint8_t rbwifi_get_ap_station_count(void);

#ifdef __cplusplus
}
#endif

#endif /* RBWIFI_H */