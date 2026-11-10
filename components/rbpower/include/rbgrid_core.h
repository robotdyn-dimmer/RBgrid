/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_core.h
 * @brief RBgrid core API for host transports (HTTP/WS/MQTT today, other hosts later).
 *
 * The core is metering, statistics, tariffs and configuration. The interfaces (web server, MQTT,
 * telemetry fan-out) call these functions and register hooks here; the core never calls an
 * interface directly. Every reply-returning call gives a malloc'd JSON string (caller frees) and,
 * where it can fail, an HTTP-style status code (200, 201, 400, 404, 409, 507) that any transport
 * can map.
 *
 * Threading: call from task context. Callbacks registered with rbgrid_core_register_3s_cb() run
 * on the esp_timer task: copy what you need or set a flag, never block or publish there.
 */
#ifndef RBGRID_CORE_H
#define RBGRID_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "rbpower.h"
#include "rbjson.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief REST/MQTT API contract version (/api/v1/schema `api_version`). Bump on a breaking change. */
#define RBGRID_API_VERSION 1

/* ---- Context ------------------------------------------------------------------------------- */

/**
 * @brief Give the core the configuration and JSON contexts the builders use.
 *
 * Called once by the application after both are created (before any transport starts).
 *
 * @param[in] ext_cfg   Extended configuration (kept by pointer).
 * @param[in] json_ctx  JSON device context (kept by pointer).
 */
void rbgrid_core_set_context(rbpower_extended_config_t *ext_cfg, rbgrid_json_context_t *json_ctx);
/** @brief Extended configuration set by rbgrid_core_set_context(). @return Pointer, or NULL before. */
rbpower_extended_config_t *rbgrid_core_ext_cfg(void);
/** @brief JSON device context set by rbgrid_core_set_context(). @return Pointer, or NULL before. */
rbgrid_json_context_t *rbgrid_core_json_ctx(void);

/* ---- Lifecycle hooks ----------------------------------------------------------------------- */

/** @brief 3-second period callback. @param period_end_us End of the closed 3 s period (esp_timer µs). */
typedef void (*rbgrid_3s_cb_t)(uint64_t period_end_us);
/**
 * @brief Register a callback for the end of every 3 s statistics period (telemetry publish point).
 *
 * Runs on the esp_timer task after the home lock is released. Registering the same function twice
 * is a no-op.
 *
 * @param[in] cb  Callback.
 * @return true if registered (or already registered), false if the table is full.
 */
bool rbgrid_core_register_3s_cb(rbgrid_3s_cb_t cb);
/** @brief Run the registered 3 s callbacks (called by the statistics engine). @param period_end_us End time. */
void rbgrid_core_run_3s_cbs(uint64_t period_end_us);

/**
 * @brief Register a function to run when a planned reboot starts (before flush and deinit).
 *
 * For transports that must say goodbye first, e.g. MQTT publishing its retained "offline".
 * Must be quick and must not block on the network.
 *
 * @param[in] fn  Function.
 * @return true if registered (or already registered), false if the table is full.
 */
bool rbgrid_core_register_reboot_notify(void (*fn)(void));
/** @brief Run the reboot notifiers (called by rbgrid_system_reboot() and the HTTP fallback path). */
void rbgrid_core_notify_reboot(void);

/**
 * @brief The host has decided the running image is good (OTA confirm, or a host's "boot is good").
 *
 * Releases work that waits for a confirmed image, such as a deferred statistics-format migration.
 * Idempotent.
 */
void rbgrid_core_image_confirmed(void);

/* ---- Factory reset ------------------------------------------------------------------------- */

/**
 * @brief Arm a factory reset for the next software restart.
 *
 * The marker lives in RTC no-init memory: it survives esp_restart(), not a power cut. The caller
 * restarts the hub (rbgrid_system_reboot()).
 *
 * @param[in] erase_history  true: also erase the statistics (history) partition.
 */
void rbgrid_core_factory_reset_arm(bool erase_history);
/**
 * @brief Carry out an armed factory reset. Call first thing at boot, before NVS and littlefs open.
 * @return true if a reset was carried out (the hub then starts as on first boot).
 */
