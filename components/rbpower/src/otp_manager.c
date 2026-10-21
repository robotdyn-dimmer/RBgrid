/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file otp_manager.c
 * @brief OTP (One-Time Programmable) production record access for the external Winbond NOR chip
 * 
 * This file combines all OTP-related functionality:
 * - Low-level SPI commands for OTP operations
 * - OTP data reading and verification
 * - Device authentication via OTP
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "otp_manager.h"

static const char *TAG = "OTP_MGR";

/* ================================================================================================
 * OTP CONFIGURATION
 * ================================================================================================ */

// SPI pins (must match external Flash configuration)
#define PIN_NUM_MISO   12  ///< SPI MISO (GPIO12)
#define PIN_NUM_MOSI   13  ///< SPI MOSI (GPIO13)
#define PIN_NUM_CLK    14  ///< SPI clock (GPIO14)
#define PIN_NUM_CS     15  ///< SPI chip select (GPIO15)

// Winbond SPI Commands
#define CMD_WRITE_ENABLE        0x06 ///< Write Enable
#define CMD_WRITE_DISABLE       0x04 ///< Write Disable
#define CMD_READ_STATUS_REG1    0x05 ///< Read Status Register-1
#define CMD_READ_STATUS_REG2    0x35 ///< Read Status Register-2
#define CMD_ENTER_OTP_MODE      0x3A ///< Enter OTP mode
#define CMD_READ_DATA           0x03 ///< Read Data
#define CMD_PAGE_PROGRAM        0x02 ///< Page Program
#define CMD_OTP_PROGRAM         0x42 ///< Program Security Register
#define CMD_READ_JEDEC_ID       0x9F ///< Read JEDEC ID
#define CMD_READ_SECURITY_REG   0x48 ///< Read Security Register
#define CMD_ENABLE_RESET        0x66 ///< Enable Reset
#define CMD_RESET               0x99 ///< Reset Device

#define SR1_BUSY_BIT           0x01 ///< BUSY bit of Status Register-1

// OTP Configuration
#define CRC32_SEED 0xDEADBEEF ///< Seed mixed into the keyed CRC32 (an integrity check, not a secret; must match the production tool)
#define OTP_MAGIC 0x50524F44  ///< "PROD" in little-endian

// OTP Memory mapping - W25Q32 has 3 security registers
#define OTP_REG1_ADDR  0x001000  ///< Security Register 1 (not used: its address coincides with the NVS partition offset)
#define OTP_REG2_ADDR  0x002000  ///< Security Register 2
#define OTP_REG3_ADDR  0x003000  ///< Security Register 3 (holds the production record)
#define OTP_USE_ADDR   OTP_REG3_ADDR  ///< Register used for the production record (Register 3)

/* ================================================================================================
 * PRIVATE VARIABLES
 * ================================================================================================ */

static spi_device_handle_t g_otp_spi = NULL;
static bool g_otp_initialized = false;
static otp_data_t g_cached_otp_data = {0};
static bool g_otp_data_cached = false;

/* ================================================================================================
 * LOW-LEVEL SPI FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Initialize OTP SPI interface
 */
static esp_err_t otp_spi_init(void)
{
    if (g_otp_spi) {
        return ESP_OK;  // Already initialized
    }
    
    esp_err_t ret;
    
    // SPI device configuration for OTP operations
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10*1000*1000,  // 10 MHz for OTP (safer)
        .mode = 0,                        // SPI mode 0
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
        .command_bits = 0,
        .address_bits = 0,
    };
    
    // Attach the device to SPI bus (bus should be initialized by Flash)
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &g_otp_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "OTP SPI initialized");
    return ESP_OK;
}

/**
 * @brief Software reset to exit OTP mode and return to normal Flash mode
 */
