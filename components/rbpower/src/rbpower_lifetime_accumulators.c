/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_lifetime_accumulators.c
 * @brief Daily/lifetime energy and cost accumulators with RTC-memory and NVS backup.
 *
 * - O(1) unit access through the existing unit-ID map of rbhome.c;
 * - graceful degradation when the RTC backup is invalid;
 * - integration with the rbpower 3-second periods;
 * - daily reset at local midnight (SNTP time).
 *
 * Persistence chain: RTC slow memory (survives software resets) -> NVS snapshot
 * (survives power cycles) -> start from zero.
 */

#include "rbpower_lifetime_accumulators.h"
#include "rbpower_lifetime_api.h"
#include "rbhome_types.h"
#include "rbhome_main_supply.h"
#include "rbhome_alt_supply.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_crc.h>
#include <esp_attr.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <nvs.h>        /* flash snapshot of the totalizers (survives a power cycle, unlike RTC) */

//=============================================================================
// CONSTANTS AND MACROS
//=============================================================================

static const char* TAG = "LIFETIME_ACCUM";

#define RBPOWER_LIFETIME_CRC_SEED     0x12345678U   ///< Seed of the RTC image CRC32

//=============================================================================
// GLOBAL VARIABLES
//=============================================================================

/* RTC image of the totalizers. It must be RTC_NOINIT_ATTR, NOT RTC_DATA_ATTR: the bootloader
 * reloads the .rtc.data segment from the firmware image on EVERY start (except deep-sleep wake),
 * so with RTC_DATA_ATTR any esp_restart/OTA/crash wiped the image (magic mismatch) and the restore
 * fell back to the NVS snapshot, losing up to an hour of exact energy. The bootloader leaves
 * .rtc_noinit alone, so the image survives a software reset; after a power cut it holds garbage,
 * which the regular magic+CRC check rejects (-> NVS -> seed). No initializer is allowed here. */
RTC_NOINIT_ATTR static rbpower_rtc_backup_t g_rtc_backup;

// Subsystem statistics (regular RAM)
static rbpower_lifetime_stats_t g_system_stats = {0};
static bool g_system_initialized = false;
/* Where the boot restore came from (reported by the etstat diagnostics):
 * 0 = cold start / float seed only, 1 = RTC backup, 2 = NVS snapshot. */
static uint8_t g_restore_src = 0;
uint8_t rbpower_lifetime_restore_src(void) { return g_restore_src; }
/* Init diagnostics: copy of the RTC image magic/units_count taken BEFORE validation, so the
 * restore decision is visible; read through rbpower_lifetime_init_diag(). */
static uint32_t s_init_diag_magic;
/* Wall-clock ts of the snapshot the daily_* fields were restored from (0 = nothing restored).
 * Read by rbpower_lifetime_check_day_rollover() — a midnight that passed while the hub was down. */
static uint32_t s_restored_snapshot_ts;
/* NVS snapshot throttle (60 s) + dirty flag: the 15-min costing books the day cost right AFTER the
 * energy sync persisted, so the quarter's cost was only in RTC memory and a cold reset lost it.
 * The 3-s tick flushes a dirty image after 10 s. */
static int64_t  s_last_nvs_persist_us = 0;
static bool     s_nvs_dirty = false;

uint32_t rbpower_lifetime_restored_snapshot_ts(void) { return s_restored_snapshot_ts; }

void rbpower_lifetime_nvs_tick(void)
{
    if (!g_system_initialized || !s_nvs_dirty) return;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_nvs_persist_us < 10LL * 1000000LL) return;
    if (rbpower_lifetime_persist_to_nvs() == RBPOWER_OK) {
        s_last_nvs_persist_us = now_us;
        s_nvs_dirty = false;
    }
}
static uint8_t  s_init_diag_count;
/**
 * @brief Init diagnostics: RTC image state seen at boot and the chosen restore source.
 * @param[out] magic RTC image magic before validation (may be NULL).
 * @param[out] count RTC image units_count before validation (may be NULL).
 * @param[out] src   Restore source, see rbpower_lifetime_restore_src() (may be NULL).
 */
void rbpower_lifetime_init_diag(uint32_t* magic, uint8_t* count, int* src)
{
    if (magic) *magic = s_init_diag_magic;
    if (count) *count = s_init_diag_count;
    if (src)   *src   = g_restore_src;
}


// External variables from rbhome.c (reuse the existing unit-ID map)
/** @brief Unit-ID -> index map owned by rbhome.c. */
extern struct {
    rbpower_unit_id_t unit_id;   ///< Unit ID
    uint8_t unit_index;          ///< Index in g_home_network.units
    bool is_valid;               ///< Entry in use
} g_unit_id_map[RBPOWER_MAX_HOME_UNITS];

/** @brief Global home network (units, supply structures) owned by rbhome.c. */
extern rbpower_electrical_network_t g_home_network;

//=============================================================================
// PRIVATE FUNCTIONS - UTILITIES
//=============================================================================

/* Unit lookup uses the existing map of rbhome.c (find_unit_index_by_id(), declared in rbhome_types.h). */

/**
 * @brief Check whether a unit is the main or alternative supply unit.
 */
static bool is_supply_unit(rbpower_unit_id_t unit_id) {
    return (unit_id == g_home_network.system.main_supply_unit_id ||
            unit_id == g_home_network.system.alternative_supply_unit_id);
}

/**
 * @brief Current wall-clock time, epoch s.
 */
static uint32_t get_current_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

/**
 * @brief CRC32 over the valid unit records of the RTC image.
 */
static uint32_t calculate_rtc_crc32(void) {
    const uint8_t* data = (const uint8_t*)&g_rtc_backup.units[0];
    size_t data_size = sizeof(rbpower_rtc_unit_data_t) * g_rtc_backup.units_count;
    
    return esp_crc32_le(RBPOWER_LIFETIME_CRC_SEED, data, data_size);
}

/**
 * @brief Validate the RTC image (magic, unit count, CRC32) and log the result.
 */
