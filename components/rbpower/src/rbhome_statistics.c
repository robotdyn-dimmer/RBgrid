/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_statistics.c
 * @brief rbpower statistics system: period accumulation, hierarchy rollup (15 min -> hour ->
 *        day/week/month/year), calendar buckets and flash persistence of period records.
 *
 * Runs the statistics task (period events from rbhome_timers.c) and the flash-recompute path for
 * DAY and longer periods. Integrates with rbtime for wall-clock alignment. Shared state
 * (g_home_network, g_period_accumulators) is accessed under the home lock.
 *
 * @version 1.0.0
 * @date 2025-01-10
 */

#include "nvs.h"            /* open-HOUR accumulator persist/restore across reboots */
#include "rbpower.h"
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbhome_types.h"
#include "rbhome_main_supply.h"
#include "rbhome_alt_supply.h"
#include "rbpower_lifetime_api.h"
#include "rbextflash_buffer_write.h"
#include "rbextflash_operations.h"
#include "rbstorage_transfer_types.h"  // For rbpower_electrical_transfer_t, rbpower_energy_transfer_t, etc.
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR — place g_period_accumulators in PSRAM */
#include "esp_system.h"
#include <sys/time.h>

//=============================================================================
// PRIVATE VARIABLES
//=============================================================================

/** Global statistics system state */
rbpower_statistics_system_t g_stats_system = {0};

/** Accumulators for all units and periods (private). EXT_RAM_BSS_ATTR → PSRAM: this is the
 *  heaviest per-unit array (560 B × periods × units) and scales with the unit count; cold
 *  data (touched at period boundaries), safe in PSRAM. */
// 2nd dim = RBPOWER_RAM_PERIOD_COUNT (3), NOT RBPOWER_STAT_PERIOD_COUNT (7): only 3SEC/15MIN/1HOUR
// are RAM-accumulated; DAY..YEAR are flash-recompute (no accumulator). Reclaims ~144KB PSRAM at 64-unit
// max scale. Index ONLY with 0..RBPOWER_RAM_PERIOD_COUNT-1 (= 3SEC/15MIN/1HOUR). See rbpower_enums.h.
EXT_RAM_BSS_ATTR rbpower_period_accumulator_t g_period_accumulators[RBPOWER_MAX_HOME_UNITS][RBPOWER_RAM_PERIOD_COUNT];

/* Experimental 4-flow scaffold — merit-order source attribution (PV→load→battery→grid). Pure: no
 * globals, no side effects. Degenerate input (no PV, no battery) → grid_to_load = p_load, everything
 * else 0 (= the 2-quadrant "all grid" model). Declared in rbhome_types.h. */
void rbpower_compute_energy_balance(const rbpower_energy_balance_input_t* in, rbpower_energy_4flow_t* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!in) return;

    // Clamp to the physical domain: PV/load are non-negative magnitudes; battery split into a
    // non-negative charge magnitude (p_batt > 0) and discharge magnitude (p_batt < 0).
    float p_pv   = (in->p_pv   > 0.0f) ? in->p_pv   : 0.0f;
    float p_load = (in->p_load > 0.0f) ? in->p_load : 0.0f;
    float batt_charge    = (in->p_batt > 0.0f) ? in->p_batt  : 0.0f;
    float batt_discharge = (in->p_batt < 0.0f) ? -in->p_batt : 0.0f;

    // PV serves load first (self-consumption), surplus charges the battery, remainder exports.
    float pv_to_load     = (p_pv < p_load) ? p_pv : p_load;
    float pv_surplus     = p_pv - pv_to_load;
    float charge_from_pv = (pv_surplus < batt_charge) ? pv_surplus : batt_charge;
    float pv_to_grid     = pv_surplus - charge_from_pv;
    float charge_from_grid = batt_charge - charge_from_pv;

    // Remaining load is covered by battery discharge first, then grid import.
    float load_deficit      = p_load - pv_to_load;
    float discharge_to_load = (batt_discharge < load_deficit) ? batt_discharge : load_deficit;
    float discharge_to_grid = batt_discharge - discharge_to_load;
    float grid_to_load      = load_deficit - discharge_to_load;

    out->pv_to_load        = pv_to_load;
    out->pv_to_grid        = pv_to_grid;
    out->charge_from_pv    = charge_from_pv;
    out->charge_from_grid  = charge_from_grid;
    out->discharge_to_load = discharge_to_load;
    out->discharge_to_grid = discharge_to_grid;
    out->grid_to_load      = grid_to_load;
    out->self_consumption_ratio = (p_load > 0.0f)
        ? (pv_to_load + discharge_to_load) / p_load : 0.0f;
}

/* 4-flow scaffold — rbpower_gather_system_balance_input lives in rbhome.c (where g_home_network is
 * defined); the pure rbpower_compute_energy_balance above takes its result. */

/** Callback subscription registry */
rbpower_callback_registry_t g_callback_registry = {0};

/** Initialization flag */
bool g_statistics_initialized = false;

/** Storage system enabled flag (set from configuration) */
static bool g_storage_enabled = false;

//=============================================================================
// ASYNCHRONOUS FLASH WRITE SYSTEM
//=============================================================================

/** @brief Request queued to the asynchronous flash writer. */
typedef struct {
    rbpower_unit_id_t unit_id;                ///< Unit ID
    rbpower_stat_period_type_t period_type;   ///< Period type
    rbpower_period_statistics_t period_stats; ///< Finalized period statistics
    uint64_t timestamp_us;                    ///< Request time, µs since boot
} flash_write_request_t;

/** Queue for asynchronous flash write */
#define FLASH_WRITE_QUEUE_SIZE 10
static QueueHandle_t g_flash_write_queue = NULL;

/** Task handle for flash write */
static TaskHandle_t g_flash_write_task_handle = NULL;

/** Flash write task active flag */
static bool g_flash_write_task_running = false;

/** @brief Global home network (rbhome.c). */
extern rbpower_electrical_network_t g_home_network;
extern bool g_home_network_initialized; ///< Network initialization flag (rbhome.c)
//=============================================================================
// PRIVATE FUNCTIONS - TASKS
static void rbpower_statistics_task(void* pvParameters);    
static void finalize_15min_period(rbpower_unit_id_t unit_id);
void rbpower_finalize_period_callback(rbpower_unit_id_t unit_id, 
                                     rbpower_stat_period_type_t period_type, 
                                     uint64_t period_end_time_us);
static void aggregate_1hour_from_15min_period(rbpower_unit_id_t unit_id,
                                            const rbpower_period_statistics_t* completed_15min_period);
static void finalize_1hour_period(rbpower_unit_id_t unit_id);
static void hour_persist(uint8_t unit_idx);          /* open-HOUR accumulator NVS image */
static uint32_t s_store_ts_override = 0;             /* record ts to stamp instead of "now" (past-hour image write) */
static rbpower_stat_period_type_t s_store_ts_override_period = RBPOWER_STAT_PERIOD_COUNT;   /* ...only for THIS period (a HOUR store inside a DAY recompute must not inherit it) */
static void hour_persist_erase(uint8_t unit_idx);
static void hour_restore_once(uint8_t unit_idx);
static uint32_t s_hour_of_acc[RBPOWER_MAX_HOME_UNITS];   /* hour start (unix s) the open 1HOUR accumulator belongs to (0 = by clock) */
static void rollup_dispatch_buckets(rbpower_unit_id_t unit_id);
static void rbpower_stats_service_tick(void);
static void finalize_1day_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket);
static void finalize_1week_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket);
static void finalize_1month_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket);
static void finalize_1year_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket);
/* Non-static: shared by finalize_15min_period (native) and the rbAmp inject path. */
bool rbpower_rollup_after_15min(rbpower_unit_id_t unit_id,
                                const rbpower_period_statistics_t* stats_15min);

// Period storage write function (synchronous - for internal use)
static void store_period_to_flash_sync(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type,
                                       const rbpower_period_statistics_t* period_stats);

// LEGACY: Asynchronous system for LittleFS (NO LONGER USED)
// static void flash_write_task(void* pvParameters);
// static esp_err_t store_period_to_flash(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type,
//                                        const rbpower_period_statistics_t* period_stats);
//=============================================================================
// PRIVATE FUNCTIONS - UTILITIES
//=============================================================================

/**
 * @brief Get current system time in microseconds
 */
static uint64_t get_system_time_us(void)  // !!! move to rbpower system
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
    }
    return esp_timer_get_time(); // Fallback to uptime
}


/**
 * @brief Check system time readiness
 */
static bool is_system_time_ready(void)
{
    if (!rbtime_is_time_valid()) {
        ESP_LOGD("RBHOME_STATS", "System time not ready");
        return false;
    }

    uint64_t unix_time = rbtime_get_unix_timestamp();
    if (unix_time < 1609459200) { // January 1, 2021 00:00:00 UTC
        ESP_LOGD("RBHOME_STATS", "System time invalid: %llu", unix_time);
        return false;
    }

    return true;
}

//=============================================================================
// PRIVATE FUNCTIONS - ACCUMULATORS
//=============================================================================

/**
 * @brief Initialize period accumulator
 */
static void init_period_accumulator(rbpower_period_accumulator_t* acc, 
                                   rbpower_stat_period_type_t period_type)
{
    memset(acc, 0, sizeof(rbpower_period_accumulator_t));
    
    acc->period_type = period_type;
    acc->is_active = false;
    acc->is_enabled = false;

    // Initialize extremes to "invalid" state
    acc->extremes.voltage_min = INFINITY;
    acc->extremes.voltage_max = -INFINITY;
    acc->extremes.current_min = INFINITY;
    acc->extremes.current_max = -INFINITY;
    acc->extremes.power_min = INFINITY;
    acc->extremes.power_max = -INFINITY;
    acc->extremes.extremes_initialized = false;
}

/**
 * @brief Reset an accumulator to the start of a new period (also used by rbhome.c).
 * @param[in,out] acc Accumulator (NULL is ignored).
 */
void reset_period_accumulator(rbpower_period_accumulator_t* acc)
{
    if (!acc) return;

    rbpower_stat_period_type_t period_type = acc->period_type;
    bool is_enabled = acc->is_enabled;

    // Save important parameters
    init_period_accumulator(acc, period_type);
    acc->is_enabled = is_enabled;
    acc->is_active = true;
    //acc->economics = saved_economics; // Restore economics

    // Set period start time
    acc->time_boundaries.period_start_us = get_system_time_us();
    acc->time_boundaries.expected_duration_ms = RBPOWER_GET_PERIOD_DURATION_US(period_type) / 1000;

    //ESP_LOGD("RBHOME_STATS", "Period %d accumulator reset", period_type);
}


//=============================================================================
// PUBLIC API FUNCTIONS
//=============================================================================

rbpower_err_t rbpower_statistics_init(const rbpower_statistics_global_config_t* config)
{
    if (g_statistics_initialized) {
        ESP_LOGD("RBHOME_STATS", "Statistics system already initialized");
        return RBPOWER_ERR_ALREADY_INIT;
    }

    if (!config) {
        ESP_LOGE("RBHOME_STATS", "Statistics configuration not specified");
        return RBPOWER_ERR_INVALID_ARG;
    }

    ESP_LOGI("RBHOME_STATS", "=== Initializing rbpower statistics system ===");

#if RBGRID_STATS_TIME_SCALE > 1
    /* 🐛 Accelerated stats test build: periods fire RBGRID_STATS_TIME_SCALE× more often, so the
     * per-finalize INFO logs (15-min/hour/day chatter) would flood the UART and burn CPU on a fast
     * loop. Raise the stats tags to WARN — errors still surface, but the routine finalize lines go
     * quiet. Verify period progression via the pstat / statcursor diagnostic verbs (state queries), not
     * logs. Per-record ESP_LOGD loops are already compiled out at the INFO build level (zero cost) —
     * just don't build this at DEBUG log level. */
    ESP_LOGW("RBHOME_STATS", "⏩ ACCELERATED STATS TEST MODE: time scale ×%d (NOT for production!) — "
             "stats logs quieted to WARN; check progress via pstat/statcursor", RBGRID_STATS_TIME_SCALE);
    esp_log_level_set("RBHOME-AGG",        ESP_LOG_WARN);
    esp_log_level_set("RBHOME-AGG-DAY",    ESP_LOG_WARN);
    esp_log_level_set("RBHOME-AGG-WEEK",   ESP_LOG_WARN);
    esp_log_level_set("RBHOME-AGG-MONTH",  ESP_LOG_WARN);
    esp_log_level_set("RBHOME-AGG-YEAR",   ESP_LOG_WARN);
    esp_log_level_set("RBHOME_STATS_TASK", ESP_LOG_WARN);
#endif

    // Set global storage flag from configuration
    g_storage_enabled = config->storage_enabled;
    ESP_LOGI("RBHOME_STATS", "Storage system: %s", g_storage_enabled ? "ENABLED" : "DISABLED");
    
    // Start statistics with fallback time, resynchronize when SNTP received
    bool time_ready = is_system_time_ready();
    if (!time_ready) {
        ESP_LOGW("RBHOME_STATS", "System time not synchronized - starting with fallback time");
        ESP_LOGD("RBHOME_STATS", "Resynchronization will be performed automatically when SNTP received");
    }

    // Clear global state
    memset(&g_stats_system, 0, sizeof(rbpower_statistics_system_t));
    memset(&g_period_accumulators, 0, sizeof(g_period_accumulators));
    memset(&g_callback_registry, 0, sizeof(rbpower_callback_registry_t));

    // Copy configuration
    memcpy(&g_stats_system.config, config, sizeof(rbpower_statistics_global_config_t));

    // Initialize accumulators for all units
    for (uint8_t unit_id = 0; unit_id < RBPOWER_MAX_HOME_UNITS; unit_id++) {
        for (uint8_t period = 0; period < RBPOWER_RAM_PERIOD_COUNT; period++) {   // only RAM-accumulated slots exist
            init_period_accumulator(&g_period_accumulators[unit_id][period],
                                   (rbpower_stat_period_type_t)period);
        }
    }

    // Initialize time context
    g_stats_system.time_context.system_start_time_us = get_system_time_us();
    g_stats_system.time_context.last_ntp_sync_time_us = rbtime_get_unix_timestamp() * 1000000ULL;
    g_stats_system.time_context.sync_interval_hours = config->timer_sync_interval_hours;
    
    // Create FreeRTOS queues
    if (config->statistics_enabled) {
        g_stats_system.statistics_event_queue = xQueueCreate(
            config->statistics_queue_size,
            sizeof(rbpower_timer_event_context_t)
        );

        g_stats_system.callback_event_queue = xQueueCreate(
            config->callback_queue_size,
            sizeof(rbpower_callback_event_t)
        );

        if (!g_stats_system.statistics_event_queue || !g_stats_system.callback_event_queue) {
            ESP_LOGE("RBHOME_STATS", "Failed to create statistics queues");
            return RBPOWER_ERR_NO_MEMORY;
        }

        ESP_LOGD("RBHOME_STATS", "Statistics queues created (stats: %d, callbacks: %d)",
                 config->statistics_queue_size, config->callback_queue_size);
    }

    // Set state flags
    g_stats_system.system_initialized = true;
    g_stats_system.timers_running = false;
    g_stats_system.time_synchronized = time_ready;

    g_statistics_initialized = true;

    ESP_LOGI("RBHOME_STATS", "Statistics system initialized | enabled=%s, time_sync=%s, start=%llu",
             config->statistics_enabled ? "YES" : "NO",
             g_stats_system.time_synchronized ? "YES" : "NO (fallback)",
             g_stats_system.time_context.system_start_time_us);
    
// === CREATE STATISTICS TASK ===
    if (config->statistics_enabled) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            rbpower_statistics_task,
            "rbpwr_stats",
            config->statistics_task_stack_size,
            NULL,
            config->statistics_task_priority,
            &g_stats_system.statistics_task_handle,
            config->statistics_task_core
        );

        if (task_ret != pdPASS) {
            ESP_LOGE("RBHOME_STATS_TASK", "Failed to create Statistics Task");
            return RBPOWER_ERR_NO_MEMORY;
        }

        // Register task in watchdog to prevent warnings with esp_task_wdt_reset()
        esp_err_t wdt_ret = esp_task_wdt_add(g_stats_system.statistics_task_handle);
        if (wdt_ret == ESP_OK) {
            ESP_LOGD("RBHOME_STATS_TASK", "Statistics Task registered in watchdog");
        } else if (wdt_ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW("RBHOME_STATS_TASK", "Watchdog not initialized, registration skipped");
        } else {
            ESP_LOGW("RBHOME_STATS_TASK", "Failed to register task in watchdog: %d", wdt_ret);
        }

        ESP_LOGI("RBHOME_STATS_TASK", "Statistics Task created (core=%d, priority=%d, stack=%d)",
                 config->statistics_task_core, config->statistics_task_priority, config->statistics_task_stack_size);
    }

    return RBPOWER_OK;
}

rbpower_err_t rbpower_statistics_get_system_state(rbpower_statistics_system_t** system_state)
{
    if (!system_state) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    *system_state = &g_stats_system;
    return RBPOWER_OK;
}

rbpower_err_t rbpower_statistics_print_diagnostics(void)
{
    if (!g_statistics_initialized) {
        ESP_LOGI("RBHOME_STATS", "Statistics system not initialized");
        return RBPOWER_ERR_NOT_INIT;
    }

    uint64_t current_time = get_system_time_us();
    uint64_t uptime_hours = (current_time - g_stats_system.time_context.system_start_time_us) / 3600000000ULL;

    ESP_LOGI("RBHOME_STATS", "=== DIAGNOSTICS === Init=%s Timers=%s TimeSync=%s | Events: proc=%lu drop=%lu err=%lu mem=%lu | Callbacks: active=%d calls=%lu drop=%lu | Uptime=%lluh",
             g_stats_system.system_initialized ? "Y" : "N",
             g_stats_system.timers_running ? "Y" : "N",
             g_stats_system.time_synchronized ? "Y" : "N",
             g_stats_system.global_stats.total_events_processed,
             g_stats_system.global_stats.events_dropped,
             g_stats_system.global_stats.timer_errors,
             g_stats_system.global_stats.memory_warnings,
             g_callback_registry.subscription_count,
             g_callback_registry.total_calls,
             g_callback_registry.dropped_calls,
             uptime_hours);

    return RBPOWER_OK;
}

bool rbpower_statistics_is_initialized(void)
{
    return g_statistics_initialized;
}

/* External period-boundary hook (e.g. rbAmp collects its own period at the 15-min
 * boundary). Set from the app layer; called in the statistics task on PERIOD_END. */
static rbpower_period_boundary_cb_t s_period_boundary_cb = NULL;
void rbpower_register_period_boundary_cb(rbpower_period_boundary_cb_t cb)
{
    s_period_boundary_cb = cb;
}

/**
 * @brief Main statistics event processing task
 */
