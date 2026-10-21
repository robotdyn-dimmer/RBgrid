/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid.h
 * @brief rbgrid - unit_id encoding helpers and the reserved-UID registry.
 *
 * High-level helpers over the rbpower library: build, validate, decode and format the 16-bit
 * hierarchical unit_id, plus a small registry of reserved UIDs persisted in NVS
 * (namespace "rbgrid_map").
 *
 * Thread safety: the unit_id helpers are pure and reentrant. The reserved-UID registry and the
 * NVS handle are module globals without a lock; call those functions from one task (normally
 * during initialisation).
 */

#ifndef RBGRID_H
#define RBGRID_H

#ifdef __cplusplus
extern "C"
{
#endif

/* ================================================================================================
 * INCLUDES
 * ================================================================================================ */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rbpower.h" // Base library

/* ================================================================================================
 * VERSION AND CONSTANTS
 * ================================================================================================ */
#define RBGRID_VERSION_MAJOR 1    ///< rbgrid API major version
#define RBGRID_VERSION_MINOR 0    ///< rbgrid API minor version
#define RBGRID_VERSION_PATCH 0    ///< rbgrid API patch version
#define RBGRID_VERSION "1.0.0"    ///< rbgrid API version string

#define RBGRID_MAX_UNITS 64            ///< Maximum units in the system
#define RBGRID_MAX_UNIT_NAME_LEN 32    ///< Maximum unit name length, chars
#define RBGRID_INVALID_UNIT_UID 0xFFFF ///< Invalid unit_id

    /* ================================================================================================
     * ERROR CODES
     * ================================================================================================ */
    /** @brief rbgrid error codes. */
    typedef enum
    {
        RBGRID_OK = 0,                        ///< Successful execution
        RBGRID_ERR_INVALID_ARG = -2001,       ///< Invalid argument
        RBGRID_ERR_NOT_INIT = -2002,          ///< System not initialized
        RBGRID_ERR_ALREADY_INIT = -2003,      ///< System already initialized
        RBGRID_ERR_NO_MEMORY = -2004,         ///< Out of memory
        RBGRID_ERR_UNIT_NOT_FOUND = -2005,    ///< Unit not found
        RBGRID_ERR_UNIT_EXISTS = -2006,       ///< Unit already exists
        RBGRID_ERR_RESOURCE_CONFLICT = -2007, ///< Resource conflict
        RBGRID_ERR_CONFIG_INVALID = -2008,    ///< Invalid configuration
        RBGRID_ERR_RBPOWER_FAILED = -2009     ///< rbpower error
    } rbgrid_err_t;

    /* ================================================================================================
     * UNIT_ID IDENTIFICATION SYSTEM
     * ================================================================================================ */

    /**
     * @brief Hierarchical device identifier (16 bits)
     *
     * Bit structure:
     * [15-14] TYPE   - Data source type (local, ESP-NOW, external bus, virtual)
     * [13-8]  DOMAIN - Domain/bus (64 variants)
     * [7-0]   LOCAL  - Local ID in domain (256 variants)
     */
    typedef uint16_t rbgrid_unit_id_t;

    /**
     * @brief Data source types
     */
    typedef enum
    {
        RBGRID_UNIT_TYPE_ADC = 0,     ///< Local unit (00 in bits 15-14); also used by the predefined voltage-bus and main-supply ids
        RBGRID_UNIT_TYPE_ESP_NOW = 1, ///< ESP-NOW remote devices (01)
        RBGRID_UNIT_TYPE_SPI = 2,     ///< External bus device (SPI or I2C, 10); the DOMAIN
                                      ///< field distinguishes the bus (SPI1-3 vs I2C0+).
        RBGRID_UNIT_TYPE_VIRTUAL = 3  ///< Virtual/calculated (11)
    } rbgrid_unit_type_t;

/** @brief External-bus alias (SPI and I2C share TYPE=2; DOMAIN selects the bus). */
#define RBGRID_UNIT_TYPE_EXTBUS RBGRID_UNIT_TYPE_SPI

    /**
     * @brief Virtual device types
     */
    typedef enum
    {
        RBGRID_VIRTUAL_TYPE_ZONE = 0,   ///< Zone device group
        RBGRID_VIRTUAL_TYPE_TOTAL,      ///< Total system
        RBGRID_VIRTUAL_TYPE_CALCULATED, ///< Calculated metric
        RBGRID_VIRTUAL_TYPE_AGGREGATE   ///< Aggregated metric
    } rbgrid_virtual_type_t;

#define RBGRID_TYPE_MASK 0xC000   ///< unit_id mask of TYPE (bits 15-14)
#define RBGRID_DOMAIN_MASK 0x3F00 ///< unit_id mask of DOMAIN (bits 13-8)
#define RBGRID_LOCAL_MASK 0x00FF  ///< unit_id mask of LOCAL (bits 7-0)

#define RBGRID_TYPE_SHIFT 14      ///< unit_id shift of TYPE
#define RBGRID_DOMAIN_SHIFT 8     ///< unit_id shift of DOMAIN
#define RBGRID_LOCAL_SHIFT 0      ///< unit_id shift of LOCAL

#define RBGRID_DOMAIN_LOCAL 0x00 ///< Domain: the local ESP32 device
#define RBGRID_DOMAIN_SPI1 0x01  ///< Domain: SPI1 bus
#define RBGRID_DOMAIN_SPI2 0x02  ///< Domain: SPI2 bus (HSPI)
#define RBGRID_DOMAIN_SPI3 0x03  ///< Domain: SPI3 bus (VSPI)
#define RBGRID_DOMAIN_I2C0 0x10  ///< Domain: I2C bus 0 (rbAmp and other I2C sensors)
#define RBGRID_DOMAIN_I2C1 0x11  ///< Domain: I2C bus 1

/* ================================================================================================
 * MACROS FOR UNIT_ID OPERATIONS
 * ================================================================================================ */

/** @brief Extract the TYPE field (rbgrid_unit_type_t) of a unit_id. */
#define RBGRID_GET_TYPE(unit_id) (((unit_id) & RBGRID_TYPE_MASK) >> RBGRID_TYPE_SHIFT)
/** @brief Extract the DOMAIN field (0..63) of a unit_id. */
#define RBGRID_GET_DOMAIN(unit_id) (((unit_id) & RBGRID_DOMAIN_MASK) >> RBGRID_DOMAIN_SHIFT)
/** @brief Extract the LOCAL field (0..255) of a unit_id. */
#define RBGRID_GET_LOCAL(unit_id) (((unit_id) & RBGRID_LOCAL_MASK) >> RBGRID_LOCAL_SHIFT)

/** @brief Build a unit_id from TYPE (2 bits), DOMAIN (6 bits) and LOCAL (8 bits); excess bits are masked. */
#define RBGRID_MAKE_UNIT_ID(type, domain, local)  \
    ((((type) & 0x03) << RBGRID_TYPE_SHIFT) |     \
     (((domain) & 0x3F) << RBGRID_DOMAIN_SHIFT) | \
     (((local) & 0xFF) << RBGRID_LOCAL_SHIFT))

/** @brief True if the unit_id has the local (TYPE=0) type. */
#define RBGRID_IS_ADC_UNIT(unit_id) (RBGRID_GET_TYPE(unit_id) == RBGRID_UNIT_TYPE_ADC)
/** @brief True if the unit_id belongs to an ESP-NOW remote device. */
#define RBGRID_IS_ESP_NOW_UNIT(unit_id) (RBGRID_GET_TYPE(unit_id) == RBGRID_UNIT_TYPE_ESP_NOW)
/** @brief True if the unit_id is a virtual/calculated unit. */
#define RBGRID_IS_VIRTUAL_UNIT(unit_id) (RBGRID_GET_TYPE(unit_id) == RBGRID_UNIT_TYPE_VIRTUAL)
/** @brief True if the unit_id is an I2C unit (external-bus TYPE with DOMAIN >= RBGRID_DOMAIN_I2C0). */
#define RBGRID_IS_I2C_UNIT(unit_id) (RBGRID_GET_TYPE(unit_id) == RBGRID_UNIT_TYPE_EXTBUS && \
                                     RBGRID_GET_DOMAIN(unit_id) >= RBGRID_DOMAIN_I2C0)
