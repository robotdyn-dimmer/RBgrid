/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver_internal.h
 * @brief PRIVATE shared surface of the web server translation units (core ↔ _ota / _wifi / _api / _ui).
 *
 * NOT a public API — do not include outside the rbwebserver*.c translation units. The web server is
 * split by concern into rbwebserver.c (core: start/stop, routing, CORS, system handlers),
 * rbwebserver_api.c, rbwebserver_ota.c, rbwebserver_wifi.c and rbwebserver_ui.c. This header carries
 * only what the split handler files need from the core TU: shared helpers and accessors for the core's
 * module state. Module globals stay `static` in rbwebserver.c (notably g_json_context, which also exists
 * as a SEPARATE static in rbjson.c — de-static'ing would collide at link); the split files reach
 * them through the accessors below instead of touching the globals directly.
 *
 * Auth model used in the handler notes below: "admin token" = rbapi_require_auth() (enforced only once
 * an admin token is provisioned; until then the hub is in the open onboarding state), "read token" =
 * rbapi_require_read() (enforced only once a read token is provisioned). All HTTP handlers run on the
 * httpd task.
 */
#ifndef RBWEBSERVER_INTERNAL_H
#define RBWEBSERVER_INTERNAL_H

#include "esp_http_server.h"
#include "rbwebserver.h"
#include "rbjson.h"
#include "rbpower.h"

/**
 * @brief Read a request body COMPLETELY.
 *
 * httpd_req_recv() returns whatever one socket read delivers — for a body larger than one TCP
 * segment (units-config ≈6 KB) a single call hands the JSON parser a truncated document →
 * "Invalid JSON". Loops until @p want bytes, a timeout retry budget (3 timeouts) or an error.
 *
 * @param[in]  req   HTTP request.
 * @param[out] buf   Destination buffer (at least @p want bytes; not NUL-terminated).
 * @param[in]  want  Number of bytes to read, normally req->content_len.
 * @return Bytes read (== @p want on success; fewer if the timeout budget ran out after a partial
 *         read), 0/negative on failure — same sign convention as httpd_req_recv().
 */
static inline int rbapi_recv_body(httpd_req_t *req, char *buf, size_t want)
{
    size_t total = 0; int retries = 0;
    while (total < want) {
        int r = httpd_req_recv(req, buf + total, want - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++retries > 3) return (int)total ? (int)total : r; continue; }
        if (r <= 0) return r;
        total += (size_t)r;
    }
    return (int)total;
}


#define CONFIG_COMPACT_SIZE   8192   ///< Maximum JSON config size, bytes (shared by the split handlers)

/* Shared helpers — DEFINED (non-static) in rbwebserver.c. */

/**
 * @brief Add the CORS response headers according to the /sec_cfg policy.
 *
 * dev_cors mode (default, no /sec_cfg) → `Access-Control-Allow-Origin: *`; otherwise only an
 * allow-listed Origin is echoed back (+ `Vary: Origin`). Also sets the allowed methods/headers, the
 * exposed headers and, on OPTIONS, the Chrome Private-Network-Access grant.
 *
 * @param[in] req  HTTP request the headers are added to.
 */
void      add_cors_headers(httpd_req_t *req);

/**
 * @brief OPTIONS (CORS preflight) handler for the listed endpoints and the catch-all OPTIONS wildcard. No auth.
 *
 * Adds the CORS headers and answers 204 No Content.
 *
 * @param[in] req  HTTP request.
 * @return ESP_OK always.
 */
esp_err_t cors_options_handler(httpd_req_t *req);

/**
 * @brief Tell whether a request arrived over the hub's own access point.
 *
 * True when the local socket address is the soft-AP address 192.168.4.1, or the peer is in the
 * 192.168.4.x soft-AP subnet (ESP-IDF default soft-AP network).
 *
 * @param[in] req  HTTP request.
 * @return true for an AP-side client, false for a station-side (home network) client.
 */
