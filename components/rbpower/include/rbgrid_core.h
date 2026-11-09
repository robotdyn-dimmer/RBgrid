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
#include "nvs.h"
#include "sdkconfig.h"

/* A build whose sdkconfig predates the RBgrid Kconfig keys would read every new bool key as "off" (ESP-IDF
 * defines only enabled bools) and silently drop features, e.g. SNTP. The string key is always defined in a
 * current config: stop the build instead. */
#ifndef CONFIG_RBGRID_NVS_PARTITION
#error "sdkconfig is older than components/rbpower/Kconfig: run `idf.py reconfigure` (or delete the build's sdkconfig)"
#endif

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

/* ---- NVS (RBgrid's own data) ------------------------------------------------------------------ */

/** @brief Label of the NVS partition for RBgrid data (CONFIG_RBGRID_NVS_PARTITION, default "nvs"). */
const char *rbgrid_nvs_partition(void);
/**
 * @brief Initialise RBgrid's NVS partition when it is not the default one (no-op for "nvs").
 *
 * A partition that is full or from a newer NVS version is erased and initialised again, as the default
 * partition is at boot. Safe to call more than once.
 *
 * @return ESP_OK, or the nvs_flash error.
 */
esp_err_t rbgrid_nvs_init(void);
/**
 * @brief nvs_open() on RBgrid's NVS partition. Use for all RBgrid data namespaces (not for Wi-Fi).
 * @param[in]  ns      Namespace.
 * @param[in]  mode    NVS_READONLY / NVS_READWRITE.
 * @param[out] handle  Handle.
 * @return As nvs_open_from_partition().
 */
esp_err_t rbgrid_nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *handle);

/** @brief MD5 of the running app image (hex, 32 chars; "" if unreadable). Computed once (~1 s), then cached. */
const char *rbgrid_core_running_image_md5(void);

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
 * @brief Run one command: {"cmd":"get|set|history|unit","section":…,"op":…,"unit":…,"data":…,"id":…,"token":…}.
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

/* ---- Snapshots for hosts (no JSON) --------------------------------------------------------- */

/** @brief System totals at the grid boundary (supply points only). */
typedef struct {
    float   p_w;            ///< Net active power, W (signed: import > 0)
    float   q_var;          ///< Net reactive power, var
    uint8_t direction;      ///< System flow direction (as telemetry `net_dir`)
    uint8_t units_valid;    ///< Configured points
    uint8_t units_present;  ///< Points with their source present
    double  e_in_wh;        ///< Lifetime import, Wh (non-decreasing)
    double  e_out_wh;       ///< Lifetime export, Wh (non-decreasing)
    double  e_day_in_wh;    ///< Today's import, Wh
    double  e_day_out_wh;   ///< Today's export, Wh
    double  cost_total, cost_day, rev_total, rev_day;   ///< Money, plan currency
} rbgrid_system_totals_t;

/**
 * @brief System totals, the same figures as telemetry `system{}`.
 * @param[out] t            Totals.
 * @param[in]  with_energy  Also the lifetime/day energy and money (a little more work).
 * @note Call with rbpower_home_lock() held.
 */
void rbgrid_core_system_totals(rbgrid_system_totals_t *t, bool with_energy);

/** @brief One point's live values and lifetime energy. */
typedef struct {
    uint16_t unit_id;       ///< Point id
    uint16_t inside;        ///< Point it is measured inside (0 = top level)
    uint8_t  role;          ///< rbpower_load_role_t
    bool     present;       ///< Source present (module / node heard)
    bool     estimated;     ///< Energy is a host estimate (current-only source)
    char     point[33];     ///< Point slug
    char     name[32];      ///< Display name
    float    p_w;           ///< Active power, W (signed)
    float    i_a;           ///< RMS current, A
    float    v_v;           ///< RMS voltage, V (0 = unknown)
    double   e_in_wh;       ///< Lifetime energy in, Wh
    double   e_out_wh;      ///< Lifetime energy out, Wh
    double   cost_total;    ///< Lifetime cost of the energy in, plan currency
    double   rev_total;     ///< Lifetime value of the energy out, plan currency
} rbgrid_point_snap_t;

/**
 * @brief Snapshot of every point (the voltage bus excluded), in point-list order.
 * @param[out] out  Array of at least @p max entries.
 * @param[in]  max  Capacity.
 * @return Number of entries written.
 * @note Call with rbpower_home_lock() held; copy, release the lock, then publish.
 */
int rbgrid_core_points_snapshot(rbgrid_point_snap_t *out, int max);

/** @brief The tariff in force now (as telemetry `system.tariff{}`). */
typedef struct {
    char   zone[16];          ///< Active zone name ("" = no active plan)
    float  buy_rate;          ///< Zone buy rate, currency/kWh
    float  sell_rate;         ///< Zone sell (feed-in) rate, currency/kWh
    double buy_rate_all_in;   ///< Buy rate with per-kWh fees and VAT (the basis of the cost figures)
    char   currency[8];       ///< Plan currency (ISO code)
    bool   placeholder;       ///< Money is computed off the built-in demo plan (no plan saved yet)
    bool   time_valid;        ///< The wall clock is set (zones need it)
    bool   external;          ///< A dynamic price covers now (buy/sell come from it, not the plan zone)
    char   price_state[16];   ///< Dynamic price state code: ok|backup|today_only|stale|plan_fallback|disabled
    char   price_source[28];  ///< Provider in use now ("plan" if none), "(backup)" appended for the backup
    uint32_t price_valid_until; ///< End of the continuous dynamic-price coverage from now (UTC s), 0 = none
} rbgrid_tariff_now_t;

/**
 * @brief The tariff in force now. Cheap; no lock needed.
 * @param[out] t  Tariff now.
 */
void rbgrid_core_tariff_now(rbgrid_tariff_now_t *t);

/**
 * @brief Version of the active configuration (points + tariff plan): CRC32, 8 hex digits.
 * @param[out] out  9 bytes.
 * @note Builds the two documents (a few KB); call rarely. Takes the home lock itself: do not hold it.
 */
void rbgrid_core_config_version(char out[9]);

/* ---- Hooks main/ registers (ESP-NOW hub lives above this component) ------------------------- */

/** @brief Register the ESP-NOW status JSON builder (`cmd get espnow`). @param fn Builder. */
void rbgrid_register_espnow_status_provider(char *(*fn)(void));
/** @brief Register the I2C fleet action handler (`cmd get i2c` / `set i2c`). @param fn Handler. */
void rbgrid_register_i2c_action_handler(char *(*fn)(const cJSON *req, int *status));
/** @brief Register the ESP-NOW action handler (`cmd set espnow`). @param fn Handler. */
void rbgrid_register_espnow_action_handler(char *(*fn)(const cJSON *req, int *status));
/** @brief Register a function called after a point is deleted. @param fn Hook. */
void rbgrid_register_unit_deleted_hook(void (*fn)(uint16_t unit_id));

#ifdef __cplusplus
}
#endif

#endif /* RBGRID_CORE_H */
