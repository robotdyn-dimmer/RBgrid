/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower.c
 * @brief rbpower library core: lifecycle (init/start/stop), extended bring-up and external-flash status.
 *
 * This file is also the amalgamation unit of the home-network engine: rbhome.c, rbtime.c,
 * rbhome_timers.c, rbhome_statistics.c and rbhome_tariff.c are included here as source files and compiled as
 * one translation unit, so their static state is shared.
 */

#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbpower.h"
#include "rbsensors.h"
#include "rbhome.c"
#include "rbtime.c"
#include "rbhome_timers.c"
#include "rbhome_statistics.c"
#include "rbhome_tariff.c"   // tariff/economics engine (amalgamated into this translation unit)
#include "rbstorage.h"
#include "rbstorage_types.h"
#include "rbextflash_operations.h"  // Flash buffer initialization
#include "otp_manager.h"
#include "partition_layout.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_flash.h"

// External Flash functions from otp_external_flash_test.c
esp_err_t init_external_flash(void);          ///< Initialise the external flash chip (otp_external_flash_test.c). @return ESP_OK or an ESP-IDF error code
void deinit_external_flash(void);              ///< Release the external flash chip (otp_external_flash_test.c)
esp_flash_t* get_external_flash_handle(void);  ///< esp_flash handle of the external chip (otp_external_flash_test.c). @return Handle, or NULL

//static const char *TAG = "RBPOWER";

/* ================================================================================================
 * EXTERNAL FLASH AND OTP - NOW MANAGED BY external_flash_manager.c
 * ================================================================================================ */

// Use consolidated Flash management
extern esp_err_t external_flash_init(void);                    ///< Initialise the external flash with OTP validation. @return ESP_OK or an ESP-IDF error code
extern esp_err_t external_flash_deinit(void);                  ///< Deinitialise the external flash. @return ESP_OK or an ESP-IDF error code
extern external_flash_state_t* external_flash_get_state(void); ///< Consolidated external-flash state. @return State pointer, or NULL
extern bool external_flash_is_available(void);                 ///< Flash availability. @return true if initialised and OTP-validated
extern bool external_flash_is_config_mounted(void);            ///< Config partition state. @return true if mounted
extern uint32_t external_flash_get_serial(void);               ///< Device serial from OTP. @return Serial, or 0

/* ================================================================================================
 * PRIVATE TYPES - SIMPLIFIED
 * ================================================================================================ */

/**
 * @brief Library context.
 */
typedef struct
{
    /* System state */
    rbpower_state_t state; /**< Current state */
    bool initialized;      /**< Initialization flag */

    /* Configuration */
    rbpower_hardware_config_t config; /**< User configuration copy */

    /* Minimal statistics */
    rbpower_statistics_t statistics; /**< Minimal statistics */

    /* Timestamps */
    uint64_t start_time_us; /**< System start time */

} rbpower_context_t;

/* ================================================================================================
 * PRIVATE VARIABLES
 * ================================================================================================ */

/** Global library context */
static rbpower_context_t g_context = {0};

/** Mutex for thread safety */
static SemaphoreHandle_t g_mutex = NULL;

/* ================================================================================================
 * PRIVATE FUNCTIONS
 * ================================================================================================ */

/**
 * @brief Acquire the context mutex.
 * @param timeout_ms Wait limit, ms.
 * @return true if taken; false on timeout or if the mutex does not exist yet.
 */
