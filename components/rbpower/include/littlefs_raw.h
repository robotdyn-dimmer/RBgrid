/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file littlefs_raw.h
 * @brief Raw LittleFS on the config partition (internal flash, or the external NOR chip).
 *
 * A small LittleFS wrapper driven directly by flash callbacks, without the ESP-IDF VFS. It holds
 * the hub's configuration and metadata files. The filesystem lives on the internal `config`
 * partition (littlefs_raw_mount_internal(), the default) or on the config region of the external
 * NOR chip (littlefs_raw_mount()). Writes are atomic and power-loss safe (temp file + rename,
 * with a `.bak` copy of the previous version and a `.crc` sidecar); reads fall back to the `.bak`
 * copy when the primary file is missing or fails its CRC check.
 *
 * Thread safety: all file operations take an internal mutex and may be called from any task
 * (not from an ISR). Mount and unmount are called once from the init task.
 */

#ifndef LITTLEFS_RAW_H
#define LITTLEFS_RAW_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_flash.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Mount the raw LittleFS on the external NOR chip's config region (formats it if the mount fails).
     *
     * @param[in] flash_handle esp_flash handle of the external chip (must not be NULL).
     * @retval ESP_OK              Mounted (or already mounted).
     * @retval ESP_ERR_INVALID_ARG flash_handle is NULL.
     * @retval ESP_ERR_NO_MEM      Mutex could not be created.
     * @retval ESP_FAIL            Format or mount after format failed.
     */
    esp_err_t littlefs_raw_mount(esp_flash_t *flash_handle);

    /**
     * @brief Mount the raw LittleFS on the internal `config` partition (the default medium).
     *
     * The external NOR chip is an optional alternative medium; see littlefs_raw_mount().
     * @retval ESP_OK            Mounted (or already mounted).
     * @retval ESP_ERR_NOT_FOUND No internal `config` partition in the partition table.
     * @retval other             Same as littlefs_raw_mount().
     */
    esp_err_t littlefs_raw_mount_internal(void);

    /**
     * @brief Unmount the raw LittleFS.
     *
     * @retval ESP_OK   Unmounted, or was not mounted.
     * @retval ESP_FAIL lfs_unmount() failed.
     */
    esp_err_t littlefs_raw_unmount(void);

    /**
     * @brief Write a whole file atomically (power-loss safe), replacing its previous content.
     *
     * The data goes to `<path>.tmp`, the current file is kept as `<path>.bak`, then the temp file is
     * renamed to `<path>` and a `<path>.crc` sidecar is written. If the temp write fails, the
     * original file is left untouched.
     *
     * @param[in] path File path; at most 59 characters (it must still fit a 64-byte buffer with a 4-character suffix).
     * @param[in] data Data to write.
     * @param[in] size Data size, bytes.
     * @retval ESP_OK              Data committed.
     * @retval ESP_ERR_INVALID_ARG Not mounted, NULL argument, or path too long.
     * @retval ESP_FAIL            Temp write or commit rename failed.
     */
    esp_err_t littlefs_raw_write(const char *path, const void *data, size_t size);

    /**
     * @brief Read a whole file, falling back to the `.bak` copy if the primary is missing, empty or corrupt.
     *
     * @param[in]  path        File path.
     * @param[out] data        Buffer to read into.
     * @param[in]  max_size    Buffer size, bytes; longer files are truncated.
     * @param[out] actual_size Number of bytes read (may be NULL).
     * @retval ESP_OK              Primary or backup copy read and verified.
     * @retval ESP_ERR_INVALID_ARG Not mounted or NULL path/data.
     * @retval ESP_ERR_NOT_FOUND   Neither a valid primary nor a valid backup exists.
     */
    esp_err_t littlefs_raw_read(const char *path, void *data, size_t max_size, size_t *actual_size);

    /**
     * @brief Delete a file and its atomic-write companions (`.bak`, `.crc`, `.bak.crc`, `.tmp`).
     *
     * @param[in] path File path to delete.
     * @retval ESP_OK              Deleted.
     * @retval ESP_ERR_INVALID_ARG Not mounted or NULL path.
     * @retval ESP_FAIL            The primary file could not be removed.
     */
    esp_err_t littlefs_raw_remove(const char *path);

    /**
     * @brief Check whether a file exists (a surviving `.bak` copy also counts, since reads recover from it).
     *
     * @param[in] path File path.
     * @return true if the file or its `.bak` copy exists; false if not, if not mounted, or if path is NULL.
     */
    bool littlefs_raw_exists(const char *path);

    /**
     * @brief Create a directory (an existing directory is not an error).
     *
     * @param[in] path Directory path.
     * @retval ESP_OK              Created or already exists.
     * @retval ESP_ERR_INVALID_ARG Not mounted or NULL path.
     * @retval ESP_FAIL            lfs_mkdir() failed.
     */
    esp_err_t littlefs_raw_mkdir(const char *path);

    /**
     * @brief Get filesystem usage in blocks (4 KB each).
     *
     * @param[out] total_blocks Total number of blocks (may be NULL).
     * @param[out] used_blocks  Used blocks (may be NULL).
     * @retval ESP_OK                Success.
     * @retval ESP_ERR_INVALID_STATE Not mounted.
     * @retval ESP_FAIL              lfs_fs_size() failed.
     */
    esp_err_t littlefs_raw_info(size_t *total_blocks, size_t *used_blocks);

#ifdef __cplusplus
}
#endif

#endif // LITTLEFS_RAW_H