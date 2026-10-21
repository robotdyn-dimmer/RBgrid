/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file external_flash_manager.c
 * @brief Consolidated external Flash management
 */

#include "external_flash_config.h"
#include "otp_manager.h"
#include "littlefs_raw.h"  // Use raw LittleFS instead
#include "rbstorage_media.h"   // internal-first media routing
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "FLASH_MGR";

/** @brief One entry of the fixed partition table of the storage medium. */
typedef struct {
    const char* name;    ///< Partition name ("partition_table", "nvs", "config", "log", "stats")
    uint32_t offset;     ///< Offset in the external-layout address space, bytes
    uint32_t size;       ///< Size, bytes
    bool is_filesystem;  ///< true for LittleFS, false for raw
    bool mounted;        ///< Filesystem mounted on this partition
    bool available;      ///< Partition verified and usable
} flash_partition_info_t;

// Global partition table - using values from partition_layout.h
// These partitions are pre-configured in production, we only verify them
static flash_partition_info_t g_partitions[] = {
    {
        .name = "partition_table",
        .offset = PARTITION_TABLE_OFFSET,    // 0x000000 (4KB)
        .size = PARTITION_TABLE_SIZE,
        .is_filesystem = false,
        .mounted = false,
        .available = false
    },
    {
        .name = "nvs",
        .offset = PARTITION_NVS_OFFSET,      // 0x001000 (16KB), to be verified
        .size = PARTITION_NVS_SIZE,
        .is_filesystem = false,
        .mounted = false,
        .available = false
    },
    {
        .name = "config",
        .offset = PARTITION_CONFIG_OFFSET,    // 0x005000 (256KB)
        .size = PARTITION_CONFIG_SIZE,
        .is_filesystem = true,  // LittleFS
        .mounted = false,
        .available = false
    },
    // (The former external "ota" partition was removed and its space given to STATS; see partition_layout.h.)
    {
        .name = "log",
        .offset = PARTITION_LOG_OFFSET,       // 0x045000 (128KB) process/diagnostic log
        .size = PARTITION_LOG_SIZE,
        .is_filesystem = true,  // Could be LittleFS or raw
        .mounted = false,
        .available = false
    },
    {
        .name = "stats",
        .offset = PARTITION_STATS_OFFSET,     // 0x065000 → end of chip; .size set at runtime from flash_size
        .size = PARTITION_STATS_SIZE,         // 4MB fallback; overridden in external_flash_init()
        .is_filesystem = true,  // Could be LittleFS or raw
        .mounted = false,
        .available = false
    }
};

/** @brief Number of entries in the partition table. */
#define NUM_PARTITIONS (sizeof(g_partitions)/sizeof(g_partitions[0]))

// Global Flash state - single source of truth
static external_flash_state_t g_flash_state = {
    .pins = {
        .miso_pin = DEFAULT_FLASH_MISO_PIN,
        .mosi_pin = DEFAULT_FLASH_MOSI_PIN,
        .sclk_pin = DEFAULT_FLASH_SCLK_PIN,
        .cs_pin = DEFAULT_FLASH_CS_PIN
    },
    .status = {
        .initialized = false,
        .otp_validated = false,
        .filesystem_mounted = false,
        .device_serial = 0,
        .flash_id = 0,
        .flash_size = 0,
        .error_message = NULL
    },
    .flash_handle = NULL,
    .partitions = {0}
};

// External functions from otp_external_flash_test.c (documented in external_flash_config.h)
/** @brief Initialise the external SPI NOR flash device (see external_flash_config.h).
 *  @return ESP_OK on success, an ESP-IDF error code otherwise. */
extern esp_err_t init_external_flash(void);
/** @brief Release the external SPI NOR flash device. */
extern void deinit_external_flash(void);
/** @brief Handle of the external SPI NOR flash device.
 *  @return The device handle, or NULL when it is not initialised. */
extern esp_flash_t* get_external_flash_handle(void);

/**
 * @brief Check if partition table exists at the beginning of Flash
 */
