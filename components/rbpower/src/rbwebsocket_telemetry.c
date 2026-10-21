/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebsocket_telemetry.c
 * @brief WebSocket handler for telemetry streaming (`/ws-telemetry`).
 *
 * Keeps a small table of telemetry clients (socket fd + requested frame version), starts the
 * 3 s telemetry timer when the first client connects and stops it when the last one leaves.
 * Broadcasts are produced on the esp_timer task and marshalled onto the httpd task with
 * httpd_queue_work(), because httpd_ws_send_frame_async() must touch the socket from the httpd
 * task. The client table is protected by @c client_count_mutex.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rbtelemetry.h"
#include "rbjson.h"

static const char* TAG = "WS_TELEMETRY";

// Number of active telemetry WebSocket clients
static int active_ws_clients = 0;
static SemaphoreHandle_t client_count_mutex = NULL;

/** @brief Maximum number of simultaneous telemetry WebSocket clients (client table size). */
#define MAX_TELEMETRY_CLIENTS 10
static struct {
    int fd;
    bool active;
    uint8_t frame;      /* 1 = legacy frame, 2 = v2 frame (client connected with ?v=2) */
} telemetry_clients[MAX_TELEMETRY_CLIENTS];
static httpd_handle_t telemetry_server_handle = NULL;

/**
 * @brief Per-send context queued onto the httpd task.
 *
 * Owns a private copy of the payload because the caller (broadcast) frees its JSON as soon as
 * it returns — long before the worker runs.
 */
typedef struct {
    httpd_handle_t hd;        ///< HTTP server handle the socket belongs to
    int            fd;        ///< Client socket descriptor
    char          *payload;   ///< Private copy of the frame text; freed by the worker
} ws_async_send_ctx_t;

/* Backpressure (observed on a live 3-phase hub with 4–5 app tabs incl. throttled background pages: PSRAM
 * fell in 100–340 KB steps, HTTP answers took 1.5–4 s, module poll holes 5–11 s). A client whose previous frame
 * is still queued/being sent gets NO new frame (telemetry is latest-wins): memory is bounded to one frame
 * per client and a slow socket can't pile work onto the httpd task. Counters are indexed by fd. */
/** @brief Size of the per-fd "frame in flight" counter table (socket fds at or above this are not tracked). */
#define WS_PENDING_FD_MAX 64
static volatile uint8_t s_ws_pending[WS_PENDING_FD_MAX];
static volatile uint32_t s_ws_dropped_busy;

// Runs ON the httpd task (via httpd_queue_work). httpd_ws_send_frame_async must touch the
// socket from the httpd task itself — calling it from the core-0/esp_timer task races httpd's
// internal socket state (intermittent INVALID_STATE / frame corruption).
static void ws_async_send_worker(void *arg) {
    ws_async_send_ctx_t *ctx = (ws_async_send_ctx_t*)arg;
    if (!ctx) return;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)ctx->payload;
    ws_pkt.len = ctx->payload ? strlen(ctx->payload) : 0;

    /* A client that vanished without a CLOSE frame keeps its slot; once httpd hands the fd to a REST
     * connection, send_frame_async still "succeeds" and writes WS frames into that HTTP response, and the
     * slot is never cleaned. Send only to a socket that is still a websocket. */
    esp_err_t ret = (httpd_ws_get_fd_info(ctx->hd, ctx->fd) == HTTPD_WS_CLIENT_WEBSOCKET)
                    ? httpd_ws_send_frame_async(ctx->hd, ctx->fd, &ws_pkt) : ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "async WS send to fd=%d failed: %s", ctx->fd, esp_err_to_name(ret));
        // Drop the dead client + keep the counter in sync. We are on the httpd task, so taking
        // client_count_mutex here is safe (same task family as the connect/disconnect handlers).
        if (client_count_mutex && xSemaphoreTake(client_count_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int i = 0; i < MAX_TELEMETRY_CLIENTS; i++) {
                if (telemetry_clients[i].active && telemetry_clients[i].fd == ctx->fd) {
                    telemetry_clients[i].active = false;
                    if (active_ws_clients > 0) active_ws_clients--;
                    // If this dead-socket drop emptied the client list, stop the 3-sec telemetry
                    // timer — otherwise it keeps firing and broadcasting to nobody. The graceful
                    // CLOSE handler stops it too, but an ungraceful drop (no CLOSE frame) is only
                    // ever detected HERE, via the failed send. Same task as the
                    // CLOSE handler (httpd), so calling stop under the lock mirrors it safely.
                    if (active_ws_clients == 0) {
                        ESP_LOGI(TAG, "Last WS client dropped (dead socket) — stopping telemetry");
                        rbpower_telemetry_stop_websocket();
                    }
                    break;
                }
            }
            xSemaphoreGive(client_count_mutex);
        }
    }
    if (ctx->fd >= 0 && ctx->fd < WS_PENDING_FD_MAX && s_ws_pending[ctx->fd]) s_ws_pending[ctx->fd]--;
    free(ctx->payload);
    free(ctx);
}