static bool validate_rtc_data(void) {
    ESP_LOGI(TAG, "=== RTC VALIDATION START ===");
    ESP_LOGI(TAG, "RTC backup address: %p", (void*)&g_rtc_backup);
    ESP_LOGI(TAG, "Magic number: 0x%08lX (expected 0x%08lX)", 
             (unsigned long)g_rtc_backup.magic, (unsigned long)RBPOWER_LIFETIME_MAGIC);
    ESP_LOGI(TAG, "Units count: %d (max %d)", 
             g_rtc_backup.units_count, RBPOWER_LIFETIME_MAX_UNITS);
    ESP_LOGI(TAG, "Timestamp: %lu", (unsigned long)g_rtc_backup.timestamp_sntp);
    ESP_LOGI(TAG, "Stored CRC32: 0x%08lX", (unsigned long)g_rtc_backup.crc32);
    
    // Check the magic number
    if (g_rtc_backup.magic != RBPOWER_LIFETIME_MAGIC) {
        ESP_LOGW(TAG, "RTC magic mismatch: expected 0x%08lX, got 0x%08lX", 
                 (unsigned long)RBPOWER_LIFETIME_MAGIC, (unsigned long)g_rtc_backup.magic);
        ESP_LOGI(TAG, "=== RTC VALIDATION FAILED (MAGIC) ===");
        return false;
    }
    
    // Sanity-check the unit count
    if (g_rtc_backup.units_count > RBPOWER_LIFETIME_MAX_UNITS) {
        ESP_LOGW(TAG, "RTC units_count too large: %d > %d", 
                 g_rtc_backup.units_count, RBPOWER_LIFETIME_MAX_UNITS);
        ESP_LOGI(TAG, "=== RTC VALIDATION FAILED (UNITS_COUNT) ===");
        return false;
    }
    
    // Check CRC32
    uint32_t calculated_crc = calculate_rtc_crc32();
    ESP_LOGI(TAG, "Calculated CRC32: 0x%08lX", (unsigned long)calculated_crc);
    if (g_rtc_backup.crc32 != calculated_crc) {
        ESP_LOGW(TAG, "RTC CRC32 mismatch: expected 0x%08lX, got 0x%08lX",
                 (unsigned long)calculated_crc, (unsigned long)g_rtc_backup.crc32);
        ESP_LOGI(TAG, "=== RTC VALIDATION FAILED (CRC32) ===");
        return false;
    }
    
    ESP_LOGI(TAG, "=== RTC VALIDATION SUCCESS ===");
    ESP_LOGI(TAG, "RTC data valid: %d units, timestamp=%lu", 
             g_rtc_backup.units_count, (unsigned long)g_rtc_backup.timestamp_sntp);
    return true;
}

/**
 * @brief Reset the RTC image to an empty, valid state.
 */
static void clear_rtc_data(void) {
    memset(&g_rtc_backup, 0, sizeof(g_rtc_backup));
    g_rtc_backup.magic = RBPOWER_LIFETIME_MAGIC;
    g_rtc_backup.timestamp_sntp = get_current_timestamp();
    g_rtc_backup.units_count = 0;
    g_rtc_backup.crc32 = calculate_rtc_crc32();
    
    ESP_LOGI(TAG, "RTC data cleared and initialized");
}

//=============================================================================
// PRIVATE FUNCTIONS - STRUCTURE ACCESS
//=============================================================================

/**
 * @brief Regular accumulators of a unit, or NULL if the unit is unknown or undefined.
 */
static rbpower_lifetime_accumulators_basic_t* get_basic_accumulators(rbpower_unit_id_t unit_id) {
    uint8_t unit_index = find_unit_index_by_id(unit_id);
    if (unit_index == 0xFF || unit_index >= RBPOWER_MAX_HOME_UNITS) {
        ESP_LOGW(TAG, "Unit ID %d not found in mapping", unit_id);
        return NULL;
    }
    
    if (!g_home_network.units[unit_index].has_valid_definition) {
        ESP_LOGW(TAG, "Unit index %d has no valid definition", unit_index);
        return NULL;
    }
    
    return &g_home_network.units[unit_index].statistics.lifetime_accumulators;
}

/**
 * @brief Accumulators of the main supply structure, or NULL if there is none.
 */
static rbpower_lifetime_accumulators_supply_t* get_main_supply_accumulators(void) {
    if (!g_home_network.specialized.has_main_supply) {
        return NULL;
    }
    
    rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
    if (!main_supply) {
        ESP_LOGW(TAG, "Main supply pointer is NULL");
        return NULL;
    }
    
    return &main_supply->lifetime_accumulators;
}

/**
 * @brief Accumulators of the alternative supply structure, or NULL if there is none.
 */
static rbpower_lifetime_accumulators_supply_t* get_alt_supply_accumulators(void) {
    if (!g_home_network.specialized.has_alternative_supply) {
        return NULL;
    }
    
    rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
    if (!alt_supply) {
        ESP_LOGW(TAG, "Alt supply pointer is NULL");
        return NULL;
    }
    
    return &alt_supply->lifetime_accumulators;
}

//=============================================================================
// PUBLIC FUNCTIONS - INITIALIZATION
//=============================================================================

/*
 * Initialize the lifetime accumulator subsystem (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_init(void) {
    if (g_system_initialized) {
        ESP_LOGW(TAG, "System already initialized");
        return RBPOWER_OK;
    }
    
    ESP_LOGI(TAG, "Initializing lifetime accumulators system...");
    
    // Clear the statistics
    memset(&g_system_stats, 0, sizeof(g_system_stats));
    
    // Validate the RTC image
    s_init_diag_magic = g_rtc_backup.magic;
    s_init_diag_count = g_rtc_backup.units_count;
    if (validate_rtc_data()) {
        ESP_LOGI(TAG, "Valid RTC data found, will restore on demand");
        g_system_stats.rtc_valid = true;
        g_restore_src = 1;   /* rtc */
    } else {
        // Restore chain: the RTC image if valid, otherwise the LAST NVS record (not the maximum).
        // RTC empty/corrupt (power cycle, OTA with a layout change) -> last NVS snapshot.
        bool nvs_restored = false;
        nvs_handle_t h;
        if (nvs_open("rbltm", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(g_rtc_backup);
            if (nvs_get_blob(h, "snap", &g_rtc_backup, &len) == ESP_OK &&
                len == sizeof(g_rtc_backup) && validate_rtc_data()) {
                ESP_LOGW(TAG, "RTC invalid -> lifetime totalizers restored from NVS snapshot "
                              "(persisted ts=%lu; up to 1h of accumulation since is lost)",
                         (unsigned long)g_rtc_backup.timestamp_sntp);
                g_system_stats.rtc_valid = true;   /* the image in g_rtc_backup is valid -> regular restore */
                g_restore_src = 2;   /* nvs */
                nvs_restored = true;
            }
            nvs_close(h);
        }
        if (!nvs_restored) {
            ESP_LOGI(TAG, "No valid RTC data and no NVS snapshot, initializing from scratch");
            clear_rtc_data();
            g_system_stats.rtc_valid = false;
        }
    }
    
    g_system_stats.rtc_system_enabled = true;
    g_system_stats.rtc_backup_size_bytes = sizeof(rbpower_rtc_backup_t);
    
    g_system_initialized = true;
    
    ESP_LOGI(TAG, "Lifetime accumulators system initialized successfully");
    ESP_LOGI(TAG, "RTC backup size: %lu bytes", (unsigned long)g_system_stats.rtc_backup_size_bytes);
    
    return RBPOWER_OK;
}