static esp_err_t verify_partition_table(void)
{
    if (!g_flash_state.flash_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Read first 4 bytes to check for partition table magic
    uint32_t magic = 0;
    esp_err_t ret = esp_flash_read(g_flash_state.flash_handle, 
                                   &magic, 
                                   PARTITION_TABLE_OFFSET, 
                                   sizeof(magic));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read partition table magic: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (magic == PARTITION_TABLE_MAGIC) {
        ESP_LOGD(TAG, "Partition table found (magic: 0x%08lx)", (unsigned long)magic);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Partition table not found or invalid (read: 0x%08lx, expected: 0x%08lx)",
                 (unsigned long)magic, (unsigned long)PARTITION_TABLE_MAGIC);
        // Continue anyway - partitions might still be usable
        return ESP_OK;
    }
}

/**
 * @brief Verify partition boundaries and availability
 */
static esp_err_t verify_partition(flash_partition_info_t* partition)
{
    if (!partition || !g_flash_state.flash_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Verifying partition '%s' at 0x%08lx, size: %lu KB",
             partition->name, (unsigned long)partition->offset,
             (unsigned long)(partition->size / 1024));
    
    // Check partition boundaries
    if (partition->offset + partition->size > g_flash_state.status.flash_size) {
        ESP_LOGE(TAG, "Partition '%s' exceeds flash size!", partition->name);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Skip partition table verification - it's special
    if (strcmp(partition->name, "partition_table") == 0) {
        partition->available = true;
        return ESP_OK;
    }
    
    // Test read access
    uint8_t test_buf[256];
    esp_err_t ret = esp_flash_read(g_flash_state.flash_handle, 
                                   test_buf, 
                                   partition->offset, 
                                   sizeof(test_buf));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read partition '%s': %s", 
                 partition->name, esp_err_to_name(ret));
        return ret;
    }
    
    partition->available = true;
    ESP_LOGD(TAG, "Partition '%s' verified OK", partition->name);
    return ESP_OK;
}

/**
 * @brief Verify all partitions
 */
