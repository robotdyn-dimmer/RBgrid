/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_structures.h
 * @brief Data structures for statistics file system storage
 *
 * Ring buffer headers, unit metadata and global index used by the file-based
 * (filesystem-partition) statistics storage. All on-file structures are packed
 * and have fixed sizes that are checked with static assertions.
 *
 * The functions declared here operate only on caller-provided buffers and touch
 * no shared state, so they may be called from any task.
 */

#ifndef RBPOWER_STORAGE_STRUCTURES_H
#define RBPOWER_STORAGE_STRUCTURES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "rbhome_types.h"             // rbpower_unit_id_t, rbpower_load_role_t, rbpower_metric_type_t
#include "rbstorage_transfer_types.h" // rbpower_version_t, rbpower_data_type_t, rbpower_period_type_t
#include "rbstorage_config.h"         // rbpower_period_files_config_t

/* ================================================================================================
 * CONSTANTS AND LIMITS
 * ================================================================================================ */

/** @brief Maximum number of units in the system (entries in the global index). */
#define RBPOWER_MAX_UNITS 64

/** @brief Data file header size in bytes (multiple of 64 for flash alignment). */
#define RBPOWER_FILE_HEADER_SIZE 64

/** @brief Unit metadata block size in bytes. */
#define RBPOWER_UNIT_META_SIZE 256

