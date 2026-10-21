/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_transfer_types.h
 * @brief Transfer structures for optimized Flash data writing
 *
 * Structures for converting data from RAM to compact format for storage
 * in ring buffer files with minimized Flash memory wear.
 *
 * The rbpower_*_transfer_t structures are the pre-v4 record layouts; with
 * RBPOWER_RECORD_FORMAT_VERSION >= 4 the size and capacity tables refer to the
 * v4 records defined in rbstats_v4_types.h. The tables and lookup functions are
 * read-only and may be used from any task.
 */

#ifndef RBPOWER_STORAGE_TRANSFER_TYPES_H
#define RBPOWER_STORAGE_TRANSFER_TYPES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ================================================================================================
 * VERSIONING CONSTANTS AND MAGIC NUMBERS
 * ================================================================================================ */

/** @brief Storage data structure version, major (must match exactly to be compatible). */
#define RBPOWER_STORAGE_VERSION_MAJOR 1
/** @brief Storage data structure version, minor. */
#define RBPOWER_STORAGE_VERSION_MINOR 0
/** @brief Storage data structure version, patch. */
#define RBPOWER_STORAGE_VERSION_PATCH 0

/** @brief Magic of the global index ("RBID"). */
#define RBPOWER_GLOBAL_INDEX_MAGIC 0x52424944 // "RBID"
/** @brief Magic of the unit metadata block ("RBTU"). */
#define RBPOWER_UNIT_META_MAGIC 0x52425455    // "RBTU"
/** @brief Magic of the ring-buffer data file header ("RBST"). */
#define RBPOWER_FILE_HEADER_MAGIC 0x52425354  // "RBST"

/** @brief Minimum compatible major version. */
#define RBPOWER_MIN_COMPATIBLE_MAJOR 1
/** @brief Minimum compatible minor version. */
#define RBPOWER_MIN_COMPATIBLE_MINOR 0

/* ================================================================================================
 * DATA AND PERIOD TYPES
 * ================================================================================================ */

