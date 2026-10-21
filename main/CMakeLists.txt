# bench_kit is a REQUIRES always (its no-CONFIG variant is zero-footprint, rbbench precedent);
# the code gate is CONFIG_RBGRID_BENCH_KIT from sdkconfig.bench → RBGRID_BENCH_KIT define below.
idf_component_register(SRCS "main.cpp" "rbamp_bridge.c" "esp_now_hub.c" "rbevlog.c" "modfw.c" "rbi2c_api.c"
                    INCLUDE_DIRS "."
                    REQUIRES rbpower
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
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
