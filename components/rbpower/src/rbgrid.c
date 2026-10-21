/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid.c
 * @brief rbgrid - implementation of the unit_id helpers and the reserved-UID registry (see rbgrid.h).
 */

#include "rbgrid.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char* TAG = "RBGRID";

/* ================================================================================================
 * PRIVATE VARIABLES
 * ================================================================================================ */

/** rbgrid initialisation state */
static bool g_rbgrid_initialized = false;

/* ================================================================================================
 * EXTENDED STRUCTURES AND VARIABLES
 * ================================================================================================ */

/** unit_UID error callback */
static rbgrid_unit_uid_error_callback_t g_uid_error_callback = NULL;

/** Mapping usage statistics */
static rbgrid_unit_uid_mapping_stats_t g_mapping_stats = {0};

/** NVS handle (0 = not open) */
static nvs_handle_t g_nvs_handle = 0;
static const char* NVS_NAMESPACE = "rbgrid_map";
static const char* NVS_MAPPING_KEY = "uid_mapping";

/** Hash table for fast unit_UID lookup (0xFF = empty slot) */
#define UNIT_UID_HASH_SIZE 16
static uint8_t g_unit_uid_hash_table[UNIT_UID_HASH_SIZE][4];

/** Reserved unit_UIDs */
#define MAX_RESERVED_UIDS 32
static struct {
    rbgrid_unit_uid_t unit_UID;
    bool is_reserved;
} g_reserved_uids[MAX_RESERVED_UIDS] = {0};

/* ================================================================================================
 * UNIT_ID IDENTIFICATION - BASIC FUNCTIONS
 * ================================================================================================ */

rbgrid_unit_id_t rbgrid_create_adc_unit_id(uint8_t adc_channel) 
{
    // Validate the local channel index (0-7)
    if (adc_channel > 7) {
        ESP_LOGE(TAG, "Invalid ADC channel: %u (supported 0-7)", adc_channel);
        return RBGRID_INVALID_UNIT_UID;
    }

    // Create unit_id: type=ADC, domain=LOCAL, local_id=channel
    rbgrid_unit_id_t unit_id = RBGRID_MAKE_UNIT_ID(
        RBGRID_UNIT_TYPE_ADC,
        RBGRID_DOMAIN_LOCAL,
        adc_channel
    );

    ESP_LOGD(TAG, "ADC unit_id created: channel %u → 0x%04X", adc_channel, unit_id);
    return unit_id;
}

rbgrid_unit_id_t rbgrid_create_espnow_unit_id(const uint8_t device_mac[6], 
                                              uint8_t remote_channel)
{
    if (device_mac == NULL) {
        ESP_LOGE(TAG, "ESP-NOW: NULL MAC address");
        return RBGRID_INVALID_UNIT_UID;
    }

    // Domain = 6-bit hash of the MAC (simplified; distinct devices can collide —
    // a MAC → domain_id mapping table would avoid that)
    uint8_t domain_id = (device_mac[4] ^ device_mac[5]) & 0x3F; // 6 bits

    rbgrid_unit_id_t unit_id = RBGRID_MAKE_UNIT_ID(
        RBGRID_UNIT_TYPE_ESP_NOW,
        domain_id,
        remote_channel
    );

    ESP_LOGD(TAG, "ESP-NOW unit_id created: MAC %02X:%02X:%02X:%02X:%02X:%02X, channel %u → 0x%04X",
             device_mac[0], device_mac[1], device_mac[2],
             device_mac[3], device_mac[4], device_mac[5],
             remote_channel, unit_id);
    
    return unit_id;
}

