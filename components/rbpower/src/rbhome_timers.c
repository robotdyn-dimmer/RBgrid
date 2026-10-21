/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_timers.c
 * @brief Statistics period timers, aligned to wall-clock period boundaries.
 *
 * Implements a chain of one-shot esp_timer timers with automatic time correction: each
 * callback computes the time to the next period boundary and arms the next timer. Only the
 * 3-second and 15-minute periods have timers; HOUR and longer periods are finalized by
 * boundary checks in rbhome_statistics.c. A periodic watchdog re-arms a broken chain and
 * re-aligns timers after a clock step. Timer callbacks run in the esp_timer task.
 *
 * @version 2.0.0
 * @date 2025-01-10
 */

#include "rbpower.h"
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbhome_types.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#define _POSIX_TIMERS 1  ///< Enable POSIX-compatible time functions
#include <sys/time.h>
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */

static const char* TAG = "RBHOME_TIMERS";

// External rbhome integration functions
extern void rbpower_home_start_3sec_period_callback(uint64_t period_start_time_us, uint64_t period_end_time_us);
extern void rbpower_home_end_3sec_period_callback(uint64_t period_end_time_us);

//=============================================================================
// EXTERNAL VARIABLES (from rbhome_statistics.c)
//=============================================================================
extern rbpower_statistics_system_t g_stats_system;
extern bool g_statistics_initialized;

//=============================================================================
// SCHEDULING SELF-HEAL
//=============================================================================
/* The period chain is one-shot timers re-armed from their own callback; the hourly SNTP resync
 * restarts them from another task. A lost re-arm (race, transient error) used to kill the chain
 * silently and permanently. The periodic watchdog below re-arms any RUNNING/ERROR timer that is
 * no longer armed in the kernel. Counters are exposed via rbpower_statistics_timers_diag(). */
static unsigned long s_timer_rearm_races = 0;   /* benign INVALID_STATE on re-arm (resync race) */
static unsigned long s_timer_selfheals   = 0;   /* watchdog re-arms of a dead timer */
static esp_timer_handle_t s_timer_watchdog = NULL;
#define RBPOWER_TIMER_WATCHDOG_PERIOD_US (60ULL * 1000 * 1000)   ///< Scheduling watchdog period, µs (60 s)

static uint64_t calculate_time_to_next_boundary(rbpower_stat_period_type_t period_type);

/**
 * @brief Scheduling watchdog (every 60 s): runs the home housekeeping tick, re-arms dead
 *        period timers and re-aligns armed timers that drifted from the wall-clock boundary.
 */
static void timer_watchdog_cb(void* arg)
{
    extern void rbpower_home_housekeeping_tick(void);
    rbpower_home_housekeeping_tick();   /* guaranteed periodic host (heartbeat / day rollover / NVS flush) */
    (void)arg;
    if (!g_statistics_initialized || !g_stats_system.timers_running) return;
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
        rbpower_timer_info_t* ti = &g_stats_system.timers[period];
        if (ti->timer_handle == NULL) continue;
        if (ti->state != RBPOWER_TIMER_STATE_RUNNING &&
            ti->state != RBPOWER_TIMER_STATE_ERROR) continue;   /* STOPPED = intentional */
        if (esp_timer_is_active(ti->timer_handle)) {
            /* armed — but is it still aligned? A timer armed before SNTP synced (cold boot: clock at the
             * build default) or across a clock step fires at a stale wall-clock boundary (e.g. the
             * first quarter fires at 16:00:16 and is folded into the next hour). Re-arm when the armed
             * instant disagrees with the boundary the CURRENT clock gives by more than 2 s. */
            if (period >= RBPOWER_STAT_PERIOD_15MIN) {
                uint64_t want = esp_timer_get_time() + calculate_time_to_next_boundary(period);
                int64_t diff = (int64_t)ti->next_trigger_time_us - (int64_t)want;
                if (diff > 2000000LL || diff < -2000000LL) {
                    esp_timer_stop(ti->timer_handle);
                    uint64_t d2 = want - esp_timer_get_time();
                    if (esp_timer_start_once(ti->timer_handle, d2) == ESP_OK) {
                        ti->next_trigger_time_us = esp_timer_get_time() + d2;
                        ESP_LOGW(TAG, "period %d timer RE-ALIGNED to the wall clock (was off by %lld ms)", period, (long long)(diff / 1000));
                        rbpower_diag_printf("timer p%d re-aligned to wall clock (off by %llds)", (int)period, (long long)(diff / 1000000));
                    }
                }
            }
            continue;
        }
        uint64_t delay = calculate_time_to_next_boundary(period);
        uint64_t nominal = RBPOWER_GET_PERIOD_DURATION_US(period) / RBGRID_STATS_TIME_SCALE;
        if (delay < 1000 || delay > nominal * 2) delay = nominal;   /* same sanity as the re-arm */
        int prev_state = (int)ti->state;
        if (esp_timer_start_once(ti->timer_handle, delay) == ESP_OK) {
            ti->state = RBPOWER_TIMER_STATE_RUNNING;
            ti->next_trigger_time_us = esp_timer_get_time() + delay;
            s_timer_selfheals++;
            ESP_LOGW(TAG, "WATCHDOG: period %d timer was DEAD (state=%d) — re-armed in %llu us "
                          "(selfheal #%lu)", period, prev_state, delay, s_timer_selfheals);
        }
    }
}

