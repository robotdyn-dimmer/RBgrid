/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstats_migrate.c
 * @brief Deferred statistics-format migration engine and pending-record queue.
 *        See rbstats_migrate.h for the lifecycle contract.
 */
#include "rbstats_migrate.h"
#include "rbextflash_operations.h"      /* rbpower_flash_operations_init (resume after migrate) */
#include "rbextflash_buffer_write.h"    /* rbpower_flash_buffer_write_basic_set (queue flush)   */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"

static const char *TAG = "RBSTATS_MIGRATE";

/* ---- state ---- */
static volatile rbstats_store_state_t s_state = RBSTATS_STORE_READY;
static volatile uint8_t  s_progress = 100;
static uint32_t          s_from_version;
static rbstats_migrator_fn s_migrator;
static uint32_t          s_migrator_from;
static TaskHandle_t      s_task;

/* ---- pending queue (PSRAM ring; entries are the final packed transfer structs, so the
 *      time-sync re-stamp only has to patch each entry's timestamp fields in place) ---- */
/** @brief One queued record set (packed). */
typedef struct {
    rbpower_electrical_transfer_t electrical; ///< Electrical record
    rbpower_energy_transfer_t     energy;     ///< Energy record
    rbpower_economics_transfer_t  economics;  ///< Economics record
    rbstats_v4_meta_t             meta;       ///< v4 metadata (dur/q/res/plan/et_*) travelling with the set
    uint16_t unit_id;                         ///< Unit ID
    uint8_t  period;                          ///< rbpower_period_type_t
    uint8_t  has_meta;                        ///< 1 when @c meta is valid
} __attribute__((packed)) pending_rec_t;

#define PENDING_CAP 2048   ///< Queue entries (x ~70 B = ~140 KB PSRAM; >= 48 h of hourly sets for 40+ units)
static pending_rec_t     *s_pend;         /* lazy PSRAM alloc */
static size_t             s_pend_capacity;
static size_t             s_pend_head, s_pend_count;
static size_t             s_pend_dropped;
static portMUX_TYPE       s_pend_mux = portMUX_INITIALIZER_UNLOCKED;

/** @brief Allocate the pending queue on first use (PSRAM, falling back to a smaller internal-RAM queue). */
static bool pending_ensure_alloc(void)
{
    if (s_pend) return true;
    s_pend = heap_caps_calloc(PENDING_CAP, sizeof(pending_rec_t), MALLOC_CAP_SPIRAM);
    if (s_pend) {
        s_pend_capacity = PENDING_CAP;
    } else {  /* PSRAM missing/full — fall back to internal heap, smaller is still better than loss */
        s_pend = heap_caps_calloc(256, sizeof(pending_rec_t), MALLOC_CAP_8BIT);
        if (s_pend) {
            s_pend_capacity = 256;
            ESP_LOGW(TAG, "pending queue in INTERNAL heap (256 entries) — no PSRAM");
        }
    }
    return s_pend != NULL;
}

rbstats_store_state_t rbstats_migrate_state(void)   { return s_state; }
uint8_t               rbstats_migrate_progress(void){ return s_progress; }

const char* rbstats_store_state_str(void)
{
    switch (s_state) {
        case RBSTATS_STORE_READY:         return "ready";
        case RBSTATS_STORE_PENDING_VALID: return "pending_image_valid";
        case RBSTATS_STORE_MIGRATING:     return "migrating";
        case RBSTATS_STORE_INCOMPATIBLE:  return "incompatible";
    }
    return "?";
}

void rbstats_migrate_register(uint32_t from_version, rbstats_migrator_fn fn)
{
    s_migrator_from = from_version;
    s_migrator = fn;
}

esp_err_t rbstats_pending_enqueue(rbpower_unit_id_t unit_id, rbpower_period_type_t period,
                                  const rbpower_electrical_transfer_t* electrical,
                                  const rbpower_energy_transfer_t* energy,
                                  const rbpower_economics_transfer_t* economics,
                                  const rbstats_v4_meta_t* meta)
{
    if (!electrical || !energy || !economics) return ESP_ERR_INVALID_ARG;
    if (!pending_ensure_alloc()) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_pend_mux);
    size_t cap = s_pend_capacity;
    size_t idx;
    if (s_pend_count >= cap) {
        /* Ring full: drop the OLDEST (newest-wins — same policy as migration truncation). */
        s_pend_head = (s_pend_head + 1) % cap;
        s_pend_count--;
        s_pend_dropped++;
    }
    idx = (s_pend_head + s_pend_count) % cap;
    s_pend[idx].electrical = *electrical;
    s_pend[idx].energy     = *energy;
    s_pend[idx].economics  = *economics;
    if (meta) {
        s_pend[idx].meta = *meta;
        s_pend[idx].has_meta = 1;
    } else {
        memset(&s_pend[idx].meta, 0, sizeof(s_pend[idx].meta));
        s_pend[idx].has_meta = 0;
    }
    s_pend[idx].unit_id    = (uint16_t)unit_id;
    s_pend[idx].period     = (uint8_t)period;
    s_pend_count++;
    size_t n = s_pend_count, d = s_pend_dropped;
    portEXIT_CRITICAL(&s_pend_mux);
    ESP_LOGI(TAG, "finalize result queued (store %s): unit=0x%04X period=%d — pending=%u dropped=%u",
             rbstats_store_state_str(), (unsigned)unit_id, (int)period, (unsigned)n, (unsigned)d);
    return ESP_OK;
}

