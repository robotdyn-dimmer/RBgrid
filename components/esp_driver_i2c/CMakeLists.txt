# RBgrid override of the ESP-IDF esp_driver_i2c component.
# Only i2c_master.c is patched (task-watchdog and ISR fixes, marked "RBGRID PATCH"); one copy per IDF minor
# version, picked here. Every other file of the driver (sources, headers, linker fragment, Kconfig) is the IDF's
# own, taken from $IDF_PATH, so an IDF update only needs the patched file re-based.
idf_build_get_property(target IDF_TARGET)

idf_build_get_property(idf_path IDF_PATH)   # forward slashes (the env var has Windows backslashes)
set(idf_i2c "${idf_path}/components/esp_driver_i2c")
set(rb_ver "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/v${rb_ver}/i2c_master.c")
    message(FATAL_ERROR "esp_driver_i2c override: no patched i2c_master.c for ESP-IDF ${rb_ver} (have v5.4, v5.5). "
                        "Re-base the RBGRID PATCH set onto this IDF version first.")
endif()

set(srcs)
if(CONFIG_SOC_I2C_SUPPORTED)
    list(APPEND srcs "v${rb_ver}/i2c_master.c" "${idf_i2c}/i2c_common.c")
    if(CONFIG_SOC_I2C_SUPPORT_SLAVE)
        if(CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2)
            list(APPEND srcs "${idf_i2c}/i2c_slave_v2.c")
        else()
            list(APPEND srcs "${idf_i2c}/i2c_slave.c")
        endif()
    endif()
endif()

if(${target} STREQUAL "linux")
    set(priv_requires esp_ringbuf)
else()
    set(priv_requires esp_driver_gpio esp_pm esp_ringbuf)
endif()

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "${idf_i2c}/include"
                    PRIV_INCLUDE_DIRS "${idf_i2c}"
                    PRIV_REQUIRES "${priv_requires}"
                    LDFRAGMENTS "${idf_i2c}/linker.lf"
                    )
