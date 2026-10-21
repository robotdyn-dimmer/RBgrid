/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage.h
 * @brief Filesystem management: LittleFS partitions (`storage`, `config`, `logs`),
 *        boot flags in NVS, partition recovery, and storage diagnostic tests.
 *
 * Functions that change partition state take the module mutex (5 s timeout) and may be
 * called from any task after rbpower_storage_init(); they must not be called from an ISR.
 * The `config` partition is disabled by default because it is owned by the raw LittleFS
 * layer (littlefs_raw.h); mounting it a second time through VFS could corrupt it.
 */

#ifndef RBPOWER_STORAGE_H
#define RBPOWER_STORAGE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "rbstorage_types.h"

    /**
     * @brief Initialize the storage module and load the boot flags from NVS.
     *
     * Creates the module mutex. If the boot flags cannot be loaded, the boot is treated as
     * a first boot (see rbpower_storage_first_boot_init()). Idempotent.
     * Call once at boot, before any other function of this module.
     *
     * @param[in] config  Module configuration to copy, or NULL to use the built-in defaults
     *
     * @retval ESP_OK          Initialized (or already initialized)
     * @retval ESP_ERR_NO_MEM  Mutex could not be created
     */
    esp_err_t rbpower_storage_init(const rbstorage_config_t *config);

    /**
     * @brief Deinitialize the module: save boot flags, unmount all partitions, delete the mutex.
     *
     * @retval ESP_OK                 Deinitialized
     * @retval ESP_ERR_INVALID_STATE  Module not initialized
     * @retval ESP_ERR_TIMEOUT        Module lock not acquired within 5 s
     */
    esp_err_t rbpower_storage_deinit(void);

    /**
     * @brief First-boot storage preparation.
     *
     * For every enabled partition: verify it exists in the partition table, mount it (format
     * and remount on failure when allowed), and create the initialization marker. Then clear
     * the first-boot flag and save the boot flags. Does nothing when this is not a first boot.
     *
     * @retval ESP_OK                 All enabled partitions prepared, or not a first boot
     * @retval ESP_FAIL               At least one partition failed
     * @retval ESP_ERR_INVALID_STATE  Module not initialized
     * @retval ESP_ERR_TIMEOUT        Module lock not acquired within 5 s
     */
    esp_err_t rbpower_storage_first_boot_init(void);

    /**
     * @brief Mount a partition through the LittleFS VFS (never formats).
     *
     * @param[in] partition_id  Partition identifier
     *
     * @retval ESP_OK                 Mounted (or already mounted)
     * @retval ESP_ERR_INVALID_STATE  Module not initialized
     * @retval ESP_ERR_INVALID_ARG    Invalid @p partition_id
     * @retval ESP_ERR_TIMEOUT        Module lock not acquired within 5 s
     * @return Other codes from esp_vfs_littlefs_register().
     */
    esp_err_t rbpower_storage_mount_partition(rbstorage_partition_id_t partition_id);

    /**
     * @brief Unmount a partition.
     *
     * @param[in] partition_id  Partition identifier
     *
     * @retval ESP_OK                 Unmounted (or was not mounted)
     * @retval ESP_ERR_INVALID_STATE  Module not initialized
     * @retval ESP_ERR_INVALID_ARG    Invalid @p partition_id
     * @retval ESP_ERR_TIMEOUT        Module lock not acquired within 5 s
     * @return Other codes from esp_vfs_littlefs_unregister().
     */
    esp_err_t rbpower_storage_unmount_partition(rbstorage_partition_id_t partition_id);

    /**
     * @brief Format a partition (unmounts it first if mounted). All data on it is lost.
     *
     * @param[in] partition_id  Partition identifier (must be valid; not range-checked before
     *                          the mount-state lookup)
     *
     * @retval ESP_OK                 Formatted
     * @retval ESP_ERR_INVALID_STATE  Module not initialized
     * @retval ESP_ERR_INVALID_ARG    Invalid @p partition_id
     * @retval ESP_ERR_TIMEOUT        Module lock not acquired within 5 s
     * @return Other codes from esp_littlefs_format().
     */
    esp_err_t rbpower_storage_format_partition(rbstorage_partition_id_t partition_id);

    /**
     * @brief Health-check a mounted partition by creating and deleting a test file.
     *
     * @param[in]  partition_id  Partition identifier
     * @param[out] result        Check result (error code, duration in ms, message); may be NULL
     *
     * @retval ESP_OK                 Partition is writable
     * @retval ESP_ERR_INVALID_ARG    Module not initialized or invalid @p partition_id
     * @retval ESP_ERR_INVALID_STATE  Partition not mounted
     * @retval ESP_ERR_TIMEOUT        Module lock not acquired within 5 s
     * @retval ESP_FAIL               Test file could not be created or deleted
     */
    esp_err_t rbpower_storage_check_partition(rbstorage_partition_id_t partition_id,
                                              rbstorage_operation_result_t *result);

    /**
     * @brief Enable or disable a partition (in RAM state only; not persisted).
     *
     * @param[in] partition_id  Partition identifier
     * @param[in] enable        true to enable, false to disable
     *
     * @retval ESP_OK               State updated
     * @retval ESP_ERR_INVALID_ARG  Module not initialized or invalid @p partition_id
     * @retval ESP_ERR_TIMEOUT      Module lock not acquired within 5 s
     */
    esp_err_t rbpower_storage_enable_partition(rbstorage_partition_id_t partition_id, bool enable);

    /**
     * @brief Set the check mode of a partition (persisted with the next boot-flags save).
     *
     * @param[in] partition_id  Partition identifier
     * @param[in] skip_checks   true to skip checks
     *
     * @retval ESP_OK               Mode updated
     * @retval ESP_ERR_INVALID_ARG  Module not initialized or invalid @p partition_id
     * @retval ESP_ERR_TIMEOUT      Module lock not acquired within 5 s
     */
    esp_err_t rbpower_storage_set_check_mode(rbstorage_partition_id_t partition_id, bool skip_checks);

    /**
     * @brief Check whether a partition is ready for use. Lock-free read of the state.
     *
     * @param[in] partition_id  Partition identifier
     *
     * @return true if the partition is enabled, mounted and initialized; false otherwise
     *         (also when the module is not initialized or the ID is invalid).
     */
    bool rbpower_storage_is_partition_ready(rbstorage_partition_id_t partition_id);

    /**
     * @brief Get the size and usage of a mounted partition.
     *
     * @param[in]  partition_id  Partition identifier
     * @param[out] total_bytes   Total size in bytes (may be NULL)
     * @param[out] used_bytes    Used size in bytes (may be NULL)
     *
     * @retval ESP_OK                 Info returned
     * @retval ESP_ERR_INVALID_ARG    Module not initialized or invalid @p partition_id
     * @retval ESP_ERR_INVALID_STATE  Partition not mounted
     * @return Other codes from esp_littlefs_info().
     */
    esp_err_t rbpower_storage_get_info(rbstorage_partition_id_t partition_id,
                                       size_t *total_bytes, size_t *used_bytes);

    /**
     * @brief Get the runtime state of a partition.
     *
     * @param[in] partition_id  Partition identifier
     *
     * @return Pointer to the module-owned state (not locked; read-only), or NULL when the
     *         module is not initialized or the ID is invalid.
     */
    const rbstorage_partition_state_t *rbpower_storage_get_state(rbstorage_partition_id_t partition_id);

    /**
     * @brief Get the global module configuration.
     *
     * @return Pointer to the module-owned configuration (read-only), or NULL when the module
     *         is not initialized.
     */
    const rbstorage_config_t *rbpower_storage_get_config(void);

    /**
     * @brief Save the boot flags and per-partition counters to NVS (with checksum).
     *
     * @retval ESP_OK  Saved and committed
     * @return Other codes from nvs_open() / nvs_set_blob() / nvs_commit().
     */
    esp_err_t rbpower_storage_save_boot_flags(void);

    /**
     * @brief Load the boot flags and per-partition counters from NVS.
     *
     * On success the first-boot flag is cleared.
     *
     * @retval ESP_OK                   Loaded
     * @retval ESP_ERR_INVALID_VERSION  Stored magic or version does not match
     * @retval ESP_ERR_INVALID_CRC      Stored checksum does not match
     * @return Other codes from nvs_open() / nvs_get_blob() (e.g. ESP_ERR_NVS_NOT_FOUND).
     */
    esp_err_t rbpower_storage_load_boot_flags(void);

    /**
     * @brief Get the label of a partition.
     *
     * @param[in] partition_id  Partition identifier
     *
     * @return Static string: "storage", "config", "logs", or "unknown" for an invalid ID.
     */
    const char *rbpower_storage_get_partition_name(rbstorage_partition_id_t partition_id);

    /**
     * @brief Get the VFS mount path of a partition.
     *
     * @param[in] partition_id  Partition identifier
     *
     * @return Mount path (e.g. "/storage"), or NULL when the module is not initialized or the
     *         ID is invalid.
     */
    const char *rbpower_storage_get_mount_path(rbstorage_partition_id_t partition_id);

    /**
     * @brief Recover a corrupted partition. All data on it is lost.
     *
     * Steps:
     * - Unmounts the partition
     * - Formats it
     * - Remounts it
     * - Creates the initialization marker
     * - Updates the counters and saves the boot flags
     *
     * @param[in] partition_id  Partition to recover
     *
     * @retval ESP_OK               Recovered
     * @retval ESP_ERR_INVALID_ARG  Module not initialized or invalid @p partition_id
     * @retval ESP_ERR_TIMEOUT      Module lock not acquired within 5 s
     * @return Other codes from esp_littlefs_format() / esp_vfs_littlefs_register().
     */
    esp_err_t rbpower_storage_recover_partition(rbstorage_partition_id_t partition_id);

    /**
     * @brief Diagnostic test of the unit mapping logic.
     *
     * Checks:
     * - VOLTAGE_BUS unit exclusion
     * - Directory name generation
     * - Unit filtering for statistics
     * - Per-role file configuration
     *
     * @retval ESP_OK    All checks passed
     * @retval ESP_FAIL  At least one check failed (details are logged)
     */
    esp_err_t rbpower_storage_test_unit_mapping(void);

    /**
     * @brief Diagnostic test of the storage file structure creation.
     *
     * Recreates the structure for a fixed set of test units (force_recreate) and checks:
     * - Base directory structure creation
     * - Data file creation with headers
     * - Unit metadata creation
     * - Global index creation
     * - VOLTAGE_BUS exclusion
     *
     * @warning Overwrites existing storage files of the test units.
     *
     * @retval ESP_OK    All checks passed
     * @retval ESP_FAIL  Structure created but a check failed
     * @return Other codes from rbpower_storage_initialize_filesystem().
     */
    esp_err_t rbpower_storage_test_filesystem_creation(void);

    /**
     * @brief Print the sizes of the main storage structures (stdout).
     *
     * Used to diagnose alignment and size issues.
     *
     * @return Always ESP_OK.
     */
    esp_err_t rbpower_storage_debug_struct_sizes(void);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_H