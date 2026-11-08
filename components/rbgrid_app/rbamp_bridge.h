/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file    rbamp_bridge.h
 * @brief   RBgrid <-> rbAmp integration source — multi-module fleet.
 *
 * Brings up one or two I2C master buses and probes/polls N rbAmp metering modules (public
 * rbamp-esp-idf library). Each module exposes a voltage (UI variant) and/or 1..3
 * current channels; each is mapped to an RBgrid unit and fed via the external-
 * source injection API (rbpower_home_inject_200ms). rbAmp delivers calibrated
 * physical V/A/W (per-unit gains in module flash) — RBgrid applies no host
 * scaling by default (current/voltage_gain = 1.0 fallback for uncalibrated units).
 * CT model = the channel's sensor type, provisioned VERIFY-THEN-SET (only when
 * wrong/unset) so a calibrated module's per-unit gain is never clobbered.
 *
 * Uses the new IDF i2c_master driver (esp_driver_i2c, IDF >= 5.2).
 *
 * Calling contexts: one poll task (started by rbamp_bridge_start()) owns the fleet; the stats task requests the
 * quarter collect with rbamp_bridge_request_collect(); console verbs and HTTP handlers call the other functions.
 * Every function that touches a module takes the shared bus lock internally (see rbamp_bridge_bus_lock()), so
 * none of them may be called while the caller already holds it. The second I2C bus and the I2C management
 * functions (rbamp_bridge_i2c_*) are available in the code but are not part of the v1.1.0 release feature set.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "rbpower_enums.h"        /* rbpower_stat_period_type_t */
#include "rbgrid_rbamp_config.h"
#include "rbamp.h"   /* rbamp_std_info_t; rbamp_bridge_config_t comes from rbgrid_rbamp_config.h (persisted by rbpower) */
#include "rbhome_types.h"         /* rbpower_unit_definition_t (bind hook takes the full def) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unbind hook: clear the fleet entry bound to @p unit_id and persist /rbamp.json, so a deleted rbAmp unit
 *        does not respawn. Registered as the rbpower unit-unbind callback at start (called by
 *        rbpower_home_remove_unit on delete). No-op if the unit is not a fleet entry.
 * @param[in] unit_id  Unit id (rbpower_unit_id_t, a uint16_t).
 */
void rbamp_bridge_unbind_unit(uint16_t unit_id);
/**
 * @brief Bring-up diagnostics: STD identity read from the module (rbamp_std_info) plus the bridge's per-phase cache.
 * @param[in]  module  Fleet index.
 * @param[out] out     STD identity.
 * @param[out] u_ph    Per-phase voltage cache (V, NaN = unknown); may be NULL.
 * @param[out] q_ph    Per-channel reactive power cache (var, NaN = unknown); may be NULL.
 * @param[out] nv      Voltage channels the module publishes; may be NULL.
 * @param[out] nc      Current channels; may be NULL.
 * @retval ESP_OK                 Identity read.
 * @retval ESP_ERR_INVALID_STATE  Module absent or @p out is NULL.
 * @return Other codes come from the rbamp library (e.g. ESP_ERR_NOT_SUPPORTED on a BASIC module).
 */
esp_err_t rbamp_bridge_std_info(uint8_t module, rbamp_std_info_t *out, float u_ph[3], float q_ph[3], uint8_t *nv, uint8_t *nc);

/**
 * @brief Bind hook (symmetric to unbind): set the fleet entry for the unit's (module, channel) so the source
 *        resumes injecting, and persist /rbamp.json. Registered as the rbpower unit-bind callback at start.
 *
 * Takes the full definition — unit ids are opaque and carry no physics; the fleet entry is found via
 * def->device_uid (module index) and def->channel_idx. Non-I2C sources are ignored.
 * @param[in] def  Unit definition (NULL or unit_id 0 is ignored).
 */
void rbamp_bridge_bind_unit(const rbpower_unit_definition_t *def);
/**
 * @brief Rebind hook for a bridge-fed unit (e.g. a main input): move its fleet entry to (module, ch). The unit id,
 *        point and history stay; /rbamp.json is saved.
 * @param[in] unit_id  Unit to move (non-zero).
 * @param[in] module   Target fleet index.
 * @param[in] ch       Target current channel.
 * @retval 0   Moved (or already there).
 * @retval -1  Bad arguments, no such channel, or the config save failed.
 * @retval -2  The target channel is bound to another unit.
 */