static void rbpower_statistics_task(void* pvParameters)
{
    rbpower_timer_event_context_t event;

    ESP_LOGI("RBHOME_STATS_TASK", "Statistics Task started on core %d", xPortGetCoreID());

    while (1) {
        // Wait for events from queue
        BaseType_t got_event = xQueueReceive(g_stats_system.statistics_event_queue, &event, pdMS_TO_TICKS(1000));
        rbpower_stats_service_tick();   /* open-hour restore at boot + hour boundary timeout (rate-limited, 1 s) */
        if (got_event != pdTRUE) {
            /* No period event for a second. The 3-s chain runs on the esp_timer task under the home
             * lock, so a long console/flash operation holding that lock stalls the chain and this
             * task saw no event — it is idle, not hung, yet the TWDT (5 s) only ever got fed from
             * the event path and would reset the hub with both cores idle. Feed it here; a stalled
             * chain is reported as a #DIAG
             * FAIL line when events resume (below) instead of a reset. */
            esp_task_wdt_reset();
            continue;
        }
        {
            /* Event after a long silence = the chain WAS stalled; say so once per stall. */
            int64_t gap_us = esp_timer_get_time() - (int64_t)g_stats_system.global_stats.last_activity_time_us;
            if (g_stats_system.global_stats.last_activity_time_us && gap_us > 10000000LL) {
                rbpower_diag_printf("FAIL stats: period events paused for %llds (3-s chain stalled: home lock held?)",
                                    (long long)(gap_us / 1000000LL));
            }

            // Update statistics
            g_stats_system.global_stats.total_events_processed++;
            g_stats_system.global_stats.last_activity_time_us = esp_timer_get_time();

            // Process event by type
            switch (event.event_type) {
                case RBPOWER_TIMER_EVENT_PERIOD_END:
                    ESP_LOGD("RBHOME_STATS_TASK", "PERIOD_END event: period=%d, timestamp=%llu",
                             event.period_type, event.event_timestamp_us);

                    // === SIMPLE PROCESSING WITHOUT BATCHING ===
                    // Batching no longer needed - only 2 timers (3SEC and 15MIN) never fire simultaneously

                    // Process period for all units
                    if (event.period_type > RBPOWER_STAT_PERIOD_3SEC) {
                        // Finalize current period for all units
                        for (uint8_t unit_id = 0; unit_id < g_home_network.units_count; unit_id++) {
                            if (g_home_network.units[unit_id].has_valid_definition &&
                                g_home_network.units[unit_id].definition.is_active &&
                                g_home_network.units[unit_id].hardware_present) {  // skip
                                // units whose bound sensor is absent (no wasted compute; dormant in config).

                                // Call finalization function from rbhome_statistics.c
                                rbpower_finalize_period_callback(unit_id, event.period_type, g_stats_system.global_stats.last_activity_time_us);
                            }
                        }

                        // The main/alt-supply units are ALREADY regular
                        // entries in g_home_network.units (role MAIN/ALT_SUPPLY), so the per-unit loop
                        // above already produced their real period records. The old
                        // finalize_main/alt_supply_period() was a SECOND parallel finalize writing only
                        // god-struct result_* fields that nothing reads (main flash-save commented out,
                        // alt flash-save a stub) — pure redundant processing. Removed. The 3SEC path
                        // (start/finalize/accumulate) that feeds telemetry (statistics_3sec /
                        // result_base_3sec / lifetime_accumulators) is separate and untouched.

                        ESP_LOGI("RBHOME_STATS_TASK", "Period %d completed for %d units", event.period_type, g_home_network.units_count);

                        // External sources (rbAmp) collect their own period at this same
                        // boundary, so device/host period results land synchronized with
                        // the native hierarchy.
                        if (s_period_boundary_cb) {
                            rbpower_stats_stage("boundary-cb");
                            s_period_boundary_cb(event.period_type, event.event_timestamp_us);
                            rbpower_stats_stage("idle");
                        }
                    }
                    break;

                case RBPOWER_TIMER_EVENT_PERIOD_START:
                    ESP_LOGD("RBHOME_STATS_TASK", "Processing period %d start", event.period_type);
                    break;

                default:
                    ESP_LOGW("RBHOME_STATS_TASK", "Unknown event type: %d", event.event_type);
                    break;
            }

            // Periodic diagnostics (every 10 events)
            if (g_stats_system.global_stats.total_events_processed % 10 == 0) {
                UBaseType_t queue_free = uxQueueSpacesAvailable(g_stats_system.statistics_event_queue);
                ESP_LOGD("RBHOME_STATS_TASK", "Events processed: %lu, queue free: %d, event type: %d",
                         g_stats_system.global_stats.total_events_processed, (int)queue_free, event.event_type);
            }

            // Reset watchdog after event processing (protection from timeout on long operations)
            esp_task_wdt_reset();
        }
    }
}

//=============================================================================
// PERIOD AGGREGATION - BASE FUNCTIONS
//=============================================================================

/* === PERIOD DESCRIPTOR TABLE — single source of truth for the period hierarchy ===
 * Each statistical period is described ONCE here; the engine reads the row instead of
 * branching by type. This replaces the per-period switch/if logic that was duplicated across
 * init_aggregation_period (expected-count switch), the finalize_* family, and the rollup
 * boundary cascade. Fields:
 *   parent            — period this rolls UP into (COUNT = top of the chain, no parent)
 *   flash_period      — matching rbpower_period_t flash buffer (MAX = RAM-only, not persisted)
 *   children_expected — input periods per finalize (drives completeness %); = the old
 *                       init_aggregation_period switch values (behavior-identical)
 *   ready_threshold   — input periods an auto-finalize needs before it may STORE a partial
 *                       (HOUR=3 → 75%; others=1). The test _forced path overrides this.
 *   ram_accumulated   — true: filled live via aggregate_* into g_period_accumulators;
 *                       false: recomputed at finalize from child records read back from flash.
 * NOTE: flash_period for 1HOUR..YEARLY equals (stat_period - 2) by enum design; the table makes
 * that mapping explicit and centralized instead of scattering "- 2" / hardcoded RBPOWER_PERIOD_*.
 */
/** @brief Descriptor of one statistics period (see the table comment above). */
typedef struct {
    rbpower_stat_period_type_t parent;            ///< Next period in the hierarchy
    rbpower_period_t           flash_period;      ///< Flash buffer period; RBPOWER_PERIOD_MAX = RAM-only (not persisted)
    uint16_t                   children_expected; ///< Input periods per finalize
    uint16_t                   ready_threshold;   ///< Input periods an auto-finalize needs to store
    bool                       ram_accumulated;   ///< true: RAM accumulator; false: flash recompute
} rbpower_period_desc_t;

static const rbpower_period_desc_t PERIODS[RBPOWER_STAT_PERIOD_COUNT] = {
    [RBPOWER_STAT_PERIOD_3SEC]    = { RBPOWER_STAT_PERIOD_15MIN,   RBPOWER_PERIOD_MAX,   1,   1, true  },
    [RBPOWER_STAT_PERIOD_15MIN]   = { RBPOWER_STAT_PERIOD_1HOUR,   RBPOWER_PERIOD_MAX,   300, 1, true  },
    [RBPOWER_STAT_PERIOD_1HOUR]   = { RBPOWER_STAT_PERIOD_DAILY,   RBPOWER_PERIOD_HOUR,  4,   3, true  },
    [RBPOWER_STAT_PERIOD_DAILY]   = { RBPOWER_STAT_PERIOD_WEEKLY,  RBPOWER_PERIOD_DAY,   24,  1, false },
    [RBPOWER_STAT_PERIOD_WEEKLY]  = { RBPOWER_STAT_PERIOD_MONTHLY, RBPOWER_PERIOD_WEEK,  7,   1, false },
    [RBPOWER_STAT_PERIOD_MONTHLY] = { RBPOWER_STAT_PERIOD_YEARLY,  RBPOWER_PERIOD_MONTH, 30,  1, false },
    [RBPOWER_STAT_PERIOD_YEARLY]  = { RBPOWER_STAT_PERIOD_COUNT,   RBPOWER_PERIOD_YEAR,  12,  1, false },
};

/**
 * @brief Check if aggregation to target period is needed
 */
static bool should_aggregate_to_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t target_period)
{
    // RAM-accumulator gate — only valid for the 3 RAM-accumulated periods (called only with
    // 15MIN/1HOUR). day+ are flash-recompute (no accumulator) → reject (also keeps the array access safe).
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || target_period >= RBPOWER_RAM_PERIOD_COUNT) {
        return false;
    }
    //ESP_LOGD("RBHOME-AGG", "Aggregate check Unit[%d]: Period: %d, enabled: %d", unit_id, target_period, g_period_accumulators[unit_id][target_period].is_enabled);
    // Check that target period is enabled for this unit
    if (!g_period_accumulators[unit_id][target_period].is_enabled) {
        return false;
    }

    return true;
}

/**
 * @brief Initialize new aggregation period
 */
static void init_aggregation_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || period_type >= RBPOWER_RAM_PERIOD_COUNT) {  // RAM-accumulated only
        return;
    }

    rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period_type];

    if (!acc->is_enabled) {
        return;
    }

    // Reset accumulator for new period
    reset_period_accumulator(acc);
    acc->is_active = true;

    // Expected number of input periods — from the descriptor table (was a per-type switch).
    acc->metadata.input_periods_expected = PERIODS[period_type].children_expected;

    acc->metadata.input_periods_received = 0;
    acc->metadata.ready_for_finalization = false;
    acc->metadata.aggregation_iteration++;

    ESP_LOGD("RBHOME-AGG", "Unit[%d]: Period %d started (iteration #%lu, expecting %lu input periods)",
             unit_id, period_type, acc->metadata.aggregation_iteration, acc->metadata.input_periods_expected);
}

/**
 * @brief Aggregate 15-minute period from completed 3-second period
 */
static void aggregate_15min_from_3sec_period(rbpower_unit_id_t unit_id, 
                                           const rbpower_period_statistics_t* completed_3sec_period)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || !completed_3sec_period) {
        return;
    }
    
    rbpower_period_accumulator_t* acc_15min = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_15MIN];

    if (!acc_15min->is_enabled) {
        return;
    }

    // Initialize new 15-min period on first 3-sec period
    if (!acc_15min->is_active) {
        init_aggregation_period(unit_id, RBPOWER_STAT_PERIOD_15MIN);
        acc_15min->time_boundaries.period_start_us = completed_3sec_period->period_start_us;
    }

    // === RMS AGGREGATION (root mean square) ===
    if (completed_3sec_period->voltage_rms > 0) {
        acc_15min->electrical.voltage_rms_sum_squares += 
            completed_3sec_period->voltage_rms * completed_3sec_period->voltage_rms;
    }
    if (completed_3sec_period->current_rms >= 0) {
        acc_15min->electrical.current_rms_sum_squares += 
            completed_3sec_period->current_rms * completed_3sec_period->current_rms;
    }
    acc_15min->electrical.rms_samples_count++;

    // === POWER AGGREGATION (linear) ===
    acc_15min->electrical.power_active_sum += completed_3sec_period->power_active_avg;
    acc_15min->electrical.power_reactive_sum += completed_3sec_period->power_reactive_avg;
    acc_15min->electrical.power_apparent_sum += completed_3sec_period->power_apparent_avg;
    acc_15min->electrical.power_factor_sum += completed_3sec_period->power_factor_avg;
    acc_15min->electrical.pf_samples_count++;
    acc_15min->electrical.power_samples_count++;

    // === ENERGY AGGREGATION (direct summation) ===
    acc_15min->energy.consumed_wh_accumulator += completed_3sec_period->energy_period.consumed_wh;
    acc_15min->energy.supplied_wh_accumulator += completed_3sec_period->energy_period.supplied_wh;

    // === EXTREMES AGGREGATION (global min/max) ===
    if (!acc_15min->extremes.extremes_initialized) {
        acc_15min->extremes.voltage_min = completed_3sec_period->extremes.voltage_min;
        acc_15min->extremes.voltage_max = completed_3sec_period->extremes.voltage_max;
        acc_15min->extremes.current_min = completed_3sec_period->extremes.current_min;
        acc_15min->extremes.current_max = completed_3sec_period->extremes.current_max;
        acc_15min->extremes.power_min = completed_3sec_period->extremes.power_min;
        acc_15min->extremes.power_max = completed_3sec_period->extremes.power_max;
        acc_15min->extremes.extremes_initialized = true;
    } else {
        // Update extremes
        if (completed_3sec_period->extremes.voltage_min < acc_15min->extremes.voltage_min) {
            acc_15min->extremes.voltage_min = completed_3sec_period->extremes.voltage_min;
            //acc_15min->extremes.voltage_min_time_us = completed_3sec_period->extremes.voltage_min_time_us; !!!
        }
        if (completed_3sec_period->extremes.voltage_max > acc_15min->extremes.voltage_max) {
            acc_15min->extremes.voltage_max = completed_3sec_period->extremes.voltage_max;
            //acc_15min->extremes.voltage_max_time_us = completed_3sec_period->extremes.voltage_max_time_us; !!!
        }
        
        if (completed_3sec_period->extremes.current_min < acc_15min->extremes.current_min) {
            acc_15min->extremes.current_min = completed_3sec_period->extremes.current_min;
        }
        if (completed_3sec_period->extremes.current_max > acc_15min->extremes.current_max) {
            acc_15min->extremes.current_max = completed_3sec_period->extremes.current_max;
        }
        
        if (completed_3sec_period->extremes.power_min < acc_15min->extremes.power_min) {
            acc_15min->extremes.power_min = completed_3sec_period->extremes.power_min;
            //acc_15min->extremes.power_min_time_us = completed_3sec_period->extremes.power_min_time_us; !!!
        }
        if (completed_3sec_period->extremes.power_max > acc_15min->extremes.power_max) {
            acc_15min->extremes.power_max = completed_3sec_period->extremes.power_max;
            //acc_15min->extremes.power_max_time_us = completed_3sec_period->extremes.power_max_time_us; !!!
        }
    }

    // === FREQUENCY AGGREGATION ===
    if (completed_3sec_period->frequency_avg > 0) {
        acc_15min->electrical.frequency_sum += completed_3sec_period->frequency_avg;
        acc_15min->electrical.frequency_samples_count++;
    }

    // 2. Direct economics accumulation from 3-sec statistics
    esp_err_t tariff_err = rbpower_tariff_process_3sec_period_for_unit(unit_id);
    if (tariff_err != ESP_OK) {
        ESP_LOGD("RBHOME_TARIFF", "Tariff processing error for 3-sec period: %d", tariff_err);
    }

    // === ECONOMICS ===
    acc_15min->economics.cost_consumed += completed_3sec_period->result_base.cost_consumed;
    acc_15min->economics.revenue_supplied += completed_3sec_period->result_base.revenue_supplied;
    /* net_cost is the invariant (cost - revenue) of the RUNNING TOTALS — assign, never "+=":
     * adding the running difference per child compounds quadratically with the child count. */
    acc_15min->economics.net_cost = acc_15min->economics.cost_consumed - acc_15min->economics.revenue_supplied;

    // === COUNTERS AND METADATA ===
    acc_15min->metadata.input_periods_received++;
    acc_15min->metadata.valid_samples_count++;
    acc_15min->metadata.samples_count++;
    acc_15min->metadata.last_update_us = esp_timer_get_time();

    // Check readiness for finalization (96.7% completeness for 15-min)
    if (acc_15min->metadata.input_periods_received >= 290) {
        acc_15min->metadata.ready_for_finalization = true;
    }

    ESP_LOGD("RBHOME-AGG", "Unit[%d]: 3sec→15min #%lu (%lu/%lu=%.1f%%) E=%.1fWh, €%.3f/%.3f/%.3f",
             unit_id, 
             acc_15min->metadata.aggregation_iteration,
             acc_15min->metadata.input_periods_received, 
             acc_15min->metadata.input_periods_expected,
             (acc_15min->metadata.input_periods_received * 100.0f / acc_15min->metadata.input_periods_expected),
             (float)acc_15min->energy.consumed_wh_accumulator,
             acc_15min->economics.cost_consumed,
             acc_15min->economics.revenue_supplied, 
             acc_15min->economics.net_cost); 

}

/**
 * @brief Finalize 15-minute period and calculate results
 */
/* Compute a finalized period RESULT from its RAM accumulator — the common core shared by the
 * 15-min and 1-hour finalizers (they were byte-for-byte identical here, only the acc/stats pointer
 * and the period index differed). Reads g_period_accumulators[unit][period], writes the matching
 * unit.statistics.periods[period]. Does NOT touch readiness / is_active / flash store / the rollup —
 * those period-specific tails stay in the callers. The universal building block: a generic
 * finalize_period() and the flash-recompute path reuse the same RESULT shape.
 * Economics is 2-quadrant (cost_consumed/revenue_supplied); the experimental bidirectional 4-flow
 * model would extend THIS block without changing the per-period aggregation shape. */
static void compute_period_result_from_accumulator(rbpower_unit_id_t unit_id,
                                                    rbpower_stat_period_type_t period_type)
{
    rbpower_period_accumulator_t* acc  = &g_period_accumulators[unit_id][period_type];
    rbpower_period_statistics_t* stats = &g_home_network.units[unit_id].statistics.periods[period_type];

    memset(stats, 0, sizeof(rbpower_period_statistics_t));

    // === TIME BOUNDARIES ===
    stats->period_start_us = acc->time_boundaries.period_start_us;
    stats->period_end_us = esp_timer_get_time();
    stats->period_duration_ms = (stats->period_end_us - stats->period_start_us) / 1000;

    // === FINAL RMS VALUES (root mean square aggregation) ===
    if (acc->electrical.rms_samples_count > 0) {
        stats->voltage_rms = sqrtf(acc->electrical.voltage_rms_sum_squares / acc->electrical.rms_samples_count);
        stats->current_rms = sqrtf(acc->electrical.current_rms_sum_squares / acc->electrical.rms_samples_count);
    }

    // === AVERAGE POWER (linear aggregation) ===
    if (acc->electrical.power_samples_count > 0) {
        stats->power_active_avg   = acc->electrical.power_active_sum   / acc->electrical.power_samples_count;
    }
    /* q/s/pf: only inputs that carried an apparent-power envelope count (an injected quarter without
     * 1-s frames — gap credit, injtest — would otherwise drag the hour's S and PF down, e.g.
     * p_app 759 VA / pf 0.75 at a true PF of 1.00). */
    if (acc->electrical.pf_samples_count > 0) {
        stats->power_reactive_avg = acc->electrical.power_reactive_sum / acc->electrical.pf_samples_count;
        stats->power_apparent_avg = acc->electrical.power_apparent_sum / acc->electrical.pf_samples_count;
        stats->power_factor_avg   = acc->electrical.power_factor_sum   / acc->electrical.pf_samples_count;
    }

    // === PERIOD ENERGY (in / out / net) ===
    stats->energy_period.consumed_wh = (float)acc->energy.consumed_wh_accumulator;
    stats->energy_period.supplied_wh = (float)acc->energy.supplied_wh_accumulator;
    stats->energy_period.net_wh = stats->energy_period.consumed_wh - stats->energy_period.supplied_wh;

    // === EXTREMES (min/max envelope) — field-by-field; acc vs period extremes are different struct
    // types. Guarded so an unpopulated accumulator never leaks a zero envelope. (real-envelope fix) ===
    if (acc->extremes.extremes_initialized) {
        stats->extremes.voltage_min = acc->extremes.voltage_min;
        stats->extremes.voltage_max = acc->extremes.voltage_max;
        stats->extremes.current_min = acc->extremes.current_min;
        stats->extremes.current_max = acc->extremes.current_max;
        stats->extremes.power_min   = acc->extremes.power_min;
        stats->extremes.power_max   = acc->extremes.power_max;
        stats->extremes.power_peak_time_us = acc->extremes.power_peak_time_us;
    }

    // === FREQUENCY ===
    if (acc->electrical.frequency_samples_count > 0) {
        stats->frequency_avg = acc->electrical.frequency_sum / acc->electrical.frequency_samples_count;
    }

    // === ECONOMICS — direct copy + average tariff ===
    stats->result_base.cost_consumed    = acc->economics.cost_consumed;
    stats->result_base.revenue_supplied = acc->economics.revenue_supplied;
    if (acc->economics.samples_count > 0) {
        stats->result_base.avg_tariff_per_kwh =
            (acc->economics.cost_consumed + acc->economics.revenue_supplied) /
            (stats->energy_period.consumed_wh + stats->energy_period.supplied_wh) * 1000.0f;
    } else {
        stats->result_base.avg_tariff_per_kwh = 0.0f;
    }
    stats->result_base.samples_count = acc->economics.samples_count;
    stats->result_base.period_end_us = acc->economics.last_update_us;

    // === METADATA ===
    stats->samples_count = acc->metadata.input_periods_received;
    stats->valid_samples_count = acc->metadata.valid_samples_count;
    stats->data_completeness_percent = (acc->metadata.input_periods_expected > 0) ?
        (acc->metadata.input_periods_received * 100 / acc->metadata.input_periods_expected) : 0;
    stats->period_complete = true;
    stats->last_update_us = esp_timer_get_time();

    g_home_network.units[unit_id].statistics.last_period_update[period_type] = stats->last_update_us;
}