static esp_err_t verify_all_partitions(void)
{
    ESP_LOGD(TAG, "Verifying Flash partitions...");

    int available_count = 0;
    for (int i = 0; i < NUM_PARTITIONS; i++) {
        esp_err_t ret = verify_partition(&g_partitions[i]);
        if (ret == ESP_OK) {
            available_count++;
        }
    }

    ESP_LOGD(TAG, "Partitions verified: %d/%d available",
             available_count, NUM_PARTITIONS);

    return (available_count > 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Mount LittleFS on config partition
 */
static esp_err_t mount_config_partition(void)
{
    flash_partition_info_t* config_part = NULL;
    
    // Find config partition
    for (int i = 0; i < NUM_PARTITIONS; i++) {
        if (strcmp(g_partitions[i].name, "config") == 0) {
            config_part = &g_partitions[i];
            break;
        }
    }
    
    if (!config_part || !config_part->available) {
        ESP_LOGE(TAG, "Config partition not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    if (!g_flash_state.flash_handle) {
        ESP_LOGE(TAG, "Flash handle not available");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGD(TAG, "Mounting RAW LittleFS on EXTERNAL config partition at offset 0x%08lx, size: %lu KB",
             (unsigned long)config_part->offset,
             (unsigned long)(config_part->size / 1024));
    
    // IMPORTANT: Use raw LittleFS to ensure we're using external Flash
    esp_err_t ret = littlefs_raw_mount(g_flash_state.flash_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount raw LittleFS on external config partition: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Mark as mounted
    config_part->mounted = true;
    g_flash_state.status.filesystem_mounted = true;

    ESP_LOGD(TAG, "Raw LittleFS mounted successfully on EXTERNAL Flash config partition");
    return ESP_OK;
}

/**
 * @brief Create initial JSON config files structure
 */
static esp_err_t create_config_file_structure(void)
{
    ESP_LOGD(TAG, "Creating config file structure on EXTERNAL Flash...");

    // Create backup directory first
    esp_err_t ret = littlefs_raw_mkdir("/backup");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backup directory may already exist");
    }
    
    // Create single compact config file
    const char* config_path = "/config.json";
    if (!littlefs_raw_exists(config_path)) {
        // Create default compact config with all sections
        const char* default_config = 
            "{"
            "\"system\":{\"version\":\"1.0.0\",\"device_id\":\"ESP32\",\"initialized\":false,"
                       "\"home_name\":\"Default\",\"location\":\"Unknown\",\"home_enabled\":false},"
            "\"hardware\":{\"channels\":2},"
            "\"network\":{\"wifi_ssid\":\"\",\"ap_enabled\":false},"
            "\"telemetry\":{\"interval\":60,\"enabled\":false},"
            "\"tariffs\":{\"zones\":[],\"active\":false},"
            "\"sensors\":{\"count\":0},"
            "\"units\":{\"power\":\"kW\",\"energy\":\"kWh\"}"
            "}";
        
        ret = littlefs_raw_write(config_path, default_config, strlen(default_config));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create %s", config_path);
        } else {
            ESP_LOGD(TAG, "Created compact config file: %s (%zu bytes)",
                     config_path, strlen(default_config));
        }
    } else {
        ESP_LOGD(TAG, "Compact config file already exists: %s", config_path);
    }

    ESP_LOGD(TAG, "Config file structure created successfully on EXTERNAL Flash");
    return ESP_OK;
}

/* Initialize external Flash subsystem with OTP validation (documented in external_flash_config.h). */
esp_err_t external_flash_init(void)
{
    /* === INTERNAL FIRST ===
     * The internal 16MB flash is the PRIMARY store: raw `stats` partition for the statistics
     * rings (rbmedia shims translate the external-layout offsets) + `config` partition for the
     * littlefs config/metadata files. The external NOR is an OPTIONAL extension, to be re-enabled
     * later via rbmedia_attach_external(); its probe/OTP path below is kept intact and serves as
     * a FALLBACK on devices still running the old partition table (no `stats` partition). */
    if (rbmedia_init_internal() == ESP_OK) {
        esp_err_t lfs = littlefs_raw_mount_internal();
        if (lfs != ESP_OK) {
            ESP_LOGE(TAG, "internal littlefs mount failed: %s", esp_err_to_name(lfs));
            g_flash_state.status.error_message = "internal config littlefs mount failed";
            return lfs;
        }
        g_flash_state.flash_handle = NULL;            /* no external chip in use */
        g_flash_state.status.flash_id = 0;            /* 0 = internal media */
        esp_flash_get_size(esp_flash_default_chip, &g_flash_state.status.flash_size);
        g_flash_state.status.otp_validated = true;    /* authenticity gate is N/A on internal */
        g_flash_state.status.filesystem_mounted = true;
        g_flash_state.partitions.stats_size = rbmedia_stats_region_size();
        {
            const esp_partition_t *cfg = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "config");
            g_flash_state.partitions.config_size = cfg ? cfg->size : 0;
        }
        /* Keep the legacy g_partitions entries coherent: "stats" for info surfaces, and "config"
         * MUST be marked mounted — every rbgrid_config_storage save/LOAD path gates on
         * external_flash_is_config_mounted(). With this flag unset, config saves fail (HTTP 500)
         * and boot-time config loads silently fall back to defaults. */
        for (int i = 0; i < NUM_PARTITIONS; i++) {
            if (strcmp(g_partitions[i].name, "stats") == 0) {
                g_partitions[i].size = g_flash_state.partitions.stats_size;
                g_partitions[i].available = true;
            } else if (strcmp(g_partitions[i].name, "config") == 0) {
                g_partitions[i].size = g_flash_state.partitions.config_size;
                g_partitions[i].available = true;
                g_partitions[i].mounted = true;
            }
        }
        if (g_flash_state.status.filesystem_mounted) {
            create_config_file_structure();   /* same bootstrap as the external path */
        }
        g_flash_state.status.initialized = true;
        ESP_LOGI(TAG, "storage media = INTERNAL flash (stats %lu KB, config %lu KB); "
                      "external NOR optional/disabled",
                 (unsigned long)(g_flash_state.partitions.stats_size / 1024),
                 (unsigned long)(g_flash_state.partitions.config_size / 1024));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "internal `stats` partition absent (old layout) — falling back to the "
                  "EXTERNAL NOR path");

    ESP_LOGD(TAG, "Initializing external Flash subsystem...");

    // Step 1: Initialize Flash hardware
    esp_err_t ret = init_external_flash();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Flash hardware: %s", esp_err_to_name(ret));
        g_flash_state.status.error_message = "Flash hardware init failed";
        return ret;
    }
    
    g_flash_state.flash_handle = get_external_flash_handle();
    
    // Get Flash info
    if (g_flash_state.flash_handle) {
        esp_flash_get_size(g_flash_state.flash_handle, &g_flash_state.status.flash_size);
        esp_flash_read_id(g_flash_state.flash_handle, &g_flash_state.status.flash_id);
        ESP_LOGD(TAG, "Flash detected: ID=0x%08lx, Size=%lu MB",
                 (unsigned long)g_flash_state.status.flash_id,
                 (unsigned long)(g_flash_state.status.flash_size / (1024*1024)));

        // STATS extends to the END OF THE DETECTED CHIP: one firmware serves the 4 MB
        // test chip and a 16 MB production chip. Override the static fallback size in the partition
        // table so verify/bounds-checks and the slot allocator see the real region.
        if (g_flash_state.status.flash_size > PARTITION_STATS_OFFSET) {
            uint32_t stats_size = g_flash_state.status.flash_size - PARTITION_STATS_OFFSET;
            for (int i = 0; i < NUM_PARTITIONS; i++) {
                if (strcmp(g_partitions[i].name, "stats") == 0) {
                    g_partitions[i].size = stats_size;
                    break;
                }
            }
            ESP_LOGI(TAG, "STATS region sized to 0x%06lX (%lu KB) from 0x%06X to chip end",
                     (unsigned long)stats_size, (unsigned long)(stats_size / 1024), PARTITION_STATS_OFFSET);
        }
    }

    // Step 2: Validate OTP
    ESP_LOGD(TAG, "Reading OTP for device identification...");
    if (!otp_verify_authenticity()) {
        ESP_LOGE(TAG, "OTP validation would fail in production mode");
        g_flash_state.status.otp_validated = false;
        g_flash_state.status.error_message = "OTP validation failed";
        // In production, return error here
        // return ESP_ERR_INVALID_STATE;
    } else {
        g_flash_state.status.otp_validated = true;
    }
    
    // Get serial number regardless of validation status (for testing)
    g_flash_state.status.device_serial = otp_get_serial_number();
    if (g_flash_state.status.device_serial != 0) {
        ESP_LOGD(TAG, "Device serial from OTP: 0x%08lx",
                 (unsigned long)g_flash_state.status.device_serial);
    }

    // Step 3: Re-initialize Flash after OTP (important!)
    ESP_LOGD(TAG, "Re-initializing Flash after OTP operations...");
    deinit_external_flash();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    ret = init_external_flash();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-init Flash after OTP: %s", esp_err_to_name(ret));
        g_flash_state.status.error_message = "Flash re-init after OTP failed";
        return ret;
    }
    
    g_flash_state.flash_handle = get_external_flash_handle();
    
    // Step 4: Verify Flash is working
    if (g_flash_state.flash_handle) {
        uint32_t flash_id = 0;
        ret = esp_flash_read_id(g_flash_state.flash_handle, &flash_id);
        if (ret != ESP_OK || flash_id == 0) {
            ESP_LOGE(TAG, "Flash not responding after OTP!");
            g_flash_state.status.error_message = "Flash not responding";
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Flash verified after OTP: ID=0x%08lx", (unsigned long)flash_id);
    }

    // Step 5: Check if partition table exists (optional - might not be present)
    ret = verify_partition_table();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Partition table check failed, continuing anyway");
    }
    
    // Step 6: Verify all partitions
    ret = verify_all_partitions();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Partition verification failed");
        // Continue anyway for development
    }
    
    // Step 6: Mount config partition
    ret = mount_config_partition();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount config partition");
        // Continue anyway for development
    }
    
    // Step 7: Create JSON config files structure
    if (g_flash_state.status.filesystem_mounted) {
        ret = create_config_file_structure();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create config file structure");
        }
    }
    
    // Update global state with partition info and mark all partitions as available
    for (int i = 0; i < NUM_PARTITIONS; i++) {
        // Mark all partitions as available after successful Flash init
        g_partitions[i].available = true;

        if (strcmp(g_partitions[i].name, "config") == 0) {
            g_flash_state.partitions.config_size = g_partitions[i].size;
            g_flash_state.partitions.config_used = 0; // TODO: Calculate actual usage
        } else if (strcmp(g_partitions[i].name, "log") == 0) {
            g_flash_state.partitions.log_size = g_partitions[i].size;
            g_flash_state.partitions.log_used = 0;
        } else if (strcmp(g_partitions[i].name, "stats") == 0) {
            g_flash_state.partitions.stats_size = g_partitions[i].size;
            g_flash_state.partitions.stats_used = 0;
        }
    }
    
    g_flash_state.status.initialized = true;
    /* Legacy external path succeeded → route the stats media shims to the external chip. */
    rbmedia_attach_external(g_flash_state.flash_handle);
    ESP_LOGI(TAG, "External Flash subsystem initialized successfully");

    return ESP_OK;
}