/**
 * @brief One-line scheduling diagnosis (timerstat diagnostic verb).
 *
 * Per-timer state/armed/fires/next and the global pipeline counters (events processed, drops,
 * timer errors, re-arm races, watchdog self-heals).
 *
 * @param[out] out Destination string buffer.
 * @param[in]  cap Size of @p out, bytes.
 * @return Number of characters written (0 when @p out is NULL or @p cap is 0).
 */
int rbpower_statistics_timers_diag(char* out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if (!g_statistics_initialized) return snprintf(out, cap, "timers: not initialized");
    int w = 0;
    w = rb_scat(out, cap, w, "timers(run=%d):", (int)g_stats_system.timers_running);
    for (rbpower_stat_period_type_t p = 0; p < RBPOWER_STAT_PERIOD_COUNT; p++) {
        rbpower_timer_info_t* ti = &g_stats_system.timers[p];
        if (ti->timer_handle == NULL) continue;
        int64_t next_ms = ((int64_t)ti->next_trigger_time_us - esp_timer_get_time()) / 1000;
        w = rb_scat(out, cap, w, " p%d[st=%d act=%d fires=%lu next=%lldms]",
                      (int)p, (int)ti->state, (int)esp_timer_is_active(ti->timer_handle),
                      (unsigned long)ti->total_triggers, (long long)next_ms);
        if ((size_t)w >= cap - 1) return w;
    }
    uint64_t now = esp_timer_get_time();
    w = rb_scat(out, cap, w,
                  " | evproc=%lu last_act=%llums ago drop=%lu terr=%lu races=%lu selfheal=%lu",
                  (unsigned long)g_stats_system.global_stats.total_events_processed,
                  (unsigned long long)((now - g_stats_system.global_stats.last_activity_time_us) / 1000),
                  (unsigned long)g_stats_system.global_stats.events_dropped,
                  (unsigned long)g_stats_system.global_stats.timer_errors,
                  s_timer_rearm_races, s_timer_selfheals);
    return w;
}

//=============================================================================
// PRIVATE FUNCTIONS - TIME CALCULATIONS
//=============================================================================

/**
 * @brief Calculate time to next 3-second period boundary
 */
static uint64_t calculate_time_to_next_3sec_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Align to next 3-second boundary in minute
    time_t seconds_since_minute = tv.tv_sec % 60;
    time_t next_3sec_boundary = ((seconds_since_minute / 3) + 1) * 3;

    time_t target_time;
    if (next_3sec_boundary >= 60) {
        // Move to next minute, start = 0 sec (which equals 3-sec boundary)
        target_time = tv.tv_sec + (60 - seconds_since_minute);
    } else {
        target_time = tv.tv_sec + (next_3sec_boundary - seconds_since_minute);
    }

    // Correct microsecond compensation
    uint64_t time_to_boundary = (target_time - tv.tv_sec) * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 100000) ? time_to_boundary : 100000; // Minimum 100ms
}

/**
 * @brief Calculate time to next 15-minute period boundary
 */