/** @brief True if the unit_id is an SPI unit (external-bus TYPE with DOMAIN < RBGRID_DOMAIN_I2C0). */
#define RBGRID_IS_SPI_UNIT(unit_id) (RBGRID_GET_TYPE(unit_id) == RBGRID_UNIT_TYPE_SPI && \
                                     RBGRID_GET_DOMAIN(unit_id) < RBGRID_DOMAIN_I2C0)

/**
 * @brief Build an rbAmp/I2C unit_id: bus (0..15) -> DOMAIN (I2C0 + bus), (module<<4 | channel) -> LOCAL.
 *
 * Example: module 0 (address 0x50), channel 0 on I2C bus 0 = RBGRID_MAKE_I2C_UNIT_ID(0, 0, 0).
 */
#define RBGRID_MAKE_I2C_UNIT_ID(bus, module, channel)                       \
    RBGRID_MAKE_UNIT_ID(RBGRID_UNIT_TYPE_EXTBUS,                            \
                        (RBGRID_DOMAIN_I2C0 + ((bus) & 0x0F)),             \
                        ((((module) & 0x0F) << 4) | ((channel) & 0x0F)))
/** @brief Module index (0..15) of an I2C unit_id. */
#define RBGRID_I2C_GET_MODULE(unit_id)  ((RBGRID_GET_LOCAL(unit_id) >> 4) & 0x0F)
/** @brief Channel index (0..15) of an I2C unit_id. */
#define RBGRID_I2C_GET_CHANNEL(unit_id) (RBGRID_GET_LOCAL(unit_id) & 0x0F)

