/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwifi.c
 * @brief Wi-Fi functionality implementation for the rbpower library (see rbwifi.h).
 *
 * @version 1.0.0
 * @date 2025-07-23
 * @author RobotBroker
 */

#include "rbwifi.h"
#include "rbpower.h"
#include "rbpower_enums.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include <string.h>

/* ================================================================================================
 * PRIVATE CONSTANTS
 * ================================================================================================ */

static const char* TAG = "RBWIFI";

#define WIFI_CONNECTED_BIT BIT0 ///< Event-group bit: STA got an IP address
#define WIFI_FAIL_BIT      BIT1 ///< Event-group bit: initial STA connection attempts exhausted

/* ================================================================================================
 * PRIVATE STRUCTURES
 * ================================================================================================ */

/** @brief Module state (single static instance). */
typedef struct {
    bool initialized;                      ///< rbwifi_init() completed
    rbwifi_config_t config;                ///< Copy of the active configuration
    rbwifi_status_t status;                ///< Current STA status
    EventGroupHandle_t event_group;        ///< WIFI_CONNECTED_BIT / WIFI_FAIL_BIT
    httpd_handle_t server;                 ///< Helper HTTP server handle (NULL when stopped)
    rbwifi_ws_client_t ws_clients[RBWIFI_MAX_WS_CLIENTS]; ///< WebSocket client table (unused by the stub)
    uint8_t ws_client_count;               ///< Number of WebSocket clients (unused by the stub)
    int retry_num;                         ///< Initial-connect retry counter
    bool ever_connected;   ///< got IP at least once → a later drop is a RUNTIME drop: reconnect forever

    // Network interfaces
    esp_netif_t* netif_sta;                ///< STA network interface
    esp_netif_t* netif_ap;                 ///< AP network interface
    
    // AP info
    uint8_t ap_station_count;              ///< Stations connected to the AP
    
    // Callbacks
    rbwifi_event_callback_t event_callback;            ///< User Wi-Fi event callback
    void* event_callback_user_data;                    ///< User data for @c event_callback
    rbwifi_ws_message_callback_t ws_message_callback;  ///< WebSocket message callback (never invoked by the stub)
    void* ws_message_callback_user_data;               ///< User data for @c ws_message_callback
} rbwifi_context_t;

/* ================================================================================================
 * PRIVATE VARIABLES
 * ================================================================================================ */

static rbwifi_context_t g_wifi_ctx = {0};

/* ================================================================================================
 * PRIVATE FUNCTIONS
 * ================================================================================================ */

// NVS namespace for WiFi credentials
#define RBWIFI_NVS_NAMESPACE "rbwifi" ///< NVS namespace for the STA credentials
#define RBWIFI_NVS_KEY_SSID "ssid"    ///< NVS key: STA SSID
#define RBWIFI_NVS_KEY_PASS "pass"    ///< NVS key: STA password

/**
 * @brief WiFi event handler
 */
