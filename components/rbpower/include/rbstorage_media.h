/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_media.h
 * @brief Storage media routing: INTERNAL flash is the primary stats/config store;
 *        the external NOR chip is an optional alternative medium.
 *
 * Only one medium is active at a time.
 *
 * The statistics ring layer (src/rbextflash_operations.c) was written against the external
 * chip's own layout: absolute offsets with slots at PARTITION_STATS_OFFSET + slot*208KB. That
 * validated logic is NOT rewritten. Instead, all its physical IO is routed through these shims:
 *   - INTERNAL mode (default): the same absolute offsets are translated to the internal `stats`
 *     partition (abs - PARTITION_STATS_OFFSET) and served via esp_partition_* on the main chip.
 *   - EXTERNAL mode (optional): pass-through to esp_flash_* on the external handle.
 * The reported "stats size" keeps the external-layout convention (offset + region size) so the
 * slot allocator's math (slots = stats_size / 208KB) is unchanged.
 *
 * Mode selection (rbmedia_init_internal() / rbmedia_attach_external()) is done once at boot by
 * the storage init code; the IO shims hold no lock and rely on the callers' own serialization.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Active statistics storage medium. */
typedef enum {
    RBMEDIA_NONE = 0,       ///< Not initialized: storage unavailable (RAM-only)
    RBMEDIA_INTERNAL,       ///< Internal flash: `stats` partition + `config` partition littlefs
    RBMEDIA_EXTERNAL,       ///< External NOR chip (optional)
} rbmedia_mode_t;

/**
 * @brief Find the internal `stats` partition and select INTERNAL mode.
 *
 * Idempotent: returns ESP_OK immediately if INTERNAL mode is already selected.
 *
 * @retval ESP_OK             INTERNAL mode selected
 * @retval ESP_ERR_NOT_FOUND  No `stats` data partition in the partition table (storage stays RAM-only)
 */
esp_err_t rbmedia_init_internal(void);

/**
 * @brief Select the external NOR chip as the statistics medium.
 *
 * @param[in] handle  Initialized esp_flash handle of the external chip
 *
 * @retval ESP_OK               EXTERNAL mode selected
 * @retval ESP_ERR_INVALID_ARG  @p handle is NULL
 */
esp_err_t rbmedia_attach_external(esp_flash_t *handle);

/**
 * @brief Get the currently selected medium.
 *
 * @return Current mode (RBMEDIA_NONE before initialization).
 */
rbmedia_mode_t rbmedia_mode(void);

/**
 * @brief Size of the stats region in the EXTERNAL-layout convention.
 *
 * The ring layer computes the slot count as region_size / 208KB and the bounds as
 * PARTITION_STATS_OFFSET + size.
 *
 * @return Region size in bytes: the internal `stats` partition size (INTERNAL),
 *         PARTITION_STATS_SIZE (EXTERNAL; the caller may override it with the chip size),
 *         or 0 when no medium is selected.
 */
uint32_t rbmedia_stats_region_size(void);

/**
 * @brief Read from the stats region (physical IO).
 *
 * @param[in]  abs_off  EXTERNAL-layout absolute byte offset (>= PARTITION_STATS_OFFSET)
 * @param[out] dst      Destination buffer, at least @p len bytes
 * @param[in]  len      Number of bytes to read
 *
 * @retval ESP_OK                 Data read
 * @retval ESP_ERR_INVALID_STATE  No medium selected
 * @retval ESP_ERR_INVALID_ARG    @p abs_off below PARTITION_STATS_OFFSET (INTERNAL mode)
 * @retval ESP_ERR_INVALID_SIZE   Range exceeds the `stats` partition (INTERNAL mode)
 * @return Other codes from esp_partition_read() / esp_flash_read().
 */
esp_err_t rbmedia_stats_read(uint32_t abs_off, void *dst, size_t len);

/**
 * @brief Write to the stats region (physical IO; the range must be erased first).
 *
 * @param[in] abs_off  EXTERNAL-layout absolute byte offset (>= PARTITION_STATS_OFFSET)
 * @param[in] src      Source buffer, at least @p len bytes
 * @param[in] len      Number of bytes to write
 *
 * @retval ESP_OK                 Data written
 * @retval ESP_ERR_INVALID_STATE  No medium selected
 * @retval ESP_ERR_INVALID_ARG    @p abs_off below PARTITION_STATS_OFFSET (INTERNAL mode)
 * @retval ESP_ERR_INVALID_SIZE   Range exceeds the `stats` partition (INTERNAL mode)
 * @return Other codes from esp_partition_write() / esp_flash_write().
 */
esp_err_t rbmedia_stats_write(uint32_t abs_off, const void *src, size_t len);

/**
 * @brief Erase a range of the stats region (physical IO).
 *
 * @param[in] abs_off  EXTERNAL-layout absolute byte offset, sector-aligned (4096 bytes)
 * @param[in] len      Number of bytes to erase, a multiple of the sector size
 *
 * @retval ESP_OK                 Range erased
 * @retval ESP_ERR_INVALID_STATE  No medium selected
 * @retval ESP_ERR_INVALID_ARG    @p abs_off below PARTITION_STATS_OFFSET (INTERNAL mode)
 * @retval ESP_ERR_INVALID_SIZE   Range exceeds the `stats` partition (INTERNAL mode)
 * @return Other codes from esp_partition_erase_range() / esp_flash_erase_region().
 */
esp_err_t rbmedia_stats_erase(uint32_t abs_off, size_t len);

/**
 * @brief Non-NULL token for legacy `if (!handle)` guards in the ring layer.
 *
 * The shims above never dereference it in INTERNAL mode.
 *
 * @return The external handle (EXTERNAL), esp_flash_default_chip as a token only (INTERNAL),
 *         or NULL when no medium is selected.
 */
esp_flash_t *rbmedia_stats_handle_token(void);

#ifdef __cplusplus
}
#endif