/* === Flow-conservation diagnostics ===
 * Per-unit-INDEX counters along the quarter→HOUR path, so a suspected loss is answerable with
 * numbers instead of archaeology: injected (bumped in rbhome.c inject path) vs aggregated into the
 * hour accumulator vs included in stored HOUR records. Conservation holds when
 * injected == agg15 and Σ hour_quarters == agg15 (minus the still-open hour's tail). */
static uint32_t s_flow_agg15_to_hour[RBPOWER_MAX_HOME_UNITS];
static uint32_t s_flow_hour_finalized[RBPOWER_MAX_HOME_UNITS];
static uint32_t s_flow_hour_quarters_stored[RBPOWER_MAX_HOME_UNITS];

void rbpower_statflow_read_by_index(uint8_t unit_index, uint32_t* agg15,
                                    uint32_t* hour_fin, uint32_t* hour_quarters)
{
    if (unit_index >= RBPOWER_MAX_HOME_UNITS) return;
    if (agg15)         *agg15         = s_flow_agg15_to_hour[unit_index];
    if (hour_fin)      *hour_fin      = s_flow_hour_finalized[unit_index];
    if (hour_quarters) *hour_quarters = s_flow_hour_quarters_stored[unit_index];
}

static void finalize_15min_period(rbpower_unit_id_t unit_id)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return;
    }

    rbpower_period_accumulator_t* acc_15min = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_15MIN];
    rbpower_period_statistics_t* stats_15min = &g_home_network.units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_15MIN];

    if (!acc_15min->is_enabled || !acc_15min->is_active) {
        return;
    }
    if (rbpower_home_unit_is_period_injected((uint8_t)unit_id)) {
        /* quarters of this unit come from its source (inject); a native one opened before the source registered
         * its energy mode would be a zero quarter that dispatches the hour before the injected one */
        acc_15min->is_active = false;
        return;
    }

    ESP_LOGI("RBHOME-AGG", "Unit[%d]: Finalizing 15-min period (iteration #%lu, samples %lu/%lu)",
             unit_id, acc_15min->metadata.aggregation_iteration,
             acc_15min->metadata.input_periods_received, acc_15min->metadata.input_periods_expected);

    // Compute the RESULT from the accumulator (shared core).
    compute_period_result_from_accumulator(unit_id, RBPOWER_STAT_PERIOD_15MIN);

    // 15-min results stay in RAM ONLY — there is no 15-min flash buffer (store_period_to_flash_sync
    // rejects 15MIN). The RESULT lives in stats_15min and is rolled up into the HOUR accumulator.
    acc_15min->is_active = false;

    // Roll the completed 15-min RESULT up the hierarchy. SHARED with the external-source path
    // (rbAmp inject) — both go through the same universal, result-based chain.
    bool aggregated_to_hour = rbpower_rollup_after_15min(unit_id, stats_15min);

    ESP_LOGI("RBHOME-AGG", "Unit[%d]: 15min done | U=%.1fV I=%.2fA P=%.0fW E=%.2fWh (%u%%) | agg→1h=%s",
             unit_id, stats_15min->voltage_rms, stats_15min->current_rms,
             stats_15min->power_active_avg, stats_15min->energy_period.consumed_wh,
             stats_15min->data_completeness_percent,
             aggregated_to_hour ? "Y" : "N");
}

/* Roll a completed 15-min RESULT up the universal hierarchy (1h → day → ...). Reads the
 * period_statistics RESULT (not the raw accumulator), so it is source-agnostic: native
 * units (SoS-finalized) and external-source units (rbAmp inject_period_result) aggregate
 * through the exact same path. Native calls this from finalize_15min_period; the rbAmp
 * inject path calls it after writing the 15-min result. Higher-period finalizes fire only
 * at their real wall-clock boundaries (hour at XX:00, day at 00:00, ...). */
/* The one statistics clock: real wall time, or the uptime-anchored virtual clock in accelerated
 * builds. EVERYTHING in the DAY+ pipeline (bucket dispatch, record timestamps, child-bucket
 * filtering) must use this same timebase or calendar attribution falls apart. */
time_t rbpower_stats_clock_now(void)
{
#if RBGRID_STATS_TIME_SCALE > 1
    /* Virtual clock = uptime × scale, anchored at boot = epoch 0. Deliberately uptime-based (NOT the
     * NTP-corrected wall clock) → immune to NTP adjustments; boundaries deterministic. */
    return (time_t)((uint64_t)esp_timer_get_time() * RBGRID_STATS_TIME_SCALE / 1000000ULL);
#else
    return time(NULL);
#endif
}

/* "Trusted" wall clock for calendar/lifetime decisions. rbtime_is_time_valid() is also true in the
 * Jan-2024 MANUAL fallback the hub boots with before SNTP (status FALLBACK), and the store path's
 * `> 1600000000` lets that through too — so a baseline taken 3 s after boot sat in 2024 and the SNTP
 * step then "crossed" day/week/month/year (daily reset mid-day). Any instant before
 * this firmware was built (minus 2 days of TZ slack) cannot be real. Self-maintaining via __DATE__. */
static time_t build_epoch_floor(void)
{
    static time_t s_floor = 0;
    if (s_floor) return s_floor;
    static const char MD[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0}; int d = 1, y = 2026;
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    const char* mp = strstr(MD, mon);
    int m = mp ? (int)((mp - MD) / 3) + 1 : 1;
    /* days_from_civil (Hinnant) — TZ-free */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    s_floor = (time_t)(days * 86400 - 2 * 86400);
    return s_floor;
}

bool rbpower_stats_clock_trusted(time_t now)
{
#if RBGRID_STATS_TIME_SCALE > 1
    return now > 1600000000;                 /* accel: virtual clock, no SNTP */
#else
    return rbtime_is_time_valid() && now >= build_epoch_floor();
#endif
}

uint32_t rbpower_stats_quarter_boundary_now(void)
{
#if RBGRID_STATS_TIME_SCALE > 1
    return 0;                                /* virtual clock: attribution by the collect time */
#else
    time_t now = rbpower_stats_clock_now();
    if (!rbpower_stats_clock_trusted(now)) return 0;
    return (uint32_t)(((uint64_t)now + 450u) / 900u * 900u);   /* the callback fires at the boundary ± jitter */
#endif
}

/* ---- Calendar-bucket dispatcher state (per unit) ----
 * Baseline = the buckets the unit was last seen in; a change at a 15-min rollup = a boundary crossed.
 * The baseline is taken on the 3-s tick as soon as the wall clock is valid (not at the first rollup:
 * a hub booted at 23:50 whose first rollup lands at 00:00 would baseline INTO the new day and never
 * close yesterday / reset the daily counters). A baseline taken under an invalid clock (1970/uptime
 * epoch, no SNTP yet) is re-taken when the clock becomes valid, without firing boundaries. */
static uint64_t s_last_bucket[RBPOWER_MAX_HOME_UNITS][RBSTATS_BK_COUNT];
static bool     s_bucket_init[RBPOWER_MAX_HOME_UNITS];
static bool     s_bucket_clock_valid[RBPOWER_MAX_HOME_UNITS];
/* Buckets closed while the hub was down: the lifetime snapshot of the previous run is the "last alive"
 * mark; every DAY+ bucket it belonged to that differs from today's is finalized (bucket-filtered
 * recompute from flash) at the unit's first rollup — after hour_restore_once has written the past-hour
 * image, so yesterday's last hour is part of yesterday's DAY. Record ts = the bucket END (contract). */
static bool     s_pending_close[RBPOWER_MAX_HOME_UNITS];
static uint64_t s_snap_bk[RBSTATS_BK_COUNT];
static bool     s_snap_bk_valid;

static void bucket_baseline_unit(rbpower_unit_id_t unit_id, const uint64_t bk[RBSTATS_BK_COUNT], bool clock_valid)
{
    for (int i = 0; i < RBSTATS_BK_COUNT; i++) s_last_bucket[unit_id][i] = bk[i];
    s_bucket_init[unit_id] = true;
    s_bucket_clock_valid[unit_id] = clock_valid;
}

/* 3-s tick (rbpower_home_end_3sec_period_callback): baseline every unit once the clock is valid and
 * arm the downtime closes from the lifetime snapshot. Idempotent, cheap after the first valid tick. */
void rbpower_stats_boot_clock_tick(void)
{
    time_t now = rbpower_stats_clock_now();
    const bool clock_valid = rbpower_stats_clock_trusted(now);
    struct tm lt;
    localtime_r(&now, &lt);
    uint64_t bk[RBSTATS_BK_COUNT];
    rbpower_stats_calc_buckets(now, &lt, bk);

    if (clock_valid && !s_snap_bk_valid) {
        uint32_t snap = rbpower_lifetime_restored_snapshot_ts();
        s_snap_bk_valid = true;                       /* evaluated once per boot */
        if (rbpower_stats_clock_trusted((time_t)snap)) {   /* snapshot under Jan-2024 fallback = unknown day */
            time_t st = (time_t)snap;
            struct tm stm;
            localtime_r(&st, &stm);
            rbpower_stats_calc_buckets(st, &stm, s_snap_bk);
            bool any = false;
            for (int i = RBSTATS_BK_DAY; i < RBSTATS_BK_COUNT; i++) any |= (s_snap_bk[i] != bk[i]);
            if (any) {
                for (uint8_t u = 0; u < g_home_network.units_count; u++) s_pending_close[u] = true;
                ESP_LOGW("RBHOME-AGG", "buckets closed in downtime (snapshot %lu): DAY %s WEEK %s MONTH %s YEAR %s — will close at first rollup",
                         (unsigned long)snap,
                         s_snap_bk[RBSTATS_BK_DAY]   != bk[RBSTATS_BK_DAY]   ? "yes" : "no",
                         s_snap_bk[RBSTATS_BK_WEEK]  != bk[RBSTATS_BK_WEEK]  ? "yes" : "no",
                         s_snap_bk[RBSTATS_BK_MONTH] != bk[RBSTATS_BK_MONTH] ? "yes" : "no",
                         s_snap_bk[RBSTATS_BK_YEAR]  != bk[RBSTATS_BK_YEAR]  ? "yes" : "no");
            }
        }
    }
    for (uint8_t u = 0; u < g_home_network.units_count && u < RBPOWER_MAX_HOME_UNITS; u++) {
        if (!g_home_network.units[u].has_valid_definition) continue;
        if (!s_bucket_init[u] || (!s_bucket_clock_valid[u] && clock_valid)) {
            if (s_bucket_init[u])
                ESP_LOGW("RBHOME-AGG", "unit %u: wall clock became valid — period buckets re-baselined (no boundary fired)", (unsigned)u);
            bucket_baseline_unit(u, bk, clock_valid);
        }
    }
}

/* Finalize the DAY+ buckets the previous run was in (see s_pending_close). Runs inside the unit's
 * first rollup, right after the HOUR stage. */
/* Close the open HOUR of a unit whose hour bucket `closed_hour` is over. Any number of quarters closes it
 * (dur/q from covered time → PARTIAL). Record ts = END of that hour, not the store moment: a late close
 * (missing boundary quarter, module offline) stamped "now" and the DAY recompute (mid = ts − 1800) could put
 * the hour into the wrong day. Accelerated builds keep "now" (virtual clock). */
static void close_open_hour(rbpower_unit_id_t unit_id, uint64_t closed_hour)
{
    rbpower_period_accumulator_t* ah = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_1HOUR];
    if (ah->metadata.input_periods_received > 0 && !ah->metadata.ready_for_finalization) {
        ah->metadata.ready_for_finalization = true;
        rbpower_diag_printf("hour 0x%04X closed with %lu quarter(s) — PARTIAL", (unsigned)g_home_network.units[unit_id].definition.unit_id,
                            (unsigned long)ah->metadata.input_periods_received);
    }
#if !(RBGRID_STATS_TIME_SCALE > 1)
    if (closed_hour != 0 && closed_hour != UINT64_MAX) {
        s_store_ts_override = (uint32_t)((closed_hour + 1) * 3600ULL);
        s_store_ts_override_period = RBPOWER_STAT_PERIOD_1HOUR;
    }
#else
    (void)closed_hour;
#endif
    finalize_1hour_period(unit_id);          // HOUR must store first (DAY recomputes from flash HOUR records)
    s_store_ts_override = 0;
}

static void close_downtime_buckets(rbpower_unit_id_t unit_id, const uint64_t bk_now[RBSTATS_BK_COUNT])
{
    s_pending_close[unit_id] = false;
    static const struct { rbstats_bucket_idx_t idx; const char* name; } LV[] = {
        { RBSTATS_BK_DAY, "DAY" }, { RBSTATS_BK_WEEK, "WEEK" }, { RBSTATS_BK_MONTH, "MONTH" }, { RBSTATS_BK_YEAR, "YEAR" } };
    rbpower_unit_id_t real_id = g_home_network.units[unit_id].definition.unit_id;
    for (size_t l = 0; l < sizeof(LV) / sizeof(LV[0]); l++) {
        rbstats_bucket_idx_t idx = LV[l].idx;
        if (s_snap_bk[idx] == bk_now[idx]) continue;
        int64_t b_start = 0, b_end = 0;
        if (!rbpower_stats_bucket_span(idx, s_snap_bk[idx], &b_start, &b_end) || b_end <= 0) continue;
        s_store_ts_override = (uint32_t)b_end;         /* record ts = bucket END, not "now" */
        s_store_ts_override_period = (idx == RBSTATS_BK_DAY) ? RBPOWER_STAT_PERIOD_DAILY : (idx == RBSTATS_BK_WEEK) ? RBPOWER_STAT_PERIOD_WEEKLY : (idx == RBSTATS_BK_MONTH) ? RBPOWER_STAT_PERIOD_MONTHLY : RBPOWER_STAT_PERIOD_YEARLY;
        rbpower_diag_printf("%s 0x%04X closed in downtime: bucket=%llu ts=%lu", LV[l].name, (unsigned)real_id,
                            (unsigned long long)s_snap_bk[idx], (unsigned long)b_end);
        switch (idx) {
            case RBSTATS_BK_DAY:   finalize_1day_period(unit_id, s_snap_bk[idx]);   break;
            case RBSTATS_BK_WEEK:  finalize_1week_period(unit_id, s_snap_bk[idx]);  break;
            case RBSTATS_BK_MONTH:
                finalize_1month_period(unit_id, s_snap_bk[idx]);
                {   /* a billing month that ended during downtime must settle too, or its import/export carried
                     * into the next billing period */
                    extern void rbpower_tariff_net_metering_settle(rbpower_unit_id_t);
                    rbpower_tariff_net_metering_settle(real_id);
                }
                break;
            case RBSTATS_BK_YEAR:  finalize_1year_period(unit_id, s_snap_bk[idx]);  break;
            default: break;
        }
        s_store_ts_override = 0;
    }
}

bool rbpower_rollup_after_15min(rbpower_unit_id_t unit_id,
                                const rbpower_period_statistics_t* stats_15min)
{
    return rbpower_rollup_after_15min_at(unit_id, stats_15min, 0);
}

bool rbpower_rollup_after_15min_at(rbpower_unit_id_t unit_id,
                                   const rbpower_period_statistics_t* stats_15min, uint32_t q_end_ts)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || !stats_15min) {
        return false;
    }

    // Reflect the unit's CONFIGURED period depth (stat_class) onto the 1HOUR accumulator enable, here
    // in the SHARED rollup — the one point guaranteed to run for EVERY unit on EVERY 15-min finalize,
    // for BOTH the native finalize_15min path and the rbAmp inject path. The boot enable-loops
    // (rbpower_home_init / rbpower_home_create_unit) do NOT cover the rbAmp FLEET units' creation path
    // (they are neither plain predefined nor create_unit-bound), so their is_enabled stayed false and
    // the 15min→1HOUR aggregation was silently skipped (15MIN filled, 1HOUR empty). Deriving it from
    // stat_class is DEPTH-AWARE — NOT the old blanket
    // force-enable: a unit whose configured depth is below 1HOUR stays disabled and grows no spurious
    // HOUR record (period depth stays under config control). Idempotent.
    g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_1HOUR].is_enabled =
        (RBPOWER_STAT_PERIOD_1HOUR <= g_home_network.units[unit_id].definition.stat_class);

    bool aggregated_to_hour = false;
    if (should_aggregate_to_period(unit_id, RBPOWER_STAT_PERIOD_1HOUR)) {
        hour_restore_once((uint8_t)unit_id);           /* reboot inside the hour: continue it */
        uint32_t acc_hour_ts = 0;                      /* hour this quarter is folded into (0 = by clock) */
        {   /* The quarter belongs to the hour of its window END when the source knows it (q_end_ts = the boundary
             * it was collected for: a 10:45–11:00 quarter collected at 11:00:02 is hour 10), else of its MIDPOINT.
             * When the boundary quarter was missed (module unreachable at hh:00, dropped event), the next quarter
             * (hh:00–hh:15) arrived while the previous hour was still open and was folded into it before the
             * bucket check closed it. */
            time_t now_q = rbpower_stats_clock_now();
            uint64_t q_hour = q_end_ts ? (uint64_t)(q_end_ts - 1u) / 3600ULL
                                       : (uint64_t)((now_q > 450) ? (now_q - 450) : 0) / 3600ULL;
            const bool bk_ok = s_bucket_init[unit_id] && s_bucket_clock_valid[unit_id] && rbpower_stats_clock_trusted(now_q);
            if (bk_ok && q_hour > s_last_bucket[unit_id][RBSTATS_BK_HOUR]) {
                uint64_t closed = s_last_bucket[unit_id][RBSTATS_BK_HOUR];
                s_last_bucket[unit_id][RBSTATS_BK_HOUR] = q_hour;
                close_open_hour(unit_id, closed);
            }
            if (q_end_ts) {
                if (bk_ok && q_hour < s_last_bucket[unit_id][RBSTATS_BK_HOUR]) {
                    /* its hour was already closed (boundary timeout): it can only join the open hour */
                    rbpower_diag_printf("FAIL quarter 0x%04X end=%lu arrived after its hour was closed — folded into the open hour",
                                        (unsigned)g_home_network.units[unit_id].definition.unit_id, (unsigned long)q_end_ts);
                    q_hour = s_last_bucket[unit_id][RBSTATS_BK_HOUR];
                }
                acc_hour_ts = (uint32_t)(q_hour * 3600ULL);
            }
        }
        aggregate_1hour_from_15min_period(unit_id, stats_15min);
        aggregated_to_hour = true;
        s_hour_of_acc[unit_id] = acc_hour_ts;
        hour_persist((uint8_t)unit_id);                /* survive the next (un)planned restart */
    }

    rollup_dispatch_buckets(unit_id);
    return aggregated_to_hour;
}