static uint64_t calculate_time_to_next_15min_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm* time_info = localtime(&tv.tv_sec);
    if (!time_info) {
        return 15 * 60 * 1000000ULL; // Fallback: 15 minutes
    }

    // Align to xx:00, xx:15, xx:30, xx:45
    int current_minute = time_info->tm_min;
    int next_15min_boundary = ((current_minute / 15) + 1) * 15;

    int seconds_to_next_15min;
    if (next_15min_boundary >= 60) {
        // Move to next hour
        seconds_to_next_15min = 3600 - (current_minute * 60 + time_info->tm_sec);
    } else {
        seconds_to_next_15min = (next_15min_boundary - current_minute) * 60 - time_info->tm_sec;
    }

    // Correct microsecond compensation
    uint64_t time_to_boundary = (uint64_t)seconds_to_next_15min * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 1000000) ? time_to_boundary : 1000000; // Minimum 1 second
}

/**
 * @brief Calculate time to next hourly period boundary
 */
static uint64_t calculate_time_to_next_1hour_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm* time_info = localtime(&tv.tv_sec);
    if (!time_info) {
        return 3600 * 1000000ULL; // Fallback: 1 hour
    }

    // Align to next hour (xx:00:00)
    time_t seconds_to_next_hour = 3600 - (time_info->tm_min * 60 + time_info->tm_sec);

    // Correct microsecond compensation
    uint64_t time_to_boundary = (uint64_t)seconds_to_next_hour * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 1000000) ? time_to_boundary : 1000000; // Minimum 1 second
}

/**
 * @brief Calculate time to next daily period boundary
 */
static uint64_t calculate_time_to_next_1day_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm* time_info = localtime(&tv.tv_sec);
    if (!time_info) {
        return 24 * 3600 * 1000000ULL; // Fallback: 24 hours
    }

    // Align to next day (00:00:00)
    time_t seconds_to_next_day = 86400 - (time_info->tm_hour * 3600 +
                                         time_info->tm_min * 60 +
                                         time_info->tm_sec);

    // Correct microsecond compensation
    uint64_t time_to_boundary = (uint64_t)seconds_to_next_day * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 1000000) ? time_to_boundary : 1000000; // Minimum 1 second
}

/**
 * @brief Calculate time to next weekly period boundary
 */
static uint64_t calculate_time_to_next_1week_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm* time_info = localtime(&tv.tv_sec);
    if (!time_info) {
        return 7 * 24 * 3600 * 1000000ULL; // Fallback: 7 days
    }

    // Align to next Monday 00:00:00
    // Corrected logic for all weekdays (tm_wday: 0=Sunday, 1=Monday, ..., 6=Saturday)
    int days_to_monday;
    if (time_info->tm_wday == 0) {      // Sunday
        days_to_monday = 1;
    } else if (time_info->tm_wday == 1) { // Monday
        days_to_monday = 7;
    } else {                            // Tuesday-Saturday
        days_to_monday = 8 - time_info->tm_wday;
    }

    time_t seconds_to_next_week = days_to_monday * 86400 -
                                 (time_info->tm_hour * 3600 +
                                  time_info->tm_min * 60 +
                                  time_info->tm_sec);

    // Correct microsecond compensation
    uint64_t time_to_boundary = (uint64_t)seconds_to_next_week * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 1000000) ? time_to_boundary : 1000000; // Minimum 1 second
}

/**
 * @brief Calculate time to next monthly period boundary
 */
static uint64_t calculate_time_to_next_1month_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm* time_info = localtime(&tv.tv_sec);
    if (!time_info) {
        return 30 * 24 * 3600 * 1000000ULL; // Fallback: 30 days
    }

    // Align to first day of next month 00:00:00
    struct tm next_month = *time_info;
    next_month.tm_mon++;
    next_month.tm_mday = 1;
    next_month.tm_hour = 0;
    next_month.tm_min = 0;
    next_month.tm_sec = 0;
    next_month.tm_isdst = -1;  // Allow mktime() to auto-determine DST

    // Handle year transition
    if (next_month.tm_mon >= 12) {
        next_month.tm_mon = 0;
        next_month.tm_year++;
    }

    time_t target_time = mktime(&next_month);
    if (target_time == -1) {
        return 30 * 24 * 3600 * 1000000ULL; // Fallback
    }

    // Correct microsecond compensation
    time_t seconds_to_next_month = target_time - tv.tv_sec;
    uint64_t time_to_boundary = (uint64_t)seconds_to_next_month * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 1000000) ? time_to_boundary : 1000000; // Minimum 1 second
}

/**
 * @brief Calculate time to next yearly period boundary
 */