int rbamp_bridge_feed_rebind(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch);

/**
 * @brief Start the fleet: create the bus mutex, init I2C bus 0 (and bus 1 when configured), probe and provision
 *        each module (CT verify-then-set), then start the poll task pinned to core 1 and register the rbpower
 *        unit callbacks. A module that does not answer is left absent and re-probed by the healer every 10 s.
 * @param[in] cfg  Fleet configuration (copied); NULL uses RBAMP_BRIDGE_DEFAULT_CONFIG().
 * @retval ESP_OK                 Started (even with zero modules found).
 * @retval ESP_ERR_INVALID_STATE  Already running.
 * @retval ESP_ERR_INVALID_ARG    module_count is 0 or above RBAMP_MAX_MODULES.
 * @retval ESP_ERR_NO_MEM         Mutex or poll task could not be created.
 * @return Any other code is the i2c_new_master_bus() error for bus 0.
 */
esp_err_t rbamp_bridge_start(const rbamp_bridge_config_t *cfg);
/** @brief Stop the poll task (waits up to 1 s) and release all module handles and the I2C buses. */
void rbamp_bridge_stop(void);
/** @brief Handle of sensor I2C bus 0, for other code to borrow (same bus, no second master).
 *  @return The bus handle, or NULL before start. */
i2c_master_bus_handle_t rbamp_bridge_get_bus(void);

/**
 * @brief Lock the shared sensor I2C bus. ANY code touching it outside the bridge (e.g. rbbench i2c verbs) MUST
 *        bracket its transaction with rbamp_bridge_bus_lock() / rbamp_bridge_bus_unlock() — concurrent
 *        i2c_master access wedges the IDF driver in a bus-busy spin. Blocks until free (feeds the task watchdog
 *        every second if the calling task is subscribed). Do not call any other rbamp_bridge_* function while
 *        holding it.
 */
void rbamp_bridge_bus_lock(void);
/** @brief Unlock the shared sensor I2C bus taken by rbamp_bridge_bus_lock(). */
void rbamp_bridge_bus_unlock(void);

/**
 * @brief Set the fleet-wide host-gain calibration trim at runtime. Applies live (next poll). Persistence is the
 *        caller's job (rbgrid_storage_save_rbamp). Host trim is a fallback for an uncalibrated module —
 *        factory-calibrated rbAmp modules carry their gains in module flash and want 1.0.
 * @param[in] v_gain  Voltage gain (> 0).
 * @param[in] i_gain  Current gain (> 0); also scales power.
 * @retval ESP_OK               Applied.
 * @retval ESP_ERR_INVALID_ARG  A gain is NaN or <= 0.
 */
esp_err_t rbamp_bridge_set_host_gains(float v_gain, float i_gain);
/** @brief Get the fleet-wide host gains.
 *  @param[out] v_gain  Voltage gain; may be NULL.
 *  @param[out] i_gain  Current gain; may be NULL. */
void      rbamp_bridge_get_host_gains(float *v_gain, float *i_gain);

/**
 * @brief Set the CT model of one channel, loading the module's factory calibration for that clamp (RBgrid needs no
 *        host scaling). RAM-only until rbamp_bridge_save_user_config(). Codes: 1=5A 2=10A 3=30A 4=50A 6=20A.
 *        Takes the bus lock.
 * @param[in] module   Fleet index.
 * @param[in] channel  Current channel.
 * @param[in] code     CT model code.
 * @retval ESP_OK                 Written.
 * @retval ESP_ERR_INVALID_STATE  Module absent, mid firmware update, or not an SCT- (class 1) module.
 * @retval ESP_ERR_NOT_SUPPORTED  CT- series module (CT calibrated in pair with the module; never overwritten).
 * @return Other codes come from the rbamp library.
 */
