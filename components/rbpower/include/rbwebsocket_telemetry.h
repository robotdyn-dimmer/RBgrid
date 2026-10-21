/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebsocket_telemetry.h
 * @brief WebSocket handler for telemetry streaming (`/ws-telemetry`).
 *
 * The handler and the counters run on the httpd task; broadcasts are called from the
 * esp_timer telemetry task and queue the actual socket writes onto the httpd task.
 * The client table is protected by an internal mutex.
 */

#ifndef RBWEBSOCKET_TELEMETRY_H
#define RBWEBSOCKET_TELEMETRY_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WebSocket telemetry: create the client mutex (once) and clear the client table.
 * @retval ESP_OK          Initialized.
 * @retval ESP_ERR_NO_MEM  The client mutex could not be created.
 */
esp_err_t rbwebsocket_telemetry_init(void);

/**
 * @brief WebSocket handler: GET `/ws-telemetry` (upgrade). Read token required when one is armed
 *        (passed as `?token=` because browsers cannot set WebSocket headers).
 *
 * Runs on the httpd task. A `?v=2` query on the handshake selects the v2 telemetry frame.
 *
 * @param[in] req  HTTP request (handshake or incoming WebSocket frame).
 * @retval ESP_OK                Handshake accepted / frame processed.
 * @retval ESP_FAIL              Read-token check failed (401/429 already sent) or buffer allocation failed.
 * @retval ESP_ERR_INVALID_SIZE  Incoming frame longer than 1024 bytes (dropped).
 * @return Any other error from httpd_ws_recv_frame() / httpd_ws_send_frame().
 *
 * This handler:
 * - On new connection (GET) starts telemetry if this is the first client
 * - On connection close stops telemetry if this is the last client
 * - Processes commands from client (get_telemetry, start_telemetry, stop_telemetry)
 * - Responds to PING messages
 */
esp_err_t websocket_telemetry_handler(httpd_req_t *req);

/**
 * @brief Get number of active telemetry WebSocket clients.
 * @return Number of connected clients (0 if the mutex could not be taken within 100 ms).
 */
int rbwebsocket_telemetry_get_client_count(void);

/**
 * @brief Reset the client counter and stop telemetry if clients were active.
 *
 * Used when restarting the server to clear state.
 */
void rbwebsocket_telemetry_reset_clients(void);

/**
 * @brief Send a message to all telemetry clients (same frame for v1 and v2 clients).
 *
 * Safe to call from the esp_timer task: the socket writes are queued onto the httpd task.
 *
 * @param[in] message  NUL-terminated text frame; copied per client, so the caller may free it on return.
 * @retval ESP_OK              At least one send was queued.
 * @retval ESP_ERR_INVALID_ARG @p message is NULL or no client has connected yet.
 * @retval ESP_FAIL            Nothing was queued.
 */
esp_err_t broadcast_to_telemetry_clients(const char* message);

#ifdef __cplusplus
}
#endif

#endif // RBWEBSOCKET_TELEMETRY_H