/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_sector_metadata.h
 * @brief Per-sector markers of the flash statistics ring buffers
 *
 * The ring-buffer state is kept on flash itself, not in NVS:
 * - a 16-byte service area (::sector_metadata_t) at the end of each 4096-byte sector;
 * - state markers (ACTIVE/CLOSED) programmed in place (NOR can only clear bits, 1 -> 0);
 * - at boot the state is recovered by scanning the sector markers;
 * - the runtime state is kept in RAM.
 *
 * The helpers are inline and pure; they do not access flash.
 */

#ifndef RBPOWER_FLASH_SECTOR_METADATA_H
#define RBPOWER_FLASH_SECTOR_METADATA_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"

/* ================================================================================================
 * CONSTANTS
 * ================================================================================================ */

// Sector sizes
#define RBPOWER_FLASH_SECTOR_SIZE 4096                                            ///< Flash sector size (4KB)
#define SECTOR_METADATA_SIZE 16                                                   ///< Metadata size at end of sector
#define SECTOR_METADATA_OFFSET (RBPOWER_FLASH_SECTOR_SIZE - SECTOR_METADATA_SIZE) ///< 4080
#define SECTOR_DATA_SIZE (RBPOWER_FLASH_SECTOR_SIZE - SECTOR_METADATA_SIZE)       ///< 4080

// Sector state flags
#define SECTOR_FLAG_ACTIVE 0xFF    ///< Sector active (can write)
#define SECTOR_FLAG_CLOSED 0x00    ///< Sector closed (full)
#define SECTOR_FLAG_ERASED 0xFE    ///< Sector erased, not used
#define SECTOR_FLAG_CORRUPTED 0x01 ///< Sector corrupted (CRC error)

// Magic number for initialization
#define SECTOR_INIT_MAGIC 0xA5 ///< Marker of initialized sector

/** @brief Special sector index value meaning "not found". */
#define SECTOR_NOT_FOUND 0xFFFF

    /* ================================================================================================
     * STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Sector metadata (16 bytes)
     *
     * Located at the end of each sector (offset 4080).
     * Used for restoring buffer state at startup.
     *
     * Field structure:
     * - last_write_timestamp (4 bytes): Unix timestamp (epoch s) of last write
     * - crc16 (2 bytes): CRC16 of sector data area
     * - flags (1 byte): State flags (ACTIVE/CLOSED/ERASED/CORRUPTED)
     * - init_magic (1 byte): Initialization marker (0xA5 = initialized, 0xFF = virgin)
     * - sequence_number (4 bytes): Monotonic counter for sector ordering
     * - reserved (4 bytes): Reserved for future extensions
     */
    typedef struct
    {
        uint32_t last_write_timestamp; ///< Unix timestamp (epoch s) of last write to sector; 0xFFFFFFFF while ACTIVE
        uint16_t crc16;                ///< CRC16 of sector data area (bytes 0-4079); 0xFFFF while ACTIVE, 0 = "no CRC" (older sectors)
        uint8_t flags;                 ///< State flags (SECTOR_FLAG_*)
        uint8_t init_magic;            ///< Initialization marker (0xA5 = initialized)
        uint32_t sequence_number;      ///< Monotonic counter (incremented when sector is created)
        uint8_t reserved[4];           ///< Reserved for future extensions
    } __attribute__((packed)) sector_metadata_t;

    _Static_assert(sizeof(sector_metadata_t) == SECTOR_METADATA_SIZE, "Sector metadata must be 16 bytes");

    /* ================================================================================================
     * HELPER FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Check if sector is active
     *
     * Sector is considered active if:
     * - Flag == SECTOR_FLAG_ACTIVE (0xFF)
     * - init_magic == SECTOR_INIT_MAGIC (0xA5)
     *
     * This distinguishes initialized active sector from virgin Flash,
     * where all bytes = 0xFF.
     *
     * @param[in] meta Pointer to sector metadata (not NULL)
     * @return true if sector is active and initialized
     */
    static inline bool sector_is_active(const sector_metadata_t *meta)
    {
        return (meta->flags == SECTOR_FLAG_ACTIVE &&
                meta->init_magic == SECTOR_INIT_MAGIC);
    }

    /**
     * @brief Check if sector is closed
     *
     * @param[in] meta Pointer to sector metadata (not NULL)
     * @return true if sector is closed (flag == CLOSED)
     */
    static inline bool sector_is_closed(const sector_metadata_t *meta)
    {
        return (meta->flags == SECTOR_FLAG_CLOSED);
    }

    /**
     * @brief Check if sector is corrupted
     *
     * @param[in] meta Pointer to sector metadata (not NULL)
     * @return true if sector is corrupted (flag == CORRUPTED)
     */
    static inline bool sector_is_corrupted(const sector_metadata_t *meta)
    {
        return (meta->flags == SECTOR_FLAG_CORRUPTED);
    }

    /**
     * @brief Check if sector is virgin (never initialized)
     *
     * Virgin Flash has all bytes = 0xFF, including metadata.
     * But this looks like flags=0xFF (ACTIVE)!
     *
     * Difference: virgin Flash has init_magic = 0xFF, initialized has = 0xA5
     * (the timestamp and CRC are also checked to be erased).
     *
     * @param[in] meta Pointer to sector metadata (not NULL)
     * @return true if sector is virgin (not initialized)
     */
    static inline bool sector_is_virgin(const sector_metadata_t *meta)
    {
        return (meta->flags == SECTOR_FLAG_ACTIVE &&
                meta->init_magic == 0xFF &&
                meta->last_write_timestamp == 0xFFFFFFFF &&
                meta->crc16 == 0xFFFF);
    }

    /**
     * @brief Initialize active sector metadata
     *
     * Sets:
     * - flags = SECTOR_FLAG_ACTIVE (0xFF)
     * - init_magic = SECTOR_INIT_MAGIC (0xA5) - initialization marker
     * - sequence_number = passed value (monotonically increases)
     * - timestamp = 0xFFFFFFFF (erased; programmed when the sector is closed)
     * - crc16 = 0xFFFF (erased; programmed when the sector is closed)
     * - reserved = 0xFF (erased)
     *
     * @param[out] meta Pointer to metadata to initialize (not NULL)
     * @param[in] seq_num Sequence number for this sector
     */
    static inline void sector_metadata_init_active(sector_metadata_t *meta, uint32_t seq_num)
    {
        /* timestamp / crc16 / reserved stay ERASED (0xFF): close_sector programs them later, and NOR can only
         * clear bits. If they were written as 0 here, the real CRC could never be programmed, every CLOSED
         * sector would fail its CRC check and real corruption would be undetectable. Sectors written by older
         * firmware have crc16 == 0, which validate_sector_metadata treats as "no CRC". */
        meta->last_write_timestamp = 0xFFFFFFFFu;
        meta->crc16 = 0xFFFF;
        meta->flags = SECTOR_FLAG_ACTIVE;
        meta->init_magic = SECTOR_INIT_MAGIC;
        meta->sequence_number = seq_num;
        memset(meta->reserved, 0xFF, sizeof(meta->reserved));
    }

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_FLASH_SECTOR_METADATA_H
