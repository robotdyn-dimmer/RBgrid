/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_config_runtime.h
 * @brief Compact RAM tables of units and their statistics ring buffers (runtime configuration manager).
 *
 * The manager keeps, for every active unit, the list of its history ring buffers in the statistics
 * region (period, data type, offset, size, capacity) plus a small per-buffer cache of the ring
 * position. The tables can be saved to and loaded from per-unit JSON files on the config LittleFS
 * (`/configstat_unit_XXXX.json`). When the storage medium is not mounted the manager stays in
 * RAM-only mode.
 *
 * Thread safety: none. A manager instance must be used from one task at a time, or the caller
 * must serialize access.
 */

#ifndef RBPOWER_FLASH_CONFIG_RUNTIME_H
#define RBPOWER_FLASH_CONFIG_RUNTIME_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "rbpower_enums.h"
#include "rbextflash_structures.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ================================================================================================
 * CONSTANTS
 * ================================================================================================ */

// Realistic runtime limits
#define RBPOWER_RUNTIME_MAX_UNITS 8    ///< Maximum units in the manager (rbpower_runtime_add_unit() fails beyond it)
#define RBPOWER_RUNTIME_MAX_BUFFERS 50 ///< Nominal maximum of all buffers in all units (not enforced by rbpower_runtime_add_buffer())