rbgrid_unit_id_t rbgrid_create_spi_unit_id(uint8_t spi_bus, 
                                           uint8_t cs_gpio_pin,
                                           uint8_t spi_device_id)
{
    // Validate SPI bus
    if (spi_bus < 1 || spi_bus > 3) {
        ESP_LOGE(TAG, "Invalid SPI bus: %u (supported 1-3)", spi_bus);
        return RBGRID_INVALID_UNIT_UID;
    }

    // Validate GPIO (ESP32 has GPIO 0-39)
    if (cs_gpio_pin > 39) {
        ESP_LOGE(TAG, "Invalid GPIO pin: %u (supported 0-39)", cs_gpio_pin);
        return RBGRID_INVALID_UNIT_UID;
    }

    // Create domain for SPI: [5-4]=bus, [3-0]=GPIO%16
    uint8_t domain_id = ((spi_bus - 1) << 4) | (cs_gpio_pin & 0x0F);

    rbgrid_unit_id_t unit_id = RBGRID_MAKE_UNIT_ID(
        RBGRID_UNIT_TYPE_SPI,
        domain_id,
        spi_device_id
    );

    ESP_LOGD(TAG, "SPI unit_id created: bus %u, CS GPIO%u, device %u → 0x%04X",
             spi_bus, cs_gpio_pin, spi_device_id, unit_id);
    
    return unit_id;
}

rbgrid_unit_id_t rbgrid_create_virtual_unit_id(rbgrid_virtual_type_t virtual_type, 
                                               uint8_t instance)
{
    // Validate virtual device type
    if (virtual_type > RBGRID_VIRTUAL_TYPE_AGGREGATE) {
        ESP_LOGE(TAG, "Invalid virtual device type: %u", virtual_type);
        return RBGRID_INVALID_UNIT_UID;
    }

    // For virtual devices domain encodes type
    uint8_t domain_id = virtual_type;

    rbgrid_unit_id_t unit_id = RBGRID_MAKE_UNIT_ID(
        RBGRID_UNIT_TYPE_VIRTUAL,
        domain_id,
        instance
    );

    ESP_LOGD(TAG, "Virtual unit_id created: type %u, instance %u → 0x%04X",
             virtual_type, instance, unit_id);
    
    return unit_id;
}

/* ================================================================================================
 * VALIDATION AND DECODING
 * ================================================================================================ */

bool rbgrid_validate_unit_id(rbgrid_unit_id_t unit_id)
{
    // Reject the invalid marker
    if (unit_id == RBGRID_INVALID_UNIT_UID) {
        return false;
    }
    
    rbgrid_unit_type_t type = RBGRID_GET_TYPE(unit_id);
    uint8_t domain = RBGRID_GET_DOMAIN(unit_id);
    uint8_t local_id = RBGRID_GET_LOCAL(unit_id);
    
    // Per-type validation
    switch (type) {
        case RBGRID_UNIT_TYPE_ADC:
            // Local: domain must be LOCAL, local ID 0-7
            return (domain == RBGRID_DOMAIN_LOCAL) && (local_id <= 7);
            
        case RBGRID_UNIT_TYPE_ESP_NOW:
            // ESP-NOW: any domain, any local ID
            return true;
            
        case RBGRID_UNIT_TYPE_SPI:
            // SPI: domain upper bits carry bus 1-3
            {
                uint8_t spi_bus = (domain >> 4) + 1;
                return (spi_bus >= 1) && (spi_bus <= 3);
            }
            
        case RBGRID_UNIT_TYPE_VIRTUAL:
            // Virtual: domain = virtual device type
            return domain <= RBGRID_VIRTUAL_TYPE_AGGREGATE;
            
        default:
            return false;
    }
}

rbgrid_unit_type_t rbgrid_get_unit_type(rbgrid_unit_id_t unit_id)
{
    return (rbgrid_unit_type_t)RBGRID_GET_TYPE(unit_id);
}

bool rbgrid_is_local_unit(rbgrid_unit_id_t unit_id)
{
    rbgrid_unit_type_t type = rbgrid_get_unit_type(unit_id);
    
    // Local: TYPE local and virtual units
    return (type == RBGRID_UNIT_TYPE_ADC) || (type == RBGRID_UNIT_TYPE_VIRTUAL);
}

rbgrid_spi_info_t rbgrid_get_spi_info(rbgrid_unit_id_t unit_id)
{
    rbgrid_spi_info_t info = {0};
    
    // Check type
    if (RBGRID_GET_TYPE(unit_id) != RBGRID_UNIT_TYPE_SPI) {
        ESP_LOGE(TAG, "unit_id 0x%04X is not SPI device", unit_id);
        info.is_valid = false;
        return info;
    }

    uint8_t domain = RBGRID_GET_DOMAIN(unit_id);
    uint8_t local_id = RBGRID_GET_LOCAL(unit_id);

    // Extract information from domain
    info.spi_bus = (domain >> 4) + 1;      // Bits [5-4] → bus 1-3
    info.cs_gpio_pin = domain & 0x0F;      // Bits [3-0] → GPIO % 16
    info.device_id = local_id;             // Local ID = device ID

    // Additional validation
    info.is_valid = (info.spi_bus >= 1) && (info.spi_bus <= 3);
    
    return info;
}