static void rbwifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_wifi_ctx.config.enable_sta) {
            esp_wifi_connect();
            g_wifi_ctx.status = RBWIFI_STATUS_CONNECTING;
            ESP_LOGI(TAG, "WiFi STA started, connecting...");
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_wifi_ctx.config.enable_sta) {
            if (g_wifi_ctx.ever_connected) {
                /* RUNTIME drop (we had an IP) — an always-on device (an HA MQTT sensor must stay up
                 * for days) must NEVER give up: reconnect indefinitely so a transient AP/RF
                 * blip self-heals without a reboot. esp_wifi_connect() is paced by the internal scan
                 * timeout (~1-2 s/attempt), so this is a steady retry, not a CPU spin. On reconnect,
                 * IP_EVENT_STA_GOT_IP fires → esp-mqtt auto-reconnects → LWT "online" re-published. */
                esp_wifi_connect();
                g_wifi_ctx.status = RBWIFI_STATUS_CONNECTING;
                ESP_LOGW(TAG, "STA dropped after being connected — reconnecting (always-on retry)");
            } else if (g_wifi_ctx.retry_num < g_wifi_ctx.config.max_retry) {
                /* INITIAL boot connect — bounded fast retries before falling back to AP mode. */
                esp_wifi_connect();
                g_wifi_ctx.retry_num++;
                g_wifi_ctx.status = RBWIFI_STATUS_DISCONNECTED;
                ESP_LOGD(TAG, "Retry connection to AP (attempt %d/%d)", (int)g_wifi_ctx.retry_num, (int)g_wifi_ctx.config.max_retry);
            } else {
                /* Initial connect exhausted → signal boot logic (AP fallback). Still keep trying in the
                 * background so STA recovers if the AP appears later. */
                xEventGroupSetBits(g_wifi_ctx.event_group, WIFI_FAIL_BIT);
                g_wifi_ctx.status = RBWIFI_STATUS_FAILED;
                esp_wifi_connect();
                ESP_LOGE(TAG, "Failed to connect to AP (initial) — AP fallback + background retry");
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        g_wifi_ctx.retry_num = 0;
        g_wifi_ctx.ever_connected = true;   // a later drop is now a runtime drop → reconnect forever
        g_wifi_ctx.status = RBWIFI_STATUS_CONNECTED;
        xEventGroupClearBits(g_wifi_ctx.event_group, WIFI_FAIL_BIT);   // stale fail from a prior AP-fallback
        xEventGroupSetBits(g_wifi_ctx.event_group, WIFI_CONNECTED_BIT);

        // Time synchronization after network connection
        rbpower_err_t time_sync_err = rbtime_kick_sync();   /* non-blocking: we are in the sys_evt task */
        const char* sync_status = (time_sync_err == RBPOWER_OK) ? "started" :
                                  (time_sync_err == RBPOWER_ERR_NOT_INIT) ? "not initialized (skipped)" :
                                  "failed";
        ESP_LOGI(TAG, "STA got IP: %s | Time sync: %s", ip_str, sync_status);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(event->mac));
        ESP_LOGD(TAG, "Station %s joined, AID=%d", mac_str, (int)event->aid);
        g_wifi_ctx.ap_station_count++;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(event->mac));
        ESP_LOGD(TAG, "Station %s left, AID=%d", mac_str, (int)event->aid);
        if (g_wifi_ctx.ap_station_count > 0) {
            g_wifi_ctx.ap_station_count--;
        }
    }

    // Call user callback
    if (g_wifi_ctx.event_callback) {
        g_wifi_ctx.event_callback(g_wifi_ctx.status, g_wifi_ctx.event_callback_user_data);
    }
}

/**
 * @brief Add WebSocket client (stub)
 */
static int rbwifi_ws_add_client(httpd_handle_t server, int fd)
{
    ESP_LOGW(TAG, "WebSocket client management not implemented");
    return -1;
}

/**
 * @brief Remove WebSocket client (stub)
 */
static void rbwifi_ws_remove_client(int fd)
{
    ESP_LOGW(TAG, "WebSocket client management not implemented");
}

/**
 * @brief WebSocket handler (stub - WebSocket API requires additional configuration)
 */
static esp_err_t rbwifi_websocket_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "WebSocket support is not enabled in this build");
    httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "WebSocket not supported");
    return ESP_FAIL;
}

/* ================================================================================================
 * PUBLIC API FUNCTIONS
 * ================================================================================================ */

