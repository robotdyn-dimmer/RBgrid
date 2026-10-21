cmake_minimum_required(VERSION 3.16)

# Source files. Measurement comes only from rbAmp I2C modules (main/rbamp_bridge.c) and
# ESP-NOW nodes; the hub has no analog measurement path of its own.
set(RBPOWER_SRCS
    "src/rbpower.c"
    "src/rbsensors.c"
    "src/rbhome.c"
    "src/rbtime.c"
    "src/rbhome_statistics.c"
    "src/rbhome_timers.c"
    "src/rbhome_voltage_bus.c"
    "src/rbhome_main_supply.c"
    "src/rbhome_alt_supply.c"
    "src/rbgrid.c"
    "src/rbgrid_config_api.c"
    "src/rbgrid_config_storage.c"
    "src/rbjson.c"
    "src/rbtelemetry.c"
    "src/rbmqtt.c"
    "src/rbwifi.c"
    "src/rbwebserver.c"
    "src/rbwebserver_ota.c"
    "src/rbwebserver_wifi.c"
    "src/rbwebserver_api.c"
    "src/rbwebserver_ui.c"                   # embedded web-UI hosting (storage littlefs)
    "src/rbwebsocket.c"
    "src/rbwebsocket_telemetry.c"
    "src/rbpower_inject_test.c"
    "src/rbpower_logic_test.c"
    "src/rbpower_lifetime_accumulators.c"   # lifetime energy accumulators
    "src/rbstats_v4_convert.c"              # pure v3->v4 record converters (phase-0 T2.1; unit-tested)
    "src/rbpoint.c"                         # T7 point-slug utils + caps mapping (pure; unit-tested)
    "src/otp_manager.c"                      # flash-chip OTP validation (CONFIG partition too)
    "src/external_flash_manager.c"           # SPI NOR chip driver (CONFIG partition too)
    "src/rbstorage_media.c"                  # internal-first media routing (stats partition shims)
    "src/littlefs_raw.c"                     # raw littlefs IO for config/token/registry
    "src/rbhome_tariff.c"                    # tariff/economics/billing engine (also #include'd by rbpower.c)
    # --- flash statistics data layer (history/summary/DAY+ aggregation persistence) ---
    "src/rbstorage.c"
    "src/rbstorage_transfer_types.c"
    "src/rbstorage_config.c"
    "src/rbstorage_structures.c"
    "src/rbstorage_init.c"
    "src/rbstorage_write.c"
    "src/rbstorage_ringbuffer.c"
    "src/rbextflash_buffer_write.c"
    "src/rbextflash_config.c"
    "src/rbextflash_config_runtime.c"
    "src/rbextflash_operations.c"
    "src/rbstats_migrate.c"                  # deferred breaking-format migration engine (phase-0 T1)
    "src/rbstats_v4_read.c"                  # v4-facing record reader shim (phase-0 T2.4a)
    "src/rbstats_migrate_v3v4.c"             # v3->v4 region migrator (phase-0 T2.4c)
    "src/otp_external_flash_test.c"
    "src/rbgrid_period_stats_api.c"          # history / DAY+ on-demand aggregation API
)

# Component registration
idf_component_register(
    SRCS ${RBPOWER_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/joltwallet__littlefs/src"
    REQUIRES
        driver
        esp_common
        log
        nvs_flash
        freertos
        esp_timer
        json
        esp_wifi
        esp_netif
        esp_http_server
        esp_https_server
        esp_event
        mqtt
        esp_partition
        app_update
        mbedtls
        spi_flash
        wear_levelling
        joltwallet__littlefs
    PRIV_REQUIRES
        heap
        soc
        hal
        rbamp        # rbamp_sensor_models.h — CT model registry for the schema / ct_code check
    EMBED_TXTFILES
        "certs/rbgrid_dev_cert.pem"
        "certs/rbgrid_dev_key.pem"
)

# LittleFS configuration define
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LFS_CONFIG=lfs_config.h
)