/** @brief Predefined unit_id of the voltage bus (0x0007). */
#define RBGRID_UNIT_VOLTAGE_BUS RBGRID_MAKE_UNIT_ID(RBGRID_UNIT_TYPE_ADC, RBGRID_DOMAIN_LOCAL, 7)
/** @brief Predefined unit_id of the main supply (0x0006). */
#define RBGRID_UNIT_MAIN_SUPPLY RBGRID_MAKE_UNIT_ID(RBGRID_UNIT_TYPE_ADC, RBGRID_DOMAIN_LOCAL, 6)
/** @brief Predefined unit_id of the virtual whole-system total. */
#define RBGRID_UNIT_TOTAL_SYSTEM RBGRID_MAKE_UNIT_ID(RBGRID_UNIT_TYPE_VIRTUAL, 0, RBGRID_VIRTUAL_TYPE_TOTAL)

    /* ================================================================================================
     * HELPER STRUCTURES
     * ================================================================================================ */

    /**
     * @brief SPI device information
     */
    typedef struct
    {
        uint8_t spi_bus;     ///< SPI bus number (1, 2, 3)
        uint8_t cs_gpio_pin; ///< CS GPIO pin modulo 16 (only 4 bits are encoded in the unit_id)
        uint8_t device_id;   ///< Device ID on the bus (0-255)
        bool is_valid;       ///< true if the unit_id is an SPI unit with a valid bus number
    } rbgrid_spi_info_t;

    /* ================================================================================================
     * MAIN API FUNCTIONS
     * ================================================================================================ */

    /**
     * @brief Create a local (TYPE=0, DOMAIN=LOCAL) unit_id.
     * @param[in] adc_channel Local channel index (0-7).
     * @return The unit_id, or RBGRID_INVALID_UNIT_UID if @p adc_channel > 7.
     */
    rbgrid_unit_id_t rbgrid_create_adc_unit_id(uint8_t adc_channel);

    /**
     * @brief Create an ESP-NOW unit_id.
     *
     * The DOMAIN is a 6-bit hash of the MAC (mac[4] ^ mac[5]); different devices can collide.
     *
     * @param[in] device_mac     Remote device MAC address (6 bytes).
     * @param[in] remote_channel Channel on the remote device (0-255).
     * @return The unit_id, or RBGRID_INVALID_UNIT_UID if @p device_mac is NULL.
     */
    rbgrid_unit_id_t rbgrid_create_espnow_unit_id(const uint8_t device_mac[6],
                                                  uint8_t remote_channel);

    /**
     * @brief Create an SPI unit_id (DOMAIN = [5-4] bus-1, [3-0] CS GPIO modulo 16).
     * @param[in] spi_bus       SPI bus number (1, 2, 3).
     * @param[in] cs_gpio_pin   CS GPIO pin (0-39).
     * @param[in] spi_device_id Device ID on the bus (0-255).
     * @return The unit_id, or RBGRID_INVALID_UNIT_UID if the bus or GPIO is out of range.
     */
    rbgrid_unit_id_t rbgrid_create_spi_unit_id(uint8_t spi_bus,
                                               uint8_t cs_gpio_pin,
                                               uint8_t spi_device_id);

    /**
     * @brief Create a virtual unit_id (DOMAIN = virtual type).
     * @param[in] virtual_type Virtual device type.
     * @param[in] instance     Instance number (0-255).
     * @return The unit_id, or RBGRID_INVALID_UNIT_UID if @p virtual_type is out of range.
     */
    rbgrid_unit_id_t rbgrid_create_virtual_unit_id(rbgrid_virtual_type_t virtual_type,
                                                   uint8_t instance);

    /**
     * @brief Validate a unit_id against the per-type encoding rules.
     * @param[in] unit_id Identifier to check.
     * @return true if @p unit_id is valid, false otherwise (including RBGRID_INVALID_UNIT_UID).
     */
    bool rbgrid_validate_unit_id(rbgrid_unit_id_t unit_id);

    /**
     * @brief Get the unit type (TYPE field).
     * @param[in] unit_id Identifier.
     * @return Data source type.
     */
    rbgrid_unit_type_t rbgrid_get_unit_type(rbgrid_unit_id_t unit_id);

    /**
     * @brief Check whether a unit is local to the hub.
     * @param[in] unit_id Identifier.
     * @return true if the unit is local (TYPE local or virtual), false otherwise.
     */
    bool rbgrid_is_local_unit(rbgrid_unit_id_t unit_id);

    /**
     * @brief Extract SPI device information from a unit_id.
     * @param[in] unit_id SPI device identifier.
     * @return Decoded SPI information; rbgrid_spi_info_t::is_valid is false if @p unit_id is not an SPI unit.
     */
    rbgrid_spi_info_t rbgrid_get_spi_info(rbgrid_unit_id_t unit_id);

    /**
     * @brief Format a unit_id as a readable string (e.g. "SPI2:GPIO5:DEV1", "VIRTUAL:TOTAL:0").
     * @param[in]  unit_id Identifier.
     * @param[out] buffer  Buffer for the result, at least 32 bytes.
     * @return @p buffer, or NULL if @p buffer is NULL. An invalid id is formatted as "INVALID:0xNNNN".
     */
    char *rbgrid_format_unit_id(rbgrid_unit_id_t unit_id, char *buffer);

    /* ================================================================================================
     * EXTENDED TYPES FOR UNIT_UID MANAGEMENT
     * ================================================================================================ */

    /** @brief Alias of rbgrid_unit_id_t used by the UID-registry API. */
    typedef rbgrid_unit_id_t rbgrid_unit_uid_t;

    /**
     * @brief Extended error codes for unit_UID
     */
    typedef enum
    {
        RBGRID_ERR_UNIT_UID_INVALID = -3001,     ///< Invalid unit_UID
        RBGRID_ERR_UNIT_UID_CONFLICT = -3002,    ///< unit_UID conflict
        RBGRID_ERR_UNIT_UID_TABLE_FULL = -3003,  ///< Mapping table full
        RBGRID_ERR_UNIT_UID_NOT_MAPPED = -3004,  ///< unit_UID not found in mapping
        RBGRID_ERR_UNIT_UID_RTC_CORRUPT = -3005, ///< RTC data corrupted
        RBGRID_ERR_UNIT_UID_HASH_FULL = -3006,   ///< Hash table full
        RBGRID_ERR_UNIT_UID_NVS_FAILED = -3007   ///< NVS operation error
    } rbgrid_unit_uid_err_t;

    /**
     * @brief Callback for unit_UID error notifications.
     *
     * Called synchronously from the task that detected the error.
     *
     * @param[in] unit_UID    Affected unit_UID (0 if not unit-specific).
     * @param[in] error       Error code.
     * @param[in] description Static human-readable description.
     */
    typedef void (*rbgrid_unit_uid_error_callback_t)(rbgrid_unit_uid_t unit_UID,
                                                     rbgrid_unit_uid_err_t error,
                                                     const char *description);

    /**
     * @brief Detailed unit_UID information
     */
    typedef struct
    {
        rbgrid_unit_uid_t unit_UID;  ///< The unit_UID described
        rbgrid_unit_type_t type;     ///< TYPE field
        uint8_t domain;              ///< DOMAIN field
        uint8_t local_id;            ///< LOCAL field
        bool is_active;              ///< Unit is active (currently always false: not linked to the unit registry)
        bool is_reserved;            ///< unit_UID is in the reserved-UID registry
        char formatted_name[32];     ///< Output of rbgrid_format_unit_id()
    } rbgrid_unit_uid_info_t;

    /**
     * @brief unit_UID mapping usage statistics
     */
    typedef struct
    {
        uint8_t total_units;         ///< Total number of units
        uint8_t active_units;        ///< Active units
        uint8_t units_by_type[4];    ///< Count per rbgrid_unit_type_t
        uint8_t mapping_table_usage; ///< Mapping table usage, %
        uint8_t hash_collisions;     ///< Number of hash table collisions
        uint8_t max_search_depth;    ///< Maximum search depth
        uint8_t reserved_uids;       ///< Number of reserved UIDs
    } rbgrid_unit_uid_mapping_stats_t;

    /**
     * @brief Mapping integrity check
     */
    typedef struct
    {
        bool mapping_consistent;  ///< Mapping is consistent (no duplicate reserved UIDs)
        uint8_t orphaned_indices; ///< Indices without unit_UID
        uint8_t missing_mappings; ///< unit_UIDs without indices
        uint8_t duplicate_uids;   ///< Duplicate unit_UIDs
        bool nvs_data_valid;      ///< NVS storage is open
    } rbgrid_uid_mapping_integrity_t;

    /* ================================================================================================
     * EXTENDED UNIT_UID MANAGEMENT FUNCTIONS
     * ================================================================================================ */

    /* --- Error handling --- */

    /**
     * @brief Register (or clear) the unit_UID error callback.
     * @param[in] callback Callback, or NULL to disable notifications.
     * @return RBGRID_OK (always).
     */
    rbgrid_err_t rbgrid_register_uid_error_callback(rbgrid_unit_uid_error_callback_t callback);

    /**
     * @brief Get a static text description of a unit_UID error code.
     * @param[in] error Error code.
     * @return Static NUL-terminated string ("Unknown error" for unknown codes); never NULL.
     */
    const char *rbgrid_get_uid_error_description(rbgrid_unit_uid_err_t error);

    /* --- Diagnostic and debug utilities --- */

    /**
     * @brief Print the unit_UID mapping table to the log.
     * @note Placeholder: the mapping table lives in the unit registry and is not printed yet.
     */
    void rbgrid_print_unit_uid_mapping_table(void);

    /**
     * @brief Check the reserved-UID registry for duplicates; reports a conflict through the error callback.
     * @return true if there are no conflicts, false if a duplicate reserved UID was found.
     */
    bool rbgrid_check_unit_uid_conflicts(void);

    /**
     * @brief Get detailed information about a unit_UID.
     * @param[in]  unit_UID Identifier.
     * @param[out] info     Receives the decoded fields, reservation state and formatted name.
     * @retval RBGRID_OK                   Success.
     * @retval RBGRID_ERR_INVALID_ARG      @p info is NULL.
     * @retval RBGRID_ERR_UNIT_UID_INVALID @p unit_UID fails rbgrid_validate_unit_id() (the error callback is invoked).
     */
    rbgrid_err_t rbgrid_get_unit_uid_info(rbgrid_unit_uid_t unit_UID, rbgrid_unit_uid_info_t *info);

    /**
     * @brief Get usage statistics of the reserved-UID registry and its hash table.
     * @param[out] stats Receives the statistics (only reserved_uids, units_by_type and hash_collisions are filled).
     * @retval RBGRID_OK              Success.
     * @retval RBGRID_ERR_INVALID_ARG @p stats is NULL.
     */
    rbgrid_err_t rbgrid_get_uid_mapping_stats(rbgrid_unit_uid_mapping_stats_t *stats);

    /**
     * @brief Format a unit_UID for display (bounded wrapper of rbgrid_format_unit_id()).
     * @param[in]  unit_UID    Identifier.
     * @param[out] buffer      Output buffer.
     * @param[in]  buffer_size Size of @p buffer in bytes; must be at least 32.
     * @return @p buffer, or NULL if @p buffer is NULL or @p buffer_size < 32.
     */
    char *rbgrid_format_unit_uid(rbgrid_unit_uid_t unit_UID, char *buffer, size_t buffer_size);

    /* --- Recovery and synchronisation --- */

    /**
     * @brief Rebuild the unit_UID mapping.
     * @note Placeholder: only logs a warning.
     * @return RBGRID_OK (always).
     */
    rbgrid_err_t rbgrid_rebuild_unit_uid_mapping(void);

    /**
     * @brief Check the integrity of the reserved-UID registry.
     * @param[out] result Receives the integrity report (mapping_consistent and nvs_data_valid are filled).
     * @retval RBGRID_OK              Success.
     * @retval RBGRID_ERR_INVALID_ARG @p result is NULL.
     */
    rbgrid_err_t rbgrid_validate_uid_mapping(rbgrid_uid_mapping_integrity_t *result);

    /**
     * @brief Synchronise the registry with NVS (same as rbgrid_backup_uid_mapping_to_nvs()).
     * @retval RBGRID_OK                      Success.
     * @retval RBGRID_ERR_UNIT_UID_NVS_FAILED NVS is not open, or the write/commit failed.
     */
    rbgrid_err_t rbgrid_sync_uid_mapping_with_nvs(void);

    /**
     * @brief Remove duplicate entries from the reserved-UID registry and save it to NVS.
     * @return RBGRID_OK (always; an NVS save failure is ignored).
     */
    rbgrid_err_t rbgrid_repair_uid_mapping(void);

    /**
     * @brief Save the reserved-UID registry to NVS (blob "reserved_uids") and commit.
     * @retval RBGRID_OK                      Success.
     * @retval RBGRID_ERR_UNIT_UID_NVS_FAILED NVS is not open, or the write/commit failed.
     */
    rbgrid_err_t rbgrid_backup_uid_mapping_to_nvs(void);

    /**
     * @brief Load the reserved-UID registry from NVS; a missing blob resets it to empty.
     * @retval RBGRID_OK                      Success (also when no data was stored).
     * @retval RBGRID_ERR_UNIT_UID_NVS_FAILED NVS is not open, or the read failed.
     */
    rbgrid_err_t rbgrid_restore_uid_mapping_from_nvs(void);

    /* --- Additional unit_UID utilities --- */

    /**
     * @brief Find the first unit_UID with the given TYPE and DOMAIN that is not reserved.
     * @param[in] type   TYPE field.
     * @param[in] domain DOMAIN field.
     * @return A free unit_UID, or RBGRID_INVALID_UNIT_UID if LOCAL 0..254 are all reserved
     *         (the error callback is invoked with RBGRID_ERR_UNIT_UID_TABLE_FULL).
     */
    rbgrid_unit_uid_t rbgrid_generate_unique_uid(rbgrid_unit_type_t type, uint8_t domain);

    /**
     * @brief Check whether a unit_UID is free.
     * @note Only the reserved-UID registry is checked, not the active unit registry.
     * @param[in] unit_UID Identifier.
     * @return true if @p unit_UID is not reserved, false otherwise.
     */
    bool rbgrid_is_uid_unique(rbgrid_unit_uid_t unit_UID);

    /**
     * @brief List free (unreserved) unit_UIDs with the given TYPE and DOMAIN.
     * @param[in]  type      TYPE field.
     * @param[in]  domain    DOMAIN field.
     * @param[out] free_uids Array of at least @p max_count entries; receives the free UIDs.
     * @param[out] count     Receives the number of entries written.
     * @param[in]  max_count Capacity of @p free_uids.
     * @retval RBGRID_OK              Success.
     * @retval RBGRID_ERR_INVALID_ARG @p free_uids or @p count is NULL.
     */
    rbgrid_err_t rbgrid_get_free_uids_in_range(rbgrid_unit_type_t type, uint8_t domain,
                                               rbgrid_unit_uid_t *free_uids,
                                               uint8_t *count, uint8_t max_count);

    /**
     * @brief Reserve a unit_UID and save the registry to NVS.
     * @param[in] unit_UID Identifier to reserve.
     * @retval RBGRID_OK                      Reserved.
     * @retval RBGRID_ERR_UNIT_UID_INVALID    @p unit_UID fails rbgrid_validate_unit_id().
     * @retval RBGRID_ERR_UNIT_UID_CONFLICT   Already reserved.
     * @retval RBGRID_ERR_UNIT_UID_TABLE_FULL All 32 reservation slots are used.
     */
    rbgrid_err_t rbgrid_reserve_uid(rbgrid_unit_uid_t unit_UID);

    /**
     * @brief Release a reserved unit_UID and save the registry to NVS.
     * @param[in] unit_UID Identifier to release.
     * @retval RBGRID_OK                      Released.
     * @retval RBGRID_ERR_UNIT_UID_NOT_MAPPED @p unit_UID was not reserved.
     */
    rbgrid_err_t rbgrid_release_uid(rbgrid_unit_uid_t unit_UID);

    /**
     * @brief Migrate a unit from one unit_UID to another.
     * @note Not implemented.
     * @param[in] old_uid Current identifier.
     * @param[in] new_uid New identifier.
     * @return RBGRID_ERR_CONFIG_INVALID (always).
     */
    rbgrid_err_t rbgrid_migrate_uid(rbgrid_unit_uid_t old_uid, rbgrid_unit_uid_t new_uid);

    /* --- System initialisation --- */

    /**
     * @brief Initialise the rbgrid layer: open NVS, restore the reserved-UID registry, check and repair it.
     *
     * Requires nvs_flash_init() to have been called.
     *
     * @retval RBGRID_OK                      Success (a failed registry restore is only logged).
     * @retval RBGRID_ERR_UNIT_UID_NVS_FAILED The NVS namespace could not be opened.
     */
    rbgrid_err_t rbgrid_init_system(void);

    /**
     * @brief Deinitialise the rbgrid layer: save the registry, close NVS, clear the callback and registry.
     * @return RBGRID_OK (always).
     */
    rbgrid_err_t rbgrid_deinit_system(void);

    /* --- NVS support --- */

    /**
     * @brief Open the NVS namespace used by the registry ("rbgrid_map").
     * @retval RBGRID_OK                      Success.
     * @retval RBGRID_ERR_UNIT_UID_NVS_FAILED nvs_open() failed (the error callback is invoked).
     */
    rbgrid_err_t rbgrid_init_nvs_storage(void);

    /**
     * @brief Close the registry's NVS handle if open.
     * @return RBGRID_OK (always).
     */
    rbgrid_err_t rbgrid_deinit_nvs_storage(void);

/* ================================================================================================
 * UTILITY MACROS
 * ================================================================================================ */

/** @brief True if @p err is RBGRID_OK. */
#define RBGRID_SUCCESS(err) ((err) == RBGRID_OK)

/** @brief True if @p err is not RBGRID_OK. */
#define RBGRID_FAILED(err) ((err) != RBGRID_OK)

#ifdef __cplusplus
}
#endif

#endif /* RBGRID_H */