// Use unified types from rbpower_enums.h
#include "rbpower_enums.h"

    /** @brief Period type; alias of rbpower_period_t kept for backward compatibility. */
    typedef rbpower_period_t rbpower_period_type_t;

    /**
     * @brief Version structure (8 bytes)
     */
    typedef struct
    {
        uint16_t major;    ///< Major version
        uint16_t minor;    ///< Minor version
        uint16_t patch;    ///< Patch version
        uint16_t reserved; ///< Reserved (0)
    } rbpower_version_t;

    /* ================================================================================================
     * TRANSFER STRUCTURES FOR FLASH WRITING
     * ================================================================================================ */

    /**
     * @brief Electrical parameters for file writing
     *
     * Optimized structure for storing basic electrical parameters
     * with minimized write size (20 bytes instead of ~100 bytes RAM structure).
     */
    typedef struct
    {
        uint32_t timestamp;                                  ///< Record time, epoch s
        uint16_t voltage_rms_v10;                            ///< RMS voltage, V × 10; range 0-6553.5 V
        uint16_t current_rms_ma;                             ///< RMS current, mA; range 0-65.535 A
        int16_t power_active_w10;                            ///< Active power, W × 10; range ±3276.7 W
        int16_t power_reactive_var10;                        ///< Reactive power, var × 10
        uint16_t power_apparent_va10;                        ///< Apparent power, VA × 10
        int8_t power_factor_100;                             ///< Power factor × 100; range ±1.27
        uint8_t frequency_hz10;                              ///< Frequency, Hz; range 0-255 Hz (typically 50 or 60)
        uint16_t thd_voltage_100;                            ///< Voltage THD, % × 100; range 0-655.35 %
        uint16_t thd_current_100;                            ///< Current THD, % × 100
        // Per-period envelope min/max (shown as envelope bands in the web UI; the avg fields
        // above lose the spread, which in power monitoring is the point). Same scaling as the avg
        // fields. APPENDED at the end so the offsets of all existing fields stay stable.
        uint16_t voltage_min_v10;                            ///< Min RMS voltage in the period, V × 10
        uint16_t voltage_max_v10;                            ///< Max RMS voltage in the period, V × 10
        uint16_t current_min_ma;                             ///< Min RMS current in the period, mA
        uint16_t current_max_ma;                             ///< Max RMS current in the period, mA
        int16_t  power_min_w10;                              ///< Min active power in the period, W × 10
        int16_t  power_max_w10;                              ///< Max active power in the period, W × 10
    } __attribute__((packed)) rbpower_electrical_transfer_t; // 32 bytes (was 20; +12 for min/max envelope)

    /**
     * @brief Energy data for file writing
     *
     * Compact representation of energy metrics with support
     * for bidirectional metering (consumption + supply).
     */
    typedef struct
    {
        uint32_t timestamp;                              ///< Record time, epoch s
        uint32_t energy_consumed_wh;                     ///< Consumed energy, Wh (up to 4.2 GWh)
        uint32_t energy_supplied_wh;                     ///< Supplied energy, Wh (up to 4.2 GWh)
        int32_t energy_net_wh;                           ///< Net energy, Wh (±2.1 GWh)
    } __attribute__((packed)) rbpower_energy_transfer_t; // 16 bytes

    /**
     * @brief Economic indicators for file writing
     *
     * Money amounts in minor currency units (1/100 of the currency unit,
     * e.g. cents or kopecks).
     */
    typedef struct
    {
        uint32_t timestamp;                                 ///< Record time, epoch s
        uint32_t cost_consumed_kopecks;                     ///< Consumption cost, currency × 100
        uint32_t revenue_supplied_kopecks;                  ///< Supply revenue, currency × 100
        int32_t net_cost_kopecks;                           ///< Net cost, currency × 100
    } __attribute__((packed)) rbpower_economics_transfer_t; // 16 bytes

    /**
     * @brief Power quality for file writing
     *
     * Counters of power quality violation events.
     * Used only for MAIN_SUPPLY units.
     */
    typedef struct
    {
        uint32_t timestamp;                               ///< Record time, epoch s
        uint8_t voltage_events;                           ///< Voltage event counter (overvoltage/undervoltage)
        uint8_t frequency_events;                         ///< Frequency event counter (deviations from nominal)
        uint8_t thd_events;                               ///< THD event counter (threshold exceedances)
        uint8_t interruption_events;                      ///< Power interruption counter
    } __attribute__((packed)) rbpower_quality_transfer_t; // 8 bytes

    /**
     * @brief Current direction statistics (hour/day periods only)
     *
     * Detailed statistics of bidirectional unit operation (MAIN_SUPPLY, ALT_SUPPLY).
     * Not saved for month/year periods (space saving).
     */
    typedef struct
    {
        uint32_t timestamp;                                 ///< Record time, epoch s
        uint32_t time_consuming_sec;                        ///< Time spent consuming, s
        uint32_t time_supplying_sec;                        ///< Time spent supplying, s
        uint32_t time_idle_sec;                             ///< Idle/disconnected time, s
        uint16_t switch_count;                              ///< Number of direction switches
        uint8_t dominant_direction;                         ///< Dominant direction (0 = consumption, 1 = supply)
        uint8_t reserved;                                   ///< Structure alignment
        int32_t peak_consumption_w10;                       ///< Peak consumption, W × 10
        int32_t peak_supply_w10;                            ///< Peak supply, W × 10
        uint32_t avg_session_duration_sec;                  ///< Average session duration, s
    } __attribute__((packed)) rbpower_direction_transfer_t; // 32 bytes

    /* ================================================================================================
     * SERVICE STRUCTURES
     * ================================================================================================ */

    /**
     * @brief Transfer structure (on-flash record) size for each data type, bytes
     */
    extern const uint16_t RBPOWER_TRANSFER_SIZES[RBPOWER_DATA_TYPE_MAX];

    /**
     * @brief Logical ring-buffer capacity per period, records (public, for the API)
     */
    extern const uint32_t RBPOWER_RECORDS_COUNT[RBPOWER_PERIOD_MAX];

    /**
     * @brief Number of 4 KB sectors for each period + data type combination
     * Layout: [period][data_type] = number_of_sectors (0 = not stored)
     */
    extern const uint8_t RBPOWER_BUFFER_SECTORS[RBPOWER_PERIOD_MAX][RBPOWER_DATA_TYPE_MAX];

    /**
     * @brief Per-period selection of data files to create (5 bytes)
     */
    typedef struct
    {
        uint8_t save_electrical;                             ///< 1 = create electrical.dat (1 byte)
        uint8_t save_energy;                                 ///< 1 = create energy.dat (1 byte)
        uint8_t save_economics;                              ///< 1 = create economics.dat (1 byte)
        uint8_t save_quality;                                ///< 1 = create quality.dat (1 byte)
        uint8_t save_direction;                              ///< 1 = create direction.dat (1 byte)
    } __attribute__((packed)) rbpower_period_files_config_t; // Total: 5 bytes

    /**
     * @brief Versioning header
     */
    typedef struct
    {
        uint32_t magic;                         ///< Structure magic
        rbpower_version_t structure_version;    ///< Format version of the structure
        rbpower_version_t created_with_version; ///< Firmware storage version that created it
        uint32_t creation_timestamp;            ///< Creation time, s
        uint32_t crc32;                         ///< CRC32 of the header
    } rbpower_version_header_t;

    /* ================================================================================================
     * TRANSFER STRUCTURE FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Get the transfer structure size for a data type
     *
     * @param[in] data_type Data type
     * @return Structure size in bytes, 0 if the type is out of range
     */
    uint16_t rbpower_get_transfer_size(rbpower_data_type_t data_type);

    /**
     * @brief Get the logical ring-buffer capacity for a period
     *
     * @param[in] period_type Period type
     * @return Number of records in the ring buffer, 0 if the period is out of range
     */
    uint32_t rbpower_get_records_count(rbpower_period_type_t period_type);

    /**
     * @brief Check version compatibility
     *
     * Compatible means: same major version and minor >= RBPOWER_MIN_COMPATIBLE_MINOR.
     *
     * @param[in] version Version to check
     * @return true if the version is compatible; false otherwise or if version is NULL
     */
    bool rbpower_is_version_compatible(const rbpower_version_t *version);

    /**
     * @brief Get the current storage structure version
     *
     * @param[out] version Receives the version (no-op if NULL)
     */
    void rbpower_get_current_version(rbpower_version_t *version);

    /**
     * @brief Get the name of a data type
     *
     * @param[in] data_type Data type
     * @return Static string name (e.g. "energy"), or "unknown" if out of range
     */
    const char *rbpower_get_data_type_name(rbpower_data_type_t data_type);

    /**
     * @brief Get the name of a period
     *
     * @param[in] period_type Period type
     * @return Static string name (e.g. "hour"), or "unknown" if out of range
     */
    const char *rbpower_get_period_name(rbpower_period_type_t period_type);

    /**
     * @brief Get the number of sectors allocated to a ring buffer
     *
     * @param[in] period    Period
     * @param[in] data_type Data type
     * @return Number of 4 KB sectors (0 if the combination is not stored or out of range)
     */
    uint8_t rbpower_get_buffer_sectors(rbpower_period_t period, rbpower_data_type_t data_type);

    /**
     * @brief Calculate the physical ring-buffer capacity
     *
     * @param[in] period    Period
     * @param[in] data_type Data type
     * @return Physical capacity in records: sectors × (4096 / record size), including
     *         the extra wrap-around sector; 0 if the combination is not stored
     */
    uint32_t rbpower_get_physical_capacity(rbpower_period_t period, rbpower_data_type_t data_type);

    /**
     * @brief Calculate the ring-buffer size in bytes
     *
     * @param[in] period    Period
     * @param[in] data_type Data type
     * @return Buffer size in bytes (number_of_sectors × 4096), 0 if not stored
     */
    uint32_t rbpower_get_buffer_size_bytes(rbpower_period_t period, rbpower_data_type_t data_type);

#ifdef __cplusplus
}
#endif

#endif // RBPOWER_STORAGE_TRANSFER_TYPES_H