rbpower_err_t rbwifi_init(const rbwifi_config_t* config)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (g_wifi_ctx.initialized) {
        ESP_LOGD(TAG, "WiFi already initialized");
        return RBPOWER_ERR_INVALID_STATE;
    }

    // Copy configuration
    memcpy(&g_wifi_ctx.config, config, sizeof(rbwifi_config_t));

    // NVS initialization
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_NVS_INIT_FAILED;
    }

    // Create event group
    g_wifi_ctx.event_group = xEventGroupCreate();
    if (!g_wifi_ctx.event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return RBPOWER_ERR_NO_MEMORY;
    }

    // TCP/IP stack initialization
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create network interfaces
    if (config->enable_sta) {
        g_wifi_ctx.netif_sta = esp_netif_create_default_wifi_sta();
    }

    if (config->enable_ap) {
        g_wifi_ctx.netif_ap = esp_netif_create_default_wifi_ap();
    }

    // WiFi initialization
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi: %s", esp_err_to_name(ret));
        vEventGroupDelete(g_wifi_ctx.event_group);
        return RBPOWER_ERR_WIFI_INIT_FAILED;
    }

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &rbwifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &rbwifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    g_wifi_ctx.initialized = true;
    g_wifi_ctx.status = RBWIFI_STATUS_DISCONNECTED;
    
    ESP_LOGI(TAG, "WiFi initialized successfully");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_deinit(void)
{
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    // Stop WiFi
    rbwifi_stop();

    // Stop server
    rbwifi_server_stop();

    // WiFi deinitialization
    esp_wifi_deinit();

    // Delete event group
    if (g_wifi_ctx.event_group) {
        vEventGroupDelete(g_wifi_ctx.event_group);
    }

    // Clear context
    memset(&g_wifi_ctx, 0, sizeof(g_wifi_ctx));
    
    ESP_LOGI(TAG, "WiFi deinitialized");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_start(void)
{
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    esp_err_t ret;

    // Determine WiFi mode
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (g_wifi_ctx.config.enable_sta && g_wifi_ctx.config.enable_ap) {
        mode = WIFI_MODE_APSTA;
    } else if (g_wifi_ctx.config.enable_sta) {
        mode = WIFI_MODE_STA;
    } else if (g_wifi_ctx.config.enable_ap) {
        mode = WIFI_MODE_AP;
    } else {
        ESP_LOGE(TAG, "No WiFi mode enabled");
        return RBPOWER_ERR_INVALID_STATE;
    }

    // Set mode
    ret = esp_wifi_set_mode(mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_WIFI_INIT_FAILED;
    }

    // STA configuration if enabled
    if (g_wifi_ctx.config.enable_sta) {
        wifi_config_t sta_config = {0};

        // Check if SSID and password are set in configuration
        if (strlen(g_wifi_ctx.config.ssid) > 0) {
            strncpy((char*)sta_config.sta.ssid, g_wifi_ctx.config.ssid,
                    sizeof(sta_config.sta.ssid) - 1);
            strncpy((char*)sta_config.sta.password, g_wifi_ctx.config.password,
                    sizeof(sta_config.sta.password) - 1);
        } else {
            // Try to load from NVS
            char ssid[RBWIFI_MAX_SSID_LEN] = {0};
            char password[RBWIFI_MAX_PASSWORD_LEN] = {0};

            if (rbwifi_nvs_load_credentials(ssid, password) == RBPOWER_OK) {
                ESP_LOGD(TAG, "Loaded WiFi credentials from NVS");
                strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
                strncpy((char*)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

                // Update configuration
                strncpy(g_wifi_ctx.config.ssid, ssid, sizeof(g_wifi_ctx.config.ssid) - 1);
                strncpy(g_wifi_ctx.config.password, password, sizeof(g_wifi_ctx.config.password) - 1);
            } else {
                ESP_LOGW(TAG, "No WiFi credentials available for STA mode");
            }
        }
        
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        
        ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(ret));
            return RBPOWER_ERR_WIFI_INIT_FAILED;
        }
    }

    // AP configuration if enabled
    if (g_wifi_ctx.config.enable_ap) {
        wifi_config_t ap_config = {0};

        strncpy((char*)ap_config.ap.ssid, g_wifi_ctx.config.ap_ssid,
                sizeof(ap_config.ap.ssid) - 1);
        strncpy((char*)ap_config.ap.password, g_wifi_ctx.config.ap_password,
                sizeof(ap_config.ap.password) - 1);

        ap_config.ap.ssid_len = strlen(g_wifi_ctx.config.ap_ssid);
        ap_config.ap.channel = g_wifi_ctx.config.ap_channel;
        ap_config.ap.max_connection = g_wifi_ctx.config.ap_max_connections;
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

        if (strlen(g_wifi_ctx.config.ap_password) == 0) {
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(ret));
            return RBPOWER_ERR_WIFI_AP_INIT_FAILED;
        }

        ESP_LOGD(TAG, "AP configured: SSID=%s, channel=%d", g_wifi_ctx.config.ap_ssid, (int)g_wifi_ctx.config.ap_channel);
    }

    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_WIFI_INIT_FAILED;
    }

    ESP_LOGI(TAG, "WiFi started in %s mode", mode == WIFI_MODE_APSTA ? "AP+STA" : mode == WIFI_MODE_STA ? "STA" : "AP");

    // For STA-only mode, wait for connection
    if (mode == WIFI_MODE_STA && strlen(g_wifi_ctx.config.ssid) > 0) {
        EventBits_t bits = xEventGroupWaitBits(g_wifi_ctx.event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(30000)); // 30 seconds timeout

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP successfully");
            return RBPOWER_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGE(TAG, "Failed to connect to AP");
            return RBPOWER_ERR_TIMEOUT;
        }
    }
    
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_stop(void)
{
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_WIFI_NOT_INITIALIZED;
    }
    
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_WIFI_INIT_FAILED;
    }
    
    g_wifi_ctx.status = RBWIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi stopped");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_get_status(rbwifi_info_t* info)
{
    if (!info) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_WIFI_NOT_INITIALIZED;
    }

    memset(info, 0, sizeof(rbwifi_info_t));
    info->status = g_wifi_ctx.status;

    if (g_wifi_ctx.status == RBWIFI_STATUS_CONNECTED) {
        // Get AP information
        esp_err_t ret = esp_wifi_sta_get_ap_info(&info->ap_info);
        if (ret == ESP_OK) {
            info->rssi = info->ap_info.rssi;
            info->channel = info->ap_info.primary;
        }

        // Get IP information
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL) {
            esp_netif_get_ip_info(netif, &info->ip_info);
        }
    }
    
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_register_event_callback(rbwifi_event_callback_t callback, void* user_data)
{
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_WIFI_NOT_INITIALIZED;
    }
    
    g_wifi_ctx.event_callback = callback;
    g_wifi_ctx.event_callback_user_data = user_data;
    
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_server_start(const rbwifi_server_config_t* config)
{
    if (!config) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_NOT_STARTED;
    }
    
    if (g_wifi_ctx.server) {
        ESP_LOGD(TAG, "Server already running");
        return RBPOWER_ERR_NOT_STARTED;
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = config->port;
    httpd_config.ctrl_port = 32768;
    httpd_config.max_open_sockets = 7;
    httpd_config.task_priority = 5;

    esp_err_t ret = httpd_start(&g_wifi_ctx.server, &httpd_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d: %s", (int)httpd_config.server_port, esp_err_to_name(ret));
        return RBPOWER_ERR_HTTP_INIT_FAILED;
    }

    // Register WebSocket handler if enabled
    if (config->enable_websocket && config->ws_path) {
        httpd_uri_t ws_uri = {
            .uri        = config->ws_path,
            .method     = HTTP_GET,
            .handler    = rbwifi_websocket_handler,
            .user_ctx   = NULL
        };
        httpd_register_uri_handler(g_wifi_ctx.server, &ws_uri);
        ESP_LOGD(TAG, "WebSocket handler registered on %s", config->ws_path);
    }
    
    ESP_LOGI(TAG, "Web server started successfully");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_server_stop(void)
{
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_HTTP_NOT_INITIALIZED;
    }
    
    if (g_wifi_ctx.server) {
        httpd_stop(g_wifi_ctx.server);
        g_wifi_ctx.server = NULL;

        // Clear WebSocket clients list
        memset(g_wifi_ctx.ws_clients, 0, sizeof(g_wifi_ctx.ws_clients));
        g_wifi_ctx.ws_client_count = 0;

        ESP_LOGI(TAG, "Web server stopped");
    }
    
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_server_register_uri_handler(const httpd_uri_t* uri)
{
    if (!uri) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_wifi_ctx.initialized || !g_wifi_ctx.server) {
        return RBPOWER_ERR_HTTP_NOT_INITIALIZED;
    }
    
    esp_err_t ret = httpd_register_uri_handler(g_wifi_ctx.server, uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_HTTP_URI_REGISTER_FAILED;
    }
    
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_ws_send_to_all_clients(const char* message, size_t length)
{
    if (!message || length == 0) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_wifi_ctx.initialized || !g_wifi_ctx.server) {
        return RBPOWER_ERR_WIFI_NOT_INITIALIZED;
    }

    // WebSocket stub
    ESP_LOGW(TAG, "WebSocket send not implemented - WebSocket API requires additional configuration");
    return RBPOWER_ERR_WEBSOCKET_INIT_FAILED;
}