static uint64_t calculate_time_to_next_1year_boundary(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm* time_info = localtime(&tv.tv_sec);
    if (!time_info) {
        return 365 * 24 * 3600 * 1000000ULL; // Fallback: 365 days
    }

    // Align to January 1st of next year 00:00:00
    struct tm next_year = *time_info;
    next_year.tm_year++;
    next_year.tm_mon = 0;
    next_year.tm_mday = 1;
    next_year.tm_hour = 0;
    next_year.tm_min = 0;
    next_year.tm_sec = 0;
    next_year.tm_isdst = -1;  // Allow mktime() to auto-determine DST

    time_t target_time = mktime(&next_year);
    if (target_time == -1) {
        return 365 * 24 * 3600 * 1000000ULL; // Fallback
    }

    // Correct microsecond compensation
    time_t seconds_to_next_year = target_time - tv.tv_sec;
    uint64_t time_to_boundary = (uint64_t)seconds_to_next_year * 1000000ULL - tv.tv_usec;

    return (time_to_boundary > 1000000) ? time_to_boundary : 1000000; // Minimum 1 second
}

/**
 * @brief Universal function to calculate time to next period boundary
 */
static uint64_t calculate_time_to_next_boundary(rbpower_stat_period_type_t period_type)
{
#if RBGRID_STATS_TIME_SCALE > 1
    /* DEBUG accelerated stats (RBGRID_STATS_TIME_SCALE): fire at a FIXED periodic interval =
     * nominal/scale, bypassing wall-clock (:00/:15) alignment. The boundary CLOCK in
     * rbhome_statistics.c is scaled by the same factor and anchored at boot=epoch 0, so virtual
     * XX:00 lands exactly on a timer fire (no missed/aliased boundaries, and NTP-immune since the
     * virtual clock is uptime-based, not the NTP-corrected wall clock).
     * HIGH-SCALE FLOOR (e.g. scale 2920): the scaled 3SEC interval drops to ~1ms = a 1 kHz
     * callback+event storm that saturates the stats task. Clamp every scaled interval to >=50ms:
     * at the floor the native 3sec chain under-fills (native quarters never reach ready) — fine,
     * high-scale runs are driven by the statfeed QUARTER injector, which bypasses the native chain. */
    {
        uint64_t iv = RBPOWER_GET_PERIOD_DURATION_US(period_type) / RBGRID_STATS_TIME_SCALE;
        const uint64_t floor_us = 50000;
        return (iv < floor_us) ? floor_us : iv;
    }
#else
    switch (period_type) {
        case RBPOWER_STAT_PERIOD_3SEC:
            return calculate_time_to_next_3sec_boundary();
        case RBPOWER_STAT_PERIOD_15MIN:
            return calculate_time_to_next_15min_boundary();
        case RBPOWER_STAT_PERIOD_1HOUR:
            return calculate_time_to_next_1hour_boundary();
        case RBPOWER_STAT_PERIOD_DAILY:
            return calculate_time_to_next_1day_boundary();
        case RBPOWER_STAT_PERIOD_WEEKLY:
            return calculate_time_to_next_1week_boundary();
        case RBPOWER_STAT_PERIOD_MONTHLY:
            return calculate_time_to_next_1month_boundary();
        case RBPOWER_STAT_PERIOD_YEARLY:
            return calculate_time_to_next_1year_boundary();
        default:
            ESP_LOGE(TAG, "Unknown period type: %d", period_type);
            return RBPOWER_GET_PERIOD_DURATION_US(period_type);
    }
#endif
}

//=============================================================================
// PRIVATE FUNCTIONS - TIMER CALLBACKS
//=============================================================================

/**
 * @brief Universal timer callback for all periods
 * Implements chain of one-shot timers with automatic time correction
 */
