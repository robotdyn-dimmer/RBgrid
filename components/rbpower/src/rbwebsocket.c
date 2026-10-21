/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebsocket.c
 * @brief Simplified WebSocket client registry for rbpower (see rbwebsocket.h).
 * 
 * This is a basic implementation that doesn't rely on the full ESP-IDF WebSocket API.
 * Only rbwebsocket_init() and the broadcast helper do real work; the live telemetry stream
 * is implemented in rbwebsocket_telemetry.c.
 */

#include "rbwebsocket.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "RBWEBSOCKET";

// For now, we'll use a simple HTTP polling mechanism instead of WebSockets
// This is a temporary solution until WebSocket support is confirmed

static websocket_server_state_t ws_state = {0};

esp_err_t rbwebsocket_init(httpd_handle_t server_handle) {
    if (ws_state.is_initialized) {
        ESP_LOGW(TAG, "WebSocket server already initialized");
        return ESP_OK;
    }
    
    memset(&ws_state, 0, sizeof(websocket_server_state_t));
    
    ws_state.clients_mutex = xSemaphoreCreateMutex();
    if (!ws_state.clients_mutex) {
        ESP_LOGE(TAG, "Failed to create clients mutex");
        return ESP_ERR_NO_MEM;
    }
    
    ws_state.server_handle = server_handle;
    ws_state.is_initialized = true;
    
    ESP_LOGI(TAG, "WebSocket server initialized (using polling mode)");
    return ESP_OK;
}

void rbwebsocket_deinit(void) {
    if (!ws_state.is_initialized) {
        return;
    }
    
    if (ws_state.clients_mutex) {
        vSemaphoreDelete(ws_state.clients_mutex);
    }
    
    memset(&ws_state, 0, sizeof(websocket_server_state_t));
    
    ESP_LOGI(TAG, "WebSocket server deinitialized");
}

esp_err_t rbwebsocket_broadcast_message(const char* message) {
    if (!message || !ws_state.is_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check whether there are active WebSocket clients
    if (ws_state.active_clients_count == 0) {
        ESP_LOGD(TAG, "No active WebSocket clients to broadcast to");
        return ESP_FAIL; // Return FAIL to signal that there are no clients
    }
    
    // If there is an HTTP server, send through the WebSocket API
    if (ws_state.server_handle) {
        // Build the WebSocket frame
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)message;
        ws_pkt.len = strlen(message);
        
        // Send to all active clients
        size_t clients_sent = 0;
        
        // Take the mutex for safe access to the client list
        if (xSemaphoreTake(ws_state.clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < RBWEBSOCKET_MAX_CLIENTS; i++) {
                if (ws_state.clients[i].is_active) {
                    // Send the data to the client
                    esp_err_t ret = httpd_ws_send_frame_async(ws_state.server_handle, 
                                                              ws_state.clients[i].socket_fd, 
                                                              &ws_pkt);
                    if (ret == ESP_OK) {
                        clients_sent++;
                        ws_state.total_messages_sent++;
                        ESP_LOGD(TAG, "Sent telemetry to client fd=%d", ws_state.clients[i].socket_fd);
                    } else {
                        ESP_LOGW(TAG, "Failed to send to client fd=%d: %s", 
                                ws_state.clients[i].socket_fd, esp_err_to_name(ret));
                        // If the send failed, mark the client inactive
                        ws_state.clients[i].is_active = false;
                        ws_state.active_clients_count--;
                    }
                }
            }
            xSemaphoreGive(ws_state.clients_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to acquire mutex for WebSocket broadcast");
            return ESP_ERR_TIMEOUT;
        }
        
        if (clients_sent > 0) {
            ESP_LOGD(TAG, "Broadcasted message to %d WebSocket clients", clients_sent);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Failed to send to any WebSocket clients");
            return ESP_FAIL;
        }
    } else {
        // No HTTP server: just log
        ESP_LOGD(TAG, "No HTTP server handle, logging message: %s", message);
        return ESP_OK;
    }
}

esp_err_t rbwebsocket_send_to_client(int socket_fd, const char* message) {
    if (!message || !ws_state.is_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Would send to client %d: %s", socket_fd, message);
    
    return ESP_OK;
}

uint8_t rbwebsocket_get_active_clients_count(void) {
    return 0; // No real WebSocket clients in this implementation
}

void rbwebsocket_get_statistics(uint32_t* total_connections, 
                               uint32_t* total_messages_sent, 
                               uint32_t* total_messages_received) {
    if (total_connections) *total_connections = 0;
    if (total_messages_sent) *total_messages_sent = 0;
    if (total_messages_received) *total_messages_received = 0;
}

// This handler will be used for HTTP long-polling instead of WebSocket
esp_err_t rbwebsocket_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "WebSocket endpoint called (using HTTP polling fallback)");
    
    // Send a JSON response indicating WebSocket is not available
    const char* response = "{\"error\":\"WebSocket not supported, use polling\"}";
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response, strlen(response));
    
    return ESP_OK;
}

esp_err_t rbwebsocket_ping_clients(void) {
    // No-op in this implementation
    return ESP_OK;
}

uint8_t rbwebsocket_cleanup_inactive_clients(void) {
    // No-op in this implementation
    return 0;
}

void rbwebsocket_maintenance_task(void *pvParameters) {
    ESP_LOGI(TAG, "WebSocket maintenance task started (no-op mode)");
    
    // This task doesn't do anything in the simplified implementation
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Sleep for 30 seconds
    }
}

websocket_server_state_t* rbwebsocket_get_state(void) {
    return &ws_state;
}