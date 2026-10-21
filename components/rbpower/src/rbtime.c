/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbtime.c
 * @brief Time and synchronization system for rbpower (public API declared in rbpower.h).
 *
 * Supports SNTP, manual time setting, and fallback/holdover modes. The ESP-NOW hub time source is
 * declared but not implemented (selecting it logs a warning). A background task (rbtime_sync) performs
 * the periodic resync; the module state is protected by an internal mutex.
 *
 * @version 1.0.0
 * @date 2025-01-10
 * @author RobotBroker
 */

#include "rbpower.h"
#include "rbpower_enums.h"
#include "rbpower_constants.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_system.h"

/** @brief Retry interval before the FIRST successful SNTP sync, s.
 *
 * Until the first sync, the sync task retries this often instead of the full periodic
 * resync_interval_sec. lwIP's own SNTP retry is ~1h, so a lost/slow first request otherwise leaves the clock
 * on the Jan-2024 fallback for an hour — which silently disables the calendar/zone-latch
 * features that gate on RBTIME_IS_SYNCED. After the first sync, the configured interval governs. */
#define RBTIME_FIRST_SYNC_RETRY_SEC 30

/* ================================================================================================
 * PRIVATE TYPES AND STRUCTURES
 * ================================================================================================ */

/**
 * @brief Internal time system context
 */
typedef struct {
    // === STATE ===
    bool initialized;                   /**< Initialization flag */
    rbtime_status_t status;             /**< Current status */
    rbtime_source_t active_source;      /**< Active time source */

    // === CONFIGURATION ===
    rbtime_config_t config;             /**< Configuration copy */

    // === STATISTICS ===
    rbtime_statistics_t stats;          /**< Operation statistics */

    // === SYNCHRONIZATION ===
    SemaphoreHandle_t mutex;            /**< Mutex for thread safety */
    TimerHandle_t resync_timer;         /**< Periodic resync timer */
    TaskHandle_t sync_task_handle;      /**< Sync task handle */

    // === CALLBACKS ===
    rbtime_event_callback_t event_callback;  /**< Event callback */
    void *callback_user_data;                /**< Callback user data */

    // === TIMESTAMPS ===
    uint64_t initialization_time_us;    /**< Initialization time */
    uint64_t last_manual_time_us;       /**< Last manual time */
    // === NETWORK PARAMETERS ===
    bool network_available;             /**< Network availability */
    bool sntp_initialized;              /**< SNTP initialized */
    bool waiting_for_network;           /**< Waiting for network for SNTP */

} rbtime_context_t;

/* ================================================================================================
 * PRIVATE VARIABLES
 * ================================================================================================ */

/** Global time system context */
static rbtime_context_t g_time_ctx = {0};

/* ================================================================================================
 * PRIVATE FUNCTIONS - UTILITIES
 * ================================================================================================ */

/**
 * @brief Acquire time mutex
 */
