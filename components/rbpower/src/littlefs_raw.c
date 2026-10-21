/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file littlefs_raw.c
 * @brief Raw LittleFS implementation on the config partition (internal flash or external NOR).
 *
 * Drives LittleFS directly through custom block-device callbacks: esp_partition_* on the internal
 * `config` partition, or esp_flash_* on the external chip. See littlefs_raw.h.
 */

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"   // esp_rom_crc32_le — config-file integrity sidecar
#include "littlefs/lfs.h"
#include "external_flash_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "LITTLEFS_RAW";

// LittleFS instance and configuration
static lfs_t g_lfs;
static struct lfs_config g_cfg;
static esp_flash_t *g_ext_flash = NULL;
/* INTERNAL mode (the default medium): littlefs lives on the internal `config` partition;
 * when s_int_part is set the callbacks below use esp_partition_* with partition-relative
 * addresses instead of esp_flash_* on the external chip. */
static const esp_partition_t *s_int_part = NULL;
static bool g_mounted = false;
static SemaphoreHandle_t g_mutex = NULL;

// Buffer pools for LittleFS
static uint8_t g_read_buffer[256];
static uint8_t g_prog_buffer[256];
static uint8_t g_lookahead_buffer[16];

// Configuration partition parameters from external_flash_config.h
#define LFS_PARTITION_OFFSET PARTITION_CONFIG_OFFSET ///< Config region offset on the external chip, bytes
#define LFS_PARTITION_SIZE   PARTITION_CONFIG_SIZE   ///< Config region size on the external chip, bytes
#define LFS_BLOCK_SIZE       4096  ///< LittleFS block size, bytes (one 4 KB erase sector)

/**
 * @brief Read callback for LittleFS
 */
static int lfs_external_read(const struct lfs_config *c, lfs_block_t block,
                             lfs_off_t off, void *buffer, lfs_size_t size)
{
    if (s_int_part) {   /* INTERNAL: partition-relative */
        if (esp_partition_read(s_int_part, (block * c->block_size) + off, buffer, size) != ESP_OK)
            return LFS_ERR_IO;
        return 0;
    }
    uint32_t addr = LFS_PARTITION_OFFSET + (block * c->block_size) + off;

    esp_err_t err = esp_flash_read(g_ext_flash, buffer, addr, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash read failed at 0x%08lx: %s", (unsigned long)addr, esp_err_to_name(err));
        return LFS_ERR_IO;
    }
    
    return 0;
}

/**
 * @brief Program/Write callback for LittleFS
 */
static int lfs_external_prog(const struct lfs_config *c, lfs_block_t block,
                            lfs_off_t off, const void *buffer, lfs_size_t size)
{
    if (s_int_part) {   /* INTERNAL: partition-relative */
        if (esp_partition_write(s_int_part, (block * c->block_size) + off, buffer, size) != ESP_OK)
            return LFS_ERR_IO;
        return 0;
    }
    uint32_t addr = LFS_PARTITION_OFFSET + (block * c->block_size) + off;

    esp_err_t err = esp_flash_write(g_ext_flash, buffer, addr, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash write failed at 0x%08lx: %s", (unsigned long)addr, esp_err_to_name(err));
        return LFS_ERR_IO;
    }
    
    return 0;
}

/**
 * @brief Erase callback for LittleFS
 */
static int lfs_external_erase(const struct lfs_config *c, lfs_block_t block)
{
    if (s_int_part) {   /* INTERNAL: partition-relative */
        if (esp_partition_erase_range(s_int_part, block * c->block_size, c->block_size) != ESP_OK)
            return LFS_ERR_IO;
        return 0;
    }
    uint32_t addr = LFS_PARTITION_OFFSET + (block * c->block_size);

    esp_err_t err = esp_flash_erase_region(g_ext_flash, addr, c->block_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash erase failed at 0x%08lx: %s", (unsigned long)addr, esp_err_to_name(err));
        return LFS_ERR_IO;
    }
    
    return 0;
}