/*
 * Restore the accumulators from the backup image at boot (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_restore_from_rtc(void) {
    if (!g_system_initialized) {
        ESP_LOGE(TAG, "System not initialized");
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    if (!g_system_stats.rtc_valid) {
        ESP_LOGW(TAG, "RTC data not valid, skipping restore");
        return RBPOWER_ERR_NO_DATA;
    }
    
    ESP_LOGI(TAG, "=== RESTORING LIFETIME DATA FROM RTC START ===");
    ESP_LOGI(TAG, "RTC backup address: %p", (void*)&g_rtc_backup);
    ESP_LOGI(TAG, "Units to restore: %d", g_rtc_backup.units_count);
    ESP_LOGI(TAG, "RTC timestamp: %lu", (unsigned long)g_rtc_backup.timestamp_sntp);
    
    uint32_t restored_units = 0;
    
    // Restore unit by unit
    for (uint16_t i = 0; i < g_rtc_backup.units_count; i++) {
        const rbpower_rtc_unit_data_t* rtc_unit = &g_rtc_backup.units[i];
        rbpower_unit_id_t unit_id = rtc_unit->unit_id;
        
        bool is_supply = (rtc_unit->flags & RBPOWER_RTC_FLAG_IS_SUPPLY_UNIT) != 0;

        if (is_supply) {
            // Restore a supply unit
            rbpower_lifetime_accumulators_supply_t* accumulators = NULL;

            if (unit_id == g_home_network.system.main_supply_unit_id) {
                accumulators = get_main_supply_accumulators();
            } else if (unit_id == g_home_network.system.alternative_supply_unit_id) {
                accumulators = get_alt_supply_accumulators();
            }

            /* Supply-flagged snapshot but no specialized struct on THIS boot (I2C-fed supply
             * unit, or a config change) — restore into the unit's BASIC accumulator instead of
             * dropping the energy (feeder/reader live there in that case). */
            if (!accumulators) {
                is_supply = false;
            }

            if (accumulators) {
                accumulators->daily_energy_consumed_wh = rtc_unit->daily_energy_consumed_wh;
                accumulators->daily_energy_supplied_wh = rtc_unit->daily_energy_supplied_wh;
                accumulators->daily_cost_consumed = rtc_unit->daily_cost_consumed;
                accumulators->daily_cost_supplied = rtc_unit->daily_cost_supplied;
                accumulators->total_energy_consumed_wh = rtc_unit->total_energy_consumed_wh;
                accumulators->total_energy_supplied_wh = rtc_unit->total_energy_supplied_wh;
                accumulators->total_cost_consumed = rtc_unit->total_cost_consumed;
                accumulators->total_cost_supplied = rtc_unit->total_cost_supplied;
                accumulators->et_in_mwh  = rtc_unit->et_in_mwh;
                accumulators->et_out_mwh = rtc_unit->et_out_mwh;
                /* Snapshot written before the exact totalizers existed (et empty, float totals
                 * non-zero) -> seed et from the float totals. */
                if (accumulators->et_in_mwh == 0.0 && accumulators->total_energy_consumed_wh > 0) {
                    accumulators->et_in_mwh = (double)accumulators->total_energy_consumed_wh * 1000.0;
                }
                if (accumulators->et_out_mwh == 0.0 && accumulators->total_energy_supplied_wh > 0) {
                    accumulators->et_out_mwh = (double)accumulators->total_energy_supplied_wh * 1000.0;
                }
                accumulators->last_rtc_sync_timestamp = g_rtc_backup.timestamp_sntp;
                accumulators->rtc_enabled = true;
                
                restored_units++;
                ESP_LOGI(TAG, "Restored supply unit %d: daily_consumed=%.2f Wh, total_consumed=%.2f Wh",
                         unit_id, accumulators->daily_energy_consumed_wh, accumulators->total_energy_consumed_wh);
            }
        }
        if (!is_supply) {
            // Restore a regular unit
            rbpower_lifetime_accumulators_basic_t* accumulators = get_basic_accumulators(unit_id);
            if (accumulators) {
                accumulators->daily_energy_consumed_wh = rtc_unit->daily_energy_consumed_wh;
                accumulators->daily_cost_consumed = rtc_unit->daily_cost_consumed;
                accumulators->total_energy_consumed_wh = rtc_unit->total_energy_consumed_wh;
                accumulators->total_cost_consumed = rtc_unit->total_cost_consumed;
                accumulators->et_in_mwh = rtc_unit->et_in_mwh;
                if (accumulators->et_in_mwh == 0.0 && accumulators->total_energy_consumed_wh > 0) {
                    accumulators->et_in_mwh = (double)accumulators->total_energy_consumed_wh * 1000.0;
                }
                accumulators->daily_energy_supplied_wh = rtc_unit->daily_energy_supplied_wh;
                accumulators->total_energy_supplied_wh = rtc_unit->total_energy_supplied_wh;
                accumulators->daily_cost_supplied      = rtc_unit->daily_cost_supplied;
                accumulators->total_cost_supplied      = rtc_unit->total_cost_supplied;
                accumulators->et_out_mwh               = rtc_unit->et_out_mwh;
                if (accumulators->et_out_mwh == 0.0 && accumulators->total_energy_supplied_wh > 0) {
                    accumulators->et_out_mwh = (double)accumulators->total_energy_supplied_wh * 1000.0;
                }
                accumulators->last_rtc_sync_timestamp = g_rtc_backup.timestamp_sntp;
                accumulators->rtc_enabled = true;
                
                restored_units++;
                ESP_LOGI(TAG, "Restored basic unit %d: daily_consumed=%.2f Wh, total_consumed=%.2f Wh",
                         unit_id, accumulators->daily_energy_consumed_wh, accumulators->total_energy_consumed_wh);
            }
        }
    }
    
    s_restored_snapshot_ts = g_rtc_backup.timestamp_sntp;
    ESP_LOGI(TAG, "=== RESTORING LIFETIME DATA FROM RTC COMPLETED ===");
    ESP_LOGI(TAG, "Restored %lu/%d lifetime accumulators from RTC", 
             (unsigned long)restored_units, g_rtc_backup.units_count);
    
    if (restored_units == 0) {
        ESP_LOGW(TAG, "No units were restored! Check unit mapping and RTC data validity");
    }
    
    g_system_stats.last_rtc_sync_time_us = esp_timer_get_time();
    
    return RBPOWER_OK;
}

//=============================================================================
// PUBLIC FUNCTIONS - DATA ACCUMULATION
//=============================================================================