// Paths to JSON files
#define RBPOWER_CONFIG_DIR "/config/rbpower" ///< Legacy config directory name (not used; files live in the LittleFS root)
#define RBPOWER_UNIT_CONFIG_PATTERN "/config/rbpower/unit_%04X.json" ///< Legacy per-unit file pattern (not used; the actual name is "/configstat_unit_%04X.json")

    /* ================================================================================================
     * COMPACT RUNTIME STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Compact buffer configuration (runtime)
     *
     * Only the data needed at run time, plus a RAM cache of the ring position so writes and the
     * HTTP API do not have to scan the sector markers. Size: 40 bytes (packed, checked by a static assert).
     */
    typedef struct
    {
        // Identification (8 bytes)
        rbpower_unit_id_t unit_id; ///< Owning unit ID (2 bytes)
        uint8_t period;            ///< Aggregation period (rbpower_period_t, 1 byte)
        uint8_t data_type;         ///< Data type (rbpower_data_type_t, 1 byte)
        uint16_t total_sectors;    ///< Ring size, flash sectors
        uint16_t record_size;      ///< Record size, bytes

        // Flash placement (8 bytes)
        uint32_t flash_offset;      ///< Buffer start, bytes from the start of the STATS partition
        uint32_t buffer_size_bytes; ///< Buffer size, bytes

        // Capacities (8 bytes)
        uint32_t physical_capacity; ///< Record slots across all sectors of the ring, records
        uint32_t logical_capacity;  ///< Records guaranteed readable (the ring minus the sector being erased), records

        // Buffer state cache in RAM (16 bytes)
        uint16_t cached_active_sector;     ///< Cached index of the active (currently written) sector
        uint16_t cached_next_record_index; ///< Cached index of the next free record slot in the active sector
        uint32_t cached_sequence_number;   ///< Cached sequence_number of the active sector
        uint32_t cached_newest_timestamp;  ///< Timestamp of the newest record, epoch s (served by the HTTP API)
        bool cache_valid;                  ///< The cached_* fields are valid (false until first filled from flash)
        uint8_t reserved_cache[3];         ///< Padding to 40 bytes

    } __attribute__((packed)) rbpower_buffer_runtime_t;

    _Static_assert(sizeof(rbpower_buffer_runtime_t) == 40, "Buffer runtime must be 40 bytes");

    /**
     * @brief Compact unit information (runtime)
     *
     * Minimum data about a unit. Size: 40 bytes (packed, checked by a static assert).
     *
     * NOTE: unit_role and unit_name are used by other modules (JSON serialization, logging,
     * tests), so they cannot be removed without wider refactoring.
     */
    typedef struct
    {
        rbpower_unit_id_t unit_id; ///< Unit ID
        uint16_t unit_role;        ///< Unit role code
        char unit_name[32];        ///< Unit name, NUL-terminated
        bool enabled;              ///< Unit enabled
        uint8_t buffer_count;      ///< Number of buffers of this unit in the manager
        uint16_t reserved;         ///< Padding to 40 bytes

    } __attribute__((packed)) rbpower_unit_runtime_t;

    _Static_assert(sizeof(rbpower_unit_runtime_t) == 40, "Unit runtime must be 40 bytes");

    /**
     * @brief Global runtime configuration manager
     *
     * Stores only active units and buffers, in heap arrays grown with realloc().
     */
    typedef struct
    {
        // STATS partition information
        uint32_t stats_partition_offset; ///< STATS partition offset, bytes
        uint32_t stats_partition_size;   ///< STATS partition size, bytes

        // Units (dynamic array)
        uint8_t unit_count;            ///< Number of active units
        rbpower_unit_runtime_t *units; ///< Heap array of unit_count units

        // Buffers (dynamic array of all buffers)
        uint16_t buffer_count;             ///< Number of active buffers
        rbpower_buffer_runtime_t *buffers; ///< Heap array of buffer_count buffers (all units)

        // Flags
        bool is_initialized; ///< true when the storage medium was available at init; false = RAM-only mode (most calls then fail)

    } rbpower_runtime_config_manager_t;

    /* ================================================================================================
     * API FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Initialize runtime configuration manager
     *
     * Creates an empty manager without allocating memory for units/buffers. If the storage medium
     * is not mounted, the manager is left in RAM-only mode (is_initialized = false) and ESP_OK is returned.
     *
     * @param[out] manager Manager to initialize (zeroed first).
     * @retval ESP_OK              Initialized, or RAM-only mode.
     * @retval ESP_ERR_INVALID_ARG manager is NULL.
     * @retval ESP_FAIL            STATS partition info not available.
     */
    esp_err_t rbpower_runtime_config_init(rbpower_runtime_config_manager_t *manager);

    /**
     * @brief Deinitialize runtime manager
     *
     * Frees all allocated memory and marks the manager uninitialized. NULL is ignored.
     *
     * @param[in,out] manager Manager to deinitialize.
     */
    void rbpower_runtime_config_deinit(rbpower_runtime_config_manager_t *manager);

    /**
     * @brief Load the configuration of all known units from JSON files
     *
     * Tries the known unit IDs (0x4400, 0x4401, 0x4402) and loads every existing
     * `/configstat_unit_XXXX.json` file into the manager.
     *
     * @param[in,out] manager Initialized manager.
     * @retval ESP_OK                At least one unit was loaded.
     * @retval ESP_ERR_INVALID_STATE manager is NULL or not initialized.
     * @retval ESP_ERR_NOT_FOUND     Storage not mounted, or no unit could be loaded.
     */
    esp_err_t rbpower_runtime_config_load_all(rbpower_runtime_config_manager_t *manager);

    /**
     * @brief Load the configuration of one unit from its JSON file and add it to the manager
     *
     * @param[in,out] manager Initialized manager.
     * @param[in]     unit_id Unit ID to load.
     * @retval ESP_OK                Loaded.
     * @retval ESP_ERR_INVALID_STATE manager is NULL or not initialized.
     * @retval ESP_ERR_NOT_FOUND     Storage not mounted, or the file does not exist.
     * @retval ESP_ERR_NO_MEM        Out of memory.
     * @retval ESP_FAIL              JSON parse error.
     * @retval other                 Read error, or an error from rbpower_runtime_add_unit()/rbpower_runtime_add_buffer().
     */
    esp_err_t rbpower_runtime_config_load_unit(rbpower_runtime_config_manager_t *manager,
                                               rbpower_unit_id_t unit_id);

    /**
     * @brief Save a unit's configuration to its JSON file (`/configstat_unit_XXXX.json`)
     *
     * @param[in] manager Initialized manager.
     * @param[in] unit_id Unit ID to save.
     * @retval ESP_OK                Saved, or skipped because the storage is not mounted (RAM-only mode).
     * @retval ESP_ERR_INVALID_STATE manager is NULL or not initialized.
     * @retval ESP_FAIL              Unit not found or serialization failed.
     * @retval ESP_ERR_NO_MEM        JSON string could not be allocated.
     * @retval other                 Error from littlefs_raw_write().
     */
    esp_err_t rbpower_runtime_config_save_unit(const rbpower_runtime_config_manager_t *manager,
                                               rbpower_unit_id_t unit_id);

    /**
     * @brief Find a unit in the manager
     *
     * @param[in] manager Manager.
     * @param[in] unit_id Unit ID.
     * @return Pointer into the manager's unit array (valid until the next add/deinit), or NULL if
     *         not found or the manager is not initialized.
     */
    const rbpower_unit_runtime_t *rbpower_runtime_find_unit(
        const rbpower_runtime_config_manager_t *manager,
        rbpower_unit_id_t unit_id);

    /**
     * @brief Find a buffer in the manager
     *
     * @param[in] manager   Manager.
     * @param[in] unit_id   Unit ID.
     * @param[in] period    Aggregation period.
     * @param[in] data_type Data type.
     * @return Pointer into the manager's buffer array (valid until the next add/deinit), or NULL if
     *         not found or the manager is not initialized.
     */
    const rbpower_buffer_runtime_t *rbpower_runtime_find_buffer(
        const rbpower_runtime_config_manager_t *manager,
        rbpower_unit_id_t unit_id,
        rbpower_period_t period,
        rbpower_data_type_t data_type);

    /**
     * @brief Get all buffers of a unit
     *
     * @param[in]  manager     Manager.
     * @param[in]  unit_id     Unit ID.
     * @param[out] buffers_out Array that receives pointers into the manager's buffer array.
     * @param[in]  max_buffers Capacity of buffers_out, entries.
     * @param[out] count_out   Number of buffers written to buffers_out.
     * @retval ESP_OK              Success (count may be 0).
     * @retval ESP_ERR_INVALID_ARG NULL argument or manager not initialized.
     */
    esp_err_t rbpower_runtime_get_unit_buffers(
        const rbpower_runtime_config_manager_t *manager,
        rbpower_unit_id_t unit_id,
        const rbpower_buffer_runtime_t **buffers_out,
        uint8_t max_buffers,
        uint8_t *count_out);

    /**
     * @brief Add unit to runtime configuration
     *
     * Grows the unit array and adds the unit (enabled, no buffers).
     *
     * @param[in,out] manager   Initialized manager.
     * @param[in]     unit_id   Unit ID.
     * @param[in]     unit_name Unit name, truncated to 31 characters; NULL gives "Unit_XXXX".
     * @param[in]     unit_role Unit role code.
     * @retval ESP_OK                Added.
     * @retval ESP_ERR_INVALID_STATE manager is NULL or not initialized.
     * @retval ESP_ERR_NO_MEM        RBPOWER_RUNTIME_MAX_UNITS reached or allocation failed.
     * @retval ESP_ERR_INVALID_ARG   A unit with this ID already exists.
     */
    esp_err_t rbpower_runtime_add_unit(rbpower_runtime_config_manager_t *manager,
                                       rbpower_unit_id_t unit_id,
                                       const char *unit_name,
                                       uint16_t unit_role);

    /**
     * @brief Add buffer to runtime configuration
     *
     * Grows the buffer array, copies the buffer (its cache fields are reset to invalid) and
     * increments the owning unit's buffer count.
     *
     * @param[in,out] manager Initialized manager.
     * @param[in]     buffer  Buffer description to copy.
     * @retval ESP_OK              Added.
     * @retval ESP_ERR_INVALID_ARG NULL argument or manager not initialized.
     * @retval ESP_ERR_NO_MEM      Allocation failed.
     */
    esp_err_t rbpower_runtime_add_buffer(rbpower_runtime_config_manager_t *manager,
                                         const rbpower_buffer_runtime_t *buffer);

    /**
     * @brief Print the runtime configuration to the log
     *
     * @param[in] manager Manager.
     */
    void rbpower_runtime_config_dump(const rbpower_runtime_config_manager_t *manager);

    /**
     * @brief Get the manager's memory usage
     *
     * @param[in] manager Manager.
     * @return Size of the manager structure plus its unit and buffer arrays, bytes (0 if not initialized).
     */
    size_t rbpower_runtime_config_get_memory_usage(const rbpower_runtime_config_manager_t *manager);

    /**
     * @brief Self-test of the runtime manager (diagnostic; writes and reads a test unit's JSON file)
     *
     * @return ESP_OK if all steps passed, otherwise the first failing step's error (ESP_FAIL on a data mismatch).
     */
    esp_err_t rbpower_runtime_config_test(void);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_FLASH_CONFIG_RUNTIME_H