static bool rbtime_lock(uint32_t timeout_ms)
{
    if (g_time_ctx.mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(g_time_ctx.mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/**
 * @brief Release time mutex
 */
static void rbtime_unlock(void)
{
    if (g_time_ctx.mutex != NULL) {
        xSemaphoreGive(g_time_ctx.mutex);
    }
}

/**
 * @brief Update time status with notification
 */
static void rbtime_set_status(rbtime_status_t new_status)
{
    rbtime_status_t old_status = g_time_ctx.status;

    if (old_status != new_status) {
        g_time_ctx.status = new_status;
        // IMPORTANT: Update status in statistics as well
        g_time_ctx.stats.current_status = new_status;

        ESP_LOGD("RBTIME", "Time status changed: %d -> %d", old_status, new_status);

        // Call callback if registered
        if (g_time_ctx.event_callback != NULL) {
            g_time_ctx.event_callback(old_status, new_status, g_time_ctx.callback_user_data);
        }
    }
}

/* Probe-validate a POSIX TZ string ("<UTC+9>" without an offset silently fell back to UTC in newlib).
 * Applies tz temporarily and checks that a zone not claiming UTC actually moves local time
 * against UTC at a fixed epoch. ALWAYS restores the previous TZ — callers apply it themselves. */
static bool rbtime_tz_is_valid(const char *tz)
{
    if (tz == NULL || tz[0] == '\0') {
        return false;
    }
    bool claims_utc = (strcmp(tz, "UTC") == 0 || strcmp(tz, "UTC0") == 0 ||
                       strcmp(tz, "UTC+0") == 0 || strcmp(tz, "UTC-0") == 0 ||
                       strcmp(tz, "GMT") == 0 || strcmp(tz, "GMT0") == 0);
    char prev[64] = {0};
    const char *old_tz = getenv("TZ");
    if (old_tz) strncpy(prev, old_tz, sizeof(prev) - 1);
    setenv("TZ", tz, 1);
    tzset();
    time_t probe = 1760140800;   /* fixed epoch: 2025-10-11 00:00 UTC */
    struct tm lt, gt;
    localtime_r(&probe, &lt);
    gmtime_r(&probe, &gt);
    bool moved = (lt.tm_hour != gt.tm_hour) || (lt.tm_min != gt.tm_min) ||
                 (lt.tm_mday != gt.tm_mday);
    if (prev[0]) setenv("TZ", prev, 1); else unsetenv("TZ");
    tzset();
    return claims_utc || moved;
}

/* Public wrapper (declared in rbpower.h): the time-sync POST validates the TZ BEFORE
 * storing it — "<UTC+9>" (name, no offset) used to be persisted and silently ran the device in UTC. */
bool rbtime_timezone_is_valid(const char *tz) { return rbtime_tz_is_valid(tz); }

/* Validate time configuration */
static rbpower_err_t rbtime_validate_config(const rbtime_config_t *config)
{
    if (config == NULL) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    // Check SNTP server
    if (config->primary_source == RBTIME_SOURCE_SNTP) {
        if (strlen(config->sntp_server) == 0) {
            ESP_LOGE("RBTIME", "SNTP server not specified");
            return RBPOWER_ERR_INVALID_ARG;
        }

        if (config->sntp_timeout_ms < 1000 || config->sntp_timeout_ms > 60000) {
            ESP_LOGE("RBTIME", "Invalid SNTP timeout: %lu ms", config->sntp_timeout_ms);
            return RBPOWER_ERR_INVALID_ARG;
        }
    }

    // Check timezone
    if (strlen(config->timezone) == 0) {
        ESP_LOGW("RBTIME", "Timezone not specified, UTC will be used");
    }

    // Check resync interval
    if (config->resync_interval_sec < 60 || config->resync_interval_sec > 86400) {
        ESP_LOGE("RBTIME", "Invalid resync interval: %lu sec", config->resync_interval_sec);
        return RBPOWER_ERR_INVALID_ARG;
    }

    return RBPOWER_OK;
}

/**
 * @brief Time quality of the wall clock: 0 unsynced · 1 synced · 2 holdover.
 *
 * The holdover threshold is DERIVED, not picked: tq=2 once the worst-case drift error exceeds the
 * ±5 s tariff-boundary budget — at the ESP32's ~5 s/day crystal drift that is 24 h without a resync.
 * Not declared in a public header (callers use an extern declaration). Reads the sync statistics
 * without taking the module mutex.
 *
 * @param[out] sync_age_s_out  Seconds since the last successful sync (0 when never synced; may be NULL).
 * @return 0 = never synced, 1 = synced within 24 h, 2 = holdover (last sync older than 24 h).
 */
uint8_t rbtime_quality(uint32_t *sync_age_s_out)
{
    uint64_t last_us = g_time_ctx.stats.last_sync_timestamp_us;
    if (g_time_ctx.stats.successful_syncs == 0 || last_us == 0) {
        if (sync_age_s_out) *sync_age_s_out = 0;
        return 0;
    }
    uint32_t age_s = (uint32_t)((esp_timer_get_time() - last_us) / 1000000ULL);
    if (sync_age_s_out) *sync_age_s_out = age_s;
    return (age_s <= 86400u) ? 1 : 2;
}

/* ================================================================================================
 * PRIVATE FUNCTIONS - SNTP
 * ================================================================================================ */

/* CLOCK_STEP detection. offset = wall-clock − uptime is constant under smooth drift,
 * so a jump in it between syncs is a real step. The FIRST sync (uptime era → wall clock) is NOT a
 * step — that transition is covered by TIME_UNSYNC/re-stamp of the pending records. */
static int64_t  s_sync_offset_s = 0;
static bool     s_sync_offset_valid = false;
static uint32_t s_last_clock_step_epoch = 0;

uint32_t rbtime_last_clock_step_epoch(void) { return s_last_clock_step_epoch; }

/* SNTP notification callback (runs in the lwIP/SNTP context on every successful sync) */
static void rbtime_sntp_notification_cb(struct timeval *tv)
{
    if (!rbtime_lock(100)) {
        return;
    }

    {
        int64_t up_s = (int64_t)(esp_timer_get_time() / 1000000ULL);
        int64_t off  = (int64_t)time(NULL) - up_s;
        if (s_sync_offset_valid) {
            int64_t d = off - s_sync_offset_s;
            if (d > 2 || d < -2) {
                s_last_clock_step_epoch = (uint32_t)time(NULL);
                ESP_LOGW("RBTIME", "CLOCK STEP at sync: %+lld s — records covering this moment get q.CLOCK_STEP",
                         (long long)d);
            }
        }
        s_sync_offset_s = off;
        s_sync_offset_valid = true;
    }

    ESP_LOGI("RBTIME", "SNTP synchronization successful\n"
             "  IMPORTANT: Starting statistics timer resynchronization\n"
             "  Tariff system will now work with accurate time");

    // Update statistics
    g_time_ctx.stats.successful_syncs++;
    g_time_ctx.stats.last_sync_timestamp_us = esp_timer_get_time();
    g_time_ctx.stats.active_source = RBTIME_SOURCE_SNTP;
    g_time_ctx.stats.sntp_responses_received++;

    // Update status
    rbtime_set_status(RBTIME_STATUS_SYNCED);
    g_time_ctx.active_source = RBTIME_SOURCE_SNTP;

    rbtime_unlock();

    // Re-stamp the pending finalize-result queue (records accumulated before
    // this sync carry uptime timestamps; the delta is now known exactly) and flush it to flash.
    {
        extern void rbstats_pending_on_time_sync(void);
        rbstats_pending_on_time_sync();
    }

    // === CRITICAL INTEGRATION: Statistics timer resynchronization ===
    // External call to timer sync function after SNTP time correction
    extern rbpower_err_t rbpower_statistics_timers_sync_with_system_time(void);
    rbpower_err_t sync_result = rbpower_statistics_timers_sync_with_system_time();
    if (sync_result == RBPOWER_OK) {
        ESP_LOGI("RBTIME", "Statistics timers resynchronized after SNTP correction\n"
                 "  3-second periods now synchronized with real time");
    } else {
        ESP_LOGW("RBTIME", "Statistics timer resynchronization error: %d", sync_result);
    }
}

/**
 * @brief Check network availability
 */
static bool rbtime_is_network_available(void)
{
    // Check network stack initialization
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGD("RBTIME", "WiFi netif not found");
        return false;
    }

    // Check WiFi connection
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGD("RBTIME", "Failed to get IP info");
        return false;
    }

    if (ip_info.ip.addr == 0) {
        ESP_LOGD("RBTIME", "IP address not assigned");
        return false;
    }

    ESP_LOGD("RBTIME", "Network available, IP: " IPSTR, IP2STR(&ip_info.ip));
    return true;
}

/**
 * @brief Initialize SNTP client
 */
static rbpower_err_t rbtime_init_sntp(const rbtime_config_t *config)
{
#if RBGRID_STATS_TIME_SCALE > 1
    /* Accelerated stats test (RBGRID_STATS_TIME_SCALE>1): do NOT start SNTP. The statistics boundary
     * clock runs on uptime×scale (NTP-immune), so real wall time isn't needed, and the periodic NTP
     * resync (every resync_interval_sec) would only perturb the deterministic accelerated test. */
    (void)config;
    ESP_LOGW("RBTIME", "⏩ SNTP DISABLED (accelerated stats test mode, scale=%d) — clock is uptime-based",
             RBGRID_STATS_TIME_SCALE);
    return RBPOWER_OK;
#endif
    ESP_LOGD("RBTIME", "Initializing SNTP client...");

    // Check if network is available
    if (!rbtime_is_network_available()) {
        ESP_LOGE("RBTIME", "Network unavailable for SNTP");
        return RBPOWER_ERR_WIFI_NO_CONNECTION;
    }

    // Stop previous SNTP (if any)
    if (g_time_ctx.sntp_initialized) {
        esp_sntp_stop();
        g_time_ctx.sntp_initialized = false;
    }

    // Configure SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, config->sntp_server);

    if (strlen(config->sntp_server_backup) > 0) {
        esp_sntp_setservername(1, config->sntp_server_backup);
        ESP_LOGD("RBTIME", "Backup SNTP server configured: %s", config->sntp_server_backup);
    }

    // Set callback
    sntp_set_time_sync_notification_cb(rbtime_sntp_notification_cb);

    // TZ is applied (validated) in rbtime_init, not here: this path never ran in
    // accelerated builds (early return above) and applied the string unvalidated.

    // Start SNTP
    esp_sntp_init();
    g_time_ctx.sntp_initialized = true;

    ESP_LOGI("RBTIME", "SNTP client started with server: %s\n"
             "  Waiting for time synchronization (fallback mode active until synced)...",
             config->sntp_server);

    g_time_ctx.stats.sntp_requests_sent++;

    return RBPOWER_OK;
}

/**
 * @brief Force SNTP synchronization
 */
static rbpower_err_t rbtime_force_sntp_sync(uint32_t timeout_ms)
{
    ESP_LOGD("RBTIME", "Forcing SNTP synchronization...");

    // Check WiFi status
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGE("RBTIME", "WiFi not connected - SNTP synchronization impossible");
        return RBPOWER_ERR_WIFI_NO_CONNECTION;
    }

    rbtime_set_status(RBTIME_STATUS_SYNCING);

    // Restart SNTP
    esp_sntp_restart();
    g_time_ctx.stats.sntp_requests_sent++;

    // Wait for synchronization with timeout
    uint32_t elapsed_ms = 0;
    const uint32_t check_interval_ms = 100;

    while (elapsed_ms < timeout_ms) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGD("RBTIME", "SNTP synchronization completed in %lu ms", elapsed_ms);
            // Update status and statistics on successful sync
            rbtime_set_status(RBTIME_STATUS_SYNCED);
            g_time_ctx.stats.successful_syncs++;
            g_time_ctx.stats.last_sync_timestamp_us = esp_timer_get_time();
            g_time_ctx.stats.active_source = RBTIME_SOURCE_SNTP;
            return RBPOWER_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        elapsed_ms += check_interval_ms;
    }

    ESP_LOGW("RBTIME", "SNTP synchronization timeout: %lu ms", timeout_ms);
    g_time_ctx.stats.failed_syncs++;
    rbtime_set_status(RBTIME_STATUS_TIMEOUT);

    return RBPOWER_ERR_TIMEOUT;
}