/*
 * Accumulate consumption of a regular unit (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_accumulate_basic(rbpower_unit_id_t unit_id, 
                                              float energy_consumed_wh, 
                                              float cost_consumed) {
    if (!g_system_initialized) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    rbpower_lifetime_accumulators_basic_t* accumulators = get_basic_accumulators(unit_id);
    if (!accumulators) {
        ESP_LOGW(TAG, "Cannot get accumulators for unit %d", unit_id);
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Accumulating basic unit %d: +%.3f Wh, +%.4f EUR", 
             unit_id, energy_consumed_wh, cost_consumed);
    ESP_LOGD(TAG, "  Before: daily=%.3f Wh, total=%.1f Wh", 
             accumulators->daily_energy_consumed_wh, accumulators->total_energy_consumed_wh);
    
    // Accumulate
    accumulators->daily_energy_consumed_wh += energy_consumed_wh;
    accumulators->total_energy_consumed_wh += energy_consumed_wh;
    accumulators->daily_cost_consumed += cost_consumed;
    accumulators->total_cost_consumed += cost_consumed;
    accumulators->et_in_mwh += (double)energy_consumed_wh * 1000.0;   /* exact totalizer, mWh */
    
    ESP_LOGD(TAG, "  After: daily=%.3f Wh, total=%.1f Wh", 
             accumulators->daily_energy_consumed_wh, accumulators->total_energy_consumed_wh);
    
    // Update the statistics
    g_system_stats.total_accumulations++;
    g_system_stats.last_accumulation_time_us = esp_timer_get_time();
    
    // Sync to the RTC backup
    ESP_LOGD(TAG, "Triggering RTC sync for unit %d", unit_id);
    rbpower_lifetime_sync_to_rtc();
    
    return RBPOWER_OK;
}

/* Cost-only booking: energy for rbAmp-fed units arrives through the module
 * counters (accumulate_* with cost 0); the money is known only at the 15-min tariff costing, which
 * calls this. Supply units (main/alt) carry consumed AND supplied cost; basic units consumed only. */
rbpower_err_t rbpower_lifetime_add_cost(rbpower_unit_id_t unit_id, float cost_consumed, float cost_supplied)
{
    if (!g_system_initialized) return RBPOWER_ERR_INVALID_STATE;
    if (cost_consumed == 0.0f && cost_supplied == 0.0f) return RBPOWER_OK;
    if (unit_id == g_home_network.system.main_supply_unit_id ||
        unit_id == g_home_network.system.alternative_supply_unit_id) {
        rbpower_lifetime_accumulators_supply_t* sa = (unit_id == g_home_network.system.main_supply_unit_id)
                                                   ? get_main_supply_accumulators() : get_alt_supply_accumulators();
        if (!sa) return RBPOWER_ERR_INVALID_ARG;
        sa->daily_cost_consumed += cost_consumed;  sa->total_cost_consumed += cost_consumed;
        sa->daily_cost_supplied += cost_supplied;  sa->total_cost_supplied += cost_supplied;
    } else {
        rbpower_lifetime_accumulators_basic_t* ba = get_basic_accumulators(unit_id);
        if (!ba) return RBPOWER_ERR_INVALID_ARG;
        ba->daily_cost_consumed += cost_consumed;  ba->total_cost_consumed += cost_consumed;
        ba->daily_cost_supplied += cost_supplied;  ba->total_cost_supplied += cost_supplied;
    }
    rbpower_lifetime_sync_to_rtc();
    return RBPOWER_OK;
}

/*
 * Accumulate consumption and supply of a supply unit (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_accumulate_supply(rbpower_unit_id_t unit_id,
                                                float energy_consumed_wh,
                                                float energy_supplied_wh, 
                                                float cost_consumed,
                                                float cost_supplied) {
    if (!g_system_initialized) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    rbpower_lifetime_accumulators_supply_t* accumulators = NULL;
    
    if (unit_id == g_home_network.system.main_supply_unit_id) {
        accumulators = get_main_supply_accumulators();
    } else if (unit_id == g_home_network.system.alternative_supply_unit_id) {
        accumulators = get_alt_supply_accumulators();
    }
    
    if (!accumulators) {
        ESP_LOGW(TAG, "Cannot get supply accumulators for unit %d", unit_id);
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Accumulate
    accumulators->daily_energy_consumed_wh += energy_consumed_wh;
    accumulators->daily_energy_supplied_wh += energy_supplied_wh;
    accumulators->total_energy_consumed_wh += energy_consumed_wh;
    accumulators->total_energy_supplied_wh += energy_supplied_wh;
    accumulators->et_in_mwh  += (double)energy_consumed_wh * 1000.0;  /* exact totalizers, mWh */
    accumulators->et_out_mwh += (double)energy_supplied_wh * 1000.0;
    accumulators->daily_cost_consumed += cost_consumed;
    accumulators->daily_cost_supplied += cost_supplied;
    accumulators->total_cost_consumed += cost_consumed;
    accumulators->total_cost_supplied += cost_supplied;
    
    // Update the statistics
    g_system_stats.total_accumulations++;
    g_system_stats.last_accumulation_time_us = esp_timer_get_time();
    
    // Sync to the RTC backup
    rbpower_lifetime_sync_to_rtc();
    
    return RBPOWER_OK;
}

//=============================================================================
// PUBLIC FUNCTIONS - DAILY RESET
//=============================================================================

/* One unit's daily_* at ITS day boundary (its own rollup). The global reset at the first unit's crossing let
 * every other unit add its 23:45–24:00 quarter into the NEW day after the reset.
 * No snapshot here — the regular NVS persist (≤ 60 s) carries it; one per unit at midnight would multiply
 * NVS writes by the unit count. */
rbpower_err_t rbpower_lifetime_reset_daily_unit(uint8_t i) {
    if (!g_system_initialized || i >= g_home_network.units_count || !g_home_network.units[i].has_valid_definition) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    rbpower_lifetime_accumulators_basic_t* acc = &g_home_network.units[i].statistics.lifetime_accumulators;
    acc->daily_energy_consumed_wh = 0.0f;
    acc->daily_cost_consumed = 0.0f;
    acc->daily_energy_supplied_wh = 0.0f;
    acc->daily_cost_supplied = 0.0f;
    rbpower_unit_id_t uid = g_home_network.units[i].definition.unit_id;
    rbpower_lifetime_accumulators_supply_t* sup =
        (uid == g_home_network.system.main_supply_unit_id) ? get_main_supply_accumulators() :
        (uid == g_home_network.system.alternative_supply_unit_id) ? get_alt_supply_accumulators() : NULL;
    if (sup) {
        sup->daily_energy_consumed_wh = 0.0f;
        sup->daily_energy_supplied_wh = 0.0f;
        sup->daily_cost_consumed = 0.0f;
        sup->daily_cost_supplied = 0.0f;
    }
    g_system_stats.daily_resets_count++;
    g_system_stats.last_daily_reset_time_us = esp_timer_get_time();
    return RBPOWER_OK;
}