static void statistics_timer_callback(void* arg)
{
    rbpower_stat_period_type_t period_type = (rbpower_stat_period_type_t)(uintptr_t)arg;
    
    if (!g_statistics_initialized || !g_stats_system.timers_running) {
        return;
    }
    
    uint64_t current_time = esp_timer_get_time();
    rbpower_timer_info_t* timer_info = &g_stats_system.timers[period_type];

    // === RBHOME INTEGRATION ===
    if (period_type == RBPOWER_STAT_PERIOD_3SEC) {
        // End previous 3-sec period
        ESP_LOGD("TIMER3SEC", "END_3sec_period_callback time=%llu us", current_time);
        rbpower_home_end_3sec_period_callback(current_time);

        // Calculate next period time to start new one
        uint64_t next_period_start = current_time;
        uint64_t next_period_end = current_time + RBPOWER_GET_PERIOD_DURATION_US(RBPOWER_STAT_PERIOD_3SEC) / RBGRID_STATS_TIME_SCALE;

        // Start new 3-sec period
        //ESP_LOGD("TIMER3SEC", "START_3sec_period_callback time=%llu us, next_start=%llu us, next_end=%llu us",current_time, next_period_start, next_period_end);
        rbpower_home_start_3sec_period_callback(next_period_start, next_period_end);
    }

    // === SEND EVENT TO STATISTICS TASK ===
    rbpower_timer_event_context_t event = {
        .event_type = RBPOWER_TIMER_EVENT_PERIOD_END,
        .period_type = period_type,
        .unit_id = RBPOWER_HOME_INVALID_ID, // All units
        .event_timestamp_us = current_time
    };

    // Calculate completed period parameters
    uint64_t period_duration_us = RBPOWER_GET_PERIOD_DURATION_US(period_type);
    event.data.period_event.period_duration_us = period_duration_us;
    event.data.period_event.period_start_us = current_time - period_duration_us;

    // Send to Statistics Task queue
    if (g_stats_system.statistics_event_queue) {
        BaseType_t result = xQueueSendFromISR(g_stats_system.statistics_event_queue, &event, NULL);
        if (result != pdTRUE) {
            g_stats_system.global_stats.events_dropped++;
            ESP_LOGW(TAG, "Period %d event dropped: queue full", period_type);
        }
    }

    // === UPDATE TIMER STATISTICS ===
    timer_info->total_triggers++;
    timer_info->last_trigger_time_us = current_time;
    
    // === CALCULATE AND START NEXT TIMER ===
    /* The hourly SNTP resync calls
     * timers_restart() from the SNTP task with NO serialization against this callback (esp_timer
     * task). Any overlap could latch state=ERROR via a benign ESP_ERR_INVALID_STATE
     * ("already armed by the resync"), and ERROR would permanently stop the one-shot re-arm chain —
     * the period pipeline would die silently. Therefore: re-arm from ERROR too (un-latch), treat
     * INVALID_STATE as benign, and let the watchdog below heal anything else. */
    if (timer_info->state == RBPOWER_TIMER_STATE_RUNNING ||
        timer_info->state == RBPOWER_TIMER_STATE_ERROR) {
        // Calculate time to next period boundary
        uint64_t time_to_next_boundary = calculate_time_to_next_boundary(period_type);

        // Check interval sanity. Bounds scaled by RBGRID_STATS_TIME_SCALE (=1 in production → no-op)
        // so the debug accelerated interval isn't rejected as "unexpected" and reset to full duration.
        uint64_t min_interval = (RBPOWER_GET_PERIOD_DURATION_US(period_type) / RBGRID_STATS_TIME_SCALE) / 2;
        uint64_t max_interval = (RBPOWER_GET_PERIOD_DURATION_US(period_type) / RBGRID_STATS_TIME_SCALE) * 2;
#if RBGRID_STATS_TIME_SCALE > 1
        // Keep the sanity window consistent with the high-scale 50ms floor in
        // calculate_time_to_next_boundary — otherwise the floored interval is "unexpected",
        // gets reset to the sub-ms raw value and the clamp is defeated (with a warn per fire).
        if (max_interval < 100000) max_interval = 100000;
#endif

        if (time_to_next_boundary < min_interval || time_to_next_boundary > max_interval) {
            ESP_LOGW(TAG, "Unexpected interval for period %d: %llu us, using standard",
                     period_type, time_to_next_boundary);
            time_to_next_boundary = RBPOWER_GET_PERIOD_DURATION_US(period_type) / RBGRID_STATS_TIME_SCALE;
        }

        // Start next one-shot timer
        esp_err_t timer_err = esp_timer_start_once(timer_info->timer_handle, time_to_next_boundary);

        if (timer_err == ESP_OK) {
            timer_info->state = RBPOWER_TIMER_STATE_RUNNING;   /* un-latch a prior ERROR */
            timer_info->next_trigger_time_us = current_time + time_to_next_boundary;

            //ESP_LOGD(TAG, "Period %d: next timer in %llu us",period_type, time_to_next_boundary);
        } else if (timer_err == ESP_ERR_INVALID_STATE) {
            /* Already armed — the SNTP-resync restart raced us and won. Benign: scheduling is
             * alive (the resync's arm is wall-clock-aligned), just keep state RUNNING. */
            timer_info->state = RBPOWER_TIMER_STATE_RUNNING;
            s_timer_rearm_races++;
            ESP_LOGW(TAG, "Period %d re-arm: already armed (resync race #%lu) — benign",
                     period_type, (unsigned long)s_timer_rearm_races);
        } else {
            ESP_LOGE(TAG, "Failed to start next timer for period %d: %s (watchdog will re-arm)",
                     period_type, esp_err_to_name(timer_err));
            timer_info->state = RBPOWER_TIMER_STATE_ERROR;
            g_stats_system.global_stats.timer_errors++;
        }
    }

    //ESP_LOGD(TAG, "Completed period %d (trigger #%lu)", period_type, timer_info->total_triggers);
}

