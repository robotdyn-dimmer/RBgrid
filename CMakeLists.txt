# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Explicit firmware version (overrides git "v1.0.1-dirty") — bump per release so /api/system/version
# + the HA `update` entity can compare current-vs-available for OTA. (dev = version owner.)
set(PROJECT_VER "1.1.0")

idf_build_set_property(MINIMAL_BUILD ON)
project(main)

# Accelerated-statistics TEST build (UPDATE_SPEC_2026-10 block A.2): -DRBGRID_STATS_TIME_SCALE=N
# compresses every stats period by N (15min→15min/N etc., see rbhome_timers.c) so WEEK/MONTH/YEAR
# recompute can be exercised on the bench in hours. NEVER a release artifact — build into a separate
# -B build_accel dir and label the md5 as accel.
if(RBGRID_FORMAT_V4)
    idf_build_set_property(COMPILE_DEFINITIONS "RBGRID_FORMAT_V4=1" APPEND)
endif()
if(RBGRID_STATS_TIME_SCALE)
    idf_build_set_property(COMPILE_DEFINITIONS "RBGRID_STATS_TIME_SCALE=${RBGRID_STATS_TIME_SCALE}" APPEND)
endif()

# cJSON parse depth: the default limit is 1000 levels of recursion — about 1 KB of '[' overflows the httpd / MQTT
# task stack (unauthenticated on the MQTT cmd topic and on a hub without a token). Our documents are < 10 deep.
# (pre-release review 2026-10-18, K3)
idf_component_get_property(_json_lib json COMPONENT_LIB)
target_compile_definitions(${_json_lib} PRIVATE CJSON_NESTING_LIMIT=32)

# Static-analysis build (pre-release review): RBGRID_LINT="<flags>" (environment) adds compiler flags to OUR components
# only (main, rbpower), not to ESP-IDF. Flags come from the RBGRID_LINT environment variable (idf.py parses
# a -W inside -D as its own option). Separate -B build_lint dir; never a release artifact.
if(DEFINED ENV{RBGRID_LINT})
    separate_arguments(_rbgrid_lint UNIX_COMMAND "$ENV{RBGRID_LINT}")
    foreach(_c main rbpower)
        idf_component_get_property(_lib ${_c} COMPONENT_LIB)
        target_compile_options(${_lib} PRIVATE ${_rbgrid_lint})
    endforeach()
endif()