/*
 * Reset the daily accumulators of all units (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_reset_daily_all(void) {
    if (!g_system_initialized) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Resetting daily accumulators for all units");
    
    uint32_t reset_count = 0;
    
    // Reset regular units
    for (uint8_t i = 0; i < g_home_network.units_count; i++) {
        if (g_home_network.units[i].has_valid_definition) {
            rbpower_lifetime_accumulators_basic_t* acc = &g_home_network.units[i].statistics.lifetime_accumulators;
            acc->daily_energy_consumed_wh = 0.0f;
            acc->daily_cost_consumed = 0.0f;
            acc->daily_energy_supplied_wh = 0.0f;
            acc->daily_cost_supplied = 0.0f;
            reset_count++;
        }
    }
    
    // Reset the main supply
    rbpower_lifetime_accumulators_supply_t* main_acc = get_main_supply_accumulators();
    if (main_acc) {
        main_acc->daily_energy_consumed_wh = 0.0f;
        main_acc->daily_energy_supplied_wh = 0.0f;
        main_acc->daily_cost_consumed = 0.0f;
        main_acc->daily_cost_supplied = 0.0f;
        reset_count++;
    }
    
    // Reset the alternative supply
    rbpower_lifetime_accumulators_supply_t* alt_acc = get_alt_supply_accumulators();
    if (alt_acc) {
        alt_acc->daily_energy_consumed_wh = 0.0f;
        alt_acc->daily_energy_supplied_wh = 0.0f;
        alt_acc->daily_cost_consumed = 0.0f;
        alt_acc->daily_cost_supplied = 0.0f;
        reset_count++;
    }
    
    // Update the statistics
    g_system_stats.daily_resets_count++;
    g_system_stats.last_daily_reset_time_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Daily reset completed for %lu units", (unsigned long)reset_count);

    // The day boundary is a natural snapshot point (on top of the throttled persist):
    // the day is closed and the totalizers are consistent with the last DAY record.
    rbpower_lifetime_sync_all_to_rtc();
    rbpower_lifetime_persist_to_nvs();

    return RBPOWER_OK;
}

/*
 * Day rollover that happened while the hub was down (or before its clock became valid).
 *
 * After boot the daily_* fields come from the snapshot of the PREVIOUS run (RTC_NOINIT / NVS). If
 * that snapshot belongs to a different local day, the "today" counters must start from zero: after
 * boot the bucket dispatcher sees only today's bucket and never runs the midnight reset for a
 * midnight that passed during downtime, so e_day/cost_day would carry yesterday's energy all day.
 * Called from the 3-s callback until the first valid clock reading; once per boot. Returns true if
 * the reset was performed.
 */