char* rbgrid_format_unit_id(rbgrid_unit_id_t unit_id, char* buffer)
{
    if (buffer == NULL) {
        return NULL;
    }
    
    if (!rbgrid_validate_unit_id(unit_id)) {
        snprintf(buffer, 32, "INVALID:0x%04X", unit_id);
        return buffer;
    }
    
    rbgrid_unit_type_t type = rbgrid_get_unit_type(unit_id);
    uint8_t domain = RBGRID_GET_DOMAIN(unit_id);
    uint8_t local_id = RBGRID_GET_LOCAL(unit_id);
    
    switch (type) {
        case RBGRID_UNIT_TYPE_ADC:
            snprintf(buffer, 32, "ADC:CH%u", local_id);
            break;
            
        case RBGRID_UNIT_TYPE_ESP_NOW:
            snprintf(buffer, 32, "ESP-NOW:D%u:CH%u", domain, local_id);
            break;
            
        case RBGRID_UNIT_TYPE_SPI:
            {
                rbgrid_spi_info_t spi = rbgrid_get_spi_info(unit_id);
                snprintf(buffer, 32, "SPI%u:GPIO%u:DEV%u", 
                         spi.spi_bus, spi.cs_gpio_pin, spi.device_id);
            }
            break;
            
        case RBGRID_UNIT_TYPE_VIRTUAL:
            {
                const char* vtype_names[] = {"ZONE", "TOTAL", "CALC", "AGGR"};
                const char* vtype_name = (domain < 4) ? vtype_names[domain] : "UNKNOWN";
                snprintf(buffer, 32, "VIRTUAL:%s:%u", vtype_name, local_id);
            }
            break;
            
        default:
            snprintf(buffer, 32, "UNKNOWN:0x%04X", unit_id);
            break;
    }
    
    return buffer;
}

/* ================================================================================================
 * STATE HELPERS
 * ================================================================================================ */

/**
 * @brief Get the rbgrid initialisation state.
 * @return true if rbgrid_init_system() has completed and rbgrid_deinit_system() has not been called.
 */
bool rbgrid_is_initialized(void)
{
    return g_rbgrid_initialized;
}

/**
 * @brief Set the initialisation state (internal).
 * @param[in] initialized New state.
 */
void rbgrid_set_initialized(bool initialized)
{
    g_rbgrid_initialized = initialized;
    ESP_LOGD(TAG, "rbgrid state: %s", initialized ? "initialized" : "deinitialized");
}

/* ================================================================================================
 * INTERNAL HELPERS
 * ================================================================================================ */

/**
 * @brief Hash of a unit_UID into UNIT_UID_HASH_SIZE buckets.
 */
static uint8_t unit_uid_hash(rbgrid_unit_uid_t unit_UID) {
    return (unit_UID ^ (unit_UID >> 8)) & (UNIT_UID_HASH_SIZE - 1);
}

/**
 * @brief Invoke the registered unit_UID error callback (if any) and log the error.
 */
static void trigger_uid_error_callback(rbgrid_unit_uid_t unit_UID, 
                                      rbgrid_unit_uid_err_t error,
                                      const char* description) {
    if (g_uid_error_callback) {
        g_uid_error_callback(unit_UID, error, description);
    }
    ESP_LOGE(TAG, "unit_UID Error 0x%04X: %s (%d)", unit_UID, description, error);
}

/* ================================================================================================
 * ERROR HANDLING
 * ================================================================================================ */

/*
 * Register the unit_UID error callback.
 */
rbgrid_err_t rbgrid_register_uid_error_callback(rbgrid_unit_uid_error_callback_t callback)
{
    g_uid_error_callback = callback;
    ESP_LOGD(TAG, "UID error callback %s", callback ? "registered" : "disabled");
    return RBGRID_OK;
}

