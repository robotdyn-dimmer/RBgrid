/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_lifetime_api.h
 * @brief Public API of the rbpower daily/lifetime energy and cost accumulators.
 *
 * Covers:
 * - initialization and restore of the accumulators at boot (RTC backup, then NVS snapshot);
 * - accumulation of energy and cost per unit;
 * - daily reset of the accumulators;
 * - reading statistics and per-unit data.
 *
 * Thread safety: the accumulators live inside the global home network structure and have no
 * internal lock. They are updated from the statistics/period context; callers in other tasks
 * should hold the home lock (rbpower_home_lock()) around the accumulate/reset/read functions.
 */

#ifndef RBPOWER_LIFETIME_API_H
#define RBPOWER_LIFETIME_API_H

#include "rbpower_lifetime_accumulators.h"
#include "rbpower_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// PUBLIC API - INITIALIZATION AND CONTROL
//=============================================================================

/**
 * @brief Initialize the lifetime accumulator subsystem.
 *
 * Call once at startup, after rbhome has been initialized. Validates the RTC backup image;
 * if it is invalid (power cycle, layout change after a firmware update) the last NVS snapshot
 * is loaded instead, otherwise the image is cleared and counting starts from zero.
 * Calling it again is a no-op.
 *
 * @return RBPOWER_OK (always; an invalid backup is not an error).
 */
rbpower_err_t rbpower_lifetime_init(void);

/**
 * @brief Restore the accumulators from the backup image into the unit structures.
 *
 * Copies the per-unit records of the validated RTC (or NVS) image into the RAM accumulators.
 * Supply-flagged records whose supply structure does not exist on this boot are restored into
 * the unit's regular accumulator instead of being dropped. Snapshots without exact totalizers
 * seed et_in/et_out from the float totals.
 *
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_STATE the subsystem is not initialized.
 * @retval RBPOWER_ERR_NO_DATA no valid backup image is available.
 */
rbpower_err_t rbpower_lifetime_restore_from_rtc(void);

//=============================================================================
// PUBLIC API - DATA ACCUMULATION
//=============================================================================

/**
 * @brief Accumulate consumption of a regular (non-supply) unit.
 *
 * Adds the period energy and cost to the daily and lifetime counters and the exact import
 * totalizer, then syncs the RTC backup.
 *
 * @param[in] unit_id            Unit ID (must be present in the unit map).
 * @param[in] energy_consumed_wh Energy consumed in the period, Wh.
 * @param[in] cost_consumed      Cost of that energy, tariff currency.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_STATE the subsystem is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG   the unit is unknown or has no valid definition.
 */
rbpower_err_t rbpower_lifetime_accumulate_basic(rbpower_unit_id_t unit_id,
                                              float energy_consumed_wh,
                                              float cost_consumed);

/**
 * @brief Book cost only (no energy) for a unit at the 15-minute tariff costing.
 *
 * Energy of rbAmp-fed units arrives through the module counters (accumulated with cost 0);
 * the money is known only when the 15-minute tariff costing runs, which calls this function.
 * Supply units carry consumed and supplied cost; regular units carry both fields as well.
 * Syncs the RTC backup when anything was booked.
 *
 * @param[in] unit_id       Unit ID.
 * @param[in] cost_consumed Cost of consumed energy to add, tariff currency.
 * @param[in] cost_supplied Revenue for supplied energy to add, tariff currency.
 * @return RBPOWER_OK on success (also when both amounts are 0).
 * @retval RBPOWER_ERR_INVALID_STATE the subsystem is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG   the unit (or its supply structure) is not found.
 */
rbpower_err_t rbpower_lifetime_add_cost(rbpower_unit_id_t unit_id, float cost_consumed, float cost_supplied);
/**
 * @brief Accumulate consumption and supply of a supply unit (main or alternative supply).
 *
 * Adds the period energy and cost to the daily and lifetime counters and both exact
 * totalizers, then syncs the RTC backup.
 *
 * @param[in] unit_id            Supply unit ID (main_supply_unit_id or alternative_supply_unit_id).
 * @param[in] energy_consumed_wh Energy consumed (imported) in the period, Wh.
 * @param[in] energy_supplied_wh Energy supplied (exported) in the period, Wh.
 * @param[in] cost_consumed      Cost of consumption in the period, tariff currency.
 * @param[in] cost_supplied      Revenue for supply in the period, tariff currency.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_STATE the subsystem is not initialized.
 * @retval RBPOWER_ERR_INVALID_ARG   the unit is not a configured supply unit with a supply structure.
 */