/* ================================================================================================
 * PRIVATE FUNCTIONS - FALLBACK
 * ================================================================================================ */

/* "Not before" for the wall clock: this file's build day minus 2 days (TZ slack; same rule as the stats
 * build_epoch_floor). A clock earlier than that is
 * not real time (1970 after a cold boot, the Jan-2024 manual fallback) — never "valid" for statistics. */
uint32_t rbtime_not_before(void)
{
    static uint32_t s_nb = 0;
    if (s_nb) return s_nb;
    static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = {0}; int d = 0, y = 0;
    sscanf(__DATE__, "%3s %d %d", m, &d, &y);
    const char *p = strstr(MON, m);
    struct tm t = { .tm_year = y - 1900, .tm_mon = p ? (int)((p - MON) / 3) : 0, .tm_mday = d };
    /* timegm-equivalent: days since epoch from the civil date (UTC) */
    int yy = t.tm_year + 1900, mm = t.tm_mon + 1;
    yy -= mm <= 2;
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153u * (unsigned)(mm + (mm > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    s_nb = (uint32_t)(days * 86400L) - 2u * 86400u;
    return s_nb;
}

/**
 * @brief Activate fallback mode
 */
static rbpower_err_t rbtime_activate_fallback(void)
{
    ESP_LOGW("RBTIME", "Activating time fallback mode...");

    /* HOLDOVER: a failed (re)sync must never throw away a clock that is already real. The system time
     * survives esp_restart and keeps running between syncs; the Jan-2024 manual fallback used to
     * OVERWRITE it — e.g. an OTA reboot while the LAN NTP server was down → clock 2024-01-01,
     * reboot-gap energy dropped, the open hour's image dropped, a garbage 2024 hour record written. */
    if ((uint32_t)time(NULL) >= rbtime_not_before()) {
        if (g_time_ctx.status != RBTIME_STATUS_SYNCED) rbtime_set_status(RBTIME_STATUS_FALLBACK);
        g_time_ctx.stats.fallback_activations++;
        ESP_LOGW("RBTIME", "sync failed — holdover: keeping the running clock (%lu)", (unsigned long)time(NULL));
        return RBPOWER_OK;
    }

    rbtime_source_t fallback_source = g_time_ctx.config.fallback_source;
    
    switch (fallback_source) {
        case RBTIME_SOURCE_MANUAL:
            /* the stock manual time is Jan 1 2024 — a fake date; setting it made a cold-booted hub without
             * NTP stamp records in 2024. Only a configured time that can be real is applied; otherwise the
             * clock stays "not yet set" (as before the first sync) and statistics wait for real time. */
            if (g_time_ctx.config.manual_time_us > 0 &&
                g_time_ctx.config.manual_time_us / 1000000ULL >= (uint64_t)rbtime_not_before()) {
                struct timeval tv = {
                    .tv_sec = g_time_ctx.config.manual_time_us / 1000000,
                    .tv_usec = g_time_ctx.config.manual_time_us % 1000000
                };
                settimeofday(&tv, NULL);

                ESP_LOGI("RBTIME", "Manual fallback time set: %llu (Jan 1 2024)\n"
                         "  Tariff and statistics systems working with fallback time",
                         g_time_ctx.config.manual_time_us);
                g_time_ctx.active_source = RBTIME_SOURCE_MANUAL;
                rbtime_set_status(RBTIME_STATUS_FALLBACK);
                return RBPOWER_OK;
            }
            break;
            
        case RBTIME_SOURCE_RTC:
            if (g_time_ctx.config.enable_rtc_fallback) {
                // TODO: RTC fallback implementation
                ESP_LOGW("RBTIME", "RTC fallback not implemented");
            }
            break;
            
        default:
            break;
    }
    
    // Last fallback - CPU time
    if (g_time_ctx.config.enable_cpu_time_fallback) {
        ESP_LOGD("RBTIME", "Using CPU time as fallback");
        g_time_ctx.active_source = RBTIME_SOURCE_NONE;
        rbtime_set_status(RBTIME_STATUS_FALLBACK);
        g_time_ctx.stats.fallback_activations++;
        return RBPOWER_OK;
    }

    ESP_LOGE("RBTIME", "All fallback sources unavailable");
    rbtime_set_status(RBTIME_STATUS_ERROR);
    return RBPOWER_ERR_TIME_NO_FALLBACK;
}

/* ================================================================================================
 * PRIVATE FUNCTIONS - SYNC TASK
 * ================================================================================================ */

/**
 * @brief Time synchronization task
 */
static void rbtime_sync_task(void *pvParameters)
{
    ESP_LOGD("RBTIME", "Time synchronization task started");
    
    while (g_time_ctx.initialized) {
        if (!rbtime_lock(1000)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        rbtime_config_t *config = &g_time_ctx.config;
        rbpower_err_t sync_result = RBPOWER_ERR_TIMEOUT;

        // Attempt synchronization with primary source
        switch (config->primary_source) {
            case RBTIME_SOURCE_SNTP:
                /* SNTP recovery: when boot-time rbtime_init_sntp() bailed
                 * because WiFi/DHCP wasn't up yet, SNTP stayed UNCONFIGURED (no servers set) while
                 * this loop kept esp_sntp_restart()-ing it forever — 40 requests / 0 responses /
                 * permanent FALLBACK until a lucky reboot. Heal here: (re)initialize first; init
                 * itself checks network availability, so this retries until the network is up.
                 * (The manual-sync API at rbtime_sync_time_from_source already had this heal —
                 * the background task did not.) */
                if (!g_time_ctx.sntp_initialized) {
                    rbpower_err_t ierr = rbtime_init_sntp(config);
                    if (ierr != RBPOWER_OK) {
                        ESP_LOGW("RBTIME", "SNTP not initialized yet (network not ready?) — will retry");
                        sync_result = ierr;
                        break;
                    }
                    ESP_LOGI("RBTIME", "SNTP initialized by sync task (network became available)");
                }
                sync_result = rbtime_force_sntp_sync(config->sntp_timeout_ms);
                break;
                
            case RBTIME_SOURCE_ESP_NOW_HUB:
                // TODO: ESP-NOW synchronization implementation
                ESP_LOGW("RBTIME", "ESP-NOW synchronization not implemented");
                sync_result = RBPOWER_ERR_NOT_IMPLEMENTED;
                break;

            default:
                ESP_LOGW("RBTIME", "Unsupported time source: %d", config->primary_source);
                sync_result = RBPOWER_ERR_INVALID_ARG;
                break;
        }
        
        // On failure - attempt fallback
        if (sync_result != RBPOWER_OK) {
            ESP_LOGW("RBTIME", "Primary synchronization failed, attempting fallback...");
            rbtime_activate_fallback();
        }

        uint32_t resync_sec = config->resync_interval_sec;
        rbtime_unlock();

        /* Robust first-sync: until we've synced at least once, retry fast (bounded first-sync latency) rather
         * than waiting the full periodic interval after a lost/slow first request. After the first success,
         * use the configured periodic interval. */
        uint32_t wait_sec = (g_time_ctx.stats.successful_syncs > 0) ? resync_sec : RBTIME_FIRST_SYNC_RETRY_SEC;
        vTaskDelay(pdMS_TO_TICKS(wait_sec * 1000));
    }

    ESP_LOGD("RBTIME", "Time synchronization task completed");
    vTaskDelete(NULL);
}

/* Periodic synchronization timer callback (unused; the sync task paces itself) */
// static void rbtime_resync_timer_callback(TimerHandle_t xTimer)
// {
//     if (g_time_ctx.sync_task_handle != NULL) {
//         // Notify sync task
//         xTaskNotifyGive(g_time_ctx.sync_task_handle);
//     }
// }

/* ================================================================================================
 * PUBLIC API FUNCTIONS
 * ================================================================================================ */

rbpower_err_t rbtime_init(const rbtime_config_t *config)
{
    ESP_LOGI("RBTIME", "=== Initializing rbpower time system ===");

    // Validate configuration
    rbpower_err_t err = rbtime_validate_config(config);
    if (err != RBPOWER_OK) {
        return err;
    }
    
    // Check for reinitialization
    if (g_time_ctx.initialized) {
        ESP_LOGW("RBTIME", "Time system already initialized");
        return RBPOWER_ERR_ALREADY_INIT;
    }

    /* clear first, then create + take the mutex (the old order took a mutex, memset it away and recreated an
     * untaken one — the final unlock gave a mutex nobody held) */
    memset(&g_time_ctx, 0, sizeof(rbtime_context_t));
    g_time_ctx.mutex = xSemaphoreCreateMutex();
    if (g_time_ctx.mutex == NULL) {
        ESP_LOGE("RBTIME", "Failed to create time mutex");
        return RBPOWER_ERR_NO_MEMORY;
    }
    if (!rbtime_lock(1000)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    // Copy configuration
    memcpy(&g_time_ctx.config, config, sizeof(rbtime_config_t));

    // Apply the stored timezone HERE, once, for every localtime_r user (tariff zones,
    // calendar stats buckets) — tariff init no longer hard-codes CET
    // and the SNTP path no longer applies it unvalidated.
    // An invalid persisted TZ falls back to UTC0 with a loud warning on every boot
    // until the config is fixed via the time-sync POST (which rejects it with 400).
    if (rbtime_tz_is_valid(config->timezone)) {
        setenv("TZ", config->timezone, 1);
        tzset();
        ESP_LOGI("RBTIME", "Timezone applied from config: %s", config->timezone);
    } else if (strlen(config->timezone) == 0) {
        setenv("TZ", "UTC0", 1);
        tzset();
        ESP_LOGI("RBTIME", "Timezone not configured — running on UTC0");
    } else {
        setenv("TZ", "UTC0", 1);
        tzset();
        ESP_LOGW("RBTIME", "⚠ Config timezone '%s' is NOT a valid POSIX TZ — running on UTC0 "
                           "(fix via time-sync POST, e.g. 'CET-1CEST,M3.5.0,M10.5.0/3')",
                 config->timezone);
    }

    // Initialize statistics
    g_time_ctx.stats.current_status = RBTIME_STATUS_INITIALIZING;
    g_time_ctx.stats.system_start_time_us = esp_timer_get_time();
    g_time_ctx.initialization_time_us = esp_timer_get_time();
    /* BEFORE the source init and the sync task: the task loops while(initialized) — created first, it could see
     * false, exit and leave the hub without periodic resync; manual time also returned NOT_INIT) */
    g_time_ctx.initialized = true;

    rbtime_set_status(RBTIME_STATUS_INITIALIZING);

    // Initialize time source
    if (config->enable_time_sync) {
        switch (config->primary_source) {
            case RBTIME_SOURCE_SNTP:
                err = rbtime_init_sntp(config);
                break;
                
            case RBTIME_SOURCE_ESP_NOW_HUB:
                ESP_LOGW("RBTIME", "ESP-NOW time source not implemented");
                err = RBPOWER_ERR_NOT_IMPLEMENTED;
                break;

            case RBTIME_SOURCE_MANUAL:
                if (config->manual_time_us > 0) {
                    err = rbtime_set_manual_time(config->manual_time_us / 1000000);
                } else {
                    ESP_LOGW("RBTIME", "Manual time not set");
                    err = RBPOWER_ERR_INVALID_ARG;
                }
                break;

            default:
                ESP_LOGE("RBTIME", "Unsupported time source: %d", config->primary_source);
                err = RBPOWER_ERR_INVALID_ARG;
                break;
        }
        
        // On primary source initialization failure - attempt fallback
        if (err != RBPOWER_OK) {
            ESP_LOGW("RBTIME", "Failed to initialize primary time source, attempting fallback...");
            err = rbtime_activate_fallback();
        }

 /* no forced sync here: it always returned NOT_INIT (never ran), and running it now would busy-wait up to
         * sntp_timeout_ms in boot before Wi-Fi is up. SNTP syncs on its own once the network is there. */
        
        // Create periodic synchronization task
        if (config->resync_interval_sec > 0) {
            xTaskCreate(rbtime_sync_task, "rbtime_sync", 4096, NULL,
                       tskIDLE_PRIORITY + 1, &g_time_ctx.sync_task_handle);

            ESP_LOGD("RBTIME", "Periodic synchronization task created (interval: %lu sec)",
                     config->resync_interval_sec);
        }
    } else {
        ESP_LOGD("RBTIME", "Time synchronization disabled in configuration");
        rbtime_set_status(RBTIME_STATUS_SYNCED);
        err = RBPOWER_OK;
    }

    rbtime_unlock();

    if (err == RBPOWER_OK) {
        ESP_LOGI("RBTIME", "Time system initialized successfully");
        ESP_LOGD("RBTIME", "- Source: %d", config->primary_source);
        ESP_LOGD("RBTIME", "- Timezone: %s", config->timezone);
        ESP_LOGD("RBTIME", "- Status: %d", g_time_ctx.status);
    } else {
        ESP_LOGE("RBTIME", "Time system initialization error: %d", err);
    }
    
    return err;
}

rbpower_err_t rbtime_deinit(void)
{
    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    ESP_LOGD("RBTIME", "Deinitializing time system...");

    if (!rbtime_lock(1000)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    // Stop sync task
    if (g_time_ctx.sync_task_handle != NULL) {
        vTaskDelete(g_time_ctx.sync_task_handle);
        g_time_ctx.sync_task_handle = NULL;
    }

    // Stop timer
    if (g_time_ctx.resync_timer != NULL) {
        xTimerDelete(g_time_ctx.resync_timer, pdMS_TO_TICKS(1000));
        g_time_ctx.resync_timer = NULL;
    }

    // Stop SNTP
    esp_sntp_stop();

    // Clear context
    g_time_ctx.initialized = false;
    rbtime_set_status(RBTIME_STATUS_UNINITIALIZED);

    rbtime_unlock();

    // Delete mutex
    if (g_time_ctx.mutex != NULL) {
        vSemaphoreDelete(g_time_ctx.mutex);
        g_time_ctx.mutex = NULL;
    }

    ESP_LOGD("RBTIME", "Time system deinitialized");
    return RBPOWER_OK;
}

rbtime_status_t rbtime_get_status(void)
{
    return g_time_ctx.status;
}

rbpower_err_t rbtime_get_statistics(rbtime_statistics_t *stats)
{
    if (stats == NULL) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbtime_lock(100)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    memcpy(stats, &g_time_ctx.stats, sizeof(rbtime_statistics_t));

    rbtime_unlock();
    return RBPOWER_OK;
}

rbpower_err_t rbtime_get_config(rbtime_config_t *config)
{
    if (config == NULL) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbtime_lock(100)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    memcpy(config, &g_time_ctx.config, sizeof(rbtime_config_t));

    rbtime_unlock();
    return RBPOWER_OK;
}

/* Non-blocking: start SNTP (first network) or restart its request, and return at once. For event handlers —
 * rbtime_force_sync busy-waits up to sntp_timeout_ms, and the Wi-Fi GOT_IP handler runs in the system event
 * task (2.3 KB stack, every Wi-Fi/netif event queued behind it). */
rbpower_err_t rbtime_kick_sync(void)
{
    if (!g_time_ctx.initialized) return RBPOWER_ERR_NOT_INIT;
    if (g_time_ctx.config.primary_source != RBTIME_SOURCE_SNTP || !g_time_ctx.config.enable_time_sync) return RBPOWER_OK;
    if (!rbtime_lock(100)) return RBPOWER_ERR_TIMEOUT;
    rbpower_err_t err = RBPOWER_OK;
    g_time_ctx.waiting_for_network = false;
    if (!g_time_ctx.sntp_initialized) err = rbtime_init_sntp(&g_time_ctx.config);
    else esp_sntp_restart();
    rbtime_unlock();
    return err;
}

rbpower_err_t rbtime_force_sync(rbtime_source_t source, uint32_t timeout_ms)
{
    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    ESP_LOGD("RBTIME", "=== Force time synchronization ===");
    ESP_LOGD("RBTIME", "Source: %d, timeout: %lu ms", source, timeout_ms);

    // Use saved source if not specified
    if (source == RBTIME_SOURCE_NONE) {
        source = g_time_ctx.config.primary_source;
    }

    // If timeout not specified, use from configuration
    if (timeout_ms == 0) {
        timeout_ms = g_time_ctx.config.sntp_timeout_ms;
    }

    if (!rbtime_lock(1000)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    rbpower_err_t err = RBPOWER_ERR_NOT_IMPLEMENTED;

    switch (source) {
        case RBTIME_SOURCE_SNTP:
            // Check network availability
            if (!rbtime_is_network_available()) {
                ESP_LOGW("RBTIME", "Network unavailable for SNTP synchronization");
                g_time_ctx.waiting_for_network = true;
                rbtime_unlock();
                return RBPOWER_ERR_WIFI_NO_CONNECTION;
            }

            g_time_ctx.waiting_for_network = false;

            // If SNTP was not initialized or network was unavailable before
            if (!g_time_ctx.sntp_initialized) {
                ESP_LOGI("RBTIME", "SNTP not initialized, initializing now...");
                err = rbtime_init_sntp(&g_time_ctx.config);
                if (err != RBPOWER_OK) {
                    ESP_LOGE("RBTIME", "SNTP initialization error: %d", err);
                    rbtime_unlock();
                    return err;
                }
                ESP_LOGI("RBTIME", "SNTP initialized after network connection");
            }

            // Perform synchronization
            rbtime_unlock(); // Release before calling force_sntp_sync
            err = rbtime_force_sntp_sync(timeout_ms);

            if (err == RBPOWER_OK) {
                ESP_LOGI("RBTIME", "Time synchronized successfully");
            } else {
                ESP_LOGW("RBTIME", "Synchronization failed: %d, SNTP will continue retrying in background", err);
            }
            break;

        case RBTIME_SOURCE_ESP_NOW_HUB:
            ESP_LOGW("RBTIME", "ESP-NOW synchronization not implemented");
            err = RBPOWER_ERR_NOT_IMPLEMENTED;
            rbtime_unlock();
            break;

        case RBTIME_SOURCE_MANUAL:
            ESP_LOGD("RBTIME", "Manual time source does not require synchronization");
            err = RBPOWER_OK;
            rbtime_unlock();
            break;

        default:
            ESP_LOGE("RBTIME", "Unsupported source for synchronization: %d", source);
            err = RBPOWER_ERR_INVALID_ARG;
            rbtime_unlock();
            break;
    }

    return err;
}

rbpower_err_t rbtime_set_timezone(const char* timezone)
{
    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (timezone == NULL || strlen(timezone) == 0) {
        return RBPOWER_ERR_INVALID_ARG;
    }

    ESP_LOGD("RBTIME", "Setting timezone: %s", timezone);

    // VALIDATE the POSIX TZ string before accepting it. A rejected string leaves the
    // current TZ untouched and returns INVALID_ARG (HTTP layer maps it to 400).
    if (!rbtime_tz_is_valid(timezone)) {
        ESP_LOGE("RBTIME", "Timezone REJECTED (not a valid POSIX TZ, falls back to UTC): '%s' "
                           "— use e.g. 'CST-8' or 'CET-1CEST,M3.5.0,M10.5.0/3'", timezone);
        return RBPOWER_ERR_INVALID_ARG;
    }

    if (!rbtime_lock(100)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    // Save new timezone to configuration
    strncpy(g_time_ctx.config.timezone, timezone, sizeof(g_time_ctx.config.timezone) - 1);
    g_time_ctx.config.timezone[sizeof(g_time_ctx.config.timezone) - 1] = '\0';

    // Apply timezone to system
    setenv("TZ", timezone, 1);
    tzset();

    rbtime_unlock();

    ESP_LOGI("RBTIME", "Timezone updated to: %s", timezone);
    return RBPOWER_OK;
}

rbpower_err_t rbtime_set_manual_time(uint64_t unix_timestamp_sec)
{
    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }

    ESP_LOGI("RBTIME", "Setting manual time: %llu", unix_timestamp_sec);

    if (!rbtime_lock(100)) {
        return RBPOWER_ERR_TIMEOUT;
    }

    struct timeval tv = {
        .tv_sec = unix_timestamp_sec,
        .tv_usec = 0
    };

    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE("RBTIME", "Error setting system time");
        rbtime_unlock();
        return RBPOWER_ERR_TIME_SET_ERROR;
    }

    // Update statistics
    g_time_ctx.stats.time_corrections_count++;
    g_time_ctx.stats.last_sync_timestamp_us = esp_timer_get_time();
    g_time_ctx.last_manual_time_us = unix_timestamp_sec * 1000000ULL;

    g_time_ctx.active_source = RBTIME_SOURCE_MANUAL;
    rbtime_set_status(RBTIME_STATUS_SYNCED);

    rbtime_unlock();

    /* a hand-set clock opens the store gate too: re-stamp + flush the pre-sync queue like an SNTP sync does,
     * or an offline hub lost it at the next reboot) */
    {
        extern void rbstats_pending_on_time_sync(void);
        rbstats_pending_on_time_sync();
    }

    // === INTEGRATION: Statistics timers resynchronization after manual time set ===
    extern rbpower_err_t rbpower_statistics_timers_sync_with_system_time(void);
    rbpower_err_t sync_result = rbpower_statistics_timers_sync_with_system_time();
    if (sync_result == RBPOWER_OK) {
        ESP_LOGI("RBTIME", "Statistics timers resynchronized after manual time set");
    } else {
        ESP_LOGW("RBTIME", "Statistics timers resynchronization error: %d", sync_result);
    }

    ESP_LOGI("RBTIME", "Manual time set successfully");
    return RBPOWER_OK;
}

rbpower_err_t rbtime_register_event_callback(rbtime_event_callback_t callback, void *user_data)
{
    if (!g_time_ctx.initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (!rbtime_lock(100)) {
        return RBPOWER_ERR_TIMEOUT;
    }
    
    g_time_ctx.event_callback = callback;
    g_time_ctx.callback_user_data = user_data;

    rbtime_unlock();

    ESP_LOGD("RBTIME", "Event callback registered");
    return RBPOWER_OK;
}

uint64_t rbtime_get_unix_timestamp(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return tv.tv_sec;
    }
    return 0;
}

uint64_t rbtime_get_uptime_us(void)
{
    return esp_timer_get_time();
}

bool rbtime_is_time_valid(void)
{
    if (!g_time_ctx.initialized) {
        return false;
    }
    /* a clock before the firmware's build day is not real time, whatever the status says */
    if ((uint32_t)time(NULL) < rbtime_not_before()) return false;
    
    return (g_time_ctx.status == RBTIME_STATUS_SYNCED || 
            g_time_ctx.status == RBTIME_STATUS_FALLBACK);
}