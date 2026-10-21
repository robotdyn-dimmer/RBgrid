/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_operations.h
 * @brief Flash operations for writing/reading statistical data
 *
 * Module provides API for writing and reading statistics records to Flash.
 * Supports ring buffers with automatic sector management.
 *
 * The stats medium is either the internal `stats` flash partition or an optional external
 * W25Q NOR chip (one at a time, selected by rbstorage_media). Each unit owns a fixed slot;
 * each (period, data type) pair inside it is a ring of 4 KB sectors whose ACTIVE/CLOSED
 * markers let the write cursor be recovered by scanning after a reboot.
 *
 * Locking: all flash I/O is serialized by one internal mutex with writer priority
 * (readers yield while any writer is waiting). Functions must be called from task context.
 */

#ifndef RBPOWER_FLASH_OPERATIONS_H
#define RBPOWER_FLASH_OPERATIONS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rbextflash_structures.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ================================================================================================
     * PUBLIC API FOR STATISTICS MODULE
     * ================================================================================================ */

    /**
     * @brief Write statistics record to Flash
     *
     * Universal function for writing any statistical data.
     * Automatically determines buffer by unit_id, period and data_type.
     * Manages ring buffer and metadata.
     *
     * @param[in] unit_id Device ID (e.g., 0x4401)
     * @param[in] period Aggregation period (RBPOWER_PERIOD_HOUR, RBPOWER_PERIOD_DAY, etc.)
     * @param[in] data_type Data type (RBPOWER_DATA_TYPE_ELECTRICAL, RBPOWER_DATA_TYPE_ENERGY, etc.)
     * @param[in] data Pointer to the record; must start with a uint32_t timestamp (epoch s)
     * @param[in] data_size Data size in bytes (must match buffer record_size)
     * @retval ESP_OK                Written, or flash disabled by configuration (RAM-only, silent).
     * @retval ESP_ERR_INVALID_STATE Flash enabled but unavailable (write dropped), or no handle.
     * @retval ESP_ERR_INVALID_ARG   @p data is NULL.
     * @retval ESP_ERR_TIMEOUT       Write lock not obtained within 1 s.
     * @retval ESP_ERR_NOT_FOUND     No buffer for (unit, period, type).
     * @retval ESP_ERR_INVALID_SIZE  @p data_size != record size.
     * @return Other codes from the medium erase/write calls.
     *
     * @note Function is thread-safe (takes the flash write lock, writer priority)
     * @note On sector overflow the next sector is erased and marked ACTIVE before the full one is
     *       CLOSED (the oldest sector is overwritten — ring semantics)
     * @note Ring state lives in the on-flash sector markers, not in NVS
     */
    esp_err_t rbpower_flash_write_record(rbpower_unit_id_t unit_id,
                                         rbpower_period_t period,
                                         rbpower_data_type_t data_type,
                                         const void *data,
                                         size_t data_size);

    /**
     * @brief Read one record from Flash
     *
     * @param[in] unit_id Device ID
     * @param[in] period Aggregation period
     * @param[in] data_type Data type
     * @param[in] record_index Record index (0 = NEWEST, N-1 = oldest — counts back from the write cursor;
     *        the history API offset/limit window and statmeta rely on this)
     * @param[out] data_out Buffer for reading data
     * @param[in] data_size Buffer size in bytes (must equal the record size)
     * @retval ESP_OK                Record read.
     * @retval ESP_ERR_NOT_FOUND     Flash unavailable, no such buffer, or buffer empty.
     * @retval ESP_ERR_INVALID_ARG   @p data_out NULL or @p record_index >= logical capacity.
     * @retval ESP_ERR_TIMEOUT       Read lock not obtained within 1 s.
     * @retval ESP_ERR_INVALID_SIZE  @p data_size != record size.
     * @retval ESP_ERR_INVALID_STATE Buffer cursor cache not initialized, or no medium handle.
     * @return Other codes from the medium read call.
     * @note Takes the flash read lock (defers to waiting writers).
     */
    esp_err_t rbpower_flash_read_record(rbpower_unit_id_t unit_id,
                                        rbpower_period_t period,
                                        rbpower_data_type_t data_type,
                                        uint32_t record_index,
                                        void *data_out,
                                        size_t data_size);

    /**
     * @brief Read range of records from Flash
     *
     * @param[in] unit_id Device ID
     * @param[in] period Aggregation period
     * @param[in] data_type Data type
     * @param[in] start_index Start index (0 = newest record, same as rbpower_flash_read_record())
     * @param[in] count Number of records to read
     * @param[out] data_out Array for reading data (must be >= count * record_size bytes)
     * @param[out] records_read_out Number of actually read records (can be NULL)
     * @retval ESP_OK              At least one record read (or @p count == 0); stops at the first failure.
     * @retval ESP_ERR_NOT_FOUND   Flash unavailable, no such buffer, or no record readable.
     * @retval ESP_ERR_INVALID_ARG @p data_out is NULL.
     * @note Takes the read lock per record and feeds the task watchdog every 10 records.
     */
    esp_err_t rbpower_flash_read_records(rbpower_unit_id_t unit_id,
                                         rbpower_period_t period,
                                         rbpower_data_type_t data_type,
                                         uint32_t start_index,
                                         uint32_t count,
                                         void *data_out,
                                         uint32_t *records_read_out);

    /* ---- v3→v4 migrator support ---- */
    /**
     * @brief Get the unit_id stored in each slot; loads the slot map if it is not loaded yet.
     * @param[out] unit_by_slot_out Array of at least @p max_slots entries; receives the unit_id per
     *                              slot (0xFFFF = free slot).
     * @param[in]  max_slots        Capacity of @p unit_by_slot_out (entries).
     * @param[out] used_out         Highest occupied slot index + 1 (may be NULL).
     * @retval ESP_OK               Map copied.
     * @retval ESP_ERR_INVALID_ARG  @p unit_by_slot_out is NULL or @p max_slots <= 0.
     * @note Called by the region migrator during init, before concurrent flash clients exist.
     */
    esp_err_t rbpower_flash_slot_map_get(uint16_t *unit_by_slot_out, int max_slots, int *used_out);
    /**
     * @brief Absolute stats-medium offset of a slot under the LIVE layout (v4 builds: the v4 stride).
     * @param[in] slot Slot index (0 .. RBPOWER_MAX_UNITS-1; not range-checked).
     * @return Offset in bytes (PARTITION_STATS_OFFSET + slot * slot size).
     */
    uint32_t rbpower_flash_slot_base_offset(int slot);
    /**
     * @brief Number of slots that fit the detected medium's STATS region under the LIVE layout.
     * @return Usable slot count (0 .. RBPOWER_MAX_UNITS); RBPOWER_MAX_UNITS before slot init.
     */
    int rbpower_flash_max_usable_slots(void);
    /**
     * @brief Restamp the format marker to the current version (the migrator's atomic finale:
     *        only a fully migrated region may carry the new marker).
     * @return ESP_OK, or the littlefs_raw_write() error code.
     */
    esp_err_t rbpower_flash_format_marker_restamp(void);
    /**
     * @brief Remove the stats format marker. Pairs with rbpower_flash_erase_all_slots(): a wiped
     *        region is fresh, so a stale marker must not survive it.
     * @return ESP_OK, or the littlefs_raw_remove() error code.
     */
    esp_err_t rbpower_flash_format_marker_clear(void);   /* pairs with erase_all_slots (flasherase) */

    /**
     * @brief Read records as v4 structs regardless of the on-flash format.
     *
     * `out_v4` is an array of rbstats_electrical_v4_t / rbstats_energy_v4_t / rbstats_econ_v4_t
     * matching `data_type`. While the on-flash format is v3 the records are lifted through the
     * pure v3→v4 converters (dur = nominal, q |= MIGRATED — aggregating callers must treat
     * MIGRATED as "pre-v4 record", not propagate it); on a v4 region this reads raw v4.
     * Index semantics match rbpower_flash_read_records.
     *
     * @param[in]  unit_id          Unit ID.
     * @param[in]  period           Aggregation period.
     * @param[in]  data_type        Data type (ELECTRICAL / ENERGY / ECONOMICS).
     * @param[in]  start_index      First record index (0 = newest).
     * @param[in]  count            Number of records requested.
     * @param[out] out_v4           Caller array of at least @p count v4 records of the matching type.
     * @param[out] records_read_out Number of records actually read (may be NULL).
     * @retval ESP_OK                At least one record read (or a partial range).
     * @retval ESP_ERR_INVALID_ARG   NULL output or bad arguments.
     * @retval ESP_ERR_NOT_SUPPORTED Data type has no v4 representation.
     * @retval ESP_ERR_NOT_FOUND     Nothing readable (flash offline / buffer missing / empty).
     * @note Takes the flash read lock per record; call from task context.
     */
    esp_err_t rbstats_flash_read_v4(rbpower_unit_id_t unit_id,
                                    rbpower_period_t period,
                                    rbpower_data_type_t data_type,
                                    uint32_t start_index,
                                    uint32_t count,
                                    void *out_v4,
                                    uint32_t *records_read_out);

    /**
     * @brief TEST function for checking reading of last N records
     *
     * Temporary function for debugging read algorithm.
     * Outputs last count records to log (ELECTRICAL and ENERGY types only).
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Buffer period
     * @param[in] data_type Data type
     * @param[in] count Number of records to read
     * @return Always ESP_OK (per-record read failures are logged)
     */
    esp_err_t rbpower_flash_test_read_recent(rbpower_unit_id_t unit_id,
                                             rbpower_period_t period,
                                             rbpower_data_type_t data_type,
                                             uint32_t count);

    /**
     * @brief TEST function for checking batch record reading
     *
     * Temporary function for debugging rbpower_flash_read_records().
     * Outputs range of records [start_index, start_index + count - 1] to log.
     * Demonstrates working with caller-allocated buffer.
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Buffer period
     * @param[in] data_type Data type
     * @param[in] start_index Start index (from newest, 0 = newest record)
     * @param[in] count Number of records to read
     * @retval ESP_OK          Success (or unsupported data type: nothing printed)
     * @retval ESP_ERR_NO_MEM  Buffer allocation failed
     * @return Other codes from rbpower_flash_read_records()
     */
    esp_err_t rbpower_flash_test_read_batch(rbpower_unit_id_t unit_id,
                                            rbpower_period_t period,
                                            rbpower_data_type_t data_type,
                                            uint32_t start_index,
                                            uint32_t count);

    /**
     * @brief Check external Flash availability
     *
     * Checks global external Flash status (initialized and filesystem_mounted).
     * Returns false if Flash is unavailable, in which case all read/write operations
     * will be skipped without errors (system operates in RAM-only mode).
     *
     * @return true if Flash is available for read/write operations
     */
    bool rbpower_flash_is_available(void);

    /**
     * @brief Runtime gating for stats-flash writes: the "use the stats flash" switch.
     *
     * rbpower_flash_is_available() returns true only when: enabled AND initialized AND
     * filesystem mounted AND (OTP authentic OR enforcement off).
     *  - set_enabled(false): app-level "don't use the stats flash" → stats stay RAM-only.
     *
     * @param[in] enabled true (default) = persist stats when the medium is available.
     * @note Plain flag write; takes effect on the next write/read call.
     */
    void rbpower_flash_set_enabled(bool enabled);
    /**
     * @brief Enable/disable OTP authenticity enforcement for the external NOR chip.
     *
     * When enforced, a chip that fails OTP authenticity (foreign / not prepared) is treated as
     * unavailable for stats. Default is OFF: history continuity on a generic, un-provisioned
     * chip outweighs the anti-foreign-chip gate; enforcement is an opt-in for provisioned SKUs.
     *
     * @param[in] enforce true = reject chips that fail OTP validation.
     */
    void rbpower_flash_set_enforce_otp(bool enforce);
    /**
     * @brief Current value of the stats-flash enable switch.
     * @return true if stats persistence is enabled (default true).
     */
    bool rbpower_flash_get_enabled(void);
    /**
     * @brief Current value of the OTP enforcement switch.
     * @return true if OTP authenticity is enforced (default false).
     */
    bool rbpower_flash_get_enforce_otp(void);

    /**
     * @brief Read the ring-buffer write cursor for a (unit, period, type) buffer.
     * Returns the active sector, the next-record index within it, the total sector count, and
     * the newest record timestamp. Uses the RAM cache when valid, else reconstructs by scanning
     * the flash (the same scan boot recovery uses) under the read lock. For diagnostics /
     * integrity inspection.
     * @param[in]  unit_id           Unit ID.
     * @param[in]  period            Aggregation period.
     * @param[in]  data_type         Data type.
     * @param[out] sector_out        Active sector index (0 for an empty ring; may be NULL).
     * @param[out] next_index_out    Next record slot within the active sector (may be NULL).
     * @param[out] total_sectors_out Sectors in this ring (may be NULL).
     * @param[out] newest_ts_out     Newest record timestamp, epoch s (cached; may be NULL).
     * @retval ESP_OK            Cursor returned.
     * @retval ESP_ERR_NOT_FOUND No such buffer.
     * @retval ESP_ERR_TIMEOUT   Read lock not obtained within 1 s.
     */
    esp_err_t rbpower_flash_get_cursor(rbpower_unit_id_t unit_id,
                                       rbpower_period_t period,
                                       rbpower_data_type_t data_type,
                                       uint16_t *sector_out,
                                       uint16_t *next_index_out,
                                       uint16_t *total_sectors_out,
                                       uint32_t *newest_ts_out);

    /**
     * @brief Number of flash WRITERS currently waiting/holding the flash lock. Diagnostic for the
     * write/read time-separation + write-priority model (a non-zero value means reads are deferring).
     * @return Count of writers waiting for or holding the flash lock (unsynchronized snapshot).
     */
    int rbpower_flash_writers_waiting(void);

    /**
     * @brief Erase the whole STATS region (DROPS ALL HISTORY), then reboot to re-create buffers
     * at the current record format. Maintenance/format-migration tool (e.g. the min/max record
     * expansion). Erases blindly by address in 64 KB chunks (no record reads), taking the write
     * lock per chunk when the lock exists; OTP/config/log regions are untouched.
     * @retval ESP_OK                Region erased; reboot required.
     * @retval ESP_ERR_INVALID_STATE No stats medium handle.
     * @retval ESP_ERR_INVALID_SIZE  Region size is 0.
     * @retval ESP_ERR_TIMEOUT       Write lock not obtained within 5 s for a chunk.
     * @return Other codes from the medium erase call.
     */
    esp_err_t rbpower_flash_erase_all_slots(void);

    /**
     * @brief Reclaim ONE deleted unit's flash STATS slot: ERASE its region (so a reused slot never
     * inherits the removed unit's records) then mark the slot free + persist. Unit-deletion path.
     * @param[in] unit_id ID of the removed unit.
     * @retval ESP_OK            Slot reclaimed, or no-op (flash unavailable → RAM-only).
     * @retval ESP_ERR_NOT_FOUND Unit has no slot.
     * @retval ESP_ERR_TIMEOUT   Write lock not obtained within 2 s.
     * @return Other codes from the medium erase call.
     * @note Takes the flash write lock; call from task context.
     */
    esp_err_t rbpower_flash_remove_unit(rbpower_unit_id_t unit_id);

    /**
     * @brief A unit re-created at runtime with the id of a unit removed in this boot: re-occupy the
     * slot its (still bound) runtime buffers write into + persist. No-op if not applicable.
     * @param[in] unit_id ID of the re-created unit.
     * @retval ESP_OK                Slot re-occupied, or no-op (flash unavailable, already slotted,
     *                               or no runtime buffers for this id).
     * @retval ESP_ERR_INVALID_STATE The slot at the buffers' base is taken by another unit.
     * @retval ESP_ERR_NOT_FOUND     No usable slot starts at the buffers' base offset.
     */
    esp_err_t rbpower_flash_reattach_unit(rbpower_unit_id_t unit_id);

    /**
     * @brief Number of records ACTUALLY written in a ring (not capacity) — for the history API so
     * it doesn't serialize the unwritten 0xFF tail or report capacity as total_available.
     * @param[in]  unit_id   Unit ID.
     * @param[in]  period    Aggregation period.
     * @param[in]  data_type Data type.
     * @param[out] count_out Records written, capped at the logical capacity (0 for an empty ring).
     * @retval ESP_OK              Count returned.
     * @retval ESP_ERR_INVALID_ARG @p count_out is NULL.
     * @retval ESP_ERR_NOT_FOUND   No such buffer.
     * @retval ESP_ERR_TIMEOUT     Read lock not obtained within 1 s.
     */
    esp_err_t rbpower_flash_get_record_count(rbpower_unit_id_t unit_id, rbpower_period_t period,
                                             rbpower_data_type_t data_type, uint32_t* count_out);

    /**
     * @brief Initialize Flash operations module
     *
     * Should be called once at application startup. Checks/migrates the on-flash record format,
     * creates the flash lock, loads the slot map (`/flash_slots.json`) and the runtime buffer
     * configuration. If the format check hands the region to the deferred migration engine,
     * init returns ESP_OK but the module stays uninitialized until migration completes.
     *
     * @retval ESP_OK          Initialized, RAM-only (flash unavailable) or deferred to migration.
     * @retval ESP_ERR_NO_MEM  Mutex allocation failed.
     * @return Other codes from slot or runtime-configuration initialization.
     */
    esp_err_t rbpower_flash_operations_init(void);

    /**
     * @brief Read the record-format compat state (current/min-compatible + stored marker).
     *        Diagnostics for the `fmtinfo` console command. Read-only.
     * @param[out] current        Compiled-in RBPOWER_RECORD_FORMAT_VERSION (may be NULL).
     * @param[out] min_compatible Compiled-in RBPOWER_RECORD_FORMAT_MIN_COMPATIBLE (may be NULL).
     * @param[out] stored         Version stored in the marker file, 0 if absent (may be NULL).
     * @param[out] marker_present true if the marker file exists and was read (may be NULL).
     */
    void rbpower_flash_get_format_info(uint32_t *current, uint32_t *min_compatible,
                                       uint32_t *stored, bool *marker_present);

    /**
     * @brief Deinitialize Flash operations module (deletes the flash lock and the runtime
     *        buffer table). No-op if not initialized.
     */
    void rbpower_flash_operations_deinit(void);

    /* ================================================================================================
     * INITIALIZATION AND MANAGEMENT
     * ================================================================================================ */

    /**
     * @brief Initialize Flash buffers for all active units
     *
     * Automatically initializes buffers based on home network configuration (g_home_network).
     * Uses RBPOWER_BUFFER_SECTORS matrix to determine required period×type combinations.
     *
     * Function:
     * 1. Checks external Flash availability
     * 2. Gets list of active units from g_home_network
     * 3. Filters units (excludes VOLTAGE_BUS, checks statistics_enabled)
     * 4. Initializes buffers according to RBPOWER_BUFFER_SECTORS matrix
     * 5. Runs diagnostics and repair if needed
     *
     * @retval ESP_OK                All buffers initialized, or flash unavailable (RAM-only mode).
     * @retval ESP_ERR_INVALID_STATE The home network has no units.
     * @retval ESP_FAIL              Some buffers failed to initialize.
     *
     * @note Call after rbpower_statistics_timers_start() in rbpower_init_extended()
     * @note Requires g_home_network and external Flash initialization
     */
    esp_err_t rbpower_flash_init_buffers_from_network(void);

    /* ================================================================================================
     * DIAGNOSTICS AND MAINTENANCE
     * ================================================================================================ */

    /**
     * @brief Diagnose buffer state
     *
     * Checks integrity of all sector metadata, detects anomalies.
     *
     * @param[in] unit_id Device ID
     * @param[in] period Aggregation period
     * @param[in] data_type Data type
     * @retval ESP_OK                Buffer is healthy.
     * @retval ESP_ERR_INVALID_STATE No/multiple ACTIVE sectors or corrupted sectors found.
     * @retval ESP_ERR_NOT_SUPPORTED Flash unavailable (RAM-only mode).
     * @retval ESP_ERR_NOT_FOUND     No such buffer.
     * @note Reads flash without taking the lock; intended for the init sequence.
     */
    esp_err_t rbpower_flash_diagnose_buffer(rbpower_unit_id_t unit_id,
                                            rbpower_period_t period,
                                            rbpower_data_type_t data_type);

    /**
     * @brief Repair buffer after detecting problems
     *
     * Fixes detected problems: closes every ACTIVE sector except the one with the highest
     * sequence number, then invalidates the cursor cache so the next access rescans.
     *
     * @param[in] unit_id Device ID
     * @param[in] period Aggregation period
     * @param[in] data_type Data type
     * @retval ESP_OK                Repair done (or nothing to repair).
     * @retval ESP_ERR_NOT_SUPPORTED Flash unavailable (RAM-only mode).
     * @retval ESP_ERR_NOT_FOUND     No such buffer.
     * @return Other codes from closing a sector.
     * @note Writes flash without taking the lock; intended for the init sequence.
     */
    esp_err_t rbpower_flash_repair_buffer(rbpower_unit_id_t unit_id,
                                          rbpower_period_t period,
                                          rbpower_data_type_t data_type);

    /* ================================================================================================
     * TEST STRUCTURES (for debugging)
     * ================================================================================================ */

    /**
     * @brief Test record for debugging Flash operations
     *
     * Used for quick write/read testing.
     * Size: 26 bytes packed (sum of the fields below).
     */
    typedef struct
    {
        uint32_t timestamp;                          ///< Unix timestamp, epoch s (4 bytes)
        uint16_t test_counter;                       ///< Counter for checking sequence (2 bytes)
        uint16_t voltage;                            ///< Test voltage (in 0.1V, e.g., 2200 = 220.0V) (2 bytes)
        uint16_t current;                            ///< Test current (in 0.01A, e.g., 100 = 1.00A) (2 bytes)
        uint32_t energy;                             ///< Accumulated energy (in Wh) (4 bytes)
        uint16_t power;                              ///< Power (in W) (2 bytes)
        uint16_t frequency;                          ///< Frequency (in 0.01Hz, e.g., 5000 = 50.00Hz) (2 bytes)
        uint16_t power_factor;                       ///< Power factor (in 0.001, e.g., 950 = 0.950) (2 bytes)
        uint32_t reserved;                           ///< Reserved for alignment (4 bytes)
        uint16_t crc16;                              ///< CRC16-CCITT over all preceding fields (2 bytes)
    } __attribute__((packed)) rbpower_test_record_t; // Size will be checked at runtime

    // Note: Size check is performed at runtime, as C++ compiler
    // may ignore packed attribute in some cases

    /**
     * @brief Calculate CRC16 for test record
     *
     * @param[in] record Pointer to record
     * @return CRC16-CCITT (poly 0x1021, init 0xFFFF) over all fields except crc16; 0 if @p record is NULL
     */
    uint16_t rbpower_test_record_calc_crc(const rbpower_test_record_t *record);

    /**
     * @brief Verify CRC16 of test record
     *
     * @param[in] record Pointer to record
     * @return true if CRC is correct; false on mismatch or NULL @p record
     */
    bool rbpower_test_record_verify_crc(const rbpower_test_record_t *record);

    /**
     * @brief Get buffer capacity (number of records)
     *
     * @param[in] unit_id Unit ID
     * @param[in] period Buffer period
     * @param[in] data_type Data type
     * @param[out] capacity Pointer to save capacity (records; set to 0 on failure)
     * @retval ESP_OK                Capacity returned.
     * @retval ESP_ERR_INVALID_ARG   @p capacity is NULL.
     * @retval ESP_ERR_NOT_SUPPORTED Flash unavailable (RAM-only mode).
     * @retval ESP_ERR_NOT_FOUND     Buffer not found.
     */
    esp_err_t rbpower_flash_get_buffer_capacity(rbpower_unit_id_t unit_id,
                                                rbpower_period_t period,
                                                rbpower_data_type_t data_type,
                                                uint32_t *capacity);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_FLASH_OPERATIONS_H