bool      is_request_from_ap_network(httpd_req_t *req);

/* Accessors for the core's module state (globals remain static in rbwebserver.c). */

/**
 * @brief Running httpd handle (also exported in rbwebserver.h).
 * @return Server handle, or NULL before rbwebserver_start().
 */
httpd_handle_t               rbwebserver_core_server(void);

/**
 * @brief Extended configuration the server was initialized with.
 * @return Pointer to the live configuration (may be NULL if not set). Not locked.
 */
rbpower_extended_config_t   *rbwebserver_core_ext_cfg(void);

/**
 * @brief JSON module context the server was initialized with.
 * @return Pointer to the JSON context (may be NULL if not set).
 */
rbgrid_json_context_t       *rbwebserver_core_json_ctx(void);

/* ---- OTA handlers (rbwebserver_ota.c) ---- */

/**
 * @brief GET /api/system/version — running firmware version, project, IDF, build date, partition,
 *        stats-storage state and image MD5. No auth.
 * @param[in] req  HTTP request.
 * @return ESP_OK always.
 */
esp_err_t rbwebserver_api_version_handler(httpd_req_t *req);

/**
 * @brief POST /api/system/ota — stream a firmware .bin into the update slot, validate, switch the boot
 *        partition and reboot. Admin token required.
 *
 * Optional headers X-FW-SHA256 (64 hex) and X-FW-Version pin the image; X-FW-SHA256 is mandatory when
 * the only usable target is the factory slot. On success the response is sent, open hours are
 * flushed and the hub restarts (the function does not return in that case).
 *
 * @param[in] req  HTTP request (body = raw firmware image).
 * @return ESP_OK after an error response (400/500 JSON); ESP_FAIL on auth failure or buffer OOM.
 */
esp_err_t rbwebserver_api_ota_handler(httpd_req_t *req);

/* ---- API/config handlers + command dispatch (rbwebserver_api.c) ---- */

/**
 * @brief GET system config (legacy implementation; not registered — GET /api/system/system-config is served by
 *        rbwebserver_system_config_handler). No auth.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_system_config_handler(httpd_req_t *req);

/**
 * @brief GET/POST /api/system/hardware-config — hardware configuration. GET: no auth; POST: admin token
 *        (applies on restart).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_hardware_config_handler(httpd_req_t *req);

/**
 * @brief GET /api/system/time-config — time configuration. No auth.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_time_config_handler(httpd_req_t *req);

/**
 * @brief GET/POST/OPTIONS /api/system/tariff-plan-config — tariff plan. GET: no auth; POST: admin token
 *        (saved, then applied live).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_tariff_plan_config_handler(httpd_req_t *req);

/**
 * @brief GET/POST/OPTIONS /api/system/telemetry-config — telemetry config, profile (lean/full), frame
 *        version and connected WS clients. GET: no auth; POST: admin token (applied at runtime).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_telemetry_config_handler(httpd_req_t *req);

/**
 * @brief GET /api/system/wifi-config — Wi-Fi configuration. No auth.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_wifi_config_handler(httpd_req_t *req);

/**
 * @brief GET /api/system/ap — this hub's AP SSID and password. Admin token required AND the hub must be
 *        armed (403 while no admin token is provisioned).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ap_info_handler(httpd_req_t *req);

/**
 * @brief POST /api/system/ap {"password":"..."} — set the user's own AP password (8..63 printable ASCII;
 *        "" = back to the provisioned setup password). Admin token required.
 *
 * Persisted in NVS and applied to the running AP at once (its clients reconnect).
 *
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ap_set_handler(httpd_req_t *req);

/**
 * @brief The user's AP password from NVS (rbwifi/ap_pw_user).
 *
 * The hub never generates or changes this password; until the user sets one via POST /api/system/ap
 * the provisioned setup password (RBGRID_AP_SETUP_PASSWORD, ap_provision.h) is used.
 *
 * @param[out] out  Buffer for the password (NUL-terminated).
 * @param[in]  n    Size of @p out, bytes (at least 9).
 * @return true if a user password (≥ 8 characters) is stored; false = none, use the setup password.
 */