bool rbpower_lifetime_check_day_rollover(void)
{
    static bool s_done = false;
    if (s_done || !g_system_initialized) return false;
    time_t now = time(NULL);
    if (!rbpower_stats_clock_trusted(now)) return false;      /* clock not valid yet (or the Jan-2024 fallback) — wait */
    s_done = true;
    if (!rbpower_stats_clock_trusted((time_t)s_restored_snapshot_ts)) return false;   /* seed, or a snapshot taken under the fallback clock */
    struct tm a, b;
    time_t snap = (time_t)s_restored_snapshot_ts;
    localtime_r(&snap, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return false;
    ESP_LOGW(TAG, "day rollover in downtime: snapshot %04d-%02d-%02d, now %04d-%02d-%02d -> daily counters reset",
             a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday);
    rbpower_diag_printf("lifetime day rollover in downtime: snapshot=%lu (%02d-%02d) now=%lu (%02d-%02d) -> daily counters reset",
                        (unsigned long)snap, a.tm_mon + 1, a.tm_mday, (unsigned long)now, b.tm_mon + 1, b.tm_mday);
    rbpower_lifetime_reset_daily_all();
    return true;
}

//=============================================================================
// PUBLIC FUNCTIONS - STATISTICS
//=============================================================================

/*
 * Get the subsystem statistics (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_get_stats(rbpower_lifetime_stats_t* stats) {
    if (!g_system_initialized || !stats) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    memcpy(stats, &g_system_stats, sizeof(rbpower_lifetime_stats_t));
    return RBPOWER_OK;
}

//=============================================================================
// 3-SECOND PERIOD INTEGRATION
//=============================================================================

/**
 * @brief Accumulate a finished 3-second period into the lifetime accumulators.
 *
 * Called for each unit from the 3-second period finalization (declared in rbpower_lifetime_api.h).
 *
 * @param[in] unit_id    Unit ID (the real ID, not the index).
 * @param[in] stats_3sec Finished 3-second period statistics of the unit.
 * @return Result of rbpower_lifetime_accumulate_supply() / rbpower_lifetime_accumulate_basic().
 * @retval RBPOWER_ERR_INVALID_ARG @p stats_3sec is NULL or the unit is unknown.
 */
rbpower_err_t rbpower_lifetime_process_3sec_period(rbpower_unit_id_t unit_id,
                                                  const rbpower_period_statistics_t* stats_3sec) {

/*                                                    
                                                    if (!g_system_initialized) {
        ESP_LOGE(TAG, "Lifetime accumulators system not initialized! Call rbpower_lifetime_init() first");
        return RBPOWER_ERR_NOT_INIT;
    }
*/    
    if (!stats_3sec) {
        ESP_LOGE(TAG, "stats_3sec is NULL");
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Determine the unit type (supply or regular)
    bool is_supply = (unit_id == g_home_network.system.main_supply_unit_id ||
                     unit_id == g_home_network.system.alternative_supply_unit_id);
    
    // Extract the data according to the unit type
    float energy_consumed_wh = 0.0f;
    float energy_supplied_wh = 0.0f;
    float cost_consumed = 0.0f;
    float cost_supplied = 0.0f;
    
    if (is_supply) {
        // Supply units: take the data directly from the specialized structures
        if (unit_id == g_home_network.system.main_supply_unit_id) {
            rbpower_main_supply_t* main_supply = (rbpower_main_supply_t*)g_home_network.specialized.main_supply;
            if (main_supply) {
                energy_consumed_wh = main_supply->statistics_3sec.energy.consumed_wh_accumulator;
                energy_supplied_wh = main_supply->statistics_3sec.energy.supplied_wh_accumulator;
                cost_consumed = main_supply->period_economics.economics_3sec.instant_cost;
                cost_supplied = main_supply->period_economics.economics_3sec.instant_revenue;
            }
        } else if (unit_id == g_home_network.system.alternative_supply_unit_id) {
            rbpower_alternative_supply_t* alt_supply = (rbpower_alternative_supply_t*)g_home_network.specialized.alternative_supply;
            if (alt_supply) {
                // Alternative supply: generated energy counts as supplied
                energy_consumed_wh = alt_supply->statistics_3sec.energy_consumed_final; 
                energy_supplied_wh = alt_supply->statistics_3sec.energy_generated_final;
                cost_consumed = alt_supply->period_economics.economics_3sec.instant_generation_cost;
                cost_supplied = alt_supply->period_economics.economics_3sec.instant_generation_revenue;
            }
        }
        
        ESP_LOGI(TAG, "Unit %d (supply): energy_in=%.3f Wh, energy_out=%.3f Wh, cost_in=%.6f EUR, revenue=%.6f EUR", 
                 unit_id, energy_consumed_wh, energy_supplied_wh, cost_consumed, cost_supplied);
    } else {
        // Regular units: take the data from the common period statistics
        energy_consumed_wh = stats_3sec->energy_period.consumed_wh;
        cost_consumed = (float)stats_3sec->result_base.cost_consumed;

        ESP_LOGI(TAG, "Unit %d (basic): energy=%.3f Wh, cost=%.6f EUR",
                 unit_id, energy_consumed_wh, cost_consumed);
    }
    
    rbpower_err_t result;
    if (is_supply) {
        // Supply unit: accumulate consumption and supply
        result = rbpower_lifetime_accumulate_supply(unit_id,
                                                 energy_consumed_wh, energy_supplied_wh,
                                                 cost_consumed, cost_supplied);
        if (result != RBPOWER_OK) {
            ESP_LOGW(TAG, "Failed to accumulate supply data for unit %d: %d", unit_id, result);
        }
    } else {
        // Regular unit: consumption only
        result = rbpower_lifetime_accumulate_basic(unit_id, energy_consumed_wh, cost_consumed);
        if (result != RBPOWER_OK) {
            ESP_LOGW(TAG, "Failed to accumulate basic data for unit %d: %d", unit_id, result);
        }
    }
    
    return result;
}

/*
 * Get the accumulated data of one unit (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_get_unit_data(rbpower_unit_id_t unit_id, 
                                            rbpower_lifetime_unit_data_t* data) {
    if (!g_system_initialized || !data) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    memset(data, 0, sizeof(rbpower_lifetime_unit_data_t));
    data->unit_id = unit_id;
    data->is_supply_unit = is_supply_unit(unit_id);
    
    if (data->is_supply_unit) {
        // Supply unit
        rbpower_lifetime_accumulators_supply_t* accumulators = NULL;

        if (unit_id == g_home_network.system.main_supply_unit_id) {
            accumulators = get_main_supply_accumulators();
        } else if (unit_id == g_home_network.system.alternative_supply_unit_id) {
            accumulators = get_alt_supply_accumulators();
        }

        if (!accumulators) {
            /* No specialized struct (I2C-fed supply unit) — data lives in the basic accumulator;
             * report it as basic so callers read the fields that are actually fed. */
            data->is_supply_unit = false;
            goto basic_fallback;
        }

        data->daily_energy_consumed_wh = accumulators->daily_energy_consumed_wh;
        data->daily_energy_supplied_wh = accumulators->daily_energy_supplied_wh;
        data->daily_cost_consumed = accumulators->daily_cost_consumed;
        data->daily_cost_supplied = accumulators->daily_cost_supplied;
        data->total_energy_consumed_wh = accumulators->total_energy_consumed_wh;
        data->total_energy_supplied_wh = accumulators->total_energy_supplied_wh;
        data->total_cost_consumed = accumulators->total_cost_consumed;
        data->total_cost_supplied = accumulators->total_cost_supplied;
        data->last_rtc_sync_timestamp = accumulators->last_rtc_sync_timestamp;
        data->rtc_enabled = accumulators->rtc_enabled;
        
    } else {
        // Regular unit
        rbpower_lifetime_accumulators_basic_t* accumulators;
basic_fallback:
        accumulators = get_basic_accumulators(unit_id);
        if (!accumulators) {
            return RBPOWER_ERR_NVS_NOT_FOUND;
        }

        data->daily_energy_consumed_wh = accumulators->daily_energy_consumed_wh;
        data->daily_cost_consumed = accumulators->daily_cost_consumed;
        data->total_energy_consumed_wh = accumulators->total_energy_consumed_wh;
        data->total_cost_consumed = accumulators->total_cost_consumed;
        data->daily_energy_supplied_wh = accumulators->daily_energy_supplied_wh;
        data->total_energy_supplied_wh = accumulators->total_energy_supplied_wh;
        data->daily_cost_supplied = accumulators->daily_cost_supplied;
        data->total_cost_supplied = accumulators->total_cost_supplied;
        data->last_rtc_sync_timestamp = accumulators->last_rtc_sync_timestamp;
        data->rtc_enabled = accumulators->rtc_enabled;
    }
    
    return RBPOWER_OK;
}

//=============================================================================
// RTC BACKUP FUNCTIONS
//=============================================================================

/**
 * @brief Copy one unit's accumulators into slot @p rtc_index of the RTC image.
 */
static void save_unit_to_rtc(uint16_t rtc_index, rbpower_unit_id_t unit_id, bool is_supply) {
    if (rtc_index >= RBPOWER_LIFETIME_MAX_UNITS) {
        return;
    }
    
    rbpower_rtc_unit_data_t* rtc_unit = &g_rtc_backup.units[rtc_index];

    /* Slot is reused across syncs — zero it first, so a NULL-accumulator path can't leave a
     * previous unit's payload under this unit_id (restore would seed the wrong unit). */
    memset(rtc_unit, 0, sizeof(*rtc_unit));

    // Header fields of the record
    rtc_unit->unit_id = unit_id;
    rtc_unit->flags = is_supply ? RBPOWER_RTC_FLAG_IS_SUPPLY_UNIT : 0;
    
    if (is_supply) {
        // Supply unit: save the full data
        rbpower_lifetime_accumulators_supply_t* accumulators = NULL;

        if (unit_id == g_home_network.system.main_supply_unit_id) {
            accumulators = get_main_supply_accumulators();
        } else if (unit_id == g_home_network.system.alternative_supply_unit_id) {
            accumulators = get_alt_supply_accumulators();
        }

        /* No specialized struct (I2C-fed supply unit): its energy lives in the BASIC accumulator
         * (feeder fallback) — save THAT, with the supply flag cleared so restore matches. Otherwise
         * the slot keeps a stale payload, restore skips it and the exact totalizers never survive
         * a reboot. */
        if (!accumulators) {
            is_supply = false;
            rtc_unit->flags = 0;
        }

        if (accumulators) {
            rtc_unit->daily_energy_consumed_wh = accumulators->daily_energy_consumed_wh;
            rtc_unit->total_energy_consumed_wh = accumulators->total_energy_consumed_wh;
            rtc_unit->daily_cost_consumed = accumulators->daily_cost_consumed;
            rtc_unit->total_cost_consumed = accumulators->total_cost_consumed;

            // Supply-specific data
            rtc_unit->daily_energy_supplied_wh = accumulators->daily_energy_supplied_wh;
            rtc_unit->total_energy_supplied_wh = accumulators->total_energy_supplied_wh;
            rtc_unit->daily_cost_supplied = accumulators->daily_cost_supplied;
            rtc_unit->total_cost_supplied = accumulators->total_cost_supplied;
            rtc_unit->et_in_mwh  = accumulators->et_in_mwh;
            rtc_unit->et_out_mwh = accumulators->et_out_mwh;

            ESP_LOGD(TAG, "Saved supply unit %d to RTC[%d]: daily_in=%.3f, total_in=%.1f",
                     unit_id, rtc_index, rtc_unit->daily_energy_consumed_wh, rtc_unit->total_energy_consumed_wh);
        }
    }
    if (!is_supply) {
        // Regular unit
        rbpower_lifetime_accumulators_basic_t* accumulators = get_basic_accumulators(unit_id);
        
        if (accumulators) {
            rtc_unit->daily_energy_consumed_wh = accumulators->daily_energy_consumed_wh;
            rtc_unit->total_energy_consumed_wh = accumulators->total_energy_consumed_wh;
            rtc_unit->daily_cost_consumed = accumulators->daily_cost_consumed;
            rtc_unit->total_cost_consumed = accumulators->total_cost_consumed;
            
            rtc_unit->daily_energy_supplied_wh = accumulators->daily_energy_supplied_wh;
            rtc_unit->total_energy_supplied_wh = accumulators->total_energy_supplied_wh;
            rtc_unit->daily_cost_supplied = accumulators->daily_cost_supplied;
            rtc_unit->total_cost_supplied = accumulators->total_cost_supplied;
            rtc_unit->et_in_mwh  = accumulators->et_in_mwh;
            rtc_unit->et_out_mwh = accumulators->et_out_mwh;
            
            ESP_LOGD(TAG, "Saved basic unit %d to RTC[%d]: daily=%.3f, total=%.1f", 
                     unit_id, rtc_index, rtc_unit->daily_energy_consumed_wh, rtc_unit->total_energy_consumed_wh);
        }
    }
}