size_t rbstats_pending_count(void) { return s_pend_count; }

size_t rbstats_pending_flush(void)
{
    size_t flushed = 0;
    for (;;) {
        pending_rec_t rec;
        portENTER_CRITICAL(&s_pend_mux);
        if (s_pend_count == 0 || !s_pend) { portEXIT_CRITICAL(&s_pend_mux); break; }
        rec = s_pend[s_pend_head];
        s_pend_head = (s_pend_head + 1) % s_pend_capacity;
        s_pend_count--;
        portEXIT_CRITICAL(&s_pend_mux);
        esp_err_t e = rbpower_flash_buffer_write_record_set((rbpower_unit_id_t)rec.unit_id,
                                                            (rbpower_period_type_t)rec.period,
                                                            &rec.electrical, &rec.energy, &rec.economics,
                                                            rec.has_meta ? &rec.meta : NULL);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "pending flush write failed (%s) — record dropped", esp_err_to_name(e));
        } else {
            flushed++;
        }
        vTaskDelay(1);   /* yield between writes */
    }
    if (flushed || s_pend_dropped) {
        ESP_LOGW(TAG, "pending queue flushed: %u written, %u lost to overflow earlier",
                 (unsigned)flushed, (unsigned)s_pend_dropped);
    }
    return flushed;
}

/* ---- migration task ---- */
/** @brief Progress callback passed to the migrator. */
static void migrate_progress_cb(uint8_t pct) { s_progress = pct; }

/** @brief Migration task: runs the migrator, then resumes flash init and flushes the queue. */
static void migrate_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "stats migration v%lu → current: START (measurement continues, history offline)",
             (unsigned long)s_from_version);
    esp_err_t e = s_migrator(s_from_version, migrate_progress_cb);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "stats migration FAILED (%s) — storage stays INCOMPATIBLE, data preserved",
                 esp_err_to_name(e));
        s_state = RBSTATS_STORE_INCOMPATIBLE;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* Region is now at the CURRENT format (migrator restamped the marker itself, atomically
     * after the last slot). Resume the FULL init path — operations init alone leaves the
     * unit→buffer bindings empty (they are built by init_buffers_from_network, whose boot-time
     * call ran while the region was still offline): every cursor/read would answer NOT_FOUND and the
     * pending flush would write into the void until the next reboot.
     * The write gate (s_state) opens only AFTER the bindings exist, or concurrent 15-min
     * finalizes race the half-initialized store. */
    ESP_LOGW(TAG, "stats migration DONE — resuming flash operations init");
    s_progress = 100;
    esp_err_t ie = rbpower_flash_operations_init();
    if (ie == ESP_OK) {
        ie = rbpower_flash_init_buffers_from_network();
    }
    if (ie != ESP_OK) {
        ESP_LOGE(TAG, "post-migration flash init failed: %s — store stays gated until reboot",
                 esp_err_to_name(ie));
        s_state = RBSTATS_STORE_READY;   /* marker IS current; next boot initializes normally */
    } else {
        s_state = RBSTATS_STORE_READY;
        rbstats_pending_flush();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/** @brief Enter MIGRATING and start the migration task (no-op if it is already running). */
static void migrate_start_task(void)
{
    if (s_task) return;
    s_state = RBSTATS_STORE_MIGRATING;
    s_progress = 0;
    if (xTaskCreate(migrate_task, "stats_migrate", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "migrate task create failed — storage INCOMPATIBLE (retry next boot)");
        s_state = RBSTATS_STORE_INCOMPATIBLE;
    }
}

esp_err_t rbstats_migrate_begin_deferred(uint32_t stored_version)
{
    s_from_version = stored_version;
    if (!s_migrator || s_migrator_from != stored_version) {
        /* Policy: no migrator => storage offline, DATA PRESERVED. Never wipe. */
        ESP_LOGE(TAG, "format v%lu → current: NO migrator registered — storage INCOMPATIBLE "
                      "(data preserved; a format bump without a migrator is a release-process bug)",
                 (unsigned long)stored_version);
        s_state = RBSTATS_STORE_INCOMPATIBLE;
        return ESP_ERR_NOT_SUPPORTED;
    }
    pending_ensure_alloc();
    /* Never touch the data before the running image is confirmed (a rollback needs it intact). */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    bool image_confirmed = true;   /* factory partition / no otadata → nothing to roll back to */
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        image_confirmed = (ota_state != ESP_OTA_IMG_PENDING_VERIFY);
    }
    if (image_confirmed) {
        migrate_start_task();
    } else {
        ESP_LOGW(TAG, "stats migration ARMED, waiting for OTA image validation "
                      "(rollback would need the v%lu data intact)", (unsigned long)stored_version);
        s_state = RBSTATS_STORE_PENDING_VALID;
        s_progress = 0;
    }
    return ESP_OK;
}

