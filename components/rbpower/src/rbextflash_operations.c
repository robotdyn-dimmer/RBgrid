/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbextflash_operations.c
 * @brief Statistics ring-buffer storage: write/read of period records on the stats flash medium
 *
 * Implements the per-unit fixed-slot layout, the sector-based ring buffers inside each slot,
 * boot-time cursor recovery by scanning sector markers, the record-format marker and the
 * write/read locking model. The physical medium (internal `stats` partition or optional
 * external W25Q NOR chip) is selected by rbstorage_media.
 */

#include "rbextflash_operations.h"
#include "rbextflash_sector_metadata.h"
#include "rbextflash_config_runtime.h"
#include "rbstorage_transfer_types.h"  // RBPOWER_BUFFER_SECTORS
#include "rbhome_types.h"  // rbpower_electrical_network_t, rbpower_home_unit_t, RBPOWER_ROLE_*
#include "external_flash_config.h"
#include "littlefs_raw.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

/* ================================================================================================
 * MEDIA ROUTING SEAM (internal flash first)
 * ------------------------------------------------------------------------------------------------
 * Everything below was written and HW-validated against the EXTERNAL NOR: absolute offsets in its
 * layout (slots at PARTITION_STATS_OFFSET + slot*208KB) via esp_flash_* + get_external_flash_handle.
 * The default store is the INTERNAL `stats` partition; the external NOR is an optional
 * alternative medium. Rather than editing ~30 call sites of validated ring/sector/migration logic, the three
 * physical IO calls and the handle getter are re-routed FILE-LOCALLY to rbstorage_media shims,
 * which translate the same absolute offsets to the internal partition (or pass through to the
 * external chip when it is attached). The `handle` argument becomes an opaque non-NULL token —
 * existing `if (!flash) return` guards keep working; nothing dereferences it here.
 * NOTE: otp_external_flash_test.c is NOT seamed — OTP runs only against a real external chip. */
#include "rbstorage_media.h"
#include "rbstats_migrate.h"   /* deferred breaking-format migration engine */
/** @brief File-local reroute: read @p len bytes at absolute stats offset @p addr via rbstorage_media (handle @p h ignored). */
#define esp_flash_read(h, buf, addr, len)    rbmedia_stats_read((uint32_t)(addr), (buf), (len))
/** @brief File-local reroute: write @p len bytes at absolute stats offset @p addr via rbstorage_media (handle @p h ignored). */
#define esp_flash_write(h, buf, addr, len)   rbmedia_stats_write((uint32_t)(addr), (buf), (len))
/** @brief File-local reroute: erase @p len bytes (sector-aligned) at absolute stats offset @p addr via rbstorage_media. */
#define esp_flash_erase_region(h, addr, len) rbmedia_stats_erase((uint32_t)(addr), (len))
/** @brief File-local reroute: opaque non-NULL token standing in for the external flash handle. */
#define get_external_flash_handle()          rbmedia_stats_handle_token()

/** @brief External flash manager state (defined in external_flash_manager.c).
 *  @return Pointer to the global state (never NULL; check status.initialized). */
extern external_flash_state_t* external_flash_get_state(void);

static const char* TAG = "RBFLASH_OPS";

/** @brief 1 = erase all flash buffers on initialization (clean-test aid). Must stay 0 in normal
 *  builds, otherwise history is wiped on every boot. */
#define CLEAR_BUFFERS_ON_INIT 0

/* ================================================================================================
 * INTERNAL STRUCTURES
 * ================================================================================================ */

// Global runtime manager for buffer configuration access
static rbpower_runtime_config_manager_t g_runtime_manager;
static bool g_is_initialized = false;
static SemaphoreHandle_t g_flash_mutex = NULL;

/* ================================================================================================
 * WRITE/READ TIME-SEPARATION + WRITE PRIORITY
 * ------------------------------------------------------------------------------------------------
 * This is a purpose-built direct-flash "disk" — writes and reads MUST be time-separated (never
 * overlap on the chip), and WRITES TAKE PRIORITY over reads (stats persistence must not be starved
 * by history reads). g_flash_mutex gives mutual exclusion (time-separation). On top of it, a
 * writers-waiting counter implements writer preference: a reader yields BEFORE taking the mutex
 * while any writer is waiting, so a pending write goes ahead of new/queued-at-the-gate reads.
 * An in-progress read is never preempted mid-operation (that would corrupt the SPI transaction) —
 * priority applies at acquisition, which is the correct granularity for a NOR ring buffer.
 * ================================================================================================ */
static portMUX_TYPE g_flash_wp_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int g_flash_writers_waiting = 0;

// WRITER: announce intent (blocks new readers at the gate), then take the mutex.
static bool flash_lock_write(TickType_t timeout)
{
    portENTER_CRITICAL(&g_flash_wp_mux);
    g_flash_writers_waiting++;
    portEXIT_CRITICAL(&g_flash_wp_mux);
    if (xSemaphoreTake(g_flash_mutex, timeout) == pdTRUE) {
        return true;
    }
    portENTER_CRITICAL(&g_flash_wp_mux);
    g_flash_writers_waiting--;
    portEXIT_CRITICAL(&g_flash_wp_mux);
    return false;
}
static void flash_unlock_write(void)
{
    xSemaphoreGive(g_flash_mutex);
    portENTER_CRITICAL(&g_flash_wp_mux);
    if (g_flash_writers_waiting > 0) g_flash_writers_waiting--;
    portEXIT_CRITICAL(&g_flash_wp_mux);
}
// READER: defer to any waiting writer (WRITE PRIORITY), then take the mutex.
static bool flash_lock_read(TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    while (g_flash_writers_waiting > 0) {
        if ((xTaskGetTickCount() - start) >= timeout) return false;
        vTaskDelay(1);
    }
    return (xSemaphoreTake(g_flash_mutex, timeout) == pdTRUE);
}
static void flash_unlock_read(void)
{
    xSemaphoreGive(g_flash_mutex);
}

// Diagnostic: number of writers currently waiting/holding (for the concurrency diagnostics report).
int rbpower_flash_writers_waiting(void) { return g_flash_writers_waiting; }

/* ================================================================================================
 * FIXED SLOT SYSTEM
 * ================================================================================================ */

#if RBPOWER_RECORD_FORMAT_VERSION >= 4
/** @brief Per-unit slot size in KB. v4: 320 KB = 80 data sectors (11 slots on a 4 MB chip). */
#define RBPOWER_SLOT_SIZE_KB 320
#else
/** @brief Per-unit slot size in KB (pre-v4 layout): 208 KB = 52 sectors. */
#define RBPOWER_SLOT_SIZE_KB 208         // Slot size: 208 KB (52 sectors). Footprint after the
#endif
                                         // quality/direction removal + HOUR 60-day retention:
                                         // HOUR 22 + DAY 6 + WEEK 6 + MONTH 6 + YEAR 4 =
                                         // 44 sectors used, 8 sectors margin. (Was 50 with QUAL/DIR.)

// A unit's slot lives INSIDE the STATS partition: base = STATS_OFFSET + slot*SLOT_SIZE.
// Derived from slot_index every time — never trusted from a persisted offset — so a layout change
// (this rework, or a different chip size) is self-correcting. Old chip-relative offsets that used to
// overlap the table/NVS/config/OTA regions are gone.
/** @brief Absolute stats-medium offset (bytes) of slot @p slot under the live layout. */
#define RBPOWER_SLOT_BASE_OFFSET(slot) (PARTITION_STATS_OFFSET + (uint32_t)(slot) * (RBPOWER_SLOT_SIZE_KB * 1024))
/** @brief LittleFS path of the persisted slot map (JSON, unit_id per slot). */
#define RBPOWER_FLASH_SLOTS_CONFIG_FILE "/flash_slots.json"
/** @def MAX_SLOTS_JSON_SIZE
 *  @brief Read buffer size (bytes) for the slot-map JSON: ~128 B per slot entry + envelope
 *         (defined locally in load_flash_slots_config()). */

/**
 * @brief Fixed memory slot structure
 */
typedef struct {
    uint8_t slot_index;               ///< Slot index (0 .. RBPOWER_MAX_UNITS-1)
    uint32_t base_offset;             ///< Base offset on the stats medium (bytes)
    rbpower_unit_id_t unit_id;        ///< Unit ID in slot (0xFFFF = free)
    bool is_occupied;                 ///< true if slot is occupied
} rbpower_flash_slot_t;

// Global slot table
static rbpower_flash_slot_t g_flash_slots[RBPOWER_MAX_UNITS] = {0};
static bool g_slots_initialized = false;
// Slots that actually FIT in the STATS region of the detected chip (4MB test ≈ 17, 16MB prod = 64).
// Units beyond this get no slot → RAM-only (graceful). Computed in init_flash_slots from the runtime
// STATS size; defaults to RBPOWER_MAX_UNITS until then.
static int g_max_usable_slots = RBPOWER_MAX_UNITS;

// Sanity: at least one unit slot must fit within a 4MB external chip (the smallest supported).
_Static_assert(PARTITION_STATS_OFFSET + (RBPOWER_SLOT_SIZE_KB * 1024) <= 0x400000,
               "STATS_OFFSET + one 208KB slot must fit a 4MB external flash");

/* ================================================================================================
 * CRC16 FOR TEST DATA
 * ================================================================================================ */

/**
 * @brief CRC16-CCITT (polynomial 0x1021)
 */
static uint16_t crc16_ccitt(const uint8_t* data, size_t length)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

uint16_t rbpower_test_record_calc_crc(const rbpower_test_record_t* record)
{
    if (!record) {
        return 0;
    }

    // CRC calculated for all fields except CRC itself
    return crc16_ccitt((const uint8_t*)record, sizeof(rbpower_test_record_t) - sizeof(uint16_t));
}

bool rbpower_test_record_verify_crc(const rbpower_test_record_t* record)
{
    if (!record) {
        return false;
    }

    uint16_t calculated_crc = rbpower_test_record_calc_crc(record);
    return (calculated_crc == record->crc16);
}

/* ================================================================================================
 * FIXED SLOT MANAGEMENT
 * ================================================================================================ */

/**
 * @brief Save the slot map to the JSON file
 */
static esp_err_t save_flash_slots_config(void)
{
    ESP_LOGD(TAG, "Saving flash slots config to %s...", RBPOWER_FLASH_SLOTS_CONFIG_FILE);

    // Check flash availability via the global status
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGD(TAG, "External Flash not available, skipping config save (RAM-only mode)");
        return ESP_OK;  // Silently skip the save
    }

    // Create the JSON object
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root object");
        return ESP_ERR_NO_MEM;
    }

    // Create the slots array
    cJSON* slots_array = cJSON_CreateArray();
    if (!slots_array) {
        ESP_LOGE(TAG, "Failed to create slots array");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    // Fill the slots array
    for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
        cJSON* slot_obj = cJSON_CreateObject();
        if (!slot_obj) {
            ESP_LOGE(TAG, "Failed to create slot object");
            cJSON_Delete(slots_array);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddNumberToObject(slot_obj, "slot", g_flash_slots[i].slot_index);
        cJSON_AddNumberToObject(slot_obj, "offset", g_flash_slots[i].base_offset);
        cJSON_AddNumberToObject(slot_obj, "unit_id", g_flash_slots[i].unit_id);
        cJSON_AddBoolToObject(slot_obj, "occupied", g_flash_slots[i].is_occupied);

        cJSON_AddItemToArray(slots_array, slot_obj);
    }

    cJSON_AddItemToObject(root, "slots", slots_array);

    // Convert to a string
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to convert JSON to string");
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);

    // Write via the LittleFS API
    esp_err_t ret = littlefs_raw_write(RBPOWER_FLASH_SLOTS_CONFIG_FILE, json_str, json_len);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write JSON to file: %s (%s)",
                 RBPOWER_FLASH_SLOTS_CONFIG_FILE, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Flash slots config saved (%d bytes)", json_len);
    return ESP_OK;
}

/**
 * @brief Load the slot map from the JSON file
 */
static esp_err_t load_flash_slots_config(void)
{
    ESP_LOGD(TAG, "Loading flash slots config from %s...", RBPOWER_FLASH_SLOTS_CONFIG_FILE);

    // Check flash availability via the global status
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGD(TAG, "External Flash not available, skipping config load (RAM-only mode)");
        return ESP_ERR_NOT_FOUND;  // Return "not found" so the caller skips the load
    }

    // Check that the file exists
    if (!littlefs_raw_exists(RBPOWER_FLASH_SLOTS_CONFIG_FILE)) {
        ESP_LOGI(TAG, "Config file not found: %s (first run)", RBPOWER_FLASH_SLOTS_CONFIG_FILE);
        return ESP_ERR_NOT_FOUND;
    }

    /* Buffer sized for the FULL scheme: save writes ALL RBPOWER_MAX_UNITS entries (~120 B each
     * pretty-printed). The old fixed 2 KB ("enough for 4 slots") predates MAX_UNITS=64 — the file
     * grew to ~7 KB, every read TRUNCATED, CRC failed, load silently fell back to an EMPTY scheme.
     * Nobody noticed because slots are re-allocated deterministically each boot; the only real
     * consumer of the loaded map was the v3→v4 migrator, which therefore saw 0 occupied slots and
     * restamped the marker over unmigrated data. */
    /* Read buffer size (bytes): ~128 B per slot entry + envelope. */
    #define MAX_SLOTS_JSON_SIZE (RBPOWER_MAX_UNITS * 128 + 256)
    char* json_str = (char*)malloc(MAX_SLOTS_JSON_SIZE);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to allocate buffer for JSON (%d bytes)", MAX_SLOTS_JSON_SIZE);
        return ESP_ERR_NO_MEM;
    }

    // Read the file via the LittleFS API
    size_t actual_size = 0;
    esp_err_t ret = littlefs_raw_read(RBPOWER_FLASH_SLOTS_CONFIG_FILE, json_str,
                                       MAX_SLOTS_JSON_SIZE - 1, &actual_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read JSON file: %s (%s)",
                 RBPOWER_FLASH_SLOTS_CONFIG_FILE, esp_err_to_name(ret));
        free(json_str);
        return ret;
    }

    json_str[actual_size] = '\0';

    // Parse the JSON
    cJSON* root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", cJSON_GetErrorPtr());
        return ESP_FAIL;
    }

    // Get the slots array
    cJSON* slots_array = cJSON_GetObjectItem(root, "slots");
    if (!cJSON_IsArray(slots_array)) {
        ESP_LOGE(TAG, "Invalid JSON: 'slots' is not an array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int slot_count = cJSON_GetArraySize(slots_array);
    if (slot_count != RBPOWER_MAX_UNITS) {
        ESP_LOGW(TAG, "Slot count mismatch: expected %d, got %d", RBPOWER_MAX_UNITS, slot_count);
    }

    // Load the slot entries
    for (int i = 0; i < RBPOWER_MAX_UNITS && i < slot_count; i++) {
        cJSON* slot_obj = cJSON_GetArrayItem(slots_array, i);
        if (!slot_obj) {
            continue;
        }

        cJSON* slot_idx_obj = cJSON_GetObjectItem(slot_obj, "slot");
        cJSON* offset_obj = cJSON_GetObjectItem(slot_obj, "offset");
        cJSON* unit_id_obj = cJSON_GetObjectItem(slot_obj, "unit_id");
        cJSON* occupied_obj = cJSON_GetObjectItem(slot_obj, "occupied");

        if (!cJSON_IsNumber(slot_idx_obj) || !cJSON_IsNumber(offset_obj) ||
            !cJSON_IsNumber(unit_id_obj) || !cJSON_IsBool(occupied_obj)) {
            ESP_LOGW(TAG, "Invalid slot entry %d, skipping", i);
            continue;
        }

        g_flash_slots[i].slot_index = (uint8_t)slot_idx_obj->valueint;
        g_flash_slots[i].base_offset = (uint32_t)offset_obj->valuedouble;
        g_flash_slots[i].unit_id = (rbpower_unit_id_t)unit_id_obj->valueint;
        g_flash_slots[i].is_occupied = cJSON_IsTrue(occupied_obj);
    }

    cJSON_Delete(root);

    ESP_LOGD(TAG, "Flash slots config loaded successfully (%d bytes)", actual_size);
    return ESP_OK;
}

/**
 * @brief Log the current state of all slots
 */
static void print_flash_slots_info(void)
{
    ESP_LOGD(TAG, "=== FLASH SLOTS INFO ===");
    ESP_LOGD(TAG, "Max slots: %d, Slot size: %d KB", RBPOWER_MAX_UNITS, RBPOWER_SLOT_SIZE_KB);

    for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
        if (g_flash_slots[i].is_occupied) {
            ESP_LOGD(TAG, "Slot %d: Unit 0x%04X @ 0x%06lX",
                     i, g_flash_slots[i].unit_id, (unsigned long)g_flash_slots[i].base_offset);
        } else {
            ESP_LOGD(TAG, "Slot %d: FREE @ 0x%06lX",
                     i, (unsigned long)g_flash_slots[i].base_offset);
        }
    }

    ESP_LOGD(TAG, "========================");
}

