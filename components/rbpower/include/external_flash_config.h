/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file external_flash_config.h
 * @brief Storage-media bring-up: external NOR flash SPI pins, runtime state and partition queries.
 *
 * The hub keeps its configuration files and statistics history on one storage medium at a time:
 * either the internal flash (`stats` + `config` partitions, the preferred medium) or an optional
 * external W25Q-class SPI NOR chip. external_flash_init() selects the medium; the rest of the API
 * reports the selected medium's state and partition layout. Despite the "external" prefix, the
 * state and partition queries also describe the internal medium when it is in use.
 */
#ifndef EXTERNAL_FLASH_CONFIG_H
#define EXTERNAL_FLASH_CONFIG_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_flash.h"
#include <stdbool.h>
#include <stdint.h>
#include "partition_layout.h" // For partition size definitions

/*
 * Default SPI pins for the external NOR flash (HSPI/SPI2).
 * HARDWARE REQUIREMENT: MISO = GPIO12 is also the VDD_SDIO flash-voltage strap (MTDI). The PCB must
 * fit an external 5-10 kOhm pull-down from GPIO12 to GND so the strap reads LOW (3.3 V) at every
 * reset, including warm resets. Otherwise NOR MISO leakage during the boot window can flip VDD_SDIO
 * and cause an intermittent dead boot (including after an OTA restart). The firmware needs no change
 * for this; burning the eFuse (set_flash_voltage 3.3V) is an optional fallback.
 * See documents/architecture/MIGRATION_TO_EXTERNAL_FLASH.md (GPIO12 section).
 */
/** @brief Default SPI MISO pin of the external NOR (GPIO12, a boot strap pin: see note above). */
#define DEFAULT_FLASH_MISO_PIN GPIO_NUM_12
/** @brief Default SPI MOSI pin of the external NOR. */
#define DEFAULT_FLASH_MOSI_PIN GPIO_NUM_13
/** @brief Default SPI clock pin of the external NOR. */
#define DEFAULT_FLASH_SCLK_PIN GPIO_NUM_14
/** @brief Default SPI chip-select pin of the external NOR. */
#define DEFAULT_FLASH_CS_PIN GPIO_NUM_15

/** @brief SPI host used for the external NOR. */
#define FLASH_SPI_HOST SPI2_HOST
/** @brief SPI clock of the external NOR, Hz (20 MHz). */
#define FLASH_SPI_CLOCK_SPEED (20 * 1000 * 1000) // 20MHz
/** @brief SPI mode of the external NOR (mode 0). */
#define FLASH_SPI_MODE 0                         // SPI mode 0

// Partition offsets and sizes are defined in partition_layout.h.

/**
 * @brief External Flash pins configuration
 */
typedef struct
{
    gpio_num_t miso_pin; ///< MISO (controller in, peripheral out)
    gpio_num_t mosi_pin; ///< MOSI (controller out, peripheral in)
    gpio_num_t sclk_pin; ///< Serial clock
    gpio_num_t cs_pin;   ///< Chip select
} external_flash_pins_t;

/**
 * @brief External Flash runtime status
 */
typedef struct
{
    bool initialized;          ///< Storage medium initialized (internal partitions found, or external SPI chip up)
    bool otp_validated;        ///< OTP authenticity check passed (always true on the internal medium, where it does not apply)
    bool filesystem_mounted;   ///< Config LittleFS mounted
    uint32_t device_serial;    ///< Device serial number read from the external chip's OTP (0 = not available)
    uint32_t flash_id;         ///< JEDEC ID of the external chip (0 = internal medium in use)
    uint32_t flash_size;       ///< Size of the flash chip in use, bytes
    const char *error_message; ///< Last error message (static string), NULL if none
} external_flash_status_t;

/**
 * @brief External Flash runtime state (consolidated)
 */