esp_err_t rbamp_bridge_set_ct_model(uint8_t module, uint8_t channel, uint8_t code);
/**
 * @brief Read back a channel's CT model code (bind-verify). Takes the bus lock.
 * @param[in]  module   Fleet index.
 * @param[in]  channel  Current channel.
 * @param[out] out      CT model code.
 * @retval ESP_OK                 Read.
 * @retval ESP_ERR_INVALID_STATE  Module absent or @p out is NULL.
 * @return Other codes come from the rbamp library.
 */
esp_err_t rbamp_bridge_read_ct_model(uint8_t module, uint8_t channel, uint8_t *out);
/**
 * @brief Persist the user-config block (CT model/class/address) to a module's flash (~700 ms). Takes the bus lock.
 * @param[in] module  Fleet index.
 * @retval ESP_OK                 Saved (virtual module: no-op).
 * @retval ESP_ERR_INVALID_STATE  Module absent or mid firmware update.
 * @return Other codes come from the rbamp library.
 */
esp_err_t rbamp_bridge_save_user_config(uint8_t module);

/**
 * @brief Close one module's period window and inject the finished RESULTS into RBgrid via
 *        rbpower_home_inject_period_result (bypasses the 200 ms accumulator).
 *
 * Voltage-bearing modules: latch + read the on-device avg_p/max_p + master_dt; energy = avg_p × dt, with the
 * reboot-gap tail folded into the first quarter after a restart. Current-only modules: close the host-side
 * V × I integration window. Target the 15-min slot (RBPOWER_STAT_PERIOD_15MIN); higher periods aggregate from
 * the 15-min results. Called by the poll task after rbamp_bridge_request_collect(), by the planned-restart hook,
 * by the module firmware update and by the `period` verb. Takes the bus lock per transaction.
 * @param[in] module  Fleet index.
 * @param[in] period  Period slot to inject into.
 * @retval ESP_OK                    At least one unit injected.
 * @retval ESP_ERR_INVALID_STATE     Module absent.
 * @retval ESP_ERR_NOT_SUPPORTED     Current-only module with no voltage source in the fleet.
 * @retval ESP_ERR_INVALID_RESPONSE  Period snapshot stale (< 30 s since the last latch) or nothing injected.
 * @return Other codes come from the rbamp period-snapshot read.
 */
esp_err_t rbamp_bridge_collect_period(uint8_t module, rbpower_stat_period_type_t period);
/**
 * @brief Ask the poll task to collect every module's period at its next iteration (≤ 1 poll later).
 *
 * The quarter-boundary path, called by the stats task — keeps slow module I/O off its watchdog-supervised loop.
 * @param[in] period  Period slot to inject into.
 * @param[in] end_ts  Boundary (Unix s) the quarter ends at; the quarter is attributed to that boundary's hour even
 *                    when the collect lands seconds later (0 = unknown → by the collect time).
 */
void rbamp_bridge_request_collect(rbpower_stat_period_type_t period, uint32_t end_ts);
/**
 * @brief `drdy` verb: report the DRDY line and per-module DRDY/inject statistics, or set the fleet DRDY GPIO
 *        (persisted in /rbamp.json and applied to every module now).
 * @param[in]  set_gpio  New GPIO (0..39), -1 = off; used only when @p do_set.
 * @param[in]  do_set    true to set, false for status only.
 * @param[out] out       Text output.
 * @param[in]  cap       Size of @p out.
 * @return Number of characters written (snprintf semantics).
 */
int rbamp_bridge_drdy(int set_gpio, bool do_set, char *out, size_t cap);
/**
 * @brief `digest` verb: read @p n BASIC windows via wait_data_ready + rbamp_read_digest (seq / signed P / read µs)
 *        plus one read_all to compare.
 * @param[in]  m    Fleet index.
 * @param[in]  n    Number of windows.
 * @param[out] out  Text output.
 * @param[in]  cap  Size of @p out.
 * @return Number of characters written.
 */
int rbamp_bridge_digest(uint8_t m, int n, char *out, size_t cap);
/**
 * @brief `ctmodel` verb: set channel @p ch's CT model on module @p m (also unbound channels).
 * @param[in]  m     Fleet index.
 * @param[in]  ch    Current channel.
 * @param[in]  code  CT model code.
 * @param[in]  mode  0 = normal (a real change is marked as the user's choice), 1 = factory (write the model number
 *                   only; the shipped calibration returns at the next module restart), 2 = force-write the preset.
 * @param[out] out   Text output.
 * @param[in]  cap   Size of @p out.
 * @return Number of characters written.
 */