/**
 * @brief Initialize the slot system
 */
static esp_err_t init_flash_slots(void)
{
    if (g_slots_initialized) {
        ESP_LOGW(TAG, "Slots already initialized");
        return ESP_OK;
    }

    // Try to load the existing map
    esp_err_t ret = load_flash_slots_config();

    if (ret == ESP_ERR_NOT_FOUND) {
        // First run - create an empty map in RAM
        ESP_LOGI(TAG, "First run: initializing empty slot scheme in RAM");

        for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
            g_flash_slots[i].slot_index = i;
            g_flash_slots[i].base_offset = RBPOWER_SLOT_BASE_OFFSET(i);   // inside STATS partition
            g_flash_slots[i].unit_id = 0xFFFF;
            g_flash_slots[i].is_occupied = false;
        }

        // Try to save the map (non-critical if it fails)
        ret = save_flash_slots_config();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save slot config: %s (continuing in RAM-only mode)",
                     esp_err_to_name(ret));
            ESP_LOGW(TAG, "Slots will be re-allocated on next boot (no persistence)");
        } else {
            ESP_LOGI(TAG, "Empty slot scheme saved to %s", RBPOWER_FLASH_SLOTS_CONFIG_FILE);
        }
    } else if (ret != ESP_OK) {
        // Load error (other than NOT_FOUND) - initialize an empty map in RAM
        ESP_LOGW(TAG, "Failed to load slot config: %s, using RAM-only mode",
                 esp_err_to_name(ret));

        for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
            g_flash_slots[i].slot_index = i;
            g_flash_slots[i].base_offset = RBPOWER_SLOT_BASE_OFFSET(i);   // inside STATS partition
            g_flash_slots[i].unit_id = 0xFFFF;
            g_flash_slots[i].is_occupied = false;
        }
    } else {
        ESP_LOGI(TAG, "Slot scheme loaded from %s", RBPOWER_FLASH_SLOTS_CONFIG_FILE);
    }

    // Authoritative offsets: ALWAYS derive base_offset from slot_index + the CURRENT layout, never
    // from a persisted value (the loaded flash_slots.json may carry stale pre-rework offsets). This
    // makes a layout/chip change self-correcting. The unit_id↔slot mapping from the config is kept.
    for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
        g_flash_slots[i].base_offset = RBPOWER_SLOT_BASE_OFFSET(i);
    }

    // How many slots actually fit in the STATS region of THIS chip (runtime-sized: 4MB≈17, 16MB=64).
    // Units beyond this get no slot → RAM-only. stats_size is set in external_flash_init from flash_size.
    external_flash_state_t* fs = external_flash_get_state();
    uint32_t stats_size = (fs && fs->partitions.stats_size > 0) ? fs->partitions.stats_size : PARTITION_STATS_SIZE;
    uint32_t slot_bytes = RBPOWER_SLOT_SIZE_KB * 1024;
    int fit = (int)(stats_size / slot_bytes);
    g_max_usable_slots = (fit < RBPOWER_MAX_UNITS) ? fit : RBPOWER_MAX_UNITS;
    if (g_max_usable_slots < 1) g_max_usable_slots = 0;
    ESP_LOGI(TAG, "Slot layout: STATS=0x%06lX (%lu KB) → %d usable slots of %d KB (max units %d)",
             (unsigned long)stats_size, (unsigned long)(stats_size / 1024),
             g_max_usable_slots, RBPOWER_SLOT_SIZE_KB, RBPOWER_MAX_UNITS);
    if (g_max_usable_slots < RBPOWER_MAX_UNITS) {
        ESP_LOGW(TAG, "Chip holds only %d unit slots (< %d) — units beyond that are RAM-only. Use a bigger external flash for full %d-unit persistence.",
                 g_max_usable_slots, RBPOWER_MAX_UNITS, RBPOWER_MAX_UNITS);
    }

    // Slots are always initialized in RAM (regardless of whether the save succeeded)
    g_slots_initialized = true;

    // Log the current state
    print_flash_slots_info();

    return ESP_OK;
}

/**
 * @brief Find the first free slot
 *
 * @return Index of a free slot, or -1 if none is free
 */
static int find_first_free_slot(void)
{
    if (!g_slots_initialized) {
        ESP_LOGE(TAG, "Slots not initialized!");
        return -1;
    }

    // Only hand out slots that actually FIT in this chip's STATS region (g_max_usable_slots).
    for (int i = 0; i < g_max_usable_slots; i++) {
        if (!g_flash_slots[i].is_occupied) {
            ESP_LOGD(TAG, "Found free slot: %d (offset: 0x%06lX)",
                     i, (unsigned long)g_flash_slots[i].base_offset);
            return i;
        }
    }

    ESP_LOGW(TAG, "No free slots: all %d chip-fitting slots occupied (chip too small for more units → RAM-only)",
             g_max_usable_slots);
    return -1;
}

/**
 * @brief Find the slot of a specific unit
 *
 * @param unit_id Unit ID
 * @return Slot index, or -1 if not found
 */
static int find_slot_for_unit(rbpower_unit_id_t unit_id)
{
    for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
        if (g_flash_slots[i].unit_id == unit_id) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Place a unit into the given slot
 *
 * @param unit_id Unit ID
 * @param slot_idx Slot index
 * @return ESP_OK on success
 */
static esp_err_t allocate_slot_for_unit(rbpower_unit_id_t unit_id, int slot_idx)
{
    if (!g_slots_initialized) {
        ESP_LOGE(TAG, "Slots not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    if (slot_idx < 0 || slot_idx >= RBPOWER_MAX_UNITS) {
        ESP_LOGE(TAG, "Invalid slot index: %d", slot_idx);
        return ESP_ERR_INVALID_ARG;
    }

    if (g_flash_slots[slot_idx].is_occupied) {
        ESP_LOGE(TAG, "Slot %d already occupied by unit 0x%04X",
                 slot_idx, g_flash_slots[slot_idx].unit_id);
        return ESP_ERR_INVALID_STATE;
    }

    // Place the unit into the slot (in RAM)
    g_flash_slots[slot_idx].unit_id = unit_id;
    g_flash_slots[slot_idx].is_occupied = true;

    ESP_LOGI(TAG, "Unit 0x%04X allocated to slot %d (offset: 0x%06lX)",
             unit_id, slot_idx, (unsigned long)g_flash_slots[slot_idx].base_offset);

    // Try to save the map (non-critical if it fails)
    esp_err_t ret = save_flash_slots_config();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save slot config: %s (continuing in RAM-only mode)",
                 esp_err_to_name(ret));
        // Do NOT roll back the change - keep working from RAM
    }

    return ESP_OK;
}

/**
 * @brief Free the slot occupied by a unit
 *
 * @param unit_id Unit ID
 * @return ESP_OK on success
 */
static esp_err_t free_slot_for_unit(rbpower_unit_id_t unit_id)
{
    // Find the slot holding this unit
    for (int i = 0; i < RBPOWER_MAX_UNITS; i++) {
        if (g_flash_slots[i].unit_id == unit_id) {
            // Free the slot
            g_flash_slots[i].unit_id = 0xFFFF;
            g_flash_slots[i].is_occupied = false;

            // Flash data is NOT erased here!
            // The slot is ready for a new unit

            ESP_LOGI(TAG, "Unit 0x%04X removed from slot %d (offset: 0x%06lX)",
                     unit_id, i, (unsigned long)g_flash_slots[i].base_offset);

            // Try to save the change (non-critical if it fails)
            esp_err_t ret = save_flash_slots_config();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save slot config: %s (continuing in RAM-only mode)",
                         esp_err_to_name(ret));
            }
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Unit 0x%04X not found in any slot", unit_id);
    return ESP_ERR_NOT_FOUND;
}

/* Forget the RAM ring state of every buffer of `unit_id` (write cursor / sequence / newest ts cache).
 * The runtime buffers of a removed unit stay in g_runtime_manager (bound by unit_id, until reboot), so
 * a unit re-created with the SAME id resolves to them again. Without this reset their cache stayed
 * valid with the pre-delete cursor: get_record_count reported the old count over the freshly erased
 * slot and history served all-0xFF records (observed on unit delete→re-create), and the next
 * write would land mid-sector with no ACTIVE marker. Invalidated, the next access re-scans the erased
 * flash → empty ring → the first write takes the "initialize sector 0" path. Caller holds the write lock. */
static void forget_unit_ring_state(rbpower_unit_id_t unit_id)
{
    for (uint16_t i = 0; i < g_runtime_manager.buffer_count; i++) {
        rbpower_buffer_runtime_t* buf = &g_runtime_manager.buffers[i];
        if (buf->unit_id != unit_id) continue;
        buf->cached_active_sector = 0;
        buf->cached_next_record_index = 0;
        buf->cached_sequence_number = 0;
        buf->cached_newest_timestamp = 0;
        buf->cache_valid = false;
    }
}

/* Public: reclaim a deleted unit's flash STATS slot. ERASES the slot region first (free_slot_for_unit
 * deliberately does NOT erase, so without this a reused slot would let the next unit inherit the
 * removed unit's records via the ring-buffer scan-recovery), then marks the slot free + persists.
 * Called by the unit-deletion path (rbpower_home_remove_unit). No-op if flash unavailable / not slotted. */