rbpower_err_t rbwifi_ws_register_message_callback(rbwifi_ws_message_callback_t callback, void* user_data)
{
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_WIFI_CONNECT_FAILED;
    }

    g_wifi_ctx.ws_message_callback = callback;
    g_wifi_ctx.ws_message_callback_user_data = user_data;
    
    return RBPOWER_OK;
}

uint8_t rbwifi_ws_get_client_count(void)
{
    return g_wifi_ctx.ws_client_count;
}

bool rbwifi_is_initialized(void)
{
    return g_wifi_ctx.initialized;
}

bool rbwifi_is_connected(void)
{
    return g_wifi_ctx.status == RBWIFI_STATUS_CONNECTED;
}

bool rbwifi_server_is_running(void)
{
    return g_wifi_ctx.server != NULL;
}

const char* rbwifi_status_to_string(rbwifi_status_t status)
{
    switch (status) {
        case RBWIFI_STATUS_NOT_INITIALIZED: return "Not Initialized";
        case RBWIFI_STATUS_DISCONNECTED:    return "Disconnected";
        case RBWIFI_STATUS_CONNECTING:      return "Connecting";
        case RBWIFI_STATUS_CONNECTED:       return "Connected";
        case RBWIFI_STATUS_FAILED:          return "Failed";
        default:                            return "Unknown";
    }
}