bool rbgrid_core_factory_reset_at_boot(void);

/* ---- Configuration (transport-neutral) ------------------------------------------------------ */

/**
 * @brief Run one command: {"cmd":"get|set|history","section":…,"data":…,"id":…,"token":…}.
 * @param[in] in_json  Command JSON (not NUL-terminated necessarily).
 * @param[in] in_len   Length in bytes.
 * @return malloc'd reply JSON (caller frees), or NULL.
 */
char *rbgrid_command_dispatch(const char *in_json, int in_len);

/**
 * @brief Create a point bound to a sensor (POST /api/v1/units body).
 * @param[in]  json    Body JSON: {sensor:{iface,…}, name, role?, point?, reuse?, …}.
 * @param[out] status  201 created; 400 bad body / sensor not found; 409 bound / slug taken; 507 full.
 * @return malloc'd reply JSON (caller frees), or NULL on OOM.
 */
char *rbgrid_units_create(const char *json, int *status);
/**
 * @brief Change a point: params patch, rebind or rename.
 * @param[in]  uid     Point id.
 * @param[in]  op      NULL or "" = params patch; "rebind" {module,ch}; "rename" {point,reuse?}.
 * @param[in]  json    Body JSON.
 * @param[out] status  200 ok; 400 bad body/field; 404 unknown point; 409 conflict.
 * @return malloc'd reply JSON (caller frees), or NULL on OOM.
 */
char *rbgrid_units_mutate(rbpower_unit_id_t uid, const char *op, const char *json, int *status);
/** @brief Off: rbgrid_units_mutate() changes RAM only (boot re-apply of saved fields: the file already holds them,
 *  and per-patch saves would rewrite it from a half-restored state). Default on. */
void rbgrid_units_set_persist(bool on);
/**
 * @brief Delete a point (its history stays on flash, unreachable).
 * @param[in]  uid     Point id.
 * @param[out] status  200 removed; 400 id 0; 404 unknown; 409 has children.
 * @return malloc'd reply JSON (caller frees), or NULL on OOM.
 */
char *rbgrid_units_delete(rbpower_unit_id_t uid, int *status);

/* ---- Reads (transport-neutral) -------------------------------------------------------------- */

/** @brief /api/v1/schema JSON (API contract descriptor). @return malloc'd JSON, or NULL. */
char *rbgrid_build_schema_json(void);
/** @brief /api/v1/economics JSON for a point. @param unit Point id. @return malloc'd JSON, or NULL. */
char *rbgrid_build_economics_json(rbpower_unit_id_t unit);
/**
 * @brief /api/v1/summary JSON (range totals + per-zone money).
 * @param unit Point id. @param sp Stat period (rbgrid_period_str_to_stat). @param period_str "hour".."year".
 * @param from Window start, epoch s. @param to Window end, epoch s.
 * @return malloc'd JSON, or NULL.
 */
char *rbgrid_build_summary_json(rbpower_unit_id_t unit, int sp, const char *period_str, uint32_t from, uint32_t to);
/**
 * @brief Night-time standby baseline of a point, from a background scan (non-blocking).
 * @param[in] unit Point id. @param[out] base_w W or NAN. @param[out] hours Hours used. @param[out] nights Nights used.
 * @return true if a non-empty result for @p unit is available.
 */
bool rbgrid_base_w(rbpower_unit_id_t unit, float *base_w, int *hours, int *nights);

/* ---- Hooks main/ registers (ESP-NOW hub lives above this component) ------------------------- */

/** @brief Register the ESP-NOW status JSON builder (`cmd get espnow`). @param fn Builder. */
void rbgrid_register_espnow_status_provider(char *(*fn)(void));
/** @brief Register the ESP-NOW action handler (`cmd set espnow`). @param fn Handler. */
void rbgrid_register_espnow_action_handler(char *(*fn)(const cJSON *req, int *status));
/** @brief Register a function called after a point is deleted. @param fn Hook. */
void rbgrid_register_unit_deleted_hook(void (*fn)(uint16_t unit_id));

#ifdef __cplusplus
}
#endif

#endif /* RBGRID_CORE_H */