esp_err_t rbpower_flash_remove_unit(rbpower_unit_id_t unit_id)
{
    /* External flash is OPTIONAL (not present by default). The flash-reclaim path (erase slot +
     * free + persist flash_slots.json) runs ONLY when the external flash is actually initialized
     * and available — otherwise there is nothing persisted to reclaim and esp_flash ops would fail.
     * The unit's RAM-side removal (home layer) still happens regardless. */
    if (!g_is_initialized || !rbpower_flash_is_available()) {
        ESP_LOGD(TAG, "remove_unit 0x%04X: external flash not initialized — skip flash reclaim (RAM-only)",
                 unit_id);
        return ESP_OK;
    }
    int slot = find_slot_for_unit(unit_id);
    if (slot < 0) {
        ESP_LOGW(TAG, "remove_unit 0x%04X: not in any slot (nothing to free)", unit_id);
        return ESP_ERR_NOT_FOUND;
    }

    esp_flash_t* flash = get_external_flash_handle();
    if (flash) {
        uint32_t base = g_flash_slots[slot].base_offset;
        uint32_t size = (uint32_t)RBPOWER_SLOT_SIZE_KB * 1024;  /* whole slot (v4: 320 KB = 80 sectors), 4K-aligned */
        /* Erase under the WRITE lock (write-priority) so a concurrent reader can't see a half-erased
         * slot. The unit is already marked removed at the home layer → no live writer for this slot. */
        if (!flash_lock_write(pdMS_TO_TICKS(2000))) {
            ESP_LOGE(TAG, "remove_unit 0x%04X: write-lock timeout", unit_id);
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t e = esp_flash_erase_region(flash, base, size);
        forget_unit_ring_state(unit_id);   /* same lock: no reader sees the old cursor over the erased slot */
        flash_unlock_write();
        ESP_LOGW(TAG, "remove_unit 0x%04X: erased slot %d region 0x%06lX..+%uKB: %s",
                 unit_id, slot, (unsigned long)base, (unsigned)RBPOWER_SLOT_SIZE_KB, esp_err_to_name(e));
        if (e != ESP_OK) return e;
    }

    /* Mark the slot free (unit_id=0xFFFF, is_occupied=false) + persist flash_slots.json so a future
     * find_first_free_slot reuses it. */
    return free_slot_for_unit(unit_id);
}

/* Public: a unit re-created at runtime with the id of a unit removed in this boot. Its runtime buffers
 * (still in g_runtime_manager, cursor reset by forget_unit_ring_state) keep writing into the slot that
 * rbpower_flash_remove_unit freed — re-occupy that slot + persist, so the next boot's
 * init_buffers_from_network finds the unit in it (find_slot_for_unit) instead of handing it another
 * free slot and leaving these records orphaned in a "free" slot a later unit would inherit.
 * No-op when flash is unavailable, the unit has no runtime buffers (a brand-new id gets its slot at the
 * next boot) or it is still slotted. */
esp_err_t rbpower_flash_reattach_unit(rbpower_unit_id_t unit_id)
{
    if (!g_is_initialized || !rbpower_flash_is_available()) return ESP_OK;
    if (find_slot_for_unit(unit_id) >= 0) return ESP_OK;
    /* The unit's first buffer starts at its slot base (init_buffers_from_network packs from there). */
    bool found = false;
    uint32_t base = 0;
    for (uint16_t i = 0; i < g_runtime_manager.buffer_count; i++) {
        const rbpower_buffer_runtime_t* buf = &g_runtime_manager.buffers[i];
        if (buf->unit_id != unit_id) continue;
        if (!found || buf->flash_offset < base) base = buf->flash_offset;
        found = true;
    }
    if (!found) return ESP_OK;
    for (int i = 0; i < g_max_usable_slots; i++) {
        if (g_flash_slots[i].base_offset != base) continue;
        if (g_flash_slots[i].is_occupied) {
            ESP_LOGE(TAG, "reattach 0x%04X: slot %d (0x%06lX) taken by 0x%04X", unit_id, i,
                     (unsigned long)base, g_flash_slots[i].unit_id);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "reattach 0x%04X: re-occupying its slot %d (0x%06lX)", unit_id, i, (unsigned long)base);
        return allocate_slot_for_unit(unit_id, i);
    }
    ESP_LOGW(TAG, "reattach 0x%04X: no slot at base 0x%06lX", unit_id, (unsigned long)base);
    return ESP_ERR_NOT_FOUND;
}

/* ================================================================================================
 * HELPER FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Find a runtime buffer by (unit, period, type)
 */
static const rbpower_buffer_runtime_t* find_buffer(rbpower_unit_id_t unit_id,
                                                     rbpower_period_t period,
                                                     rbpower_data_type_t data_type)
{
    return rbpower_runtime_find_buffer(&g_runtime_manager, unit_id, period, data_type);
}

/**
 * @brief Find a buffer (non-const pointer, for updating its cache)
 */
static rbpower_buffer_runtime_t* find_buffer_mutable(rbpower_unit_id_t unit_id,
                                                      rbpower_period_t period,
                                                      rbpower_data_type_t data_type)
{
    // Avoid casting away const: search the non-const array directly
    for (uint16_t i = 0; i < g_runtime_manager.buffer_count; i++) {
        rbpower_buffer_runtime_t* buf = &g_runtime_manager.buffers[i];
        if (buf->unit_id == unit_id && buf->period == period && buf->data_type == data_type) {
            return buf;
        }
    }
    return NULL;
}

/**
 * @brief Compute the physical flash address of a record
 */
static uint32_t calculate_record_address(const rbpower_buffer_runtime_t* buffer,
                                           uint16_t sector_index,
                                           uint16_t record_in_sector)
{
    // Buffer base address + sector offset + record offset
    uint32_t sector_offset = sector_index * RBPOWER_FLASH_SECTOR_SIZE;
    uint32_t record_offset = record_in_sector * buffer->record_size;

    return buffer->flash_offset + sector_offset + record_offset;
}

/* ================================================================================================
 * INITIALIZATION / DEINITIALIZATION
 * ================================================================================================ */
/* ================================================================================================
 * SECTOR MARKERS
 * ================================================================================================ */

/**
 * @brief Compute CRC16 over a sector's data area
 */
static uint16_t calculate_sector_crc(const rbpower_buffer_runtime_t* buffer, uint16_t sector_index)
{
    uint32_t sector_offset = buffer->flash_offset +
                            (sector_index * RBPOWER_FLASH_SECTOR_SIZE);

    // Read only the data area (without the metadata)
    uint8_t* data_buffer = malloc(SECTOR_DATA_SIZE);
    if (!data_buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for CRC calculation");
        return 0;
    }

    esp_flash_t* flash = get_external_flash_handle();
    esp_err_t ret = esp_flash_read(flash, data_buffer, sector_offset, SECTOR_DATA_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read sector data for CRC: %s", esp_err_to_name(ret));
        free(data_buffer);
        return 0;
    }

    uint16_t crc = crc16_ccitt(data_buffer, SECTOR_DATA_SIZE);
    free(data_buffer);

    return crc;
}

/**
 * @brief Get the highest sequence_number across all sectors of the buffer
 * @return Highest sequence_number, or 0 if none found
 */
static uint32_t get_max_sequence_number(const rbpower_buffer_runtime_t* buffer)
{
    sector_metadata_t meta;
    esp_flash_t* flash = get_external_flash_handle();
    uint32_t max_seq = 0;

    for (uint16_t i = 0; i < buffer->total_sectors; i++) {
        uint32_t meta_offset = buffer->flash_offset +
                              (i * RBPOWER_FLASH_SECTOR_SIZE) +
                              SECTOR_METADATA_OFFSET;

        esp_err_t ret = esp_flash_read(flash, &meta, meta_offset, sizeof(meta));
        if (ret != ESP_OK) {
            continue;
        }

        // Consider only initialized sectors
        if (meta.init_magic == SECTOR_INIT_MAGIC && meta.sequence_number > max_seq) {
            max_seq = meta.sequence_number;
        }
    }

    ESP_LOGD(TAG, "Max sequence_number found: %lu", max_seq);
    return max_seq;
}

/* ================================================================================================
 * BUFFER STATE CACHE
 * ================================================================================================ */

/**
 * @brief Invalidate the buffer state cache
 */
static inline void invalidate_buffer_cache(rbpower_buffer_runtime_t* buffer)
{
    buffer->cache_valid = false;
}

/**
 * @brief Update the buffer state cache
 *
 * @param buffer Buffer pointer (non-const, the cache is modified)
 * @param active_sector Index of the active sector
 * @param next_record_index Index of the next record slot in the active sector
 * @param sequence_number Sequence number of the active sector
 */
static inline void update_buffer_cache(rbpower_buffer_runtime_t* buffer,
                                       uint16_t active_sector,
                                       uint16_t next_record_index,
                                       uint32_t sequence_number)
{
    buffer->cached_active_sector = active_sector;
    buffer->cached_next_record_index = next_record_index;
    buffer->cached_sequence_number = sequence_number;

    // Initialize newest_timestamp from flash on the first cache initialization
    if (!buffer->cache_valid && next_record_index > 0) {
        // Records exist - read ONLY the timestamp (4 bytes) of the last record
        esp_flash_t* flash = get_external_flash_handle();
        if (flash) {
            uint32_t last_record_addr = buffer->flash_offset +
                                        (active_sector * RBPOWER_FLASH_SECTOR_SIZE) +
                                        ((next_record_index - 1) * buffer->record_size);
            uint32_t last_ts = 0;
            esp_err_t ret = esp_flash_read(flash, &last_ts, last_record_addr, sizeof(uint32_t));

            if (ret == ESP_OK) {
                buffer->cached_newest_timestamp = last_ts;
                ESP_LOGD(TAG, "Initialized newest_timestamp=%lu from Flash", last_ts);
            } else {
                buffer->cached_newest_timestamp = 0;
                ESP_LOGW(TAG, "Failed to read newest_timestamp: %s", esp_err_to_name(ret));
            }
        }
    } else if (!buffer->cache_valid) {
        // No records - timestamp = 0
        buffer->cached_newest_timestamp = 0;
        ESP_LOGD(TAG, "No records, newest_timestamp=0");
    }

    buffer->cache_valid = true;

    ESP_LOGD(TAG, "Cache updated: sector=%d, index=%d, seq=%lu, newest_ts=%lu",
             active_sector, next_record_index, sequence_number, buffer->cached_newest_timestamp);
}

/**
 * @brief Get the cached buffer state
 *
 * @param buffer Buffer pointer
 * @param active_sector_out Output: index of the active sector
 * @param next_record_index_out Output: index of the next record slot
 * @return true if the cache is valid, false if the flash must be scanned
 */
static inline bool get_cached_buffer_state(const rbpower_buffer_runtime_t* buffer,
                                           uint16_t* active_sector_out,
                                           uint16_t* next_record_index_out)
{
    if (buffer->cache_valid) {
        *active_sector_out = buffer->cached_active_sector;
        *next_record_index_out = buffer->cached_next_record_index;
        ESP_LOGD(TAG, "Using cached state: sector=%d, index=%d",
                 buffer->cached_active_sector, buffer->cached_next_record_index);
        return true;
    }
    return false;
}

/**
 * @brief Find active sector (with ACTIVE flag and maximum sequence_number)
 *
 * Important: if multiple ACTIVE sectors exist (e.g., after power failure
 * during close_sector), selects sector with maximum sequence_number.
 *
 * @return Active sector index or SECTOR_NOT_FOUND
 */
static uint16_t find_active_sector(const rbpower_buffer_runtime_t* buffer)
{
    sector_metadata_t meta;
    esp_flash_t* flash = get_external_flash_handle();

    uint16_t best_sector = SECTOR_NOT_FOUND;
    uint32_t max_seq = 0;
    uint8_t active_count = 0;

    ESP_LOGD(TAG, "Scanning %d sectors for active marker...", buffer->total_sectors);

    for (uint16_t i = 0; i < buffer->total_sectors; i++) {
        uint32_t meta_offset = buffer->flash_offset +
                              (i * RBPOWER_FLASH_SECTOR_SIZE) +
                              SECTOR_METADATA_OFFSET;

        esp_err_t ret = esp_flash_read(flash, &meta, meta_offset, sizeof(meta));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read sector %d metadata: %s", i, esp_err_to_name(ret));
            continue;
        }

        if (sector_is_active(&meta)) {
            active_count++;
            ESP_LOGD(TAG, "Found ACTIVE sector %d: seq=%lu, timestamp=%lu",
                     i, meta.sequence_number, meta.last_write_timestamp);

            // Select sector with maximum sequence_number
            if (meta.sequence_number > max_seq) {
                max_seq = meta.sequence_number;
                best_sector = i;
            }
        }
    }

    if (best_sector != SECTOR_NOT_FOUND) {
        if (active_count > 1) {
            ESP_LOGW(TAG, "Multiple ACTIVE sectors detected (%d), selected sector %d with max seq=%lu",
                     active_count, best_sector, max_seq);
        } else {
            ESP_LOGD(TAG, "Found active sector: %d (seq=%lu)", best_sector, max_seq);
        }
        return best_sector;
    }

    ESP_LOGD(TAG, "No active sector found (first run)");
    return SECTOR_NOT_FOUND;
}

/**
 * @brief Find last record index in sector
 * @return Index for next record (0 if sector is empty)
 */
static uint16_t find_record_index(const rbpower_buffer_runtime_t* buffer, uint16_t sector_index)
{
    // Defensive: a corrupt/incompatible buffer (bad record_size) OR a not-found/out-of-range sector
    // (e.g. SECTOR_NOT_FOUND=0xFFFF on an empty ring) must not divide-by-zero or read at a wild
    // offset (0xFFFF×4096 ≈ 268MB, past the chip) → treat as empty, never crash.
    if (buffer->record_size == 0 || buffer->record_size > SECTOR_DATA_SIZE) {
        ESP_LOGE(TAG, "find_record_index: invalid record_size=%u → sector treated empty", buffer->record_size);
        return 0;
    }
    if (sector_index >= buffer->total_sectors) {
        ESP_LOGD(TAG, "find_record_index: sector %u out of range (total %u) → empty", sector_index, buffer->total_sectors);
        return 0;
    }
    uint16_t records_per_sector = SECTOR_DATA_SIZE / buffer->record_size;
    uint8_t* scan_buffer = malloc(buffer->record_size);
    if (!scan_buffer) {
        ESP_LOGE(TAG, "Failed to allocate scan buffer");
        return 0;
    }

    esp_flash_t* flash = get_external_flash_handle();

    // Scan from end to beginning
    for (int16_t i = records_per_sector - 1; i >= 0; i--) {
        uint32_t record_in_sector_offset = i * buffer->record_size;

        // Check that record doesn't overlap metadata
        if (record_in_sector_offset + buffer->record_size > SECTOR_DATA_SIZE) {
            continue; // Skip this record, it exceeds boundary
        }

        uint32_t record_offset = buffer->flash_offset +
                                (sector_index * RBPOWER_FLASH_SECTOR_SIZE) +
                                record_in_sector_offset;

        esp_err_t ret = esp_flash_read(flash, scan_buffer, record_offset, buffer->record_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read record %d: %s", i, esp_err_to_name(ret));
            free(scan_buffer);
            return 0;
        }

        // Check if record is empty (all 0xFF)
        bool is_empty = true;
        for (uint16_t j = 0; j < buffer->record_size; j++) {
            if (scan_buffer[j] != 0xFF) {
                is_empty = false;
                break;
            }
        }

        if (!is_empty) {
            free(scan_buffer);
            uint16_t next_index = i + 1;
            ESP_LOGD(TAG, "Last record at %d, next index: %d", i, next_index);
            return next_index;
        }
    }

    free(scan_buffer);
    ESP_LOGD(TAG, "Sector %d is empty", sector_index);
    return 0;
}

/* True if the FIRST record slot of `sector_index` is written (not all-0xFF). Used to detect a
 * PHYSICAL ring wrap (the oldest sector is populated) without trusting the fragile sequence-number
 * heuristic, which climbs spuriously across erase/rewrite cycles and falsely reports "wrapped"
 * (observed: history returned 0 records after an OTA — an over-count read into uninitialized sectors).
 * Reads one record; caller holds the read lock. (record 0 is at the sector's data-area start.) */
static bool sector_has_first_record(const rbpower_buffer_runtime_t* buffer, uint16_t sector_index)
{
    if (buffer->record_size == 0 || sector_index >= buffer->total_sectors) {
        return false;
    }
    uint8_t* rec = malloc(buffer->record_size);
    if (!rec) {
        return false;
    }
    uint32_t off = buffer->flash_offset + (uint32_t)sector_index * RBPOWER_FLASH_SECTOR_SIZE;  // record 0
    bool written = false;
    if (esp_flash_read(get_external_flash_handle(), rec, off, buffer->record_size) == ESP_OK) {
        for (uint16_t j = 0; j < buffer->record_size; j++) {
            if (rec[j] != 0xFF) { written = true; break; }
        }
    }
    free(rec);
    return written;
}

/**
 * @brief Close sector (write metadata with CLOSED flag)
 */
static esp_err_t close_sector(const rbpower_buffer_runtime_t* buffer, uint16_t sector_index)
{
    esp_flash_t* flash = get_external_flash_handle();
    uint32_t meta_offset = buffer->flash_offset +
                          (sector_index * RBPOWER_FLASH_SECTOR_SIZE) +
                          SECTOR_METADATA_OFFSET;

    // Read existing metadata (to preserve init_magic and sequence_number)
    sector_metadata_t meta;
    esp_err_t ret = esp_flash_read(flash, &meta, meta_offset, sizeof(meta));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read sector %d metadata before closing: %s",
                 sector_index, esp_err_to_name(ret));
        return ret;
    }

    // Calculate CRC of data area
    uint16_t crc = calculate_sector_crc(buffer, sector_index);

    // Update only necessary fields (preserve init_magic and sequence_number)
    meta.last_write_timestamp = (uint32_t)time(NULL);
    meta.crc16 = crc;
    meta.flags = SECTOR_FLAG_CLOSED;

    // Write updated metadata
    ret = esp_flash_write(flash, &meta, meta_offset, sizeof(meta));

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Sector %d closed: seq=%lu, timestamp=%lu, crc=0x%04X",
                 sector_index, meta.sequence_number, meta.last_write_timestamp, meta.crc16);
    } else {
        ESP_LOGE(TAG, "Failed to close sector %d: %s", sector_index, esp_err_to_name(ret));
    }

    return ret;
}

