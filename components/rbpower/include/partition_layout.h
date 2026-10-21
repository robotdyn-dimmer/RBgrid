/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file partition_layout.h
 * @brief Layout of the OPTIONAL external NOR flash chip (statistics store) — offsets, sizes and
 *        the on-chip partition-entry format.
 *
 * This is NOT the ESP32 internal partition table (that one lives in partitions.csv). The external
 * W25Q-series chip carries its own mini partition scheme, described here; rbstorage_media selects
 * between the internal `stats` partition and this external chip at runtime (media adoption).
 */
#ifndef PARTITION_LAYOUT_H
#define PARTITION_LAYOUT_H

/** Magic number of an on-chip partition entry ("PART"). */
#define PARTITION_TABLE_MAGIC 0x50415254

/*
 * External-flash partition layout (sized for 64-unit statistics).
 *
 * One unit's statistics = 50 data sectors + 1 header ≈ 208 KB (matrix RBPOWER_BUFFER_SECTORS).
 * 64 units need about 13 MB of STATS, so:
 *   - There is no external "ota" partition: OTA images are staged in the internal flash, and the
 *     space is used for STATS instead.
 *   - "log" (process/diagnostic event log, a serial-port replacement, not measurement data) sits at 0x45000.
 *   - STATS starts at 0x65000 and extends to the END OF THE CHIP. Its size is set at runtime from
 *     the detected flash size (external_flash_init → esp_flash_get_size); PARTITION_STATS_SIZE is only
 *     a 4 MB fallback. One firmware therefore serves a 4 MB chip (~17 units) and a 16 MB chip
 *     (64 units); the live slot count = min(64, STATS size / 208 KB).
 * config stays at 0x5000, so /stats_fmt and flash_slots.json survive layout changes.
 */
#define PARTITION_TABLE_OFFSET 0x000000 ///< Partition table offset, bytes (4 KB area)
#define PARTITION_TABLE_SIZE 0x001000   ///< Partition table size, bytes

#define PARTITION_NVS_OFFSET 0x001000 ///< NVS partition offset, bytes (16 KB area)
#define PARTITION_NVS_SIZE 0x004000   ///< NVS partition size, bytes

#define PARTITION_CONFIG_OFFSET 0x005000 ///< Config partition (LittleFS, 256 KB) offset, bytes; fixed across layouts
#define PARTITION_CONFIG_SIZE 0x040000   ///< Config partition size, bytes

#define PARTITION_LOG_OFFSET 0x045000 ///< Process/diagnostic log (128 KB, serial-port replacement) offset, bytes
#define PARTITION_LOG_SIZE 0x020000   ///< Log partition size, bytes

#define PARTITION_STATS_OFFSET 0x065000 ///< Statistics partition offset, bytes; extends to the end of the chip
#define PARTITION_STATS_SIZE 0x39B000   ///< 4 MB-chip fallback size, bytes (0x400000 - 0x65000); overridden at runtime from the detected flash size

/**
 * @brief One partition entry as stored on the external chip (packed, 32 bytes).
 */
typedef struct
{
    uint32_t magic;  /**< ::PARTITION_TABLE_MAGIC ("PART") */
    uint8_t type;    /**< ::partition_type */
    uint8_t subtype; /**< ::partition_subtype_data */
    uint16_t flags;  /**< reserved, 0 */
    uint32_t offset; /**< partition start, bytes from chip start */
    uint32_t size;   /**< partition size, bytes */
    char label[16];  /**< NUL-padded partition name */
} __attribute__((packed)) partition_entry_t;

/** @brief Partition types (partition_entry_t::type). */
enum partition_type
{
    PARTITION_TYPE_DATA = 0x01, ///< Data partition
    PARTITION_TYPE_APP = 0x00,  ///< Application image partition (not used on the external chip)
};

/** @brief Data partition subtypes (partition_entry_t::subtype). */
enum partition_subtype_data
{
    PARTITION_SUBTYPE_NVS = 0x02,    ///< NVS key/value store
    PARTITION_SUBTYPE_CONFIG = 0x82, ///< Custom: configuration (LittleFS)
    PARTITION_SUBTYPE_OTA = 0x00,    ///< OTA staging (no longer allocated on the external chip)
    PARTITION_SUBTYPE_LOG = 0x83,    ///< Custom: process/diagnostic log storage
    PARTITION_SUBTYPE_STATS = 0x84,  ///< Custom: statistics
};

#endif // PARTITION_LAYOUT_H