/*
 * Write all units to the RTC backup (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_sync_all_to_rtc(void) {
    if (!g_system_initialized) {
        ESP_LOGW(TAG, "System not initialized, sync to RTC failed");
        return RBPOWER_ERR_NOT_INIT;
    }
/*    
    ESP_LOGI(TAG, "=== SYNC ALL LIFETIME DATA TO RTC START ===");
    ESP_LOGI(TAG, "RTC backup address: %p", (void*)&g_rtc_backup);
    ESP_LOGI(TAG, "Main supply unit ID: %d", g_home_network.system.main_supply_unit_id);
    ESP_LOGI(TAG, "Alt supply unit ID: %d", g_home_network.system.alternative_supply_unit_id);
    ESP_LOGI(TAG, "Total home units: %d", g_home_network.units_count);
*/    
    uint16_t rtc_index = 0;
    
    // Save the supply units first
    if (g_home_network.system.main_supply_unit_id != 0) {
        save_unit_to_rtc(rtc_index++, g_home_network.system.main_supply_unit_id, true);
    }
    
    if (g_home_network.system.alternative_supply_unit_id != 0) {
        save_unit_to_rtc(rtc_index++, g_home_network.system.alternative_supply_unit_id, true);
    }
    
    // Save the regular units
    for (uint8_t i = 0; i < g_home_network.units_count && rtc_index < RBPOWER_LIFETIME_MAX_UNITS; i++) {
        if (g_home_network.units[i].has_valid_definition) {
            rbpower_unit_id_t unit_id = g_home_network.units[i].definition.unit_id;
            
            // Skip the supply units (already saved)
            if (unit_id != g_home_network.system.main_supply_unit_id && 
                unit_id != g_home_network.system.alternative_supply_unit_id) {
                save_unit_to_rtc(rtc_index++, unit_id, false);
            }
        }
    }
    
    // Update the RTC header
    g_rtc_backup.magic = RBPOWER_LIFETIME_MAGIC;
    g_rtc_backup.timestamp_sntp = get_current_timestamp();
    g_rtc_backup.units_count = rtc_index;
    // The CRC MUST be recomputed after every data change. If it is stale, every warm reboot
    // fails validate_rtc_data() on the CRC and the RTC restore never works.
    g_rtc_backup.crc32 = calculate_rtc_crc32();

    // Update the statistics
    g_system_stats.total_rtc_syncs++;
    g_system_stats.last_rtc_sync_time_us = esp_timer_get_time();

    /* NVS snapshot on EVERY sync (= every 15-min feed; ~96 writes/day, well within NVS wear
     * limits). An hourly throttle proved too coarse: the RTC image does not survive a change of
     * the firmware IMAGE (the .rtc_noinit layout may shift), so after an update the restore fell
     * back to an NVS snapshot up to an hour old. The exact totalizers are billing data, so NVS
     * freshness matters more than saving writes. The 60-s minimum interval guards against storms. */
    {
        int64_t now_us = esp_timer_get_time();
        if (s_last_nvs_persist_us == 0 || now_us - s_last_nvs_persist_us >= 60LL * 1000000LL) {
            if (rbpower_lifetime_persist_to_nvs() == RBPOWER_OK) {
                s_last_nvs_persist_us = now_us;
                s_nvs_dirty = false;
            }
        } else {
            s_nvs_dirty = true;   /* flushed by rbpower_lifetime_nvs_tick() within ~10 s */
        }
    }

    return RBPOWER_OK;
}

/*
 * Snapshot of the totalizers to NVS (namespace "rbltm", key "snap" = rbpower_rtc_backup_t).
 * Only a valid, freshly computed image is written (the same CRC as used for RTC validation).
 */
rbpower_err_t rbpower_lifetime_persist_to_nvs(void) {
    if (!g_system_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open("rbltm", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed for lifetime snapshot: %s", esp_err_to_name(e));
        return RBPOWER_ERR_INVALID_STATE;
    }
    e = nvs_set_blob(h, "snap", &g_rtc_backup, sizeof(g_rtc_backup));
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "NVS lifetime snapshot write failed: %s", esp_err_to_name(e));
        return RBPOWER_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "lifetime totalizers snapshot -> NVS (%u units, ts=%lu)",
             g_rtc_backup.units_count, (unsigned long)g_rtc_backup.timestamp_sntp);
    return RBPOWER_OK;
}

/*
 * Exact totalizers of a unit, mWh (for v4 et_in/et_out).
 */