//=============================================================================
// PRIVATE FUNCTIONS - TIMER MANAGEMENT
//=============================================================================

/**
 * @brief Create timer for specific period
 */
static rbpower_err_t create_period_timer(rbpower_stat_period_type_t period_type)
{
    if (period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    rbpower_timer_info_t* timer_info = &g_stats_system.timers[period_type];

    // Check that timer is not already created
    if (timer_info->timer_handle != NULL) {
        ESP_LOGD(TAG, "Timer for period %d already created", period_type);
        return RBPOWER_ERR_ALREADY_INIT;
    }

    // Timer configuration
    char timer_name[16];
    snprintf(timer_name, sizeof(timer_name), "stat_%d", period_type);

    esp_timer_create_args_t timer_args = {
        .callback = statistics_timer_callback,
        .arg = (void*)(uintptr_t)period_type,
        .name = timer_name,
        .dispatch_method = ESP_TIMER_TASK
    };

    esp_err_t err = esp_timer_create(&timer_args, &timer_info->timer_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer for period %d: %s", period_type, esp_err_to_name(err));
        return RBPOWER_ERR_TIMER_CREATE_FAILED;
    }

    // Initialize timer info
    timer_info->period_type = period_type;
    timer_info->state = RBPOWER_TIMER_STATE_STOPPED;
    timer_info->period_duration_us = RBPOWER_GET_PERIOD_DURATION_US(period_type);
    timer_info->requires_sync = true;
    timer_info->total_triggers = 0;
    timer_info->last_trigger_time_us = 0;
    timer_info->next_trigger_time_us = 0;

    ESP_LOGD(TAG, "Timer for period %d created", period_type);
    return RBPOWER_OK;
}

/**
 * @brief Start timer for specific period
 */
static rbpower_err_t start_period_timer(rbpower_stat_period_type_t period_type)
{
    if (period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    rbpower_timer_info_t* timer_info = &g_stats_system.timers[period_type];

    if (timer_info->timer_handle == NULL) {
        ESP_LOGE(TAG, "Timer for period %d not created", period_type);
        return RBPOWER_ERR_NOT_INIT;
    }

    if (timer_info->state == RBPOWER_TIMER_STATE_RUNNING) {
        ESP_LOGD(TAG, "Timer for period %d already running", period_type);
        return RBPOWER_OK;
    }

    // Calculate time to next period boundary
    uint64_t time_to_boundary = calculate_time_to_next_boundary(period_type);

    // Start one-shot timer with boundary alignment
    esp_err_t timer_err = esp_timer_start_once(timer_info->timer_handle, time_to_boundary);

    if (timer_err == ESP_OK) {
        timer_info->state = RBPOWER_TIMER_STATE_RUNNING;
        timer_info->next_trigger_time_us = esp_timer_get_time() + time_to_boundary;

        ESP_LOGI(TAG, "Timer period %d started (first trigger in %llu us)",
                 period_type, time_to_boundary);
        return RBPOWER_OK;
    } else {
        ESP_LOGE(TAG, "Failed to start timer for period %d: %s",
                 period_type, esp_err_to_name(timer_err));
        timer_info->state = RBPOWER_TIMER_STATE_ERROR;
        g_stats_system.global_stats.timer_errors++;
        return RBPOWER_ERR_TIMER_START_FAILED;
    }
}

/**
 * @brief Stop timer for specific period
 */
static rbpower_err_t stop_period_timer(rbpower_stat_period_type_t period_type)
{
    if (period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    rbpower_timer_info_t* timer_info = &g_stats_system.timers[period_type];

    if (timer_info->timer_handle == NULL) {
        return RBPOWER_OK; // Timer not created - this is OK
    }

    if (timer_info->state != RBPOWER_TIMER_STATE_RUNNING) {
        return RBPOWER_OK; // Timer not running - this is OK
    }

    esp_err_t err = esp_timer_stop(timer_info->timer_handle);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        timer_info->state = RBPOWER_TIMER_STATE_STOPPED;
        timer_info->next_trigger_time_us = 0;
        ESP_LOGD(TAG, "Timer period %d stopped", period_type);
        return RBPOWER_OK;
    } else {
        ESP_LOGE(TAG, "Failed to stop timer for period %d: %s",
                 period_type, esp_err_to_name(err));
        return RBPOWER_ERR_TIMER_STOP_FAILED;
    }
}

//=============================================================================
// PUBLIC API FUNCTIONS
//=============================================================================

/**
 * @brief Create and start the 3-second and 15-minute period timers and the scheduling watchdog.
 *
 * If the wall clock is not valid yet the timers start on the fallback time and are re-aligned
 * on SNTP sync. Calling it while the timers run is a no-op.
 *
 * @return RBPOWER_OK on success (individual timer failures are logged).
 * @retval RBPOWER_ERR_NOT_INIT the statistics system is not initialized.
 */
rbpower_err_t rbpower_statistics_timers_start(void)
{
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (g_stats_system.timers_running) {
        ESP_LOGD(TAG, "Statistics timers already running");
        return RBPOWER_OK;
    }

    // Start timers with fallback time, resynchronize when SNTP received
    bool time_valid = rbtime_is_time_valid();
    ESP_LOGI(TAG, "Starting statistics timers | time_sync=%s",
             time_valid ? "YES" : "NO (fallback, will resync on SNTP)");

    // Create and start timers ONLY for 3SEC and 15MIN
    // HOUR/DAY/WEEK/MONTH/YEAR finalized via time boundary checks
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
        // Skip periods finalized via time masks
        if (period == RBPOWER_STAT_PERIOD_1HOUR ||
            period == RBPOWER_STAT_PERIOD_DAILY ||
            period == RBPOWER_STAT_PERIOD_WEEKLY ||
            period == RBPOWER_STAT_PERIOD_MONTHLY ||
            period == RBPOWER_STAT_PERIOD_YEARLY) {
            ESP_LOGD(TAG, "Timer period %d DISABLED (using time masks)", period);
            continue;
        }

        // Create timer (only for 3SEC and 15MIN)
        rbpower_err_t err = create_period_timer(period);
        if (err != RBPOWER_OK) {
            ESP_LOGE(TAG, "Failed to create timer for period %d", period);
            continue;
        }

        // Start timer
        err = start_period_timer(period);
        if (err != RBPOWER_OK) {
            ESP_LOGE(TAG, "Failed to start timer for period %d", period);
            continue;
        }

        ESP_LOGI(TAG, "Timer period %d started", period);
    }

    g_stats_system.timers_running = true;
    g_stats_system.time_synchronized = true;

    /* Scheduling watchdog (periodic — the kernel re-arms it itself, so it can't die the way the
     * one-shot chain can): re-arms any RUNNING/ERROR period timer found un-armed. */
    if (s_timer_watchdog == NULL) {
        const esp_timer_create_args_t wd_args = {
            .callback = timer_watchdog_cb,
            .name = "stats_timer_wd",
        };
        if (esp_timer_create(&wd_args, &s_timer_watchdog) == ESP_OK)
            esp_timer_start_periodic(s_timer_watchdog, RBPOWER_TIMER_WATCHDOG_PERIOD_US);
        else
            ESP_LOGE(TAG, "scheduling watchdog create failed");
    }

    ESP_LOGI(TAG, "Statistics timers started and synchronized with system time");
    return RBPOWER_OK;
}

/**
 * @brief Stop and delete all period timers (the watchdog keeps running but skips stopped timers).
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT the statistics system is not initialized.
 */
rbpower_err_t rbpower_statistics_timers_stop(void)
{
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    ESP_LOGI(TAG, "Stopping statistics timers...");

    // Stop all timers
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
        stop_period_timer(period);

        rbpower_timer_info_t* timer_info = &g_stats_system.timers[period];
        if (timer_info->timer_handle != NULL) {
            esp_timer_delete(timer_info->timer_handle);
            timer_info->timer_handle = NULL;
            timer_info->state = RBPOWER_TIMER_STATE_STOPPED;
        }
    }

    g_stats_system.timers_running = false;

    ESP_LOGI(TAG, "Statistics timers stopped");
    return RBPOWER_OK;
}

