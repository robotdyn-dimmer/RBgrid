/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_config.h
 * @brief Static layout configuration for the flash statistics buffers
 *
 * Describes a per-unit, per-period buffer layout and provides a default layout, validation,
 * lookup and a log dump. The JSON file in the /config filesystem is not implemented yet:
 * rbpower_flash_config_load() returns the default layout and rbpower_flash_config_save()
 * returns ESP_ERR_NOT_SUPPORTED. The live slot configuration is in rbextflash_config_runtime.h.
 */

#ifndef RBPOWER_FLASH_CONFIG_H
#define RBPOWER_FLASH_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "rbpower_enums.h"
#include "rbstorage_transfer_types.h"
#include "rbextflash_structures.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ================================================================================================
 * CONSTANTS
 * ================================================================================================ */

/** @brief Path of the JSON configuration file (in the /config filesystem). */
#define RBPOWER_CONFIG_PATH             "/config/rbpower_buffers_config.json"
/** @brief Path of the JSON configuration backup file. */
#define RBPOWER_CONFIG_BACKUP_PATH      "/config/rbpower_buffers_config.bak"

/** @brief Configuration format major version (must match to be valid). */
#define RBPOWER_CONFIG_VERSION_MAJOR    1
/** @brief Configuration format minor version. */
#define RBPOWER_CONFIG_VERSION_MINOR    0

/** @brief Maximum units in a configuration. */
#define RBPOWER_CONFIG_MAX_UNITS        32      ///< Maximum units in configuration
/** @brief Maximum buffers per unit. */
#define RBPOWER_CONFIG_MAX_BUFFERS      25      ///< Maximum buffers per unit

/** @brief JSON parse buffer size, bytes (32 KB). */
#define RBPOWER_CONFIG_JSON_BUFFER_SIZE (32 * 1024)  ///< 32KB for JSON parsing

/* ================================================================================================
 * DATA STRUCTURES
 * ================================================================================================ */

/**
 * @brief Configuration of one flash statistics buffer
 */
typedef struct {
    // Identification
    rbpower_period_t period;                    ///< Period (HOUR, DAY, etc)
    rbpower_data_type_t data_type;              ///< Data type (ELECTRICAL, etc)
    bool enabled;                               ///< Is buffer enabled

    // Flash placement
    uint32_t flash_offset;                      ///< Offset from start of STATS partition (bytes)
    uint32_t buffer_size_bytes;                 ///< Buffer size (bytes)

    // Record parameters
    uint16_t record_size;                       ///< Size of one record (bytes)
    uint16_t records_per_sector;                ///< Records per 4096-byte sector
    uint16_t total_sectors;                     ///< Total sectors in buffer

    // Capacities
    uint32_t physical_capacity;                 ///< Physical capacity (records)
    uint32_t logical_capacity;                  ///< Logical capacity (records)

} rbpower_flash_buffer_config_t;

/**
 * @brief Configuration of one unit for flash statistics
 */
typedef struct {
    // Identification
    rbpower_unit_id_t unit_id;                  ///< Unit ID (0x4401, etc)
    char unit_name[32];                         ///< Unit name ("Main Supply")
    uint16_t unit_role;                         ///< Role (1=MAIN_SUPPLY, 2=ALT_SUPPLY, 3=ZONE_LOAD)

    // Status
    bool enabled;                               ///< Is unit enabled
    uint64_t creation_timestamp;                ///< Creation time, s

    // Buffers
    uint8_t buffer_count;                       ///< Number of buffers
    rbpower_flash_buffer_config_t buffers[RBPOWER_CONFIG_MAX_BUFFERS]; ///< Buffer table (buffer_count used)

} rbpower_flash_unit_config_t;

/**
 * @brief Main configuration of all buffers (large, ~50 KB: allocate on the heap)
 */
