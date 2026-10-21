/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_config.h
 * @brief Data-file layout configuration per unit role.
 *
 * A constant matrix maps each unit role and period to the set of data files
 * (electrical, energy, economics, quality, direction) kept for it, plus the
 * special-folder rules (main / alternative supply) and the exclusion of
 * VOLTAGE_BUS units from statistics.
 *
 * All tables are read-only; the functions are pure lookups (apart from
 * logging and the allocation in rbpower_storage_filter_units_for_stats())
 * and may be called from any task.
 */

#ifndef RBPOWER_STORAGE_CONFIG_H
#define RBPOWER_STORAGE_CONFIG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "rbhome_types.h"             // rbpower_load_role_t, rbpower_metric_type_t, rbpower_unit_id_t
#include "rbstorage_transfer_types.h" // rbpower_period_files_config_t, rbpower_period_type_t

/* ================================================================================================
 * CONSTANTS AND LIMITS
 * ================================================================================================ */

/** @brief Number of role slots in the configuration tables (rbpower_load_role_t up to RBPOWER_ROLE_SMART_APPLIANCE, plus reserve). */
#define RBPOWER_STORAGE_ROLE_MAX 12

/** @brief Minimum buffer size for a unit directory name, bytes (including the terminating NUL). */
#define RBPOWER_STORAGE_MAX_DIR_NAME 32

    /* ================================================================================================
     * UNIT PROCESSING TYPES
     * ================================================================================================ */

    /**
     * @brief Unit processing types for storage system
     */
    typedef enum
    {
        RBPOWER_UNIT_PROCESSING_NORMAL = 0,   ///< Normal processing (folder named by unit_id in hex)
        RBPOWER_UNIT_PROCESSING_EXCLUDE,      ///< Excluded from statistics (VOLTAGE_BUS)
        RBPOWER_UNIT_PROCESSING_SPECIAL_MAIN, ///< Special folder "Unit_MainSupply"
        RBPOWER_UNIT_PROCESSING_SPECIAL_ALT   ///< Special folder "Unit_AltSupply"
    } rbpower_unit_processing_type_t;

    /* ================================================================================================
     * CONFIGURATION MATRIX
     * ================================================================================================ */

    /**
     * @brief Mapping of roles to processing type
     *
     * Defines how each unit role is processed:
     * - VOLTAGE_BUS: excluded from statistics
     * - MAIN_SUPPLY: special folder Unit_MainSupply
     * - ALTERNATIVE_SUPPLY: special folder Unit_AltSupply
     * - Others: regular folders by unit_id
     */
    extern const rbpower_unit_processing_type_t UNIT_PROCESSING_MAP[RBPOWER_STORAGE_ROLE_MAX];

    /**
     * @brief Main file configuration matrix
     *
     * Defines which files to create for each role and period combination.
     * Indices: [role][period] -> file configuration
     */
    extern const rbpower_period_files_config_t UNIT_FILES_CONFIG[RBPOWER_STORAGE_ROLE_MAX][RBPOWER_PERIOD_MAX];

    /**
     * @brief Special folder names, indexed by ::rbpower_unit_processing_type_t (NULL for NORMAL/EXCLUDE).
     */
    extern const char *SPECIAL_UNIT_NAMES[4];

    /* ================================================================================================
     * UNIT PROCESSING DETERMINATION FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Determine the processing type of a unit.
     *
     * VOLTAGE_BUS units are excluded regardless of role; an out-of-range role
     * gets NORMAL processing.
     *
     * @param[in] unit_role Unit role
     * @param[in] unit_type Unit metric type (for the VOLTAGE_BUS check)
     * @return Unit processing type
     */
    rbpower_unit_processing_type_t rbpower_storage_get_unit_processing_type(
        rbpower_load_role_t unit_role,
        rbpower_metric_type_t unit_type);

    /**
     * @brief Check whether a unit is excluded from statistics.
     *
     * @param[in] unit_role Unit role
     * @param[in] unit_type Unit metric type
     * @return true if the unit is excluded
     */
    bool rbpower_storage_should_exclude_unit(
        rbpower_load_role_t unit_role,
        rbpower_metric_type_t unit_type);

    /**
     * @brief Determine directory name for unit
     *
     * Returns the folder name for a unit based on its role and type:
     * - VOLTAGE_BUS: error (no folder is created)
     * - MAIN_SUPPLY: "Unit_MainSupply"
     * - ALTERNATIVE_SUPPLY: "Unit_AltSupply"
     * - Others: unit_id as 4 hex digits (e.g. "4401")
     *
     * @param[in]  unit_id        Unit ID
     * @param[in]  unit_role      Unit role
     * @param[in]  unit_type      Unit metric type
     * @param[out] directory_name Buffer for the name (at least RBPOWER_STORAGE_MAX_DIR_NAME bytes)
     * @param[in]  name_size      Size of directory_name, bytes
     * @retval ESP_OK                success
     * @retval ESP_ERR_INVALID_ARG   NULL buffer or name_size < RBPOWER_STORAGE_MAX_DIR_NAME
     * @retval ESP_ERR_NOT_SUPPORTED unit is excluded from storage
     */
    esp_err_t rbpower_storage_get_unit_directory_name(
        rbpower_unit_id_t unit_id,
        rbpower_load_role_t unit_role,
        rbpower_metric_type_t unit_type,
        char *directory_name,
        size_t name_size);

    /**
     * @brief Get the data-file set for a role and period.
     *
     * @param[in]  unit_role   Unit role
     * @param[in]  period_type Period type
     * @param[out] config      Receives the file configuration
     * @retval ESP_OK              success
     * @retval ESP_ERR_INVALID_ARG config is NULL, or role/period out of range
     */
    esp_err_t rbpower_storage_get_files_config(
        rbpower_load_role_t unit_role,
        rbpower_period_type_t period_type,
        rbpower_period_files_config_t *config);

    /**
     * @brief Check whether a data file of the given type is kept for a role and period.
     *
     * @param[in] unit_role   Unit role
     * @param[in] period_type Period type
     * @param[in] data_type   Data type
     * @return true if the file should be created; false if not or on invalid arguments
     */
    bool rbpower_storage_should_create_file(
        rbpower_load_role_t unit_role,
        rbpower_period_type_t period_type,
        rbpower_data_type_t data_type);

    /* ================================================================================================
     * UNIT FILTERING FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Filter a unit list down to the units that get statistics.
     *
     * Removes VOLTAGE_BUS units. Currently the check is by the fixed
     * VOLTAGE_BUS unit ID (0x0007) only, not by the unit definition.
     *
     * @param[in]  input_units    Input unit array
     * @param[in]  input_count    Number of input units
     * @param[out] filtered_units Receives a heap-allocated array, or NULL if no unit remains
     * @param[out] filtered_count Receives the number of units in filtered_units
     * @retval ESP_OK              success
     * @retval ESP_ERR_INVALID_ARG NULL pointer argument
     * @retval ESP_ERR_NO_MEM      allocation failed
     *
     * @note The caller must free filtered_units with free().
     */
    esp_err_t rbpower_storage_filter_units_for_stats(
        const rbpower_unit_id_t *input_units,
        uint8_t input_count,
        rbpower_unit_id_t **filtered_units,
        uint8_t *filtered_count);

    /**
     * @brief Count the data files created for a unit role.
     *
     * @param[in] unit_role Unit role
     * @return Total file count over all periods (0 for an invalid role)
     */
    uint8_t rbpower_storage_count_unit_files(rbpower_load_role_t unit_role);

    /**
     * @brief Estimate storage size for unit
     *
     * Approximate size of all data files of a unit, from the configuration,
     * the per-period record counts and the transfer-structure sizes (a 64-byte
     * header is assumed per file).
     *
     * @param[in] unit_role Unit role
     * @return Size in bytes (0 for an invalid role)
     */
    uint64_t rbpower_storage_estimate_unit_size(rbpower_load_role_t unit_role);

    /* ================================================================================================
     * UTILITY FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Get the string name of a processing type.
     *
     * @param[in] processing_type Processing type
     * @return Static string ("normal", "exclude", "main_supply", "alt_supply" or "unknown")
     */
    const char *rbpower_storage_get_processing_type_name(rbpower_unit_processing_type_t processing_type);

    /**
     * @brief Check whether a role is within the storage configuration tables.
     *
     * @param[in] unit_role Unit role
     * @return true if the role is supported
     */
    bool rbpower_storage_is_role_valid(rbpower_load_role_t unit_role);

    /**
     * @brief Log the configuration of a role (debug, ESP_LOGI).
     *
     * @param[in] unit_role Role to print
     */
    void rbpower_storage_print_role_config(rbpower_load_role_t unit_role);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_CONFIG_H