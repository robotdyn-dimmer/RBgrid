# rbbench — development/test-rig debug console (generic half). RBgrid
# gates the build with CONFIG_RBGRID_DEBUG_BRIDGE (bench-only image) or
# CONFIG_RBGRID_BENCH_FULL (variant A: bench console + wifi/telemetry together)
# and adds its own introspection verbs. When neither flag is set, register
# headers only (no sources) so the component still satisfies `REQUIRES rbbench`
# at zero footprint.
if(CONFIG_RBGRID_DEBUG_BRIDGE OR CONFIG_RBGRID_BENCH_FULL)
    idf_component_register(
        SRCS "rbbench.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_i2c esp_driver_uart
    )
else()
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_i2c esp_driver_uart
    )
endif()