/* ================================================================================================
 * API FOR CHECKING FLASH AVAILABILITY
 * ================================================================================================ */

// Stats-write gating:
//  - g_ext_flash_enabled: user/runtime "use external flash" switch → satisfies "if available AND
//    ENABLED → write". Default ON (use the chip whenever it's present and valid).
//  - g_ext_flash_enforce_otp: when true, a chip that FAILS OTP authenticity (foreign / not the
//    prepared chip) is treated as unavailable for stats → anti-foreign-chip enforcement.
//    DEFAULT OFF (design decision): for an energy-metering product,
//    continuity of stored history outweighs the anti-foreign-chip gate — a device that ships with
//    a generic / un-OTP-provisioned NOR must still persist its energy history, not silently lose
//    it. OTP enforcement stays available as an OPT-IN (rbpower_flash_set_enforce_otp(true)) for a
//    secure/premium SKU that provisions its chips. Boot still soft-fails either way.
static bool g_ext_flash_enabled     = true;
static bool g_ext_flash_enforce_otp = false;   // history continuity > authenticity gate

void rbpower_flash_set_enabled(bool enabled)      { g_ext_flash_enabled = enabled; }
void rbpower_flash_set_enforce_otp(bool enforce)  { g_ext_flash_enforce_otp = enforce; }
bool rbpower_flash_get_enabled(void)              { return g_ext_flash_enabled; }
bool rbpower_flash_get_enforce_otp(void)          { return g_ext_flash_enforce_otp; }

bool rbpower_flash_is_available(void)
{
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state) {
        return false;
    }

    // #4: respect the runtime enable switch.
    if (!g_ext_flash_enabled) {
        return false;
    }

    // Flash hardware up + filesystem mounted.
    if (!flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        return false;
    }

    // #2: reject a foreign/un-prepared chip — OTP authenticity must pass when enforced.
    if (g_ext_flash_enforce_otp && !flash_state->status.otp_validated) {
        return false;
    }

    return true;
}

/* ================================================================================================
 * v3→v4 MIGRATOR SUPPORT (public helpers for rbstats_migrate_v3v4.c)
 * ================================================================================================ */

esp_err_t rbpower_flash_slot_map_get(uint16_t *unit_by_slot_out, int max_slots, int *used_out)
{
    if (!unit_by_slot_out || max_slots <= 0) return ESP_ERR_INVALID_ARG;
    if (!g_slots_initialized) {
        esp_err_t e = init_flash_slots();
        if (e != ESP_OK) return e;
    }
    int used = 0;
    for (int i = 0; i < RBPOWER_MAX_UNITS && i < max_slots; i++) {
        unit_by_slot_out[i] = g_flash_slots[i].is_occupied ? g_flash_slots[i].unit_id : 0xFFFF;
        if (g_flash_slots[i].is_occupied) used = i + 1;
    }
    if (used_out) *used_out = used;
    return ESP_OK;
}

uint32_t rbpower_flash_slot_base_offset(int slot)
{
    return RBPOWER_SLOT_BASE_OFFSET(slot);
}

int rbpower_flash_max_usable_slots(void)
{
    return g_max_usable_slots;
}

/* ================================================================================================
 * MODULE INITIALIZATION
 * ================================================================================================ */

/** @brief LittleFS path of the stats record-format marker (config partition): one uint32 = format version. */
#define STATS_FORMAT_MARKER_FILE "/stats_fmt"

/* Auto-migrate the STATS region when the on-flash record format changes (non-destructive
 * within the compatibility window).
 * Compares the marker's stored version to RBPOWER_RECORD_FORMAT_VERSION:
 *   - stored == current                          → no-op (idempotent; history persists).
 *   - stored ∈ [MIN_COMPATIBLE, current)         → BACKWARD-COMPATIBLE upgrade → KEEP history,
 *       just restamp the marker to current. Existing record types are immutable (contract, see
 *       rbextflash_structures.h), so old records still read at their unchanged stride; any new record
 *       TYPE added by the new format simply has an empty ring buffer on this device. NO WIPE.
 *   - stored < MIN_COMPATIBLE / stored > current (BREAKING, data present) → NO WIPE: defer to the
 *       migration engine (rbstats_migrate). Storage stays offline until the
 *       registered migrator lifts the region; caller must SKIP slot init (returns false).
 *   - no marker (fresh region or pre-versioning development data) → wipe stays correct and cheap,
 *       unless initialized sectors are found (lost marker) → keep the data and re-stamp.
 * Runs BEFORE any record read. Returns true when init may proceed over the region. */
/* Any sector of the STATS region carries an initialized metadata block (init_magic 0xA5 at 4080+7)?
 * One byte per 4 KB sector — about 1000 tiny reads for 4 MB. */
static bool stats_region_has_data(void)
{
    esp_flash_t* fh = get_external_flash_handle();
    if (!fh) return false;
    uint32_t size;
    if (rbmedia_mode() == RBMEDIA_INTERNAL) {
        size = rbmedia_stats_region_size();
    } else {
        external_flash_state_t* fs = external_flash_get_state();
        size = (fs && fs->partitions.stats_size > 0) ? fs->partitions.stats_size : PARTITION_STATS_SIZE;
    }
    for (uint32_t off = 0; off + RBPOWER_FLASH_SECTOR_SIZE <= size; off += RBPOWER_FLASH_SECTOR_SIZE) {
        uint8_t magic = 0xFF;
        if (esp_flash_read(fh, &magic, PARTITION_STATS_OFFSET + off + SECTOR_METADATA_OFFSET +
                           offsetof(sector_metadata_t, init_magic), 1) == ESP_OK && magic == SECTOR_INIT_MAGIC)
            return true;
    }
    return false;
}

static bool flash_check_and_migrate_format(void)
{
    uint32_t stored = 0;
    size_t actual = 0;
    bool have_marker = false;
    if (littlefs_raw_exists(STATS_FORMAT_MARKER_FILE)) {
        // The marker file is PRESENT → a prior boot stamped a format version, so the STATS region
        // holds real (known-format) billing data. Read it; only a CONFIRMED incompatibility may wipe.
        esp_err_t re = littlefs_raw_read(STATS_FORMAT_MARKER_FILE, &stored, sizeof(stored), &actual);
        if (re != ESP_OK || actual != sizeof(stored)) {
            // Marker exists but couldn't be read (transient FS/IO error, or a torn write). Do NOT
            // wipe on a read failure — that would destroy valid history on a hiccup.
            // Skip migration this boot; the marker is re-read next boot. A user can force a
            // rebuild via the `flasherase` console command if the marker is genuinely corrupt.
            ESP_LOGE(TAG, "STATS format marker present but unreadable (%s, %u bytes) — skipping "
                          "migration this boot (NOT wiping history)",
                     esp_err_to_name(re), (unsigned)actual);
            return true;
        }
        have_marker = true;
    }

    if (have_marker && stored == RBPOWER_RECORD_FORMAT_VERSION) {
        ESP_LOGI(TAG, "STATS record format v%d — OK (no migration)", RBPOWER_RECORD_FORMAT_VERSION);
        return true;
    }

    // Backward-compatible upgrade: stored is an OLDER but still-readable format (within the compat
    // window). Existing record types are unchanged → keep the data; just advance the marker.
    if (have_marker &&
        stored >= RBPOWER_RECORD_FORMAT_MIN_COMPATIBLE &&
        stored <  RBPOWER_RECORD_FORMAT_VERSION) {
        uint32_t cur = RBPOWER_RECORD_FORMAT_VERSION;
        esp_err_t we = littlefs_raw_write(STATS_FORMAT_MARKER_FILE, &cur, sizeof(cur));
        ESP_LOGW(TAG, "STATS format v%lu → v%d: BACKWARD-COMPATIBLE upgrade — history PRESERVED "
                      "(marker restamp: %s)",
                 (unsigned long)stored, RBPOWER_RECORD_FORMAT_VERSION, esp_err_to_name(we));
        return true;
    }

    if (have_marker) {
        // BREAKING change over REAL data (older than the window, or a downgrade): never wipe
        // (billing history must survive firmware updates). Hand the region to the deferred migration engine — it waits
        // for OTA image validation, migrates slot-by-slot in the background and resumes init;
        // with no registered migrator the storage stays INCOMPATIBLE with data preserved.
        ESP_LOGW(TAG, "STATS record format v%lu vs current v%d (min-compat v%d) → BREAKING, "
                      "deferring to the migration engine (NO wipe)",
                 (unsigned long)stored, RBPOWER_RECORD_FORMAT_VERSION, RBPOWER_RECORD_FORMAT_MIN_COMPATIBLE);
        rbstats_migrate_begin_deferred(stored);
        return false;   // caller must NOT init slots over the old-format region
    }

    /* No marker, but the region holds initialized sectors: the marker was LOST (a torn config write, a config
     * partition format), the history is real. A blind wipe here would destroy all of it.
     * Keep the data, re-stamp the current format, say so loudly. */
    if (stats_region_has_data()) {
        uint32_t cur = RBPOWER_RECORD_FORMAT_VERSION;
        esp_err_t we = littlefs_raw_write(STATS_FORMAT_MARKER_FILE, &cur, sizeof(cur));
        ESP_LOGE(TAG, "STATS format marker missing but the region holds data — NOT wiping; marker re-stamped v%d (%s)",
                 RBPOWER_RECORD_FORMAT_VERSION, esp_err_to_name(we));
        rbpower_diag_printf("FAIL stats: format marker missing over existing history — kept the data, re-stamped v%d",
                            RBPOWER_RECORD_FORMAT_VERSION);
        return true;
    }
    // No marker: a fresh region or pre-versioning development data (versioning shipped before any public
    // release). Wipe — erasing an empty region is cheap + harmless, and there is no user data here.
    ESP_LOGW(TAG, "STATS region has no format marker → fresh/pre-versioning, "
                  "AUTO-MIGRATING (wiping stats region)");
    esp_err_t e = rbpower_flash_erase_all_slots();   // blind STATS-region wipe (no reads)
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "STATS auto-migrate erase failed: %s — history may be unreadable", esp_err_to_name(e));
        return true;
    }
    uint32_t cur = RBPOWER_RECORD_FORMAT_VERSION;
    esp_err_t we = littlefs_raw_write(STATS_FORMAT_MARKER_FILE, &cur, sizeof(cur));
    ESP_LOGW(TAG, "STATS auto-migrated to format v%d (marker write: %s)",
             RBPOWER_RECORD_FORMAT_VERSION, esp_err_to_name(we));
    return true;
}

/* Diagnostics for the record-format compat state (`fmtinfo` console command). ESP_LOG boot lines aren't
 * visible on the diagnostic console, so expose current/min-compatible + the stored marker on demand —
 * useful for validating future format migrations. Read-only. */
void rbpower_flash_get_format_info(uint32_t* current, uint32_t* min_compatible,
                                   uint32_t* stored, bool* marker_present)
{
    if (current)        *current        = RBPOWER_RECORD_FORMAT_VERSION;
    if (min_compatible) *min_compatible = RBPOWER_RECORD_FORMAT_MIN_COMPATIBLE;
    uint32_t s = 0;
    bool present = false;
    if (littlefs_raw_exists(STATS_FORMAT_MARKER_FILE)) {
        size_t actual = 0;
        if (littlefs_raw_read(STATS_FORMAT_MARKER_FILE, &s, sizeof(s), &actual) == ESP_OK &&
            actual == sizeof(s)) {
            present = true;
        }
    }
    if (stored)         *stored         = s;
    if (marker_present) *marker_present = present;
}

/* Clear the stats format marker — pairs with a full region wipe (flasherase): a wiped region is
 * FRESH, so the stale marker must go too, or a version-downgrade boot (e.g. a v4 image back to
 * a v3 image) wrongly lands in the BREAKING/migration branch over an empty region. */
esp_err_t rbpower_flash_format_marker_clear(void)
{
    return littlefs_raw_remove(STATS_FORMAT_MARKER_FILE);
}

/* Restamp the format marker to the CURRENT version — called by the region migrator as
 * its atomic finale (only a fully migrated region may carry the new marker). */
esp_err_t rbpower_flash_format_marker_restamp(void)
{
    uint32_t cur = RBPOWER_RECORD_FORMAT_VERSION;
    return littlefs_raw_write(STATS_FORMAT_MARKER_FILE, &cur, sizeof(cur));
}

esp_err_t rbpower_flash_operations_init(void)
{
    if (g_is_initialized) {
        ESP_LOGW(TAG, "Flash operations already initialized");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Initializing Flash operations module...");

    // Check flash availability
    if (!rbpower_flash_is_available()) {
        ESP_LOGW(TAG, "External Flash not available - Flash operations will work in RAM-only mode");
        ESP_LOGD(TAG, "Skipping Flash buffer initialization (no persistence)");
        g_is_initialized = false;
        return ESP_OK;
    }

    ESP_LOGD(TAG, "✓ Flash available, proceeding with initialization...");

#if RBPOWER_RECORD_FORMAT_VERSION >= 4
    // The v3->v4 region migrator must be registered BEFORE the format check sees the
    // breaking marker, or the engine will park the region as INCOMPATIBLE (no-migrator policy).
    {
        extern esp_err_t rbstats_migrate_v3_to_v4(uint32_t, void (*)(uint8_t));
        rbstats_migrate_register(3, rbstats_migrate_v3_to_v4);
    }
#endif

    // Auto-migrate the STATS region if the on-flash record format changed. MUST run before any
    // buffer/cursor read. Non-destructive for backward-compatible upgrades (additive new record
    // types within the [MIN_COMPATIBLE, VERSION] window → history preserved); a BREAKING version
    // over real data defers to the background migration engine instead of wiping.
    if (!flash_check_and_migrate_format()) {
        // The migration engine owns the region now (waits for OTA validation, migrates in the
        // background, then re-enters this init). Until then: finalize results go to the pending
        // queue (write gate in rbpower_flash_buffer_write_basic_set), history answers 503 via
        // storage_state. Deliberately NOT g_is_initialized.
        ESP_LOGW(TAG, "flash operations init DEFERRED until the stats migration completes");
        return ESP_OK;
    }

    // Create the mutex for thread safety
    g_flash_mutex = xSemaphoreCreateMutex();
    if (!g_flash_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Initialize the fixed-slot system
    esp_err_t ret = init_flash_slots();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init flash slots: %s", esp_err_to_name(ret));
        vSemaphoreDelete(g_flash_mutex);
        g_flash_mutex = NULL;
        return ret;
    }

    // Initialize the runtime manager
    ret = rbpower_runtime_config_init(&g_runtime_manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init runtime manager: %s", esp_err_to_name(ret));
        vSemaphoreDelete(g_flash_mutex);
        g_flash_mutex = NULL;
        return ret;
    }

    // Load the configuration from JSON (if it exists)
    ret = rbpower_runtime_config_load_all(&g_runtime_manager);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No configuration files found, will use empty config");
        // This is normal on first boot
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config: %s", esp_err_to_name(ret));
        rbpower_runtime_config_deinit(&g_runtime_manager);
        vSemaphoreDelete(g_flash_mutex);
        g_flash_mutex = NULL;
        return ret;
    }


    g_is_initialized = true;
    ESP_LOGD(TAG, "Flash operations module initialized (units: %d, buffers: %d)",
             g_runtime_manager.unit_count, g_runtime_manager.buffer_count);

    return ESP_OK;
}