/**
 * @brief Diagnostics for GET /api/system/telemetry-config: list active WS clients with their frame version.
 *
 * Appends one object `{ "fd": <n>, "frame": 1|2 }` per active client. Takes the client mutex
 * (blocking); call from task context.
 *
 * @param[in,out] arr  cJSON array to append to (NULL → no-op).
 */
void telemetry_ws_client_list(cJSON* arr)
{
    if (!arr) return;
    if (client_count_mutex) xSemaphoreTake(client_count_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TELEMETRY_CLIENTS; i++) {
        if (!telemetry_clients[i].active) continue;
        cJSON* o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddNumberToObject(o, "fd", telemetry_clients[i].fd);
        cJSON_AddNumberToObject(o, "frame", telemetry_clients[i].frame ? telemetry_clients[i].frame : 1);
        cJSON_AddItemToArray(arr, o);
    }
    if (client_count_mutex) xSemaphoreGive(client_count_mutex);
}

/**
 * @brief Check whether any active telemetry client requested the v2 frame (`?v=2`).
 * @return true if at least one active client uses frame version 2.
 */
bool telemetry_ws_has_frame2(void)
{
    bool any = false;
    if (client_count_mutex) xSemaphoreTake(client_count_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TELEMETRY_CLIENTS; i++) if (telemetry_clients[i].active && telemetry_clients[i].frame == 2) { any = true; break; }
    if (client_count_mutex) xSemaphoreGive(client_count_mutex);
    return any;
}

/* Two payloads: the global frame for ordinary clients, v2 for clients that asked ?v=2 on the
 * handshake. message2 == NULL → everyone gets message.
 *
 * Snapshots the active client fds under the lock, then marshals one async send per client
 * onto the httpd task via httpd_queue_work — the broadcast itself runs on the esp_timer 3-sec
 * task, and the actual socket write must happen on the httpd task (see ws_async_send_worker). */
esp_err_t broadcast_to_telemetry_clients2(const char* message, const char* message2);
esp_err_t broadcast_to_telemetry_clients(const char* message) { return broadcast_to_telemetry_clients2(message, NULL); }
/**
 * @brief Broadcast a telemetry frame, with an optional v2 variant for `?v=2` clients.
 *
 * Runs on the esp_timer task; each send is queued onto the httpd task. A client whose previous
 * frame is still in flight is skipped (latest-wins backpressure).
 *
 * @param[in] message   Frame for ordinary (v1) clients; copied per client.
 * @param[in] message2  Frame for v2 clients, or NULL to send @p message to everyone.
 * @retval ESP_OK              At least one send was queued.
 * @retval ESP_ERR_INVALID_ARG @p message is NULL or no client has connected yet (no server handle).
 * @retval ESP_FAIL            Nothing was queued (no clients, all busy, or allocation/queue failure).
 */