bool      rbgrid_ap_user_password(char *out, size_t n);

/**
 * @brief GET/POST/OPTIONS /api/system/units-config — measurement units. GET: no auth; POST: admin token
 *        (persisted; applies on restart except invert_sign, which applies live).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_units_config_handler(httpd_req_t *req);

/**
 * @brief DELETE /api/v1/units/{hexid} — remove a unit (its flash slot is erased; the bound sensor's config
 *        is kept). Admin token required. 404 unknown unit, 409 unit has children.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_unit_delete_handler(httpd_req_t *req);

/**
 * @brief POST /api/v1/units — bind a sensor to a NEW unit. Admin token required. 201 created,
 *        409 already bound, 400 bad request / sensor not found.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_unit_create_handler(httpd_req_t *req);

/**
 * @brief POST /api/v1/units/{hex}[/rebind|/rename] — patch unit parameters, move a unit onto another
 *        sensor (rebind) or change its point slug (rename). Admin token required.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_unit_mutate_handler(httpd_req_t *req);

/**
 * @brief GET /api/v1/sensors — configured sensors with interface, presence, unit binding and addressing.
 *        No auth. 503 + Retry-After until the home network has finished init.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_sensors_handler(httpd_req_t *req);

/**
 * @brief GET /api/v1/history?unit_id=&period=&type=&offset=&limit=[&from=&to=|&since=] — stored period
 *        records. Read token required when armed. 503 during a storage format migration, 404 unknown unit.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_history_handler(httpd_req_t *req);

/**
 * @brief GET /api/v1/economics?unit=\<hex\> — per-zone cost/revenue, net-metering credit bank and
 *        progressive-tier state of a unit. Read token required when armed.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_economics_handler(httpd_req_t *req);

/**
 * @brief GET /api/v1/export.csv?unit=&period=&type=&from=&to= — streamed CSV download of period records.
 *        Read token required when armed.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_export_csv_handler(httpd_req_t *req);

/**
 * @brief GET /api/v1/summary?unit=&period=&from=&to= — range totals (energy, cost, revenue) and per-zone
 *        distribution. Read token required when armed.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_summary_handler(httpd_req_t *req);

/**
 * @brief GET/POST/OPTIONS /api/system/mqtt-config — MQTT broker URI. GET: no auth (credentials masked);
 *        POST: admin token (persisted, applies on restart).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_mqtt_config_handler(httpd_req_t *req);

/**
 * @brief GET /api/v1/schema — machine-readable API contract descriptor (API version, endpoint surface,
 *        enums, limits). No auth.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_schema_handler(httpd_req_t *req);

/**
 * @brief GET/POST/OPTIONS /api/system/token — API token provisioning. GET: no auth (reports only whether
 *        tokens are set); POST {token, role?}: admin token once armed (the first token can be set while
 *        unarmed; "" clears).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_token_handler(httpd_req_t *req);

/**
 * @brief GET/POST/OPTIONS /api/system/backup — full-config backup slot (single slot). GET: no auth
 *        (slot info); POST: admin token (snapshot the current config).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_backup_handler(httpd_req_t *req);

/**
 * @brief POST/OPTIONS /api/system/restore — restore every module config file from the backup slot, then
 *        reboot. Admin token required. 404 if no backup.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_restore_handler(httpd_req_t *req);

/* Unified command dispatcher (MQTT command channel) — documented in rbwebserver.h. */
char *rbgrid_command_dispatch(const char* in_json, int in_len);

/**
 * @brief Write gate: require the ADMIN role (token-on-writes).
 *
 * OPTIONS always passes. With no admin token provisioned (onboarding) every request passes.
 * Tokens are taken from `Authorization: Bearer`, `X-API-Key` or `?token=`.
 *
 * @param[in] req  HTTP request.
 * @return true → proceed; false → a 401 (wrong/missing token), 403 (read token on a write) or
 *         429 (too many failed attempts) response has already been sent.
 */