void rbstats_migrate_notify_image_valid(void)
{
    if (s_state == RBSTATS_STORE_PENDING_VALID) {
        ESP_LOGW(TAG, "OTA image confirmed valid — starting deferred stats migration");
        migrate_start_task();
    }
}

/* ================================================================================================
 * Time gate + re-stamp. Shares the pending queue with the migration gate.
 * ================================================================================================ */
#include "rbpower_constants.h"   /* RBGRID_STATS_TIME_SCALE default */
#include <time.h>

/* TRUE while the wall clock has never synced (records would carry uptime timestamps). Accel builds
 * keep uptime timestamps BY DESIGN (virtual clock), so the gate is off there. */
bool rbstats_time_unsynced_gate(void)
{
#if RBGRID_STATS_TIME_SCALE > 1
    return false;
#else
    return time(NULL) <= 1600000000;   /* same validity rule as store_period_to_flash_sync */
#endif
}

/* The SNTP notification runs in the lwIP tcpip thread (3 KB stack): flushing up to the whole queue there
 * would freeze HTTP/MQTT/WS for seconds and risk a stack overflow. Flush from a one-shot
 * task instead; one at a time. */
static volatile bool s_flush_task_running;
/** @brief One-shot task that drains the pending queue. */
static void pending_flush_task(void *arg)
{
    (void)arg;
    (void)rbstats_pending_flush();
    s_flush_task_running = false;
    vTaskDelete(NULL);
}
/** @brief Start pending_flush_task() unless one is already running. */
static void pending_flush_async(void)
{
    if (s_flush_task_running) return;   /* the running one drains whatever arrives meanwhile */
    s_flush_task_running = true;
    if (xTaskCreate(pending_flush_task, "stats_pflush", 6144, NULL, 2, NULL) != pdPASS) {
        s_flush_task_running = false;
        ESP_LOGE(TAG, "pending flush task not created — queue stays in RAM until the next sync");
    }
}

/* First (and every) SNTP sync: re-stamp queued uptime-timestamped records with the now-known
 * uptime→epoch delta, then flush if the store is otherwise ready. Exact: queued ts was
 * period_end uptime seconds, delta = wall_now − uptime_now is constant for one boot. */
void rbstats_pending_on_time_sync(void)
{
    if (!s_pend || s_pend_count == 0) {
        if (rbstats_migrate_state() == RBSTATS_STORE_READY) pending_flush_async();
        return;
    }
    int64_t delta = (int64_t)time(NULL) - (int64_t)(esp_timer_get_time() / 1000000ULL);
    size_t restamped = 0;
    portENTER_CRITICAL(&s_pend_mux);
    for (size_t k = 0; k < s_pend_count; k++) {
        pending_rec_t *r = &s_pend[(s_pend_head + k) % s_pend_capacity];
        if (r->electrical.timestamp != 0 && r->electrical.timestamp < 1600000000u) {
            uint32_t ts = (uint32_t)((int64_t)r->electrical.timestamp + delta);
            r->electrical.timestamp = ts;
            r->energy.timestamp     = ts;
            r->economics.timestamp  = ts;
            restamped++;
        }
    }
    portEXIT_CRITICAL(&s_pend_mux);
    if (restamped) {
        ESP_LOGW(TAG, "time sync: re-stamped %u pending record set(s) (uptime→epoch, delta=%lld s)",
                 (unsigned)restamped, (long long)delta);
    }
    if (rbstats_migrate_state() == RBSTATS_STORE_READY) {
        pending_flush_async();
    }
}
