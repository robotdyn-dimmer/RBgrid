/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_init.h
 * @brief API for creating and initializing statistics file structure
 *
 * Functions for creating directories, data files and metadata for units on the
 * VFS-mounted `storage` LittleFS partition (see rbstorage.h).
 *
 * The functions use plain stdio/VFS calls and hold no lock of their own; call them
 * from one task at a time (normally during storage initialization).
 *
 * @note The unit role/type used by rbpower_storage_initialize_filesystem() and
 *       rbpower_storage_create_global_index() is currently a placeholder
 *       (DEVICE_LOAD / POWER_LOAD), and rbpower_storage_get_space_info() returns
 *       fixed estimates.
 */

#ifndef RBPOWER_STORAGE_INIT_H
#define RBPOWER_STORAGE_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rbhome_types.h"                    // rbpower_unit_id_t, rbpower_load_role_t, rbpower_metric_type_t
#include "rbstorage_structures.h"      // rbpower_unit_creation_result_t, rbpower_global_index_t

/* ================================================================================================
 * CONSTANTS AND SETTINGS
 * ================================================================================================ */

/** @brief Base VFS path of the statistics tree on the `storage` partition. */
#define RBPOWER_STORAGE_BASE_PATH        "/storage"
/** @brief Directory holding one subdirectory per unit. */
#define RBPOWER_STORAGE_UNITS_PATH       "/storage/units"
/** @brief Archive directory. */
#define RBPOWER_STORAGE_ARCHIVE_PATH     "/storage/.archive"

/** @brief Version service file (text: storage version, creation time, format version). */
#define RBPOWER_STORAGE_VERSION_FILE     "/storage/.version"
/** @brief Global index service file (binary rbpower_global_index_t). */
#define RBPOWER_STORAGE_INDEX_FILE       "/storage/.index"

/** @brief Maximum number of units processed in one batch. */
#define RBPOWER_MAX_BATCH_UNITS          16

/* ================================================================================================
 * RESULT STRUCTURES
 * ================================================================================================ */

/**
 * @brief Filesystem initialization result
 */
typedef struct {
    uint8_t total_units;                    ///< Total number of units passed in
    uint8_t processed_units;                ///< Units processed successfully
    uint8_t failed_units;                   ///< Units that failed
    uint8_t excluded_units;                 ///< Units excluded from storage (VOLTAGE_BUS)
    uint16_t total_files_created;           ///< Total data files created
    uint16_t total_directories_created;     ///< Total directories created
    uint64_t total_allocated_size;          ///< Total size of the allocated files, bytes
    bool index_created;                     ///< Global index created
    bool version_file_created;              ///< Version file created
    esp_err_t overall_result;               ///< Overall operation result
} rbpower_storage_init_result_t;

/* ================================================================================================
 * MAIN INITIALIZATION FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Initialize the file structure for the active units.
 *
 * Main initialization function that:
 * 1. Creates the base structure
 * 2. Creates the version file
 * 3. Creates directories, pre-allocated data files (with headers) and metadata for each unit
 * 4. Creates the global index
 *
 * @param[in]  units_list      Array of active unit IDs
 * @param[in]  units_count     Number of units in @p units_list
 * @param[in]  force_recreate  Recreate existing files (their contents are lost)
 * @param[out] result          Initialization result and counters
 *
 * @retval ESP_OK               At least one unit processed (partial success included)
 * @retval ESP_FAIL             No unit processed
 * @retval ESP_ERR_INVALID_ARG  @p units_list or @p result is NULL
 * @return Error from rbpower_storage_create_base_structure() if the base structure failed.
 */
esp_err_t rbpower_storage_initialize_filesystem(
    const rbpower_unit_id_t* units_list,
    uint8_t units_count,
    bool force_recreate,
    rbpower_storage_init_result_t* result
);