/*
 * Text description of a unit_UID error code.
 */
const char* rbgrid_get_uid_error_description(rbgrid_unit_uid_err_t error)
{
    switch (error) {
        case RBGRID_ERR_UNIT_UID_INVALID:     return "Invalid unit_UID";
        case RBGRID_ERR_UNIT_UID_CONFLICT:    return "unit_UID conflict";
        case RBGRID_ERR_UNIT_UID_TABLE_FULL:  return "Mapping table full";
        case RBGRID_ERR_UNIT_UID_NOT_MAPPED:  return "unit_UID not found in mapping";
        case RBGRID_ERR_UNIT_UID_RTC_CORRUPT: return "RTC data corrupted";
        case RBGRID_ERR_UNIT_UID_HASH_FULL:   return "Hash table overflow";
        case RBGRID_ERR_UNIT_UID_NVS_FAILED:  return "NVS operation error";
        default:                              return "Unknown error";
    }
}

/* ================================================================================================
 * DIAGNOSTIC AND DEBUG UTILITIES
 * ================================================================================================ */

/*
 * Print the unit_UID mapping table (placeholder; needs access to the rbhome.c unit map).
 */
void rbgrid_print_unit_uid_mapping_table(void)
{
    ESP_LOGI(TAG, "=== Unit UID Mapping Table ===");
    ESP_LOGW(TAG, "Function requires integration with rbhome.c for access to g_unit_id_map");
    ESP_LOGI(TAG, "=============================");
}

/*
 * Detect duplicate reserved unit_UIDs.
 */
bool rbgrid_check_unit_uid_conflicts(void)
{
    // Check reserved UIDs for conflicts
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (!g_reserved_uids[i].is_reserved) continue;

        for (int j = i + 1; j < MAX_RESERVED_UIDS; j++) {
            if (!g_reserved_uids[j].is_reserved) continue;

            if (g_reserved_uids[i].unit_UID == g_reserved_uids[j].unit_UID) {
                trigger_uid_error_callback(g_reserved_uids[i].unit_UID,
                                         RBGRID_ERR_UNIT_UID_CONFLICT,
                                         "Duplicated reserved UID");
                return false;
            }
        }
    }

    ESP_LOGD(TAG, "No unit_UID conflicts found");
    return true;
}

/*
 * Detailed information about a unit_UID.
 */
rbgrid_err_t rbgrid_get_unit_uid_info(rbgrid_unit_uid_t unit_UID, rbgrid_unit_uid_info_t* info)
{
    if (!info) {
        return RBGRID_ERR_INVALID_ARG;
    }

    // Validate unit_UID
    if (!rbgrid_validate_unit_id(unit_UID)) {
        trigger_uid_error_callback(unit_UID, RBGRID_ERR_UNIT_UID_INVALID, "Invalid unit_UID");
        return RBGRID_ERR_UNIT_UID_INVALID;
    }

    // Fill information
    info->unit_UID = unit_UID;
    info->type = rbgrid_get_unit_type(unit_UID);
    info->domain = RBGRID_GET_DOMAIN(unit_UID);
    info->local_id = RBGRID_GET_LOCAL(unit_UID);
    info->is_active = false; // Requires integration with rbhome.c

    // Check if UID is reserved
    info->is_reserved = false;
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (g_reserved_uids[i].is_reserved && g_reserved_uids[i].unit_UID == unit_UID) {
            info->is_reserved = true;
            break;
        }
    }

    // Format name
    rbgrid_format_unit_id(unit_UID, info->formatted_name);

    return RBGRID_OK;
}

/*
 * unit_UID mapping usage statistics.
 */