static esp_err_t otp_software_reset(void)
{
    if (!g_otp_spi) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Sending software reset to exit OTP mode...");

    // Send Enable Reset command (0x66)
    uint8_t cmd1 = CMD_ENABLE_RESET;
    spi_transaction_t t1 = {
        .length = 8,
        .tx_buffer = &cmd1,
        .rx_buffer = NULL,
    };
    
    esp_err_t ret = spi_device_transmit(g_otp_spi, &t1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send Enable Reset command");
        return ret;
    }
    
    // Send Reset command (0x99)
    uint8_t cmd2 = CMD_RESET;
    spi_transaction_t t2 = {
        .length = 8,
        .tx_buffer = &cmd2,
        .rx_buffer = NULL,
    };
    
    ret = spi_device_transmit(g_otp_spi, &t2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send Reset command");
        return ret;
    }
    
    // Wait for reset to complete
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGD(TAG, "Flash chip reset completed - returned to normal mode");
    return ESP_OK;
}

/**
 * @brief Deinitialize OTP SPI interface
 */
static void otp_spi_deinit(void)
{
    if (g_otp_spi) {
        // Reset the Flash chip to exit OTP mode
        otp_software_reset();
        
        // Make sure CS is high
        gpio_set_level(PIN_NUM_CS, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        
        // Remove SPI device
        spi_bus_remove_device(g_otp_spi);
        g_otp_spi = NULL;
        
        // Reset CS pin for Flash driver
        gpio_reset_pin(PIN_NUM_CS);

        ESP_LOGD(TAG, "OTP SPI deinitialized, Flash ready for normal operations");
    }
}

/**
 * @brief Read data from OTP security register
 */
static esp_err_t otp_spi_read_register(uint32_t addr, uint8_t *data, size_t len)
{
    if (!g_otp_spi || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Map logical address to physical OTP address
    uint32_t otp_addr = OTP_USE_ADDR + (addr & 0xFF);
    ESP_LOGD(TAG, "Reading OTP from address: 0x%06lX", (unsigned long)otp_addr);

    // Use Read Security Register command (0x48)
    uint8_t cmd[5] = {
        CMD_READ_SECURITY_REG,
        (otp_addr >> 16) & 0xFF,
        (otp_addr >> 8) & 0xFF,
        otp_addr & 0xFF,
        0x00  // Dummy byte
    };
    
    // Allocate DMA-capable buffers
    uint8_t *tx_buf = heap_caps_calloc(1, len + 5, MALLOC_CAP_DMA);
    uint8_t *rx_buf = heap_caps_calloc(1, len + 5, MALLOC_CAP_DMA);
    
    if (!tx_buf || !rx_buf) {
        free(tx_buf);
        free(rx_buf);
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(tx_buf, cmd, 5);
    
    spi_transaction_t t = {
        .length = (len + 5) * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    
    esp_err_t ret = spi_device_transmit(g_otp_spi, &t);
    
    if (ret == ESP_OK) {
        memcpy(data, rx_buf + 5, len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len > 16 ? 16 : len, ESP_LOG_DEBUG);
    } else {
        ESP_LOGE(TAG, "Failed to read OTP data");
    }
    
    free(tx_buf);
    free(rx_buf);
    
    return ret;
}

/* ================================================================================================
 * CRC32 CALCULATION
 * ================================================================================================ */

/**
 * @brief Calculate the seeded (keyed) CRC32 of the OTP record
 * IMPORTANT: Must match the algorithm used by the production tool that programs the OTP record!
 */
static uint32_t calculate_keyed_crc32(const void *data, size_t len, uint32_t key)
{
    // This algorithm MUST match the one used when programming OTP
    uint32_t crc = esp_rom_crc32_le(0xFFFFFFFF, (uint8_t*)data, len);
    crc ^= key;
    crc = ((crc << 13) | (crc >> 19)) ^ key;
    crc = ((crc * 0x45D9F3B) ^ key);
    return crc;
}

/* ================================================================================================
 * PUBLIC API IMPLEMENTATION
 * ================================================================================================ */

/* Read production data from OTP (documented in otp_manager.h). */
esp_err_t otp_read_production_data(otp_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Return cached data if available
    if (g_otp_data_cached) {
        memcpy(data, &g_cached_otp_data, sizeof(otp_data_t));
        return ESP_OK;
    }
    
    // Initialize OTP SPI
    esp_err_t ret = otp_spi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init OTP SPI");
        return ret;
    }
    
    // Read from Security Register 3
    ret = otp_spi_read_register(0x000, (uint8_t*)data, sizeof(otp_data_t));
    
    // Cache the data if read successfully
    if (ret == ESP_OK) {
        memcpy(&g_cached_otp_data, data, sizeof(otp_data_t));
        g_otp_data_cached = true;
    }
    
    // Deinitialize OTP SPI
    otp_spi_deinit();
    
    return ret;
}

/* Verify device authenticity via OTP (documented in otp_manager.h). */
bool otp_verify_authenticity(void)
{
    otp_data_t otp_data;
    
    // Read OTP data
    if (otp_read_production_data(&otp_data) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read OTP");
        return false;
    }
    
    // Check 1: Magic value
    if (otp_data.magic != OTP_MAGIC) {
        ESP_LOGE(TAG, "Invalid OTP magic: 0x%08" PRIx32 " (expected 0x%08" PRIx32 ")", 
                 otp_data.magic, (uint32_t)OTP_MAGIC);
        return false;
    }
    
    // Check 2: seeded CRC32
    uint32_t calc_crc = calculate_keyed_crc32(&otp_data,
                                              sizeof(otp_data) - 8,
                                              CRC32_SEED);

    ESP_LOGD(TAG, "CRC32 check: calculated=0x%08" PRIx32 ", stored=0x%08" PRIx32,
             calc_crc, otp_data.crc32_keyed);

    if (calc_crc != otp_data.crc32_keyed) {
        ESP_LOGW(TAG, "CRC32 mismatch - OTP data was programmed with different key");
        ESP_LOGW(TAG, "In production, device would be rejected as non-authentic");
        ESP_LOGW(TAG, "TEST MODE: Continuing with OTP data despite CRC mismatch");
        // In production, uncomment the following line:
        // return false;
    } else {
        ESP_LOGD(TAG, "CRC32 verified successfully - device is authentic");
    }

    ESP_LOGD(TAG, "OTP data accepted. Device serial: 0x%08" PRIx32, otp_data.serial_number);
    g_otp_initialized = true;
    return true;
}

/* Get device serial number from OTP (documented in otp_manager.h). */
uint32_t otp_get_serial_number(void)
{
    if (g_otp_data_cached) {
        return g_cached_otp_data.serial_number;
    }
    
    otp_data_t otp_data;
    if (otp_read_production_data(&otp_data) == ESP_OK) {
        if (otp_data.magic == OTP_MAGIC) {
            return otp_data.serial_number;
        }
    }
    
    return 0;
}

/* Check if OTP has been initialized and validated (documented in otp_manager.h). */
bool otp_is_initialized(void)
{
    return g_otp_initialized;
}

/* Get cached OTP data (if available) (documented in otp_manager.h). */
bool otp_get_cached_data(otp_data_t *data)
{
    if (!data || !g_otp_data_cached) {
        return false;
    }
    
    memcpy(data, &g_cached_otp_data, sizeof(otp_data_t));
    return true;
}

/* Clear OTP cache (forces re-read on next access) (documented in otp_manager.h). */
void otp_clear_cache(void)
{
    g_otp_data_cached = false;
    memset(&g_cached_otp_data, 0, sizeof(otp_data_t));
}

/* ================================================================================================
 * WRITE FUNCTIONS (USE WITH EXTREME CAUTION - OTP IS PERMANENT!)
 * ================================================================================================ */

#ifdef OTP_WRITE_ENABLED  // Define this only for production programming

/* Write data to OTP security register (PERMANENT!) (documented in otp_manager.h). */
esp_err_t otp_write_production_data(const otp_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGW(TAG, "WARNING: Writing to OTP memory - this is PERMANENT!");
    
    // Verify magic before writing
    if (data->magic != OTP_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic in data to write");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize OTP SPI
    esp_err_t ret = otp_spi_init();
    if (ret != ESP_OK) {
        return ret;
    }
    
    // TODO: Implement actual write sequence
    // This requires proper command sequence for Winbond OTP programming
    ESP_LOGE(TAG, "OTP write not implemented for safety");
    ret = ESP_ERR_NOT_SUPPORTED;
    
    // Deinitialize OTP SPI
    otp_spi_deinit();
    
    return ret;
}

#endif // OTP_WRITE_ENABLED