/**
 * @brief Create the file structure (directory, data files, metadata) for one unit.
 *
 * @param[in]  unit_id         Unit ID
 * @param[in]  unit_role       Unit role (selects which data files are created)
 * @param[in]  unit_type       Unit metric type
 * @param[in]  force_recreate  Recreate existing files (their contents are lost)
 * @param[out] result          Per-unit result (directory name, file count, size in bytes)
 *
 * @retval ESP_OK                 All files and the metadata created
 * @retval ESP_FAIL               Some data files or the metadata could not be created
 * @retval ESP_ERR_NOT_SUPPORTED  Unit is excluded from storage (VOLTAGE_BUS)
 * @retval ESP_ERR_INVALID_ARG    @p result is NULL
 * @return Other errors from rbpower_storage_create_unit_directory().
 */
esp_err_t rbpower_storage_create_unit_structure(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    bool force_recreate,
    rbpower_unit_creation_result_t* result
);

/* ================================================================================================
 * DIRECTORY OPERATIONS FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Create the base directory structure (existing directories are kept).
 *
 * Creates the base directories:
 * - /storage/
 * - /storage/units/
 * - /storage/.archive/
 *
 * @retval ESP_OK    Directories exist
 * @retval ESP_FAIL  mkdir() failed
 */
esp_err_t rbpower_storage_create_base_structure(void);

/**
 * @brief Create the directory of a unit (kept if it already exists).
 *
 * @param[in]  unit_id         Unit ID
 * @param[in]  unit_role       Unit role
 * @param[in]  unit_type       Unit metric type
 * @param[out] directory_path  Receives the full directory path
 * @param[in]  path_size       Size of @p directory_path in bytes (at least 64)
 *
 * @retval ESP_OK                 Directory exists
 * @retval ESP_ERR_INVALID_ARG    @p directory_path is NULL or @p path_size < 64
 * @retval ESP_ERR_NOT_SUPPORTED  Unit is excluded from storage (VOLTAGE_BUS)
 * @retval ESP_FAIL               mkdir() failed
 * @return Other errors from rbpower_storage_get_unit_directory_name().
 */
esp_err_t rbpower_storage_create_unit_directory(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    char* directory_path,
    size_t path_size
);

/**
 * @brief Check that the directory of a unit exists.
 *
 * @note The implementation goes through rbpower_storage_create_unit_directory(),
 *       so a missing directory is created as a side effect.
 *
 * @param[in] unit_id    Unit ID
 * @param[in] unit_role  Unit role
 * @param[in] unit_type  Unit metric type
 *
 * @return true if the directory exists; false on error or for an excluded unit.
 */
bool rbpower_storage_unit_directory_exists(
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type
);

/* ================================================================================================
 * DATA FILE OPERATIONS FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Create a data file: header plus a zero-filled, pre-allocated record area.
 *
 * @param[in] unit_directory  Path of the unit directory
 * @param[in] data_type       Data type
 * @param[in] period_type     Aggregation period
 * @param[in] force_recreate  Recreate an existing file (its contents are lost)
 *
 * @retval ESP_OK               File created, or already exists and @p force_recreate is false
 * @retval ESP_ERR_INVALID_ARG  @p unit_directory is NULL
 * @retval ESP_FAIL             File could not be created or written
 * @return Other errors from rbpower_init_file_header().
 */
esp_err_t rbpower_storage_create_data_file(
    const char* unit_directory,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    bool force_recreate
);

/**
 * @brief Build the full path of a data file.
 *
 * @param[in]  unit_directory  Path of the unit directory
 * @param[in]  data_type       Data type
 * @param[in]  period_type     Aggregation period
 * @param[out] file_path       Receives the path
 * @param[in]  path_size       Size of @p file_path in bytes (at least 128)
 *
 * @retval ESP_OK               Path built
 * @retval ESP_ERR_INVALID_ARG  NULL pointer or @p path_size < 128
 */
esp_err_t rbpower_storage_get_data_file_path(
    const char* unit_directory,
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type,
    char* file_path,
    size_t path_size
);

/* ================================================================================================
 * METADATA OPERATIONS FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Create the unit metadata file (`<unit_directory>/.meta`).
 *
 * @param[in] unit_directory  Path of the unit directory
 * @param[in] unit_id         Unit ID
 * @param[in] unit_role       Unit role
 * @param[in] unit_type       Unit metric type
 * @param[in] force_recreate  Recreate an existing file
 *
 * @retval ESP_OK               File created, or already exists and @p force_recreate is false
 * @retval ESP_ERR_INVALID_ARG  @p unit_directory is NULL
 * @retval ESP_FAIL             File could not be created or written
 * @return Other errors from rbpower_init_unit_metadata().
 */