typedef struct {
    // Configuration version
    uint8_t version_major; ///< Configuration format major version
    uint8_t version_minor; ///< Configuration format minor version

    // STATS partition information
    uint32_t stats_partition_offset;            ///< STATS partition offset (bytes)
    uint32_t stats_partition_size;              ///< STATS partition size (bytes)

    // Units
    uint8_t unit_count;                         ///< Number of units
    rbpower_flash_unit_config_t units[RBPOWER_CONFIG_MAX_UNITS]; ///< Unit table (unit_count used)

    // Metadata
    uint64_t last_updated;                      ///< Time of last update, s (the default layout uses seconds since boot)

} rbpower_flash_config_t;

/* ================================================================================================
 * API FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Initialize configuration module
 *
 * Checks that the /config filesystem is mounted and logs whether the configuration file exists.
 *
 * @retval ESP_OK                 /config is mounted
 * @retval ESP_ERR_INVALID_STATE  /config partition not mounted
 */
esp_err_t rbpower_flash_config_init(void);

/**
 * @brief Load configuration from JSON file (not implemented)
 *
 * Intended to read /config/rbpower_buffers_config.json. Currently logs a warning and returns
 * the default layout from rbpower_flash_config_create_default().
 *
 * @param[out] config_out Destination for the configuration
 * @return Result of rbpower_flash_config_create_default()
 */
esp_err_t rbpower_flash_config_load(rbpower_flash_config_t* config_out);

/**
 * @brief Save configuration to JSON file (not implemented)
 *
 * @param[in] config Configuration to save (unused)
 * @retval ESP_ERR_NOT_SUPPORTED Always
 */
esp_err_t rbpower_flash_config_save(const rbpower_flash_config_t* config);

/**
 * @brief Create default configuration
 *
 * Builds a one-unit layout (unit 0x4401 "Main Supply") with HOUR and DAY buffers for the
 * electrical, energy and economics data types, placed back to back from offset 0 of the
 * STATS partition.
 *
 * @param[out] config_out Destination for the configuration
 * @retval ESP_OK               Configuration created
 * @retval ESP_ERR_INVALID_ARG  NULL @p config_out
 * @retval ESP_FAIL             STATS partition information not available
 */
esp_err_t rbpower_flash_config_create_default(rbpower_flash_config_t* config_out);

/**
 * @brief Validate configuration
 *
 * Checks: major version compatible, unit and buffer counts within limits, period and data type
 * in range, non-zero sizes. A records_per_sector mismatch is only logged as a warning.
 * Offset overlap is not checked.
 *
 * @param[in] config Pointer to configuration
 * @retval true  Configuration is valid
 * @retval false NULL pointer or a check failed (logged)
 */
bool rbpower_flash_config_validate(const rbpower_flash_config_t* config);

/**
 * @brief Find buffer in configuration
 *
 * @param[in] config Pointer to configuration
 * @param[in] unit_id Unit ID
 * @param[in] period Period
 * @param[in] data_type Data type
 * @return Pointer to buffer configuration (inside @p config) or NULL if not found
 */
const rbpower_flash_buffer_config_t* rbpower_flash_config_find_buffer(
    const rbpower_flash_config_t* config,
    rbpower_unit_id_t unit_id,
    rbpower_period_t period,
    rbpower_data_type_t data_type);

/**
 * @brief Find unit in configuration
 *
 * @param[in] config Pointer to configuration
 * @param[in] unit_id Unit ID
 * @return Pointer to unit configuration (inside @p config) or NULL if not found
 */
const rbpower_flash_unit_config_t* rbpower_flash_config_find_unit(
    const rbpower_flash_config_t* config,
    rbpower_unit_id_t unit_id);

/**
 * @brief Print configuration to log (for debugging)
 *
 * @param[in] config Pointer to configuration
 */
void rbpower_flash_config_dump(const rbpower_flash_config_t* config);

/**
 * @brief Configuration module self-test (diagnostic)
 *
 * Creates the default configuration, validates it, looks up a unit and a buffer, and dumps it.
 *
 * @retval ESP_OK          All steps passed
 * @retval ESP_ERR_NO_MEM  Could not allocate the configuration
 * @retval ESP_FAIL        A step failed (or the error returned by
 *                         rbpower_flash_config_create_default())
 */
esp_err_t rbpower_flash_config_test(void);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_FLASH_CONFIG_H