/**
 * @brief Re-arm one period timer at the next boundary computed from the current clock.
 *
 * A concurrent re-arm by the timer's own callback (ESP_ERR_INVALID_STATE) is benign and
 * reported as success.
 *
 * @param[in] period_type Period whose timer is restarted.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT           the statistics system is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG        @p period_type is out of range.
 * @retval RBPOWER_ERR_TIMER_START_FAILED the timer could not be armed (the watchdog retries).
 * @return Other codes from stopping the timer.
 */
rbpower_err_t rbpower_statistics_timers_restart(rbpower_stat_period_type_t period_type)
{
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Restarting timer for period %d...", period_type);

    // Stop timer
    rbpower_err_t err = stop_period_timer(period_type);
    if (err != RBPOWER_OK) {
        ESP_LOGE(TAG, "Failed to stop timer for period %d", period_type);
        return err;
    }

    // Recalculate time to next boundary (accounting for possible system time changes)
    uint64_t time_to_boundary = calculate_time_to_next_boundary(period_type);

    // Start timer with new interval
    rbpower_timer_info_t* timer_info = &g_stats_system.timers[period_type];
    esp_err_t timer_err = esp_timer_start_once(timer_info->timer_handle, time_to_boundary);

    if (timer_err == ESP_OK) {
        timer_info->state = RBPOWER_TIMER_STATE_RUNNING;
        timer_info->next_trigger_time_us = esp_timer_get_time() + time_to_boundary;

        ESP_LOGD(TAG, "Timer period %d restarted (trigger in %llu us)",
                 period_type, time_to_boundary);
        return RBPOWER_OK;
    } else if (timer_err == ESP_ERR_INVALID_STATE) {
        /* Already armed — the timer's own callback re-armed it between our stop and start
         * (resync race, other direction). Benign: scheduling is alive. */
        timer_info->state = RBPOWER_TIMER_STATE_RUNNING;
        s_timer_rearm_races++;
        ESP_LOGW(TAG, "Restart period %d: already armed (resync race #%lu) — benign",
                 period_type, s_timer_rearm_races);
        return RBPOWER_OK;
    } else {
        ESP_LOGE(TAG, "Failed to restart timer for period %d: %s (watchdog will re-arm)",
                 period_type, esp_err_to_name(timer_err));
        timer_info->state = RBPOWER_TIMER_STATE_ERROR;
        g_stats_system.global_stats.timer_errors++;
        return RBPOWER_ERR_TIMER_START_FAILED;
    }
}