/* Calendar-bucket dispatch of one unit (HOUR close, DAY+ finalizes) against the stats clock. Runs after each
 * 15-min rollup and from the boundary timeout (rbpower_stats_service_tick). Caller holds the home lock. */
static void rollup_dispatch_buckets(rbpower_unit_id_t unit_id)
{
    time_t now = rbpower_stats_clock_now();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // === BUCKET-CROSSING BOUNDARY DISPATCH (drift- / NTP- / no-SNTP-robust) — keystone fix ===
    // A higher period finalizes when the unit crosses into a NEW period BUCKET, not on an exact
    // wall-clock minute match. The old `tm_min==0` (and nested tm_hour/tm_wday/tm_mday/tm_mon ==)
    // equality required a 15-min tick to land EXACTLY on XX:00 — with timer jitter, boot phase, or the
    // accelerated virtual clock it almost never does, so finalize_1hour never fired and the 1HOUR
    // result stayed empty even though aggregate_1hour kept filling the accumulator (only the manual
    // statforce1h/period verbs produced records). Comparing integer bucket indices is immune
    // to jitter, NTP step corrections, AND missing SNTP (uptime epoch) alike — it only needs the bucket
    // to tick once per period. Per-unit last-seen buckets; first sight records the baseline without
    // firing (no spurious boundary at boot). `now` is the same virtual/real clock chosen above.
    uint64_t bk[RBSTATS_BK_COUNT];
    rbpower_stats_calc_buckets(now, &timeinfo, bk);
    const bool clock_valid_now = rbpower_stats_clock_trusted(now);
    if (!s_bucket_init[unit_id] || (!s_bucket_clock_valid[unit_id] && clock_valid_now)) {
        /* Normally taken on the 3-s tick (rbpower_stats_boot_clock_tick); fallback here. */
        if (s_bucket_init[unit_id])
            ESP_LOGW("RBHOME-AGG", "unit %u: wall clock became valid — period buckets re-baselined (no boundary fired)", (unsigned)unit_id);
        bucket_baseline_unit(unit_id, bk, clock_valid_now);
    } else {
        if (s_pending_close[unit_id] && clock_valid_now) {
            close_downtime_buckets(unit_id, bk);      /* buckets the previous run was in (downtime) */
        }
        /* Buckets act only when they move FORWARD (">"): a TZ change / the selftest's temporary TZ switch /
         * a clock step back moved DAY/MONTH backwards and "!=" fired spurious closes (a future-dated DAY, a
         * daily reset mid-day, a duplicate DAY at the next midnight). A backward
         * move is ignored; the bucket is acted on once real time passes it again. */
        if (bk[RBSTATS_BK_HOUR] > s_last_bucket[unit_id][RBSTATS_BK_HOUR]) {
            /* A natural hour boundary closes the hour with ANY number of quarters (dur/q from covered
             * time → PARTIAL). The legacy "3 of 4 quarters" readiness silently skipped the close: the
             * hour record was never written and its quarters folded into the NEXT hour (e.g. hub up at
             * hh:45 / a midnight crossing with 2 quarters → no 23:00 record, DAY short by it). */
            uint64_t closed_hour = s_last_bucket[unit_id][RBSTATS_BK_HOUR];
            s_last_bucket[unit_id][RBSTATS_BK_HOUR] = bk[RBSTATS_BK_HOUR];
            close_open_hour(unit_id, closed_hour);
        }
        if (bk[RBSTATS_BK_DAY] > s_last_bucket[unit_id][RBSTATS_BK_DAY]) {
            uint64_t closed_day = s_last_bucket[unit_id][RBSTATS_BK_DAY];
            s_last_bucket[unit_id][RBSTATS_BK_DAY] = bk[RBSTATS_BK_DAY];
            rbpower_lifetime_reset_daily_unit((uint8_t)unit_id);   /* this unit, at ITS boundary (E8) */
            finalize_1day_period(unit_id, closed_day);
        }
        if (bk[RBSTATS_BK_WEEK] > s_last_bucket[unit_id][RBSTATS_BK_WEEK]) {
            uint64_t closed_week = s_last_bucket[unit_id][RBSTATS_BK_WEEK];
            s_last_bucket[unit_id][RBSTATS_BK_WEEK] = bk[RBSTATS_BK_WEEK];
            finalize_1week_period(unit_id, closed_week);
        }
        if (bk[RBSTATS_BK_MONTH] > s_last_bucket[unit_id][RBSTATS_BK_MONTH]) {
            uint64_t closed_month = s_last_bucket[unit_id][RBSTATS_BK_MONTH];
            s_last_bucket[unit_id][RBSTATS_BK_MONTH] = bk[RBSTATS_BK_MONTH];
            finalize_1month_period(unit_id, closed_month);
            // Net-metering settles on the billing-period (monthly) boundary: net the month's import vs
            // export → bank/draw kWh credits. Takes the REAL unit id (resolves to index internally).
            extern void rbpower_tariff_net_metering_settle(rbpower_unit_id_t);
            rbpower_tariff_net_metering_settle(g_home_network.units[unit_id].definition.unit_id);
        }
        if (bk[RBSTATS_BK_YEAR] > s_last_bucket[unit_id][RBSTATS_BK_YEAR]) {
            uint64_t closed_year = s_last_bucket[unit_id][RBSTATS_BK_YEAR];
            s_last_bucket[unit_id][RBSTATS_BK_YEAR] = bk[RBSTATS_BK_YEAR];
            finalize_1year_period(unit_id, closed_year);
        }
    }
}

/* Pure period-bucket calculator (unit-tested on 5 TZs in rbpower_logic_test.c).
 * DAY/WEEK/MONTH/YEAR are LOCAL-calendar buckets — the caller passes localtime_r(now) so the TZ
 * (incl. DST) decides where midnight is:
 *   - local day index = (now − local_seconds_of_day) mapped to days (rounded at half-day to absorb
 *     the ±1h DST wobble of local midnight's epoch);
 *   - week = Monday-aligned local day index (epoch day 0 = Thu → +4);
 *   - month/year straight from the local calendar fields;
 *   - HOUR stays the UTC hour (equal to local for whole-hour TZs).
 * A UTC day (now/86400) would make a UTC+8 user's "today" span 08:00–08:00 local, and daily
 * sums would disagree with the utility bill. */
void rbpower_stats_calc_buckets(int64_t now_epoch, const struct tm* local,
                                uint64_t bk_out[RBSTATS_BK_COUNT])
{
    int64_t sod = (int64_t)local->tm_hour * 3600 + local->tm_min * 60 + local->tm_sec;
    int64_t local_day = (now_epoch - sod + 43200) / 86400;   /* local-calendar day index */
    bk_out[RBSTATS_BK_HOUR]  = (uint64_t)(now_epoch / 3600);
    bk_out[RBSTATS_BK_DAY]   = (uint64_t)local_day;
    /* Monday-aligned: epoch day 0 = Thursday, so a Monday is local_day ≡ 4 (mod 7) and the
     * bucket must flip there → offset +3. The old +4 flipped at SUNDAY midnight (caught by
     * suite_local_buckets "WEEK flips at local Monday 00:00"). */
    bk_out[RBSTATS_BK_WEEK]  = (uint64_t)((local_day + 3) / 7);
    bk_out[RBSTATS_BK_MONTH] = (uint64_t)(local->tm_year * 12 + local->tm_mon);
    bk_out[RBSTATS_BK_YEAR]  = (uint64_t)local->tm_year;
}

/* Epoch of the local midnight OPENING local-day `day_id` (inverse of the DAY bucket formula).
 * Probe near that day's nominal noon, normalize to midnight via mktime, then verify by the
 * forward bucket formula and correct ±1 day (extreme TZ offsets can land the probe adjacent). */
static int64_t local_midnight_of_day(int64_t day_id)
{
    time_t probe = (time_t)(day_id * 86400 + 43200);
    struct tm lt;
    localtime_r(&probe, &lt);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    time_t mid = mktime(&lt);
    for (int guard = 0; guard < 3; guard++) {
        time_t chk_t = mid + 43200;
        struct tm chk;
        localtime_r(&chk_t, &chk);
        uint64_t bk[RBSTATS_BK_COUNT];
        rbpower_stats_calc_buckets((int64_t)chk_t, &chk, bk);
        if ((int64_t)bk[RBSTATS_BK_DAY] == day_id) {
            break;
        }
        probe = mid + (((int64_t)bk[RBSTATS_BK_DAY] < day_id) ? 86400 + 43200 : -86400 + 43200);
        localtime_r(&probe, &lt);
        lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
        mid = mktime(&lt);
    }
    return (int64_t)mid;
}

/* Exact [start, end) of a calendar bucket under the current TZ. */
bool rbpower_stats_bucket_span(rbstats_bucket_idx_t idx, uint64_t bucket,
                               int64_t* start_out, int64_t* end_out)
{
    int64_t start = 0, end = 0;
    switch (idx) {
        case RBSTATS_BK_HOUR:
            start = (int64_t)bucket * 3600;
            end   = start + 3600;
            break;
        case RBSTATS_BK_DAY:
            start = local_midnight_of_day((int64_t)bucket);
            end   = local_midnight_of_day((int64_t)bucket + 1);
            break;
        case RBSTATS_BK_WEEK:   /* week W covers local days [7W−3, 7W+3] = Mon..Sun (offset +3, epoch day 0 = Thu) */
            start = local_midnight_of_day((int64_t)bucket * 7 - 3);
            end   = local_midnight_of_day((int64_t)bucket * 7 + 4);
            break;
        case RBSTATS_BK_MONTH: {
            int64_t y = (int64_t)bucket / 12, m = (int64_t)bucket % 12;
            struct tm a = {0}, b = {0};
            a.tm_year = (int)y; a.tm_mon = (int)m; a.tm_mday = 1; a.tm_isdst = -1;
            b.tm_year = (int)(y + (m == 11 ? 1 : 0)); b.tm_mon = (int)((m + 1) % 12); b.tm_mday = 1; b.tm_isdst = -1;
            start = (int64_t)mktime(&a);
            end   = (int64_t)mktime(&b);
            break;
        }
        case RBSTATS_BK_YEAR: {
            struct tm a = {0}, b = {0};
            a.tm_year = (int)bucket; a.tm_mon = 0; a.tm_mday = 1; a.tm_isdst = -1;
            b.tm_year = (int)bucket + 1; b.tm_mon = 0; b.tm_mday = 1; b.tm_isdst = -1;
            start = (int64_t)mktime(&a);
            end   = (int64_t)mktime(&b);
            break;
        }
        default:
            return false;
    }
    if (start_out) *start_out = start;
    if (end_out)   *end_out = end;
    return end > start;
}

/**
 * @brief Aggregate hourly period from completed 15-minute period
 */
static void aggregate_1hour_from_15min_period(rbpower_unit_id_t unit_id, 
                                            const rbpower_period_statistics_t* completed_15min_period)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || !completed_15min_period) {
        return;
    }
    
    rbpower_period_accumulator_t* acc_1hour = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_1HOUR];

    if (!acc_1hour->is_enabled) {
        return;
    }

    // Initialize new hourly period on first 15-min period
    if (!acc_1hour->is_active) {
        init_aggregation_period(unit_id, RBPOWER_STAT_PERIOD_1HOUR);
        acc_1hour->time_boundaries.period_start_us = completed_15min_period->period_start_us;
    }

    // === RMS AGGREGATION (root mean square) ===
    if (completed_15min_period->voltage_rms > 0) {
        acc_1hour->electrical.voltage_rms_sum_squares += 
            completed_15min_period->voltage_rms * completed_15min_period->voltage_rms;
    }
    if (completed_15min_period->current_rms >= 0) {
        acc_1hour->electrical.current_rms_sum_squares += 
            completed_15min_period->current_rms * completed_15min_period->current_rms;
    }
    acc_1hour->electrical.rms_samples_count++;

    // === POWER AGGREGATION (linear) ===
    acc_1hour->electrical.power_active_sum += completed_15min_period->power_active_avg;
    acc_1hour->electrical.power_samples_count++;
    if (completed_15min_period->power_apparent_avg > 0.0f) {       /* envelope present (see compute) */
        acc_1hour->electrical.power_reactive_sum += completed_15min_period->power_reactive_avg;
        acc_1hour->electrical.power_apparent_sum += completed_15min_period->power_apparent_avg;
        acc_1hour->electrical.power_factor_sum += completed_15min_period->power_factor_avg;
        acc_1hour->electrical.pf_samples_count++;
    }

    // === ENERGY AGGREGATION (direct summation) ===
    acc_1hour->energy.consumed_wh_accumulator += completed_15min_period->energy_period.consumed_wh;
    acc_1hour->energy.supplied_wh_accumulator += completed_15min_period->energy_period.supplied_wh;

    // === EXTREMES AGGREGATION (global min/max) ===
    if (!acc_1hour->extremes.extremes_initialized) {
        acc_1hour->extremes.voltage_min = completed_15min_period->extremes.voltage_min;
        acc_1hour->extremes.voltage_max = completed_15min_period->extremes.voltage_max;
        acc_1hour->extremes.current_min = completed_15min_period->extremes.current_min;
        acc_1hour->extremes.current_max = completed_15min_period->extremes.current_max;
        acc_1hour->extremes.power_min = completed_15min_period->extremes.power_min;
        acc_1hour->extremes.power_max = completed_15min_period->extremes.power_max;
        acc_1hour->extremes.extremes_initialized = true;
    } else {
        // Update extremes
        if (completed_15min_period->extremes.voltage_min < acc_1hour->extremes.voltage_min) {
            acc_1hour->extremes.voltage_min = completed_15min_period->extremes.voltage_min;
            //acc_1hour->extremes.voltage_min_time_us = completed_15min_period->extremes.voltage_min_time_us; !!!
        }
        if (completed_15min_period->extremes.voltage_max > acc_1hour->extremes.voltage_max) {
            acc_1hour->extremes.voltage_max = completed_15min_period->extremes.voltage_max;
            //acc_1hour->extremes.voltage_max_time_us = completed_15min_period->extremes.voltage_max_time_us; !!!
        }
        
        if (completed_15min_period->extremes.current_min < acc_1hour->extremes.current_min) {
            acc_1hour->extremes.current_min = completed_15min_period->extremes.current_min;
        }
        if (completed_15min_period->extremes.current_max > acc_1hour->extremes.current_max) {
            acc_1hour->extremes.current_max = completed_15min_period->extremes.current_max;
        }
        
        if (completed_15min_period->extremes.power_min < acc_1hour->extremes.power_min) {
            acc_1hour->extremes.power_min = completed_15min_period->extremes.power_min;
            //acc_1hour->extremes.power_min_time_us = completed_15min_period->extremes.power_min_time_us; !!!
        }
        if (completed_15min_period->extremes.power_max > acc_1hour->extremes.power_max) {
            acc_1hour->extremes.power_max = completed_15min_period->extremes.power_max;
            //acc_1hour->extremes.power_max_time_us = completed_15min_period->extremes.power_max_time_us; !!!
        }
    }

    // === FREQUENCY AGGREGATION ===
    if (completed_15min_period->frequency_avg > 0) {
        acc_1hour->electrical.frequency_sum += completed_15min_period->frequency_avg;
        acc_1hour->electrical.frequency_samples_count++;
    }

    // Simply sum (economics already zeroed during initialization)
    acc_1hour->economics.cost_consumed += completed_15min_period->result_base.cost_consumed;
    acc_1hour->economics.revenue_supplied += completed_15min_period->result_base.revenue_supplied;
    /* Assign the running-totals invariant — "+=" of the running difference compounds quadratically. */
    acc_1hour->economics.net_cost = acc_1hour->economics.cost_consumed - acc_1hour->economics.revenue_supplied;

    // === COUNTERS AND METADATA ===
    acc_1hour->metadata.input_periods_received++;
    acc_1hour->metadata.valid_samples_count++;
    acc_1hour->metadata.samples_count++;
    /* covered time = Σ quarter windows (period_duration_ms = dt of the injected/native quarter, incl. a
     * reboot-gap tail). The record's dur comes from HERE, not from the input count: an on-demand
     * collect (`period` verb) or a late join SPLITS a window, it doesn't add time — count×900 would
     * give dur=4500 for an hour with one extra collect. */
    acc_1hour->metadata.covered_ms += completed_15min_period->period_duration_ms;
    acc_1hour->metadata.last_update_us = esp_timer_get_time();
    s_flow_agg15_to_hour[unit_id]++;   // flow diagnostics: quarter accepted into the hour acc

    // Readiness for hourly finalization: 75% completeness (3 of 4 fifteen-minute periods). Threshold
    // now from the descriptor table (PERIODS[1HOUR].ready_threshold = 3) instead of a magic literal.

    if (acc_1hour->metadata.input_periods_received >= PERIODS[RBPOWER_STAT_PERIOD_1HOUR].ready_threshold) {
        acc_1hour->metadata.ready_for_finalization = true;
    }

ESP_LOGD("RBHOME-AGG", "Unit[%d]: 15min→1hour #%lu (%lu/%lu=%.1f%%) E=%.0fWh, €%.3f/%.3f/%.3f",
         unit_id, 
         acc_1hour->metadata.aggregation_iteration,
         acc_1hour->metadata.input_periods_received, 
         acc_1hour->metadata.input_periods_expected,
         (acc_1hour->metadata.input_periods_received * 100.0f / acc_1hour->metadata.input_periods_expected),
         (float)acc_1hour->energy.consumed_wh_accumulator,
         acc_1hour->economics.cost_consumed,
         acc_1hour->economics.revenue_supplied,
         acc_1hour->economics.net_cost);
}

/**
 * @brief Finalize hourly period and calculate results
 */
/* q bits raised by the source side for the hour now open (module firmware update / restart), per units[] index */
static uint16_t s_hour_q_extra[RBPOWER_MAX_HOME_UNITS];
void rbpower_stats_flag_open_hour(rbpower_unit_id_t unit_def_id, uint16_t q_bits)
{
    uint8_t i = find_unit_index_by_id(unit_def_id);
    if (i < RBPOWER_MAX_HOME_UNITS) s_hour_q_extra[i] |= q_bits;
}

