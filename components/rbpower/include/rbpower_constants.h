/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_constants.h
 * @brief Compile-time limits and the 16-bit unit_id encoding — every capacity constant of the
 *        hub in one place.
 *
 * Highlights: ::RBPOWER_MAX_HOME_UNITS (PSRAM-gated platform ceiling), the hierarchical unit_id
 * bit layout (type / profile / local), and ::RBGRID_STATS_TIME_SCALE (test-only accelerated
 * statistics clock; MUST be 1 in production builds).
 */
#ifndef RBPOWER_CONSTANTS_H
#define RBPOWER_CONSTANTS_H

#include "sdkconfig.h"   /* CONFIG_SPIRAM gates RBPOWER_MAX_HOME_UNITS (PSRAM scaling) */

/* ================================================================================================
 * CONSTANTS AND LIMITS - MINIMUM
 * ================================================================================================ */

/** Maximum number of measurement channels */
#define RBPOWER_MAX_CHANNELS 4

#define RBPOWER_FREQ_50HZ 50 ///< Nominal mains frequency, Hz (50 Hz grids)
#define RBPOWER_FREQ_60HZ 60 ///< Nominal mains frequency, Hz (60 Hz grids)

//=============================================================================
// HOME ELECTRICAL NETWORK - Integrated rbhome structures
//=============================================================================

/*
 * Maximum logical metrics (units). With PSRAM the big per-unit statics (g_home_network,
 * g_period_accumulators, tariff arrays) live in PSRAM (EXT_RAM_BSS_ATTR), so the unit count
 * is not bounded by internal SRAM and scales to the product need. Without PSRAM a 6-unit
 * internal-DRAM ceiling applies (8 units ran out of memory at boot together with Wi-Fi and the
 * web server). The supported platform is ESP32 with PSRAM.
 */
#if defined(CONFIG_SPIRAM)
#define RBPOWER_MAX_HOME_UNITS 64      ///< Maximum units: PSRAM platform (4 MB PSRAM; raise as needed)
#else
#define RBPOWER_MAX_HOME_UNITS 6       ///< Maximum units: bare-ESP32 internal-SRAM ceiling
#endif
#define RBPOWER_MAX_HOME_DEVICES 4     ///< Maximum managed devices (kept small to save DRAM)
#define RBPOWER_MAX_SOURCES_PER_UNIT 2 ///< Data sources per unit (kept small to save DRAM)
#define RBPOWER_MAX_CHILDREN 4         ///< Child units per unit (kept small to save DRAM)
#define RBPOWER_HOME_INVALID_ID 0xFFFF ///< Invalid unit ID (16 bits)

//=============================================================================
// NEW UNIT_ID SYSTEM - 16-bit hierarchical structure
//=============================================================================

/*
 * Bit structure of unit_id:
 * [15-14] TYPE    - Source type (2 bits)
 * [13-8]  PROFILE - Sensor profile ID (6 bits = 64 profiles)
 * [7-0]   LOCAL   - Local information (8 bits)
 */

#define RBPOWER_UNIT_TYPE_MASK 0xC000    ///< unit_id mask of the source type (bits 15-14)
#define RBPOWER_UNIT_TYPE_SHIFT 14       ///< unit_id shift of the source type
#define RBPOWER_UNIT_TYPE_ADC 0x0000     ///< 00 - local ADC type code (no native-ADC path in this firmware; kept for unit_id compatibility)
#define RBPOWER_UNIT_TYPE_I2C 0x4000     ///< 01 - I2C devices (rbAmp modules)
#define RBPOWER_UNIT_TYPE_SPI 0x8000     ///< 10 - SPI devices
#define RBPOWER_UNIT_TYPE_ESP_NOW 0xC000 ///< 11 - ESP-NOW remote devices

#define RBPOWER_UNIT_PROFILE_MASK 0x3F00 ///< unit_id mask of the sensor profile (bits 13-8, 6 bits)
#define RBPOWER_UNIT_PROFILE_SHIFT 8     ///< unit_id shift of the sensor profile

#define RBPOWER_UNIT_LOCAL_MASK 0x00FF ///< unit_id mask of the local information (bits 7-0)
#define RBPOWER_UNIT_LOCAL_SHIFT 0     ///< unit_id shift of the local information

#define RBPOWER_I2C_BUS_MASK 0xC0  ///< I2C LOCAL field: bits 7-6 = bus number (up to 4 buses)
#define RBPOWER_I2C_BUS_SHIFT 6    ///< I2C LOCAL field: shift of the bus number
#define RBPOWER_I2C_ADDR_MASK 0x3F ///< I2C LOCAL field: bits 5-0 = I2C address slot (up to 64)
#define RBPOWER_I2C_ADDR_SHIFT 0   ///< I2C LOCAL field: shift of the I2C address slot

#define RBPOWER_MAX_SENSOR_PROFILES 64 ///< Maximum sensor profiles
#define RBPOWER_MAX_I2C_BUSES 4        ///< Maximum I2C buses
#define RBPOWER_MAX_I2C_ADDRESSES 64   ///< Maximum I2C addresses per bus

/**
 * @def RBGRID_STATS_TIME_SCALE
 * @brief Debug toggle: statistics test-time acceleration factor.
 *
 *   1   = PRODUCTION — real-time periods (REQUIRED for correct billing; the only valid production value).
 *   >1  = compress the WHOLE period hierarchy by this factor so hour→day→week→month→year can be
 *         validated in MINUTES instead of days. Test builds ONLY — set back to 1 before any
 *         production/OTA build.
 *
 * <pre>
 * One factor drives THREE things:
 *   • period TIMER interval  (rbhome_timers.c): fixed periodic = nominal/scale, wall-clock align bypassed
 *   • boundary CLOCK         (rbhome_statistics.c): virtual = uptime×scale, anchored boot=epoch0, so
 *                             virtual XX:00 lands exactly on a timer fire (NTP-immune — uptime-based)
 *   • SNTP                   (rbtime.c): skipped when >1 (no periodic NTP resync to perturb the test;
 *                             the virtual clock doesn't need real wall time anyway)
 *   Example @ 60: 15min=15s · hour≈1min · day≈24min · week≈2.8h · month≈12h · year≈6h.
 * </pre>
 * Defined here (not rbhome_types.h) so rbtime.c — which doesn't include rbhome_types.h — sees it.
 */
#ifndef RBGRID_STATS_TIME_SCALE
#define RBGRID_STATS_TIME_SCALE 1
#endif

#endif // RBPOWER_CONST_H