static bool rbpower_lock(uint32_t timeout_ms)
{
    if (g_mutex == NULL)
    {
        return false;
    }

    return xSemaphoreTake(g_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/**
 * @brief Release context mutex
 */
static void rbpower_unlock(void)
{
    if (g_mutex != NULL)
    {
        xSemaphoreGive(g_mutex);
    }
}

/**
 * @brief Validate the configuration (mains frequency 50/60 Hz).
 */
static rbpower_err_t rbpower_validate_config(const rbpower_hardware_config_t *config)
{
    if (config == NULL)
    {
        return RBPOWER_ERR_INVALID_ARG;
    }

    if (config->network.nominal_frequency != RBPOWER_FREQ_50HZ &&
        config->network.nominal_frequency != RBPOWER_FREQ_60HZ)
    {
        ESP_LOGE(TAG, "Unsupported network frequency: %d Hz", (int)config->network.nominal_frequency);
        return RBPOWER_ERR_INVALID_ARG;
    }

    return RBPOWER_OK;
}

/**
 * @brief Get current time in microseconds
 */
static uint64_t rbpower_get_time_us(void)
{
    return esp_timer_get_time();
}

/* ================================================================================================
 * PUBLIC API IMPLEMENTATION - BASIC FUNCTIONS ONLY
 * ================================================================================================ */

rbpower_err_t rbpower_init(const rbpower_hardware_config_t *config)
{
    ESP_LOGI(TAG, "=== Initializing rbpower ===");

    /* 1. Initialize storage module (if enabled) */
    bool storage_enabled = false;
    // Check if external flash is available and initialized
    if (config->external_flash.status.initialized) {
        storage_enabled = true;  // Storage is enabled if Flash is initialized
    }

    if (storage_enabled) {
        esp_err_t storage_err = rbpower_storage_init(NULL);
        if (storage_err != ESP_OK) {
            ESP_LOGE(TAG, "Storage initialization error: 0x%x", (unsigned int)storage_err);
            // Critical error - cannot continue without storage
            return RBPOWER_ERR_NOT_INIT;
        }
        ESP_LOGI(TAG, "Storage module initialized");
    } else {
        ESP_LOGD(TAG, "Storage module DISABLED in configuration");
    }

    /* 2. Work with storage (if enabled) */
    if (storage_enabled) {
        const rbstorage_config_t* storage_config = rbpower_storage_get_config();
        if (storage_config == NULL) {
            ESP_LOGE(TAG, "Failed to get storage configuration");
            return RBPOWER_ERR_NOT_INIT;
        }

        /* 3. Check if first boot initialization is needed */
        if (storage_config->first_boot) {
            ESP_LOGI(TAG, "First system boot detected");
            esp_err_t storage_err = rbpower_storage_first_boot_init();
            if (storage_err != ESP_OK) {
                ESP_LOGE(TAG, "First boot initialization error: 0x%x", (unsigned int)storage_err);
                return RBPOWER_ERR_NOT_INIT;
            }
        } else {
            ESP_LOGD(TAG, "Normal boot | Total boots: %lu", (unsigned long)storage_config->total_boots);

            /* 4. For normal boot - mount partitions */
            for (int i = 0; i < RBSTORAGE_PARTITION_COUNT; i++) {
                const rbstorage_partition_state_t* state = rbpower_storage_get_state(i);

            if (!state || !state->is_enabled) {
                continue;
            }

            /* Mount if not yet mounted */
            if (!state->is_mounted) {
                esp_err_t storage_err = rbpower_storage_mount_partition(i);
                if (storage_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to mount partition %d: 0x%x", i, (unsigned int)storage_err);

                    /* Automatic recovery for critical CONFIG partition */
                    if (i == RBSTORAGE_PARTITION_CONFIG) {
                        ESP_LOGW(TAG, "Attempting CONFIG partition recovery");
                        storage_err = rbpower_storage_recover_partition(i);
                        if (storage_err == ESP_OK) {
                            ESP_LOGI(TAG, "CONFIG partition recovered successfully");
                        } else {
                            ESP_LOGE(TAG, "Failed to recover CONFIG partition: 0x%x", (unsigned int)storage_err);
                        }
                    }
                }
            }
            }
        }

        /* CONFIG is not mounted by rbstorage — littlefs_raw owns it (see rbstorage.c) */
    }  // Close if (storage_enabled) block

    /* Arguments validation */
    rbpower_err_t err = rbpower_validate_config(config);
    if (err != RBPOWER_OK)
    {
        return err;
    }

    /* Check for duplicate initialization */
    if (g_context.initialized)
    {
        ESP_LOGD(TAG, "Library already initialized");
        return RBPOWER_ERR_ALREADY_INIT;
    }

    /* Create mutex */
    if (g_mutex == NULL)
    {
        g_mutex = xSemaphoreCreateMutex();
        if (g_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create mutex");
            return RBPOWER_ERR_NO_MEMORY;
        }
    }

    /* Acquire mutex for initialization */
    if (!rbpower_lock(1000))
    {
        return RBPOWER_ERR_TIMEOUT;
    }

    /* Clear context */
    memset(&g_context, 0, sizeof(rbpower_context_t));

    /* Copy configuration */
    memcpy(&g_context.config, config, sizeof(rbpower_hardware_config_t));

    /* Measurement comes only from rbAmp I2C modules and ESP-NOW nodes (inject API); there is no
     * native ESP32-ADC engine. adc.* in the config only describes the legacy unit bindings
     * (e.g. 0006/0007 fed by the rbAmp bridge) — no sampling hardware is started. */
    g_context.config.adc_dma_enabled = false;

    /* Initialize state */
    g_context.state = RBPOWER_STATE_INITIALIZED;
    g_context.initialized = true;
    g_context.start_time_us = rbpower_get_time_us();

    /* Initialize minimal statistics */
    memset(&g_context.statistics, 0, sizeof(rbpower_statistics_t));

    rbpower_unlock();

    ESP_LOGI(TAG, "rbpower initialized");

    return RBPOWER_OK;
}

rbpower_err_t rbpower_init_extended(const rbpower_extended_config_t *extended_config)
{
    if (!extended_config)
    {
        return RBPOWER_ERR_INVALID_ARG;
    }

    ESP_LOGI("RBPOWER-INIT", "=== Initializing extended rbpower ===");

    // 1. First initialize base rbpower
    rbpower_err_t err = rbpower_init(extended_config->base_hardware_config);
    if (err != RBPOWER_OK)
    {
        ESP_LOGE("RBPOWER-INIT", "Failed to initialize rbpower base: %d", err);
        return err;
    }

   // 2. Additional time initialization (if requested in extended)
    if (extended_config->enable_time_synchronization)
    {
        rbtime_config_t *time_config = &extended_config->base_hardware_config->time_config;

        // Check that time was not already initialized in rbpower_init()
        if (!time_config->enable_time_sync)
        {
            ESP_LOGD("RBPOWER-INIT", "Enabling time synchronization via extended config");
            time_config->enable_time_sync = true;

            // Initialize time
            err = rbtime_init(time_config);
            if (err != RBPOWER_OK)
            {
                ESP_LOGE("RBPOWER-INIT", "Failed to initialize time system: %d", err);
                ESP_LOGD("RBPOWER-INIT", "Continuing without time synchronization");
            }
        }
        else
        {
            ESP_LOGD("RBPOWER-INIT", "Time already initialized in base configuration");
        }
    }

    // 2. Initialize statistics (if enabled in extended_config)
    // Initialize statistics immediately, without waiting for time synchronization
    bool time_ready = rbtime_is_time_valid();

    // Get storage flag from configuration
    bool storage_enabled = false;
    // Check if external flash is available and initialized
    if (extended_config->base_hardware_config->external_flash.status.initialized) {
        storage_enabled = true;  // Storage is enabled if Flash is initialized
    }

    ESP_LOGD("RBPOWER", "Initializing statistics system (time synchronized: %s)",
             time_ready ? "YES" : "NO - fallback mode");

    // Initialize statistics system independently from time
        rbpower_statistics_global_config_t stats_config = {
            .statistics_enabled = true,
            .storage_enabled = storage_enabled,  // Pass flag from external_flash_config check
            .statistics_task_priority = tskIDLE_PRIORITY + 3,
            .callback_task_priority = tskIDLE_PRIORITY + 1,
            /* 8 KB: the quarter collect → inject → hour close → flash store → tariff chain runs here (period
             * boundary callback); with 4096 B only ~300 B stayed free after an hour close (`stacks` verb) */
            .statistics_task_stack_size = 8192,
            .callback_task_stack_size = 3072,
            .statistics_task_core = 0,
            .callback_task_core = 0,
            .statistics_queue_size = 32,
            .callback_queue_size = 16,
            .queue_timeout_ms = 1000,
            .enable_timer_auto_sync = true,
            .timer_sync_interval_hours = 6,
            .timer_drift_threshold_ms = 100,
            .enable_timer_error_recovery = true,
            .max_total_memory_kb = 64,
            .enable_memory_monitoring = true,
            .memory_check_interval_ms = 30000
        };

        rbpower_err_t stats_err = rbpower_statistics_init(&stats_config);
        if (stats_err == RBPOWER_OK) {
            ESP_LOGI("RBPOWER", "Statistics system initialized successfully");
            ESP_LOGD("RBPOWER", "Starting statistics timers...");

            // Auto-start timers 1 second after initialization
            vTaskDelay(pdMS_TO_TICKS(1000));
            rbpower_err_t timer_err = rbpower_statistics_timers_start();
            if (timer_err == RBPOWER_OK) {
                ESP_LOGI("RBPOWER", "Statistics timers started (3-second periods active)");
            } else {
                ESP_LOGE("RBPOWER", "Statistics timers start error: %d", timer_err);
            }
            rbpower_statistics_print_diagnostics();

        } else {
            ESP_LOGE("RBPOWER", "Statistics system initialization error: %d", stats_err);
        }

    // 3. Then initialize home network if enabled
    if (extended_config->enable_home_network)
    {
        err = rbpower_home_init(extended_config);
        if (err != RBPOWER_OK)
        {
            ESP_LOGE("RBPOWER-INIT", "Failed to initialize home network: %d", err);
            // Don't stop rbpower on rbhome error
            return err;
        }
        ESP_LOGI("RBPOWER-INIT", "Home network initialized: %s", extended_config->home_name ? extended_config->home_name : "Unnamed");
    }

    // 4. Start accumulation system (sets has_statistics_enabled for units)
    rbpower_start_accumulation_system();

    // 5. Initialize Flash buffers (AFTER rbpower_start_accumulation_system!)
    // Note: using external_flash_is_config_mounted() instead of external_flash_is_available()
    // because external_flash_is_available() requires OTP validation which may not be ready in dev mode
    if (external_flash_is_config_mounted() && storage_enabled && extended_config->enable_home_network) {
        ESP_LOGD("RBPOWER", "Initializing Flash buffers...");

        esp_err_t flash_err = rbpower_flash_init_buffers_from_network();
        if (flash_err == ESP_OK) {
            ESP_LOGI("RBPOWER", "Flash buffers initialized successfully");
        } else {
            ESP_LOGE("RBPOWER", "Flash buffer initialization error: %d", flash_err);
        }
    } else {
        if (!external_flash_is_config_mounted()) {
            ESP_LOGD("RBPOWER", "Flash buffering disabled: Flash config not mounted");
        } else if (!storage_enabled) {
            ESP_LOGD("RBPOWER", "Flash buffering disabled: storage_enabled=false");
        } else if (!extended_config->enable_home_network) {
            ESP_LOGD("RBPOWER", "Flash buffering disabled: home network not enabled");
        }
    }
           // === TARIFF SYSTEM INITIALIZATION ===
            ESP_LOGD("RBPOWER", "Initializing tariff system...");

            esp_err_t tariff_err = rbpower_tariff_system_init();
            if (tariff_err == ESP_OK) {
                ESP_LOGI("RBPOWER", "Tariff system initialized successfully");

                // Check tariff system readiness
                if (rbpower_tariff_system_is_ready()) {
                    ESP_LOGD("RBPOWER", "Tariff system ready");

                } else {
                    ESP_LOGW("RBPOWER", "Tariff system not ready");
                }
            } else {
                ESP_LOGE("RBPOWER", "Tariff system initialization error: %d", tariff_err);
            }

        // === STORAGE FILESYSTEM TESTING ===
        // (storage_enabled is computed above)

        // LEGACY: LittleFS filesystem testing (FULLY DISABLED - using Flash buffers)
        // if (storage_enabled) {
        //     ESP_LOGI("RBPOWER", "=== Testing storage filesystem ===");
        //     esp_err_t debug_err = rbpower_storage_debug_struct_sizes();
        //     if (debug_err == ESP_OK) {
        //         ESP_LOGI("RBPOWER", "Structure size debugging completed");
        //     }
        //     esp_err_t storage_test_err = rbpower_storage_test_filesystem_creation();
        //     if (storage_test_err == ESP_OK) {
        //         ESP_LOGI("RBPOWER", "Filesystem testing PASSED");
        //     } else {
        //         ESP_LOGE("RBPOWER", "Filesystem testing FAILED: 0x%x", storage_test_err);
        //     }
        // }

        if (storage_enabled) {
            ESP_LOGD("RBPOWER", "Statistics saved to Flash buffers (LittleFS filesystem not used)");
        } else {
            ESP_LOGD("RBPOWER", "Storage system DISABLED in configuration");
        }

        // 4. Automatic measurements start (enabled by default)
        // If auto_start_measurements not initialized (0), assume auto-start needed
        bool auto_start = (extended_config->auto_start_measurements == 0) ? true : extended_config->auto_start_measurements;

        if (auto_start) {
            ESP_LOGD("RBPOWER-INIT", "Auto-starting measurement system...");
            rbpower_err_t start_err = rbpower_start();
            if (start_err != RBPOWER_OK) {
                ESP_LOGE("RBPOWER-INIT", "Measurement system start error: %d", start_err);
                ESP_LOGW("RBPOWER-INIT", "System initialized but measurements not started");
                // Don't return error as initialization succeeded
                // User can call rbpower_start() manually later
            } else {
                ESP_LOGI("RBPOWER-INIT", "Measurement system started automatically");
            }
        } else {
            ESP_LOGD("RBPOWER-INIT", "Auto-start disabled, call rbpower_start() manually to begin measurements");
        }
                
        return RBPOWER_OK;
}        

rbpower_err_t rbpower_deinit(void)
{
    if (!g_context.initialized)
    {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbpower_lock(1000))
    {
        return RBPOWER_ERR_TIMEOUT;
    }

    /* Stop if running */
    if (g_context.state == RBPOWER_STATE_RUNNING)
    {
        rbpower_stop();
    }

    /* Clear context */
    g_context.initialized = false;
    g_context.state = RBPOWER_STATE_UNINITIALIZED;

    rbpower_unlock();

    /* Delete mutex */
    if (g_mutex != NULL)
    {
        vSemaphoreDelete(g_mutex);
        g_mutex = NULL;
    }

    ESP_LOGI(TAG, "rbpower deinitialized");
    return RBPOWER_OK;
}

rbpower_err_t rbpower_start(void)
{
    if (!g_context.initialized)
    {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbpower_lock(100))
    {
        return RBPOWER_ERR_TIMEOUT;
    }

    if (g_context.state == RBPOWER_STATE_RUNNING)
    {
        rbpower_unlock();
        return RBPOWER_ERR_ALREADY_STARTED;
    }

    g_context.state = RBPOWER_STATE_RUNNING;
    rbpower_unlock();

    ESP_LOGI(TAG, "rbpower started");
    return RBPOWER_OK;
}

rbpower_err_t rbpower_stop(void)
{
    if (!g_context.initialized)
    {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbpower_lock(100))
    {
        return RBPOWER_ERR_TIMEOUT;
    }

    if (g_context.state != RBPOWER_STATE_RUNNING && g_context.state != RBPOWER_STATE_PAUSED)
    {
        rbpower_unlock();
        return RBPOWER_ERR_NOT_STARTED;
    }

    g_context.state = RBPOWER_STATE_CONFIGURED;
    rbpower_unlock();

    ESP_LOGI(TAG, "rbpower stopped");
    return RBPOWER_OK;
}

rbpower_err_t rbpower_pause(void)
{
    if (!g_context.initialized)
    {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbpower_lock(100))
    {
        return RBPOWER_ERR_TIMEOUT;
    }

    if (g_context.state != RBPOWER_STATE_RUNNING)
    {
        rbpower_unlock();
        return RBPOWER_ERR_INVALID_STATE;
    }

    g_context.state = RBPOWER_STATE_PAUSED;

    rbpower_unlock();
    return RBPOWER_OK;
}

rbpower_err_t rbpower_resume(void)
{
    if (!g_context.initialized)
    {
        return RBPOWER_ERR_NOT_INIT;
    }

    if (!rbpower_lock(100))
    {
        return RBPOWER_ERR_TIMEOUT;
    }

    if (g_context.state != RBPOWER_STATE_PAUSED)
    {
        rbpower_unlock();
        return RBPOWER_ERR_INVALID_STATE;
    }

    g_context.state = RBPOWER_STATE_RUNNING;

    rbpower_unlock();
    return RBPOWER_OK;
}


/* ================================================================================================
 * EXTERNAL FLASH MEMORY AND OTP VALIDATION - IMPLEMENTATION
 * ================================================================================================ */

// External functions already declared above

/**
 * @brief Check Config partition availability on external Flash
 */
static bool verify_config_partition(void) {
    // TODO: Implement proper external Flash partition check
    // For now, just verify Flash chip is accessible
    
    ESP_LOGW("RBPOWER", "Config partition check skipped - external Flash filesystem not implemented yet");
    
    // Try to get external Flash handle to verify it's accessible
    esp_flash_t* ext_flash = get_external_flash_handle();
    if (!ext_flash) {
        ESP_LOGE("RBPOWER", "No handle for external Flash");
        return false;
    }
    
    // Get Flash size
    uint32_t flash_size = 0;
    esp_err_t ret = esp_flash_get_size(ext_flash, &flash_size);
    if (ret != ESP_OK) {
        ESP_LOGE("RBPOWER", "Failed to get Flash size: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Get Flash ID
    uint32_t flash_id = 0;
    ret = esp_flash_read_id(ext_flash, &flash_id);
    if (ret != ESP_OK) {
        ESP_LOGE("RBPOWER", "Failed to read Flash ID: %s", esp_err_to_name(ret));
        return false;
    }

    // Check if it's a valid Winbond chip (0xEF in manufacturer ID)
    uint8_t mfg_id = (flash_id >> 16) & 0xFF;
    if (mfg_id == 0xEF) {
        ESP_LOGD("RBPOWER", "External Flash detected: Size=%" PRIu32 " MB, ID=0x%08" PRIx32 ", Winbond chip confirmed",
                 flash_size / (1024 * 1024), flash_id);
        return true;
    } else if (mfg_id != 0x00 && mfg_id != 0xFF) {
        ESP_LOGD("RBPOWER", "External Flash detected: Size=%" PRIu32 " MB, ID=0x%08" PRIx32 ", Unknown manufacturer: 0x%02X",
                 flash_size / (1024 * 1024), flash_id, mfg_id);
        return true;  // Still allow if we got valid response
    }
    
    ESP_LOGE("RBPOWER", "Invalid Flash response");
    return false;
}

/*
 * Initialize external Flash with OTP validation
 */
esp_err_t rbpower_external_flash_init(void) {
    // Use consolidated Flash manager
    return external_flash_init();
}

/*
 * Check external Flash memory availability
 */
bool rbpower_external_flash_is_available(void) {
    return external_flash_is_available();
}

/*
 * Check OTP validation status
 */
bool rbpower_external_flash_otp_validated(void) {
    external_flash_state_t* state = external_flash_get_state();
    return state ? state->status.otp_validated : false;
}

/*
 * Get device serial number from OTP
 */
uint32_t rbpower_external_flash_get_serial(void) {
    return external_flash_get_serial();
}

/*
 * Get external Flash memory status
 */
rbpower_flash_status_t rbpower_external_flash_get_status(void) {
    rbpower_flash_status_t status = {0};
    external_flash_state_t* state = external_flash_get_state();
    
    if (state) {
        status.external_flash_available = state->status.initialized;
        status.otp_validated = state->status.otp_validated;
        status.device_serial = state->status.device_serial;
        status.config_partition_mounted = state->status.filesystem_mounted;
        status.config_partition_size = state->partitions.config_size;
        status.config_used_space = state->partitions.config_used;
        status.error_message = state->status.error_message;
    }
    
    return status;
}