bool rbapi_require_auth(httpd_req_t *req);

/**
 * @brief Data-read gate: require the READ or ADMIN role.
 *
 * Armed only when a read token is provisioned; without one, reads stay open even with an admin
 * token set (write-protection-only mode). OPTIONS always passes.
 *
 * @param[in] req  HTTP request.
 * @return true → proceed; false → a 401/429 response has already been sent.
 */
bool rbapi_require_read(httpd_req_t *req);

#define RBWEB_CORS_MAX_ORIGINS_PUB 8   ///< Maximum CORS allow-list entries (kept equal to RBWEB_CORS_MAX_ORIGINS in rbwebserver.c)

/**
 * @brief Reload the CORS/security policy from littlefs /sec_cfg (defaults: dev_cors = true, no origins).
 */
void rbwebserver_security_cfg_reload(void);

/**
 * @brief Read the current CORS policy (loads it on first use).
 * @param[out] dev_cors  Wildcard-CORS flag (may be NULL).
 * @param[out] origins   Array of at least @p max entries of 64 bytes for the allow-listed origins (may be NULL).
 * @param[in]  max       Capacity of @p origins.
 * @param[out] n         Number of origins copied (may be NULL).
 * @return true always.
 */
bool rbwebserver_security_cfg_get(bool *dev_cors, char origins[][64], int max, int *n);

/**
 * @brief GET/POST/OPTIONS /api/system/security — CORS policy (dev_cors flag + origin allow-list).
 *        Admin token required for GET and POST; POST persists /sec_cfg and reloads it live.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_api_security_handler(httpd_req_t *req);

/** @brief API contract version (reported by /api/v1/schema; checked by the ui-commit api_version guard). */
#define RBGRID_API_VERSION 1

/* Embedded web-UI hosting (rbwebserver_ui.c) */

/**
 * @brief Attach UI hosting to the `storage` littlefs mount (owned by the storage layer).
 *
 * Idempotent and safe to fail: without the mount the firmware stub page stays active and the
 * call is retried lazily.
 *
 * @retval ESP_OK                 UI storage available.
 * @retval ESP_ERR_INVALID_STATE  /storage is not mounted (yet).
 */
esp_err_t rbwebserver_ui_mount(void);

/**
 * @brief Check whether a web UI is installed (cur/index.html or .gz present). Retries the mount lazily.
 * @return true if an installed UI can be served.
 */
bool      rbwebserver_ui_installed(void);

/**
 * @brief GET / (station side, when a UI is installed) — serve the installed index.html. No auth.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ui_index_handler(httpd_req_t *req);

/**
 * @brief GET UI static files (/index.html and the /static/, /fonts/, /icons/, /lang/ prefixes …; registered after the API
 *        routes) — serve the installed UI, gzip variant first. No auth. AP-side clients are redirected to /wifi.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ui_file_handler(httpd_req_t *req);

/**
 * @brief POST /api/system/ui-upload?path= — stage one UI file (raw body). Admin token required.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ui_upload_handler(httpd_req_t *req);

/**
 * @brief POST /api/system/ui-commit — verify the staged files against the manifest (size + sha256) and
 *        swap them in atomically. Admin token required.
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ui_commit_handler(httpd_req_t *req);

/**
 * @brief GET/POST /api/system/lang[?code=xx] — the single stored language pack. GET: no auth (code and size);
 *        POST: admin token (body = pack JSON, replaces the previous pack).
 * @param[in] req  HTTP request.
 * @return ESP_OK when a response was sent normally; ESP_FAIL after an error response
 *         (4xx/5xx, including 401/403/429 from the token guard).
 */
esp_err_t rbwebserver_ui_lang_handler(httpd_req_t *req);

#endif /* RBWEBSERVER_INTERNAL_H */
