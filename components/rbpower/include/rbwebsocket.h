/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebsocket.h
 * @brief Generic WebSocket client registry (simplified, polling-mode implementation).
 *
 * The live telemetry stream is served by rbwebsocket_telemetry.h (`/ws-telemetry`). This module
 * keeps a server-state structure and a broadcast helper; most of the per-client functions are
 * no-ops in the current implementation (see rbwebsocket.c). Functions are not ISR-safe.
 */

#ifndef RBWEBSOCKET_H
#define RBWEBSOCKET_H

#include <esp_err.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================================
 * CONSTANTS AND CONFIGURATION
 * ================================================================================================ */

#define MAX_WEBSOCKET_CLIENTS 6         ///< Maximum number of simultaneous connections (client array size)
#define WEBSOCKET_PING_INTERVAL_MS 30000 ///< Ping message interval, ms (30 seconds)
#define WEBSOCKET_TIMEOUT_MS 60000       ///< Inactivity timeout, ms (60 seconds)
#define WEBSOCKET_MAX_FRAME_SIZE 8192    ///< Maximum WebSocket frame size, bytes
#define WEBSOCKET_CLIENT_INFO_LEN 64     ///< Buffer size for client information, bytes
#define RBWEBSOCKET_MAX_CLIENTS 6        ///< Maximum number of simultaneous connections (loop bound in rbwebsocket.c)
/* ================================================================================================
 * DATA STRUCTURES
 * ================================================================================================ */

/**
 * @brief WebSocket client information
 */
typedef struct {
    httpd_handle_t server_handle;           ///< HTTP server handle
    int socket_fd;                          ///< WebSocket connection file descriptor
    bool is_active;                         ///< Connection is active
    uint64_t connect_time_us;               ///< Connection time, µs (esp_timer time base)
    uint32_t messages_sent;                 ///< Number of sent messages
    uint32_t messages_received;             ///< Number of received messages
    char client_info[WEBSOCKET_CLIENT_INFO_LEN]; ///< Client information (IP, User-Agent)
    uint64_t last_ping_us;                  ///< Last ping time, µs
    uint64_t last_activity_us;              ///< Last activity time, µs
    bool ping_pending;                      ///< Waiting for pong response
} websocket_client_t;

/**
 * @brief WebSocket server state
 */
typedef struct {
    websocket_client_t clients[MAX_WEBSOCKET_CLIENTS];  ///< Array of clients
    uint8_t active_clients_count;                        ///< Number of active clients
    SemaphoreHandle_t clients_mutex;                     ///< Mutex for thread-safe access to @c clients
    uint32_t total_connections;                          ///< Total number of connections
    uint64_t last_broadcast_us;                          ///< Last broadcast time, µs
    uint32_t total_messages_sent;                        ///< Total number of messages sent
    uint32_t total_messages_received;                    ///< Total number of messages received
    bool is_initialized;                                 ///< Server is initialized
    httpd_handle_t server_handle;                        ///< HTTP server handle
} websocket_server_state_t;

/* ================================================================================================
 * API FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Initialize the WebSocket server state (idempotent).
 * @param[in] server_handle  HTTP server handle.
 * @retval ESP_OK          Initialized (or already initialized).
 * @retval ESP_ERR_NO_MEM  The clients mutex could not be created.
 */
esp_err_t rbwebsocket_init(httpd_handle_t server_handle);

/**
 * @brief Deinitialize the WebSocket server state (deletes the mutex, clears the state).
 */
void rbwebsocket_deinit(void);

/**
 * @brief Send a text message to all registered clients.
 * @param[in] message  NUL-terminated JSON string to send.
 * @retval ESP_OK              Sent to at least one client (or no server handle: message only logged).
 * @retval ESP_ERR_INVALID_ARG @p message is NULL or the server is not initialized.
 * @retval ESP_ERR_TIMEOUT     The clients mutex could not be taken within 100 ms.
 * @retval ESP_FAIL            No active clients, or every send failed.
 */
esp_err_t rbwebsocket_broadcast_message(const char* message);

/**
 * @brief Send a message to a specific client (currently only logs the message).
 * @param[in] socket_fd  Client file descriptor.
 * @param[in] message    Message to send.
 * @retval ESP_OK              Accepted.
 * @retval ESP_ERR_INVALID_ARG @p message is NULL or the server is not initialized.
 */
esp_err_t rbwebsocket_send_to_client(int socket_fd, const char* message);

/**
 * @brief Get number of active clients.
 * @return Always 0 in the current implementation; use rbwebsocket_telemetry_get_client_count()
 *         for the telemetry stream.
 */
uint8_t rbwebsocket_get_active_clients_count(void);

/**
 * @brief Get WebSocket server statistics (all zero in the current implementation).
 * @param[out] total_connections        Total number of connections (may be NULL).
 * @param[out] total_messages_sent      Number of sent messages (may be NULL).
 * @param[out] total_messages_received  Number of received messages (may be NULL).
 */
void rbwebsocket_get_statistics(uint32_t* total_connections, 
                                uint32_t* total_messages_sent, 
                                uint32_t* total_messages_received);

/**
 * @brief Legacy WebSocket endpoint handler: answers with a JSON error telling the client to poll.
 *
 * Not registered by the current web server (the live stream is `/ws-telemetry`).
 *
 * @param[in] req  HTTP request.
 * @return ESP_OK always.
 */
esp_err_t rbwebsocket_handler(httpd_req_t *req);

/**
 * @brief Ping all active clients (no-op in the current implementation).
 * @return ESP_OK always.
 */
esp_err_t rbwebsocket_ping_clients(void);

/**
 * @brief Clean up inactive connections (no-op in the current implementation).
 * @return Number of closed connections (always 0).
 */
uint8_t rbwebsocket_cleanup_inactive_clients(void);

/**
 * @brief WebSocket connections maintenance task (FreeRTOS task body; idles in the current implementation).
 * @param[in] pvParameters  Task parameters (not used).
 */
void rbwebsocket_maintenance_task(void *pvParameters);

/**
 * @brief Access the WebSocket server state (for external access).
 * @return Pointer to the module's static state (never NULL). Take @c clients_mutex before
 *         touching the client array.
 */
extern websocket_server_state_t* rbwebsocket_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // RBWEBSOCKET_H