rbgrid_err_t rbgrid_get_uid_mapping_stats(rbgrid_unit_uid_mapping_stats_t* stats)
{
    if (!stats) {
        return RBGRID_ERR_INVALID_ARG;
    }

    // Initialize statistics
    memset(stats, 0, sizeof(rbgrid_unit_uid_mapping_stats_t));

    // Count reserved UIDs
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (g_reserved_uids[i].is_reserved) {
            stats->reserved_uids++;

            rbgrid_unit_type_t type = rbgrid_get_unit_type(g_reserved_uids[i].unit_UID);
            if (type < 4) {
                stats->units_by_type[type]++;
            }
        }
    }

    // Count hash table collisions
    for (int i = 0; i < UNIT_UID_HASH_SIZE; i++) {
        uint8_t bucket_count = 0;
        for (int j = 0; j < 4; j++) {
            if (g_unit_uid_hash_table[i][j] != 0xFF) {
                bucket_count++;
            }
        }
        if (bucket_count > 1) {
            stats->hash_collisions += (bucket_count - 1);
        }
    }

    ESP_LOGD(TAG, "UID mapping stats: reserved=%d, collisions=%d",
             stats->reserved_uids, stats->hash_collisions);

    return RBGRID_OK;
}

/*
 * Format a unit_UID for display.
 */
char* rbgrid_format_unit_uid(rbgrid_unit_uid_t unit_UID, char* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 32) {
        return NULL;
    }
    
    // Use existing formatting function
    return rbgrid_format_unit_id(unit_UID, buffer);
}

/* ================================================================================================
 * NVS SUPPORT
 * ================================================================================================ */

/*
 * Open the registry's NVS namespace.
 */
rbgrid_err_t rbgrid_init_nvs_storage(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open error: %s", esp_err_to_name(err));
        trigger_uid_error_callback(0, RBGRID_ERR_UNIT_UID_NVS_FAILED, "Failed to open NVS");
        return RBGRID_ERR_UNIT_UID_NVS_FAILED;
    }

    ESP_LOGD(TAG, "NVS storage initialized");
    return RBGRID_OK;
}

/*
 * Close the registry's NVS handle.
 */
rbgrid_err_t rbgrid_deinit_nvs_storage(void)
{
    if (g_nvs_handle != 0) {
        nvs_close(g_nvs_handle);
        g_nvs_handle = 0;
        ESP_LOGD(TAG, "NVS storage closed");
    }
    return RBGRID_OK;
}

/*
 * Save the reserved-UID registry to NVS.
 */
rbgrid_err_t rbgrid_backup_uid_mapping_to_nvs(void)
{
    if (g_nvs_handle == 0) {
        return RBGRID_ERR_UNIT_UID_NVS_FAILED;
    }

    // Save reserved UIDs to NVS
    esp_err_t err = nvs_set_blob(g_nvs_handle, "reserved_uids",
                                g_reserved_uids, sizeof(g_reserved_uids));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving reserved UIDs to NVS: %s", esp_err_to_name(err));
        return RBGRID_ERR_UNIT_UID_NVS_FAILED;
    }

    err = nvs_commit(g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit error: %s", esp_err_to_name(err));
        return RBGRID_ERR_UNIT_UID_NVS_FAILED;
    }

    ESP_LOGD(TAG, "UID mapping saved to NVS");
    return RBGRID_OK;
}

/*
 * Restore the reserved-UID registry from NVS.
 */
rbgrid_err_t rbgrid_restore_uid_mapping_from_nvs(void)
{
    if (g_nvs_handle == 0) {
        return RBGRID_ERR_UNIT_UID_NVS_FAILED;
    }

    size_t required_size = sizeof(g_reserved_uids);
    esp_err_t err = nvs_get_blob(g_nvs_handle, "reserved_uids",
                                g_reserved_uids, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Mapping data not found in NVS - using default values");
        memset(g_reserved_uids, 0, sizeof(g_reserved_uids));
        return RBGRID_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading mapping from NVS: %s", esp_err_to_name(err));
        return RBGRID_ERR_UNIT_UID_NVS_FAILED;
    }

    ESP_LOGD(TAG, "UID mapping restored from NVS");
    return RBGRID_OK;
}

/* ================================================================================================
 * ADDITIONAL UNIT_UID UTILITIES
 * ================================================================================================ */

/*
 * Generate a new unreserved unit_UID.
 */