rbpower_err_t rbpower_lifetime_accumulate_supply(rbpower_unit_id_t unit_id,
                                                float energy_consumed_wh,
                                                float energy_supplied_wh,
                                                float cost_consumed,
                                                float cost_supplied);

//=============================================================================
// PUBLIC API - DAILY RESET
//=============================================================================

/**
 * @brief Reset the daily accumulators of all units.
 *
 * Clears every daily_* field and keeps the total_* (lifetime) data. The day boundary is a
 * natural snapshot point, so the RTC backup and the NVS snapshot are written afterwards.
 *
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_STATE the subsystem is not initialized.
 */
rbpower_err_t rbpower_lifetime_reset_daily_all(void);
/**
 * @brief Reset the daily_* fields of one unit at its own day boundary.
 *
 * Resetting all units at the first unit's crossing would let the other units add their last
 * quarter of the old day (23:45-24:00) into the new day. No snapshot is written here; the
 * regular NVS persist (at most 60 s later) carries it.
 *
 * @param[in] unit_idx Unit index (not ID) in the home network.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_STATE not initialized, index out of range or unit without a valid definition.
 */
rbpower_err_t rbpower_lifetime_reset_daily_unit(uint8_t unit_idx);

/**
 * @brief Handle a midnight that passed while the device was off.
 *
 * After boot the daily_* fields come from the snapshot of the previous run. If that snapshot
 * belongs to a different local day, the "today" counters must start from zero; otherwise they
 * would carry yesterday's energy for the whole day. Runs once per boot and waits until the
 * wall clock is trusted.
 *
 * @return true if the daily counters were reset, false otherwise (already done, clock not yet
 *         trusted, snapshot from an untrusted clock, or same day).
 */
bool rbpower_lifetime_check_day_rollover(void);

/**
 * @brief Wall-clock time of the snapshot the daily and exact totalizers were restored from at boot.
 * @return Snapshot time, epoch s; 0 when nothing was restored (fresh start).
 */
uint32_t rbpower_lifetime_restored_snapshot_ts(void);

/**
 * @brief Periodic (3 s) tick: flush a pending, throttled NVS snapshot.
 *
 * Cost may be booked right after the energy sync already persisted the snapshot; the NVS write
 * is then deferred and this tick flushes it once at least 10 s have passed since the last write.
 */
void rbpower_lifetime_nvs_tick(void);

//=============================================================================
// PUBLIC API - DATA ACCESS
//=============================================================================

/**
 * @brief Get runtime statistics of the lifetime accumulator subsystem.
 *
 * @param[out] stats Destination for the statistics.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG @p stats is NULL or the subsystem is not initialized.
 */
rbpower_err_t rbpower_lifetime_get_stats(rbpower_lifetime_stats_t* stats);

/**
 * @brief Get all accumulated data (daily and lifetime) of one unit.
 *
 * Detects the unit type automatically. A supply unit without a supply structure is reported
 * as a regular unit, because its data lives in the regular accumulator.
 *
 * @param[in]  unit_id Unit ID.
 * @param[out] data    Destination for the unit data.
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG   @p data is NULL or the subsystem is not initialized.
 * @retval RBPOWER_ERR_NVS_NOT_FOUND the unit is not found.
 */
rbpower_err_t rbpower_lifetime_get_unit_data(rbpower_unit_id_t unit_id,
                                            rbpower_lifetime_unit_data_t* data);

/**
 * @brief Log the state of the RTC backup (diagnostics).
 *
 * Prints magic, unit count, timestamp and CRC32 validity, the stored per-unit records and the
 * subsystem statistics to the console log.
 *
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_STATE the subsystem is not initialized.
 */
