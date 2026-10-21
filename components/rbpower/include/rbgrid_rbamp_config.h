/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file    rbgrid_rbamp_config.h
 * @brief   Persistable rbAmp fleet/bus configuration.
 *
 * The rbAmp bridge (main/rbamp_bridge.c) brings up one I2C master bus and polls N
 * rbAmp metering modules. Its config — bus pins, poll cadence, host gains, and the
 * per-module {address, channel→unit} map — is persisted. This struct family is defined
 * HERE in rbpower (not main/) so the unified
 * config/persistence layer (rbjson.c serialize/parse, rbgrid_config_storage.c → /rbamp.json)
 * can own its round-trip alongside units.json/tariffs.json. main/rbamp_bridge.h includes
 * this header and the bridge consumes the struct directly — ONE definition, no duplication
 * and no translation layer (main depends on rbpower, so the include direction is legal).
 *
 * The per-channel CT model + the I2C-domain unit UIDs are ALSO carried by the persisted
 * units (units.json: source_type=I2C_SENSOR, sensor_profile_id=CT code, module/channel in
 * the unit_id) — this file adds the bus-level + address-map portion that units don't hold.
 *
 * The structures are plain data with no locking; the bridge owns the live copy and the config
 * layer must not modify it concurrently with the bridge's poll task.
 */
#ifndef RBGRID_RBAMP_CONFIG_H
#define RBGRID_RBAMP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define RBAMP_MAX_MODULES   3   /**< Maximum rbAmp modules in the fleet (raise with the chip/board) */
#define RBAMP_MAX_ENTRIES   8   /**< Inject entries per module: voltage + up to 7 current channels */
#define RBAMP_ENTRY_VOLTAGE 0xFF /**< entry.source sentinel: feed the module voltage (not a current channel) */
#define RBAMP_ENTRY_PHASES  0xFE /**< entry.source sentinel: ALL current channels of the module are
                                  *   grouped into ONE phase-grouped unit — Σ P/I/energy injected to the unit,
                                  *   per-phase live values (v/i/p/q/pf per channel = phase L(c+1)) published as ph[].
                                  *   ct_code applies to every current channel. Used for a three-phase main input. */

/** @brief One inject mapping: a module source (voltage or current channel) -> an RBgrid unit. */
typedef struct {
    uint8_t  source;   /**< RBAMP_ENTRY_VOLTAGE, or a current channel index 0..6. */
    uint8_t  ct_code;  /**< CT code to provision for a current channel (0 = skip); ignored for voltage. */
    uint16_t unit_id;  /**< RBgrid unit to inject into (0 = skip inject). */
    uint8_t  ct_user;  /**< 1 = ct_code is the user's choice, not the module's shipped model: a BASIC module restart
                        *   reloads the shipped calibration, so the hub force-re-applies it. */
} rbamp_entry_cfg_t;

/** @brief One rbAmp module on the bus. */
typedef struct {
    uint8_t           addr;        /**< 7-bit I2C address (0 = unused slot). */
    uint8_t           entry_count; /**< number of valid entries[]. */
    uint8_t           drdy_p1;     /**< this module's own DRDY GPIO + 1; 0 = the fleet line (drdy_gpio_p1) */
    uint8_t           bus;         /**< I2C bus the module sits on: 0 = the main bus (sda/scl_gpio), 1 = bus 1
                                    *   (bus1_*_p1). Modules may share one bus or be split over two. */
    uint8_t           phase;       /**< Mains phase (1..3) a SINGLE-voltage module measures; 0 = L1.
                                    *   Multi-voltage modules map voltage channel c -> L(c+1). */
    rbamp_entry_cfg_t entries[RBAMP_MAX_ENTRIES]; ///< Inject mappings; the first entry_count are valid
    uint8_t           ct_uid[12];  /**< UID of the module the entries' ct_user models were written to (all-0 = unknown,
                                    *   e.g. a config saved by older firmware). A different module found at the same address
                                    *   drops them instead of having them force-written over its own calibration. */
} rbamp_module_cfg_t;

/** @brief Fleet config. Use RBAMP_BRIDGE_DEFAULT_CONFIG() for the single @0x50 case. */
typedef struct {
    int      i2c_port;        /**< I2C port (-1 = auto). */
    int      sda_gpio;        /**< SDA GPIO (default 25). */
    int      scl_gpio;        /**< SCL GPIO (default 26). */
    bool     internal_pullup; /**< enable internal pull-ups (use externals on a real board). */
    uint32_t poll_period_ms;  /**< Poll cadence, ms (200 for the 200 ms accumulation window). */
    bool     log_readings;    /**< log each module's U/I/P/PF snapshot. */
    bool     inject_enabled;  /**< inject into rbhome stats. */
    float    current_gain;    /**< optional host gain on current/power (default 1.0; fallback for uncalibrated). */
    float    voltage_gain;    /**< optional host trim on voltage (default 1.0). */
    bool     ct_persist;      /**< CMD_SAVE_USER_CONFIG after a CT-model write (flash; once). */
    uint8_t  drdy_gpio_p1;    /**< DRDY (data-ready) line GPIO + 1, wired-OR of all modules; 0 = not wired (zero-init safe:
                                   never GPIO0). */
    uint8_t  bus1_sda_p1;     /**< bus 1 (the second ESP32 I2C controller) SDA GPIO + 1; 0 = bus 1 not used (zero-init safe) */
    uint8_t  bus1_scl_p1;     /**< bus 1 SCL GPIO + 1 */
    uint8_t  module_count;    /**< Number of valid modules[] (0..RBAMP_MAX_MODULES). */
    rbamp_module_cfg_t modules[RBAMP_MAX_MODULES]; ///< Per-module configuration
} rbamp_bridge_config_t;

/**
 * @brief Default fleet config: one single-voltage module at 0x50 — voltage -> unit 0x0007 (voltage bus),
 *        current ch0 (SCT-013-10A, CT code 2) -> unit 0x0006 (main supply); SDA 25 / SCL 26, 1000 ms poll,
 *        injection disabled.
 */
#define RBAMP_BRIDGE_DEFAULT_CONFIG() (rbamp_bridge_config_t){              \
    .i2c_port = 0, .sda_gpio = 25, .scl_gpio = 26, .internal_pullup = true, \
    .poll_period_ms = 1000, .log_readings = true, .inject_enabled = false,  \
    .current_gain = 1.0f, .voltage_gain = 1.0f, .ct_persist = false,        \
    .drdy_gpio_p1 = 0, .bus1_sda_p1 = 0, .bus1_scl_p1 = 0,                    \
    .module_count = 1,                                                      \
    .modules = { { .addr = 0x50, .entry_count = 2, .entries = {             \
        { .source = RBAMP_ENTRY_VOLTAGE, .ct_code = 0, .unit_id = 0x0007, .ct_user = 0 }, \
        { .source = 0,                   .ct_code = 2, .unit_id = 0x0006, .ct_user = 0 }, \
    } } },                                                                  \
}

#endif /* RBGRID_RBAMP_CONFIG_H */
