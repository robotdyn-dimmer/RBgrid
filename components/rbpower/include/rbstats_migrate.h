/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstats_migrate.h
 * @brief Deferred statistics-format migration engine and pending-record queue.
 *
 * Replaces the blind wipe on a BREAKING on-flash format change with a deferred,
 * power-loss-tolerant migration that never blocks measurement:
 *
 *   boot: marker=vOLD, current=vNEW (breaking)
 *     -> storage enters MIGRATING/PENDING_VALID (no wipe, no slot init, history -> HTTP 503)
 *     -> finalized period results are queued in PSRAM (rbstats_pending_*) instead of being lost
 *     -> the migration task starts ONLY once the running OTA image is confirmed valid
 *        (esp_ota_mark_app_valid_cancel_rollback); otherwise a rollback to the old firmware
 *        would meet half-migrated data
 *     -> per-slot progress is persisted (idempotent re-entry after a power loss; worst case is
 *        ONE slot lost if power dies between its erase and rewrite, which is accepted because
 *        the module's own energy counters cover the gap)
 *     -> on success: the marker is restamped, normal init resumes and the pending queue is flushed.
 *
 * The actual vOLD->vNEW record conversion is registered by the code that introduces the format
 * change (rbstats_migrate_v3v4.c registers v3->v4); this engine owns gating, state, queue and
 * lifecycle only. Policy: a format change WITHOUT a registered migrator leaves the storage
 * INCOMPATIBLE (offline, data preserved); it never wipes.
 *
 * The pending queue is protected by a spinlock and may be used from any task.
 */
#ifndef RBSTATS_MIGRATE_H
#define RBSTATS_MIGRATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rbstorage_transfer_types.h"   /* transfer structs + rbpower_period_type_t */
#include "rbhome_types.h"               /* rbpower_unit_id_t */
#include "rbstats_v4_types.h"           /* rbstats_v4_meta_t (queued alongside each record set) */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief State of the statistics store with respect to format migration. */
typedef enum {
    RBSTATS_STORE_READY = 0,       ///< Normal operation
    RBSTATS_STORE_PENDING_VALID,   ///< Breaking format change seen; waiting for OTA image validation
    RBSTATS_STORE_MIGRATING,       ///< Migration task running
    RBSTATS_STORE_INCOMPATIBLE,    ///< Breaking change but no migrator registered, or it failed: storage offline, DATA PRESERVED (never wiped)
} rbstats_store_state_t;

/**
 * @brief Migrator callback, registered by the code that introduces a format change.
 *
 * Must be idempotent per slot (it reads/updates the persisted progress itself) and report
 * progress 0..100 through @p progress.
 *
 * @param[in] from_version Stored (old) format version.
 * @param[in] progress     Progress callback, percent 0..100.
 * @return ESP_OK when the whole region is at the CURRENT format, an error code otherwise.
 */
typedef esp_err_t (*rbstats_migrator_fn)(uint32_t from_version, void (*progress)(uint8_t pct));

/**
 * @brief Current migration state of the statistics store.
 * @return One of rbstats_store_state_t.
 */
rbstats_store_state_t rbstats_migrate_state(void);
/**
 * @brief Current store state as a string.
 * @return "ready", "pending_image_valid", "migrating" or "incompatible" (static string).
 */
const char*           rbstats_store_state_str(void);
/**
 * @brief Migration progress.
 * @return Percent 0..100; 100 when READY.
 */
uint8_t               rbstats_migrate_progress(void);   /* 0..100; 100 when READY */

/**
 * @brief Register the migrator that lifts @p from_version data to the current format.
 *
 * Call BEFORE rbpower_flash_operations_init() (e.g. from the storage init path).
 * Only one migrator is held; a later call replaces it.
 *
 * @param[in] from_version Source format version handled by @p fn.
 * @param[in] fn           Migrator callback.
 */
void rbstats_migrate_register(uint32_t from_version, rbstats_migrator_fn fn);

/**
 * @brief Start a deferred migration after a BREAKING stored format version was detected.
 *
 * Called by flash_check_and_migrate_format(). Allocates the pending queue, resolves the OTA image
 * state and either starts the migration task (image already valid, or factory partition) or arms
 * the image-valid trigger (rbstats_migrate_notify_image_valid()).
 *
 * @param[in] stored_version Format version found on flash.
 * @return ESP_OK when the migration was started or armed.
 * @retval ESP_ERR_NOT_SUPPORTED no migrator is registered for @p stored_version; the store is
 *         set to RBSTATS_STORE_INCOMPATIBLE (data preserved).
 */
esp_err_t rbstats_migrate_begin_deferred(uint32_t stored_version);

/**
 * @brief Start a pending deferred migration once the OTA image is confirmed valid.
 *
 * Hook for the place that marks the OTA image valid. No-op in every state other than
 * RBSTATS_STORE_PENDING_VALID. Safe to call repeatedly and from any task.
 */
void rbstats_migrate_notify_image_valid(void);

/* ---- Time gate + re-stamp ---- */
/**
 * @brief Whether period records must be queued because the wall clock has never synced.
 *
 * While true, records must be queued rather than persisted with uptime timestamps. Always false
 * in accelerated-time builds (RBGRID_STATS_TIME_SCALE > 1), which use a virtual clock by design.
 *
 * @return true while time(NULL) <= 1600000000 (clock not yet valid).
 */
bool rbstats_time_unsynced_gate(void);
/**
 * @brief SNTP sync hook: re-stamp queued records and flush them.
 *
 * Re-stamps queued records that carry uptime timestamps with this boot's uptime->epoch delta and,
 * when the store is READY, flushes the queue from a one-shot background task (the SNTP callback
 * runs in the small-stack lwIP thread). Cheap when the queue is empty.
 */
void rbstats_pending_on_time_sync(void);

/* ---- Pending finalize-result queue (shared by the migration gate and the time gate) ---- */
/**
 * @brief Queue one finalized period record set until the store can accept it.
 *
 * When the ring is full the OLDEST entry is dropped (newest wins). The queue lives in PSRAM
 * (2048 entries), or in internal RAM (256 entries) when PSRAM is unavailable.
 *
 * @param[in] unit_id    Unit ID.
 * @param[in] period     Period type.
 * @param[in] electrical Electrical transfer record.
 * @param[in] energy     Energy transfer record.
 * @param[in] economics  Economics transfer record.
 * @param[in] meta       v4 record metadata, or NULL for defaults.
 * @return ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG a record pointer is NULL.
 * @retval ESP_ERR_NO_MEM      the queue could not be allocated.
 */
esp_err_t rbstats_pending_enqueue(rbpower_unit_id_t unit_id, rbpower_period_type_t period,
                                  const rbpower_electrical_transfer_t* electrical,
                                  const rbpower_energy_transfer_t* energy,
                                  const rbpower_economics_transfer_t* economics,
                                  const rbstats_v4_meta_t* meta /* NULL = defaults */);
/**
 * @brief Number of record sets waiting in the pending queue.
 * @return Queued record sets.
 */
size_t    rbstats_pending_count(void);
/**
 * @brief Drain the queue into the (now ready) flash writer.
 *
 * Blocking: writes one record set at a time and yields between writes. Record sets whose write
 * fails are dropped and logged.
 *
 * @return Number of record sets written.
 */
size_t    rbstats_pending_flush(void);

#ifdef __cplusplus
}
#endif
#endif /* RBSTATS_MIGRATE_H */