/** @brief Maximum length of the migration reason string in bytes, including the terminating NUL. */
#define RBPOWER_MIGRATION_REASON_LEN 32

    /* ================================================================================================
     * DATA FILE HEADER (RING BUFFER)
     * ================================================================================================ */

    /**
     * @brief Statistical data file header
     *
     * Header structure for files with ring buffers.
     * Size exactly 64 bytes for optimal flash memory alignment.
     * The data area of max_records * record_size bytes follows the header.
     */
    typedef struct
    {
        // Identification and versions (16 bytes)
        uint32_t magic;              ///< Must equal RBPOWER_FILE_HEADER_MAGIC
        rbpower_version_t version;   ///< Data format version (8 bytes)
        uint32_t creation_timestamp; ///< File creation time, s (set from esp_timer, i.e. seconds since boot)

        // Ring buffer configuration (16 bytes)
        uint32_t data_type;   ///< Data type, a rbpower_data_type_t value
        uint32_t period_type; ///< Aggregation period, a rbpower_period_type_t value
        uint32_t record_size; ///< Size of one transfer structure, bytes
        uint32_t max_records; ///< Ring buffer capacity, records

        // Ring buffer state (16 bytes)
        uint32_t current_index;   ///< Index of the next record to write (head), 0..max_records-1
        uint32_t total_records;   ///< Total records written over the file lifetime (not wrapped)
        uint32_t first_timestamp; ///< Time of the first record, s (0 = no record yet)
        uint32_t last_timestamp;  ///< Time of the last record, s (0 = no record yet)

        // Integrity control and reserve (16 bytes)
        uint32_t crc32_header; ///< CRC32 of the header, computed with this field excluded
        uint32_t data_crc32;   ///< CRC32 of the data area (optional, currently 0)
        uint32_t write_count;  ///< Write operations counter
        uint32_t reserved;     ///< Reserved for future extensions (0)

    } __attribute__((packed)) rbpower_file_header_t;

    // Static size check
    _Static_assert(sizeof(rbpower_file_header_t) == RBPOWER_FILE_HEADER_SIZE,
                   "File header must be exactly 64 bytes");

    /* ================================================================================================
     * UNIT METADATA
     * ================================================================================================ */

    /**
     * @brief Unit migration history (ID or role change)
     */
    typedef struct
    {
        rbpower_unit_id_t previous_unit_id;                  ///< Previous unit_id (2 bytes)
        uint8_t previous_role;                               ///< Previous role (1 byte)
        uint8_t reserved_align;                              ///< Alignment (1 byte)
        uint32_t migration_timestamp;                        ///< Migration time, s (4 bytes)
        char migration_reason[RBPOWER_MIGRATION_REASON_LEN]; ///< NUL-terminated reason, e.g. "ROLE_CHANGE" (32 bytes)
    } __attribute__((packed)) rbpower_unit_migration_t;      // Total: 2+1+1+4+32 = 40 bytes

    /**
     * @brief Unit metadata for file storage
     *
     * Contains information about unit, file configuration and change history.
     * Size exactly 256 bytes for convenient alignment.
     */
    typedef struct
    {
        // Section 1: Header (16 bytes)
        uint32_t magic;              ///< Must equal RBPOWER_UNIT_META_MAGIC (4)
        uint16_t version_major;      ///< Format version, major (2)
        uint16_t version_minor;      ///< Format version, minor (2)
        uint16_t version_patch;      ///< Format version, patch (2)
        uint16_t version_reserved;   ///< Format version, reserved (2)
        uint32_t creation_timestamp; ///< Creation time, s (set from esp_timer, i.e. seconds since boot) (4)

        // Section 2: Basic unit information (16 bytes)
        uint16_t unit_id;       ///< Unit ID (2)
        uint8_t unit_role;      ///< Unit role, a rbpower_load_role_t value (1)
        uint8_t unit_type;      ///< Unit metric type, a rbpower_metric_type_t value (1)
        uint32_t last_updated;  ///< Last update time, s (4)
        uint16_t unit_name_crc; ///< CRC of the unit name (not computed yet, 0) (2)
        uint16_t flags;         ///< Flags (reserved, 0) (2)
        uint32_t reserved_unit; ///< Reserved (4)

        // Section 3: File configuration (20 bytes)
        uint8_t files_config_data[20]; ///< File configuration (not filled yet, zeros) (20)

        // Section 4: Record sizes (16 bytes)
        uint16_t record_sizes[5];  ///< Transfer structure size per rbpower_data_type_t, bytes (10)
        uint8_t reserved_sizes[6]; ///< Reserved (6)

        // Section 5: Flags and counters (16 bytes)
        uint8_t is_special_unit;   ///< 1 = special unit (main or alternative supply) (1)
        uint8_t is_active;         ///< 1 = unit is active (1)
        uint8_t files_count;       ///< Number of data files of the unit (1)
        uint8_t reserved_flags1;   ///< Reserved (1)
        uint32_t migration_count;  ///< Number of migrations (4)
        uint16_t original_unit_id; ///< unit_id the metadata was created with (2)
        uint16_t reserved_flags2;  ///< Reserved (2)
        uint32_t total_size_bytes; ///< Estimated total size of the unit files, bytes (4)

        // Section 6: Migration history (40 bytes)
        uint16_t prev_unit_id;        ///< Previous unit_id (2)
        uint8_t prev_role;            ///< Previous role (1)
        uint8_t migration_reserved;   ///< Reserved (1)
        uint32_t migration_timestamp; ///< Migration time, s (4)
        char migration_reason[32];    ///< NUL-terminated migration reason (32)

        // Section 7: Usage statistics (32 bytes)
        uint64_t total_records_written;  ///< Total records written (8)
        uint64_t last_write_timestamp;   ///< Time of the last write, s (8)
        uint32_t write_errors_count;     ///< Write error counter (4)
        uint32_t recovery_count;         ///< Recovery counter (4)
        uint64_t estimated_next_cleanup; ///< Estimated time of the next cleanup, s (8)

        // Section 8: Directory path (76 bytes)
        char unit_directory_path[76]; ///< NUL-terminated unit directory path (76)

        // Section 9: Expansion reserve (16 bytes)
        uint8_t reserved_expansion[16]; ///< Reserved for future use (16)

        // Section 10: Integrity control (8 bytes)
        uint32_t metadata_crc32; ///< CRC32 of the metadata, computed with this field excluded (4)
        uint32_t reserved_crc;   ///< Reserved (4)

    } __attribute__((packed)) rbpower_unit_metadata_t;
    // Total: 16+16+20+16+16+40+32+76+16+8 = 256 bytes

    // Static size check
    _Static_assert(sizeof(rbpower_unit_metadata_t) == RBPOWER_UNIT_META_SIZE,
                   "Unit metadata must be exactly 256 bytes");

    /* ================================================================================================
     * SYSTEM GLOBAL INDEX
     * ================================================================================================ */

    /**
     * @brief Global index entry for each unit
     */
    typedef struct
    {
        rbpower_unit_id_t unit_id;  ///< Unit ID
        rbpower_load_role_t role;   ///< Unit role
        rbpower_metric_type_t type; ///< Metric type
        bool is_special_unit;       ///< true = special folder (main or alternative supply)
        uint8_t files_count;        ///< Number of data files of the unit
        uint16_t reserved;          ///< Alignment
        char unit_path[20];         ///< Unit folder name, e.g. "Unit_MainSupply" or "4401"
        uint64_t estimated_size;    ///< Estimated unit size, bytes
        uint32_t last_activity;     ///< Last activity time, s
        uint32_t reserved2;         ///< Reserved
    } rbpower_unit_index_entry_t;   // 48 bytes

    /**
     * @brief Storage system global index
     *
     * Contains general information about all units and their placement.
     */
    typedef struct
    {
        // Index header (32 bytes)
        uint32_t magic;               ///< Must equal RBPOWER_GLOBAL_INDEX_MAGIC
        rbpower_version_t version;    ///< Index format version (8 bytes)
        uint32_t creation_timestamp;  ///< Creation time, s
        uint32_t last_scan_timestamp; ///< Last scan time, s
        uint32_t total_units;         ///< Number of units in the system
        uint32_t active_units;        ///< Number of active units
        uint32_t special_units;       ///< Number of special units
        uint32_t reserved1;           ///< Alignment

        // Usage statistics (32 bytes)
        uint64_t total_estimated_size;                    ///< Estimated total size of all data, bytes
        uint64_t available_space;                         ///< Available space in the partition, bytes
        uint16_t units_by_role[RBPOWER_STORAGE_ROLE_MAX]; ///< Unit counters per role (24 bytes)

        // Units list (variable size)
        uint8_t units_count;                                 ///< Number of valid entries in units[]
        uint8_t reserved2[7];                                ///< Alignment
        rbpower_unit_index_entry_t units[RBPOWER_MAX_UNITS]; ///< Unit entries (up to RBPOWER_MAX_UNITS)

        // Integrity control
        uint32_t index_crc32; ///< CRC32 of the whole index, computed with this field excluded

    } rbpower_global_index_t;

    /* ================================================================================================
     * HELPER STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Data file information
     */
    typedef struct
    {
        rbpower_data_type_t data_type;     ///< Data type
        rbpower_period_type_t period_type; ///< Aggregation period
        char file_path[96];                ///< Full file path (NUL-terminated)
        uint32_t file_size;                ///< File size, bytes
        uint32_t records_count;            ///< Number of records in the ring buffer
        bool exists;                       ///< true = file exists
        bool is_valid;                     ///< true = header is valid
    } rbpower_file_info_t;

    /**
     * @brief Unit file structure creation result
     */
    typedef struct
    {
        rbpower_unit_id_t unit_id;                         ///< Processed unit ID
        uint8_t created_files;                             ///< Number of files created
        uint8_t existing_files;                            ///< Number of files that already existed
        uint8_t failed_files;                              ///< Number of files that could not be created
        bool metadata_created;                             ///< true = metadata file was created
        uint64_t total_size;                               ///< Total size of the created files, bytes
        esp_err_t result;                                  ///< Overall operation result
        char unit_directory[RBPOWER_STORAGE_MAX_DIR_NAME]; ///< Name of the created directory
    } rbpower_unit_creation_result_t;

    /* ================================================================================================
     * HEADER AND STRUCTURE FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Initialize a data file header
     *
     * Zeroes the header, sets magic, current format version, record size and
     * capacity for the given data type/period, and computes crc32_header.
     *
     * @param[out] header      Header to initialize
     * @param[in]  data_type   Data type
     * @param[in]  period_type Aggregation period
     * @retval ESP_OK              Header initialized
     * @retval ESP_ERR_INVALID_ARG header is NULL or data_type/period_type is out of range
     */
    esp_err_t rbpower_init_file_header(
        rbpower_file_header_t *header,
        rbpower_data_type_t data_type,
        rbpower_period_type_t period_type);

    /**
     * @brief Initialize unit metadata
     *
     * Zeroes the block, fills identity, record sizes, file count and size
     * estimate for the role, copies the directory path and computes metadata_crc32.
     *
     * @param[out] metadata       Metadata block to initialize
     * @param[in]  unit_id        Unit ID
     * @param[in]  unit_role      Unit role
     * @param[in]  unit_type      Unit metric type
     * @param[in]  directory_path Unit directory path (NUL-terminated, shorter than 76 bytes)
     * @retval ESP_OK              Metadata initialized
     * @retval ESP_ERR_INVALID_ARG metadata or directory_path is NULL, or the path is too long
     */
    esp_err_t rbpower_init_unit_metadata(
        rbpower_unit_metadata_t *metadata,
        rbpower_unit_id_t unit_id,
        rbpower_load_role_t unit_role,
        rbpower_metric_type_t unit_type,
        const char *directory_path);

    /**
     * @brief Calculate the file header CRC
     *
     * CRC32 (little-endian, seed 0) over the header with the crc32_header field excluded.
     *
     * @param[in] header Header
     * @return CRC32 value, or 0 if header is NULL
     */
    uint32_t rbpower_calculate_header_crc(const rbpower_file_header_t *header);

    /**
     * @brief Calculate the unit metadata CRC
     *
     * CRC32 (little-endian, seed 0) over the block with the metadata_crc32 field excluded.
     *
     * @param[in] metadata Metadata block
     * @return CRC32 value, or 0 if metadata is NULL
     */
    uint32_t rbpower_calculate_metadata_crc(const rbpower_unit_metadata_t *metadata);

    /**
     * @brief Validate a file header
     *
     * Checks magic, version compatibility, data/period type range, record size and
     * capacity against the current configuration, and the header CRC.
     *
     * @param[in] header Header
     * @return true if the header is valid; false otherwise or if header is NULL
     */
    bool rbpower_validate_file_header(const rbpower_file_header_t *header);

    /**
     * @brief Validate unit metadata
     *
     * Checks magic, version compatibility, role, a non-empty directory path and the CRC.
     *
     * @param[in] metadata Metadata block
     * @return true if the metadata is valid; false otherwise or if metadata is NULL
     */
    bool rbpower_validate_unit_metadata(const rbpower_unit_metadata_t *metadata);

    /**
     * @brief Get the full file size described by a header
     *
     * @param[in] header Header
     * @return File size in bytes (header + max_records * record_size), or 0 if
     *         header is NULL or fails validation
     */
    uint64_t rbpower_get_file_size_from_header(const rbpower_file_header_t *header);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_STRUCTURES_H