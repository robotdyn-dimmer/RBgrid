/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_config_storage.h
 * @brief Storage layer for RBGrid configuration on external Flash
 *
 * Each configuration domain lives in its own JSON file on the external-flash LittleFS config
 * partition (/config.json system, /hardware.json, /units.json, /time.json, /tariffs.json,
 * /rbamp.json). This module reads and writes those files; JSON (de)serialization is delegated to
 * rbjson.c and applying the parsed values goes through the rbgrid_config API.
 *
 * Thread safety: the functions are intended to be called from the boot sequence and from the web
 * server / console task context. They are not re-entrant with respect to the same file; callers
 * serialize configuration writes.
 */

#ifndef RBGRID_CONFIG_STORAGE_H
#define RBGRID_CONFIG_STORAGE_H

#include "esp_err.h"
#include "rbgrid_config_api.h"
#include "rbjson.h"

/**
 * @brief Maximum size of /units.json, in bytes.
 *
 * /units.json has its own ceiling: each unit takes about 1.2-1.3 KB, and the 8 KB cap shared by
 * the other config files held only 6 units (a multichannel module with a voltage channel and 7
 * current channels already needs 8). Sized for 64 units (RBPOWER_MAX_HOME_UNITS with PSRAM) at
 * about 1.3 KB each plus margin. Buffers of this size are allocated in PSRAM via
 * rbgrid_cfg_big_alloc().
 */
#define CONFIG_UNITS_SIZE   (96 * 1024)

/**
 * @brief Allocate a large configuration buffer, preferring PSRAM.
 * @param[in] n Size of the buffer, in bytes.
 * @return Pointer to the buffer (PSRAM if available, otherwise internal heap), or NULL when both
 *         allocations fail. The caller frees it with free().
 */