int rbamp_bridge_ctmodel(uint8_t m, uint8_t ch, uint8_t code, int mode, char *out, size_t cap);   /* mode 0 normal · 1 factory · 2 force */
/**
 * @brief `modreset` verb: CMD_RESET of module @p m (it reloads its flash calibration).
 * @param[in]  m    Fleet index.
 * @param[out] out  Text output.
 * @param[in]  cap  Size of @p out.
 * @return Number of characters written.
 */
int rbamp_bridge_modreset(uint8_t m, char *out, size_t cap);
/**
 * @brief `vmod` verb: virtual STD module control (debug builds with CONFIG_RBGRID_BENCH_FULL only).
 * @param[in]  argc  Argument count (argv[0] = verb).
 * @param[in]  argv  Arguments: module index, then one of: on [nch] [W] [V] | off | p (ch or all) W | v V |
 *                   restart | gen.
 * @param[out] out   Text output.
 * @param[in]  cap   Size of @p out.
 * @return Number of characters written.
 */
int rbamp_bridge_virt_cmd(int argc, char **argv, char *out, size_t cap);
/**
 * @brief `pollpause` verb: skip polling module @p m for @p secs (software poll hole, the module stays bound).
 * @param[in] m     Fleet index.
 * @param[in] secs  Pause length (s); 0 resumes now.
 * @retval 0   Applied.
 * @retval -1  Bad module index.
 */
int rbamp_bridge_poll_pause(uint8_t m, uint32_t secs);   /* `pollpause <m> <s>`: software poll hole (0 = resume) */

/* STD module firmware update (hub-driven). img = PSRAM buffer with a vendor-PREPARED image for the NON-running
 * bank (ownership passes to the bridge, freed when done or refused). Refusals return an error with a reason in
 * why. Progress: rbamp_bridge_module_ota_status() writes a JSON object. One update at a time. */
/**
 * @brief Report whether module @p m is inside a firmware-update write/commit session (it then answers BUSY, so
 *        other module commands are refused).
 * @param[in] m  Fleet index.
 * @return true while writing or committing.
 */
bool rbamp_bridge_module_ota_busy(uint8_t m);       /* module m mid OTA write/commit: other module commands are refused */
/**
 * @brief SAVE_CAL: persist the module's current calibration registers (+ the cal_ok marker on STD 0x7A+).
 *        Maintenance-level, explicit only, never automatic. Real modules only. Takes the bus lock.
 * @param[in] module  Fleet index.
 * @retval ESP_OK                 Saved; the cached cal flag is refreshed.
 * @retval ESP_ERR_INVALID_STATE  Module absent, virtual, or mid firmware update.
 * @return Other codes come from the rbamp library.
 */
esp_err_t rbamp_bridge_save_gains(uint8_t module);   /* SAVE_CAL: persist current calibration (+cal_ok marker); explicit only */
/**
 * @brief `libcheck` verb: one-shot check of the rbamp-esp-idf 1.7.0 STD calls (start current, phase angles,
 *        phase sequence, config, energy flags); restores what it changes and saves nothing to the module. Each
 *        section is also written to the event log.
 * @param[in]  module  Fleet index (real module only).
 * @param[out] out     Text output.
 * @param[in]  n       Size of @p out.
 */