static void finalize_1hour_period(rbpower_unit_id_t unit_id)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return;
    }
    
    rbpower_period_accumulator_t* acc_1hour = &g_period_accumulators[unit_id][RBPOWER_STAT_PERIOD_1HOUR];
    rbpower_period_statistics_t* stats_1hour = &g_home_network.units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_1HOUR];
    
    if (!acc_1hour->is_enabled || !acc_1hour->is_active) {
        return;
    }

    // === CHECK READINESS FOR FINALIZATION ===
    if (!acc_1hour->metadata.ready_for_finalization) {
        ESP_LOGW("RBHOME-AGG", "Unit[%d]: Hourly period NOT READY for finalization (%lu/%lu periods, threshold: 3)",
                 unit_id,
                 acc_1hour->metadata.input_periods_received,
                 acc_1hour->metadata.input_periods_expected);
        return;
    }

    ESP_LOGI("RBHOME-AGG", "Unit[%d]: Finalizing hourly period (iteration #%lu, samples %lu/%lu)",
             unit_id, acc_1hour->metadata.aggregation_iteration,
             acc_1hour->metadata.input_periods_received, acc_1hour->metadata.input_periods_expected);

    // Compute the RESULT from the accumulator (shared core).
    compute_period_result_from_accumulator(unit_id, RBPOWER_STAT_PERIOD_1HOUR);
    {   /* v4 dur/q from the covered time (see aggregate_1hour_from_15min_period). Within ±60/120 s of
         * the nominal hour → exactly 3600 (quarter-window jitter); shorter → PARTIAL; a tail that
         * crossed the hour boundary cannot make an hour longer than itself. */
        uint32_t cov_s = (acc_1hour->metadata.covered_ms + 500u) / 1000u;
        if (cov_s > 0) {
            if (cov_s >= 3600u - 60u) cov_s = 3600u;
            stats_1hour->v4_dur_s = cov_s;
            stats_1hour->v4_q = (cov_s < 3600u) ? RBSTATS_Q_PARTIAL : 0;
        }
        stats_1hour->v4_q |= s_hour_q_extra[unit_id];
        s_hour_q_extra[unit_id] = 0;
    }

    ESP_LOGD("RBHOME-AGG", "Unit[%d]: Economics 1hour - cost=%.4f EUR, revenue=%.4f EUR, net=%.4f EUR",
            unit_id, stats_1hour->result_base.cost_consumed,
            stats_1hour->result_base.revenue_supplied, (stats_1hour->result_base.cost_consumed - stats_1hour->result_base.revenue_supplied));

    // === SYNCHRONOUS SAVE OF HOURLY PERIOD TO FLASH BUFFERS ===
    // Unlike 15MIN, the HOUR result IS persisted (it seeds the DAY/WEEK/... flash-recompute path).
    rbpower_unit_id_t real_unit_id = g_home_network.units[unit_id].definition.unit_id;
    ESP_LOGI("RBHOME-AGG", "Unit[%d/0x%04X]: Writing 1hour period to Flash buffers", unit_id, real_unit_id);
    store_period_to_flash_sync(real_unit_id, RBPOWER_STAT_PERIOD_1HOUR, stats_1hour);

    // Flow diagnostics: one hour finalized, carrying this many quarters.
    s_flow_hour_finalized[unit_id]++;
    s_flow_hour_quarters_stored[unit_id] += acc_1hour->metadata.input_periods_received;

    // Deactivate accumulator
    acc_1hour->is_active = false;
    hour_persist_erase((uint8_t)unit_id);              /* the hour is on flash now */

    ESP_LOGI("RBHOME-AGG", "Unit[%d]: Hourly period completed - U=%.1fV I=%.2fA P=%.0fW E=%.1fWh (%u%%)",
             unit_id, stats_1hour->voltage_rms, stats_1hour->current_rms,
             stats_1hour->power_active_avg, stats_1hour->energy_period.consumed_wh,
             stats_1hour->data_completeness_percent);
    {   /* diagnostics trace: one line per closed hour, FAIL on an impossible envelope */
        float pf = stats_1hour->power_factor_avg;
        bool bad = (pf < 0.0f || pf > 1.0f) ||
                   (stats_1hour->power_active_avg != 0.0f && stats_1hour->energy_period.consumed_wh == 0.0f &&
                    stats_1hour->energy_period.supplied_wh == 0.0f);
        rbpower_diag_printf("%sHOUR 0x%04X ec=%.3f es=%.3fWh P=%.1fW pf=%.3f dur=%lus%s inputs=%lu",
                            bad ? "FAIL " : "", (unsigned)real_unit_id,
                            stats_1hour->energy_period.consumed_wh, stats_1hour->energy_period.supplied_wh,
                            stats_1hour->power_active_avg, pf, (unsigned long)stats_1hour->v4_dur_s,
                            (stats_1hour->v4_q & RBSTATS_Q_PARTIAL) ? " PARTIAL" : "",
                            (unsigned long)acc_1hour->metadata.input_periods_received);
    }
}


/* ---- Open-HOUR accumulator persistence (otherwise a mid-hour reboot splits the hour into a
 * PARTIAL flushed record + a second "complete" record missing the pre-reboot quarters) ----
 * The 1HOUR accumulator lives in RAM. It is now saved to NVS (ns "rbhour", key u<idx>) after every
 * quarter rollup and on a planned restart, and restored at the first quarter after boot when the
 * wall clock is still inside the SAME hour — one complete record, no duplicate. A blob from another
 * hour (down across the boundary) is dropped: the energy is still exact via the module counters
 * (reboot-gap tail), only that hour's record loses its pre-outage quarters. */
/** @brief NVS image of an open HOUR accumulator. */
typedef struct {
    uint32_t magic;                        ///< RBHOME_HOUR_MAGIC
    uint32_t hour_ts;                      ///< UTC hour start the accumulator belongs to, epoch s
    uint16_t unit_id;                      ///< real hex id (slot reuse guard)
    uint16_t reserved;                     ///< Reserved
    rbpower_period_accumulator_t acc;      ///< The accumulator
} rbhome_hour_blob_t;
#define RBHOME_HOUR_MAGIC 0x32305248u   ///< "HR02": + metadata.covered_ms (HR01 images are rejected)
static bool s_hour_restore_tried[RBPOWER_MAX_HOME_UNITS];

static uint32_t hour_start_now(void)
{
    if (!rbtime_is_time_valid()) return 0;
    /* −2 s: the quarter collected AT a boundary (08:00:00) belongs to the hour that just ended. With the
     * bare wall clock the first quarter after a planned restart at 07:52 was restored as a PAST hour and
     * the hour got split into two records (ts 07:52:01 dur 3121 + ts 08:00 dur 475). */
    uint64_t t = rbtime_get_unix_timestamp();
    if (t > 2) t -= 2;
    return (uint32_t)(t - (t % 3600ULL));
}

static void hour_persist(uint8_t unit_idx)
{
    rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_idx][RBPOWER_STAT_PERIOD_1HOUR];
    /* the hour the accumulator belongs to: a boundary quarter collected at hh:00:02 is still the past hour's */
    uint32_t h = (rbtime_is_time_valid() && s_hour_of_acc[unit_idx]) ? s_hour_of_acc[unit_idx] : hour_start_now();
    if (h == 0 || !acc->is_active || acc->metadata.input_periods_received == 0) return;
    nvs_handle_t nh;
    if (nvs_open("rbhour", NVS_READWRITE, &nh) != ESP_OK) return;
    static rbhome_hour_blob_t b;   /* ~accumulator-sized: off the stats task stack (callers hold the home lock) */
    memset(&b, 0, sizeof(b));
    b.magic = RBHOME_HOUR_MAGIC; b.hour_ts = h;
    b.unit_id = (uint16_t)g_home_network.units[unit_idx].definition.unit_id;
    b.acc = *acc;
    char key[8]; snprintf(key, sizeof(key), "u%u", (unsigned)unit_idx);
    esp_err_t e = nvs_set_blob(nh, key, &b, sizeof(b));
    if (e == ESP_OK) e = nvs_commit(nh);
    if (e != ESP_OK) {
        ESP_LOGE("RBHOME-AGG", "open-hour image u%u NOT saved (%s) — a restart now loses this hour's quarters", unit_idx, esp_err_to_name(e));
        rbpower_diag_printf("FAIL hour persist u%u: %s", unit_idx, esp_err_to_name(e));
    } else {
        nvs_stats_t st; size_t nfree = 0, nused = 0;
        if (nvs_get_stats(NULL, &st) == ESP_OK) { nfree = st.free_entries; nused = st.used_entries; }
        rbpower_diag_printf("%shour persist u%u ts=%lu quarters=%lu E=%.3fWh nvs=%u/%u", nfree < 40 ? "FAIL " : "",
                            unit_idx, (unsigned long)h, (unsigned long)acc->metadata.input_periods_received,
                            acc->energy.consumed_wh_accumulator, (unsigned)nused, (unsigned)(nused + nfree));
    }
    nvs_close(nh);
}

static void hour_persist_erase(uint8_t unit_idx)
{
    nvs_handle_t nh;
    if (nvs_open("rbhour", NVS_READWRITE, &nh) != ESP_OK) return;
    char key[8]; snprintf(key, sizeof(key), "u%u", (unsigned)unit_idx);
    if (nvs_erase_key(nh, key) == ESP_OK) nvs_commit(nh);
    nvs_close(nh);
}

/* First quarter after boot: bring back the open hour saved before the restart (same hour only). */
static void hour_restore_once(uint8_t unit_idx)
{
    if (s_hour_restore_tried[unit_idx]) return;
    uint32_t h = hour_start_now();
    if (h == 0) return;                                   /* no wall clock yet — try again next quarter */
    s_hour_restore_tried[unit_idx] = true;
    nvs_handle_t nh;
    if (nvs_open("rbhour", NVS_READONLY, &nh) != ESP_OK) return;
    char key[8]; snprintf(key, sizeof(key), "u%u", (unsigned)unit_idx);
    static rbhome_hour_blob_t b;   /* off the stack (rollup holds the home lock) */
    size_t len = sizeof(b);
    esp_err_t e = nvs_get_blob(nh, key, &b, &len);
    nvs_close(nh);
    if (e != ESP_OK) return;
    hour_persist_erase(unit_idx);                         /* consumed either way; a fresh save follows */
    uint16_t real = (uint16_t)g_home_network.units[unit_idx].definition.unit_id;
    if (len != sizeof(b) || b.magic != RBHOME_HOUR_MAGIC || b.unit_id != real) return;
    rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_idx][RBPOWER_STAT_PERIOD_1HOUR];
    bool enabled = acc->is_enabled;
    /* Quarters already folded in while the wall clock was not valid (power cut that took the router down too:
     * no NTP at the first collects). A plain "*acc = image" erased them — Wh missing from HOUR/DAY while the
     * lifetime totals had them. With live quarters present, the image goes out as its
     * own PARTIAL record and the live accumulator is kept: DAY sums both, nothing lost, nothing twice. */
    const bool live = acc->is_active && acc->metadata.input_periods_received > 0;
    static rbpower_period_accumulator_t live_acc;   /* static, not on the stats task stack (rollup runs under the home lock) */
    if (live) live_acc = *acc;
    if (b.hour_ts != h || live) {
        /* Downtime crossed the hour boundary: the image belongs to an hour that is over. Write it as
         * that hour's PARTIAL record (its own ts; dur = quarters × 900) instead of dropping the
         * pre-outage quarters. The energy of the outage itself is credited by the
         * module counters (reboot-gap tail) into the current hour. A future-dated image (clock went
         * back) or an empty one is still dropped. */
        uint32_t q = (uint32_t)b.acc.metadata.input_periods_received;
        uint32_t cov_s = (b.acc.metadata.covered_ms + 500u) / 1000u;
        if (cov_s == 0 || cov_s > 3600u) cov_s = (q * 900u > 3600u) ? 3600u : q * 900u;
        if (b.hour_ts <= h && q > 0) {  /* restore runs before this quarter is folded in (same as the same-hour path);
                                          q can exceed 4 (on-demand collects split windows) — never a reason to drop */
            /* q bits flagged for the CURRENT hour (module restart at join) must not ride out on this past image */
            const uint16_t q_extra_now = s_hour_q_extra[unit_idx];
            s_hour_q_extra[unit_idx] = 0;
            *acc = b.acc;
            acc->is_enabled = true;
            acc->is_active = true;
            uint64_t now_us = esp_timer_get_time();
            uint64_t span_us = (uint64_t)q * 900ULL * 1000000ULL;
            acc->time_boundaries.period_start_us = (now_us > span_us) ? now_us - span_us : 0;
            /* contract: record ts = END of the covered interval; the image holds the FIRST q quarters
             * of that hour → [hour_ts, hour_ts + q×900] (a natural PARTIAL after boot covers the LAST
             * quarters and ends at the hour boundary — same rule, different placement) */
            s_store_ts_override = b.hour_ts + cov_s;
            s_store_ts_override_period = RBPOWER_STAT_PERIOD_1HOUR;
            rbpower_finalize_period_callback_forced(unit_idx, RBPOWER_STAT_PERIOD_1HOUR, now_us);
            s_store_ts_override = 0;
            acc->is_enabled = enabled;
            s_hour_q_extra[unit_idx] |= q_extra_now;
            if (live) *acc = live_acc;              /* E5: the post-boot quarters continue the current hour */
            ESP_LOGW("RBHOME-AGG", "Unit[%u/0x%04X]: open-hour image from %lu written as PARTIAL (%lu quarter(s), E=%.3f/%.3f Wh); now in hour %lu",
                     unit_idx, real, (unsigned long)b.hour_ts, (unsigned long)q,
                     b.acc.energy.consumed_wh_accumulator, b.acc.energy.supplied_wh_accumulator, (unsigned long)h);
            rbpower_diag_printf("hour image 0x%04X ts=%lu written PARTIAL quarters=%lu dur=%lus E=%.3fWh (downtime crossed the hour)",
                                (unsigned)real, (unsigned long)(b.hour_ts + cov_s), (unsigned long)q, (unsigned long)cov_s,
                                b.acc.energy.consumed_wh_accumulator);
        } else {
            ESP_LOGW("RBHOME-AGG", "Unit[%u/0x%04X]: open-hour image from %lu dropped (now in hour %lu; %lu quarter(s))",
                     unit_idx, real, (unsigned long)b.hour_ts, (unsigned long)h, (unsigned long)q);
            rbpower_diag_printf("FAIL hour image 0x%04X ts=%lu dropped (now %lu, quarters=%lu)", (unsigned)real,
                                (unsigned long)b.hour_ts, (unsigned long)h, (unsigned long)q);
        }
        return;
    }
    *acc = b.acc;
    acc->is_enabled = enabled;
    acc->is_active = true;
    acc->metadata.ready_for_finalization = false;
    uint64_t now_us = esp_timer_get_time();
    uint64_t into_hour_us = (rbtime_get_unix_timestamp() - (uint64_t)h) * 1000000ULL;
    acc->time_boundaries.period_start_us = (now_us > into_hour_us) ? now_us - into_hour_us : 0;
    ESP_LOGW("RBHOME-AGG", "Unit[%u/0x%04X]: open hour %lu restored (%lu quarter(s), E=%.3f/%.3f Wh)",
             unit_idx, real, (unsigned long)h, (unsigned long)acc->metadata.input_periods_received,
             acc->energy.consumed_wh_accumulator, acc->energy.supplied_wh_accumulator);
    rbpower_diag_printf("hour 0x%04X open hour ts=%lu restored after restart: quarters=%lu E=%.3fWh", (unsigned)real,
                        (unsigned long)h, (unsigned long)acc->metadata.input_periods_received, acc->energy.consumed_wh_accumulator);
    /* the image was consumed above; restored at boot (not at a quarter), re-save it at once so a second
     * restart before the next quarter does not lose it */
    s_hour_of_acc[unit_idx] = h;
    hour_persist(unit_idx);
}

/* Unit removed (contract in rbhome_types.h): drop its open-hour NVS image + pending q bits. The image is
 * keyed by units[] index and guarded only by unit_id, so a unit re-created with the same id in the same
 * index would otherwise restore the deleted unit's quarters after the next boot. Caller holds the home lock. */
void rbpower_stats_forget_unit(uint8_t unit_idx)
{
    if (unit_idx >= RBPOWER_MAX_HOME_UNITS) return;
    hour_persist_erase(unit_idx);
    s_hour_q_extra[unit_idx] = 0;
}

/* Planned restart — contract in rbhome_types.h. Was: finalize open hours as PARTIAL records (which
 * left a duplicate hour once the post-boot remainder closed as a second record). Now: persist the
 * open accumulators; the first quarter after boot restores them into the same hour. */
int rbpower_stats_flush_open_hours(void)
{
    int saved = 0;
    rbpower_home_lock();
    for (uint8_t u = 0; u < RBPOWER_MAX_HOME_UNITS; u++) {
        if (!g_home_network.units[u].has_valid_definition) continue;
        rbpower_period_accumulator_t* acc = &g_period_accumulators[u][RBPOWER_STAT_PERIOD_1HOUR];
        if (!acc->is_enabled || !acc->is_active || acc->metadata.input_periods_received == 0) continue;
        hour_persist(u);
        saved++;
    }
    rbpower_home_unlock();
    if (saved) {
        ESP_LOGW("RBHOME-AGG", "planned restart: persisted %d open HOUR accumulator(s) (restored after boot within the same hour)", saved);
    }
    return saved;
}

/* Stats-task service, ~1 s (runs here, not in the housekeeping tick: that one is also hosted by the esp_timer
 * task, whose stack is too small for flash writes):
 *  1. Open-hour restore of EVERY unit as soon as the wall clock is trusted, i.e. at boot / unit join — not at the
 *     unit's first quarter: a hub reset at hh:54 restored the main supply only at the hh:00 collect, where it
 *     already counted as a PAST hour → a PARTIAL record stamped at the reset time + a second record for the same
 *     hour.
 *  2. Boundary timeout: an hour whose boundary passed RBSTATS_HOUR_CLOSE_TIMEOUT_S ago without the quarter that
 *     closes it (injected units close their hour right after the boundary quarter; module unreachable, collect
 *     failed, timer fired a moment early) is closed now instead of at the next quarter. */