void rbpower_flash_operations_deinit(void)
{
    if (!g_is_initialized) {
        return;
    }

    ESP_LOGD(TAG, "Deinitializing Flash operations module...");

    if (g_flash_mutex) {
        vSemaphoreDelete(g_flash_mutex);
        g_flash_mutex = NULL;
    }

    rbpower_runtime_config_deinit(&g_runtime_manager);
    g_is_initialized = false;
    ESP_LOGD(TAG, "Flash operations module deinitialized");
}

/* ================================================================================================
 * PUBLIC API - WRITE
 * ================================================================================================ */

esp_err_t rbpower_flash_write_record(rbpower_unit_id_t unit_id,
                                      rbpower_period_t period,
                                      rbpower_data_type_t data_type,
                                      const void* data,
                                      size_t data_size)
{
    // Flash availability gate — TWO distinct cases, do NOT collapse them:
    //  (1) Flash explicitly DISABLED (g_ext_flash_enabled==false): legitimate RAM-only build —
    //      silently skip + report success; the caller never expected persistence.
    //  (2) Flash ENABLED but not actually writable (hw init failed / fs not mounted / OTP-gated):
    //      a REAL failure to persist data the caller DID expect to land. Returning ESP_OK here
    //      silently MASKS data loss as success — observed on an un-OTP-provisioned device:
    //      every HOUR write "succeeded" yet the stored history stayed empty.
    //      Surface it (warn + error) so the store layer logs FAIL instead of a false OK.
    if (!rbpower_flash_get_enabled()) {
        return ESP_OK;  // case (1): RAM-only by configuration — expected, silent
    }
    if (!g_is_initialized || !rbpower_flash_is_available()) {
        // case (2): persistence was expected but flash is unavailable — never fake success.
        ESP_LOGW(TAG, "WRITE DROPPED — flash ENABLED but unavailable (init=%d available=%d): "
                      "unit=0x%04X period=%d type=%d NOT persisted. Check flashinfo (OTP/mount); "
                      "a device without an OTP production record needs OTP enforcement off (flashen 1 0).",
                 (int)g_is_initialized, (int)rbpower_flash_is_available(),
                 (unsigned)unit_id, (int)period, (int)data_type);
        return ESP_ERR_INVALID_STATE;
    }

    if (!data) {
        ESP_LOGE(TAG, "Data pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Take the mutex for thread safety (WRITE PRIORITY: blocks new readers at the gate)
    if (!flash_lock_write(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to take mutex (write)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    // Find the buffer (non-const, its cache is modified)
    rbpower_buffer_runtime_t* buffer = find_buffer_mutable(unit_id, period, data_type);
    if (!buffer) {
        ESP_LOGE(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d",
                 unit_id, period, data_type);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // Check the data size
    if (data_size != buffer->record_size) {
        ESP_LOGE(TAG, "Data size mismatch: expected %d, got %d",
                 buffer->record_size, data_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    // Try to use the cache
    uint16_t current_sector;
    uint16_t record_in_sector;

#if CONFIG_RBPOWER_VERBOSE_DEBUG
    ESP_LOGI(TAG, "=== VERBOSE: Buffer write: unit=0x%04X, period=%d, type=%d ===", unit_id, period, data_type);
    ESP_LOGI(TAG, "  flash_offset=0x%08lX, total_sectors=%u, record_size=%u",
             (unsigned long)buffer->flash_offset, buffer->total_sectors, buffer->record_size);
#endif

    if (get_cached_buffer_state(buffer, &current_sector, &record_in_sector)) {
        // Cache valid - use the cached state
        ESP_LOGD(TAG, "Using cache: current_sector=%u, record_in_sector=%u", current_sector, record_in_sector);
    } else {
        // Cache invalid - the flash must be scanned
        ESP_LOGD(TAG, "Cache invalid, scanning Flash");
        current_sector = find_active_sector(buffer);
        ESP_LOGD(TAG, "  find_active_sector() returned: %u (0x%04X)", current_sector, current_sector);

        if (current_sector == SECTOR_NOT_FOUND) {
            record_in_sector = 0;  // Initialized below
            ESP_LOGD(TAG, "  No active sector found, will initialize sector 0");
        } else {
            record_in_sector = find_record_index(buffer, current_sector);
            ESP_LOGD(TAG, "  find_record_index() returned: %u", record_in_sector);
        }
    }

    if (current_sector == SECTOR_NOT_FOUND) {
        // First write - initialize the first sector
        ESP_LOGD(TAG, "First write to buffer, initializing sector 0");
        current_sector = 0;
        record_in_sector = 0;

        // Erase the first sector
        esp_flash_t* flash_handle = get_external_flash_handle();
        uint32_t sector_addr = buffer->flash_offset;
        ret = esp_flash_erase_region(flash_handle, sector_addr, RBPOWER_FLASH_SECTOR_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase first sector: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        // Get the next sequence_number (highest found + 1; 1 on a fresh ring)
        uint32_t next_seq = get_max_sequence_number(buffer) + 1;

        ESP_LOGD(TAG, "Initializing sector 0 with seq=%lu", next_seq);

        // Write the ACTIVE marker
        sector_metadata_t meta;
        sector_metadata_init_active(&meta, next_seq);
        uint32_t meta_offset = sector_addr + SECTOR_METADATA_OFFSET;
        ret = esp_flash_write(flash_handle, &meta, meta_offset, sizeof(meta));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write active marker: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        // Update the cache for the first sector
        update_buffer_cache(buffer, current_sector, 0, next_seq);
    }

    // Compute the maximum number of records per sector
    uint16_t records_per_sector = SECTOR_DATA_SIZE / buffer->record_size;

#if CONFIG_RBPOWER_VERBOSE_DEBUG
    ESP_LOGI(TAG, "=== VERBOSE: Sector capacity check ===");
    ESP_LOGI(TAG, "  current_sector=%u, record_in_sector=%u", current_sector, record_in_sector);
    ESP_LOGI(TAG, "  SECTOR_DATA_SIZE=%u, record_size=%u, records_per_sector=%u",
             SECTOR_DATA_SIZE, buffer->record_size, records_per_sector);
    ESP_LOGI(TAG, "  Check: record_in_sector(%u) >= records_per_sector(%u) ? %s",
             record_in_sector, records_per_sector,
             (record_in_sector >= records_per_sector) ? "YES (FULL)" : "NO (OK)");
#endif

    // Check for sector overflow
    if (record_in_sector >= records_per_sector) {
        /* Order: open the NEXT sector (erase + ACTIVE marker) BEFORE closing the full one. Closing first left
         * no ACTIVE sector on a power cut in between → the next write took the first-write path, erased
         * sector 0 and restarted the ring over live history. Two ACTIVE sectors after
         * a cut are resolved by the max-sequence rule (find_active / repair). */
        ESP_LOGD(TAG, "Sector %u full, moving to next", current_sector);

        // Move to the next sector
        uint16_t old_sector = current_sector;
        current_sector = (current_sector + 1) % buffer->total_sectors;
        record_in_sector = 0;

        ESP_LOGD(TAG, "Transitioning: old_sector=%u -> new_sector=%u (total_sectors=%u)",
                 old_sector, current_sector, buffer->total_sectors);
        ESP_LOGD(TAG, "  Calculation: (%u + 1) %% %u = %u", old_sector, buffer->total_sectors, current_sector);

        // Erase the new sector
        esp_flash_t* flash_handle = get_external_flash_handle();
        uint32_t sector_addr = buffer->flash_offset + (current_sector * RBPOWER_FLASH_SECTOR_SIZE);
        ret = esp_flash_erase_region(flash_handle, sector_addr, RBPOWER_FLASH_SECTOR_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase sector %d: %s", current_sector, esp_err_to_name(ret));
            goto cleanup;
        }

        // Get the next sequence_number
        uint32_t next_seq = get_max_sequence_number(buffer) + 1;

        ESP_LOGI(TAG, "Initializing sector %d with seq=%lu", current_sector, next_seq);

        // Write the ACTIVE marker
        sector_metadata_t meta;
        sector_metadata_init_active(&meta, next_seq);
        uint32_t meta_offset = sector_addr + SECTOR_METADATA_OFFSET;
        ret = esp_flash_write(flash_handle, &meta, meta_offset, sizeof(meta));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write active marker: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        // Close the full sector (only after the new one is already ACTIVE — see ordering note above)
        ret = close_sector(buffer, old_sector);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close sector %u: %s", old_sector, esp_err_to_name(ret));
        }

        // Update the cache for the new sector
        update_buffer_cache(buffer, current_sector, 0, next_seq);
    }

    // Compute the write address
    uint32_t flash_address = calculate_record_address(buffer, current_sector, record_in_sector);

    ESP_LOGI(TAG, "=== DEBUG: Calculate record address ===");
    ESP_LOGI(TAG, "  buffer->flash_offset=0x%08lX (%lu)", (unsigned long)buffer->flash_offset, (unsigned long)buffer->flash_offset);
    ESP_LOGI(TAG, "  current_sector=%u, SECTOR_SIZE=%u, sector_offset=0x%08lX (%lu)",
             current_sector, RBPOWER_FLASH_SECTOR_SIZE,
             (unsigned long)(current_sector * RBPOWER_FLASH_SECTOR_SIZE),
             (unsigned long)(current_sector * RBPOWER_FLASH_SECTOR_SIZE));
    ESP_LOGI(TAG, "  record_in_sector=%u, record_size=%u, record_offset=0x%08lX (%lu)",
             record_in_sector, buffer->record_size,
             (unsigned long)(record_in_sector * buffer->record_size),
             (unsigned long)(record_in_sector * buffer->record_size));
    ESP_LOGI(TAG, "  Final flash_address=0x%08lX (%lu)", (unsigned long)flash_address, (unsigned long)flash_address);
    ESP_LOGI(TAG, "Writing record: unit=0x%04X, period=%d, type=%d, size=%u",
             unit_id, period, data_type, data_size);

    // Debug: dump the first bytes of the record being written (commented out for production)
    // const uint8_t* data_bytes = (const uint8_t*)data;
    // ESP_LOGD(TAG, "  Write bytes: [0-3]=%02X %02X %02X %02X, [24-27]=%02X %02X %02X %02X",
    //          data_bytes[0], data_bytes[1], data_bytes[2], data_bytes[3],
    //          data_bytes[24], data_bytes[25], data_bytes[26], data_bytes[27]);

    // Write the data to flash via the esp_flash API
    esp_flash_t* flash_handle = get_external_flash_handle();
    if (!flash_handle) {
        ESP_LOGE(TAG, "Flash handle is NULL");
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    ret = esp_flash_write(flash_handle, data, flash_address, data_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write to flash: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGD(TAG, "Record written successfully: sector=%d, index=%d", current_sector, record_in_sector);

    // Update the cache after a successful write (the next record goes one slot further)
    update_buffer_cache(buffer, current_sector, record_in_sector + 1, buffer->cached_sequence_number);

    // Update cached_newest_timestamp from the record just written
    // Every record structure starts with a uint32_t timestamp!
    uint32_t ts_first;   /* data may point at a packed (align-1) struct: no direct 32-bit load */
    memcpy(&ts_first, data, sizeof(ts_first));
    buffer->cached_newest_timestamp = ts_first;
    ESP_LOGD(TAG, "Updated cached_newest_timestamp=%lu", buffer->cached_newest_timestamp);

cleanup:
    flash_unlock_write();
    return ret;
}

/* Expose the ring-buffer write cursor for diagnostics (shows the exact cursor position
 * directly). Returns the active sector + next-record index, the total sector
 * count, and the newest record timestamp. The cursor is read from the in-RAM cache if valid,
 * else reconstructed by SCANNING the flash (find_active_sector by max sequence + find_record_index
 * by first-blank-0xFF) — the same scan used at boot, so this also shows what recovery would find. */
esp_err_t rbpower_flash_get_cursor(rbpower_unit_id_t unit_id,
                                   rbpower_period_t period,
                                   rbpower_data_type_t data_type,
                                   uint16_t* sector_out,
                                   uint16_t* next_index_out,
                                   uint16_t* total_sectors_out,
                                   uint32_t* newest_ts_out)
{
    rbpower_buffer_runtime_t* buffer = find_buffer_mutable(unit_id, period, data_type);
    if (!buffer) {
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t sector = 0, idx = 0;
    if (!get_cached_buffer_state(buffer, &sector, &idx)) {
        // Cache miss → must scan flash (find_active_sector/find_record_index do raw esp_flash_read).
        // Take the read lock (defers to any pending writer — write priority) so the scan doesn't
        // race a concurrent sector write.
        if (!flash_lock_read(pdMS_TO_TICKS(1000))) {
            return ESP_ERR_TIMEOUT;
        }
        sector = find_active_sector(buffer);
        idx = (sector != SECTOR_NOT_FOUND) ? find_record_index(buffer, sector) : 0;
        flash_unlock_read();
    }
    // An empty ring (nothing written yet) reads back as SECTOR_NOT_FOUND (0xFFFF) — report it as
    // sector 0 (where the first write will go) so the cursor display is sane, not 65535.
    if (sector == SECTOR_NOT_FOUND) sector = 0;
    if (sector_out)        *sector_out = sector;
    if (next_index_out)    *next_index_out = idx;
    if (total_sectors_out) *total_sectors_out = buffer->total_sectors;
    if (newest_ts_out)     *newest_ts_out = buffer->cached_newest_timestamp;
    return ESP_OK;
}

/* Number of records ACTUALLY written in a (unit, period, type) ring — NOT the capacity. The
 * history API needs this so it never reports/serializes the unwritten 0xFF tail of the ring as
 * records (observed: limit=30 on a 19-record ring returned 11 garbage 0xFF slots, and
 * total_available was the 720 capacity). Derived from the cursor: non-wrapped → active_sector*rps
 * + next_index; wrapped (max sector sequence advanced past total_sectors → all sectors recycled) →
 * the ring is full = total_sectors*rps (capped at logical_capacity). */
esp_err_t rbpower_flash_get_record_count(rbpower_unit_id_t unit_id, rbpower_period_t period,
                                         rbpower_data_type_t data_type, uint32_t* count_out)
{
    if (!count_out) return ESP_ERR_INVALID_ARG;
    rbpower_buffer_runtime_t* buffer = find_buffer_mutable(unit_id, period, data_type);
    if (!buffer) return ESP_ERR_NOT_FOUND;
    *count_out = 0;
    if (buffer->record_size == 0) return ESP_OK;

    uint16_t rps = SECTOR_DATA_SIZE / buffer->record_size;

    // find_active_sector/find_record_index/get_max_sequence_number all do raw esp_flash_read. Hold
    // the read lock (write-priority) across the whole scan so the count doesn't race a concurrent
    // write. get_max_sequence_number scans even on a cache hit, so lock either way.
    if (!flash_lock_read(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    uint16_t active_sector = 0, next_index = 0;
    if (!get_cached_buffer_state(buffer, &active_sector, &next_index)) {
        active_sector = find_active_sector(buffer);
        next_index = (active_sector != SECTOR_NOT_FOUND) ? find_record_index(buffer, active_sector) : 0;
    }
    if (active_sector == SECTOR_NOT_FOUND) { flash_unlock_read(); return ESP_OK; }   // empty ring

    // PHYSICAL wrap detection (was: `get_max_sequence_number > total_sectors`, a seq heuristic that
    // climbed spuriously across erase/rewrite cycles → false "wrapped" → count overshot into uninit
    // sectors → read_records served 0xFFFFFFFF slots that HTTP skipped → returned=0 despite valid
    // records). The ring is WRAPPED iff the sector that would be overwritten next
    // (the oldest = (active+1)%total) is already populated; otherwise it is still filling linearly and
    // the count is exactly active_sector*rps + next_index — the same value the cache/cursor and
    // read_record's newest-walk-back assume, keeping count/cursor/read mutually consistent.
    uint32_t count;
    uint16_t oldest_sector = (uint16_t)((active_sector + 1) % buffer->total_sectors);
    if (sector_has_first_record(buffer, oldest_sector)) {
        count = (uint32_t)buffer->total_sectors * rps;     // physically wrapped → full ring
    } else {
        count = (uint32_t)active_sector * rps + next_index; // not wrapped → exact
    }
    flash_unlock_read();
    uint32_t cap = (buffer->logical_capacity > 0) ? buffer->logical_capacity
                                                  : (uint32_t)buffer->total_sectors * rps;
    if (count > cap) count = cap;
    *count_out = count;
    return ESP_OK;
}

/* Erase every occupied flash slot (DROPS ALL HISTORY). Used to migrate the on-flash record format
 * (e.g. the min/max envelope expansion 20→32B): old buffers were created with the old
 * record_size, so after erasing, the next BOOT's init re-creates them at the current size. Reboot
 * after calling this. Maintenance/format-migration tool, not a routine operation. */
esp_err_t rbpower_flash_erase_all_slots(void)
{
    esp_flash_t* flash_handle = get_external_flash_handle();
    if (!flash_handle) return ESP_ERR_INVALID_STATE;
    // BLIND-erase the entire STATS partition region by offset — NO reads, NO dependency on the
    // slot config / is_occupied (which may be empty), NO record parsing. This is the only way to
    // migrate a record-format change safely: reading old-format records with the new reader can
    // hard-fault, so erase purely by address. OTP / config / log partitions
    // are untouched (different regions). Region is 4K-aligned; esp_flash_erase_region walks sectors.
    uint32_t start = PARTITION_STATS_OFFSET;
    // Region size MUST match the ACTIVE media: on INTERNAL
    // media the external manager's state is unrelated (init failed / NOR absent), so size it from
    // the internal stats partition itself; the external path keeps the runtime chip size.
    uint32_t size;
    if (rbmedia_mode() == RBMEDIA_INTERNAL) {
        size = rbmedia_stats_region_size();
    } else {
        external_flash_state_t* fs = external_flash_get_state();
        size = (fs && fs->partitions.stats_size > 0) ? fs->partitions.stats_size : PARTITION_STATS_SIZE;
    }
    if (size == 0) return ESP_ERR_INVALID_SIZE;
    ESP_LOGW(TAG, "erase STATS region 0x%06lX..+0x%06lX (media=%s, blind, no reads)...",
             (unsigned long)start, (unsigned long)size,
             rbmedia_mode() == RBMEDIA_INTERNAL ? "INTERNAL" : "EXTERNAL");
    // Erase in 64KB chunks under the write lock: a single multi-MB erase blocks every flash client
    // for tens of seconds (and starves the stats writer); per-chunk locking keeps the system live
    // and makes a failing offset visible instead of one opaque silent call.
    // NOTE: the init-time auto-migrate caller runs BEFORE g_flash_mutex is created (and before any
    // concurrent flash client exists) — lock per chunk only when the mutex already exists.
    esp_err_t e = ESP_OK;
    const uint32_t chunk = 0x10000;
    for (uint32_t off = 0; off < size && e == ESP_OK; off += chunk) {
        uint32_t len = (size - off < chunk) ? (size - off) : chunk;
        bool locked = false;
        if (g_flash_mutex) {
            if (!flash_lock_write(pdMS_TO_TICKS(5000))) { e = ESP_ERR_TIMEOUT; break; }
            locked = true;
        }
        e = esp_flash_erase_region(flash_handle, start + off, len);
        if (locked) flash_unlock_write();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "erase STATS: failed at +0x%06lX: %s", (unsigned long)off, esp_err_to_name(e));
        }
        /* Real pause between chunks: a 64 KB erase keeps BOTH cores' flash cache disabled for up to ~1-2 s
         * (Winbond W25Q128), and one tick was not enough for CPU0 — on a FRESH board (first boot = full
         * 4 MB wipe) the Wi-Fi task took the gaps, IDLE0 starved → task watchdog → reboot → wipe again:
         * an endless boot loop. 50 ms per chunk = ~3 s over 4 MB. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "erase STATS region: %s — REBOOT to re-create buffers at the current format",
             esp_err_to_name(e));
    return e;
}

/* ================================================================================================
 * HELPER FUNCTIONS - RING BUFFER STATE
 * ================================================================================================ */

/* ================================================================================================
 * PUBLIC API - READ
 * ================================================================================================ */

// TODO: Implement a new optimized read API

esp_err_t rbpower_flash_read_record(rbpower_unit_id_t unit_id,
                                     rbpower_period_t period,
                                     rbpower_data_type_t data_type,
                                     uint32_t record_index,
                                     void* data_out,
                                     size_t data_size)
{
    // === VALIDATION ===
    if (!g_is_initialized || !rbpower_flash_is_available()) {
        // Return "not found" if flash is unavailable (RAM-only mode)
        return ESP_ERR_NOT_FOUND;
    }

    if (!data_out) {
        ESP_LOGE(TAG, "Data output pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // === TAKE THE MUTEX (READ defers to any waiting WRITE — write priority) ===
    if (!flash_lock_read(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to take mutex (read, deferred to writer)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    // === FIND THE BUFFER ===
    const rbpower_buffer_runtime_t* buffer = find_buffer(unit_id, period, data_type);
    if (!buffer) {
        ESP_LOGE(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d", unit_id, period, data_type);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // === CHECK THE SIZE ===
    if (data_size != buffer->record_size) {
        ESP_LOGE(TAG, "Data size mismatch: expected=%u, got=%u",
                 (unsigned)buffer->record_size, (unsigned)data_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    // === CHECK THAT THE CACHE IS VALID ===
    if (!buffer->cache_valid) {
        ESP_LOGE(TAG, "Cache is not valid! Buffer not initialized or no writes yet");
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    // === COMPUTE PARAMETERS ===
    uint16_t records_per_sector = SECTOR_DATA_SIZE / buffer->record_size;
    uint16_t newest_sector = buffer->cached_active_sector;
    uint16_t next_record_index = buffer->cached_next_record_index;

    // Check: are there any records?
    if (next_record_index == 0 && newest_sector == 0) {
        ESP_LOGW(TAG, "Buffer is empty (no records written yet)");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // Check: the requested index must not exceed the buffer's logical capacity
    if (record_index >= buffer->logical_capacity) {
        ESP_LOGW(TAG, "Record index %lu exceeds logical capacity %lu",
                 (unsigned long)record_index, (unsigned long)buffer->logical_capacity);
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    // The last written record (newest)
    int32_t newest_record_in_sector = next_record_index - 1;

    // If next_record_index == 0, the newest record is in the previous sector
    if (newest_record_in_sector < 0) {
        if (newest_sector == 0) {
            newest_sector = buffer->total_sectors - 1;  // Wrap to last sector
        } else {
            newest_sector--;
        }
        newest_record_in_sector = records_per_sector - 1;
    }

    ESP_LOGD(TAG, "Buffer state: newest_sector=%u, next_record_idx=%u, newest_record_in_sector=%ld, records_per_sector=%u",
             (unsigned)buffer->cached_active_sector, (unsigned)buffer->cached_next_record_index,
             (long)newest_record_in_sector, (unsigned)records_per_sector);

    // === DIRECT COMPUTATION OF THE PHYSICAL LOCATION ===
    // Start at the newest record and step back record_index positions
    int32_t target_sector = newest_sector;
    int32_t target_record = newest_record_in_sector - record_index;

    // Handle crossing sector boundaries (moving backwards)
    while (target_record < 0) {
        // Step to the previous sector
        target_sector--;
        if (target_sector < 0) {
            target_sector = buffer->total_sectors - 1;  // Wrap to last sector
        }
        target_record += records_per_sector;
    }

    // === COMPUTE THE FLASH ADDRESS ===
    uint32_t flash_address = buffer->flash_offset +
                             (target_sector * RBPOWER_FLASH_SECTOR_SIZE) +
                             (target_record * buffer->record_size);

    ESP_LOGD(TAG, "Reading: index_from_newest=%lu → sector=%ld, record=%ld, addr=0x%08lX",
             (unsigned long)record_index, (long)target_sector, (long)target_record,
             (unsigned long)flash_address);

    // === READ THE DATA ===
    esp_flash_t* flash_handle = get_external_flash_handle();
    if (!flash_handle) {
        ESP_LOGE(TAG, "Flash handle is NULL");
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    ret = esp_flash_read(flash_handle, data_out, flash_address, data_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read from flash: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGD(TAG, "✓ Record read successfully");

cleanup:
    flash_unlock_read();
    return ret;
}

esp_err_t rbpower_flash_read_records(rbpower_unit_id_t unit_id,
                                      rbpower_period_t period,
                                      rbpower_data_type_t data_type,
                                      uint32_t start_index,
                                      uint32_t count,
                                      void* data_out,
                                      uint32_t* records_read_out)
{
    // === VALIDATION ===
    if (!g_is_initialized || !rbpower_flash_is_available()) {
        // Report 0 records if flash is unavailable (RAM-only mode)
        if (records_read_out) {
            *records_read_out = 0;
        }
        return ESP_ERR_NOT_FOUND;
    }

    if (!data_out) {
        ESP_LOGE(TAG, "Data output pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (count == 0) {
        if (records_read_out) {
            *records_read_out = 0;
        }
        return ESP_OK;
    }

    // === FIND THE BUFFER ===
    const rbpower_buffer_runtime_t* buffer = find_buffer(unit_id, period, data_type);
    if (!buffer) {
        ESP_LOGE(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d",
                 unit_id, period, data_type);
        if (records_read_out) {
            *records_read_out = 0;
        }
        return ESP_ERR_NOT_FOUND;
    }

    // === READ THE RANGE ===
    uint8_t* output_ptr = (uint8_t*)data_out;
    uint32_t records_read = 0;

    ESP_LOGD(TAG, "Reading range: start=%lu, count=%lu, record_size=%u",
             (unsigned long)start_index, (unsigned long)count,
             (unsigned)buffer->record_size);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t current_index = start_index + i;

        // Read one record
        esp_err_t ret = rbpower_flash_read_record(unit_id, period, data_type,
                                                    current_index,
                                                    output_ptr,
                                                    buffer->record_size);

        if (ret != ESP_OK) {
            // Stop reading on error
            ESP_LOGW(TAG, "Failed to read record at index %lu: %s",
                     (unsigned long)current_index, esp_err_to_name(ret));
            break;
        }

        // Advance the pointer to the next record
        output_ptr += buffer->record_size;
        records_read++;

        // Reset the watchdog every 10 records (avoids a timeout on large ranges)
        if ((i + 1) % 10 == 0) {
            esp_task_wdt_reset();
        }
    }

    // === RESULT ===
    if (records_read_out) {
        *records_read_out = records_read;
    }

    ESP_LOGD(TAG, "Read %lu / %lu records",
             (unsigned long)records_read, (unsigned long)count);

    // Success if at least one record was read
    return (records_read > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ================================================================================================
 * READ TESTS (TEMPORARY)
 * ================================================================================================ */

/*
 * Test helper: logs the newest `count` records with their values.
 * Used to debug the physical-location computation.
 */
esp_err_t rbpower_flash_test_read_recent(rbpower_unit_id_t unit_id,
                                          rbpower_period_t period,
                                          rbpower_data_type_t data_type,
                                          uint32_t count)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  TEST READ RECENT: unit=0x%04X, period=%d, type=%d, count=%lu",
             unit_id, period, data_type, (unsigned long)count);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════════════");

    esp_err_t ret;

    // Choose the output format by data type
    if (data_type == RBPOWER_DATA_TYPE_ELECTRICAL) {
        rbpower_electrical_transfer_t record;

        ESP_LOGI(TAG, "Index | Timestamp  | Voltage  | Current  | Power    | PF    | Freq   | THD V/I");
        ESP_LOGI(TAG, "───────────────────────────────────────────────────────────────────────────────");

        for (uint32_t i = 0; i < count; i++) {
            ret = rbpower_flash_read_record(unit_id, period, data_type, i, &record, sizeof(record));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "[%3lu] Failed to read: %s", (unsigned long)i, esp_err_to_name(ret));
                continue;
            }

            // Convert the data
            float voltage = record.voltage_rms_v10 / 10.0f;
            float current = record.current_rms_ma / 1000.0f;
            float power = record.power_active_w10 / 10.0f;
            float pf = record.power_factor_100 / 100.0f;
            float freq = record.frequency_hz10;
            float thd_v = record.thd_voltage_100 / 100.0f;
            float thd_i = record.thd_current_100 / 100.0f;

            ESP_LOGI(TAG, "[%3lu] | %10lu | %6.1fV | %6.3fA | %7.1fW | %5.2f | %5.1fHz | %4.1f/%4.1f%%",
                     (unsigned long)i,
                     (unsigned long)record.timestamp,
                     voltage, current, power, pf, freq, thd_v, thd_i);
        }

    } else if (data_type == RBPOWER_DATA_TYPE_ENERGY) {
        rbpower_energy_transfer_t record;

        ESP_LOGI(TAG, "Index | Timestamp  | Consumed | Supplied | Net      ");
        ESP_LOGI(TAG, "────────────────────────────────────────────────────────");

        for (uint32_t i = 0; i < count; i++) {
            ret = rbpower_flash_read_record(unit_id, period, data_type, i, &record, sizeof(record));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "[%3lu] Failed to read: %s", (unsigned long)i, esp_err_to_name(ret));
                continue;
            }

            float consumed = record.energy_consumed_wh;
            float supplied = record.energy_supplied_wh;
            float net = record.energy_net_wh;

            ESP_LOGI(TAG, "[%3lu] | %10lu | %8.0f | %8.0f | %8.0f",
                     (unsigned long)i,
                     (unsigned long)record.timestamp,
                     consumed, supplied, net);
        }
    }

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "");

    return ESP_OK;
}

/*
 * Test helper: logs the record range [start_index, start_index + count - 1]
 * using rbpower_flash_read_records().
 * Demonstrates use of a caller-allocated buffer.
 */
esp_err_t rbpower_flash_test_read_batch(rbpower_unit_id_t unit_id,
                                         rbpower_period_t period,
                                         rbpower_data_type_t data_type,
                                         uint32_t start_index,
                                         uint32_t count)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  TEST READ BATCH: unit=0x%04X, period=%d, type=%d",
             unit_id, period, data_type);
    ESP_LOGI(TAG, "  Range: [%lu .. %lu], count=%lu",
             (unsigned long)start_index,
             (unsigned long)(start_index + count - 1),
             (unsigned long)count);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════════════");

    esp_err_t ret;

    // Choose the output format by data type
    if (data_type == RBPOWER_DATA_TYPE_ELECTRICAL) {
        // Allocate the batch-read buffer (caller allocates!)
        rbpower_electrical_transfer_t* records = (rbpower_electrical_transfer_t*)
            malloc(count * sizeof(rbpower_electrical_transfer_t));

        if (!records) {
            ESP_LOGE(TAG, "Failed to allocate buffer for %lu records", (unsigned long)count);
            return ESP_ERR_NO_MEM;
        }

        // Batch read
        uint32_t records_read = 0;
        ret = rbpower_flash_read_records(unit_id, period, data_type,
                                          start_index, count,
                                          records, &records_read);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "rbpower_flash_read_records failed: %s", esp_err_to_name(ret));
            free(records);
            return ret;
        }

        ESP_LOGI(TAG, "Successfully read %lu records", (unsigned long)records_read);
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Index | Timestamp  | Voltage  | Current  | Power    | PF    | Freq   | THD V/I");
        ESP_LOGI(TAG, "───────────────────────────────────────────────────────────────────────────────");

        for (uint32_t i = 0; i < records_read; i++) {
            rbpower_electrical_transfer_t* rec = &records[i];

            // Convert the data
            float voltage = rec->voltage_rms_v10 / 10.0f;
            float current = rec->current_rms_ma / 1000.0f;
            float power = rec->power_active_w10 / 10.0f;
            float pf = rec->power_factor_100 / 100.0f;
            float freq = rec->frequency_hz10;
            float thd_v = rec->thd_voltage_100 / 100.0f;
            float thd_i = rec->thd_current_100 / 100.0f;

            ESP_LOGI(TAG, "[%3lu] | %10lu | %6.1fV | %6.3fA | %7.1fW | %5.2f | %5.1fHz | %4.1f/%4.1f%%",
                     (unsigned long)(start_index + i),
                     (unsigned long)rec->timestamp,
                     voltage, current, power, pf, freq, thd_v, thd_i);
        }

        free(records);

    } else if (data_type == RBPOWER_DATA_TYPE_ENERGY) {
        // Allocate the buffer for energy records
        rbpower_energy_transfer_t* records = (rbpower_energy_transfer_t*)
            malloc(count * sizeof(rbpower_energy_transfer_t));

        if (!records) {
            ESP_LOGE(TAG, "Failed to allocate buffer for %lu records", (unsigned long)count);
            return ESP_ERR_NO_MEM;
        }

        // Batch read
        uint32_t records_read = 0;
        ret = rbpower_flash_read_records(unit_id, period, data_type,
                                          start_index, count,
                                          records, &records_read);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "rbpower_flash_read_records failed: %s", esp_err_to_name(ret));
            free(records);
            return ret;
        }

        ESP_LOGI(TAG, "Successfully read %lu records", (unsigned long)records_read);
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Index | Timestamp  | Consumed | Supplied | Net      ");
        ESP_LOGI(TAG, "────────────────────────────────────────────────────────");

        for (uint32_t i = 0; i < records_read; i++) {
            rbpower_energy_transfer_t* rec = &records[i];

            float consumed = rec->energy_consumed_wh;
            float supplied = rec->energy_supplied_wh;
            float net = rec->energy_net_wh;

            ESP_LOGI(TAG, "[%3lu] | %10lu | %8.0f | %8.0f | %8.0f",
                     (unsigned long)(start_index + i),
                     (unsigned long)rec->timestamp,
                     consumed, supplied, net);
        }

        free(records);
    }

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "");

    return ESP_OK;
}

/* ================================================================================================
 * DIAGNOSTICS AND MAINTENANCE
 * ================================================================================================ */

/**
 * @brief Validate a sector's metadata
 * @return true if the sector is valid
 */
static bool validate_sector_metadata(const rbpower_buffer_runtime_t* buffer, uint16_t sector_index)
{
    esp_flash_t* flash = get_external_flash_handle();
    uint32_t meta_offset = buffer->flash_offset +
                          (sector_index * RBPOWER_FLASH_SECTOR_SIZE) +
                          SECTOR_METADATA_OFFSET;

    sector_metadata_t meta;
    esp_err_t ret = esp_flash_read(flash, &meta, meta_offset, sizeof(meta));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sector %d: failed to read metadata", sector_index);
        return false;
    }

    // Check 1: uninitialized sector (virgin flash)
    if (meta.init_magic != SECTOR_INIT_MAGIC) {
        // This is normal - the sector has not been used yet
        return true;
    }

    // Check 2: flags must be either ACTIVE (0xFF) or CLOSED (0x00)
    if (meta.flags != SECTOR_FLAG_ACTIVE && meta.flags != SECTOR_FLAG_CLOSED) {
        ESP_LOGW(TAG, "Sector %d: invalid flags 0x%02X", sector_index, meta.flags);
        return false;
    }

    // Check 3: for a closed sector, verify the data CRC
    /* crc16 0 = closed by older firmware that could not program the CRC, 0xFFFF = close did not land: no CRC */
    if (meta.flags == SECTOR_FLAG_CLOSED && meta.crc16 != 0 && meta.crc16 != 0xFFFF) {
        uint16_t calculated_crc = calculate_sector_crc(buffer, sector_index);
        if (calculated_crc != meta.crc16) {
            ESP_LOGW(TAG, "Sector %d: CRC mismatch (expected 0x%04X, got 0x%04X)",
                     sector_index, meta.crc16, calculated_crc);
            return false;
        }
    }

    return true;
}

esp_err_t rbpower_flash_diagnose_buffer(rbpower_unit_id_t unit_id,
                                         rbpower_period_t period,
                                         rbpower_data_type_t data_type)
{
    if (!g_is_initialized || !rbpower_flash_is_available()) {
        ESP_LOGD(TAG, "Flash operations not available (RAM-only mode)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const rbpower_buffer_runtime_t* buffer = find_buffer(unit_id, period, data_type);
    if (!buffer) {
        ESP_LOGE(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d",
                 unit_id, period, data_type);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t active_count = 0;
    uint8_t closed_count = 0;
    uint8_t virgin_count = 0;
    uint8_t corrupted_count = 0;
    uint32_t max_seq = 0;
    uint16_t active_sector_with_max_seq = SECTOR_NOT_FOUND;

    esp_flash_t* flash = get_external_flash_handle();

    // Scan all sectors
    for (uint16_t i = 0; i < buffer->total_sectors; i++) {
        uint32_t meta_offset = buffer->flash_offset +
                              (i * RBPOWER_FLASH_SECTOR_SIZE) +
                              SECTOR_METADATA_OFFSET;

        sector_metadata_t meta;
        esp_err_t ret = esp_flash_read(flash, &meta, meta_offset, sizeof(meta));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "  Sector %d: read failed", i);
            corrupted_count++;
            continue;
        }

        // Check initialization
        if (meta.init_magic != SECTOR_INIT_MAGIC) {
            virgin_count++;
#if CONFIG_RBPOWER_VERBOSE_DEBUG
            ESP_LOGD(TAG, "  Sector %d: VIRGIN", i);
#endif
            continue;
        }

        // Check flags
        if (sector_is_active(&meta)) {
            active_count++;
#if CONFIG_RBPOWER_VERBOSE_DEBUG
            ESP_LOGD(TAG, "  Sector %d: ACTIVE, seq=%lu", i, meta.sequence_number);
#endif
            if (meta.sequence_number > max_seq) {
                max_seq = meta.sequence_number;
                active_sector_with_max_seq = i;
            }
        } else if (sector_is_closed(&meta)) {
            closed_count++;
#if CONFIG_RBPOWER_VERBOSE_DEBUG
            ESP_LOGD(TAG, "  Sector %d: CLOSED, seq=%lu, crc=0x%04X",
                     i, meta.sequence_number, meta.crc16);
#endif
        } else {
            ESP_LOGW(TAG, "  Sector %d: INVALID FLAGS 0x%02X", i, meta.flags);
            corrupted_count++;
        }

        // Validation
        if (!validate_sector_metadata(buffer, i)) {
            corrupted_count++;
        }
    }

    // CONSOLIDATED DIAGNOSTIC OUTPUT (7 logs → 1-2 logs)
    bool has_issues = false;
    const char* status_icon = "✓";
    const char* status_msg = "healthy";

    // Check anomalies
    if (active_count == 0 && closed_count > 0) {
        has_issues = true;
        status_icon = "⚠";
        status_msg = "no ACTIVE sector";
    } else if (active_count > 1) {
        has_issues = true;
        status_icon = "⚠";
        status_msg = "multiple ACTIVE sectors";
    } else if (corrupted_count > 0) {
        has_issues = true;
        status_icon = "✗";
        status_msg = "corrupted sectors detected";
    }

    // Single consolidated log with all diagnostic info
    ESP_LOGI(TAG, "Buffer diag [u=0x%04X p=%d t=%d]: %s %s | A=%d C=%d V=%d X=%d%s",
             unit_id, period, data_type, status_icon, status_msg,
             active_count, closed_count, virgin_count, corrupted_count,
             active_sector_with_max_seq != SECTOR_NOT_FOUND ?
             (has_issues ? "" : "") : "");

    if (has_issues) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t rbpower_flash_repair_buffer(rbpower_unit_id_t unit_id,
                                       rbpower_period_t period,
                                       rbpower_data_type_t data_type)
{
    if (!g_is_initialized || !rbpower_flash_is_available()) {
        ESP_LOGD(TAG, "Flash operations not available (RAM-only mode)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    rbpower_buffer_runtime_t* buffer = find_buffer_mutable(unit_id, period, data_type);
    if (!buffer) {
        ESP_LOGE(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d",
                 unit_id, period, data_type);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Repairing buffer: unit=0x%04X, period=%d, type=%d",
             unit_id, period, data_type);

    esp_flash_t* flash = get_external_flash_handle();

    // Step 1: find all ACTIVE sectors
    uint16_t active_sectors[buffer->total_sectors];
    uint32_t active_seq[buffer->total_sectors];
    uint8_t active_count = 0;

    for (uint16_t i = 0; i < buffer->total_sectors; i++) {
        uint32_t meta_offset = buffer->flash_offset +
                              (i * RBPOWER_FLASH_SECTOR_SIZE) +
                              SECTOR_METADATA_OFFSET;

        sector_metadata_t meta;
        esp_err_t ret = esp_flash_read(flash, &meta, meta_offset, sizeof(meta));
        if (ret != ESP_OK || meta.init_magic != SECTOR_INIT_MAGIC) {
            continue;
        }

        if (sector_is_active(&meta)) {
            active_sectors[active_count] = i;
            active_seq[active_count] = meta.sequence_number;
            active_count++;
        }
    }

    // Step 2: if several are ACTIVE - close all but the one with the max seq
    if (active_count > 1) {
        ESP_LOGW(TAG, "Found %d ACTIVE sectors - closing old ones", active_count);

        // Find the sector with the highest sequence_number
        uint8_t max_seq_index = 0;
        for (uint8_t i = 1; i < active_count; i++) {
            if (active_seq[i] > active_seq[max_seq_index]) {
                max_seq_index = i;
            }
        }

        uint16_t keep_sector = active_sectors[max_seq_index];
        ESP_LOGD(TAG, "  Keeping sector %d (seq=%lu) as ACTIVE",
                 keep_sector, active_seq[max_seq_index]);

        // Close the others
        for (uint8_t i = 0; i < active_count; i++) {
            if (i == max_seq_index) continue;

            uint16_t sector_to_close = active_sectors[i];
            ESP_LOGD(TAG, "  Closing sector %d (seq=%lu)",
                     sector_to_close, active_seq[i]);

            esp_err_t ret = close_sector(buffer, sector_to_close);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "  ✗ Failed to close sector %d", sector_to_close);
                return ret;
            }
        }

        ESP_LOGD(TAG, "✓ Repaired: closed %d old ACTIVE sectors", active_count - 1);
    }

    // Step 3: invalidate the cache to force a rescan
    invalidate_buffer_cache(buffer);

    ESP_LOGI(TAG, "✓ Buffer repair completed");
    return ESP_OK;
}

/* ================================================================================================
 * BUFFER INITIALIZATION FROM THE CONFIGURATION
 * ================================================================================================ */

esp_err_t rbpower_flash_init_buffers_from_network(void)
{
#if CONFIG_RBPOWER_VERBOSE_DEBUG
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  FLASH BUFFER INITIALIZATION FROM THE NETWORK              ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
#endif

    // 1. Check flash availability via the global context
    external_flash_state_t* flash_state = external_flash_get_state();
    if (!flash_state || !flash_state->status.initialized || !flash_state->status.filesystem_mounted) {
        ESP_LOGW(TAG, "⚠ External Flash not available (initialized=%d, fs_mounted=%d)",
                 flash_state ? flash_state->status.initialized : 0,
                 flash_state ? flash_state->status.filesystem_mounted : 0);
        ESP_LOGI(TAG, "Statistics will continue in RAM-only mode (no persistence)");

        // Set the initialized flag to false so that all operations are skipped
        g_is_initialized = false;
        return ESP_OK;  // Report success so the system is not blocked
    }

    ESP_LOGD(TAG, "✓ External Flash available (initialized, filesystem mounted)");

   // 2. Check that the home network is initialized
    extern rbpower_electrical_network_t g_home_network;

    if (g_home_network.units_count == 0) {
        ESP_LOGW(TAG, "⚠ Home network has no units");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Network units count: %d", g_home_network.units_count);

    // Get the configuration matrices
    extern const uint8_t RBPOWER_BUFFER_SECTORS[RBPOWER_PERIOD_MAX][RBPOWER_DATA_TYPE_MAX];
    extern const uint32_t RBPOWER_RECORDS_COUNT[RBPOWER_PERIOD_MAX];

    uint16_t total_buffers = 0;
    uint16_t initialized_buffers = 0;
    uint8_t processed_units = 0;

    // 3. Iterate over all active units
    for (uint8_t unit_idx = 0; unit_idx < g_home_network.units_count; unit_idx++) {
        rbpower_home_unit_t* unit = &g_home_network.units[unit_idx];

#if CONFIG_RBPOWER_VERBOSE_DEBUG
        ESP_LOGI(TAG, "Checking unit[%d]: id=0x%04X, valid=%d, active=%d, type=%d",
                 unit_idx,
                 unit->definition.unit_id,
                 unit->has_valid_definition,
                 unit->definition.is_active,
                 unit->definition.type);
#endif

        // Filter 1: check validity and activity
        if (!unit->has_valid_definition || !unit->definition.is_active) {
            ESP_LOGD(TAG, "  ⊗ Unit[%d]: not valid or not active, skipping", unit_idx);
            continue;
        }

        // Filter 2: exclude VOLTAGE_BUS (needs no flash buffers)
        if (unit->definition.type == RBPOWER_METRIC_VOLTAGE_BUS) {
            ESP_LOGD(TAG, "  ⊗ Unit[%d]: VOLTAGE_BUS type, skipping Flash buffers", unit_idx);
            continue;
        }

        // Note: has_statistics_enabled is set after rbpower_start_accumulation_system(),
        // but that does not matter for slot init - slots are created for all units (except VOLTAGE_BUS)

        rbpower_unit_id_t uid = unit->definition.unit_id;
        rbpower_load_role_t role = unit->definition.role;

#if CONFIG_RBPOWER_VERBOSE_DEBUG
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "─────────────────────────────────────────────────────────");
        ESP_LOGI(TAG, "Unit[%d]: id=0x%04X, role=%d, name='%s'",
                 unit_idx, uid, role, unit->definition.name);
        ESP_LOGI(TAG, "─────────────────────────────────────────────────────────");
#endif

        processed_units++;

        // 4. SLOT CHECK: is the unit already placed?
        int slot_idx = find_slot_for_unit(uid);

        if (slot_idx < 0) {
            // Unit not placed - find a free slot
            slot_idx = find_first_free_slot();

            if (slot_idx < 0) {
                ESP_LOGE(TAG, "❌ No free slots available for unit 0x%04X!", uid);
                ESP_LOGE(TAG, "   Maximum %d units supported, slot limit reached", RBPOWER_MAX_UNITS);
                continue;
            }

            // Place the unit into the free slot
            esp_err_t ret = allocate_slot_for_unit(uid, slot_idx);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "❌ Failed to allocate slot for unit 0x%04X: %s",
                         uid, esp_err_to_name(ret));
                continue;
            }

            ESP_LOGD(TAG, "✓ Unit 0x%04X allocated to slot %d", uid, slot_idx);
        } else {
            ESP_LOGD(TAG, "✓ Unit 0x%04X already in slot %d", uid, slot_idx);
        }

        // Get the slot base offset
        uint32_t slot_base_offset = g_flash_slots[slot_idx].base_offset;
        uint32_t buffer_offset = slot_base_offset;  // Running offset inside the slot

        ESP_LOGD(TAG, "Slot %d base offset: 0x%06lX", slot_idx, (unsigned long)slot_base_offset);

        uint8_t unit_buffers = 0;

        // 5. Add the unit to the runtime manager
        esp_err_t ret = rbpower_runtime_add_unit(&g_runtime_manager, uid,
                                                   unit->definition.name,
                                                   role);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "❌ Failed to add unit to runtime: %s", esp_err_to_name(ret));
            continue;
        }

        // 6. Initialize buffers per the RBPOWER_BUFFER_SECTORS matrix
        for (uint8_t period = RBPOWER_PERIOD_HOUR; period < RBPOWER_PERIOD_MAX; period++) {
            for (uint8_t data_type = 0; data_type < RBPOWER_DATA_TYPE_MAX; data_type++) {

                // Check: is a buffer needed for this combination?
                uint8_t sectors = RBPOWER_BUFFER_SECTORS[period][data_type];
                if (sectors == 0) {
                    // Skip: no buffer is used for this combination
                    continue;
                }

                total_buffers++;
                unit_buffers++;

                // Compute the buffer parameters
                uint16_t record_size = rbpower_get_transfer_size((rbpower_data_type_t)data_type);
                uint16_t records_per_sector = RBPOWER_FLASH_SECTOR_SIZE / record_size;
                uint32_t buffer_size_bytes = sectors * RBPOWER_FLASH_SECTOR_SIZE;
                uint32_t physical_capacity = records_per_sector * sectors;
                uint32_t logical_capacity = RBPOWER_RECORDS_COUNT[period];

                // Build the buffer descriptor
                rbpower_buffer_runtime_t buffer;
                buffer.unit_id = uid;
                buffer.period = (rbpower_period_t)period;
                buffer.data_type = (rbpower_data_type_t)data_type;
                buffer.total_sectors = sectors;
                buffer.record_size = record_size;
                buffer.flash_offset = buffer_offset;
                buffer.buffer_size_bytes = buffer_size_bytes;
                buffer.physical_capacity = physical_capacity;
                buffer.logical_capacity = logical_capacity;

                // Add the buffer to the runtime manager
                ret = rbpower_runtime_add_buffer(&g_runtime_manager, &buffer);
                if (ret == ESP_OK) {
                    initialized_buffers++;

                    ESP_LOGD(TAG, "  ✓ Buffer[%d]: period=%d, type=%d, sectors=%d, offset=0x%06lX, size=%lu B",
                             unit_buffers - 1, period, data_type, sectors,
                             (unsigned long)buffer_offset,
                             (unsigned long)buffer_size_bytes);

                    // Advance the offset inside the slot
                    buffer_offset += buffer_size_bytes;

                    // NOTE: diagnostics and cache initialization run AFTER the
                    // optional flash clear (if CLEAR_BUFFERS_ON_INIT == 1).
                    // See the code after the initialization summary.
                } else {
                    ESP_LOGE(TAG, "  ❌ Failed to add buffer (period=%d, type=%d): %s",
                             period, data_type, esp_err_to_name(ret));
                }
            }
        }

        uint32_t unit_flash_used = buffer_offset - slot_base_offset;
        ESP_LOGD(TAG, "Unit[%d]: initialized %d buffers, Flash used in slot: %lu KB",
                 unit_idx, unit_buffers, (unsigned long)(unit_flash_used / 1024));
    }

#if CONFIG_RBPOWER_VERBOSE_DEBUG
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  INITIALIZATION RESULTS                                    ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
#endif
    ESP_LOGI(TAG, "Flash buffers init: Processed units: %d, Total buffers: %d (initialized: %d)",
             processed_units, total_buffers, initialized_buffers);
#if CONFIG_RBPOWER_VERBOSE_DEBUG
    ESP_LOGI(TAG, "Allocated slots:     %d / %d", processed_units, RBPOWER_MAX_UNITS);
    ESP_LOGI(TAG, "");
#endif

    // Log the final slot state
#if CONFIG_RBPOWER_VERBOSE_DEBUG
    print_flash_slots_info();
#endif

    if (initialized_buffers == total_buffers) {
        ESP_LOGD(TAG, "✅ All buffers initialized successfully");

        // DEBUG: clear all buffers to test from scratch
        // Controlled by #define CLEAR_BUFFERS_ON_INIT at the top of this file
        #if CLEAR_BUFFERS_ON_INIT
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "⚠️  DEBUG MODE: Clearing all allocated buffers for clean test");
        ESP_LOGW(TAG, "");

        esp_flash_t* flash_handle = get_external_flash_handle();
        if (flash_handle) {
            uint32_t total_erased = 0;
            uint32_t slot_size_bytes = RBPOWER_SLOT_SIZE_KB * 1024;
            uint32_t erased_slots = 0;

            // Erase ONLY allocated (occupied) slots
            for (int slot = 0; slot < RBPOWER_MAX_UNITS; slot++) {
                // Skip free slots
                if (!g_flash_slots[slot].is_occupied) {
                    continue;
                }

                uint32_t slot_offset = g_flash_slots[slot].base_offset;
                ESP_LOGI(TAG, "  Erasing slot %d (Unit 0x%04X) at offset 0x%06lX (%lu KB)...",
                         slot, g_flash_slots[slot].unit_id,
                         (unsigned long)slot_offset, (unsigned long)RBPOWER_SLOT_SIZE_KB);

                // Erase the slot sector by sector (RBPOWER_SLOT_SIZE_KB / 4 KB sectors)
                uint32_t sectors_in_slot = RBPOWER_SLOT_SIZE_KB * 1024 / RBPOWER_FLASH_SECTOR_SIZE;
                bool slot_ok = true;

                for (uint32_t sec = 0; sec < sectors_in_slot; sec++) {
                    uint32_t sector_addr = slot_offset + (sec * RBPOWER_FLASH_SECTOR_SIZE);
                    esp_err_t erase_ret = esp_flash_erase_region(flash_handle,
                                                                  sector_addr,
                                                                  RBPOWER_FLASH_SECTOR_SIZE);
                    if (erase_ret != ESP_OK) {
                        ESP_LOGE(TAG, "  ✗ Failed to erase sector %lu at 0x%06lX: %s",
                                 (unsigned long)sec, (unsigned long)sector_addr,
                                 esp_err_to_name(erase_ret));
                        slot_ok = false;
                        break;
                    }
                }

                if (slot_ok) {
                    total_erased += slot_size_bytes;
                    erased_slots++;
                    ESP_LOGI(TAG, "  ✓ Slot %d erased (%lu sectors)", slot, (unsigned long)sectors_in_slot);
                }
            }
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✅ Cleared %lu slots, %lu KB total", (unsigned long)erased_slots, (unsigned long)(total_erased / 1024));
            ESP_LOGI(TAG, "");
        } else {
            ESP_LOGE(TAG, "❌ Failed to get Flash handle for clearing");
        }
        #endif

        // ========================================================================
        // BUFFER DIAGNOSTICS AND CACHE INITIALIZATION
        // Runs AFTER the flash clear (if it was enabled)
        // ========================================================================
        ESP_LOGD(TAG, "Running buffer diagnostics and cache initialization...");
        uint16_t diagnosed_buffers = 0;
        uint16_t repaired_buffers = 0;
        uint16_t cached_buffers = 0;

        for (uint16_t i = 0; i < g_runtime_manager.buffer_count; i++) {
            rbpower_buffer_runtime_t* buf = &g_runtime_manager.buffers[i];

            // Buffer diagnostics (automatic repair)
            esp_err_t diag_ret = rbpower_flash_diagnose_buffer(buf->unit_id, buf->period, buf->data_type);
            if (diag_ret == ESP_ERR_INVALID_STATE) {
#if CONFIG_RBPOWER_VERBOSE_DEBUG
                ESP_LOGI(TAG, "  Buffer[%u] needs repair (unit=0x%04X, period=%d, type=%d)",
                         i, buf->unit_id, buf->period, buf->data_type);
#endif
                rbpower_flash_repair_buffer(buf->unit_id, buf->period, buf->data_type);
                repaired_buffers++;
            } else if (diag_ret == ESP_OK) {
                diagnosed_buffers++;
            }

            // Pre-initialize the cache (including cached_newest_timestamp)
            // Done ONCE at system start to avoid a delay on the first HTTP request
            if (!buf->cache_valid) {
                uint16_t dummy_sector, dummy_index;
                // get_cached_buffer_state() reports a miss; the cache is then initialized via update_buffer_cache()
                if (!get_cached_buffer_state(buf, &dummy_sector, &dummy_index)) {
                    // Cache invalid - initialize it
                    uint16_t active = find_active_sector(buf);
                    if (active != SECTOR_NOT_FOUND) {
                        uint16_t rec_idx = find_record_index(buf, active);

                        // Read the metadata for sequence_number
                        sector_metadata_t meta;
                        esp_flash_t* flash = get_external_flash_handle();
                        uint32_t meta_addr = buf->flash_offset +
                                            (active * RBPOWER_FLASH_SECTOR_SIZE) +
                                            SECTOR_METADATA_OFFSET;
                        esp_flash_read(flash, &meta, meta_addr, sizeof(meta));

                        // update_buffer_cache() initializes cached_newest_timestamp automatically
                        update_buffer_cache(buf, active, rec_idx, meta.sequence_number);
                        cached_buffers++;
#if CONFIG_RBPOWER_VERBOSE_DEBUG
                        ESP_LOGI(TAG, "  ✓ Buffer[%u] cache initialized (unit=0x%04X, period=%d, newest_ts=%lu)",
                                 i, buf->unit_id, buf->period, buf->cached_newest_timestamp);
#endif
                    }
                }
            }
        }

        ESP_LOGD(TAG, "✓ Diagnostics: %u OK, %u repaired", diagnosed_buffers, repaired_buffers);
        ESP_LOGD(TAG, "✓ Cache initialized: %u buffers", cached_buffers);

        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "❌ Some buffers failed to initialize (%d/%d)",
                 total_buffers - initialized_buffers, total_buffers);
        return ESP_FAIL;
    }
}

/* ================================================================================================
 * PUBLIC API - BUFFER INFO
 * ================================================================================================ */

esp_err_t rbpower_flash_get_buffer_capacity(rbpower_unit_id_t unit_id,
                                             rbpower_period_t period,
                                             rbpower_data_type_t data_type,
                                             uint32_t* capacity)
{
    if (!capacity) {
        ESP_LOGE(TAG, "capacity pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_is_initialized || !rbpower_flash_is_available()) {
        *capacity = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }

    const rbpower_buffer_runtime_t* buffer = find_buffer(unit_id, period, data_type);
    if (!buffer) {
        ESP_LOGW(TAG, "Buffer not found: unit=0x%04X, period=%d, type=%d",
                 unit_id, period, data_type);
        *capacity = 0;
        return ESP_ERR_NOT_FOUND;
    }

    *capacity = buffer->logical_capacity;

    ESP_LOGD(TAG, "Buffer capacity: unit=0x%04X, period=%d, type=%d, capacity=%lu",
             unit_id, period, data_type, (unsigned long)*capacity);

    return ESP_OK;
}