typedef struct
{
    external_flash_pins_t pins;     ///< SPI pin assignment of the external chip

    external_flash_status_t status; ///< Runtime status

    esp_flash_t *flash_handle;      ///< esp_flash handle of the external chip; NULL when the internal medium is in use

    /** @brief Partition sizes and usage, bytes. */
    struct
    {
        size_t config_size; ///< Config partition size, bytes
        size_t config_used; ///< Config partition usage, bytes (not computed yet, always 0)
        size_t log_size;    ///< Log partition size, bytes
        size_t log_used;    ///< Log partition usage, bytes (not computed yet, always 0)
        size_t stats_size;  ///< Statistics region size, bytes
        size_t stats_used;  ///< Statistics region usage, bytes (not computed yet, always 0)
    } partitions;                   ///< Partition sizes and usage

} external_flash_state_t;

/**
 * @brief Select and initialize the storage medium, then mount the config LittleFS.
 *
 * The internal flash is tried first: when the internal `stats` partition exists, the statistics
 * rings and the config LittleFS live on internal flash and the external chip is not touched. Only
 * when that partition is absent (older partition table) does the function fall back to the
 * external NOR path: SPI init, OTP read, re-init, partition checks, LittleFS mount, and routing of
 * the statistics media to the external chip.
 *
 * Call once at boot, from the init task, before any other storage function. Not thread-safe.
 *
 * @return ESP_OK on success (partition-check or mount failures on the external path are logged
 *         but do not fail the call).
 * @retval ESP_OK   Medium selected and initialized.
 * @retval ESP_FAIL External chip did not answer its JEDEC ID after the OTP read.
 * @retval other    Error from the internal LittleFS mount or from the external SPI/flash driver init.
 */
esp_err_t external_flash_init(void);

/**
 * @brief Unmount the config LittleFS and release the external SPI chip.
 * @return Always ESP_OK (an unmount failure is only logged).
 */
esp_err_t external_flash_deinit(void);

/**
 * @brief Get the storage-medium state.
 * @return Pointer to the module's static state structure (never NULL). The caller must not free it.
 */
external_flash_state_t *external_flash_get_state(void);

/**
 * @brief Check whether the storage medium is usable.
 * @return true if the medium is initialized and the OTP check passed (always passes on the internal medium).
 */
bool external_flash_is_available(void);

/**
 * @brief Get the device serial number read from the external chip's OTP area.
 * @return Device serial number, or 0 if not available (for example, on the internal medium).
 */
uint32_t external_flash_get_serial(void);

/**
 * @brief Check whether a named partition is available.
 * @param[in] partition_name Partition name ("partition_table", "nvs", "config", "log", "stats"); NULL returns false.
 * @return true if the partition is known and available.
 */
bool external_flash_is_partition_available(const char *partition_name);

/**
 * @brief Get a partition's location and size.
 * @param[in]  partition_name Partition name (see external_flash_is_partition_available()); NULL returns false.
 * @param[out] offset         Partition offset in the external-layout address space, bytes (may be NULL).
 * @param[out] size           Partition size, bytes (may be NULL).
 * @return true if the partition exists and is available. The outputs are written whenever the
 *         partition name is known, even if it is not available.
 */
bool external_flash_get_partition_info(const char *partition_name,
                                       uint32_t *offset,
                                       uint32_t *size);

/**
 * @brief Get the mount point name of the config filesystem.
 * @return "/config" when the config LittleFS is mounted, NULL otherwise.
 */
const char *external_flash_get_config_mount_point(void);

/**
 * @brief Check whether the config partition is mounted.
 *
 * Every config save/load path checks this before touching the config files.
 * @return true if the config partition is mounted.
 */
bool external_flash_is_config_mounted(void);

/**
 * @brief Initialize the SPI bus and the esp_flash driver for the external NOR chip (low-level).
 * @return ESP_OK on success, or an error from the SPI master / esp_flash driver.
 */
esp_err_t init_external_flash(void);
/** @brief Remove the external NOR esp_flash device and free the SPI bus (low-level). */
void deinit_external_flash(void);
/**
 * @brief Get the low-level esp_flash handle of the external NOR chip.
 * @return The handle, or NULL if init_external_flash() has not succeeded.
 */
esp_flash_t *get_external_flash_handle(void);

#endif // EXTERNAL_FLASH_CONFIG_H