void rbamp_bridge_libcheck(uint8_t module, char *out, size_t n);   /* rbamp-esp-idf 1.7.0 call check (start current, angles, phase seq, config); restores what it changes */
/**
 * @brief Start a hub-driven firmware update of STD module @p m (POST /api/v1/modules/{m}/firmware). A task
 *        writes the image page by page (bus taken per page, metering continues), commits, closes the open
 *        quarter, activates and re-provisions the module.
 * @param[in]     m           Fleet index.
 * @param[in]     img         PSRAM image for the NON-running bank; ownership passes to the bridge (freed when done
 *                            or refused).
 * @param[in]     size        Image size (bytes); must equal the module's bank size.
 * @param[in]     bank        Target bank address; must be the inactive bank.
 * @param[in]     has_board   true to check @p board_word against the module.
 * @param[in]     board_word  Board id word of the image.
 * @param[out]    why         Reason text ("started: N pages" on success).
 * @param[in]     wn          Size of @p why.
 * @retval ESP_OK                 Update task started.
 * @retval ESP_ERR_INVALID_STATE  Another update is still running or finishing.
 * @retval ESP_ERR_NOT_FOUND      Module absent.
 * @retval ESP_ERR_NOT_SUPPORTED  Virtual module, not an STD module, or no OTA capability.
 * @retval ESP_ERR_INVALID_SIZE   Image size differs from the bank size.
 * @retval ESP_ERR_INVALID_ARG    Bank is the running bank or no module bank, or the board word does not match.
 * @retval ESP_ERR_NO_MEM         The update task could not be created.
 */
esp_err_t rbamp_bridge_module_ota_start(uint8_t m, uint8_t *img, size_t size, uint32_t bank,
                                        bool has_board, uint32_t board_word, char *why, size_t wn);
/**
 * @brief Write the firmware-update progress as a JSON object
 *        {module,state,done,total,elapsed_s,fw_before,fw_after,reason,message}.
 * @param[out] out  Output buffer.
 * @param[in]  cap  Size of @p out.
 * @return Number of characters written (snprintf semantics).
 */
int rbamp_bridge_module_ota_status(char *out, size_t cap);

/**
 * @brief Read a probed module's status (for a console `status` verb, since CONSOLE_NONE silences boot ESP_LOG).
 * @param[in]  m            Fleet index.
 * @param[out] addr         I2C address; may be NULL.
 * @param[out] has_vhw      Module has voltage hardware; may be NULL.
 * @param[out] energy_mode  0 = NONE, 1 = ON-DEVICE, 2 = HOST; may be NULL.
 * @return true if module @p m is present.
 */
bool rbamp_bridge_module_status(uint8_t m, uint8_t *addr, bool *has_vhw, uint8_t *energy_mode);
/**
 * @brief Reboot-gap outcome of module @p m as one text line (for the `etstat` verb).
 * @param[in]  m    Fleet index.
 * @param[out] buf  Text output.
 * @param[in]  len  Size of @p buf.
 * @return Number of characters written; 0 on bad arguments.
 */
int rbamp_bridge_gap_status(uint8_t m, char *buf, size_t len);

/**
 * @brief `modscan` verb backend: immediately re-probe configured-but-absent modules (the
 *        periodic healer does the same every 10 s), re-provision/classify/publish any that
 *        joined, and write per-module state + healer counters into @p out.
 * @param[out] out  Text output.
 * @param[in]  cap  Size of @p out.
 * @return Number of characters written; 0 if @p out is NULL or @p cap is 0.
 */
int rbamp_bridge_rescan(char *out, size_t cap);


/* I2C fleet management (two buses, scan, add module, address, bus move, pins) — the /api/v1/i2c API backend.
 * Called from the httpd task (rbi2c_api.c). Every function takes the bus lock itself. */
#include "cJSON.h"
/**
 * @brief Build the I2C state for GET /api/v1/i2c: buses (port, pins, up), the board pin profile (pins_ok,
 *        pins_reserved), the DRDY line, reboot_pending and the configured modules.
 * @return New cJSON object that the caller must cJSON_Delete(), or NULL on allocation failure.
 */
cJSON    *rbamp_bridge_i2c_state(void);
/**
 * @brief Scan bus @p bus (0x08..0x77) with address-only reads; rbAmp identity reads only in 0x50..0x5F.
 * @param[in]     bus  Bus index (0 or 1).
 * @param[in,out] arr  cJSON array that receives one {addr, module|null, kind, fw?, uid?} object per device.
 * @retval ESP_OK                    Scanned.
 * @retval ESP_ERR_INVALID_STATE     Bus not running.
 * @retval ESP_ERR_INVALID_RESPONSE  Every address answers (SDA held low: stuck slave or missing pull-ups).
 */