#define RBSTATS_HOUR_CLOSE_TIMEOUT_S 20 ///< Close an hour this long after its boundary without the closing quarter, s
static void rbpower_stats_service_tick(void)
{
    static int64_t s_last_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (s_last_us != 0 && now_us - s_last_us < 1000000LL) return;
    s_last_us = now_us;
    if (!g_home_network_initialized) return;
    time_t now = rbpower_stats_clock_now();
    if (!rbpower_stats_clock_trusted(now)) return;
    if (!rbpower_home_lock_timeout(500)) return;   /* stats task is TWDT-supervised: never block on a long holder */
    for (uint8_t u = 0; u < g_home_network.units_count && u < RBPOWER_MAX_HOME_UNITS; u++) {
        if (!g_home_network.units[u].has_valid_definition) continue;
        rbpower_period_accumulator_t* ah = &g_period_accumulators[u][RBPOWER_STAT_PERIOD_1HOUR];
        /* same depth-derived enable as the rollup (rbAmp fleet units get it only at their first quarter) */
        ah->is_enabled = (RBPOWER_STAT_PERIOD_1HOUR <= g_home_network.units[u].definition.stat_class);
        if (!ah->is_enabled) continue;
        if (!s_hour_restore_tried[u]) hour_restore_once(u);
#if !(RBGRID_STATS_TIME_SCALE > 1)
        if (s_bucket_init[u] && s_bucket_clock_valid[u] && ah->is_active && ah->metadata.input_periods_received > 0) {
            uint64_t hb = (uint64_t)now / 3600ULL;
            /* the open accumulator's own hour (restored image / boundary-stamped quarter), else the bucket baseline */
            uint64_t acc_hb = s_hour_of_acc[u] ? s_hour_of_acc[u] / 3600u : s_last_bucket[u][RBSTATS_BK_HOUR];
            if (hb > acc_hb && (uint64_t)now >= hb * 3600ULL + RBSTATS_HOUR_CLOSE_TIMEOUT_S) {
                rbpower_diag_printf("hour 0x%04X: no boundary quarter %ds after hh:00 — closing the hour without it",
                                    (unsigned)g_home_network.units[u].definition.unit_id, RBSTATS_HOUR_CLOSE_TIMEOUT_S);
                if (hb > s_last_bucket[u][RBSTATS_BK_HOUR]) rollup_dispatch_buckets(u);
                else close_open_hour(u, acc_hb);   /* baseline taken after the boundary (boot at hh:59:59) */
            }
        }
#endif
    }
    rbpower_home_unlock();
}

/**
 * @brief Finalize daily period and calculate results
 */
/* ===================================================================================================
 * FLASH-RECOMPUTE FINALIZERS (DAY / WEEK / MONTH / YEAR)
 * These periods keep NO RAM accumulator — each is recomputed at its calendar-bucket boundary by
 * reading recent child records back from the flash ring buffer and rolling up ONLY those whose
 * mid-point falls inside the closed parent bucket (HOUR->DAY, DAY->WEEK, DAY->MONTH, MONTH->YEAR).
 * finalize_flash_recompute_period is the single shared implementation; the four finalize_1Xperiod
 * entry points below are thin wrappers differing in {target period, child flash period, read depth,
 * parent bucket index, nominal child count, log tag}.
 *
 * These were four ~300-line copy-paste siblings that had DIVERGED: DAY alone lacked reactive/apparent/
 * power-factor aggregation, did not compute avg_tariff, summed economics in a uint32 (overflow risk),
 * did NOT skip uninitialized (0xFFFFFFFF) electrical records, and LEAKED the heap buffers on one early
 * return. This unified core adopts the consistent WEEK/MONTH/YEAR behavior, which normalizes DAY and
 * fixes those bugs. It is therefore NOT a pure no-op for DAY — acceptable because the flash-recompute
 * (day+) path is triggered by the bucket boundary dispatcher; normalizing it here keeps all
 * DAY+ periods consistent.
 * =================================================================================================== */
/* Does a child record belong to the parent calendar bucket being closed? (Without this filter
 * MONTH summed the "last 30 DAY records" → Feb borrowed 2 days of January, 31-day months dropped a
 * day, YEAR ended −1.1%. Same latent bug for DAY across a reboot gap and the first partial YEAR.)
 * Record ts = period END (store lags the boundary by up to ~1 quarter), so attribute by the child's
 * MID-point: ts − half the child's nominal span lands inside the true period for any lateness < half
 * a span. Uninitialized/uptime-era timestamps map to 1970 buckets and drop out naturally. */
static bool child_in_bucket(uint32_t ts, int64_t child_half_span,
                            rbstats_bucket_idx_t parent_bk_idx, uint64_t parent_bucket)
{
    if (ts == 0 || ts == 0xFFFFFFFF) {
        return false;
    }
    int64_t mid = (int64_t)ts - child_half_span;
    if (mid < 0) {
        mid = 0;
    }
    time_t t = (time_t)mid;
    struct tm lt;
    localtime_r(&t, &lt);
    uint64_t bk[RBSTATS_BK_COUNT];
    rbpower_stats_calc_buckets(mid, &lt, bk);
    return bk[parent_bk_idx] == parent_bucket;
}

static void finalize_flash_recompute_period(rbpower_unit_id_t unit_id,
                                            rbpower_stat_period_type_t target_stat_period,
                                            rbpower_period_t source_flash,
                                            uint32_t max_records,
                                            rbstats_bucket_idx_t parent_bk_idx,
                                            uint64_t parent_bucket,
                                            uint32_t nominal_children,
                                            const char* tag)
{
    /* Child mid-point offset = half the child period's nominal span (month ≈ 15 days). */
    int64_t child_half_span = (source_flash == RBPOWER_PERIOD_HOUR)  ? 1800
                            : (source_flash == RBPOWER_PERIOD_DAY)   ? 43200
                            : (source_flash == RBPOWER_PERIOD_MONTH) ? 1296000
                            : 1800;

    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return;
    }
    if (!g_home_network.units[unit_id].has_valid_definition) {
        return;
    }

    rbpower_unit_id_t real_unit_id = g_home_network.units[unit_id].definition.unit_id;
    rbpower_period_statistics_t* stats = &g_home_network.units[unit_id].statistics.periods[target_stat_period];

    ESP_LOGI(tag, "Unit[%d/0x%04X]: %s finalization (bucket %llu; scanning last %lu Flash records)",
             unit_id, real_unit_id, tag, (unsigned long long)parent_bucket, (unsigned long)max_records);

    esp_task_wdt_reset();

    // Heap (not stack) to avoid task stack overflow on the per-record arrays.
    // Children are read as v4 structs (the flash layer lifts v3 records transparently),
    // incl. the exact Σdur of the children.
    /* Child buffers live in PSRAM (CPU-only reads; WEEK/MONTH read the whole DAY ring): keeps the
     * ~6-20 KB transient out of the 60-70 KB internal heap the WiFi/HTTP stack lives in (the
     * internal minimum could drop to 48 KB at a day boundary). Fallback to internal if PSRAM is absent. */
    rbstats_electrical_v4_t* ch_electrical = (rbstats_electrical_v4_t*)heap_caps_malloc(max_records * sizeof(rbstats_electrical_v4_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rbstats_energy_v4_t*     ch_energy     = (rbstats_energy_v4_t*)heap_caps_malloc(max_records * sizeof(rbstats_energy_v4_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rbstats_econ_v4_t*       ch_economics  = (rbstats_econ_v4_t*)heap_caps_malloc(max_records * sizeof(rbstats_econ_v4_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ch_electrical) ch_electrical = (rbstats_electrical_v4_t*)malloc(max_records * sizeof(rbstats_electrical_v4_t));
    if (!ch_energy)     ch_energy     = (rbstats_energy_v4_t*)malloc(max_records * sizeof(rbstats_energy_v4_t));
    if (!ch_economics)  ch_economics  = (rbstats_econ_v4_t*)malloc(max_records * sizeof(rbstats_econ_v4_t));
    if (!ch_electrical || !ch_energy || !ch_economics) {
        ESP_LOGE(tag, "Memory allocation error for %s aggregation", tag);
        free(ch_electrical); free(ch_energy); free(ch_economics);
        return;
    }

    // === READ child records from flash (index 0 = newest; ring logic handled by the flash layer) ===
    uint32_t electrical_records_read = 0, energy_records_read = 0, econ_records_read = 0;
    esp_err_t err = rbstats_flash_read_v4(real_unit_id, source_flash, RBPOWER_DATA_TYPE_ELECTRICAL,
                                          0, max_records, ch_electrical, &electrical_records_read);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "Error reading ELECTRICAL from Flash (0x%X) - %s aborted", err, tag);
        free(ch_electrical); free(ch_energy); free(ch_economics);
        return;
    }
    esp_task_wdt_reset();
    err = rbstats_flash_read_v4(real_unit_id, source_flash, RBPOWER_DATA_TYPE_ENERGY,
                                0, max_records, ch_energy, &energy_records_read);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "Error reading ENERGY from Flash (0x%X) - %s aborted", err, tag);
        free(ch_electrical); free(ch_energy); free(ch_economics);
        return;
    }
    esp_task_wdt_reset();
    err = rbstats_flash_read_v4(real_unit_id, source_flash, RBPOWER_DATA_TYPE_ECONOMICS,
                                0, max_records, ch_economics, &econ_records_read);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "Error reading ECONOMICS from Flash (0x%X) - %s aborted", err, tag);
        free(ch_electrical); free(ch_energy); free(ch_economics);
        return;
    }
    esp_task_wdt_reset();

    // === RESULT init ===
    memset(stats, 0, sizeof(rbpower_period_statistics_t));
    stats->period_start_us = esp_timer_get_time();
    stats->period_end_us = esp_timer_get_time();

    // === ELECTRICAL aggregation (RMS V/I via sum-of-squares; linear P/Q/S/PF/freq) ===
    double voltage_sum_squares = 0.0, current_sum_squares = 0.0;
    double power_active_sum = 0.0, power_reactive_sum = 0.0, power_apparent_sum = 0.0;
    double power_factor_sum = 0.0, frequency_sum = 0.0;
    uint32_t valid_electrical_records = 0;
    /* Children are weighted by their covered duration (a 901-s PARTIAL hour must not count like a full
     * one), PF of the parent = Σ P / Σ S (power-weighted, not the mean of child PFs — a near-zero-load
     * child with pf 0 can drag a day to 0.67 while its hours read 0.94–0.96), and the envelope
     * (P/V/I min/max) is carried up — otherwise the DAY+ record has p_min = p_max = 0. */
    double w_sum = 0.0;
    float  pmin = INFINITY, pmax = -INFINITY, vmin = INFINITY, vmax = 0.0f, imin = INFINITY, imax = 0.0f;
    for (uint32_t i = 0; i < electrical_records_read; i++) {
        if (!child_in_bucket(ch_electrical[i].h.ts, child_half_span, parent_bk_idx, parent_bucket)) {
            continue;  // uninitialized, or belongs to another calendar bucket
        }
        double w = ch_electrical[i].h.dur ? (double)ch_electrical[i].h.dur : (double)(2 * child_half_span);
        float voltage = ch_electrical[i].voltage_rms_v10 / 10.0f;
        float current = ch_electrical[i].current_rms_ma / 1000.0f;   // mA -> A
        voltage_sum_squares += w * (double)voltage * voltage;
        current_sum_squares += w * (double)current * current;
        power_active_sum   += w * (ch_electrical[i].power_active_w10 / 10.0f);
        power_reactive_sum += w * (ch_electrical[i].power_reactive_var10 / 10.0f);
        power_apparent_sum += w * (ch_electrical[i].power_apparent_va10 / 10.0f);
        power_factor_sum   += w * (ch_electrical[i].power_factor_100 / 100.0f);
        frequency_sum      += w * ch_electrical[i].frequency_hz10;   // RAW Hz (uint8 despite _hz10 name)
        w_sum += w;
        float cpmin = ch_electrical[i].power_min_w10 / 10.0f, cpmax = ch_electrical[i].power_max_w10 / 10.0f;
        if (cpmin < pmin) pmin = cpmin;
        if (cpmax > pmax) pmax = cpmax;
        float cvmin = ch_electrical[i].voltage_min_v10 / 10.0f, cvmax = ch_electrical[i].voltage_max_v10 / 10.0f;
        if (cvmin > 1.0f && cvmin < vmin) vmin = cvmin;
        if (cvmax > vmax) vmax = cvmax;
        float cimin = ch_electrical[i].current_min_ma / 1000.0f, cimax = ch_electrical[i].current_max_ma / 1000.0f;
        if (cimin < imin) imin = cimin;
        if (cimax > imax) imax = cimax;
        valid_electrical_records++;
    }
    if (valid_electrical_records > 0 && w_sum > 0.0) {
        stats->voltage_rms        = sqrtf(voltage_sum_squares / w_sum);
        stats->current_rms        = sqrtf(current_sum_squares / w_sum);
        stats->power_active_avg   = power_active_sum   / w_sum;
        stats->power_reactive_avg = power_reactive_sum / w_sum;
        stats->power_apparent_avg = power_apparent_sum / w_sum;
        stats->power_factor_avg   = (power_apparent_sum > 0.0) ? (float)(fabs(power_active_sum) / power_apparent_sum)
                                                               : (float)(power_factor_sum / w_sum);
        if (stats->power_factor_avg > 1.0f) stats->power_factor_avg = 1.0f;
        stats->frequency_avg      = frequency_sum      / w_sum;
        if (isfinite(pmin)) stats->extremes.power_min = pmin;
        if (isfinite(pmax)) stats->extremes.power_max = pmax;
        if (isfinite(vmin)) stats->extremes.voltage_min = vmin;
        stats->extremes.voltage_max = vmax;
        if (isfinite(imin)) stats->extremes.current_min = imin;
        stats->extremes.current_max = imax;
    }
    esp_task_wdt_reset();

    // === ENERGY aggregation (sum) — dual validity check (timestamp AND field != 0xFFFFFFFF) ===
    // Sum in int64 mWh and convert ONCE — a per-child
    // `float += ec_mwh/1000` accumulated float rounding (32 Wh steps at YEAR scale ~3.8e8 Wh).
    uint32_t valid_energy_records = 0;
    uint64_t children_dur_sum = 0;   /* Σ child dur — exact coverage once children are true v4 */
    int64_t ec_sum_mwh = 0, es_sum_mwh = 0;
    for (uint32_t i = 0; i < energy_records_read; i++) {
        if (!child_in_bucket(ch_energy[i].h.ts, child_half_span, parent_bk_idx, parent_bucket)) {
            continue;
        }
        if (ch_energy[i].h.q & RBSTATS_Q_SRC_GAP) {
            continue;   /* lifted uninitialized v3 slot (0xFFFFFFFF fields) */
        }
        ec_sum_mwh += ch_energy[i].ec_mwh;
        es_sum_mwh += ch_energy[i].es_mwh;
        children_dur_sum += ch_energy[i].h.dur;
        valid_energy_records++;
    }
    stats->energy_period.consumed_wh = (float)((double)ec_sum_mwh / 1000.0);
    stats->energy_period.supplied_wh = (float)((double)es_sum_mwh / 1000.0);
    stats->energy_period.net_wh = (float)((double)(ec_sum_mwh - es_sum_mwh) / 1000.0);
    stats->exact.ec_mwh = ec_sum_mwh;
    stats->exact.es_mwh = es_sum_mwh;   /* valid set after the cents below — never half-filled */
    esp_task_wdt_reset();

    // === ECONOMICS aggregation (sum of minor units -> currency units) + average tariff ===
    uint64_t total_cost_kopecks = 0, total_revenue_kopecks = 0;
    uint32_t valid_econ_records = 0;
    for (uint32_t i = 0; i < econ_records_read; i++) {
        if (!child_in_bucket(ch_economics[i].h.ts, child_half_span, parent_bk_idx, parent_bucket)) {
            continue;
        }
        if (ch_economics[i].h.q & RBSTATS_Q_SRC_GAP) {
            continue;   /* lifted uninitialized v3 slot */
        }
        total_cost_kopecks    += (uint64_t)ch_economics[i].c_cents;
        total_revenue_kopecks += (uint64_t)ch_economics[i].r_cents;
        valid_econ_records++;
    }
    /* double division (was /100.0f: float conversion of the cent sum is exact only to 2^24 cents
     * = 167k major units); the exact cent sums also ride to the v4 record via stats->exact. */
    stats->result_base.cost_consumed    = (float)((double)total_cost_kopecks / 100.0);
    stats->result_base.revenue_supplied = (float)((double)total_revenue_kopecks / 100.0);
    stats->exact.c_cents = (int64_t)total_cost_kopecks;
    stats->exact.r_cents = (int64_t)total_revenue_kopecks;
    stats->exact.valid = true;
    float total_energy = stats->energy_period.consumed_wh + stats->energy_period.supplied_wh;
    if (total_energy > 0) {
        stats->result_base.avg_tariff_per_kwh =
            (stats->result_base.cost_consumed + stats->result_base.revenue_supplied) / (total_energy / 1000.0f);
    }
    esp_task_wdt_reset();

    // === METADATA ===
    stats->samples_count = valid_energy_records;
    stats->valid_samples_count = valid_energy_records;
    // Completeness against the bucket's NOMINAL child count (24/7/30/12), not the read depth.
    uint32_t completeness = (nominal_children > 0) ? (valid_energy_records * 100 / nominal_children) : 0;
    stats->data_completeness_percent = (completeness > 100) ? 100 : completeness;

    // === Calendar-exact v4 record metadata for the CLOSED bucket ===
    // dur: complete bucket → the exact calendar span (23/25h DST days, 28-31d months); incomplete →
    // children × child-nominal (approximate until v4 children carry their own dur; exact for DAY
    // since HOUR children are always 3600 s). q: missing children → PARTIAL+SRC_GAP (contract §1);
    // a reboot INSIDE the bucket → RECOMPUTED (bit 4: recomputed from children after reboot).
    {
        int64_t b_start = 0, b_end = 0;
        stats->v4_q = 0;
        if (rbpower_stats_bucket_span(parent_bk_idx, parent_bucket, &b_start, &b_end)) {
            uint32_t expected;
            switch (parent_bk_idx) {
                case RBSTATS_BK_DAY:   expected = (uint32_t)((b_end - b_start) / 3600); break;
                case RBSTATS_BK_WEEK:  expected = 7;  break;
                case RBSTATS_BK_MONTH: expected = (uint32_t)((b_end - b_start + 43200) / 86400); break;
                case RBSTATS_BK_YEAR:  expected = 12; break;
                default:               expected = nominal_children; break;
            }
            /* Complete bucket → exact calendar span; incomplete → Σ child dur (nominal-based for
             * lifted v3 children, exact for true v4 children). */
            /* Children PRESENT is not children COMPLETE: a day with all 24 hour records but PARTIAL hours
             * (hub off / flashed inside them) covered 74 536 s, yet the record said dur 86400 q RECOMPUTED
             * only. dur = Σ child dur, clamped to the span; ≥ span − 60 s per
             * expected child → exactly the span (child jitter), anything less → PARTIAL. */
            uint64_t span = (uint64_t)(b_end - b_start);
            uint64_t tol  = (uint64_t)expected * 60u;
            uint64_t dsum = children_dur_sum > span ? span : children_dur_sum;
            stats->v4_dur_s = (uint32_t)((dsum + tol >= span) ? span : dsum);
            if (valid_energy_records < expected) {
                stats->v4_q |= RBSTATS_Q_PARTIAL | RBSTATS_Q_SRC_GAP;
            } else if (stats->v4_dur_s < span) {
                stats->v4_q |= RBSTATS_Q_PARTIAL;
            }
        } else {
            stats->v4_dur_s = (uint32_t)children_dur_sum;
        }
        int64_t up_s = (int64_t)(esp_timer_get_time() / 1000000ULL);
#if RBGRID_STATS_TIME_SCALE > 1
        up_s *= RBGRID_STATS_TIME_SCALE;   /* elapsed-since-boot in the same (virtual) timebase */
#endif
        int64_t boot_epoch = (int64_t)rbpower_stats_clock_now() - up_s;
        if (b_start > 0 && boot_epoch > b_start) {
            stats->v4_q |= RBSTATS_Q_RECOMPUTED;
        }
    }
    stats->period_complete = true;
    stats->last_update_us = esp_timer_get_time();
    stats->period_duration_ms = (stats->period_end_us - stats->period_start_us) / 1000;
    g_home_network.units[unit_id].statistics.last_period_update[target_stat_period] = stats->last_update_us;

    // === STORE the rolled-up record to flash ===
    esp_task_wdt_reset();
    /* Record ts = the bucket END (contract: ts = end of interval), not the store instant: the natural
     * dispatch runs at the first 15-min rollup after the boundary (seconds late), and a clock step in
     * between could put an Oct-15 DAY record at an Oct-14 ts. close_downtime_buckets()
     * pre-sets the override; keep it then. */
    bool own_ts_override = (s_store_ts_override == 0);
    if (own_ts_override) {
        int64_t b_start = 0, b_end = 0;
        if (rbpower_stats_bucket_span(parent_bk_idx, parent_bucket, &b_start, &b_end) && b_end > 0)
            { s_store_ts_override = (uint32_t)b_end; s_store_ts_override_period = target_stat_period; }
        else
            own_ts_override = false;
    }
    if (valid_energy_records == 0) {
        /* Nothing of this bucket on flash (hub was down, or the clock jumped over the bucket): no record —
         * storing here re-emitted the previous bucket's stale electrical envelope under a new ts
         * (two identical DAY records). */
        ESP_LOGW(tag, "Unit[%d/0x%04X]: bucket %llu has NO children on flash — no %s record written",
                 unit_id, real_unit_id, (unsigned long long)parent_bucket, tag);
        rbpower_diag_printf("%s 0x%04X bucket=%llu empty — no record", tag, (unsigned)real_unit_id, (unsigned long long)parent_bucket);
    } else {
        store_period_to_flash_sync(real_unit_id, target_stat_period, stats);
    }
    if (own_ts_override) s_store_ts_override = 0;
    esp_task_wdt_reset();

    ESP_LOGI(tag, "OK %s done: U=%.1fV I=%.2fA P=%.0fW | E_cons=%.0fWh E_supp=%.0fWh Net=%.0fWh | Cost=%.2f Rev=%.2f | %lu records (%u%%)",
             tag, stats->voltage_rms, stats->current_rms, stats->power_active_avg,
             stats->energy_period.consumed_wh, stats->energy_period.supplied_wh, stats->energy_period.net_wh,
             stats->result_base.cost_consumed, stats->result_base.revenue_supplied,
             (unsigned long)valid_energy_records, stats->data_completeness_percent);

    free(ch_electrical);
    free(ch_energy);
    free(ch_economics);
}