esp_err_t broadcast_to_telemetry_clients2(const char* message, const char* message2) {
    if (!message || !telemetry_server_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // Snapshot the active fds under the mutex (the connect/disconnect handlers mutate the array
    // on the httpd task). Keep the critical section to a plain array copy — no I/O under lock.
    int fds[MAX_TELEMETRY_CLIENTS];
    uint8_t frs[MAX_TELEMETRY_CLIENTS];
    int n = 0;
    if (client_count_mutex) xSemaphoreTake(client_count_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TELEMETRY_CLIENTS; i++) {
        if (telemetry_clients[i].active) {
            frs[n] = telemetry_clients[i].frame;
            fds[n++] = telemetry_clients[i].fd;
        }
    }
    if (client_count_mutex) xSemaphoreGive(client_count_mutex);

    int queued = 0;
    for (int i = 0; i < n; i++) {
        if (fds[i] >= 0 && fds[i] < WS_PENDING_FD_MAX && s_ws_pending[fds[i]]) {   /* previous frame still in flight */
            s_ws_dropped_busy++;
            if ((s_ws_dropped_busy & 0x3F) == 1)
                ESP_LOGW(TAG, "WS fd=%d busy — frame skipped (total skipped %lu)", fds[i], (unsigned long)s_ws_dropped_busy);
            continue;
        }
        /* PSRAM for the per-client copies: ten clients × ~5 KB payload per 3-s push
         * out of INTERNAL heap fragmented it for good (after a 10-client session: internal free
         * 23 KB, largest block 4.8 KB → xTaskCreate(reboot_task) failed, HTTP 500s next). LWIP
         * copies the bytes into pbufs anyway, so the source buffer may live in external RAM. */
        ws_async_send_ctx_t *ctx = heap_caps_malloc(sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ctx) ctx = malloc(sizeof(*ctx));
        if (!ctx) continue;
        ctx->hd = telemetry_server_handle;
        ctx->fd = fds[i];
        const char *src = (frs[i] == 2 && message2) ? message2 : message;
        size_t slen = strlen(src) + 1;
        ctx->payload = heap_caps_malloc(slen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ctx->payload) ctx->payload = malloc(slen);
        if (!ctx->payload) { free(ctx); continue; }
        memcpy(ctx->payload, src, slen);                                           // worker frees it
        if (fds[i] >= 0 && fds[i] < WS_PENDING_FD_MAX) s_ws_pending[fds[i]]++;
        if (httpd_queue_work(telemetry_server_handle, ws_async_send_worker, ctx) == ESP_OK) {
            queued++;
        } else {
            ESP_LOGW(TAG, "httpd_queue_work full — dropping WS frame for fd=%d", fds[i]);
            if (fds[i] >= 0 && fds[i] < WS_PENDING_FD_MAX && s_ws_pending[fds[i]]) s_ws_pending[fds[i]]--;
            free(ctx->payload);
            free(ctx);
        }
    }

    if (queued > 0) {
        ESP_LOGD(TAG, "Queued telemetry broadcast to %d clients", queued);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* Initialize WebSocket telemetry (documented in rbwebsocket_telemetry.h). */
esp_err_t rbwebsocket_telemetry_init(void) {
    if (!client_count_mutex) {
        client_count_mutex = xSemaphoreCreateMutex();
        if (!client_count_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Clear the client table
    memset(telemetry_clients, 0, sizeof(telemetry_clients));
    
    return ESP_OK;
}

/* WebSocket handler for /ws-telemetry (documented in rbwebsocket_telemetry.h). */
esp_err_t websocket_telemetry_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        /* WS read-token: the handshake is a data-read; when a READ token is armed the client
         * passes it as ?token=<read|admin> (browsers can't set WS headers). Unarmed → open.
         * 401/429 already sent on false. */
        extern bool rbwebserver_api_check_read(httpd_req_t *req);
        if (!rbwebserver_api_check_read(req)) return ESP_FAIL;
        // New WebSocket connection
        ESP_LOGI(TAG, "New WebSocket connection opened");
        
        // Remember the server handle
        if (!telemetry_server_handle) {
            telemetry_server_handle = req->handle; // Correct way to obtain the handle
        }
        
        // Get the socket descriptor
        int fd = httpd_req_to_sockfd(req);
        uint8_t want_frame = 1;   /* ?v=2 on the handshake → this client gets the v2 frame */
        {
            char qs[96];
            if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
                char vv[4];
                if (httpd_query_key_value(qs, "v", vv, sizeof(vv)) == ESP_OK && vv[0] == '2') want_frame = 2;
            }
            /* belt and braces: some httpd paths hand the handshake over with the query only in req->uri */
            const char* q = strchr(req->uri, '?');
            if (want_frame == 1 && q && (strstr(q, "v=2") != NULL)) want_frame = 2;
            ESP_LOGI(TAG, "WS handshake uri=%s → frame v%u", req->uri, (unsigned)want_frame);
        }
        
        // Increment the client count and add the client to the table
        if (client_count_mutex && xSemaphoreTake(client_count_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Add the client to the table
            bool added = false, fresh = false;
            /* fd reuse: a just-closed socket's slot may still be active when the OS hands the same
             * fd to a new connection (close is detected asynchronously). Reuse that slot instead of
             * adding a duplicate — otherwise the new client got BOTH frames (v1 + v2) once. */
            for (int i = 0; i < MAX_TELEMETRY_CLIENTS && !added; i++) {
                if (telemetry_clients[i].active && telemetry_clients[i].fd == fd) {
                    telemetry_clients[i].frame = want_frame;
                    added = true;
                    ESP_LOGI(TAG, "Telemetry client fd=%d re-registered at slot %d (frame v%u)", fd, i, (unsigned)want_frame);
                }
            }
            for (int i = 0; i < MAX_TELEMETRY_CLIENTS && !added; i++) {
                if (!telemetry_clients[i].active) {
                    telemetry_clients[i].fd = fd;
                    if (fd >= 0 && fd < WS_PENDING_FD_MAX) s_ws_pending[fd] = 0;   /* fresh client on a reused fd */
                    telemetry_clients[i].frame = want_frame;
                    telemetry_clients[i].active = true;
                    added = true; fresh = true;
                    ESP_LOGI(TAG, "Added telemetry client fd=%d at slot %d", fd, i);
                    break;
                }
            }
            
            if (!added) {
                ESP_LOGW(TAG, "No free slots for telemetry client");
            }
            
            /* count only a new slot: a re-registered fd or a refused client never decremented, so the count
             * never returned to 0 and telemetry never stopped */
            if (fresh) active_ws_clients++;
            
            // First client: start telemetry
            if (active_ws_clients == 1) {
                ESP_LOGI(TAG, "First WebSocket client connected, starting telemetry");
                esp_err_t ret = rbpower_telemetry_start_websocket();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start telemetry: %s", esp_err_to_name(ret));
                }
            }
            
            ESP_LOGI(TAG, "Active WebSocket clients: %d", active_ws_clients);
            xSemaphoreGive(client_count_mutex);
        }
        
        // Send the welcome message
        const char* welcome_msg = "{\"type\":\"welcome\",\"message\":\"Telemetry WebSocket connected\"}";
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)welcome_msg;
        ws_pkt.len = strlen(welcome_msg);
        
        esp_err_t ret = httpd_ws_send_frame(req, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send welcome message: %d", ret);
        }
        
        // Send the first telemetry frame immediately
        ret = rbpower_telemetry_send_websocket_once();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send initial telemetry: %s", esp_err_to_name(ret));
        }
        
        return ESP_OK;
    }
    
    // Receive a WebSocket frame
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    // Get the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    
    if (ws_pkt.len > 1024) {   /* client-declared length: commands are < 100 B; no multi-MB calloc */
        ESP_LOGW(TAG, "WS frame too long (%u B) — dropped", (unsigned)ws_pkt.len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (ws_pkt.len) {
        // Allocate the payload buffer
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_FAIL;
        }
        ws_pkt.payload = buf;
        
        // Read the payload
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }
    
    // Dispatch on the frame type
    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // Answer PING with PONG
        ESP_LOGD(TAG, "Received PING, sending PONG");
        httpd_ws_frame_t pong_pkt;
        memset(&pong_pkt, 0, sizeof(httpd_ws_frame_t));
        pong_pkt.type = HTTPD_WS_TYPE_PONG;
        pong_pkt.payload = ws_pkt.payload;
        pong_pkt.len = ws_pkt.len;
        ret = httpd_ws_send_frame(req, &pong_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_send_frame (PONG) failed with %d", ret);
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        // Text message
        ESP_LOGI(TAG, "Received text message: %s", ws_pkt.payload);
        
        // Parse client commands
        if (strstr((char*)ws_pkt.payload, "get_telemetry")) {
            // Client requested a telemetry frame
            ret = rbpower_telemetry_send_websocket_once();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send telemetry on request: %s", esp_err_to_name(ret));
            }
        } else if (strstr((char*)ws_pkt.payload, "stop_telemetry")) {
            // Client requested to stop telemetry
            ESP_LOGI(TAG, "Client requested to stop telemetry");
            // Send the acknowledgement
            const char* ack_msg = "{\"type\":\"ack\",\"message\":\"Telemetry stopped for this client\"}";
            httpd_ws_frame_t ack_pkt;
            memset(&ack_pkt, 0, sizeof(httpd_ws_frame_t));
            ack_pkt.type = HTTPD_WS_TYPE_TEXT;
            ack_pkt.payload = (uint8_t*)ack_msg;
            ack_pkt.len = strlen(ack_msg);
            httpd_ws_send_frame(req, &ack_pkt);
        } else if (strstr((char*)ws_pkt.payload, "start_telemetry")) {
            // Client requested to start telemetry
            ESP_LOGI(TAG, "Client requested to start telemetry");
            // Send the acknowledgement
            const char* ack_msg = "{\"type\":\"ack\",\"message\":\"Telemetry started for this client\"}";
            httpd_ws_frame_t ack_pkt;
            memset(&ack_pkt, 0, sizeof(httpd_ws_frame_t));
            ack_pkt.type = HTTPD_WS_TYPE_TEXT;
            ack_pkt.payload = (uint8_t*)ack_msg;
            ack_pkt.len = strlen(ack_msg);
            httpd_ws_send_frame(req, &ack_pkt);
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        // Client closed the connection
        ESP_LOGI(TAG, "WebSocket connection closed");
        
        // Get the socket descriptor
        int fd = httpd_req_to_sockfd(req);
        
        // Decrement the client count and remove the client from the table
        if (client_count_mutex && xSemaphoreTake(client_count_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Remove the client from the table
            bool removed = false;
            for (int i = 0; i < MAX_TELEMETRY_CLIENTS; i++) {
                if (telemetry_clients[i].active && telemetry_clients[i].fd == fd) {
                    telemetry_clients[i].active = false;
                    removed = true;
                    ESP_LOGI(TAG, "Removed telemetry client fd=%d from slot %d", fd, i);
                    break;
                }
            }

            // Decrement ONLY if we actually removed an active slot here — otherwise the
            // broadcast path may have already dropped this fd (and decremented), and an
            // unconditional decrement would double-count, drifting the counter below the
            // real client count.
            if (removed && active_ws_clients > 0) {
                active_ws_clients--;
                
                // Last client gone: stop telemetry
                if (active_ws_clients == 0) {
                    ESP_LOGI(TAG, "Last WebSocket client disconnected, stopping telemetry");
                    esp_err_t ret = rbpower_telemetry_stop_websocket();
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to stop telemetry: %s", esp_err_to_name(ret));
                    }
                }
                
                ESP_LOGI(TAG, "Active WebSocket clients: %d", active_ws_clients);
            }
            xSemaphoreGive(client_count_mutex);
        }
    }
    
    free(buf);
    return ret;
}

/* Number of active telemetry WebSocket clients (documented in rbwebsocket_telemetry.h). */
int rbwebsocket_telemetry_get_client_count(void) {
    int count = 0;
    if (client_count_mutex && xSemaphoreTake(client_count_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = active_ws_clients;
        xSemaphoreGive(client_count_mutex);
    }
    return count;
}

/* Reset the client counter on server restart (documented in rbwebsocket_telemetry.h). */
void rbwebsocket_telemetry_reset_clients(void) {
    if (client_count_mutex && xSemaphoreTake(client_count_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (active_ws_clients > 0) {
            ESP_LOGI(TAG, "Resetting client count, stopping telemetry");
            rbpower_telemetry_stop_websocket();
            active_ws_clients = 0;
        }
        xSemaphoreGive(client_count_mutex);
    }
}