esp_err_t rbamp_bridge_i2c_scan(uint8_t bus, cJSON *arr);
/**
 * @brief Add a module found by a scan: it must answer at (@p bus, @p addr) as an rbAmp. It joins at once.
 * @param[in]  bus    Bus index.
 * @param[in]  addr   7-bit address (0x08..0x77).
 * @param[out] m_out  New fleet index; may be NULL.
 * @param[out] why    Reason text on refusal.
 * @param[in]  wn     Size of @p why.
 * @retval ESP_OK                    Added (config saved).
 * @retval ESP_ERR_INVALID_STATE     Bus not running, or a module is already configured there.
 * @retval ESP_ERR_INVALID_ARG       Address out of range.
 * @retval ESP_ERR_NO_MEM            Module limit reached.
 * @retval ESP_ERR_NOT_FOUND         Nothing answers there.
 * @retval ESP_ERR_INVALID_RESPONSE  The device is not an rbAmp module.
 * @return Other codes come from saving /rbamp.json.
 */
esp_err_t rbamp_bridge_i2c_add(uint8_t bus, uint8_t addr, int *m_out, char *why, size_t wn);
/**
 * @brief Change a module's I2C address (prepare + commit; the module saves it and restarts). Verified by UID at the
 *        new address, then the module re-joins.
 * @param[in]  m     Fleet index.
 * @param[in]  addr  New 7-bit address (0x08..0x77), free on the module's bus.
 * @param[out] why   Reason text on failure.
 * @param[in]  wn    Size of @p why.
 * @retval ESP_OK                    Changed (or unchanged address).
 * @retval ESP_ERR_INVALID_STATE     Module absent/virtual/updating, or the address is taken.
 * @retval ESP_ERR_INVALID_ARG       Address out of range.
 * @retval ESP_ERR_INVALID_RESPONSE  The change was not confirmed by UID.
 * @return Other codes come from the rbamp library or from saving /rbamp.json.
 */
esp_err_t rbamp_bridge_i2c_set_addr(uint8_t m, uint8_t addr, char *why, size_t wn);
/**
 * @brief The user moved module @p m to the other bus (same address): drop its handle and re-join on @p bus.
 * @param[in]  m    Fleet index.
 * @param[in]  bus  New bus index.
 * @param[out] why  Reason text on refusal.
 * @param[in]  wn   Size of @p why.
 * @retval ESP_OK                 Moved (or already there).
 * @retval ESP_ERR_INVALID_ARG    No such module, or virtual.
 * @retval ESP_ERR_INVALID_STATE  Bus not running, address taken on that bus, or the module is updating.
 * @return Other codes come from saving /rbamp.json.
 */
esp_err_t rbamp_bridge_i2c_set_bus(uint8_t m, uint8_t bus, char *why, size_t wn);
/**
 * @brief Set the pins of bus @p bus, validated against the board profile; persisted and applied at the next restart.
 * @param[in]  bus  Bus index (0 or 1).
 * @param[in]  sda  SDA GPIO; < 0 on bus 1 turns bus 1 off (only when no module sits on it).
 * @param[in]  scl  SCL GPIO.
 * @param[out] why  Reason text on refusal.
 * @param[in]  wn   Size of @p why.
 * @retval ESP_OK                 Saved (reboot_pending set).
 * @retval ESP_ERR_INVALID_ARG    Bad bus, SDA == SCL, or a pin is reserved/used.
 * @retval ESP_ERR_INVALID_STATE  Turning bus 1 off while a module sits on it.
 * @return Other codes come from saving /rbamp.json.
 */
esp_err_t rbamp_bridge_i2c_set_pins(uint8_t bus, int sda, int scl, char *why, size_t wn);
/**
 * @brief Set module @p m's own DRDY line (applied live).
 * @param[in]  m     Fleet index.
 * @param[in]  gpio  GPIO, or -1 to fall back to the fleet DRDY line.
 * @param[out] why   Reason text on refusal.
 * @param[in]  wn    Size of @p why.
 * @retval ESP_OK               Saved and applied.
 * @retval ESP_ERR_INVALID_ARG  No such module, virtual, or the GPIO is reserved/used.
 * @return Other codes come from saving /rbamp.json.
 */
esp_err_t rbamp_bridge_i2c_set_drdy(uint8_t m, int gpio, char *why, size_t wn);
#ifdef __cplusplus
}
#endif
