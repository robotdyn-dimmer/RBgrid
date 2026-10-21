/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file otp_external_flash_test.c
 * @brief Low-level external SPI NOR bring-up (standard SPI mode, custom pins).
 *
 * init_external_flash(), deinit_external_flash() and get_external_flash_handle() are the low-level
 * driver hooks used by external_flash_manager.c on the external-NOR path.
 */

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_partition.h"
#include "esp_vfs.h"
// #include "esp_vfs_fat.h"  // Not needed for Flash operations
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_rom_spiflash.h"
#include "spi_flash_mmap.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "soc/spi_pins.h"
#include "hal/gpio_hal.h"
#include "esp_rom_gpio.h"
#include "sdkconfig.h"
#include "otp_manager.h"

static const char *TAG = "ext_flash";

// Pin configuration for external flash
#define VSPI_PIN_NUM_MISO   12  ///< DO/MISO (GPIO12 is a boot strap pin, see external_flash_config.h)
#define VSPI_PIN_NUM_MOSI   13  ///< DI/MOSI
#define VSPI_PIN_NUM_CLK    14  ///< CLK
#define VSPI_PIN_NUM_CS     15  ///< CS
#define VSPI_PIN_NUM_HD     -1  ///< Not used in Standard SPI mode
#define VSPI_PIN_NUM_WP     -1  ///< Not used in Standard SPI mode

// External flash chip configuration
#define EXTERNAL_FLASH_SIZE     (4 * 1024 * 1024)  ///< Nominal chip size, bytes (4 MB); the real size is read from the chip
#define EXTERNAL_FLASH_SPEED    SPI_FLASH_40MHZ    ///< SPI clock of the external chip

static esp_flash_t* ext_flash = NULL;

esp_err_t init_external_flash(void)
{
    ESP_LOGI(TAG, "Initializing external SPI flash");
    
    // SPI bus configuration
    const spi_bus_config_t bus_config = {
        .mosi_io_num = VSPI_PIN_NUM_MOSI,
        .miso_io_num = VSPI_PIN_NUM_MISO,
        .sclk_io_num = VSPI_PIN_NUM_CLK,
        .quadwp_io_num = VSPI_PIN_NUM_WP,
        .quadhd_io_num = VSPI_PIN_NUM_HD,
        .max_transfer_sz = 0,
    };

    // Initialize SPI bus for external flash
    // Use SPI2_HOST (HSPI) to avoid conflicts with internal flash
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // External flash configuration
    const esp_flash_spi_device_config_t device_config = {
        .host_id = SPI2_HOST,
        .cs_io_num = VSPI_PIN_NUM_CS,
        .io_mode = SPI_FLASH_SLOWRD,  // Standard SPI mode
        .freq_mhz = 40,  // 40MHz, adjust as needed
        .cs_id = 0,
        .input_delay_ns = 0,
    };

    // Add external flash device
    ESP_LOGD(TAG, "Adding external flash device | Pins - MISO:%d MOSI:%d CLK:%d CS:%d",
             VSPI_PIN_NUM_MISO, VSPI_PIN_NUM_MOSI, VSPI_PIN_NUM_CLK, VSPI_PIN_NUM_CS);
    
    ret = spi_bus_add_flash_device(&ext_flash, &device_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add external flash device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize the external flash chip
    ret = esp_flash_init(ext_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize external flash: %s", esp_err_to_name(ret));
        return ret;
    }

    // Get flash chip info
    uint32_t flash_id;
    ret = esp_flash_read_id(ext_flash, &flash_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash ID: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGD(TAG, "External flash chip ID: 0x%08lx", flash_id);

    // Get flash size
    uint32_t flash_size = 0;
    ret = esp_flash_get_size(ext_flash, &flash_size);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "External flash size: %ld MB", flash_size / (1024 * 1024));
    }

    return ESP_OK;
}

void deinit_external_flash(void)
{
    if (ext_flash) {
        spi_bus_remove_flash_device(ext_flash);
        ext_flash = NULL;
    }
    spi_bus_free(SPI2_HOST);
    ESP_LOGD(TAG, "External flash deinitialized");
}

esp_flash_t* get_external_flash_handle(void)
{
    return ext_flash;
}