/**
 * @brief Re-align all running period timers to the system clock (call after SNTP sync).
 * @return RBPOWER_OK on success (per-timer failures are logged).
 * @retval RBPOWER_ERR_NOT_INIT       the statistics system is not initialized.
 * @retval RBPOWER_ERR_TIME_SET_ERROR the system time is not valid yet.
 */
rbpower_err_t rbpower_statistics_timers_sync_with_system_time(void)
{
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    ESP_LOGI(TAG, "Synchronizing timers with system time...");

    // Check system time readiness
    if (!rbtime_is_time_valid()) {
        ESP_LOGE(TAG, "System time not ready for synchronization");
        return RBPOWER_ERR_TIME_SET_ERROR;
    }

    // Restart all active timers with new synchronization
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
        rbpower_timer_info_t* timer_info = &g_stats_system.timers[period];

        if (timer_info->state == RBPOWER_TIMER_STATE_RUNNING) {
            rbpower_err_t err = rbpower_statistics_timers_restart(period);
            if (err != RBPOWER_OK) {
                ESP_LOGW(TAG, "Failed to resync timer period %d", period);
            }
        }
    }

    g_stats_system.time_synchronized = true;

    ESP_LOGI(TAG, "Timer synchronization completed");
    return RBPOWER_OK;
}

/*
 * Copy the state of one period timer (documented in rbhome_types.h).
 */
rbpower_err_t rbpower_statistics_timers_get_info(rbpower_stat_period_type_t period_type, 
                                                 rbpower_timer_info_t* timer_info)
{
    if (!timer_info) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_statistics_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    if (period_type >= RBPOWER_STAT_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    memcpy(timer_info, &g_stats_system.timers[period_type], sizeof(rbpower_timer_info_t));
    return RBPOWER_OK;
}