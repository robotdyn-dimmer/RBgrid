/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_types.h
 * @brief Data types and structures for the filesystem management module
 *
 * Partition identifiers, per-partition configuration and runtime state, and the
 * boot-flags record persisted in NVS. The runtime state lives in a single global
 * owned by rbstorage.c.
 */

#ifndef RBPOWER_STORAGE_TYPES_H
#define RBPOWER_STORAGE_TYPES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

    /**
     * @brief Partition identifiers
     */
    typedef enum
    {
        RBSTORAGE_PARTITION_STORAGE = 0, ///< Partition for statistics
        RBSTORAGE_PARTITION_CONFIG,      ///< Partition for configuration (persistent)
        RBSTORAGE_PARTITION_LOGS,        ///< Partition for logs
        RBSTORAGE_PARTITION_COUNT        ///< Number of partitions (not a partition)
    } rbstorage_partition_id_t;

    /**
     * @brief Partition state flags
     */
    typedef struct
    {
        bool is_enabled;             ///< Partition is enabled in the configuration
        bool is_mounted;             ///< Partition is mounted
        bool is_initialized;         ///< Directory structure has been created
        bool first_boot_done;        ///< First initialization completed
        bool skip_checks;            ///< Skip checks at startup
        uint32_t mount_count;        ///< Mount counter
        uint64_t last_check_time;    ///< Time of the last check, µs (esp_timer timebase)
        uint32_t errors_count;       ///< Error counter
        uint32_t recovery_count;     ///< Recovery counter
        uint64_t last_recovery_time; ///< Time of the last recovery, µs (esp_timer_get_time())
    } rbstorage_partition_state_t;

    /**
     * @brief Partition configuration
     */
    typedef struct
    {
        const char *partition_label; ///< Partition label in the partition table
        const char *mount_path;      ///< VFS mount path
        size_t max_files;            ///< Maximum number of simultaneously open files
        bool format_on_fail;         ///< Format the partition if mounting fails
        bool enable_by_default;      ///< Enabled by default
        uint32_t check_interval_sec; ///< Periodic check interval, s
    } rbstorage_partition_config_t;

    /**
     * @brief Global storage configuration
     */
    typedef struct
    {
        // Partition configuration
        rbstorage_partition_config_t partitions[RBSTORAGE_PARTITION_COUNT]; ///< Per-partition configuration

        // Partition state (runtime)
        rbstorage_partition_state_t states[RBSTORAGE_PARTITION_COUNT]; ///< Per-partition runtime state

        // Global flags
        bool initialized;       ///< Module initialized
        bool first_boot;        ///< First system boot
        bool fast_boot_enabled; ///< Fast boot (minimum checks)
        bool recovery_mode;     ///< Recovery mode

        // Check settings
        bool check_on_mount;  ///< Check on mount
        bool periodic_checks; ///< Periodic checks
        uint32_t check_depth; ///< Check depth (0-3)

        // Statistics
        uint32_t total_boots;      ///< Total boot count
        uint32_t successful_boots; ///< Successful boot count
        uint64_t init_time_us;     ///< Module initialization time, µs
    } rbstorage_config_t;

    /**
     * @brief Operation result
     */
    typedef struct
    {
        esp_err_t error_code;                  ///< Result code
        rbstorage_partition_id_t partition_id; ///< Partition the operation applied to
        char error_msg[128];                   ///< NUL-terminated error description
        uint32_t duration_ms;                  ///< Operation duration, ms
    } rbstorage_operation_result_t;

    /**
     * @brief Boot-flags record persisted in NVS
     */
    typedef struct
    {
        uint32_t magic;    ///< RBSTORAGE_NVS_MAGIC, 0x52425354 "RBST"
        uint16_t version;  ///< Structure version (RBSTORAGE_NVS_VERSION)
        uint16_t checksum; ///< Checksum of the record

        /** @brief Persisted flags for each partition */
        struct
        {
            bool first_boot_done;        ///< First initialization completed
            bool skip_checks;            ///< Skip checks at startup
            uint32_t mount_count;        ///< Mount counter
            uint64_t last_check_time;    ///< Time of the last check, µs (esp_timer timebase)
            uint32_t recovery_count;     ///< Recovery counter
            uint64_t last_recovery_time; ///< Time of the last recovery, µs (esp_timer timebase)
        } partitions[RBSTORAGE_PARTITION_COUNT]; ///< Per-partition flags

        // Global counters
        uint32_t total_boots;      ///< Total boot count
        uint32_t successful_boots; ///< Successful boot count

    } rbstorage_nvs_data_t;

/** @brief Magic of the NVS boot-flags record ("RBST"). */
#define RBSTORAGE_NVS_MAGIC 0x52425354 // "RBST"
/** @brief Version of the NVS boot-flags record. */
#define RBSTORAGE_NVS_VERSION 1
/** @brief NVS namespace of the boot-flags record. */
#define RBSTORAGE_NVS_NAMESPACE "rbstorage"
/** @brief NVS key of the boot-flags record. */
#define RBSTORAGE_NVS_KEY "boot_flags"

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_TYPES_H