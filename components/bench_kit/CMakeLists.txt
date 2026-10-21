# bench_kit — bench-ONLY load control + YP9830 reference (see include/bench_kit.h).
# Gated by CONFIG_RBGRID_BENCH_KIT (sdkconfig.bench sets =y). A CONFIG gate, not a -D flag:
# IDF resolves component REQUIRES in an EARLY cmake pass where user -D cache vars don't exist
# (the rbbench precedent) — sdkconfig values are the only reliable early-pass condition.
# REQUIRES identical in both branches for the same reason.
if(CONFIG_RBGRID_BENCH_KIT)
    idf_component_register(SRCS "bench_kit.c" INCLUDE_DIRS "include"
                           REQUIRES esp_driver_i2c esp_driver_uart esp_driver_gpio esp_timer)
else()
    idf_component_register(INCLUDE_DIRS "include"
                           REQUIRES esp_driver_i2c esp_driver_uart esp_driver_gpio esp_timer)
endif()