/* ================================================================================================
 * NVS FUNCTIONS
 * ================================================================================================ */

rbpower_err_t rbwifi_nvs_save_credentials(const char* ssid, const char* password)
{
    if (!ssid || !password) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret;

    // Open NVS
    ret = nvs_open(RBWIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_NVS_OPEN_FAILED;
    }

    // Save SSID
    ret = nvs_set_str(nvs_handle, RBWIFI_NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return RBPOWER_ERR_NVS_WRITE_FAILED;
    }

    // Save password
    ret = nvs_set_str(nvs_handle, RBWIFI_NVS_KEY_PASS, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return RBPOWER_ERR_NVS_WRITE_FAILED;
    }

    // Commit changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return RBPOWER_ERR_NVS_COMMIT_FAILED;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_nvs_load_credentials(char* ssid, char* password)
{
    if (!ssid || !password) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret;

    // Open NVS
    ret = nvs_open(RBWIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_NVS_NOT_FOUND;
    }

    // Read SSID
    size_t ssid_len = RBWIFI_MAX_SSID_LEN;
    ret = nvs_get_str(nvs_handle, RBWIFI_NVS_KEY_SSID, ssid, &ssid_len);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to read SSID: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return RBPOWER_ERR_NVS_NOT_FOUND;
    }

    // Read password
    size_t pass_len = RBWIFI_MAX_PASSWORD_LEN;
    ret = nvs_get_str(nvs_handle, RBWIFI_NVS_KEY_PASS, password, &pass_len);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to read password: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return RBPOWER_ERR_NVS_NOT_FOUND;
    }

    nvs_close(nvs_handle);
    ESP_LOGD(TAG, "WiFi credentials loaded from NVS");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_nvs_clear_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    // Open NVS
    ret = nvs_open(RBWIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_NVS_OPEN_FAILED;
    }

    // Delete keys
    nvs_erase_key(nvs_handle, RBWIFI_NVS_KEY_SSID);
    nvs_erase_key(nvs_handle, RBWIFI_NVS_KEY_PASS);

    // Commit changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return RBPOWER_ERR_NVS_COMMIT_FAILED;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
    return RBPOWER_OK;
}

bool rbwifi_nvs_has_credentials(void)
{
    char ssid[RBWIFI_MAX_SSID_LEN];
    char password[RBWIFI_MAX_PASSWORD_LEN];
    
    return rbwifi_nvs_load_credentials(ssid, password) == RBPOWER_OK;
}

rbpower_err_t rbwifi_update_sta_config(const char* ssid, const char* password, bool save_to_nvs)
{
    if (!ssid || !password) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_wifi_ctx.initialized) {
        return RBPOWER_ERR_WIFI_NOT_INITIALIZED;
    }

    // Save to NVS if needed
    if (save_to_nvs) {
        rbpower_err_t ret = rbwifi_nvs_save_credentials(ssid, password);
        if (ret != RBPOWER_OK) {
            return ret;
        }
    }

    // Update configuration
    strncpy(g_wifi_ctx.config.ssid, ssid, sizeof(g_wifi_ctx.config.ssid) - 1);
    strncpy(g_wifi_ctx.config.password, password, sizeof(g_wifi_ctx.config.password) - 1);

    // If WiFi is running, reconnect
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_NULL) {
        // Disconnect from current network
        esp_wifi_disconnect();

        // Configure new parameters
        wifi_config_t sta_config = {0};
        strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char*)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to update STA config: %s", esp_err_to_name(ret));
            return RBPOWER_ERR_WIFI_INIT_FAILED;
        }

        // Reset retry counter and connect
        g_wifi_ctx.retry_num = 0;
        g_wifi_ctx.status = RBWIFI_STATUS_CONNECTING;
        esp_wifi_connect();
    }
    
    ESP_LOGI(TAG, "WiFi STA config updated");
    return RBPOWER_OK;
}

rbpower_err_t rbwifi_get_ap_info(esp_netif_ip_info_t* ap_info)
{
    if (!ap_info) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_wifi_ctx.initialized || !g_wifi_ctx.netif_ap) {
        return RBPOWER_ERR_WIFI_AP_NOT_INITIALIZED;
    }
    
    esp_err_t ret = esp_netif_get_ip_info(g_wifi_ctx.netif_ap, ap_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP IP info: %s", esp_err_to_name(ret));
        return RBPOWER_ERR_HAL_FAILED;
    }
    
    return RBPOWER_OK;
}

uint8_t rbwifi_get_ap_station_count(void)
{
    return g_wifi_ctx.ap_station_count;
}