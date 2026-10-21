/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_structures.h
 * @brief Constants of the raw-flash statistics store
 *
 * The statistics history is kept in ring buffers written directly through the esp_flash API
 * (no filesystem), append-only to minimise wear, on the internal statistics partition or on the
 * optional external NOR chip. Ring-buffer state is kept in per-sector markers on flash (see
 * rbextflash_sector_metadata.h).
 *
 * Definitions: the flash geometry (#RBPOWER_FLASH_SECTOR_SIZE), the on-flash record format
 * version (#RBPOWER_RECORD_FORMAT_VERSION, #RBPOWER_RECORD_FORMAT_MIN_COMPATIBLE), the slot-table
 * size (#RBPOWER_MAX_UNITS) and ::rbpower_unit_id_t.
 */

#ifndef RBPOWER_FLASH_STRUCTURES_H
#define RBPOWER_FLASH_STRUCTURES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rbpower_enums.h"
#include "rbstorage_transfer_types.h"

/* ================================================================================================
 * CONSTANTS
 * ================================================================================================ */

/** @brief Flash erase-sector size, bytes (4 KB). */
#define RBPOWER_FLASH_SECTOR_SIZE 4096 ///< Flash sector size (4KB)

/**
 * @brief On-flash STATISTICS RECORD format version.
 *
 * Bump whenever the persisted record layout changes (size or field meaning). At boot
 * rbpower_flash_operations_init() compares this to the version stored in the config-partition
 * marker; on mismatch it AUTO-MIGRATES by blind-wiping the STATS region (old records are
 * incompatible: reading them with a new reader hard-faults) and writing the new version.
 *
 * - v1: original 20-byte electrical record.
 * - v2: + min/max envelope (32 bytes).
 * - v3: slot offsets became STATS-relative and the STATS region moved to 0x65000, sized to the
 *   chip end. The data LOCATION changed, so the bump auto-wipes the wrongly placed old data.
 * - v4 (permanent; validated by migrating a real v3 store byte-exact): 11-byte common header
 *   (ts/dur/q/res) + int64 mWh energy + int64 cents economics + lifetime totalizers per record.
 *   A v3 store found at boot is migrated IN PLACE by the region migrator
 *   (rbstats_migrate_v3v4.c, two-phase per slot) after the running image is confirmed valid
 *   (OTA rollback window). The former -DRBGRID_FORMAT_V4 staging flag is a no-op.
 */
#define RBPOWER_RECORD_FORMAT_VERSION 4

/**
 * @brief Oldest on-flash format the CURRENT reader can still read WITHOUT a wipe
 *        (semver-style compatibility window).
 *
 * At boot, a stored version in [MIN_COMPATIBLE, RECORD_FORMAT_VERSION] is a backward-compatible
 * upgrade: migration is NON-DESTRUCTIVE (history preserved, marker restamped). Anything outside
 * (older than MIN_COMPATIBLE, or a downgrade) is incompatible and blind-wiped.
 *
 * CONTRACT for staying backward-compatible (so RECORD_FORMAT_VERSION can be bumped WITHOUT
 * bumping MIN_COMPATIBLE, i.e. without history loss): EXISTING record types
 * (electrical/energy/economics) are IMMUTABLE; never resize or repurpose their fields. Add
 * capability by introducing a NEW record TYPE (its ring buffer simply starts empty on old
 * devices); the existing readers keep reading old records at their unchanged stride. Set
 * MIN_COMPATIBLE = RECORD_FORMAT_VERSION ONLY for a genuinely breaking change (resizing or
 * repurposing an existing record type); that, and only that, costs the history wipe.
 * (For example, 4-flow economics must be a NEW record type, not an inline expansion of the
 * economics record; see documents/architecture/STEP6_4FLOW_ECONOMICS_DESIGN.md §6/§7.)
 *
 * v4 is a breaking relayout versus v3: v3 stores go through the in-place migrator, not the
 * compatibility window.
 */
#define RBPOWER_RECORD_FORMAT_MIN_COMPATIBLE 4   /* v4 is a breaking relayout vs v3 — v3 stores go
                                                    through the in-place migrator, not the compat window */

/**
 * @brief Maximum number of units (flash slot-table size).
 *
 * Matches RBPOWER_MAX_HOME_UNITS (PSRAM platform) and the definition in rbstorage_structures.h.
 * How many slots actually fit is bounded at runtime by the chip's STATS region
 * (g_max_usable_slots).
 */
#define RBPOWER_MAX_UNITS 64           ///< Maximum units in system

    /* ================================================================================================
     * BASIC TYPES
     * ================================================================================================ */

    /**
     * @brief Unit ID (16-bit)
     */
    typedef uint16_t rbpower_unit_id_t;

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_FLASH_STRUCTURES_H
