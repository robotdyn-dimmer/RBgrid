/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file otp_manager.h
 * @brief Production record in the OTP security register of the external NOR chip: read and verify.
 *
 * External W25Q-class NOR chips carry a 256-byte production record (serial number, batch, hardware
 * revision, keyed CRC) in a one-time-programmable security register. This module reads it over a
 * temporary SPI device on the external chip's bus and checks the magic value and keyed CRC.
 * It applies only to the external-NOR medium; the internal flash has no OTP record.
 *
 * Calling context: the read temporarily takes over the external chip's SPI bus, so call these
 * functions only from the storage init path (external_flash_init()), before the chip is used
 * for data. Not thread-safe.
 */

#ifndef OTP_MANAGER_H
#define OTP_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Production record stored in the OTP security register of the external NOR chip (256 bytes, packed).
     *
     * Written once at production time. The keyed CRC algorithm used by the production tool that
     * writes the record must match the one in otp_manager.c that verifies it.
     */
    typedef struct
    {
        uint32_t magic;            ///< 0x50524F44 ("PROD" in little-endian)
        uint32_t version;          ///< Structure version
        uint32_t chip_id;          ///< Flash chip JEDEC ID
        uint32_t production_date;  ///< Production date, YYYYMMDD as a decimal number
        uint32_t serial_number;    ///< Unique device serial number
        uint32_t batch_number;     ///< Production batch number
        uint32_t hw_revision;      ///< Hardware revision
        uint32_t factory_id;       ///< Factory/manufacturer ID
        uint8_t product_model[16]; ///< Product model string (NUL-padded)
        uint32_t validation_key;   ///< Validation key
        uint32_t secret_key;       ///< Secret key for encryption
        uint8_t reserved_keys[24]; ///< Reserved for future keys
        uint32_t flash_size;       ///< Flash size, bytes
        uint32_t partition_count;  ///< Number of partitions
        uint32_t fs_initialized;   ///< Filesystem initialization flag
        uint32_t reserved_params;  ///< Reserved parameters
        uint8_t reserved[152];     ///< Reserved for future use
        uint32_t crc32_keyed;      ///< Keyed CRC32 over all preceding bytes (record size minus 8)
        uint32_t crc32_key_id;     ///< ID of the key used for crc32_keyed
    } __attribute__((packed)) otp_data_t;

    /**
     * @brief Read the production record from the OTP security register (cached after the first successful read).
     *
     * @param[out] data Structure to fill with the OTP record.
     * @retval ESP_OK              Record read (or returned from the cache). Its content is not validated here.
     * @retval ESP_ERR_INVALID_ARG data is NULL.
     * @retval ESP_ERR_NO_MEM      No DMA-capable memory for the SPI transfer.
     * @retval other               SPI driver error (adding the device or the transfer).
     */
    esp_err_t otp_read_production_data(otp_data_t *data);

    /**
     * @brief Verify device authenticity using the OTP record.
     *
     * Reads the record and checks the magic value and the keyed CRC32. In the current firmware a
     * CRC mismatch is only logged as a warning and the record is still accepted (test mode).
     *
     * @return true if the record was read and its magic is valid (marks OTP as initialized);
     *         false if the read failed or the magic is wrong.
     */
    bool otp_verify_authenticity(void);

    /**
     * @brief Get the device serial number from the OTP record.
     *
     * @return Device serial number, or 0 if the record cannot be read or has a wrong magic.
     */
    uint32_t otp_get_serial_number(void);

    /**
     * @brief Check whether the OTP record has been verified by otp_verify_authenticity().
     *
     * @return true if verification succeeded, false otherwise.
     */
    bool otp_is_initialized(void);

    /**
     * @brief Get the cached OTP record without accessing the chip.
     *
     * @param[out] data Structure to fill with the cached record.
     * @return true if cached data was available and copied; false if there is no cache or data is NULL.
     */
    bool otp_get_cached_data(otp_data_t *data);

    /**
     * @brief Clear the OTP cache (the next access re-reads the chip).
     */
    void otp_clear_cache(void);

#ifdef OTP_WRITE_ENABLED
    /**
     * @brief Write the production record to OTP (PERMANENT; production programming only).
     *
     * @warning OTP memory can be written only once. The write sequence is not implemented in this
     *          firmware; the function validates its input and returns ESP_ERR_NOT_SUPPORTED.
     *
     * @param[in] data Record to write (magic must be valid).
     * @retval ESP_ERR_INVALID_ARG   data is NULL or has a wrong magic.
     * @retval ESP_ERR_NOT_SUPPORTED Write sequence not implemented.
     * @retval other                 SPI driver error.
     */
    esp_err_t otp_write_production_data(const otp_data_t *data);
#endif

#ifdef __cplusplus
}
#endif

#endif // OTP_MANAGER_H