static void finalize_1day_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket)
{
    // DAY <- the closed day's HOUR records (read depth 48 covers a 25h DST day + lateness slack).
    finalize_flash_recompute_period(unit_id, RBPOWER_STAT_PERIOD_DAILY, RBPOWER_PERIOD_HOUR, 48,
                                    RBSTATS_BK_DAY, closed_bucket, 24, "RBHOME-AGG-DAY");
}

static void finalize_1week_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket)
{
    // WEEK <- the closed week's DAY records (full DAY ring read, bucket filter picks the 7).
    finalize_flash_recompute_period(unit_id, RBPOWER_STAT_PERIOD_WEEKLY, RBPOWER_PERIOD_DAY,
                                    RBPOWER_RECORDS_COUNT[RBPOWER_PERIOD_DAY],
                                    RBSTATS_BK_WEEK, closed_bucket, 7, "RBHOME-AGG-WEEK");
}

static void finalize_1month_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket)
{
    // MONTH <- the closed calendar month's DAY records. With the v4 retention (DAY ring = 400
    // records) a full 31-day month is available; the bucket filter prevents Feb double-counting and
    // cross-month borrowing.
    finalize_flash_recompute_period(unit_id, RBPOWER_STAT_PERIOD_MONTHLY, RBPOWER_PERIOD_DAY,
                                    RBPOWER_RECORDS_COUNT[RBPOWER_PERIOD_DAY],
                                    RBSTATS_BK_MONTH, closed_bucket, 30, "RBHOME-AGG-MONTH");
}

static void finalize_1year_period(rbpower_unit_id_t unit_id, uint64_t closed_bucket)
{
    // YEAR <- the closed year's MONTH records (full MONTH ring read, bucket filter picks the 12).
    finalize_flash_recompute_period(unit_id, RBPOWER_STAT_PERIOD_YEARLY, RBPOWER_PERIOD_MONTH,
                                    RBPOWER_RECORDS_COUNT[RBPOWER_PERIOD_MONTH],
                                    RBSTATS_BK_YEAR, closed_bucket, 12, "RBHOME-AGG-YEAR");
}

/*
 * @brief Universal callback for period finalization
 * Called from statistics_timer_callback for all periods except 3-sec
 */
void rbpower_finalize_period_callback(rbpower_unit_id_t unit_id, 
                                     rbpower_stat_period_type_t period_type, 
                                     uint64_t period_end_time_us)
{
    ESP_LOGD("RBHOME-AGG", "rbpower_finalize_period_callback: Unit[%d]: Period %d completed at %llu us", unit_id, period_type, period_end_time_us);
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return;
    }
    
    ESP_LOGD("RBHOME-AGG", "Unit[%d]: Period %d finalization at %llu us",
             unit_id, period_type, period_end_time_us);

    // Forced/manual path (statforce/forceperiod verbs; the natural DAY+ trigger is the bucket
    // dispatcher in rbpower_rollup_after_15min): sum the CURRENT (in-progress) calendar bucket.
    time_t bk_now = rbpower_stats_clock_now();
    struct tm bk_tm;
    localtime_r(&bk_now, &bk_tm);
    uint64_t bk[RBSTATS_BK_COUNT];
    rbpower_stats_calc_buckets(bk_now, &bk_tm, bk);

    switch (period_type) {
        case RBPOWER_STAT_PERIOD_15MIN:
            finalize_15min_period(unit_id);
            ESP_LOGD("RBHOME-AGG", "Unit[%d]: 15-min period finalized", unit_id);
            break;
            
        case RBPOWER_STAT_PERIOD_1HOUR:
            finalize_1hour_period(unit_id);
            ESP_LOGD("RBHOME-AGG", "Unit[%d]: Hourly period finalized", unit_id);
            break;
            
        case RBPOWER_STAT_PERIOD_DAILY:
            finalize_1day_period(unit_id, bk[RBSTATS_BK_DAY]);
            ESP_LOGD("RBHOME-AGG", "Unit[%d]: Daily period finalized", unit_id);
            break;
            
        case RBPOWER_STAT_PERIOD_WEEKLY:
            finalize_1week_period(unit_id, bk[RBSTATS_BK_WEEK]);
            ESP_LOGD("RBHOME-AGG", "Unit[%d]: Weekly period finalized", unit_id);
            break;

        case RBPOWER_STAT_PERIOD_MONTHLY:
            finalize_1month_period(unit_id, bk[RBSTATS_BK_MONTH]);
            ESP_LOGD("RBHOME-AGG", "Unit[%d]: Monthly period finalized", unit_id);
            break;

        case RBPOWER_STAT_PERIOD_YEARLY:
            finalize_1year_period(unit_id, bk[RBSTATS_BK_YEAR]);
            ESP_LOGD("RBHOME-AGG", "Unit[%d]: Yearly period finalized", unit_id);
            break;
            
        default:
            ESP_LOGW("RBHOME-AGG", "Unit[%d]: Unknown period type: %d", unit_id, period_type);
            break;
    }
}

/* FORCED finalize (test/diagnostics: statforce1h / forceperiod). The normal finalize_1Xperiod() functions
 * EARLY-RETURN without storing when the accumulator hasn't received the auto-threshold number of
 * input periods (HOUR needs >=3 fifteen-min rollups; ready_for_finalization=false) — and the
 * verb would still report "finalized (store+rollup)", a FALSE success that makes flash-persistence
 * testing impossible (store silently skipped, statread/statcursor empty). This variant ARMS readiness for the target period so the finalize proceeds to the
 * flash store on whatever has accumulated so far (data_completeness_percent reflects partiality).
 * is_active is NOT forced — if no data was ever aggregated the store is still (correctly) skipped.
 * The PRODUCTION auto-finalize path (rbpower_finalize_period_callback) is unchanged and still
 * respects the threshold, so partial periods are not auto-persisted. */
int rbpower_finalize_period_callback_forced(rbpower_unit_id_t unit_id,
                                            rbpower_stat_period_type_t period_type,
                                            uint64_t period_end_time_us)
{
    if (unit_id < RBPOWER_MAX_HOME_UNITS && period_type < RBPOWER_RAM_PERIOD_COUNT) {  // accumulator only exists for RAM periods
        rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period_type];
        if (acc->metadata.input_periods_received == 0) {
            /* Nothing accumulated (module absent, no collect yet): a forced store would write an empty
             * record (e.g. statforce1h with the module off the bus → hour i=0 p=0). */
            ESP_LOGW("RBHOME-AGG", "Unit[%d]: FORCED finalize period %d refused — no inputs, nothing to store", unit_id, period_type);
            return 0;
        }
        acc->is_enabled = true;
        acc->metadata.ready_for_finalization = true;
        ESP_LOGW("RBHOME-AGG", "Unit[%d]: FORCED finalize period %d (readiness armed; "
                 "received=%lu, is_active=%d) — will store partial if active",
                 unit_id, period_type,
                 (unsigned long)acc->metadata.input_periods_received, (int)acc->is_active);
    }
    rbpower_finalize_period_callback(unit_id, period_type, period_end_time_us);
    return 1;
}

//=============================================================================
// AGGREGATED STATISTICS ACCESS API
//=============================================================================

/*
 * Get the statistics of any period of a unit (see rbhome_types.h).
 */
rbpower_err_t rbpower_get_unit_period_statistics(rbpower_unit_id_t unit_id, 
                                                rbpower_stat_period_type_t period_type,
                                                rbpower_period_statistics_t* stats)
{
    if (!stats) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_home_network.units[unit_id].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Copy period statistics
    memcpy(stats, &g_home_network.units[unit_id].statistics.periods[period_type], 
           sizeof(rbpower_period_statistics_t));
    
    return RBPOWER_OK;
}

/*
 * @brief Check if period is ready for use
 */
bool rbpower_is_period_ready(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return false;
    }
    
    if (!g_home_network.units[unit_id].has_valid_definition) {
        return false;
    }
    
    const rbpower_period_statistics_t* stats = 
        &g_home_network.units[unit_id].statistics.periods[period_type];
    
    return stats->period_complete && (stats->data_completeness_percent >= 50);
}

/*
 * @brief Get period readiness percentage
 */
float rbpower_get_period_completeness(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type)
{
    // Completeness is an ACCUMULATOR concept (how full the running sum is) → only meaningful for the
    // 3 RAM-accumulated periods. day+ (flash-recompute) have no accumulator → 0 (and keeps access in-bounds).
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || period_type >= RBPOWER_RAM_PERIOD_COUNT) {
        return 0.0f;
    }

    if (!g_period_accumulators[unit_id][period_type].is_enabled) {
        return 0.0f;
    }
    
    const rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period_type];
    
    if (acc->metadata.input_periods_expected == 0) {
        return 0.0f;
    }
    
    return (acc->metadata.input_periods_received * 100.0f) / acc->metadata.input_periods_expected;
}

/*
 * @brief Print aggregation system diagnostics
 */
void rbpower_print_aggregation_diagnostics(rbpower_unit_id_t unit_id)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        ESP_LOGW("RBHOME-AGG", "Invalid unit_id: %d", unit_id);
        return;
    }
    
    if (!g_home_network.units[unit_id].has_valid_definition) {
        ESP_LOGW("RBHOME-AGG", "Unit[%d]: Not defined", unit_id);
        return;
    }
    
    const char* unit_name = g_home_network.units[unit_id].definition.name;
    ESP_LOGI("RBHOME-AGG", "=== AGGREGATION DIAGNOSTICS Unit[%d] '%s' ===", unit_id, unit_name);
    
    const char* period_names[] = {
        "3sec", "15min", "1hour", "1day", "1week", "1month", "1year"
    };
    
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_RAM_PERIOD_COUNT; period++) {  // accumulator diag: RAM periods only
        const rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period];
        const rbpower_period_statistics_t* stats = &g_home_network.units[unit_id].statistics.periods[period];

        if (!acc->is_enabled) {
            ESP_LOGD("RBHOME-AGG", "  %s: DISABLED", period_names[period]);
            continue;
        }
        
        float completeness = rbpower_get_period_completeness(unit_id, period);
        // bool ready = rbpower_is_period_ready(unit_id, period);
        
        ESP_LOGD("RBHOME-AGG", "  %s: active=%s, readiness=%.1f%%, iterations=%lu",
                 period_names[period],
                 acc->is_active ? "YES" : "NO", 
                 completeness,
                 acc->metadata.aggregation_iteration);
        
        if (stats->period_complete) {
            ESP_LOGD("RBHOME-AGG", "    └─ Last: U=%.1fV I=%.2fA P=%.0fW E=%.1fWh (%u%%)",
                     stats->voltage_rms, stats->current_rms, 
                     stats->power_active_avg, stats->energy_period.consumed_wh,
                     stats->data_completeness_percent);
        }
    }
    
    ESP_LOGI("RBHOME-AGG", "=== END DIAGNOSTICS Unit[%d] ===", unit_id);
}

/*
 * @brief Print aggregation diagnostics for all units
 */
void rbpower_print_all_aggregation_diagnostics(void)
{
    if (!g_home_network_initialized) {
        ESP_LOGW("RBHOME-AGG", "Home network not initialized");
        return;
    }
    
    ESP_LOGI("RBHOME-AGG", "=== OVERALL AGGREGATION DIAGNOSTICS ===");
    ESP_LOGI("RBHOME-AGG", "Active units: %d", g_home_network.units_count);
    
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition) {
            rbpower_print_aggregation_diagnostics(i);
        }
    }
    
    ESP_LOGI("RBHOME-AGG", "=== END OVERALL DIAGNOSTICS ===");
}

/**
 * @brief Asynchronous task for writing data to flash
 */