rbpower_err_t rbpower_lifetime_debug_rtc_state(void);

//=============================================================================
// PUBLIC API - 3-SECOND PERIOD INTEGRATION
//=============================================================================

/**
 * @brief Accumulate the result of a finished 3-second period into the lifetime counters.
 *
 * Internal integration point, called from the 3-second period finalization. Supply units take
 * their energy and cost from the supply structures; regular units from @p stats_3sec.
 *
 * @param[in] unit_id    Unit ID (the real ID, not the index).
 * @param[in] stats_3sec Finished 3-second period statistics of the unit.
 * @return Result of rbpower_lifetime_accumulate_supply() / rbpower_lifetime_accumulate_basic().
 * @retval RBPOWER_ERR_INVALID_ARG @p stats_3sec is NULL or the unit is unknown.
 */
rbpower_err_t rbpower_lifetime_process_3sec_period(rbpower_unit_id_t unit_id,
                                                  const struct rbpower_period_statistics* stats_3sec);

//=============================================================================
// PUBLIC API - RTC SYNC
//=============================================================================

/**
 * @brief Sync the RAM accumulators to the RTC backup (called on every accumulator update).
 *
 * Wrapper around rbpower_lifetime_sync_all_to_rtc() that counts failures in the statistics.
 *
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT the subsystem is not initialized.
 */
rbpower_err_t rbpower_lifetime_sync_to_rtc(void);

/**
 * @brief Write all units to the RTC backup immediately.
 *
 * Rebuilds the backup image (supply units first, then regular units), updates the header and
 * CRC32, and writes the NVS snapshot at most once per 60 s (a skipped write is marked dirty and
 * flushed by rbpower_lifetime_nvs_tick()). Can be used to force a backup before critical operations.
 *
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT the subsystem is not initialized.
 */
rbpower_err_t rbpower_lifetime_sync_all_to_rtc(void);

//=============================================================================
// PUBLIC API - EXACT TOTALIZERS AND FLASH PERSISTENCE
//=============================================================================

/**
 * @brief Get the exact lifetime totalizers of a unit (source of et_in/et_out in v4 records).
 *
 * For regular units without export, et_out is 0.
 *
 * @param[in]  unit_id    Unit ID.
 * @param[out] et_in_mwh  Lifetime import, mWh (set to 0 on error).
 * @param[out] et_out_mwh Lifetime export, mWh (set to 0 on error).
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_INVALID_ARG an output pointer is NULL or the unit is unknown.
 * @retval RBPOWER_ERR_NOT_INIT    the subsystem is not initialized.
 */
rbpower_err_t rbpower_lifetime_get_totalizers(rbpower_unit_id_t unit_id,
                                              int64_t* et_in_mwh, int64_t* et_out_mwh);

/**
 * @brief Source of the boot-time restore of the totalizers.
 * @return 0 = cold start / seeded from float totals, 1 = RTC backup, 2 = NVS snapshot.
 */
uint8_t rbpower_lifetime_restore_src(void);

/**
 * @brief Write a snapshot of all totalizers to NVS now.
 *
 * Unlike RTC memory, the NVS snapshot survives a power cycle. Called from the sync path
 * (throttled to once per 60 s) and on the daily reset; call it manually before risky operations
 * (OTA, erase). Restore order at boot: the RTC image if valid, otherwise the last NVS snapshot
 * (the most recent record, not the maximum). Stored in NVS namespace "rbltm", key "snap".
 *
 * @return RBPOWER_OK on success.
 * @retval RBPOWER_ERR_NOT_INIT      the subsystem is not initialized.
 * @retval RBPOWER_ERR_INVALID_STATE NVS open, write or commit failed.
 */
rbpower_err_t rbpower_lifetime_persist_to_nvs(void);

//=============================================================================
// PUBLIC API - ASYNCHRONOUS FLASH WRITER CONTROL
//=============================================================================

/**
 * @brief Shut down the asynchronous flash-write system.
 *
 * Stops the asynchronous flash writer task and releases its resources. Call on system
 * shutdown or before a restart.
 */
void rbhome_statistics_cleanup_async_flash_system(void);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_LIFETIME_API_H