void *rbgrid_cfg_big_alloc(size_t n);

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize the configuration storage (creates the /backup folder if missing).
     *
     * The external flash must already be initialized and its config partition mounted.
     * @retval ESP_OK                Storage ready (a failure to create /backup is only logged).
     * @retval ESP_ERR_INVALID_STATE The external-flash config partition is not mounted.
     */
    esp_err_t rbgrid_storage_init(void);

    /**
     * @brief Load the system section of /config.json.
     * @param[out] config Configuration structure filled from the "system" JSON section.
     * @retval ESP_OK              Loaded.
     * @retval ESP_ERR_INVALID_ARG @p config is NULL.
     * @retval ESP_ERR_NO_MEM      Read buffer allocation failed.
     * @retval ESP_ERR_NOT_FOUND   The file has no "system" section.
     * @retval ESP_FAIL            The file is not valid JSON or the section failed to parse.
     * @return Any error from littlefs_raw_read() when the file cannot be read (defaults apply).
     */
    esp_err_t rbgrid_storage_load_system(rbpower_extended_config_t *config);

    /**
     * @brief Save the system configuration to /config.json.
     *
     * When the file already exists it is first copied to the default backup file
     * (/backup/config.json); a failed backup is logged and does not stop the save.
     * @param[in] config Configuration to serialize.
     * @retval ESP_OK              Saved.
     * @retval ESP_ERR_INVALID_ARG @p config is NULL.
     * @retval ESP_ERR_NO_MEM      JSON creation failed or the JSON exceeds the 8 KB file cap.
     * @return Any error from littlefs_raw_write() on a write failure.
     */
    esp_err_t rbgrid_storage_save_system(const rbpower_extended_config_t *config);

    /**
     * @brief Check that /config.json exists, creating a default one when it does not.
     * @retval ESP_OK The file exists or the default configuration was written.
     * @return Otherwise the error of rbgrid_storage_save_system() when writing the default fails.
     */
    esp_err_t rbgrid_storage_check_files(void);

    /**
     * @brief Get usage of the configuration filesystem.
     * @param[out] total_bytes Total capacity, in bytes (4 KB blocks); may be NULL.
     * @param[out] used_bytes  Used space, in bytes (4 KB blocks); may be NULL.
     * @retval ESP_OK Success.
     * @return Any error from littlefs_raw_info().
     */
    esp_err_t rbgrid_storage_get_stats(size_t *total_bytes, size_t *used_bytes);

    /**
     * @brief Copy /config.json to a backup file.
     * @param[in] tag Full path of the backup file, or NULL for /backup/config.json.
     * @retval ESP_OK                Backup written.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NO_MEM        Copy buffer allocation failed.
     * @return Any error from littlefs_raw_read() / littlefs_raw_write().
     */
    esp_err_t rbgrid_storage_create_backup(const char *tag);

    /**
     * @brief Restore /config.json from a backup file (the backup must be valid JSON).
     * @param[in] tag Full path of the backup file, or NULL for /backup/config.json.
     * @retval ESP_OK                Restored; reboot or reload to apply.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NOT_FOUND     The backup file does not exist.
     * @retval ESP_ERR_NO_MEM        Copy buffer allocation failed.
     * @retval ESP_ERR_INVALID_ARG   The backup is not valid JSON.
     * @return Any error from littlefs_raw_read() / littlefs_raw_write().
     */
    esp_err_t rbgrid_storage_restore_from_backup(const char *tag);

    /**
     * @brief Check whether a backup file exists.
     * @param[in] tag Full path of the backup file, or NULL for /backup/config.json.
     * @retval ESP_OK                The backup exists.
     * @retval ESP_ERR_NOT_FOUND     The backup does not exist.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     */
    esp_err_t rbgrid_storage_check_backup(const char *tag);

    /**
     * @name Full-configuration backup slot
     * Modular full backup: every configuration domain file is copied to /backup/&lt;file&gt;. There is a
     * single slot holding only the last backup (no history).
     * @{
     */

    /**
     * @brief Snapshot every present configuration file into /backup/ and write the slot
     *        metadata file /backup/_meta.json. Overwrites the previous slot.
     * @return Number of configuration files copied.
     */
    int rbgrid_storage_backup_all(void);

    /**
     * @brief Restore every configuration file present in /backup/ to its live location.
     *
     * The running configuration is held in RAM, so a reboot is needed to apply it.
     * @return Number of configuration files restored.
     */
    int rbgrid_storage_restore_all(void);

    /**
     * @brief Count the configuration file backups present in the slot.
     * @return Number of files in the slot (0 when there is no backup).
     */
    int rbgrid_storage_backup_count(void);

    /**
     * @brief Read the slot metadata JSON written by rbgrid_storage_backup_all().
     *
     * The metadata is an object {"ts": wall-clock epoch s, "fw": firmware version, "files": count}.
     * Before SNTP sync the timestamp is uptime-based, like the other records.
     * @param[out] out Buffer receiving the NUL-terminated JSON.
     * @param[in]  cap Size of @p out, in bytes (at least 2).
     * @return Length of the JSON written to @p out, or 0 when the metadata is absent, invalid or
     *         the arguments are invalid.
     */
    int rbgrid_storage_backup_meta(char *out, size_t cap);

    /** @} */

    /**
     * @brief Save the hardware configuration to /hardware.json.
     * @param[in] config Hardware configuration to serialize (the global
     *                   g_extended_config->base_hardware_config or a local copy).
     * @retval ESP_OK              Saved.
     * @retval ESP_ERR_INVALID_ARG @p config is NULL.
     * @retval ESP_ERR_NO_MEM      JSON creation failed or the JSON exceeds the 8 KB file cap.
     * @return Any error from littlefs_raw_write() on a write failure.
     */
    esp_err_t rbgrid_storage_save_hardware(const rbpower_hardware_config_t *config);

    /**
     * @brief Load the hardware configuration from /hardware.json.
     * @param[out] config Hardware configuration to fill (the global
     *                    g_extended_config->base_hardware_config or a local copy).
     * @retval ESP_OK              Loaded (a file without a "data" section leaves @p config unchanged).
     * @retval ESP_ERR_INVALID_ARG @p config is NULL, or the file's "type" is not "hardware".
     * @retval ESP_ERR_NO_MEM      Read buffer allocation failed.
     * @retval ESP_FAIL            The file is not valid JSON or the data section failed to parse.
     * @return Any error from littlefs_raw_read() when the file cannot be read (defaults apply).
     */
    esp_err_t rbgrid_storage_load_hardware(rbpower_hardware_config_t *config);

    /**
     * @brief Save the units configuration to /units.json.
     *
     * When @p json_str is NULL the JSON is built from the live units in RAM with
     * rbgrid_json_create_units_config() under the home-network lock. On success the hardware
     * revision counter is bumped so clients re-read the sensor list.
     * @param[in] json_str JSON to write verbatim, or NULL to serialize the current state.
     * @retval ESP_OK                Saved.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NO_MEM        JSON creation failed or the JSON exceeds CONFIG_UNITS_SIZE.
     * @return Any error from littlefs_raw_write() on a write failure.
     */
    esp_err_t rbgrid_storage_save_units(const char *json_str);

    /**
     * @brief Load /units.json and recreate the units through the rbgrid_config API.
     *
     * rbAmp (I2C) units are restored with their persisted identity, and only when the rbAmp fleet
     * configuration still binds them (a deleted unit does not reappear). Legacy "ADC_SENSOR"
     * records are the default-unit binding fed by the rbAmp bridge and are recreated by role.
     * ESP-NOW units are skipped; they are rebuilt from the node registry at boot.
     * @param[in] config_handle Configuration handle from rbgrid_config_create().
     * @retval ESP_OK                Loaded and applied (individual unit failures are logged).
     * @retval ESP_ERR_INVALID_ARG   @p config_handle is NULL or the file's "type" is wrong.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NOT_FOUND     No file, no "data" section or no "units" array.
     * @retval ESP_ERR_NO_MEM        Read buffer allocation failed.
     * @retval ESP_FAIL              The file is not valid JSON.
     * @return Any error from littlefs_raw_read().
     */
    esp_err_t rbgrid_storage_load_units(rbgrid_config_handle_t config_handle);

    /**
     * @brief Save the time configuration JSON (as received from the web interface) to /time.json.
     *
     * The JSON must contain a "config" object. An existing file is first copied to
     * /backup/time.json.
     * @param[in] json_config JSON text to store verbatim.
     * @retval ESP_OK                Saved.
     * @retval ESP_ERR_INVALID_ARG   @p json_config is NULL, not valid JSON, or lacks "config".
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @return Any error from littlefs_raw_write() on a write failure.
     */
    esp_err_t rbgrid_storage_save_time(const char *json_config);

    /**
     * @brief Load /time.json and apply it through the rbgrid_config time setters
     *        (SNTP servers and timezone, fallbacks, advanced sync settings).
     * @param[in] config_handle Configuration handle to apply the settings to.
     * @retval ESP_OK                Loaded (a setter that fails is logged, not returned).
     * @retval ESP_ERR_INVALID_ARG   @p config_handle is NULL, invalid JSON, or no "config" object.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NOT_FOUND     The file does not exist.
     * @return Any error from littlefs_raw_read().
     */
    esp_err_t rbgrid_storage_load_time(rbgrid_config_handle_t config_handle);

    /**
     * @brief Save the tariff configuration JSON verbatim to /tariffs.json.
     * @param[in] json_str Tariff configuration JSON text.
     * @retval ESP_OK                Saved.
     * @retval ESP_ERR_INVALID_ARG   @p json_str is NULL.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NO_MEM        The JSON exceeds the 8 KB file cap.
     * @return Any error from littlefs_raw_write() on a write failure.
     */
    esp_err_t rbgrid_storage_save_tariffs(const char *json_str);

    /**
     * @brief Load /tariffs.json, rebuild the tariff plan with the tariff builder and apply it.
     * @param[in] config_handle Configuration handle from rbgrid_config_create().
     * @retval ESP_OK                Loaded and applied.
     * @retval ESP_ERR_INVALID_ARG   @p config_handle is NULL.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NOT_FOUND     The file does not exist.
     * @retval ESP_ERR_NO_MEM        Buffer or tariff builder allocation failed.
     * @retval ESP_FAIL              Parsing, validation or applying the plan failed.
     * @return Any error from littlefs_raw_read().
     */
    esp_err_t rbgrid_storage_load_tariffs(rbgrid_config_handle_t config_handle);

    /**
     * @brief Save the rbAmp fleet configuration (bus parameters and the per-module
     *        address/entry map) to /rbamp.json, then bump the hardware revision counter.
     * @param[in] cfg Fleet configuration to persist.
     * @retval ESP_OK                Saved.
     * @retval ESP_ERR_INVALID_ARG   @p cfg is NULL.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NO_MEM        Serialization failed.
     * @return Any error from littlefs_raw_write() on a write failure.
     */
    esp_err_t rbgrid_storage_save_rbamp(const rbamp_bridge_config_t *cfg);

    /**
     * @brief Load the rbAmp fleet configuration from /rbamp.json.
     * @param[out] cfg Fleet configuration, overwritten on success.
     * @retval ESP_OK                Loaded.
     * @retval ESP_ERR_INVALID_ARG   @p cfg is NULL.
     * @retval ESP_ERR_INVALID_STATE The config partition is not mounted.
     * @retval ESP_ERR_NOT_FOUND     No file; the caller uses defaults.
     * @retval ESP_ERR_NO_MEM        Read buffer allocation failed.
     * @retval ESP_FAIL              The file failed to parse.
     * @return Any error from littlefs_raw_read().
     */
    esp_err_t rbgrid_storage_load_rbamp(rbamp_bridge_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif // RBGRID_CONFIG_STORAGE_H