/* Deinitialize external Flash subsystem (documented in external_flash_config.h). */
esp_err_t external_flash_deinit(void)
{
    ESP_LOGD(TAG, "Deinitializing external Flash subsystem...");

    // Unmount filesystem if mounted
    if (g_flash_state.status.filesystem_mounted) {
        esp_err_t ret = littlefs_raw_unmount();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to unmount raw LittleFS: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGD(TAG, "Raw LittleFS unmounted successfully");
        }
        g_flash_state.status.filesystem_mounted = false;
        
        // Mark config partition as not mounted
        for (int i = 0; i < NUM_PARTITIONS; i++) {
            if (strcmp(g_partitions[i].name, "config") == 0) {
                g_partitions[i].mounted = false;
                break;
            }
        }
    }
    
    // Deinitialize Flash hardware
    deinit_external_flash();
    
    g_flash_state.flash_handle = NULL;
    g_flash_state.status.initialized = false;

    ESP_LOGD(TAG, "External Flash subsystem deinitialized");
    return ESP_OK;
}

/* Get external Flash state (documented in external_flash_config.h). */
external_flash_state_t* external_flash_get_state(void)
{
    return &g_flash_state;
}

/* Check if external Flash is available (documented in external_flash_config.h). */
bool external_flash_is_available(void)
{
    return g_flash_state.status.initialized && 
           g_flash_state.status.otp_validated;
}