esp_err_t rbpower_storage_create_unit_metadata(
    const char* unit_directory,
    rbpower_unit_id_t unit_id,
    rbpower_load_role_t unit_role,
    rbpower_metric_type_t unit_type,
    bool force_recreate
);

/* ================================================================================================
 * GLOBAL INDEX OPERATIONS FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Create or rewrite the global index file (RBPOWER_STORAGE_INDEX_FILE).
 *
 * @param[in] units_list      Array of active unit IDs
 * @param[in] units_count     Number of units
 * @param[in] force_recreate  Rewrite an existing index
 *
 * @retval ESP_OK               Index written, or already exists and @p force_recreate is false
 * @retval ESP_ERR_INVALID_ARG  @p units_list is NULL
 * @retval ESP_ERR_NO_MEM       Index buffer could not be allocated
 * @retval ESP_FAIL             Index file could not be written
 * @return Other errors from rbpower_storage_filter_units_for_stats().
 */
esp_err_t rbpower_storage_create_global_index(
    const rbpower_unit_id_t* units_list,
    uint8_t units_count,
    bool force_recreate
);

/**
 * @brief Load the global index file and verify its magic and CRC32.
 *
 * @param[out] index  Receives the loaded index
 *
 * @retval ESP_OK                 Index loaded and valid
 * @retval ESP_ERR_INVALID_ARG    @p index is NULL
 * @retval ESP_FAIL               File could not be opened or fully read
 * @retval ESP_ERR_INVALID_STATE  Magic or CRC mismatch
 */
esp_err_t rbpower_storage_load_global_index(rbpower_global_index_t* index);

/**
 * @brief Write the global index file (the CRC field is written as given).
 *
 * @param[in] index  Index to save
 *
 * @retval ESP_OK               Saved
 * @retval ESP_ERR_INVALID_ARG  @p index is NULL
 * @retval ESP_FAIL             File could not be created or fully written
 */
esp_err_t rbpower_storage_save_global_index(const rbpower_global_index_t* index);

/* ================================================================================================
 * UTILITY AND HELPER FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Create (overwrite) the version file RBPOWER_STORAGE_VERSION_FILE.
 *
 * @retval ESP_OK    Written
 * @retval ESP_FAIL  File could not be created
 */
esp_err_t rbpower_storage_create_version_file(void);

/**
 * @brief Get the total and free space of the `storage` partition.
 *
 * @note Currently returns fixed estimates (1 MB total, 512 KB free), not values
 *       queried from LittleFS.
 *
 * @param[out] total_bytes  Receives the total size in bytes
 * @param[out] free_bytes   Receives the free space in bytes
 *
 * @retval ESP_OK               Values returned
 * @retval ESP_ERR_INVALID_ARG  A pointer is NULL
 */
esp_err_t rbpower_storage_get_space_info(uint64_t* total_bytes, uint64_t* free_bytes);

/**
 * @brief Get the data file name for a type/period.
 *
 * Not reentrant: the name is returned in a static buffer that the next call overwrites.
 *
 * @param[in] data_type    Data type
 * @param[in] period_type  Aggregation period
 *
 * @return File name (e.g. "electrical_hour.dat"), or "unknown.dat" on error.
 */
const char* rbpower_storage_get_data_filename(
    rbpower_data_type_t data_type,
    rbpower_period_type_t period_type
);

/**
 * @brief Log the initialization statistics (debug).
 *
 * @param[in] result  Initialization result to print (NULL is logged as a warning)
 */
void rbpower_storage_print_init_statistics(const rbpower_storage_init_result_t* result);

/**
 * @brief Check that the storage file structure exists.
 *
 * @return true if the base and units directories, the version file and the index file
 *         all exist (the structure is ready for writing).
 */
bool rbpower_storage_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_INIT_H