rbpower_err_t rbpower_lifetime_get_totalizers(rbpower_unit_id_t unit_id,
                                              int64_t* et_in_mwh, int64_t* et_out_mwh) {
    if (!et_in_mwh || !et_out_mwh) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    *et_in_mwh = 0;
    *et_out_mwh = 0;
    if (!g_system_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    if (is_supply_unit(unit_id)) {
        rbpower_lifetime_accumulators_supply_t* acc =
            (unit_id == g_home_network.system.main_supply_unit_id) ? get_main_supply_accumulators()
                                                                   : get_alt_supply_accumulators();
        if (acc) {
            *et_in_mwh  = (int64_t)(acc->et_in_mwh  + 0.5);
            *et_out_mwh = (int64_t)(acc->et_out_mwh + 0.5);
            return RBPOWER_OK;
        }
        /* No specialized struct (I2C-fed supply unit) — the et feeder (rbhome.c inject) falls
         * back to the unit's BASIC accumulator in exactly this case, so read the same place;
         * otherwise etstat reports an error and et stays 0 while the feeds grow. */
    }
    rbpower_lifetime_accumulators_basic_t* acc = get_basic_accumulators(unit_id);
    if (!acc) {
        return RBPOWER_ERR_INVALID_ARG;   /* unit unknown */
    }
    *et_in_mwh  = (int64_t)(acc->et_in_mwh + 0.5);
    *et_out_mwh = (int64_t)(acc->et_out_mwh + 0.5);
    return RBPOWER_OK;
}

/*
 * Sync to the RTC backup on every accumulator update.
 */
rbpower_err_t rbpower_lifetime_sync_to_rtc(void) {
    // No interval check: sync on every call
    ESP_LOGD(TAG, "Automatic RTC sync triggered on data update");
    
    rbpower_err_t result = rbpower_lifetime_sync_all_to_rtc();
    
    if (result != RBPOWER_OK) {
        g_system_stats.rtc_sync_errors++;
        ESP_LOGW(TAG, "RTC sync failed: %d", result);
    }
    
    return result;
}

//=============================================================================
// DIAGNOSTIC FUNCTIONS
//=============================================================================

/*
 * Log the state of the RTC backup (see rbpower_lifetime_api.h).
 */
rbpower_err_t rbpower_lifetime_debug_rtc_state(void) {
    ESP_LOGI(TAG, "=== LIFETIME ACCUMULATORS RTC DIAGNOSTICS ===");
    
    // Subsystem information
    ESP_LOGI(TAG, "System initialized: %s", g_system_initialized ? "YES" : "NO");
    if (!g_system_initialized) {
        ESP_LOGW(TAG, "System not initialized - RTC diagnostics limited");
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    // RTC memory address
    ESP_LOGI(TAG, "RTC backup address: %p", (void*)&g_rtc_backup);
    ESP_LOGI(TAG, "RTC backup size: %d bytes", (int)sizeof(rbpower_rtc_backup_t));
    
    // RTC data validity
    ESP_LOGI(TAG, "=== RTC DATA VALIDATION ===");
    ESP_LOGI(TAG, "Magic: 0x%08lX (expected: 0x%08lX) - %s", 
             (unsigned long)g_rtc_backup.magic, 
             (unsigned long)RBPOWER_LIFETIME_MAGIC,
             (g_rtc_backup.magic == RBPOWER_LIFETIME_MAGIC) ? "VALID" : "INVALID");
    ESP_LOGI(TAG, "Units count: %d (max: %d) - %s", 
             g_rtc_backup.units_count, 
             RBPOWER_LIFETIME_MAX_UNITS,
             (g_rtc_backup.units_count <= RBPOWER_LIFETIME_MAX_UNITS) ? "VALID" : "INVALID");
    ESP_LOGI(TAG, "Timestamp: %lu", (unsigned long)g_rtc_backup.timestamp_sntp);
    
    // CRC32 check
    uint32_t calculated_crc = calculate_rtc_crc32();
    ESP_LOGI(TAG, "CRC32 stored: 0x%08lX, calculated: 0x%08lX - %s",
             (unsigned long)g_rtc_backup.crc32,
             (unsigned long)calculated_crc,
             (g_rtc_backup.crc32 == calculated_crc) ? "VALID" : "INVALID");
    
    // Overall validation result
    bool is_valid = validate_rtc_data();
    ESP_LOGI(TAG, "Overall RTC validity: %s", is_valid ? "VALID" : "INVALID");
    ESP_LOGI(TAG, "System RTC valid flag: %s", g_system_stats.rtc_valid ? "TRUE" : "FALSE");
    
    // Saved unit records
    ESP_LOGI(TAG, "=== SAVED UNITS DATA ===");
    for (uint16_t i = 0; i < g_rtc_backup.units_count && i < RBPOWER_LIFETIME_MAX_UNITS; i++) {
        const rbpower_rtc_unit_data_t* unit = &g_rtc_backup.units[i];
        bool is_supply = (unit->flags & RBPOWER_RTC_FLAG_IS_SUPPLY_UNIT) != 0;
        
        ESP_LOGI(TAG, "Unit[%d]: ID=%d, Type=%s", i, unit->unit_id, 
                 is_supply ? "SUPPLY" : "BASIC");
        ESP_LOGI(TAG, "  Daily: energy=%.3f Wh, cost=%.4f EUR%s", 
                 unit->daily_energy_consumed_wh, unit->daily_cost_consumed,
                 is_supply ? "" : " (consumed only)");
        ESP_LOGI(TAG, "  Total: energy=%.1f Wh, cost=%.2f EUR%s", 
                 unit->total_energy_consumed_wh, unit->total_cost_consumed,
                 is_supply ? "" : " (consumed only)");
        
        if (is_supply) {
            ESP_LOGI(TAG, "  Daily supply: energy=%.3f Wh, cost=%.4f EUR", 
                     unit->daily_energy_supplied_wh, unit->daily_cost_supplied);
            ESP_LOGI(TAG, "  Total supply: energy=%.1f Wh, cost=%.2f EUR", 
                     unit->total_energy_supplied_wh, unit->total_cost_supplied);
        }
    }
    
    // Subsystem statistics
    ESP_LOGI(TAG, "=== SYSTEM STATISTICS ===");
    ESP_LOGI(TAG, "Total accumulations: %lu", (unsigned long)g_system_stats.total_accumulations);
    ESP_LOGI(TAG, "Total RTC syncs: %lu", (unsigned long)g_system_stats.total_rtc_syncs);
    ESP_LOGI(TAG, "RTC sync errors: %lu", (unsigned long)g_system_stats.rtc_sync_errors);
    ESP_LOGI(TAG, "Last accumulation: %llu μs", (unsigned long long)g_system_stats.last_accumulation_time_us);
    ESP_LOGI(TAG, "Last RTC sync: %llu μs", (unsigned long long)g_system_stats.last_rtc_sync_time_us);
    
    // Current time
    uint32_t current_time = get_current_timestamp();
    ESP_LOGI(TAG, "Current timestamp: %lu", (unsigned long)current_time);
    
    ESP_LOGI(TAG, "=== END OF RTC DIAGNOSTICS ===");
    
    return RBPOWER_OK;
}