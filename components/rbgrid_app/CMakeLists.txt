# bench_kit is a REQUIRES always (its no-CONFIG variant is zero-footprint, rbbench precedent);
# the code gate is CONFIG_RBGRID_BENCH_KIT from sdkconfig.bench → RBGRID_BENCH_KIT define below.
set(main_srcs "rbgrid_app.cpp" "rbamp_bridge.c" "esp_now_hub.c" "rbevlog.c" "rbi2c_cmd.c")
if(CONFIG_RBGRID_IF_HTTP)
    list(APPEND main_srcs "modfw.c" "rbi2c_api.c")   # HTTP APIs: module firmware update, I2C buses
endif()
idf_component_register(SRCS ${main_srcs}
                    INCLUDE_DIRS "."
                    REQUIRES rbpower
                            esp_psram   # the firmware requires PSRAM; with MINIMAL_BUILD, IDF 5.5 drops it unless asked
                            console
                            bench_kit
                            nvs_flash
                            esp_wifi
                            esp_event
                            esp_netif
                            esp_http_server
                            rbamp
                            esp_driver_i2c
                            rbbench
                            mbedtls
                            joltwallet__littlefs

)

if(CONFIG_RBGRID_BENCH_KIT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBGRID_BENCH_KIT=1)
endif()

# Panic forensics (2026-10-17): rbpower wraps esp_panic_handler to keep the crash PC/backtrace in RTC memory
# for the next boot's event log (console is NONE on bench/OTA-only hubs, no coredump partition).
if(CONFIG_RBGRID_PANIC_FORENSICS)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
endif()