/* Get device serial number from OTP (documented in external_flash_config.h). */
uint32_t external_flash_get_serial(void)
{
    return g_flash_state.status.device_serial;
}

/* Check if specific partition is available (documented in external_flash_config.h). */
bool external_flash_is_partition_available(const char* partition_name)
{
    if (!partition_name) return false;
    
    for (int i = 0; i < NUM_PARTITIONS; i++) {
        if (strcmp(g_partitions[i].name, partition_name) == 0) {
            return g_partitions[i].available;
        }
    }
    return false;
}

/* Get partition info (documented in external_flash_config.h). */
bool external_flash_get_partition_info(const char* partition_name, 
                                       uint32_t* offset, 
                                       uint32_t* size)
{
    if (!partition_name) return false;
    
    for (int i = 0; i < NUM_PARTITIONS; i++) {
        if (strcmp(g_partitions[i].name, partition_name) == 0) {
            if (offset) *offset = g_partitions[i].offset;
            if (size) *size = g_partitions[i].size;
            return g_partitions[i].available;
        }
    }
    return false;
}

/* Get config partition mount point (documented in external_flash_config.h). */
const char* external_flash_get_config_mount_point(void)
{
    return g_flash_state.status.filesystem_mounted ? "/config" : NULL;
}

/* Check if config partition is mounted (documented in external_flash_config.h). */
bool external_flash_is_config_mounted(void)
{
    for (int i = 0; i < NUM_PARTITIONS; i++) {
        if (strcmp(g_partitions[i].name, "config") == 0) {
            return g_partitions[i].mounted;
        }
    }
    return false;
}