/**
 * @brief Sync callback for LittleFS (no-op for Flash)
 */
static int lfs_external_sync(const struct lfs_config *c)
{
    // Flash doesn't require sync
    return 0;
}

/* Initialize and mount raw LittleFS on the external chip (documented in littlefs_raw.h). */
esp_err_t littlefs_raw_mount(esp_flash_t *flash_handle)
{
    if (!flash_handle) {
        ESP_LOGE(TAG, "Invalid flash handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_mounted) {
        ESP_LOGW(TAG, "Already mounted");
        return ESP_OK;
    }
    
    g_ext_flash = flash_handle;
    
    // Create mutex for thread safety
    if (!g_mutex) {
        g_mutex = xSemaphoreCreateMutex();
        if (!g_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Configure LittleFS
    g_cfg = (struct lfs_config){
        // Block device operations
        .read  = lfs_external_read,
        .prog  = lfs_external_prog,
        .erase = lfs_external_erase,
        .sync  = lfs_external_sync,
        
        // Block device configuration
        .block_size = LFS_BLOCK_SIZE,
        .block_count = (s_int_part ? s_int_part->size : LFS_PARTITION_SIZE) / LFS_BLOCK_SIZE,
        .block_cycles = 500,  // Wear leveling
        
        // Performance tuning
        .read_size = 256,
        .prog_size = 256,
        .cache_size = 256,
        .lookahead_size = 16,
        
        // Buffers
        .read_buffer = g_read_buffer,
        .prog_buffer = g_prog_buffer,
        .lookahead_buffer = g_lookahead_buffer,
        
        // Optional configuration
        .name_max = 0,  // Use default
        .file_max = 0,  // Use default
        .attr_max = 0,  // Use default
    };
    
    ESP_LOGI(TAG, "Mounting LittleFS on %s (blocks: %lu)",
             s_int_part ? "INTERNAL `config` partition" : "EXTERNAL flash config region",
             (unsigned long)g_cfg.block_count);
    
    // Try to mount
    int err = lfs_mount(&g_lfs, &g_cfg);
    if (err) {
        ESP_LOGW(TAG, "Mount failed, formatting...");
        
        // Format the filesystem
        err = lfs_format(&g_lfs, &g_cfg);
        if (err) {
            ESP_LOGE(TAG, "Format failed: %d", err);
            return ESP_FAIL;
        }
        
        // Try to mount again
        err = lfs_mount(&g_lfs, &g_cfg);
        if (err) {
            ESP_LOGE(TAG, "Mount after format failed: %d", err);
            return ESP_FAIL;
        }
    }
    
    g_mounted = true;
    ESP_LOGI(TAG, "LittleFS mounted successfully");
    
    // Get filesystem info
    struct lfs_info info;
    err = lfs_stat(&g_lfs, "/", &info);
    if (err == 0) {
        ESP_LOGI(TAG, "Root directory exists");
    }
    
    return ESP_OK;
}

/* Mount raw LittleFS on the INTERNAL `config` partition (the default medium; documented in littlefs_raw.h).
 * The external-handle argument of littlefs_raw_mount() becomes a non-NULL token only. */
esp_err_t littlefs_raw_mount_internal(void)
{
    s_int_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, "config");
    if (!s_int_part) {
        ESP_LOGE(TAG, "internal `config` partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    return littlefs_raw_mount(esp_flash_default_chip /* token, unused in internal callbacks */);
}

/* Unmount raw LittleFS (documented in littlefs_raw.h). */
esp_err_t littlefs_raw_unmount(void)
{
    if (!g_mounted) {
        ESP_LOGW(TAG, "Not mounted");
        return ESP_OK;
    }
    
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    
    int err = lfs_unmount(&g_lfs);
    if (err) {
        ESP_LOGE(TAG, "Unmount failed: %d", err);
        xSemaphoreGive(g_mutex);
        return ESP_FAIL;
    }
    
    g_mounted = false;
    g_ext_flash = NULL;
    
    xSemaphoreGive(g_mutex);
    
    ESP_LOGI(TAG, "LittleFS unmounted");
    return ESP_OK;
}

/* Write data to `path`, fully sync to flash, close. Assumes g_mutex is HELD by the caller.
 * Truncates/creates. Returns ESP_OK only on a complete, synced write. */
static esp_err_t lfs_write_synced_locked(const char *path, const void *data, size_t size)
{
    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) {
        ESP_LOGE(TAG, "Failed to open file %s: %d", path, err);
        return ESP_FAIL;
    }
    lfs_ssize_t written = lfs_file_write(&g_lfs, &file, data, size);
    if (written < 0 || (size_t)written != size) {
        ESP_LOGE(TAG, "%s write failed: %ld/%zu", path, (long)written, size);
        lfs_file_close(&g_lfs, &file);
        return ESP_FAIL;
    }
    err = lfs_file_sync(&g_lfs, &file);
    if (err) ESP_LOGW(TAG, "File sync failed: %d (continuing)", err);
    lfs_file_close(&g_lfs, &file);
    return ESP_OK;
}

/* Build "<path><suffix>" into out (suffix e.g. ".tmp"/".bak"/".crc"). Returns false if it won't fit. */
static bool lfs_suffix_path(char *out, size_t out_sz, const char *path, const char *suffix)
{
    int n = snprintf(out, out_sz, "%s%s", path, suffix);
    return (n > 0 && (size_t)n < out_sz);
}

/* ATOMIC config write with previous-version backup + CRC sidecar (power-loss safe;
 * documented in littlefs_raw.h).
 *
 * Sequence under the FS mutex: write the new content to `<path>.tmp` (full + synced) → preserve the
 * current `<path>` as `<path>.bak` (best-effort) → ATOMICALLY rename `<path>.tmp`→`<path>` (littlefs
 * rename is itself power-loss atomic) → write a `<path>.crc` sidecar (CRC32 of the data, best-effort).
 * A power loss can therefore NEVER leave a torn/half-written `<path>`: at worst `<path>` is briefly
 * absent between the two renames, and littlefs_raw_read() recovers it from `<path>.bak`. Fully
 * backward compatible (a missing .crc/.bak just means "legacy" and is accepted). On ANY tmp-write
 * failure the original `<path>` is left untouched.
 */
esp_err_t littlefs_raw_write(const char *path, const void *data, size_t size)
{
    if (!g_mounted || !path || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    char tmp[64], bak[64], crcp[64];
    if (!lfs_suffix_path(tmp, sizeof tmp, path, ".tmp") ||
        !lfs_suffix_path(bak, sizeof bak, path, ".bak") ||
        !lfs_suffix_path(crcp, sizeof crcp, path, ".crc")) {
        ESP_LOGE(TAG, "path too long for atomic write: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);

    // 1. Stage the new content in a temp file (full + synced). Failure → original untouched.
    esp_err_t rc = lfs_write_synced_locked(tmp, data, size);
    if (rc != ESP_OK) {
        lfs_remove(&g_lfs, tmp);   // clean partial temp
        xSemaphoreGive(g_mutex);
        return rc;
    }

    // 2. Preserve the current good copy as .bak (best-effort; ignore errors — first write has none).
    //    Also move its CRC sidecar to <path>.bak.crc so the backup stays integrity-verifiable.
    struct lfs_info info;
    if (lfs_stat(&g_lfs, path, &info) >= 0) {
        lfs_remove(&g_lfs, bak);          // drop stale backup (rename needs a free name)
        lfs_rename(&g_lfs, path, bak);    // path → .bak (best-effort)
        char bakcrc[64];
        if (lfs_suffix_path(bakcrc, sizeof bakcrc, path, ".bak.crc")) {
            lfs_remove(&g_lfs, bakcrc);
            lfs_rename(&g_lfs, crcp, bakcrc);   // <path>.crc → <path>.bak.crc (best-effort)
        }
    }

    // 3. Atomic commit: temp → path. littlefs guarantees this is power-loss atomic.
    int rerr = lfs_rename(&g_lfs, tmp, path);
    if (rerr) {
        // Commit failed — restore the previous version from .bak so config is not lost.
        lfs_rename(&g_lfs, bak, path);
        lfs_remove(&g_lfs, tmp);
        xSemaphoreGive(g_mutex);
        ESP_LOGE(TAG, "atomic commit failed for %s: %d (restored from .bak)", path, rerr);
        return ESP_FAIL;
    }

    // 4. CRC sidecar (best-effort: data is already committed; a sidecar failure is non-fatal —
    //    read-side treats a missing sidecar as "legacy, accept").
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)data, size);
    lfs_write_synced_locked(crcp, &crc, sizeof crc);

    xSemaphoreGive(g_mutex);
    ESP_LOGI(TAG, "Wrote %zu bytes to %s (atomic+backup, crc=%08lx)", size, path, (unsigned long)crc);
    return ESP_OK;
}

/* Read a whole file into `data` (up to max_size). Assumes g_mutex HELD. ESP_OK + *out_read on
 * success; ESP_ERR_NOT_FOUND if absent; ESP_FAIL on read error. */
static esp_err_t lfs_read_file_locked(const char *path, void *data, size_t max_size, lfs_ssize_t *out_read)
{
    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY);
    if (err) return ESP_ERR_NOT_FOUND;
    lfs_ssize_t br = lfs_file_read(&g_lfs, &file, data, max_size);
    lfs_file_close(&g_lfs, &file);
    if (br < 0) return ESP_FAIL;
    *out_read = br;
    return ESP_OK;
}

/* Verify the `<path>.crc` sidecar against crc32(data,len). Returns true when it MATCHES, and ALSO
 * true when NO sidecar exists (legacy/backward-compat: absence = trust the data). Only a present-
 * but-mismatched sidecar returns false (= corruption detected). Assumes g_mutex HELD. */
static bool lfs_crc_ok_locked(const char *path, const void *data, size_t len)
{
    char crcp[64];
    if (!lfs_suffix_path(crcp, sizeof crcp, path, ".crc")) return true;
    uint32_t stored = 0; lfs_ssize_t got = 0;
    if (lfs_read_file_locked(crcp, &stored, sizeof stored, &got) != ESP_OK ||
        got != (lfs_ssize_t)sizeof stored) {
        return true;   // no/short sidecar → legacy, accept
    }
    uint32_t actual = esp_rom_crc32_le(0, (const uint8_t *)data, len);
    if (actual != stored) {
        ESP_LOGE(TAG, "%s CRC MISMATCH (have %08lx want %08lx) — treating as corrupt",
                 path, (unsigned long)actual, (unsigned long)stored);
        return false;
    }
    return true;
}

/* Read a config file, preferring the primary and falling back to the .bak backup
 * (documented in littlefs_raw.h).
 *
 * Pairs with the atomic write above. The primary is accepted only if it reads
 * AND its CRC sidecar (when present) matches; otherwise — or if the primary is absent/empty (e.g. the
 * brief window between commit renames after a power loss) — the previous good copy is recovered from
 * `<path>.bak` (verified against `<path>.bak.crc`). Backward compatible: files without a sidecar
 * (written by older firmware) are accepted as-is.
 */
esp_err_t littlefs_raw_read(const char *path, void *data, size_t max_size, size_t *actual_size)
{
    if (!g_mounted || !path || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);

    // 1. Primary — accept only if it reads and the CRC sidecar (if any) matches.
    lfs_ssize_t br = 0;
    esp_err_t rc = lfs_read_file_locked(path, data, max_size, &br);
    if (rc == ESP_OK && br > 0 && lfs_crc_ok_locked(path, data, (size_t)br)) {
        if (actual_size) *actual_size = (size_t)br;
        xSemaphoreGive(g_mutex);
        ESP_LOGI(TAG, "Read %ld bytes from %s", (long)br, path);
        return ESP_OK;
    }

    // 2. Primary missing/empty/corrupt → recover from the .bak backup.
    char bak[64];
    if (lfs_suffix_path(bak, sizeof bak, path, ".bak")) {
        lfs_ssize_t bbr = 0;
        if (lfs_read_file_locked(bak, data, max_size, &bbr) == ESP_OK && bbr > 0 &&
            lfs_crc_ok_locked(bak, data, (size_t)bbr)) {
            if (actual_size) *actual_size = (size_t)bbr;
            xSemaphoreGive(g_mutex);
            ESP_LOGW(TAG, "%s unreadable/corrupt — RECOVERED %ld bytes from backup", path, (long)bbr);
            return ESP_OK;
        }
    }

    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "Failed to read %s (no valid primary or backup)", path);
    return ESP_ERR_NOT_FOUND;
}

/* Delete a file and its companions (documented in littlefs_raw.h). */
esp_err_t littlefs_raw_remove(const char *path)
{
    if (!g_mounted || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(g_mutex, portMAX_DELAY);

    int err = lfs_remove(&g_lfs, path);

    // Also clear the atomic-write companions (.bak/.crc/.bak.crc/.tmp) so a removed config can't be
    // silently resurrected from its backup by littlefs_raw_read's fallback. Best-effort.
    const char *sfx[] = { ".bak", ".crc", ".bak.crc", ".tmp" };
    char comp[64];
    for (size_t i = 0; i < sizeof(sfx) / sizeof(sfx[0]); i++) {
        if (lfs_suffix_path(comp, sizeof comp, path, sfx[i])) lfs_remove(&g_lfs, comp);
    }

    xSemaphoreGive(g_mutex);

    if (err) {
        ESP_LOGE(TAG, "Failed to remove %s: %d", path, err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Removed %s (+companions)", path);
    return ESP_OK;
}

/* Check if a file exists (documented in littlefs_raw.h). */
bool littlefs_raw_exists(const char *path)
{
    if (!g_mounted || !path) {
        return false;
    }
    
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    
    struct lfs_info info;
    int err = lfs_stat(&g_lfs, path, &info);
    /* A power cut between "path → .bak" and ".tmp → path" leaves only the .bak. littlefs_raw_read() recovers
     * from it, but every caller asks exists() first — a false here wrote DEFAULT config over the real one and
     * took the "no format marker" branch that wipes the stats region. */
    if (err != 0) {
        char bak[64];
        if (lfs_suffix_path(bak, sizeof bak, path, ".bak")) err = lfs_stat(&g_lfs, bak, &info);
    }
    
    xSemaphoreGive(g_mutex);
    
    return (err == 0);
}

/* Create a directory (documented in littlefs_raw.h). */
esp_err_t littlefs_raw_mkdir(const char *path)
{
    if (!g_mounted || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    
    int err = lfs_mkdir(&g_lfs, path);
    
    xSemaphoreGive(g_mutex);
    
    if (err && err != LFS_ERR_EXIST) {
        ESP_LOGE(TAG, "Failed to create directory %s: %d", path, err);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Created directory %s", path);
    return ESP_OK;
}

/* Get filesystem info (documented in littlefs_raw.h). */
esp_err_t littlefs_raw_info(size_t *total_blocks, size_t *used_blocks)
{
    if (!g_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    
    lfs_ssize_t used = lfs_fs_size(&g_lfs);
    
    xSemaphoreGive(g_mutex);
    
    if (used < 0) {
        ESP_LOGE(TAG, "Failed to get fs size: %ld", (long)used);
        return ESP_FAIL;
    }
    
    if (total_blocks) *total_blocks = g_cfg.block_count;
    if (used_blocks) *used_blocks = used;
    
    ESP_LOGI(TAG, "Filesystem: %ld/%lu blocks used", (long)used, (unsigned long)g_cfg.block_count);
    return ESP_OK;
}