rbgrid_unit_uid_t rbgrid_generate_unique_uid(rbgrid_unit_type_t type, uint8_t domain)
{
    for (uint8_t local_id = 0; local_id < 255; local_id++) {
        rbgrid_unit_uid_t candidate = RBGRID_MAKE_UNIT_ID(type, domain, local_id);

        if (rbgrid_is_uid_unique(candidate)) {
            ESP_LOGD(TAG, "Generated unique UID: 0x%04X", candidate);
            return candidate;
        }
    }

    trigger_uid_error_callback(0, RBGRID_ERR_UNIT_UID_TABLE_FULL,
                              "Failed to find unique UID in specified range");
    return RBGRID_INVALID_UNIT_UID;
}

/*
 * Check that a unit_UID is not reserved.
 */
bool rbgrid_is_uid_unique(rbgrid_unit_uid_t unit_UID)
{
    // Check among reserved UIDs
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (g_reserved_uids[i].is_reserved && g_reserved_uids[i].unit_UID == unit_UID) {
            return false;
        }
    }

    // TODO: Need integration with rbhome.c to check active UIDs
    return true;
}

/*
 * List free unit_UIDs in a TYPE/DOMAIN range.
 */
rbgrid_err_t rbgrid_get_free_uids_in_range(rbgrid_unit_type_t type, uint8_t domain,
                                          rbgrid_unit_uid_t* free_uids,
                                          uint8_t* count, uint8_t max_count)
{
    if (!free_uids || !count) {
        return RBGRID_ERR_INVALID_ARG;
    }

    *count = 0;

    for (uint8_t local_id = 0; local_id < 255 && *count < max_count; local_id++) {
        rbgrid_unit_uid_t candidate = RBGRID_MAKE_UNIT_ID(type, domain, local_id);

        if (rbgrid_is_uid_unique(candidate)) {
            free_uids[*count] = candidate;
            (*count)++;
        }
    }

    ESP_LOGD(TAG, "Found %d free UIDs in range type=%d, domain=%d", *count, type, domain);
    return RBGRID_OK;
}

/*
 * Reserve a unit_UID.
 */
rbgrid_err_t rbgrid_reserve_uid(rbgrid_unit_uid_t unit_UID)
{
    // Validate UID
    if (!rbgrid_validate_unit_id(unit_UID)) {
        return RBGRID_ERR_UNIT_UID_INVALID;
    }

    // Check if already reserved
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (g_reserved_uids[i].is_reserved && g_reserved_uids[i].unit_UID == unit_UID) {
            return RBGRID_ERR_UNIT_UID_CONFLICT;
        }
    }

    // Find free slot
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (!g_reserved_uids[i].is_reserved) {
            g_reserved_uids[i].unit_UID = unit_UID;
            g_reserved_uids[i].is_reserved = true;

            ESP_LOGD(TAG, "UID 0x%04X reserved", unit_UID);

            // Save to NVS
            rbgrid_backup_uid_mapping_to_nvs();

            return RBGRID_OK;
        }
    }

    return RBGRID_ERR_UNIT_UID_TABLE_FULL;
}

/*
 * Release a reserved unit_UID.
 */
rbgrid_err_t rbgrid_release_uid(rbgrid_unit_uid_t unit_UID)
{
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (g_reserved_uids[i].is_reserved && g_reserved_uids[i].unit_UID == unit_UID) {
            g_reserved_uids[i].is_reserved = false;
            g_reserved_uids[i].unit_UID = 0;

            ESP_LOGD(TAG, "UID 0x%04X released", unit_UID);

            // Save to NVS
            rbgrid_backup_uid_mapping_to_nvs();

            return RBGRID_OK;
        }
    }

    return RBGRID_ERR_UNIT_UID_NOT_MAPPED;
}

/*
 * Migrate a unit_UID (not implemented).
 */
rbgrid_err_t rbgrid_migrate_uid(rbgrid_unit_uid_t old_uid, rbgrid_unit_uid_t new_uid)
{
    ESP_LOGW(TAG, "UID migration 0x%04X → 0x%04X (function not implemented)", old_uid, new_uid);
    return RBGRID_ERR_CONFIG_INVALID;
}

/* ================================================================================================
 * SYSTEM INITIALISATION
 * ================================================================================================ */

/*
 * Initialise the rbgrid layer with NVS persistence.
 */