static void flash_write_task(void* pvParameters)
{
    ESP_LOGI("RBSTORAGE_TASK", "Starting asynchronous flash write task");
    
    // Register task in WDT (timeout 30 seconds)
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW("RBSTORAGE_TASK", "Failed to register task in WDT: 0x%x", wdt_ret);
    } else {
        ESP_LOGI("RBSTORAGE_TASK", "Task registered in WDT");
    }
    
    g_flash_write_task_running = true;
    flash_write_request_t write_request;
    
    while (g_flash_write_task_running) {
        // Wait for a write request (WITHOUT suspending the scheduler!)
        if (xQueueReceive(g_flash_write_queue, &write_request, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGD("RBSTORAGE_TASK", "Write request received for unit %04X, period %d", 
                     write_request.unit_id, write_request.period_type);
            
            // Temporarily raise the priority to minimize preemption
            UBaseType_t original_priority = uxTaskPriorityGet(NULL);
            vTaskPrioritySet(NULL, configMAX_PRIORITIES - 2);
            
            // IMPORTANT: do NOT use vTaskSuspendAll() around file operations!
            // Use critical sections only for the most critical parts instead
            
            // Write with the raised priority:
            // this minimizes preemption without suspending the scheduler
            store_period_to_flash_sync(write_request.unit_id, 
                                     write_request.period_type, 
                                     &write_request.period_stats);
            
            // Restore the priority
            vTaskPrioritySet(NULL, original_priority);
            
            // Let other tasks run
            vTaskDelay(1);
        }
        
        // Feed the task watchdog
        if (wdt_ret == ESP_OK) {
            esp_task_wdt_reset();
        }
        
        // Yield the CPU to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Unregister from the task watchdog
    if (wdt_ret == ESP_OK) {
        esp_task_wdt_delete(NULL);
        ESP_LOGI("RBSTORAGE_TASK", "Task removed from WDT");
    }
    
    ESP_LOGI("RBSTORAGE_TASK", "Asynchronous flash write task exiting");
    vTaskDelete(NULL);
}

/**
 * @brief Initialize the asynchronous flash write system (queue + low-priority task on core 0).
 */
static esp_err_t init_async_flash_write_system(void)
{
    // Is storage enabled?
    if (!g_storage_enabled) {
        ESP_LOGD("RBSTORAGE_TASK", "Storage disabled - skipping async flash system init");
        return ESP_OK;
    }
    
    if (g_flash_write_queue != NULL) {
        return ESP_OK; // Already initialized
    }
    
    // Create the queue
    g_flash_write_queue = xQueueCreate(FLASH_WRITE_QUEUE_SIZE, sizeof(flash_write_request_t));
    if (g_flash_write_queue == NULL) {
        ESP_LOGE("RBSTORAGE_TASK", "Failed to create the flash write queue");
        return ESP_ERR_NO_MEM;
    }
    
    // Create a low-priority task on core 0 (PRO CPU), keeping core 1 free for
    // time-critical work
    BaseType_t task_created = xTaskCreatePinnedToCore(
        flash_write_task,
        "flash_write_task", 
        4096,  // Stack size
        NULL,  // Parameters
        1,     // Priority (low)
        &g_flash_write_task_handle,
        0      // Core 0 (PRO CPU) - core for non-critical work
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE("RBSTORAGE_TASK", "Failed to create the flash write task");
        vQueueDelete(g_flash_write_queue);
        g_flash_write_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI("RBSTORAGE_TASK", "Asynchronous flash write system initialized");
    return ESP_OK;
}

// LEGACY: asynchronous LittleFS write (NO LONGER USED - replaced by store_period_to_flash_sync)
// /**
//  * @brief Asynchronously write a period to flash storage
//  */
// static esp_err_t store_period_to_flash(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type,
//                                        const rbpower_period_statistics_t* period_stats)
// {
//     if (!g_storage_enabled) {
//         ESP_LOGD("RBSTORAGE_ASYNC", "Storage disabled - skipping write for unit %04X period %d",
//                  unit_id, period_type);
//         return ESP_OK;
//     }
//     if (unit_id >= RBPOWER_MAX_HOME_UNITS || period_stats == NULL) {
//         ESP_LOGW("RBSTORAGE_ASYNC", "Invalid parameters for asynchronous write");
//         return ESP_ERR_INVALID_ARG;
//     }
//     if (g_flash_write_queue == NULL) {
//         esp_err_t ret = init_async_flash_write_system();
//         if (ret != ESP_OK) {
//             ESP_LOGE("RBSTORAGE_ASYNC", "Write system init error: 0x%x", ret);
//             return ret;
//         }
//     }
//     flash_write_request_t write_request = {
//         .unit_id = unit_id,
//         .period_type = period_type,
//         .period_stats = *period_stats,
//         .timestamp_us = esp_timer_get_time()
//     };
//     if (xQueueSend(g_flash_write_queue, &write_request, 0) != pdTRUE) {
//         ESP_LOGW("RBSTORAGE_ASYNC", "Write queue full for unit %04X, period %d",
//                  unit_id, period_type);
//         return ESP_ERR_NO_MEM;
//     }
//     ESP_LOGD("RBSTORAGE_ASYNC", "Write request queued for unit %04X, period %d",
//              unit_id, period_type);
//     return ESP_OK;
// }

/* Fixed-point transfer encode helpers: plain casts would
 * TRUNCATED (−½ LSB systematic bias) and silently overflowed on out-of-range values (float→int
 * out-of-range conversion is UB in C; power_active_w10 is i16 → ±3276.7 W ceiling, real at any
 * >3.2 kW line). Saturate at the field range and round-to-nearest. The ceilings themselves are a
 * v3/v4 FORMAT property — widening needs a record layout change, not a change here. */
static inline uint16_t enc_u16(float v, float scale)
{
    float x = v * scale;
    if (!(x > 0.0f)) return 0;                 /* also catches NaN */
    if (x >= 65535.0f) return 65535;
    return (uint16_t)lroundf(x);
}
static inline int16_t enc_i16(float v, float scale)
{
    float x = v * scale;
    if (x != x) return 0;                      /* NaN */
    if (x >= 32767.0f) return 32767;
    if (x <= -32768.0f) return -32768;
    return (int16_t)lroundf(x);
}
static inline int8_t enc_i8(float v, float scale)
{
    float x = v * scale;
    if (x != x) return 0;
    if (x >= 127.0f) return 127;
    if (x <= -128.0f) return -128;
    return (int8_t)lroundf(x);
}
static inline uint8_t enc_u8(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)lroundf(v);
}
static inline uint32_t enc_u32(float v, float scale)
{
    float x = v * scale;
    if (!(x > 0.0f)) return 0;
    if (x >= 4294967040.0f) return UINT32_MAX; /* largest float below 2^32 */
    return (uint32_t)llroundf(x);
}

/**
 * @brief Write a finalized period to flash storage synchronously (task context).
 *
 * Called after each persisted period is finalized (HOUR and longer) to append the
 * record set to the corresponding flash ring buffers.
 */
static void store_period_to_flash_sync(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period_type,
                                       const rbpower_period_statistics_t* period_stats)
{
    // Only the NULL pointer is checked here; unit_id is validated by the flash writer
    if (period_stats == NULL) {
        ESP_LOGW("RBSTORAGE_SAVE", "Invalid parameters: period_stats == NULL");
        return;
    }

    // The watchdog is fed by the calling task context

    // Map rbpower_stat_period_type_t -> rbpower_period_type_t
    rbpower_period_type_t storage_period;
    switch (period_type) {
        case RBPOWER_STAT_PERIOD_3SEC:
            // 3-second data is not stored in flash
            ESP_LOGD("FLASH_WRITE", "Period 3SEC unit 0x%04X not saved to Flash (RAM only)", unit_id);
            return;
        case RBPOWER_STAT_PERIOD_15MIN:
            // The 15MIN period is not stored in flash (RAM only)
            ESP_LOGV("FLASH_WRITE", "Period 15MIN unit 0x%04X not saved to Flash (RAM only)", unit_id);
            return;
        case RBPOWER_STAT_PERIOD_1HOUR:
            storage_period = RBPOWER_PERIOD_HOUR;
            break;
        case RBPOWER_STAT_PERIOD_DAILY:
            storage_period = RBPOWER_PERIOD_DAY;
            break;
        case RBPOWER_STAT_PERIOD_WEEKLY:
            storage_period = RBPOWER_PERIOD_WEEK;
            break;
        case RBPOWER_STAT_PERIOD_MONTHLY:
            storage_period = RBPOWER_PERIOD_MONTH;
            break;
        case RBPOWER_STAT_PERIOD_YEARLY:
            storage_period = RBPOWER_PERIOD_YEAR;
            break;
        default:
            ESP_LOGW("FLASH_WRITE", "Unsupported period for write: %d", period_type);
            return;
    }

    uint32_t start_time_us = esp_timer_get_time();
    ESP_LOGD("FLASH_WRITE", "=== Start writing period %d unit 0x%04X to Flash buffers ===",
             period_type, unit_id);

    esp_err_t ret;

    // 1. PREPARE THE ELECTRICAL RECORD
    rbpower_electrical_transfer_t electrical_data = {0};
    /* Record timestamp = WALL-CLOCK epoch seconds, not uptime. period_end_us is esp_timer uptime µs;
     * storing it raw would make history `ts` restart from 0 on every reboot even with valid SNTP.
     * Convert via the record's AGE against the current wall clock, so the ts
     * is correct even when the store happens later than the period end. Readers only treat ts as an
     * opaque value (0/0xFFFFFFFF = invalid slot), so the semantic change is safe for the recompute
     * path. No SNTP yet (clock still near epoch 0) → fall back to uptime seconds as before.
     * Accelerated stats builds keep uptime (the virtual clock is uptime-anchored by design). */
#if RBGRID_STATS_TIME_SCALE > 1
    /* VIRTUAL seconds (uptime × scale) — the same timebase the bucket dispatcher runs on. Raw
     * uptime seconds here put record ts and bucket filtering on different clocks, breaking the
     * child-bucket attribution (and made accel DAY records ~29s apart instead of 86400). */
    electrical_data.timestamp =
        (uint32_t)(period_stats->period_end_us * (uint64_t)RBGRID_STATS_TIME_SCALE / 1000000ULL);
#else
    {
        time_t wall_now = time(NULL);
        if (wall_now > 1600000000) {  /* SNTP valid (past 2020-09-13) */
            uint64_t age_us = esp_timer_get_time() - period_stats->period_end_us;
            electrical_data.timestamp = (uint32_t)((uint64_t)wall_now - age_us / 1000000ULL);
        } else {
            electrical_data.timestamp = period_stats->period_end_us / 1000000;
        }
    }
#endif
    if (s_store_ts_override && s_store_ts_override_period == period_type) electrical_data.timestamp = s_store_ts_override;   /* past-hour image / bucket end (period-scoped) */
    electrical_data.voltage_rms_v10 = enc_u16(period_stats->voltage_rms, 10.0f);
    electrical_data.current_rms_ma = enc_u16(period_stats->current_rms, 1000.0f);
    electrical_data.power_active_w10 = enc_i16(period_stats->power_active_avg, 10.0f);
    electrical_data.power_reactive_var10 = enc_i16(period_stats->power_reactive_avg, 10.0f);
    electrical_data.power_apparent_va10 = enc_u16(period_stats->power_apparent_avg, 10.0f);
    electrical_data.power_factor_100 = enc_i8(period_stats->power_factor_avg, 100.0f);
    electrical_data.frequency_hz10 = enc_u8(period_stats->frequency_avg);  /* raw Hz despite the name */
    electrical_data.thd_voltage_100 = enc_u16(period_stats->power_quality.thd_voltage_avg, 100.0f);
    electrical_data.thd_current_100 = enc_u16(period_stats->power_quality.thd_current_avg, 100.0f);
    // Per-period envelope min/max from the accumulator extremes (envelope bands in the web UI).
    electrical_data.voltage_min_v10 = enc_u16(period_stats->extremes.voltage_min, 10.0f);
    electrical_data.voltage_max_v10 = enc_u16(period_stats->extremes.voltage_max, 10.0f);
    electrical_data.current_min_ma  = enc_u16(period_stats->extremes.current_min, 1000.0f);
    electrical_data.current_max_ma  = enc_u16(period_stats->extremes.current_max, 1000.0f);
    electrical_data.power_min_w10   = enc_i16(period_stats->extremes.power_min, 10.0f);
    electrical_data.power_max_w10   = enc_i16(period_stats->extremes.power_max, 10.0f);

    // VALIDATION LOGGING - Verify all ELECTRICAL fields before Flash write
    ESP_LOGD("FLASH_WRITE", "ELECTRICAL unit 0x%04X period %d: ts=%lu V=%.1fV I=%.3fA P=%.1fW PF=%.2f F=%uHz THD_V=%.2f%% THD_I=%.2f%% size=%ub",
             unit_id, storage_period, (unsigned long)electrical_data.timestamp,
             electrical_data.voltage_rms_v10/10.0f, electrical_data.current_rms_ma/1000.0f,
             electrical_data.power_active_w10/10.0f, electrical_data.power_factor_100/100.0f,
             electrical_data.frequency_hz10, electrical_data.thd_voltage_100/100.0f,
             electrical_data.thd_current_100/100.0f, sizeof(rbpower_electrical_transfer_t));

    // 2. PREPARE THE ENERGY RECORD
    rbpower_energy_transfer_t energy_data = {0};
    energy_data.timestamp = electrical_data.timestamp;
    energy_data.energy_consumed_wh = enc_u32(period_stats->energy_period.consumed_wh, 1.0f);
    energy_data.energy_supplied_wh = enc_u32(period_stats->energy_period.supplied_wh, 1.0f);
    {   /* net is signed i32 — saturate+round like the others */
        float net = period_stats->energy_period.net_wh;
        energy_data.energy_net_wh = (net != net) ? 0
            : (net >= 2147483520.0f) ? INT32_MAX
            : (net <= -2147483648.0f) ? INT32_MIN
            : (int32_t)llroundf(net);
    }

    ESP_LOGD("FLASH_WRITE", "ENERGY: consumed=%luWh supplied=%luWh net=%ldWh size=%ub",
             (unsigned long)energy_data.energy_consumed_wh, (unsigned long)energy_data.energy_supplied_wh,
             (long)energy_data.energy_net_wh, sizeof(rbpower_energy_transfer_t));

    // 3. PREPARE THE ECONOMICS RECORD
    rbpower_economics_transfer_t economics_data = {0};
    economics_data.timestamp = electrical_data.timestamp;
    // Round (not truncate) currency units→cents: day/week/month/year totals are SUMS of these stored
    // cents, so per-record truncation would bias the long-period totals systematically low.
    economics_data.cost_consumed_kopecks = (uint32_t)(period_stats->result_base.cost_consumed * 100.0f + 0.5f);
    economics_data.revenue_supplied_kopecks = (uint32_t)(period_stats->result_base.revenue_supplied * 100.0f + 0.5f);
    economics_data.net_cost_kopecks = (int32_t)(economics_data.cost_consumed_kopecks -
                                                 economics_data.revenue_supplied_kopecks);

    ESP_LOGD("FLASH_WRITE", "ECONOMICS: cost=%lukop revenue=%lukop net=%ldkop size=%ub",
             (unsigned long)economics_data.cost_consumed_kopecks, (unsigned long)economics_data.revenue_supplied_kopecks,
             (long)economics_data.net_cost_kopecks, sizeof(rbpower_economics_transfer_t));

    // 3b. v4 METADATA of the record set: dur = coverage from the number of children (samples_count =
    // quarters for HOUR, valid child records for DAY+ after the bucket filter). The metadata travels
    // to the flash layer (also through the pending queue) and is stored in the v4 records.
    rbstats_v4_meta_t set_meta = {0};
    {
        set_meta.res = 1;  /* native quarter-resolution pipeline */
        if (period_stats->v4_dur_s || period_stats->v4_q) {
            /* The finalizer knew the calendar bucket and already computed exact dur/q
             * (DAY+ recompute: exact bucket length incl. DST, PARTIAL/SRC_GAP/RECOMPUTED). */
            set_meta.dur = period_stats->v4_dur_s;
            set_meta.q   = period_stats->v4_q;
        } else {
            /* HOUR (and any path without calendar knowledge): dur from the number of quarters. */
            uint32_t child_dur;
            switch (storage_period) {
                case RBPOWER_PERIOD_HOUR:  child_dur = 900;     break;  /* quarters   */
                case RBPOWER_PERIOD_DAY:   child_dur = 3600;    break;  /* hours      */
                case RBPOWER_PERIOD_WEEK:  child_dur = 86400;   break;  /* days       */
                case RBPOWER_PERIOD_MONTH: child_dur = 86400;   break;  /* days       */
                case RBPOWER_PERIOD_YEAR:  child_dur = 2592000; break;  /* months     */
                default:                   child_dur = 0;       break;
            }
            set_meta.dur = period_stats->samples_count * child_dur;
            if (storage_period == RBPOWER_PERIOD_HOUR &&
                set_meta.dur < rbstats_v4_nominal_dur(RBPOWER_PERIOD_HOUR)) {
                set_meta.q |= RBSTATS_Q_PARTIAL;   /* HOUR is UTC — no DST false positives */
            }
        }
        /* CLOCK_STEP: an SNTP step > 2 s happened within this period. Window = from the record ts
         * (period END) back by the nominal length (dur may be shorter for a partial period — the
         * nominal is used; a false positive at the boundary is safer than a miss). In accelerated
         * builds SNTP is off, so step_ep is always 0. */
        {
            uint32_t step_ep = rbtime_last_clock_step_epoch();
            if (step_ep != 0) {
                uint32_t ts_end = electrical_data.timestamp;
                uint32_t span = rbstats_v4_nominal_dur(storage_period);
                if (ts_end > span && step_ep > ts_end - span && step_ep <= ts_end) {
                    set_meta.q |= RBSTATS_Q_CLOCK_STEP;
                }
            }
        }
        /* et_in/et_out at period close — from the exact lifetime totalizers (contract §2). */
        rbpower_lifetime_get_totalizers(unit_id, &set_meta.et_in_mwh, &set_meta.et_out_mwh);
        /* Tariff plan stamp for the economics record (contract: 0 = unknown). Plans carry no numeric
         * id in this release (single configured plan) → plan_id 1 = "the configured plan", plan_ver =
         * its plan_version. Without this the record carries 0/0. */
        {
            const rbpower_tariff_plan_t *tp = rbpower_tariff_get_current_plan();
            if (tp && tp->is_active) {
                set_meta.plan_id  = 1;
                set_meta.plan_ver = (uint16_t)(tp->plan_version > 0xFFFF ? 0xFFFF : tp->plan_version);
            }
        }
        /* Precise period values for the v4 record body (the v3 structs truncate to whole Wh).
         * The DAY+ recompute carries the int64 sums of the children in stats->exact — use them
         * WITHOUT a float stage; the live path (HOUR) uses the floats, which are exact at that size. */
        if (period_stats->exact.valid) {
            set_meta.ec_mwh  = period_stats->exact.ec_mwh;
            set_meta.es_mwh  = period_stats->exact.es_mwh;
            set_meta.c_cents = period_stats->exact.c_cents;
            set_meta.r_cents = period_stats->exact.r_cents;
        } else {
            set_meta.ec_mwh  = (int64_t)llround((double)period_stats->energy_period.consumed_wh * 1000.0);
            set_meta.es_mwh  = (int64_t)llround((double)period_stats->energy_period.supplied_wh * 1000.0);
            set_meta.c_cents = (int64_t)llround((double)period_stats->result_base.cost_consumed * 100.0);
            set_meta.r_cents = (int64_t)llround((double)period_stats->result_base.revenue_supplied * 100.0);
        }
        set_meta.precise = true;
    }

    // 4. WRITE TO FLASH (electrical/energy/economics for ALL persisted periods).
    // QUALITY and DIRECTION records were removed from the active pipeline: their source was the
    // native ADC + ZC/THD analysis, which rbAmp modules (average power) do not provide, so in the
    // per-unit path they were always zero. Their sectors were freed.
    ret = rbpower_flash_buffer_write_record_set(
        unit_id,
        storage_period,
        &electrical_data,
        &energy_data,
        &economics_data,
        &set_meta
    );

    uint32_t total_duration_us = esp_timer_get_time() - start_time_us;

    /* the read-back check read the v3 32-B struct from the v4 41-B ring: it failed on every HOUR+ write since
     * the v4 format change and only cost a flash read — removed */

    if (ret == ESP_OK) {
        ESP_LOGI("FLASH_WRITE", "Unit 0x%04X period %d written OK (%luus)",
                 unit_id, period_type, (unsigned long)total_duration_us);
    } else {
        ESP_LOGE("FLASH_WRITE", "Unit 0x%04X period %d write ERROR: %s (%luus)",
                 unit_id, period_type, esp_err_to_name(ret), (unsigned long)total_duration_us);
    }

    // Check for abnormally slow operations
    if (total_duration_us > 500000) { // > 500ms
        ESP_LOGW("FLASH_WRITE", "Unit 0x%04X: Slow write detected (%lums)",
                 unit_id, (unsigned long)(total_duration_us / 1000));
    }
}

/* Persist a FINISHED period from an EXTERNAL source (e.g. an ESP-NOW node that latched the period
 * on-device) DIRECTLY to flash history — bypassing the native 3SEC→15MIN→1HOUR accumulation (a
 * runtime-injected ESP-NOW unit isn't wired into the source-mapped accumulator path). The node already
 * computed avg/max/Wh over its own integrated window; we write the finished record straight to flash. */
void rbpower_home_persist_external_period(rbpower_unit_id_t unit_id, rbpower_stat_period_type_t period,
                                          float avg_power_w, float max_power_w, float energy_wh,
                                          uint32_t period_ms, float voltage_v, float current_a,
                                          float current_min_a, float current_max_a)
{
    rbpower_period_statistics_t ps;
    memset(&ps, 0, sizeof(ps));
    uint64_t now = esp_timer_get_time();
    ps.period_end_us      = now;
    ps.period_start_us    = (now >= (uint64_t)period_ms * 1000ULL) ? now - (uint64_t)period_ms * 1000ULL : 0;
    ps.period_duration_ms = period_ms;
    ps.voltage_rms        = voltage_v;
    ps.current_rms        = current_a;
    ps.power_active_avg   = avg_power_w;
    ps.power_apparent_avg = voltage_v * current_a;
    ps.power_factor_avg   = (voltage_v * current_a > 0.01f) ? (avg_power_w / (voltage_v * current_a)) : 1.0f;
    ps.extremes.voltage_min = voltage_v;     ps.extremes.voltage_max = voltage_v;
    ps.extremes.current_min = current_min_a; ps.extremes.current_max = current_max_a;
    ps.extremes.power_min   = avg_power_w;   ps.extremes.power_max   = max_power_w;
    if (energy_wh >= 0.0f) ps.energy_period.consumed_wh = energy_wh;
    else                   ps.energy_period.supplied_wh = -energy_wh;
    ps.energy_period.net_wh = energy_wh;
    store_period_to_flash_sync(unit_id, period, &ps);
}

/*
 * @brief Cleanup async flash write system on shutdown
 */
void rbhome_statistics_cleanup_async_flash_system(void)
{
    // Check if storage is enabled
    if (!g_storage_enabled) {
        return;  // Do nothing if storage is disabled
    }
    
    ESP_LOGI("RBSTORAGE_TASK", "Shutting down the asynchronous flash write system");
    
    // Stop the task
    g_flash_write_task_running = false;
    
    // Wait for the task to finish
    if (g_flash_write_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Give it time to exit
        g_flash_write_task_handle = NULL;
    }
    
    // Delete the queue
    if (g_flash_write_queue != NULL) {
        vQueueDelete(g_flash_write_queue);
        g_flash_write_queue = NULL;
    }
    
    ESP_LOGI("RBSTORAGE_TASK", "Asynchronous flash write system shut down");
}