rbgrid_err_t rbgrid_init_system(void)
{
    ESP_LOGD(TAG, "Initializing rbgrid system...");

    // Initialize NVS
    rbgrid_err_t err = rbgrid_init_nvs_storage();
    if (err != RBGRID_OK) {
        ESP_LOGE(TAG, "NVS initialization error: %d", err);
        return err;
    }

    // Restore data from NVS
    err = rbgrid_restore_uid_mapping_from_nvs();
    if (err != RBGRID_OK) {
        ESP_LOGW(TAG, "Failed to restore mapping from NVS: %d", err);
    }

    // Initialize hash table
    memset(g_unit_uid_hash_table, 0xFF, sizeof(g_unit_uid_hash_table));

    // Reset statistics
    memset(&g_mapping_stats, 0, sizeof(g_mapping_stats));

    // Check data integrity
    rbgrid_uid_mapping_integrity_t integrity;
    err = rbgrid_validate_uid_mapping(&integrity);
    if (err == RBGRID_OK) {
        if (!integrity.mapping_consistent) {
            ESP_LOGW(TAG, "Mapping issues detected - running auto-repair");
            rbgrid_repair_uid_mapping();
        }
    }

    rbgrid_set_initialized(true);
    ESP_LOGI(TAG, "rbgrid system initialized successfully");
    return RBGRID_OK;
}

/*
 * Deinitialise the rbgrid layer.
 */
rbgrid_err_t rbgrid_deinit_system(void)
{
    ESP_LOGD(TAG, "Deinitializing rbgrid system...");

    // Save data to NVS before exit
    rbgrid_backup_uid_mapping_to_nvs();

    // Close NVS
    rbgrid_deinit_nvs_storage();

    // Reset callback
    g_uid_error_callback = NULL;

    // Clear reserved UIDs
    memset(g_reserved_uids, 0, sizeof(g_reserved_uids));

    rbgrid_set_initialized(false);
    ESP_LOGI(TAG, "rbgrid system deinitialized");
    return RBGRID_OK;
}

/* ================================================================================================
 * RECOVERY AND SYNCHRONISATION (basic implementations)
 * ================================================================================================ */

/*
 * Rebuild the unit_UID mapping (placeholder).
 */
rbgrid_err_t rbgrid_rebuild_unit_uid_mapping(void)
{
    ESP_LOGW(TAG, "Rebuild mapping (requires integration with rbhome.c)");
    return RBGRID_OK;
}

/*
 * Validate the integrity of the reserved-UID registry.
 */
rbgrid_err_t rbgrid_validate_uid_mapping(rbgrid_uid_mapping_integrity_t* result)
{
    if (!result) {
        return RBGRID_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(rbgrid_uid_mapping_integrity_t));

    // Check NVS data
    result->nvs_data_valid = (g_nvs_handle != 0);

    // Basic check of reserved UIDs
    result->mapping_consistent = rbgrid_check_unit_uid_conflicts();

    ESP_LOGD(TAG, "Mapping validation: consistent=%d, nvs_valid=%d",
             result->mapping_consistent, result->nvs_data_valid);

    return RBGRID_OK;
}

/*
 * Synchronise the registry with NVS.
 */
rbgrid_err_t rbgrid_sync_uid_mapping_with_nvs(void)
{
    return rbgrid_backup_uid_mapping_to_nvs();
}

/*
 * Remove duplicate reserved UIDs and save.
 */
rbgrid_err_t rbgrid_repair_uid_mapping(void)
{
    ESP_LOGD(TAG, "Auto-repairing mapping...");

    // Clear duplicated reserved UIDs
    for (int i = 0; i < MAX_RESERVED_UIDS; i++) {
        if (!g_reserved_uids[i].is_reserved) continue;

        for (int j = i + 1; j < MAX_RESERVED_UIDS; j++) {
            if (g_reserved_uids[j].is_reserved &&
                g_reserved_uids[i].unit_UID == g_reserved_uids[j].unit_UID) {

                ESP_LOGW(TAG, "Found duplicated UID 0x%04X - removing", g_reserved_uids[j].unit_UID);
                g_reserved_uids[j].is_reserved = false;
                g_reserved_uids[j].unit_UID = 0;
            }
        }
    }

    // Save repaired data
    rbgrid_backup_uid_mapping_to_nvs();

    ESP_LOGD(TAG, "Mapping repair completed");
    return RBGRID_OK;
}