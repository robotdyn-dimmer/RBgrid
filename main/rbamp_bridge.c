/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file    rbamp_bridge.c
 * @brief   RBgrid <-> rbAmp integration source — multi-module fleet. See rbamp_bridge.h.
 *
 * Consumes the public rbamp-esp-idf client library. One or two I2C buses, N modules.
 * Flow per module: rbamp_new -> rbamp_begin -> CT verify-then-set -> read_all poll
 * -> per-entry inject into RBgrid units. rbAmp returns calibrated physical values
 * (per-unit gains in module flash); host gains default 1.0.
 *
 * Tasks and locking:
 *  - The poll task (rbamp_poll) reads every module each poll period, injects the 200 ms realtime data,
 *    runs the host-side energy integration and, on request, the quarter-hour period collect.
 *  - The stats task asks for the period collect through rbamp_bridge_request_collect() so slow module I/O
 *    stays off its watchdog-supervised loop; console verbs and httpd handlers call the other public
 *    functions directly.
 *  - Every I2C transaction is bracketed by bus_take() / bus_give() (the s_bus_lock mutex), and code outside
 *    this file uses rbamp_bridge_bus_lock() / rbamp_bridge_bus_unlock(). The lock is never held across an
 *    rbpower inject or a flash write unless stated.
 *  - s_hostacc is shared between the poll task and the collector and is guarded by s_hostacc_mux.
 */

#include "rbamp_bridge.h"
#include "rbstats_v4_types.h"

#include <string.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"   /* esp_get_free_heap_size — boot heap-trail diagnostics */

#include "rbamp.h"
#include "rbpower.h"        /* rbpower_err_t */
#include "rbhome_types.h"   /* rbpower_home_inject_200ms + realtime structs + unbind cb */
#include "rbgrid_config_storage.h"   /* rbgrid_storage_save_rbamp — bind/unbind persist /rbamp.json */
#include "rbgrid.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "esp_attr.h"              /* RTC_NOINIT_ATTR — virtual module counters survive a warm reboot */
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */
#include "esp_task_wdt.h"         /* bus_take feeds the TWDT of a subscribed waiter */

#include "cJSON.h"
#ifndef CONFIG_RBAMP_SCT_RATING_CORRECTION
/** @brief Library Kconfig fallback: 0 = the hub re-applies user CT models itself. */
#define CONFIG_RBAMP_SCT_RATING_CORRECTION 0   /* library Kconfig off → hub-side re-apply of user CT models */
#endif
#include "rbamp_registers_v2.h"   /* VERSION / PRODUCT_ID / UID — scan identity reads */

static const char *TAG = "RBAMP_BRIDGE";

/* Two I2C buses (the ESP32's two controllers): bus 0 = cfg sda/scl_gpio on cfg i2c_port, bus 1 = cfg bus1_* on the
 * other port. A module's cfg .bus picks its handle (modules may sit on two buses or share one). One mutex
 * (s_bus_lock) still serializes ALL bus traffic — the poll is one task and the driver is not proven for two
 * controllers in parallel. */
/** @brief Number of I2C buses (ESP32 I2C controllers) the bridge can drive. */
#define RBAMP_MAX_BUSES 2
static rbamp_bridge_config_t   s_cfg;
static i2c_master_bus_handle_t s_bush[RBAMP_MAX_BUSES] = { NULL };
/** @brief Handle of bus 0 (the primary sensor bus). */
#define s_bus (s_bush[0])
static inline i2c_master_bus_handle_t mod_bus(uint8_t m)
{
    return s_bush[s_cfg.modules[m].bus < RBAMP_MAX_BUSES ? s_cfg.modules[m].bus : 0];
}
static rbamp_handle_t          s_dev[RBAMP_MAX_MODULES] = { NULL };
static uint8_t                 s_dev_count = 0;
static TaskHandle_t            s_task     = NULL;
static volatile bool           s_running  = false;
/* Guards ALL shared-bus access (poll task + rbbench verbs) — concurrent
 * i2c_master access wedges the IDF driver in a bus-busy spin. */
static SemaphoreHandle_t       s_bus_lock = NULL;

/** Take the bus mutex (blocks until free). The bus can be held 10-14 s by one call (module OTA
 * begin/commit/activate). A waiter that is subscribed to the task watchdog (rbpwr_stats runs the quarter
 * collect: TWDT 5 s, panic) must not block that long in one piece — a quarter boundary inside a module
 * activation would reset the hub. Wait in 1-s slices and feed the watchdog between them when the calling task is
 * subscribed. No-op before the bridge has created the lock. */
static inline void bus_take(void)
{
    if (!s_bus_lock) return;
    while (xSemaphoreTake(s_bus_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    }
}
/** Release the bus mutex taken by bus_take(). */
static inline void bus_give(void) { if (s_bus_lock) xSemaphoreGive(s_bus_lock); }

/* ── Host-side period energy for CURRENT-ONLY modules ──────────────────────────
 * Current-only rbAmp variants (I2/I3, voltage_hw=no) physically cannot compute
 * P_real on-device — power needs voltage, and PERIOD_AVG_P[1..2] are documented
 * "UI2/UI3 only" (voltage-bearing) → PERIOD_VALID never sets, on-device period is
 * impossible. Canonical fleet model: one voltage-bearing
 * module supplies mains voltage, current-only channels get P = V_bus × I integrated
 * HOST-SIDE over the wall-clock window. Caveat: current-only modules have no phase
 * reference → no PF; we treat P ≈ V·I (apparent ≈ active — exact for resistive loads,
 * an over-estimate for reactive). Make PF configurable per load later if needed. */
static bool s_has_vhw[RBAMP_MAX_MODULES] = { false }; /* voltage-hw flag, cached at probe */

/* ══ Virtual STD module (debug builds with CONFIG_RBGRID_BENCH_FULL only: exercises the counter/anchor/reboot-gap
 * path on a hub without real hardware) ══
 * A fleet slot can be driven by a software module: live snapshot from setpoints (V per phase, signed
 * P per channel), life-time counters integrated on the monotonic timer, period snapshot like the
 * library's (>= 30 s between consumed reads), generation / uptime semantics. State lives in RTC
 * memory → survives esp_restart (the "module keeps counting while the hub is down" case: the downtime
 * is credited from the wall clock on the first tick after boot). A cold boot clears it. Every library
 * call the bridge makes goes through the br_*() shims below, which route virtual slots here. */
#ifndef CONFIG_RBGRID_BENCH_FULL
/** @brief Fallback when the debug Kconfig option is absent: virtual modules disabled. */
#define CONFIG_RBGRID_BENCH_FULL 0
#endif
/** @brief State of one virtual STD module (RTC_NOINIT memory, survives esp_restart). */
typedef struct {
    uint32_t magic;               ///< VIRT_MAGIC when the slot holds valid state
    bool     on;                  ///< slot is driven by the virtual module
    uint8_t  nch;                 ///< current channels 1..3 (3 → U3I3-like: 3 voltage inputs)
    float    v[3];                ///< phase voltages, V
    float    p[3];                ///< channel power setpoints, W (signed: <0 = export)
    float    pf;                  ///< reported power factor
    double   imp_mwh[3], exp_mwh[3];   ///< life-time import / export counters per channel, mWh
    uint32_t generation;          ///< counter generation (bumped on `vmod` counter reset)
    int64_t  uptime_acc_s;        ///< module uptime = uptime_acc_s + hub esp_timer seconds
    uint32_t last_uptime_s;       ///< uptime at the last tick (restored into uptime_acc_s after a hub reboot)
    uint32_t last_wall_s;         ///< wall clock at the last tick (downtime credit after a hub reboot)
    double   latch_imp[3], latch_exp[3];   ///< counters at the last period latch, mWh
    uint32_t latch_wall_s;        ///< fallback period reference across a reboot
    uint8_t  ct_code[3];          ///< CT model code per channel
    bool     restarted_pending;   ///< `vmod restart` → next period snapshot reports module_restarted
} virt_mod_t;
/** @var virt_mod_t::imp_mwh
 *  Life-time import counter per channel (mWh).  @var virt_mod_t::latch_imp
 *  Import counter at the last period latch (mWh). */
#define VIRT_MAGIC 0x564D4F44u    ///< "VMOD": marks a valid virt_mod_t
static RTC_NOINIT_ATTR virt_mod_t s_virt[RBAMP_MAX_MODULES];
static int64_t  s_virt_last_us[RBAMP_MAX_MODULES];      /* RAM: integration timestamp (0 after boot) */
static int64_t  s_virt_latch_us[RBAMP_MAX_MODULES];     /* RAM: last consumed period read */
static uint8_t  s_virt_dummy[RBAMP_MAX_MODULES];        /* s_dev[m] points here while virtual */

static inline bool virt_is(uint8_t m)
{
    return CONFIG_RBGRID_BENCH_FULL && m < RBAMP_MAX_MODULES && s_virt[m].magic == VIRT_MAGIC && s_virt[m].on;
}

/** Advance a virtual module's counters to now (first tick after a boot credits the downtime from the wall clock). */
static void virt_tick(uint8_t m)
{
    virt_mod_t *v = &s_virt[m];
    int64_t now = esp_timer_get_time();
    uint32_t wall = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    double dt_h;
    if (s_virt_last_us[m] == 0) {
        /* first tick after a hub boot: the module kept counting — credit the downtime from the wall clock */
        v->uptime_acc_s = (int64_t)v->last_uptime_s;
        dt_h = (wall && v->last_wall_s && wall > v->last_wall_s && wall - v->last_wall_s < 24u * 3600u)
               ? (double)(wall - v->last_wall_s) / 3600.0 : 0.0;
        if (dt_h > 0.0) v->uptime_acc_s += (int64_t)(dt_h * 3600.0);
        ESP_LOGW(TAG, "virtual module %u: resumed from RTC (gen=%lu imp=%.3f exp=%.3f Wh), downtime credited %.0f s",
                 m, (unsigned long)v->generation,
                 (v->imp_mwh[0] + v->imp_mwh[1] + v->imp_mwh[2]) / 1000.0,
                 (v->exp_mwh[0] + v->exp_mwh[1] + v->exp_mwh[2]) / 1000.0, dt_h * 3600.0);
    } else {
        dt_h = (double)(now - s_virt_last_us[m]) / 3.6e9;
    }
    for (uint8_t c = 0; c < v->nch && c < 3; ++c) {
        double e = (double)v->p[c] * dt_h * 1000.0;   /* mWh */
        if (e >= 0) v->imp_mwh[c] += e; else v->exp_mwh[c] += -e;
    }
    s_virt_last_us[m] = now;
    v->last_uptime_s = (uint32_t)(v->uptime_acc_s + now / 1000000LL);
    if (wall) v->last_wall_s = wall;
}

static esp_err_t virt_read_all(uint8_t m, rbamp_snapshot_t *out)
{
    virt_tick(m);
    const virt_mod_t *v = &s_virt[m];
    memset(out, 0, sizeof(*out));
    out->voltage = v->v[0]; out->voltage_peak = v->v[0] * 1.414f;
    for (uint8_t c = 0; c < 3; ++c) {
        if (c < v->nch) {
            float vv = (v->nch == 3) ? v->v[c] : v->v[0];
            out->power[c] = v->p[c];
            out->current[c] = (vv > 1.0f) ? fabsf(v->p[c]) / vv / (v->pf > 0 ? v->pf : 1.0f) : 0.0f;
            out->current_peak[c] = out->current[c] * 1.414f;
            out->power_factor[c] = (v->p[c] >= 0) ? v->pf : -v->pf;
        } else { out->current[c] = NAN; out->current_peak[c] = NAN; out->power[c] = NAN; out->power_factor[c] = NAN; }
    }
    out->frequency = 50.0f;
    out->topology = (v->nch == 3) ? RBAMP_TOPOLOGY_THREE_PHASE : (v->nch == 2) ? RBAMP_TOPOLOGY_SPLIT_PHASE : RBAMP_TOPOLOGY_SINGLE;
    out->channels = v->nch; out->has_voltage_hw = true; out->implausible = 0;
    return ESP_OK;
}

static esp_err_t virt_read_energy(uint8_t m, rbamp_std_energy_t *out)
{
    virt_tick(m);
    const virt_mod_t *v = &s_virt[m];
    memset(out, 0, sizeof(*out));
    double imp = 0, exp_ = 0;
    for (uint8_t c = 0; c < v->nch && c < 3; ++c) {
        imp += v->imp_mwh[c]; exp_ += v->exp_mwh[c];
        out->ch_import_mwh[c] = (int64_t)v->imp_mwh[c]; out->ch_export_mwh[c] = (int64_t)v->exp_mwh[c];
    }
    out->act_import_mwh = (int64_t)imp; out->act_export_mwh = (int64_t)exp_; out->act_net_mwh = out->act_import_mwh - out->act_export_mwh;
    out->act_import_wh = out->act_import_mwh / 1000; out->act_export_wh = out->act_export_mwh / 1000; out->act_net_wh = out->act_import_wh - out->act_export_wh;
    /* rbamp 1.7: deltas/records run on the µWh fields — the virtual module publishes them too (from its mWh doubles) */
    for (uint8_t c = 0; c < v->nch && c < 3; ++c) {
        out->ch_import_uwh[c] = (int64_t)(v->imp_mwh[c] * 1000.0); out->ch_export_uwh[c] = (int64_t)(v->exp_mwh[c] * 1000.0);
    }
    out->act_import_uwh = (int64_t)(imp * 1000.0); out->act_export_uwh = (int64_t)(exp_ * 1000.0);
    out->act_net_uwh = out->act_import_uwh - out->act_export_uwh;
    out->fine = true;
    out->generation = v->generation; out->uptime_s = v->last_uptime_s;
    out->channels = v->nch; out->group_status = 0;
    return ESP_OK;
}

static esp_err_t virt_period_snapshot(uint8_t m, rbamp_period_snapshot_t *out, bool skip_latch)
{
    (void)skip_latch;
    virt_tick(m);
    virt_mod_t *v = &s_virt[m];
    int64_t now = esp_timer_get_time();
    memset(out, 0, sizeof(*out));
    uint32_t wall = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    double dt_s;
    if (s_virt_latch_us[m]) dt_s = (double)(now - s_virt_latch_us[m]) / 1e6;
    else if (wall && v->latch_wall_s && wall > v->latch_wall_s) dt_s = (double)(wall - v->latch_wall_s);   /* across a hub reboot */
    else dt_s = 0.0;
    if (dt_s < 30.0) { out->valid = false; return ESP_OK; }   /* same rule as the library: >= 30 s since the last latch */
    double net_mwh = 0;
    for (uint8_t c = 0; c < v->nch && c < 3; ++c) {
        double d_imp = v->imp_mwh[c] - v->latch_imp[c], d_exp = v->exp_mwh[c] - v->latch_exp[c];
        out->avg_p[c] = (float)((d_imp - d_exp) / 1000.0 / (dt_s / 3600.0));
        net_mwh += d_imp - d_exp;
        v->latch_imp[c] = v->imp_mwh[c]; v->latch_exp[c] = v->exp_mwh[c];
    }
    out->avg_p_total = (float)(net_mwh / 1000.0 / (dt_s / 3600.0));
    out->max_p = fabsf(out->avg_p[0]);
    out->latch_ms = out->master_dt_ms = (uint32_t)(dt_s * 1000.0);
    out->wh_net = net_mwh / 1000.0;
    out->valid = true;
    out->module_restarted = v->restarted_pending; v->restarted_pending = false;
    s_virt_latch_us[m] = now; v->latch_wall_s = wall;
    return ESP_OK;
}

static esp_err_t virt_std_info(uint8_t m, rbamp_std_info_t *out)
{
    const virt_mod_t *v = &s_virt[m];
    memset(out, 0, sizeof(*out));
    out->map_version = 0x36; out->capabilities = RBAMP_STD_CAP_ONDEVICE_ENERGY; out->status = RBAMP_STD_ST_CAL_OK;
    out->n_voltage = (v->nch == 3) ? 3 : 1; out->n_current = v->nch;
    out->variant = (v->nch == 3) ? 0x63 : 0x11; out->period_source = RBAMP_STD_PERIOD_COUNTERS;
    return ESP_OK;
}

/* ── shims: every library call the bridge makes on a module handle ── */
static esp_err_t br_read_all(uint8_t m, rbamp_snapshot_t *o);   /* below the ch3+ cache (senior modules) */
static inline esp_err_t br_read_energy(uint8_t m, rbamp_std_energy_t *o)
{ return virt_is(m) ? virt_read_energy(m, o) : rbamp_std_read_energy(s_dev[m], o); }
/* Module identity for the reboot-gap anchor: 96-bit chip UID (virtual slot: a fixed synthetic id). */
static inline esp_err_t br_read_uid(uint8_t m, uint8_t uid[12])
{
    if (virt_is(m)) { memset(uid, 0, 12); memcpy(uid, "VIRTMOD", 7); uid[7] = m; return ESP_OK; }
    return rbamp_read_uid(s_dev[m], uid);
}
static inline esp_err_t br_period_snapshot(uint8_t m, rbamp_period_snapshot_t *o, uint16_t settle_ms, bool skip_latch)
{ return virt_is(m) ? virt_period_snapshot(m, o, skip_latch) : rbamp_read_period_snapshot(s_dev[m], o, settle_ms, skip_latch); }
static inline esp_err_t br_std_info(uint8_t m, rbamp_std_info_t *o)
{ return virt_is(m) ? virt_std_info(m, o) : rbamp_std_info(s_dev[m], o); }
static inline esp_err_t br_std_read_voltage(uint8_t m, uint8_t vch, rbamp_std_voltage_t *o)
{
    if (!virt_is(m)) return rbamp_std_read_voltage(s_dev[m], vch, o);
    memset(o, 0, sizeof(*o)); o->u_rms = s_virt[m].v[vch < 3 ? vch : 0]; o->u_peak = o->u_rms * 1.414f; return ESP_OK;
}
static inline esp_err_t br_std_read_channel(uint8_t m, uint8_t c, rbamp_std_channel_t *o)
{
    if (!virt_is(m)) return rbamp_std_read_channel(s_dev[m], c, o);
    const virt_mod_t *v = &s_virt[m]; memset(o, 0, sizeof(*o));
    if (c >= v->nch) return ESP_ERR_INVALID_ARG;
    float vv = (v->nch == 3) ? v->v[c] : v->v[0];
    o->p = v->p[c]; o->s = fabsf(v->p[c]) / (v->pf > 0 ? v->pf : 1.0f); o->i_rms = (vv > 1.0f) ? o->s / vv : 0.0f;
    o->pf = (v->p[c] >= 0) ? v->pf : -v->pf; o->q = 0.0f; return ESP_OK;
}
static uint8_t s_sensor_class[RBAMP_MAX_MODULES];   /* REG_SENSOR_CLASS read at describe (0xFF unknown) */
static uint8_t s_line[RBAMP_MAX_MODULES];           /* 0 BASIC, 1 STD (descriptor) */
static inline bool ct_factory_paired(uint8_t m) { return m < RBAMP_MAX_MODULES && (s_sensor_class[m] == 2 || s_sensor_class[m] == 3); }
static inline esp_err_t br_set_ct_model(uint8_t m, uint8_t ch, uint8_t code)
{
    if (virt_is(m)) { if (ch < 3) s_virt[m].ct_code[ch] = code; return ESP_OK; }
    /* CT- series (CT shipped with the module, channel calibrated in pair): writing the SCT-013 class + a preset would
     * replace the paired calibration with a generic one — never */
    if (ct_factory_paired(m)) return ESP_ERR_NOT_SUPPORTED;
    /* only the channel model, only on an SCT- module (class 1). Writing REG_SENSOR_CLASS — even the same value —
     * clears the models of ch0..2 and the linearity curves: on a multichannel module, changing one channel would
     * reset the others. Class 0 = uncalibrated module: nothing to select, refuse. */
    if (s_sensor_class[m] != 1) return ESP_ERR_INVALID_STATE;
    return rbamp_set_ct_model_ch(s_dev[m], ch, code);
}
static inline esp_err_t br_read_ct_model(uint8_t m, uint8_t ch, uint8_t *o)
{
    if (virt_is(m)) { *o = (ch < 3) ? s_virt[m].ct_code[ch] : 0; return ESP_OK; }
    return rbamp_read_ct_model_ch(s_dev[m], ch, o);
}
static inline esp_err_t br_save_user_config(uint8_t m)
{ return virt_is(m) ? ESP_OK : rbamp_save_user_config(s_dev[m]); }
static float s_vbus = 0.0f;                           /* latest mains voltage (from a voltage module) */
/* Multi-voltage (U3I3) support. s_nv[m] = voltage channels the module publishes
 * (rbamp_std_info; 0 on BASIC/unknown). Per-phase live block read each poll for modules with
 * n_voltage > 1: voltages 1..n-1 (voltage 0 rides in the snapshot) and Q/PF per current channel. */
static uint8_t s_nv[RBAMP_MAX_MODULES];
static uint8_t s_nc[RBAMP_MAX_MODULES];
static float   s_u_ph[RBAMP_MAX_MODULES][3];   /* per-phase V (NaN = unknown) */
static float   s_ang_ph[RBAMP_MAX_MODULES][3];   /* per-phase voltage angle vs L1, ° (NaN = unknown; set on every U3I3 poll) */
static uint8_t s_phseq[RBAMP_MAX_MODULES];       /* phase sequence 0 n/a · 1 fwd · 2 rev · 3 abnormal */
static int64_t s_phseq_us[RBAMP_MAX_MODULES];
static bool    s_phseq_seen[RBAMP_MAX_MODULES];
static float   s_q_ph[RBAMP_MAX_MODULES][3];   /* per-channel reactive power (NaN = unknown) */

/* Per-module energy capability, decided at init by module type + fleet composition.
 * Tells us whether a W/Wh accumulator can exist for that module's units at all. */
/** @brief Per-module energy capability. */
typedef enum {
    RBAMP_ENERGY_NONE = 0,   ///< current-only AND no voltage source in the fleet → no W/Wh possible
    RBAMP_ENERGY_ONDEVICE,   ///< voltage-bearing module → on-device period (avg_p/energy)
    RBAMP_ENERGY_HOST,       ///< current-only + a fleet voltage source → host-side P = V_bus × I
} rbamp_energy_mode_t;
static rbamp_energy_mode_t s_energy_mode[RBAMP_MAX_MODULES] = { RBAMP_ENERGY_NONE };
/** @brief Host-side energy window of one current-only entry (guarded by s_hostacc_mux). */
typedef struct {
    double   energy_wh;     ///< Σ P·dt since window start
    float    p_max;         ///< peak P in the window, W
    float    i_min;         ///< min current in the window, A (INFINITY until first sample)
    float    i_max;         ///< max current in the window, A
    uint64_t win_start_us;  ///< window start (last reset/inject)
    uint64_t last_us;       ///< last sample time, 0 = not started
} host_acc_t;
static host_acc_t s_hostacc[RBAMP_MAX_MODULES][RBAMP_MAX_ENTRIES];
/* Guards s_hostacc against the poll task (host_accumulate) vs the stats/console task (collect_period
 * snapshot+reset). Short critical sections only — never held across I2C or inject. */
static portMUX_TYPE s_hostacc_mux = portMUX_INITIALIZER_UNLOCKED;

/* Channels 3..6 of a senior BASIC (UI5/UI7): rbamp_snapshot_t holds ch0-2 only, ch3+ live values come from the channel
 * window (rbamp_read_channel: select + data per field, ~150 ms for 4 channels — once per poll, not per DRDY window).
 * Read in the same bus section as the snapshot; the fields come from adjacent windows (not one coherent snapshot). */
/** @brief Live values of one channel >= 3 of a senior module. */
typedef struct { float i, ipk, p, pf; } br_chv_t;
/** @var br_chv_t::i
 *  RMS current (A).  @var br_chv_t::ipk
 *  Peak current (A).  @var br_chv_t::p
 *  Active power (W).  @var br_chv_t::pf
 *  Power factor. */
static br_chv_t s_chx[RBAMP_MAX_MODULES][RBAMP_MAX_CHANNELS];
static inline uint8_t ch_n(const rbamp_snapshot_t *s) { return s->channels < RBAMP_MAX_CHANNELS ? s->channels : RBAMP_MAX_CHANNELS; }
static inline float ch_i(uint8_t m, const rbamp_snapshot_t *s, uint8_t ch)
{ return ch < 3 ? (ch < s->channels ? s->current[ch] : NAN) : (ch < ch_n(s) ? s_chx[m][ch].i : NAN); }
static inline float ch_ipk(uint8_t m, const rbamp_snapshot_t *s, uint8_t ch)
{ return ch < 3 ? (ch < s->channels ? s->current_peak[ch] : NAN) : (ch < ch_n(s) ? s_chx[m][ch].ipk : NAN); }
static inline float ch_p(uint8_t m, const rbamp_snapshot_t *s, uint8_t ch)
{ return ch < 3 ? (ch < s->channels ? s->power[ch] : NAN) : (ch < ch_n(s) ? s_chx[m][ch].p : NAN); }
static inline float ch_pf(uint8_t m, const rbamp_snapshot_t *s, uint8_t ch)
{ return ch < 3 ? (ch < s->channels ? s->power_factor[ch] : NAN) : (ch < ch_n(s) ? s_chx[m][ch].pf : NAN); }
/* Senior module (UI5/UI7): rbamp_read_all fills ch0-2 (RBAMP_SNAPSHOT_CHANNELS; earlier library release candidates
 * overflowed the [3] arrays), ch3+ come from the channel window per channel. Bus held by the caller. */
static uint32_t s_rd_all_ms_max[RBAMP_MAX_MODULES], s_rd_sen_ms_max[RBAMP_MAX_MODULES], s_rd_all_ms_last[RBAMP_MAX_MODULES], s_rd_sen_ms_last[RBAMP_MAX_MODULES];
/* A channel is read only when a point is bound to it: users wire part of the channels in any order, an empty input
 * carries no point, and its readings (op-amp offset, pick-up, positive-window creep) are never used (nothing to
 * set on the module, the hub just skips the channel). */
static bool ch_bound(uint8_t m, uint8_t ch)
{
    const rbamp_module_cfg_t *mc = &s_cfg.modules[m];
    for (uint8_t e = 0; e < mc->entry_count && e < RBAMP_MAX_ENTRIES; ++e)
        if (mc->entries[e].source == ch && mc->entries[e].unit_id != 0) return true;
    return false;
}
/** Read channels >= RBAMP_SNAPSHOT_CHANNELS of a senior module into s_chx (bound channels only). Bus held by the caller. */
static void ch_read_senior(uint8_t m, const rbamp_snapshot_t *s)
{
    for (uint8_t ch = RBAMP_SNAPSHOT_CHANNELS; ch < ch_n(s); ++ch) {
        if (!ch_bound(m, ch)) { s_chx[m][ch] = (br_chv_t){ NAN, NAN, NAN, NAN }; continue; }
        vTaskDelay(1);   /* the module may stretch every byte under load (UI7: 4-8 s polls seen) — let IDLE run between channels */
        rbamp_channel_reading_t r;
        if (rbamp_read_channel(s_dev[m], ch, &r) == ESP_OK) s_chx[m][ch] = (br_chv_t){ r.i_rms, r.i_peak, r.p_real, r.pf };
        else s_chx[m][ch] = (br_chv_t){ NAN, NAN, NAN, NAN };
    }
}
/** rbamp_read_all (or the virtual module) + the senior-channel reads, with read-time statistics. Bus held by the caller. */
static esp_err_t br_read_all(uint8_t m, rbamp_snapshot_t *o)
{
    if (virt_is(m)) return virt_read_all(m, o);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = rbamp_read_all(s_dev[m], o);
    int64_t t1 = esp_timer_get_time();
    s_rd_all_ms_last[m] = (uint32_t)((t1 - t0) / 1000);
    if (s_rd_all_ms_last[m] > s_rd_all_ms_max[m]) s_rd_all_ms_max[m] = s_rd_all_ms_last[m];
    if (err == ESP_OK && o->channels > RBAMP_SNAPSHOT_CHANNELS) {
        ch_read_senior(m, o);
        s_rd_sen_ms_last[m] = (uint32_t)((esp_timer_get_time() - t1) / 1000);
        if (s_rd_sen_ms_last[m] > s_rd_sen_ms_max[m]) s_rd_sen_ms_max[m] = s_rd_sen_ms_last[m];
    }
    return err;
}

static float unit_grid_voltage(uint16_t unit_id);
/** Integrate P = V_grid × I for each current entry of a current-only module (called from the poll task every
 *  poll_period_ms). Uses the grid voltage of the unit's phase. */
static void host_accumulate(uint8_t m, const rbamp_snapshot_t *s)
{
    uint64_t now = esp_timer_get_time();
    float cg = s_cfg.current_gain;
    const rbamp_module_cfg_t *mc = &s_cfg.modules[m];
    for (uint8_t e = 0; e < mc->entry_count; ++e) {
        const rbamp_entry_cfg_t *en = &mc->entries[e];
        if (en->source == RBAMP_ENTRY_VOLTAGE || en->source == RBAMP_ENTRY_PHASES || en->unit_id == 0) continue;
        uint8_t ch = en->source;
        float ich = ch_i(m, s, ch);
        if (isnan(ich)) continue;
        float i = ich * cg;                           /* A */
        float v = unit_grid_voltage(en->unit_id);     /* grid voltage of the unit's phase */
        if (v <= 0.0f) continue;                      /* no mains voltage yet — skip until a V read lands */
        float p = v * i;                              /* W (apparent ≈ active, no PF) */
        host_acc_t *acc = &s_hostacc[m][e];
        /* Short critical section — this struct is read+reset from collect_period on the stats/console
         * task; the 8-byte energy_wh RMW + window fields must not tear. */
        taskENTER_CRITICAL(&s_hostacc_mux);
        if (acc->last_us != 0) {
            uint64_t dt_us = now - acc->last_us;
            /* > 60 s without a sample (unit unbound for hours then rebound, long bus outage): integrating would
             * credit P_now × the whole hole. Do not integrate across it, and keep it out of the window span. */
            if (dt_us > 60ULL * 1000000ULL) { acc->win_start_us += dt_us; dt_us = 0; }
            double dt_h = (double)dt_us / 3.6e9;  /* us → hours */
            acc->energy_wh += (double)p * dt_h;
            if (i < acc->i_min) acc->i_min = i;      /* current envelope (real-envelope) */
            if (i > acc->i_max) acc->i_max = i;
        } else {
            acc->win_start_us = now;                /* first sample opens the window */
            acc->i_min = i; acc->i_max = i;          /* seed the current envelope */
        }
        if (p > acc->p_max) acc->p_max = p;
        acc->last_us = now;
        taskEXIT_CRITICAL(&s_hostacc_mux);
    }
}

/* ── Reboot-gap energy reconciliation (without it, ≈135 s ≈ 38 Wh were lost per hub reboot in tests) ──
 * STD modules keep life-time counters on-device, but the library anchors its period delta in RAM at
 * rbamp_begin(), so the energy between the hub's last period read and the first read after a reboot
 * (reflash / OTA / API reboot) never reached the hub. Here the hub persists the module's counter
 * reading after every period collect (NVS "rbampan", key m<N>) and, after a boot, credits the gap
 * (persisted → first read) as one extra period result on the first collect. The library's
 * continuity rules apply (same generation, no counter going backwards); a gap over 24 h is dropped
 * (anchor re-seeded) to keep zone attribution sane. BASIC / latch-only modules answer
 * ESP_ERR_NOT_SUPPORTED to the counter read and are skipped quietly. */
/* ANC2 adds the module's chip UID: a reboot gap is credited only when the SAME module is back in the slot.
 * Otherwise a different module in the slot (e.g. a real module replacing a virtual one) gives a counter delta
 * against the old anchor of thousands of Wh that would be credited to the unit. ANC1 anchors (no UID) are
 * still accepted. */
/** @brief Reboot-gap anchor in RAM: the module's counters at the last period collect. */
typedef struct { uint32_t magic; uint32_t ts; rbamp_std_energy_t e; uint8_t uid[12]; } rbamp_anchor_t;   /* in RAM */
/** @var rbamp_anchor_t::magic
 *  RBAMP_ANCHOR_MAGIC*.  @var rbamp_anchor_t::ts
 *  Wall clock of the anchor (Unix s).  @var rbamp_anchor_t::e
 *  Module counters.  @var rbamp_anchor_t::uid
 *  Module chip UID (96 bit). */
/* ANC3 (rbamp library 1.7): the NVS blob is the hub's OWN fixed layout, not the library struct — 1.7 grew
 * rbamp_std_energy_t (µWh fields) and every stored ANC1/ANC2 blob stopped matching its size → "gap noanchor"
 * (the first reboot after the upgrade would lose its tail). Legacy blobs are decoded with a frozen copy of the
 * 1.6.0 struct below. */
#define RBAMP_ANCHOR_MAGIC3 0x414E4333u   ///< "ANC3": current NVS anchor layout (rbamp_anchor3_t)
#define RBAMP_ANCHOR_MAGIC  0x414E4332u   ///< "ANC2" — legacy (1.6 struct + UID)
#define RBAMP_ANCHOR_MAGIC1 0x414E4331u   ///< "ANC1" — legacy (1.6 struct, no UID)
#define ANC_CH 7                          ///< channel slots in the stored anchor layouts
/** @brief Frozen copy of rbamp_std_energy_t from rbamp-esp-idf 1.6.0 (decodes legacy ANC1/ANC2 blobs). Field
 *  meanings as in the library: active/reactive import/export/net counters in Wh, varh or mWh, the counter
 *  generation, module uptime (s), channel count, group status and per-channel import/export (mWh). */
typedef struct {
    int64_t act_import_wh, act_export_wh, act_net_wh, rea_import_varh, rea_export_varh;
    uint32_t generation, uptime_s;
    int64_t act_import_mwh, act_export_mwh, act_net_mwh;
    uint8_t channels, group_status;
    int64_t ch_import_mwh[ANC_CH], ch_export_mwh[ANC_CH];
} rbamp_energy16_t;                                   /* frozen: rbamp_std_energy_t of esp-idf-rbamp-1.6.0 */
/** @var rbamp_energy16_t::act_import_wh
 *  Active import (Wh).  @var rbamp_energy16_t::act_export_wh
 *  Active export (Wh).  @var rbamp_energy16_t::act_net_wh
 *  Active net (Wh).  @var rbamp_energy16_t::rea_import_varh
 *  Reactive import (varh).  @var rbamp_energy16_t::rea_export_varh
 *  Reactive export (varh).  @var rbamp_energy16_t::generation
 *  Counter generation.  @var rbamp_energy16_t::uptime_s
 *  Module uptime (s).  @var rbamp_energy16_t::act_import_mwh
 *  Active import (mWh).  @var rbamp_energy16_t::act_export_mwh
 *  Active export (mWh).  @var rbamp_energy16_t::act_net_mwh
 *  Active net (mWh).  @var rbamp_energy16_t::channels
 *  Channel count.  @var rbamp_energy16_t::group_status
 *  Group status.  @var rbamp_energy16_t::ch_import_mwh
 *  Per-channel import (mWh).  @var rbamp_energy16_t::ch_export_mwh
 *  Per-channel export (mWh). */
/** @brief Legacy ANC1 NVS anchor (1.6.0 struct, no UID). */
typedef struct { uint32_t magic; uint32_t ts; rbamp_energy16_t e; } rbamp_anchor1_t;
/** @var rbamp_anchor1_t::magic
 *  RBAMP_ANCHOR_MAGIC1.  @var rbamp_anchor1_t::ts
 *  Wall clock (Unix s).  @var rbamp_anchor1_t::e
 *  Counters. */
/** @brief Legacy ANC2 NVS anchor (1.6.0 struct + UID). */
typedef struct { uint32_t magic; uint32_t ts; rbamp_energy16_t e; uint8_t uid[12]; } rbamp_anchor2_t;
/** @var rbamp_anchor2_t::magic
 *  RBAMP_ANCHOR_MAGIC.  @var rbamp_anchor2_t::ts
 *  Wall clock (Unix s).  @var rbamp_anchor2_t::e
 *  Counters.  @var rbamp_anchor2_t::uid
 *  Module chip UID. */
/** @brief Current ANC3 NVS anchor: the hub's own fixed layout (independent of the library struct). */
typedef struct {
    uint32_t magic, ts, generation, uptime_s;
    int64_t imp_uwh, exp_uwh, imp_wh, exp_wh, rea_imp_varh, rea_exp_varh;
    int64_t ch_imp_uwh[ANC_CH], ch_exp_uwh[ANC_CH];
    uint8_t channels, group_status, fine, rsv;
    uint8_t uid[12];
} rbamp_anchor3_t;
/** @var rbamp_anchor3_t::magic
 *  RBAMP_ANCHOR_MAGIC3.  @var rbamp_anchor3_t::ts
 *  Wall clock (Unix s), 0 = not creditable.  @var rbamp_anchor3_t::generation
 *  Counter generation.  @var rbamp_anchor3_t::uptime_s
 *  Module uptime (s).  @var rbamp_anchor3_t::imp_uwh
 *  Active import (µWh).  @var rbamp_anchor3_t::exp_uwh
 *  Active export (µWh).  @var rbamp_anchor3_t::imp_wh
 *  Active import (Wh).  @var rbamp_anchor3_t::exp_wh
 *  Active export (Wh).  @var rbamp_anchor3_t::rea_imp_varh
 *  Reactive import (varh).  @var rbamp_anchor3_t::rea_exp_varh
 *  Reactive export (varh).  @var rbamp_anchor3_t::ch_imp_uwh
 *  Per-channel import (µWh).  @var rbamp_anchor3_t::ch_exp_uwh
 *  Per-channel export (µWh).  @var rbamp_anchor3_t::channels
 *  Channel count.  @var rbamp_anchor3_t::group_status
 *  Group status.  @var rbamp_anchor3_t::fine
 *  1 = µWh counters are exact.  @var rbamp_anchor3_t::rsv
 *  Reserved.  @var rbamp_anchor3_t::uid
 *  Module chip UID. */
/** Convert a legacy 1.6.0 energy record to the current rbamp_std_energy_t (µWh fields derived from mWh/Wh). */
static void anc_from16(const rbamp_energy16_t *o, rbamp_std_energy_t *e)
{
    memset(e, 0, sizeof(*e));
    e->act_import_wh = o->act_import_wh; e->act_export_wh = o->act_export_wh; e->act_net_wh = o->act_net_wh;
    e->rea_import_varh = o->rea_import_varh; e->rea_export_varh = o->rea_export_varh;
    e->generation = o->generation; e->uptime_s = o->uptime_s;
    e->act_import_mwh = o->act_import_mwh; e->act_export_mwh = o->act_export_mwh; e->act_net_mwh = o->act_net_mwh;
    e->channels = o->channels; e->group_status = o->group_status;
    e->act_import_uwh = o->act_import_mwh ? o->act_import_mwh * 1000 : o->act_import_wh * 1000000;
    e->act_export_uwh = o->act_export_mwh ? o->act_export_mwh * 1000 : o->act_export_wh * 1000000;
    e->act_net_uwh = e->act_import_uwh - e->act_export_uwh;
    for (int c = 0; c < ANC_CH && c < RBAMP_MAX_CHANNELS; ++c) {
        e->ch_import_mwh[c] = o->ch_import_mwh[c]; e->ch_export_mwh[c] = o->ch_export_mwh[c];
        e->ch_import_uwh[c] = o->ch_import_mwh[c] * 1000; e->ch_export_uwh[c] = o->ch_export_mwh[c] * 1000;
    }
    e->fine = false;
}
static uint8_t s_uid[RBAMP_MAX_MODULES][12];
static bool    s_uid_ok[RBAMP_MAX_MODULES];
#define RBAMP_GAP_MAX_S    (24u * 3600u)   ///< longest creditable reboot gap (s); a longer one is dropped
static rbamp_std_energy_t s_tail[RBAMP_MAX_MODULES];        /* pending reboot-gap delta per module */
static uint32_t           s_tail_anchor_ts[RBAMP_MAX_MODULES];
static bool               s_tail_valid[RBAMP_MAX_MODULES];
static bool               s_tail_arm_pending[RBAMP_MAX_MODULES];   /* boot counter read failed → arm at the first collect */
static int64_t            s_probe_us[RBAMP_MAX_MODULES];          /* when the module (re)joined — first-quarter staleness is expected */
/* Last reboot-gap outcome per module for the `etstat` verb (ESP_LOG is not on the console in these builds). */
/** @brief Last reboot-gap outcome of one module. */
typedef struct { uint8_t state; double wh; uint32_t gap_s; uint32_t anchor_ts; } rbamp_gap_diag_t;   /* 0 none 1 armed 2 credited 3 dropped 4 regen 5 noread 6 noanchor */
/** @var rbamp_gap_diag_t::state
 *  0 none, 1 armed, 2 credited, 3 dropped, 4 regen, 5 noread, 6 noanchor.  @var rbamp_gap_diag_t::wh
 *  Net gap energy (Wh).  @var rbamp_gap_diag_t::gap_s
 *  Gap length (s).  @var rbamp_gap_diag_t::anchor_ts
 *  Anchor wall clock (Unix s). */
static rbamp_gap_diag_t   s_gap_diag[RBAMP_MAX_MODULES];

int rbamp_bridge_gap_status(uint8_t m, char *buf, size_t len)
{
    if (m >= RBAMP_MAX_MODULES || !buf || len == 0) return 0;
    static const char *st[] = { "none", "armed", "credited", "dropped", "regen", "noread", "noanchor" };
    const rbamp_gap_diag_t *g = &s_gap_diag[m];
    return snprintf(buf, len, "gap m%u=%s wh=%.3f gap_s=%lu anchor_ts=%lu", (unsigned)m,
                    st[g->state > 6 ? 0 : g->state], g->wh, (unsigned long)g->gap_s, (unsigned long)g->anchor_ts);
}

static bool anchor_load(uint8_t m, rbamp_anchor_t *out);
static void srcid_check(uint8_t m, const char *where);
static void anchor_write(uint8_t m, const rbamp_std_energy_t *e, bool force);
static void anchor_save(uint8_t m, const rbamp_std_energy_t *e) { anchor_write(m, e, false); }
/** Persist the module's counters as the reboot-gap anchor (NVS rbampan/m<N>, ANC3 layout).
 *  force = write even with no wall clock over a usable anchor (module replaced: the old identity must go). */
static void anchor_write(uint8_t m, const rbamp_std_energy_t *e, bool force)
{
    nvs_handle_t h;
    if (nvs_open("rbampan", NVS_READWRITE, &h) != ESP_OK) return;
    rbamp_anchor3_t a; memset(&a, 0, sizeof(a));
    a.magic = RBAMP_ANCHOR_MAGIC3;
    a.ts    = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    a.generation = e->generation; a.uptime_s = e->uptime_s;
    a.imp_uwh = e->act_import_uwh; a.exp_uwh = e->act_export_uwh; a.imp_wh = e->act_import_wh; a.exp_wh = e->act_export_wh;
    a.rea_imp_varh = e->rea_import_varh; a.rea_exp_varh = e->rea_export_varh;
    for (int c = 0; c < ANC_CH && c < RBAMP_MAX_CHANNELS; ++c) { a.ch_imp_uwh[c] = e->ch_import_uwh[c]; a.ch_exp_uwh[c] = e->ch_export_uwh[c]; }
    a.channels = e->channels; a.group_status = e->group_status; a.fine = e->fine ? 1 : 0;
    if (s_uid_ok[m]) memcpy(a.uid, s_uid[m], 12);
    if (a.ts == 0 && !force) {   /* no wall clock yet (boot before SNTP): never overwrite a usable anchor with ts=0 */
        rbamp_anchor_t old;
        if (anchor_load(m, &old) && old.ts != 0) {
            ESP_LOGW(TAG, "module %u: time not valid — keeping the previous energy anchor (ts=%lu)", m, (unsigned long)old.ts);
            nvs_close(h);
            return;
        }
    }
    char key[8]; snprintf(key, sizeof(key), "m%u", (unsigned)m);
    esp_err_t se = nvs_set_blob(h, key, &a, sizeof(a));
    if (se == ESP_OK) se = nvs_commit(h);
    if (se != ESP_OK) {
        ESP_LOGE(TAG, "module %u: energy anchor NOT saved (%s) — a reboot now loses its gap energy", m, esp_err_to_name(se));
        rbpower_diag_printf("FAIL m%u anchor save: %s", m, esp_err_to_name(se));
    }
    nvs_close(h);
}

/** Load the module's anchor from NVS (ANC3, or legacy ANC2/ANC1 converted); false if none. */
static bool anchor_load(uint8_t m, rbamp_anchor_t *out)
{
    nvs_handle_t h;
    if (nvs_open("rbampan", NVS_READONLY, &h) != ESP_OK) return false;
    char key[8]; snprintf(key, sizeof(key), "m%u", (unsigned)m);
    union { rbamp_anchor3_t a3; rbamp_anchor2_t a2; rbamp_anchor1_t a1; uint8_t raw[sizeof(rbamp_anchor3_t) + 64]; } b;
    size_t len = sizeof(b);
    memset(&b, 0, sizeof(b)); memset(out, 0, sizeof(*out));
    bool ok = (nvs_get_blob(h, key, &b, &len) == ESP_OK);
    nvs_close(h);
    if (!ok) return false;
    if (len == sizeof(rbamp_anchor3_t) && b.a3.magic == RBAMP_ANCHOR_MAGIC3) {
        rbamp_std_energy_t *e = &out->e;
        e->generation = b.a3.generation; e->uptime_s = b.a3.uptime_s;
        e->act_import_uwh = b.a3.imp_uwh; e->act_export_uwh = b.a3.exp_uwh; e->act_net_uwh = b.a3.imp_uwh - b.a3.exp_uwh;
        e->act_import_mwh = b.a3.imp_uwh / 1000; e->act_export_mwh = b.a3.exp_uwh / 1000; e->act_net_mwh = e->act_import_mwh - e->act_export_mwh;
        e->act_import_wh = b.a3.imp_wh; e->act_export_wh = b.a3.exp_wh; e->act_net_wh = b.a3.imp_wh - b.a3.exp_wh;
        e->rea_import_varh = b.a3.rea_imp_varh; e->rea_export_varh = b.a3.rea_exp_varh;
        for (int c = 0; c < ANC_CH && c < RBAMP_MAX_CHANNELS; ++c) {
            e->ch_import_uwh[c] = b.a3.ch_imp_uwh[c]; e->ch_export_uwh[c] = b.a3.ch_exp_uwh[c];
            e->ch_import_mwh[c] = b.a3.ch_imp_uwh[c] / 1000; e->ch_export_mwh[c] = b.a3.ch_exp_uwh[c] / 1000;
        }
        e->channels = b.a3.channels; e->group_status = b.a3.group_status; e->fine = b.a3.fine != 0;
        out->magic = RBAMP_ANCHOR_MAGIC3; out->ts = b.a3.ts; memcpy(out->uid, b.a3.uid, 12);
        return true;
    }
    if (len == sizeof(rbamp_anchor2_t) && b.a2.magic == RBAMP_ANCHOR_MAGIC) {      /* legacy ANC2 (1.6 struct + UID) */
        anc_from16(&b.a2.e, &out->e); out->magic = b.a2.magic; out->ts = b.a2.ts; memcpy(out->uid, b.a2.uid, 12);
        return true;
    }
    if (len == sizeof(rbamp_anchor1_t) && b.a1.magic == RBAMP_ANCHOR_MAGIC1) {     /* legacy ANC1: UID unknown (zeros) */
        anc_from16(&b.a1.e, &out->e); out->magic = b.a1.magic; out->ts = b.a1.ts;
        return true;
    }
    return false;
}

/** After probe: compare the module's counters with the persisted anchor → pending tail. Takes the bus lock.
 * A transient counter-read failure right after probe must not drop the whole gap silently (that cost a few
 * percent of an hour's energy with no trace in `etstat`): the read is retried, and if it still
 * fails the arming is DEFERRED to the first period collect — the NVS anchor is left untouched until a
 * reading succeeds, so nothing is lost. Every outcome is visible in `etstat`. */
static void boot_tail_arm(uint8_t m)
{
    if (m >= RBAMP_MAX_MODULES || !s_dev[m]) return;
    rbamp_std_energy_t now;
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(50 * attempt));
        bus_take();
        e = br_read_energy(m, &now);
        bus_give();
        if (e == ESP_OK || e == ESP_ERR_NOT_SUPPORTED) break;
    }
    if (e == ESP_ERR_NOT_SUPPORTED) { s_tail_arm_pending[m] = false; return; }   /* BASIC / latch-only module */
    if (e != ESP_OK) {
        s_tail_arm_pending[m] = true;
        s_gap_diag[m].state = 5;
        ESP_LOGW(TAG, "module %u: counter read failed at boot (%s) — reboot-gap arming deferred to the first period collect",
                 m, esp_err_to_name(e));
        rbpower_diag_printf("FAIL m%u gap arm: counter read %s — deferred to first collect", m, esp_err_to_name(e));
        return;
    }
    s_tail_arm_pending[m] = false;
    {   /* identity of the module now in the slot */
        bus_take();
        s_uid_ok[m] = (br_read_uid(m, s_uid[m]) == ESP_OK);
        bus_give();
    }
    rbamp_anchor_t a;
    bool have_anchor = anchor_load(m, &a);
    static const uint8_t ZERO_UID[12] = {0};
    if (have_anchor && a.ts != 0 && s_uid_ok[m] && memcmp(a.uid, ZERO_UID, 12) != 0 && memcmp(a.uid, s_uid[m], 12) != 0) {
        /* another module now sits in this slot: its counters have nothing to do with the anchor */
        s_gap_diag[m].state = 4; s_gap_diag[m].anchor_ts = a.ts; s_gap_diag[m].wh = 0;
        ESP_LOGW(TAG, "module %u: module REPLACED in the slot (UID changed) — reboot gap not credited, re-anchoring", m);
        rbpower_diag_printf("FAIL m%u gap: module replaced in the slot (uid %02x%02x%02x%02x… → %02x%02x%02x%02x…) — gap not credited, re-anchored",
                            m, a.uid[0], a.uid[1], a.uid[2], a.uid[3], s_uid[m][0], s_uid[m][1], s_uid[m][2], s_uid[m][3]);
        anchor_write(m, &now, true);   /* new identity even with the clock unset (ts 0 = not creditable later) */
        return;
    }
    if (!have_anchor || a.ts == 0) {
        s_gap_diag[m].state = 6; s_gap_diag[m].anchor_ts = have_anchor ? a.ts : 0;
        ESP_LOGW(TAG, "module %u: no usable energy anchor in NVS (%s) — reboot gap not creditable, anchoring now",
                 m, have_anchor ? "ts=0" : "absent");
        rbpower_diag_printf("m%u gap noanchor (%s)", m, have_anchor ? "ts=0" : "absent");
    }
    if (have_anchor && a.ts != 0) {
        rbamp_std_energy_t d; bool restarted = false;
        esp_err_t de = rbamp_std_energy_delta(&a.e, &now, &d, &restarted);
        if (de == ESP_OK && restarted && memcmp(a.uid, ZERO_UID, 12) == 0) {
            /* legacy anchor (no UID) + counters went BACKWARDS: a module restart OR a different module in the
             * slot — indistinguishable without identity, and the second would credit garbage (thousands of Wh) */
            s_gap_diag[m].state = 4; s_gap_diag[m].anchor_ts = a.ts; s_gap_diag[m].wh = 0;
            rbpower_diag_printf("FAIL m%u gap: counters went backwards and the anchor has no module UID — gap not credited, re-anchored", m);
        } else if (de == ESP_OK) {
            s_tail[m] = d; s_tail_anchor_ts[m] = a.ts; s_tail_valid[m] = true;
            s_gap_diag[m].state = 1; s_gap_diag[m].anchor_ts = a.ts;
            s_gap_diag[m].wh = ((double)d.act_import_uwh - (double)d.act_export_uwh) / 1e6;
            ESP_LOGW(TAG, "module %u: reboot-gap tail armed: +%lld/-%lld Wh (mWh %lld/%lld, ch=%u) since ts=%lu%s",
                     m, (long long)d.act_import_wh, (long long)d.act_export_wh,
                     (long long)d.act_import_mwh, (long long)d.act_export_mwh, (unsigned)d.channels,
                     (unsigned long)a.ts, restarted ? " (module restarted inside)" : "");
            rbpower_diag_printf("m%u gap armed wh=%.3f anchor_ts=%lu%s", m, s_gap_diag[m].wh, (unsigned long)a.ts,
                                restarted ? " module-restarted" : "");
            /* keep the NVS anchor until the tail is credited: re-anchoring here and resetting again before the
             * first collect (panic, watchdog, boot loop, planned restart < 30 s) would lose the downtime energy for
             * good — the armed tail lives only in RAM. The first collect saves it. */
            return;
        } else {
            ESP_LOGW(TAG, "module %u: counter generation changed — reboot gap dropped, re-anchoring", m);
            s_gap_diag[m].state = 4; s_gap_diag[m].anchor_ts = a.ts;
            rbpower_diag_printf("FAIL m%u gap regen: counter generation changed — gap energy dropped", m);
        }
    }
    anchor_save(m, &now);                                     /* the library anchored here too */
}

/* ── Reconciliation trace (shows errors during accumulation, not hours later) ──
 * Per module: the hub integrates the module's live power (Σ channels × dt on the monotonic timer) and,
 * every rbpower_diag_period_s(), reads the module's life-time counters and prints both since the last
 * reference (quarter start): "m0 +120s cnt=+34.1Wh int=+34.3Wh dev=+0.5%". A deviation > 2 % on > 5 Wh,
 * a counter generation change, a read failure or a stale period snapshot are printed as FAIL lines.
 * No wall clock involved; costs one extra counter burst per period. */
/* the module restarted (firmware update / reset) inside the open hour → q SRC_RESTART on its units' hour records */
static void flag_module_hour_bits(uint8_t m, uint16_t bits);
static esp_err_t rbamp_bridge_unit_set_ct(rbpower_unit_id_t unit_id, uint8_t ct_code);   /* below, registered in start */
static void reidentify_module(uint8_t m);   /* below: module swapped on the fly? */
/* DRDY (rbamp-esp-idf rbamp_set_drdy_gpio / rbamp_read_all_ready): one coherent read per published
 * snapshot instead of a read at an arbitrary moment (a slow BASIC read straddled several publishes). Stats for `drdy`. */
static uint32_t s_drdy_edge[RBAMP_MAX_MODULES], s_drdy_poll[RBAMP_MAX_MODULES], s_drdy_timeout[RBAMP_MAX_MODULES], s_drdy_missed[RBAMP_MAX_MODULES];
/** Attach the module's DRDY GPIO in the library (its own line, else the fleet line, else none). */
static void drdy_attach(uint8_t m)
{
    if (virt_is(m) || !s_dev[m]) return;
    /* a module's own DRDY line (e.g. a second module on bus 1 → GPIO19) wins over the fleet's wired-OR line */
    int gpio = s_cfg.modules[m].drdy_p1 ? (int)s_cfg.modules[m].drdy_p1 - 1 : s_cfg.drdy_gpio_p1 ? (int)s_cfg.drdy_gpio_p1 - 1 : -1;
    esp_err_t e = rbamp_set_drdy_gpio(s_dev[m], gpio);
    if (gpio >= 0) rbpower_diag_printf("m%u DRDY on GPIO%d: %s", m, gpio, esp_err_to_name(e));
}
static volatile bool s_collect_req;                              /* set by the stats task at the quarter boundary */
static volatile rbpower_stat_period_type_t s_collect_period = RBPOWER_STAT_PERIOD_15MIN;
static volatile uint32_t s_collect_end_req;                      /* boundary (unix s) of the requested collect, 0 = unknown */
static uint32_t s_collect_q_end;                                 /* set only while the poll task runs that collect */
void rbamp_bridge_request_collect(rbpower_stat_period_type_t period, uint32_t end_ts)
{
    s_collect_period = period; s_collect_end_req = end_ts; s_collect_req = true;
}
#define RBAMP_BASIC_RESUME_LOG_MS (20u * 60u * 1000u)   ///< a first BASIC window longer than this (ms; a quarter + margin) = recovered downtime
static void describe_module(uint8_t m, rbamp_handle_t dev);
static void flag_module_hour(uint8_t m) { flag_module_hour_bits(m, RBSTATS_Q_SRC_RESTART); }
/* The open hour's extra q bits live in RAM until the hour closes; a hub reboot later in the same hour would drop
 * them (srcid already stored the new BOOT_COUNT, so the join check sees no change). Keep them in NVS
 * (rbampan/h<m> = {hour start, bits}) and re-apply at join when the hub comes back inside that hour. */
/** @brief Open-hour quality bits of one module persisted in NVS (key rbampan/h + module index). */
typedef struct { uint32_t hour; uint16_t bits; uint16_t pad; } rbamp_hourq_t;
/** @var rbamp_hourq_t::hour
 *  Hour start (Unix s).  @var rbamp_hourq_t::bits
 *  RBSTATS_Q_* bits.  @var rbamp_hourq_t::pad
 *  Padding. */
static void hourq_apply(uint8_t m, uint16_t bits)
{
    for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
        const rbamp_entry_cfg_t *en = &s_cfg.modules[m].entries[e];
        if (en->source != RBAMP_ENTRY_VOLTAGE && en->unit_id != 0) rbpower_stats_flag_open_hour(en->unit_id, bits);
    }
}
static bool s_hourq_done[RBAMP_MAX_MODULES];   /* restored once per boot (at join, or the first quarter if no time yet) */
/** Re-apply the persisted open-hour q bits once per boot when the hub is back inside the same hour. */
static void hourq_restore(uint8_t m)
{
    if (s_hourq_done[m] || !rbtime_is_time_valid()) return;
    s_hourq_done[m] = true;
    uint32_t now = (uint32_t)rbtime_get_unix_timestamp();
    nvs_handle_t h;
    if (nvs_open("rbampan", NVS_READONLY, &h) != ESP_OK) return;
    char key[8]; snprintf(key, sizeof(key), "h%u", (unsigned)m);
    rbamp_hourq_t q = {0}; size_t len = sizeof(q);
    bool ok = nvs_get_blob(h, key, &q, &len) == ESP_OK && len == sizeof(q);
    nvs_close(h);
    if (ok && q.bits && q.hour == now - now % 3600u) {
        hourq_apply(m, q.bits);
        rbpower_diag_printf("m%u open hour q bits 0x%04X restored after hub restart", m, (unsigned)q.bits);
    }
}
/** Flag q bits on the open hour of every unit of module m and persist them for this hour. */
static void flag_module_hour_bits(uint8_t m, uint16_t bits)
{
    if (rbtime_is_time_valid()) {
        uint32_t now = (uint32_t)rbtime_get_unix_timestamp(), hr = now - now % 3600u;
        nvs_handle_t h;
        if (nvs_open("rbampan", NVS_READWRITE, &h) == ESP_OK) {
            char key[8]; snprintf(key, sizeof(key), "h%u", (unsigned)m);
            rbamp_hourq_t q = {0}; size_t len = sizeof(q);
            if (nvs_get_blob(h, key, &q, &len) != ESP_OK || len != sizeof(q) || q.hour != hr) { q.hour = hr; q.bits = 0; }
            if ((q.bits | bits) != q.bits) {
                q.bits |= bits;
                if (nvs_set_blob(h, key, &q, sizeof(q)) == ESP_OK) nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    hourq_apply(m, bits);
}
static int64_t            s_diag_last_us[RBAMP_MAX_MODULES];
static int64_t            s_diag_tick_us[RBAMP_MAX_MODULES];
static int64_t            s_diag_ref_us[RBAMP_MAX_MODULES];
static double             s_diag_int_wh[RBAMP_MAX_MODULES];
static double             s_diag_last_p[RBAMP_MAX_MODULES];      /* last polled module power, W (closes the gap to the counter read) */
static rbamp_std_energy_t s_diag_ref[RBAMP_MAX_MODULES];
static bool               s_diag_ref_valid[RBAMP_MAX_MODULES];
static uint8_t            s_diag_holes[RBAMP_MAX_MODULES];
static int64_t            s_planned_restart_us[RBAMP_MAX_MODULES];  /* module OTA activation time: a restart seen within 180 s of it is planned */
static int64_t            s_poll_pause_until_us[RBAMP_MAX_MODULES];
/* Slow-poll forensics (spontaneous 5–11 s poll holes without any failed read_all were seen on a live 3-phase install).
 * Per loop iteration: time spent per stage; an iteration over 2 s prints where it went and the per-phase
 * I²C error counts. Stages: hk = housekeeping (home lock), bus = waiting for the I²C bus lock, rd = read_all,
 * ph = per-phase U/Q reads, inj = 200 ms inject (home lock), other = the rest. */
static int64_t s_pt_hk, s_pt_bus, s_pt_rd, s_pt_ph, s_pt_inj;
static uint16_t s_pt_ph_err;
#define PT_T0() int64_t _pt0 = esp_timer_get_time()   ///< start a stage timer (declares _pt0)
#define PT_ADD(acc) do { (acc) += esp_timer_get_time() - _pt0; } while (0)   ///< add the stage time (µs) to acc
/* p_min for the period record ("home vampires" — standby base load): minimum of a 10-s ROLLING MEAN of the
 * polled power within the quarter (index 3 = channel sum, for PHASES entries) — not a single sample and not
 * the quarter average. NAN = fewer than 10 samples. Reset after every collected quarter. */
#define RBAMP_PMIN_WIN 10             ///< rolling-mean length (polls) for p_min / p_max
#define PMIN_SUM RBAMP_MAX_CHANNELS   ///< ring index of the channel sum (PHASES entries); 0..6 = channels
static float              s_pmin_ring[RBAMP_MAX_MODULES][PMIN_SUM + 1][RBAMP_PMIN_WIN];
static uint8_t            s_pmin_n[RBAMP_MAX_MODULES], s_pmin_i[RBAMP_MAX_MODULES];
static float              s_pmin_q[RBAMP_MAX_MODULES][PMIN_SUM + 1];   /* NAN-filled at bridge start (pmin_reset) */
static float              s_pmax_q[RBAMP_MAX_MODULES][PMIN_SUM + 1];   /* same window, maximum */
static void pmin_reset(uint8_t m) { for (int k = 0; k <= PMIN_SUM; ++k) { s_pmin_q[m][k] = NAN; s_pmax_q[m][k] = NAN; } }   /* the ring keeps rolling across quarters */
/** Feed one poll into the 10-s rolling mean and update the quarter's p_min / p_max. */
static void pmin_feed(uint8_t m, const rbamp_snapshot_t *s)
{
    float sum = 0.0f;
    for (uint8_t c = 0; c < RBAMP_MAX_CHANNELS; ++c) {
        float p = ch_p(m, s, c);
        if (isnan(p)) p = 0.0f;
        s_pmin_ring[m][c][s_pmin_i[m]] = p; sum += p;
    }
    s_pmin_ring[m][PMIN_SUM][s_pmin_i[m]] = sum;
    s_pmin_i[m] = (uint8_t)((s_pmin_i[m] + 1) % RBAMP_PMIN_WIN);
    if (s_pmin_n[m] < RBAMP_PMIN_WIN) { s_pmin_n[m]++; if (s_pmin_n[m] < RBAMP_PMIN_WIN) return; }
    for (int k = 0; k <= PMIN_SUM; ++k) {
        float acc = 0.0f;
        for (int j = 0; j < RBAMP_PMIN_WIN; ++j) acc += s_pmin_ring[m][k][j];
        float mean = acc / RBAMP_PMIN_WIN;
        if (isnan(s_pmin_q[m][k]) || mean < s_pmin_q[m][k]) s_pmin_q[m][k] = mean;
        if (isnan(s_pmax_q[m][k]) || mean > s_pmax_q[m][k]) s_pmax_q[m][k] = mean;
    }
}
static uint32_t           s_poll_fail_n[RBAMP_MAX_MODULES];       /* consecutive failed polls (bus break visibility on #DIAG) */
static uint8_t            s_env_skip[RBAMP_MAX_MODULES];          /* polls after a restore kept out of the p_min/p_max envelope */
static int64_t            s_poll_fail_t0[RBAMP_MAX_MODULES], s_poll_fail_last_us[RBAMP_MAX_MODULES]; /* start / last time (µs) of the current run of failed polls */

/** Restart the reconciliation reference (counters, integral, hole count) at now. */
static void diag_ref_reset(uint8_t m, const rbamp_std_energy_t *e)
{
    s_diag_ref[m] = *e; s_diag_ref_valid[m] = true;
    s_diag_ref_us[m] = esp_timer_get_time(); s_diag_int_wh[m] = 0.0; s_diag_holes[m] = 0;
    /* the integral restarts at the reference instant too — otherwise the next poll integrates from the
     * PREVIOUS poll (one poll period before the counter reference) and int > cnt by P × ~1 s, which is
     * about +2.6 % on a 28-s window */
    s_diag_last_us[m] = s_diag_ref_us[m];
    s_diag_tick_us[m] = s_diag_ref_us[m];      /* periodic line restarts from the reference too (no "+1s" line right after a QTR) */
}

static double diag_int_to_now(uint8_t m)
{
    int64_t now = esp_timer_get_time();
    double extra = (s_diag_last_us[m] && s_has_vhw[m]) ? s_diag_last_p[m] * ((double)(now - s_diag_last_us[m]) / 3.6e9) : 0.0;
    return s_diag_int_wh[m] + extra;
}

static double diag_cnt_wh(const rbamp_std_energy_t *d)
{
    return ((double)d->act_import_uwh - (double)d->act_export_uwh) / 1e6;   /* µWh (rbamp 1.7; mWh·1000 when not fine) */
}

/* CT limit / clipping (the sensor nominal is a limit; rbAmp BASIC clip-detection rule):
 *   near   I_rms ≥ 0.9 × fs        (fs = RMS where a sine starts to clip — SCT-013 clips BELOW its nominal on BASIC)
 *   clip   I_rms ≥ fs, or I_peak ≥ 0.97 × peak in ≥ 3 of the last 5 polls (distorted loads; one spike is nothing)
 * Clipped windows go into the module's period average as they are — the energy is under-stated silently — so the
 * open hour gets q OVERRANGE, the journal gets one line per episode, telemetry gets load_pct / ct_warn / clip. */
static uint8_t s_ct_state[RBAMP_MAX_MODULES][RBAMP_MAX_ENTRIES];
static uint8_t s_ct_peak_hits[RBAMP_MAX_MODULES][RBAMP_MAX_ENTRIES];   /* last 5 polls, bit per poll */
static uint8_t s_rev_hits[RBAMP_MAX_MODULES][RBAMP_MAX_ENTRIES];       /* BASIC: P below −1.5 % of nominal × U, last 5 polls */
static bool    s_rev_state[RBAMP_MAX_MODULES][RBAMP_MAX_ENTRIES];
/** Called on every successful poll read (bus NOT held): CT load %, near-limit / clipping and BASIC reverse-flow
 *  detection per entry; state changes become event / FAIL lines and open-hour q bits. */
static void ct_limit_check(uint8_t m, const rbamp_snapshot_t *s)
{
    if (virt_is(m) || s_sensor_class[m] == 0 || s_sensor_class[m] == 0xFF) return;
    const rbamp_module_cfg_t *mc = &s_cfg.modules[m];
    for (uint8_t e = 0; e < mc->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
        const rbamp_entry_cfg_t *en = &mc->entries[e];
        if (en->source == RBAMP_ENTRY_VOLTAGE || en->source == RBAMP_ENTRY_PHASES || en->unit_id == 0) continue;
        uint8_t ch = en->source;
        if (isnan(ch_i(m, s, ch))) continue;
        float nom = 0, fs = 0, pk = 0;
        if (!rbamp_sensor_model_limits(s_sensor_class[m], en->ct_code, &nom, &fs, &pk) || nom <= 0 || fs <= 0) {
            rbpower_set_unit_ct_load(en->unit_id, -1.0f, 0);
            continue;
        }
        float irms = ch_i(m, s, ch);   /* the module's own reading — what the front end sees and clips (no host gain) */
        float ipk  = ch_ipk(m, s, ch);
        if (isnan(ipk)) ipk = 0.0f;
        float pch  = ch_p(m, s, ch);
        s_ct_peak_hits[m][e] = (uint8_t)(((s_ct_peak_hits[m][e] << 1) | (pk > 0 && ipk >= 0.97f * pk ? 1 : 0)) & 0x1F);
        uint8_t hits = (uint8_t)__builtin_popcount(s_ct_peak_hits[m][e]);
        uint8_t st = (irms >= fs || hits >= 3) ? 2 : (irms >= 0.9f * fs) ? 1 : 0;
        if (s_line[m] == 0 && !isnan(pch) && !isnan(s->voltage)) {   /* BASIC: reversed CT → flow not accounted */
            bool neg = pch < -(0.015f * nom * s->voltage);
            s_rev_hits[m][e] = (uint8_t)(((s_rev_hits[m][e] << 1) | (neg ? 1 : 0)) & 0x1F);
            bool rev = __builtin_popcount(s_rev_hits[m][e]) >= 3;
            if (rev != s_rev_state[m][e]) {
                if (rev) rbpower_diag_printf("FAIL m%u ch%u reverse flow on a BASIC module: P=%.0fW — NOT accounted (BASIC meters consumption only; CT reversed?)",
                                             m, ch, (double)pch);
                else     rbpower_diag_printf("m%u ch%u reverse flow ended", m, ch);
                s_rev_state[m][e] = rev;
            }
            if (rev) { st |= 0x10; rbpower_stats_flag_open_hour(en->unit_id, RBSTATS_Q_REVERSE_LOST); }
        }
        rbpower_set_unit_ct_load(en->unit_id, irms / nom * 100.0f, st);
        uint8_t lim = st & 0x0F;
        if (lim != s_ct_state[m][e]) {
            if (lim == 2)
                rbpower_diag_printf("FAIL m%u ch%u CT clipping: I=%.2fA peak=%.2fA (clip from %.1fA rms / %.1fA peak, nominal %.1fA) — energy under-stated",
                                    m, ch, (double)irms, (double)ipk, (double)fs, (double)pk, (double)nom);
            else if (lim == 1)
                rbpower_diag_printf("m%u ch%u CT near its limit: I=%.2fA = %.0f%% of the clip point %.1fA (nominal %.1fA)",
                                    m, ch, (double)irms, (double)(irms / fs * 100.0f), (double)fs, (double)nom);
            else
                rbpower_diag_printf("m%u ch%u CT back within range: I=%.2fA", m, ch, (double)irms);
            s_ct_state[m][e] = lim;
        }
        if (lim == 2) rbpower_stats_flag_open_hour(en->unit_id, RBSTATS_Q_OVERRANGE);
    }
}

/* User-chosen CT models (entry.ct_user) on a BASIC SCT- module: a module restart keeps the model NUMBER but reloads the
 * shipped calibration (gain/NF are develop-gated, not saved in user mode) — e.g. a 100 A model chosen over the
 * shipped 30 A one would read ×0.29 after a restart. Recipe: clear RESET_OCCURRED first (a restart
 * during the write is not lost), then force-write the model. The library detects restarts by uptime, so the clear
 * does not hide one from the period path. Bus held by the caller. */
static uint8_t s_rst_poll[RBAMP_MAX_MODULES];
static bool ct_user_any(uint8_t m)
{
    const rbamp_module_cfg_t *mc = &s_cfg.modules[m];
    for (uint8_t e = 0; e < mc->entry_count && e < RBAMP_MAX_ENTRIES; ++e)
        if (mc->entries[e].ct_user && mc->entries[e].source < RBAMP_MAX_CHANNELS && mc->entries[e].ct_code) return true;
    return false;
}
/** ct_user models belong to the module they were written to (cfg ct_uid). With the address as the only identity, a
 * different module put into the slot would get the previous module's presets force-written at join. Another module in the slot
 * drops the user models — nothing is written, the module's own models are adopted. A pre-fix config (ct_uid unknown)
 * is taken over by a module whose channels still fit its entries. Bus held by the caller; the config is saved by
 * the caller (s_ct_cfg_dirty) after the bus is released. */
static volatile bool s_ct_cfg_dirty;
static bool ct_user_owner_locked(uint8_t m)
{
    static const uint8_t Z[12] = {0};
    rbamp_module_cfg_t *mc = &s_cfg.modules[m];
    uint8_t uid[12];
    if (br_read_uid(m, uid) != ESP_OK) {
        rbpower_diag_printf("FAIL m%u identity (UID) unreadable — user CT models not applied", m);
        return false;
    }
    bool known = memcmp(mc->ct_uid, Z, 12) != 0, same = true;
    if (known) same = memcmp(mc->ct_uid, uid, 12) == 0;
    else
        for (uint8_t e = 0; e < mc->entry_count && e < RBAMP_MAX_ENTRIES; ++e)
            if (mc->entries[e].source < RBAMP_MAX_CHANNELS && mc->entries[e].source >= s_nc[m]) same = false;
    if (same) {
        if (!known) { memcpy(mc->ct_uid, uid, 12); s_ct_cfg_dirty = true; }
        return true;
    }
    for (uint8_t e = 0; e < mc->entry_count && e < RBAMP_MAX_ENTRIES; ++e) mc->entries[e].ct_user = 0;
    memset(mc->ct_uid, 0, 12);
    s_ct_cfg_dirty = true;
    rbpower_diag_printf("FAIL m%u another module in the slot (uid %02x%02x%02x%02x…, %u ch): the previous module's user CT models "
                        "dropped, nothing written — the module keeps its own", m, uid[0], uid[1], uid[2], uid[3], s_nc[m]);
    return false;
}
/** Force-write the user CT models of module m (when it still owns them). Bus held by the caller. */
static void ct_user_reapply_locked(uint8_t m, const char *why)
{
    const rbamp_module_cfg_t *mc = &s_cfg.modules[m];
#if CONFIG_RBAMP_SCT_RATING_CORRECTION
    (void)mc; (void)why;
    if (ct_user_any(m)) (void)ct_user_owner_locked(m);   /* keep UID ownership bookkeeping; the library corrects */
    return;
#endif
    if (!ct_user_owner_locked(m)) return;
    for (uint8_t e = 0; e < mc->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
        const rbamp_entry_cfg_t *en = &mc->entries[e];
        if (!en->ct_user || en->source >= RBAMP_MAX_CHANNELS || en->ct_code == 0) continue;
        esp_err_t we = rbamp_set_ct_model_ch_force(s_dev[m], en->source, en->ct_code);
        uint8_t rb = 0;
        if (we == ESP_OK) (void)rbamp_read_ct_model_ch(s_dev[m], en->source, &rb);
        rbpower_diag_printf("%sm%u ch%u CT model %u re-applied after %s (readback %u): %s", we == ESP_OK ? "" : "FAIL ",
                            m, en->source, en->ct_code, why, rb, esp_err_to_name(we));
    }
}
/** Publish the library's SCT rating-correction status/scale per channel to rbpower. */
static void sct_publish(uint8_t m)
{
    if (virt_is(m) || !s_dev[m]) return;
    for (uint8_t ch = 0; ch < s_nc[m] && ch < 8; ++ch) {
        rbamp_sct_correction_t c;
        if (rbamp_sct_correction(s_dev[m], ch, &c) == ESP_OK) rbpower_set_channel_sct(m, ch, (uint8_t)c.status, c.scale);
    }
}
/** After a module restart (RESET_OCCURRED), re-apply the user CT models. Bus held by the caller. */
static void ct_user_reset_watch(uint8_t m)
{
#if CONFIG_RBAMP_SCT_RATING_CORRECTION
    /* rbamp-esp-idf 1.7.2 corrects the readings itself after a module restart (and acknowledges RESET_OCCURRED for
     * its own detection) — the hub neither clears the flag nor force-writes the model any more */
    sct_publish(m);
    return;
#endif
    if (virt_is(m) || s_line[m] != 0 || s_sensor_class[m] != 1 || !ct_user_any(m)) return;
    uint8_t f = 0;
    if (rbamp_read_event_flags(s_dev[m], &f) != ESP_OK || !(f & RBAMP_EVENT_RESET_OCCURRED)) return;
    (void)rbamp_clear_event_flags(s_dev[m], RBAMP_EVENT_RESET_OCCURRED);
    ct_user_reapply_locked(m, "a module restart");
}

/** Reconciliation trace per poll: integrate the module's live power and, every rbpower_diag_period_s(), compare
 *  it with the module's counters (takes the bus lock for the counter read). */
static void diag_poll(uint8_t m, const rbamp_snapshot_t *s)
{
    if (!rbpower_diag_enabled()) { s_diag_last_us[m] = 0; return; }
    int64_t now = esp_timer_get_time();
    {
        double p = 0.0;
        for (uint8_t c = 0; c < ch_n(s); ++c) {   /* was s->power[c] up to 7 on a UI7: read past the 3-entry array */
            float pc = ch_p(m, s, c);
            if (!isnan(pc)) p += pc;                                 /* module sign, no host gain: like the counters */
        }
        /* A stretch with no poll (module absent/off, bus error, vmod off→on) is a HOLE, not module
         * time: extending the integral over it with the last power inflates int by P×hole and gives a
         * spurious FAIL dev (tens of percent). Skip anything longer than
         * 5 poll periods; the counters still carry the energy of the hole, so cnt > int = "the hub
         * did not observe it" — the hole line says why. */
        int64_t dt_us = s_diag_last_us[m] ? now - s_diag_last_us[m] : 0;
        if (dt_us > (int64_t)s_cfg.poll_period_ms * 5000LL) {
            rbpower_diag_printf("m%u poll hole %lus (integral not extended)", m, (unsigned long)(dt_us / 1000000LL));
            dt_us = 0; if (s_diag_holes[m] < 255) s_diag_holes[m]++;
        }
        if (dt_us > 0 && s_has_vhw[m]) s_diag_int_wh[m] += p * ((double)dt_us / 3.6e9);
        s_diag_last_p[m] = p;
    }
    s_diag_last_us[m] = now;
    uint32_t per = rbpower_diag_period_s();
    if (per == 0 || !s_dev[m]) return;
    if (s_diag_tick_us[m] == 0) s_diag_tick_us[m] = now;
    /* reference is taken at the FIRST poll (aligned with the integral's start) — not a period later:
     * otherwise the first QTR line has no counter reference (cnt=nan) and the first period's dev is
     * skewed by the boot-time counter read (about −2 % on a 91-s window). */
    if (s_diag_ref_valid[m] && now - s_diag_tick_us[m] < (int64_t)per * 1000000LL) return;
    s_diag_tick_us[m] = now;

    rbamp_std_energy_t e;
    bus_take();
    esp_err_t r = br_read_energy(m, &e);
    bus_give();
    if (r == ESP_ERR_NOT_SUPPORTED) return;                         /* BASIC / latch-only module: no counters */
    if (r != ESP_OK) { rbpower_diag_printf("FAIL m%u counter read: %s", m, esp_err_to_name(r)); return; }
    if (!s_diag_ref_valid[m]) {
        diag_ref_reset(m, &e);
        rbpower_diag_printf("m%u reconcile start gen=%lu imp=%.3f exp=%.3fWh", m, (unsigned long)e.generation,
                            (double)e.act_import_mwh / 1000.0, (double)e.act_export_mwh / 1000.0);
        return;
    }
    rbamp_std_energy_t d; bool restarted = false;
    if (rbamp_std_energy_delta(&s_diag_ref[m], &e, &d, &restarted) != ESP_OK) {
        rbpower_diag_printf("FAIL m%u counter generation changed %lu->%lu — continuity broken, re-anchoring",
                            m, (unsigned long)s_diag_ref[m].generation, (unsigned long)e.generation);
        diag_ref_reset(m, &e);
        return;
    }
    double cnt = diag_cnt_wh(&d), integ = diag_int_to_now(m);   /* integral up to the counter read instant */
    double base = fmax(fabs(cnt), fabs(integ));
    double dev  = (base > 0.0) ? (integ - cnt) / base * 100.0 : 0.0;
    int64_t span_us = now - s_diag_ref_us[m];
    /* FAIL only on a window long enough for a poll-period alignment error to be < ~1.5 %: >= 60 s and > 5 Wh */
    /* a hole inside the window leaves ±1 poll × P at each edge (about −2.2 % on 60 s after a 20-s hole) — informational, marked HOLE */
    /* a restart the hub itself caused (module OTA activation) is an event, not a FAIL: the window spans the reset,
     * so dev says nothing (otherwise every normal module update would show "1 failure since boot") */
    bool planned = restarted && s_planned_restart_us[m] != 0 && (now - s_planned_restart_us[m]) < 180LL * 1000000LL;
    bool bad = planned ? false : ((span_us >= 60LL * 1000000LL && base > 5.0 && fabs(dev) > 2.0 && s_diag_holes[m] == 0) || restarted);
    char g[96]; rbamp_bridge_gap_status(m, g, sizeof(g));
    /* below 0.1 Wh no verdict: the HUB integral is the noisy side there (1 Hz poll sees 1 of 5 windows, P of a
     * single window scatters at < 20 mA) and the 1-mWh counter step adds ±1–5 %. The
     * counter is the truth for STD; the hub integral is only a cross-check. */
    char dv[16];
    if (base < (d.fine ? 0.001 : 0.1)) snprintf(dv, sizeof(dv), "n/a");   /* µWh module: 1-mWh windows are meaningful */
    else snprintf(dv, sizeof(dv), "%+.2f%%", dev);
    rbpower_diag_printf("%sm%u +%lus cnt=%+.3fWh (imp %.3f exp %.3f) int=%+.3fWh dev=%s%s | %s",
                        bad ? "FAIL " : "", m, (unsigned long)((now - s_diag_ref_us[m]) / 1000000LL),
                        cnt, (double)d.act_import_uwh / 1e6, (double)d.act_export_uwh / 1e6,
                        integ, dv, planned ? " MODULE-RESTARTED (planned: module OTA)" : restarted ? " MODULE-RESTARTED" : (s_diag_holes[m] ? " HOLE" : ""), g);
    /* a module restart is ONE event: re-anchor the reconciliation on the new counters, otherwise every later
     * periodic line would keep MODULE-RESTARTED → a FAIL per minute until the quarter */
    if (restarted) { diag_ref_reset(m, &e); flag_module_hour(m); s_planned_restart_us[m] = 0; }
}

/** Energy of one entry (per-channel or phase-grouped) out of a counter delta, signed Wh (import − export).
 * Returns false when the delta carries nothing usable for this entry. */
static bool tail_entry_wh(uint8_t m, const rbamp_std_energy_t *d, const rbamp_entry_cfg_t *en, double *wh_out)
{
    double wh = 0.0;
    if (en->source == RBAMP_ENTRY_PHASES) {
        if (d->channels > 0) {
            for (uint8_t c = 0; c < d->channels && c < RBAMP_MAX_CHANNELS; ++c)
                wh += ((double)d->ch_import_uwh[c] - (double)d->ch_export_uwh[c]) / 1e6;
        } else {
            wh = (d->act_import_uwh || d->act_export_uwh)
                 ? ((double)d->act_import_uwh - (double)d->act_export_uwh) / 1e6
                 : (double)(d->act_import_wh - d->act_export_wh);
        }
    } else {
        uint8_t ch = en->source;
        if (d->channels > ch && ch < RBAMP_MAX_CHANNELS) {
            wh = ((double)d->ch_import_uwh[ch] - (double)d->ch_export_uwh[ch]) / 1e6;
        } else if (s_nc[m] == 1 && ch == 0) {               /* single-channel module: totals are the channel */
            wh = (d->act_import_uwh || d->act_export_uwh)
                 ? ((double)d->act_import_uwh - (double)d->act_export_uwh) / 1e6
                 : (double)(d->act_import_wh - d->act_export_wh);
        } else {
            return false;
        }
    }
    *wh_out = wh;
    return true;
}

/** A unit's mains phase defaults from its binding when the user set none —
 * multi-V STD module (U3I3): current channel c → L(c+1); grouped PHASES entry → 4 (3P); single-V
 * module (UI1/BASIC): the module's configured phase (L1 by default). Current-only modules without a
 * voltage input leave 0 (= mean grid voltage). A non-zero phase (set by the user) is never overridden. */
static void apply_default_phase(uint8_t m, const rbamp_entry_cfg_t *en)
{
    if (!en || en->unit_id == 0 || en->source == RBAMP_ENTRY_VOLTAGE) return;
    rbpower_unit_definition_t *d = rbpower_home_get_unit_definition_mutable((rbpower_unit_id_t)en->unit_id);
    if (!d || d->phase != 0) return;
    uint8_t ph = 0;
    if (en->source == RBAMP_ENTRY_PHASES)      ph = 4;
    else if (s_nv[m] > 1)                      ph = (uint8_t)(en->source + 1);
    else if (s_has_vhw[m])                     ph = s_cfg.modules[m].phase ? s_cfg.modules[m].phase : 1;
    if (ph) { d->phase = ph; ESP_LOGI(TAG, "unit 0x%04X: phase defaulted to %u from m%u/%s", (unsigned)en->unit_id, ph, m,
                                      en->source == RBAMP_ENTRY_PHASES ? "3P" : "ch"); }
}
/** apply_default_phase() for every entry of every probed module. */
static void apply_default_phases(void)
{
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m) {
        if (s_dev[m] == NULL) continue;
        for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e)
            apply_default_phase(m, &s_cfg.modules[m].entries[e]);
    }
}

/** Voltage for a current-only channel: the GRID voltage of the unit's phase (rbpower_home_grid_voltage;
 * phase 0/unknown = mean of the measured phases), falling back to the legacy fleet scalar s_vbus. */
static float unit_grid_voltage(uint16_t unit_id)
{
    const rbpower_unit_definition_t *d = rbpower_home_get_unit_definition((rbpower_unit_id_t)unit_id);
    uint8_t ph = (d && d->phase >= 1 && d->phase <= 3) ? d->phase : 0;
    float v = rbpower_home_grid_voltage(ph);
    return isnan(v) ? s_vbus : v;
}

/* live-inject counters per module (drdy verb): calls, units fed, units skipped (NaN current), inject errors */
static uint32_t s_inj_calls[RBAMP_MAX_MODULES], s_inj_ok[RBAMP_MAX_MODULES], s_inj_nan[RBAMP_MAX_MODULES], s_inj_err[RBAMP_MAX_MODULES];
static int      s_inj_last_err[RBAMP_MAX_MODULES];
static float    s_inj_last_i0[RBAMP_MAX_MODULES], s_inj_last_ich0[RBAMP_MAX_MODULES];
static uint8_t  s_inj_last_nch[RBAMP_MAX_MODULES], s_inj_last_impl[RBAMP_MAX_MODULES];
/** Inject one module's snapshot into its configured RBgrid units (poll task, bus not held). For current-only
 * modules the device reports no voltage/power → real-time power uses the grid voltage of the unit's phase:
 * P = V × I (apparent ≈ active, no PF), so per-load units still show live W. */
static void inject_module(uint8_t mod, const rbamp_module_cfg_t *mc, const rbamp_snapshot_t *s)
{
    s_inj_calls[mod]++;
    s_inj_last_i0[mod] = s->current[0]; s_inj_last_ich0[mod] = ch_i(mod, s, 0);
    s_inj_last_nch[mod] = s->channels; s_inj_last_impl[mod] = s->implausible;
    uint64_t now = esp_timer_get_time();
    float vg = s_cfg.voltage_gain;
    float cg = s_cfg.current_gain;
    bool  has_v = s_has_vhw[mod];
    float u  = has_v ? (isnan(s->voltage) ? 0.0f : s->voltage * vg) : s_vbus;  /* current-only: mains V_bus */

    for (uint8_t e = 0; e < mc->entry_count; ++e) {
        const rbamp_entry_cfg_t *en = &mc->entries[e];
        if (en->unit_id == 0) continue;

        rbpower_unit_realtime_average_200ms_t m;
        memset(&m, 0, sizeof(m));
        m.average_200ms.samples_count = 1;
        m.is_valid = true;
        m.last_update_us = now;

        if (en->source == RBAMP_ENTRY_VOLTAGE) {
            if (isnan(s->voltage)) continue;
            m.average_200ms.voltage_rms_avg  = u;
            m.average_200ms.power_factor_avg = 1.0f;
        } else if (en->source == RBAMP_ENTRY_PHASES) {
            /* Phase-grouped unit (U3I3 main input): top-level = Σ p / Σ i / mean V (API contract),
             * per-phase live published for ph[] (phase = channel + 1). */
            rbpower_phase_live_t phl[3];
            uint8_t n = 0;
            float sum_i = 0.0f, sum_p = 0.0f, sum_s = 0.0f, sum_v = 0.0f, sum_q = 0.0f; uint8_t nv = 0, nq = 0;
            for (uint8_t c = 0; c < s->channels && c < 3; ++c) {
                if (isnan(s->current[c])) continue;
                float i  = s->current[c] * cg;
                float p  = isnan(s->power[c]) ? 0.0f : s->power[c] * cg;
                float pf = isnan(s->power_factor[c]) ? NAN : s->power_factor[c];
                float v  = (s_nv[mod] > 1 && !isnan(s_u_ph[mod][c])) ? s_u_ph[mod][c] : u;
                phl[n].ph = (uint8_t)(c + 1);
                phl[n].v  = v; phl[n].i = i; phl[n].p = p;
                phl[n].q  = (s_nv[mod] > 1) ? s_q_ph[mod][c] : NAN;
                phl[n].pf = pf;
                phl[n].angle = (s_nv[mod] > 1) ? s_ang_ph[mod][c] : NAN;
                phl[n].seq = s_phseq[mod];
                n++;
                sum_i += i; sum_p += p; sum_s += v * i;
                if (v > 1.0f) { sum_v += v; nv++; }
                if (!isnan(phl[n-1].q)) { sum_q += phl[n-1].q; nq++; }
            }
            if (n == 0) continue;
            float vmean = nv ? sum_v / nv : u;
            m.average_200ms.voltage_rms_avg       = vmean;
            m.average_200ms.current_rms_avg       = sum_i;
            m.average_200ms.power_active_avg_w    = sum_p;
            m.average_200ms.power_apparent_avg_va = sum_s;
            m.average_200ms.power_reactive_avg_var = nq ? sum_q : 0.0f;   /* Σq (contract: top-level q = Σ) */
            {   /* Σp/Σs can exceed 1 by a few % when the host voltage trim is off (e.g. 0.974 on a
                 * calibrated module) — clamp to the physical range so pf never reads >1. */
                float pfs = (sum_s > 0.0f) ? (sum_p / sum_s) : 0.0f;
                if (pfs > 1.0f) pfs = 1.0f; else if (pfs < -1.0f) pfs = -1.0f;
                m.average_200ms.power_factor_avg  = pfs;
            }
            m.instant_power_net_w                 = sum_p;
            (void)rbpower_home_set_phase_live((rbpower_unit_id_t)en->unit_id, phl, n);
        } else {
            uint8_t ch = en->source;
            float ich = ch_i(mod, s, ch), pch = ch_p(mod, s, ch), pfch = ch_pf(mod, s, ch);
            if (isnan(ich)) { s_inj_nan[mod]++; continue; }
            float i = ich * cg;
            /* voltage module: device power + device PF; current-only: host P = V(phase) × I. */
            if (!has_v) u = unit_grid_voltage(en->unit_id);
            float p  = has_v ? (isnan(pch) ? 0.0f : pch * cg) : (u * i);
            float pf = has_v ? (isnan(pfch) ? 0.0f : pfch) : 1.0f;
            /* BASIC meters consumption only: a negative live P (CT reversed) never reaches the energy, so it must not
             * reach the overview / system sum / HA / MQTT either (it would show "exporting" on a module that cannot
             * export). The reversed CT is reported as `reversed` + an event + hour q REVERSE_LOST. */
            if (s_line[mod] == 0 && !virt_is(mod) && p < 0.0f) { p = 0.0f; pf = 0.0f; }
            if (s_line[mod] == 0 && !virt_is(mod)) {   /* BASIC accuracy floor (rbAmp spec): below 1.5 % of the CT nominal
                                                        * current / U × that power the reading is noise → 0 (live only) */
                float nom = 0, fs_ = 0, pk_ = 0;
                if (rbamp_sensor_model_limits(s_sensor_class[mod], en->ct_code, &nom, &fs_, &pk_) && nom > 0) {
                    float i_floor = (s_sensor_class[mod] == 1) ? 0.015f * nom : 0.043f * nom;   /* SCT- 1.5 %, CT- ~4.3 % */
                    if (i < i_floor) { i = 0.0f; p = 0.0f; pf = 0.0f; }
                    else if (p < u * i_floor) { p = 0.0f; }
                }
            }
            m.average_200ms.voltage_rms_avg       = u;
            m.average_200ms.current_rms_avg       = i;
            m.average_200ms.power_active_avg_w    = p;
            m.average_200ms.power_apparent_avg_va = u * i;
            m.average_200ms.power_factor_avg      = pf;
            m.instant_power_net_w                 = p;
        }
        rbpower_err_t ir = rbpower_home_inject_200ms((rbpower_unit_id_t)en->unit_id, &m);
        if (ir == RBPOWER_OK) s_inj_ok[mod]++;
        else { s_inj_err[mod]++; s_inj_last_err[mod] = (int)ir; }
    }

    /* Grid voltage is published per phase from the poll loop (rbpower_home_set_grid_phase), not through a
     * pseudo-unit. */
}

/* ---- discovery self-heal (a boot probe can find modules=0 after both cold and warm boots) ----------------
 * Two real-world ways the boot-time probe misses a live module:
 *   (a) power-cycle race — the module MCU boots slower than the ESP32 reaches the probe;
 *   (b) an esptool/app reset cuts an I2C transaction mid-flight and wedges the module's slave
 *       state machine until it sees new traffic / a power cycle.
 * Tearing the whole bridge down on 0 probed modules would be a permanent, silent death that only a
 * lucky reboot fixes (one-shot init with no healer). So the bridge always stays up and absent-but-configured
 * modules are re-probed (inline retries at boot + periodic healer in the poll task + `modscan`). */
#define RBAMP_REPROBE_PERIOD_US (10ULL * 1000 * 1000)   ///< healer re-probe period for absent modules (µs)
static uint32_t s_reprobe_attempts = 0;   /* healer probe attempts on absent modules */
static uint32_t s_late_joins       = 0;   /* modules that came up AFTER boot */
static bool probe_module(uint8_t m);

/** Classify energy capability (also re-run on a late module join) per module by TYPE +
 * fleet composition, so we know whether a W/Wh accumulator can be initialized for its units:
 *   - voltage-bearing      → ON-DEVICE period energy
 *   - current-only + a fleet voltage source → HOST-side P=V_bus×I
 *   - current-only + NO voltage source       → NONE (energy impossible; current/RT only)
 * A NONE module still tracks current/real-time, just not W/Wh. Does NOT touch s_hostacc —
 * a re-run must not wipe a running HOST integration window (join zeroes its own row). */
static void classify_energy_modes(void)
{
    bool fleet_has_vbus = false;
    for (uint8_t m = 0; m < s_dev_count; ++m) if (s_dev[m] && s_has_vhw[m]) { fleet_has_vbus = true; break; }
    for (uint8_t m = 0; m < s_dev_count; ++m) {
        if (s_dev[m] == NULL) continue;
        if (s_has_vhw[m]) {
            s_energy_mode[m] = RBAMP_ENERGY_ONDEVICE;
            ESP_LOGI(TAG, "module %u (0x%02X) energy: ON-DEVICE period (voltage-bearing)", m, s_cfg.modules[m].addr);
        } else if (fleet_has_vbus) {
            s_energy_mode[m] = RBAMP_ENERGY_HOST;
            ESP_LOGI(TAG, "module %u (0x%02X) energy: HOST-side P=V_bus×I (current-only + fleet voltage)", m, s_cfg.modules[m].addr);
        } else {
            s_energy_mode[m] = RBAMP_ENERGY_NONE;
            ESP_LOGW(TAG, "module %u (0x%02X) energy: NONE — current-only and NO voltage source in fleet; "
                          "W/Wh unavailable, real-time current only", m, s_cfg.modules[m].addr);
        }
    }
}

/** For EVERY configured module (also re-run on a late module join), publish to the
 * rbpower layer (a) whether its bound units' hardware is PRESENT (probe: s_dev[m]!=NULL →
 * present; absent → the unit's compute is gated off, dormant in config) and (b) the energy
 * provenance for present ones (`energy_mode`, NONE/ONDEVICE/HOST → RBPOWER_ENERGY_MODE_*).
 * Iterate config modules (NOT just probed) so absent modules mark their units not-present.
 * Plain bool/uint8 stores — safe from any task. */
static void publish_presence(void)
{
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m) {
        bool present = (s_dev[m] != NULL);
        uint8_t mode = (s_energy_mode[m] == RBAMP_ENERGY_ONDEVICE) ? RBPOWER_ENERGY_MODE_ON_DEVICE :
                       (s_energy_mode[m] == RBAMP_ENERGY_HOST)     ? RBPOWER_ENERGY_MODE_HOST :
                                                                     RBPOWER_ENERGY_MODE_NONE;
        const rbamp_module_cfg_t *mc = &s_cfg.modules[m];
        for (uint8_t e = 0; e < mc->entry_count; ++e) {
            if (mc->entries[e].unit_id == 0) continue;
            /* a channel the module in the slot does not have (e.g. I3-I6 entries over a 3-channel module) is absent too */
            uint8_t src = mc->entries[e].source;
            bool ch_present = present && (src == RBAMP_ENTRY_VOLTAGE || src == RBAMP_ENTRY_PHASES || src < s_nc[m]);
            rbpower_set_unit_hardware_present(mc->entries[e].unit_id, ch_present);  // presence gate
            /* source label for units whose definition carries no rbAmp source (e.g. a main input ← U3I3 PHASES):
             * recorded regardless of presence — the module is this unit's source even while absent */
            rbpower_set_unit_rbamp_feed(mc->entries[e].unit_id, m, mc->entries[e].source);
            if (ch_present) {
                rbpower_set_unit_energy_mode(mc->entries[e].unit_id, mode);
            }
        }
        {   /* keep the module descriptor's presence in step (identity stays for the UI) */
            rbpower_module_desc_t d;
            if (rbpower_get_module_desc(m, &d) && d.present != present) { d.present = present; rbpower_set_module_desc(m, &d); }
        }
        if (!present) {
            ESP_LOGW(TAG, "module %u (0x%02X) ABSENT — its units marked hardware_present=false "
                          "(dormant, no compute)", m, s_cfg.modules[m].addr);
        }
    }
    /* I2C-bus units that NO fleet entry references (slot dropped by `rbampcfg count`, entry re-pointed,
     * hand-edited /rbamp.json): nobody publishes their presence, and a unit defaults to present — e.g.
     * after `rbampcfg count 1` the units of the removed slots would show present:true and join
     * telemetry with no source. Mark every unreferenced bus-0 I2C id absent. */
    {
        uint16_t ref[16] = {0};   /* bit ch of ref[m] = (module m, channel ch) referenced by the fleet */
        for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m)
            for (uint8_t e = 0; e < s_cfg.modules[m].entry_count; ++e) {
                uint16_t u = s_cfg.modules[m].entries[e].unit_id;
                if (u && RBGRID_IS_I2C_UNIT(u)) ref[RBGRID_I2C_GET_MODULE(u)] |= (uint16_t)(1u << RBGRID_I2C_GET_CHANNEL(u));
            }
        for (uint8_t m = 0; m < 16; ++m)
            for (uint8_t ch = 0; ch < 16; ++ch)
                if (!(ref[m] & (1u << ch)))
                    rbpower_set_unit_hardware_present((rbpower_unit_id_t)RBGRID_MAKE_I2C_UNIT_ID(0, m, ch), false);
    }
}

/** Re-probe every configured-but-absent module once; on any join, re-classify + re-publish.
 * Returns the number of modules that joined. Called from the poll task (periodic healer)
 * and from the `modscan` verb (rbbench rx_task) — probe_module serializes the two. */
static uint8_t heal_absent_modules(void)
{
    uint8_t joined = 0;
    for (uint8_t m = 0; m < s_dev_count; ++m) {
        if (s_dev[m] != NULL || s_cfg.modules[m].addr == 0) continue;
        s_reprobe_attempts++;
        if (probe_module(m)) {
            s_late_joins++;
            ESP_LOGW(TAG, "module %u (0x%02X) JOINED LATE (healer) — provisioned, re-publishing fleet",
                     m, s_cfg.modules[m].addr);
            rbpower_diag_printf("m%u (0x%02X) joined late — provisioned", m, s_cfg.modules[m].addr);
            joined++;
        }
    }
    if (joined) {
        classify_energy_modes();
        publish_presence();
        /* a module absent at boot (powered later / bus glitch) must get its units' default phases too —
         * otherwise they keep phase 0 (the boot probe applies the default only to modules present then) */
        apply_default_phases();
    }
    return joined;
}

/* Planned restart: close the open quarter of every present module NOW (same path as the `period`
 * verb), so the hour image on NVS holds everything measured while the hub was alive and the
 * reboot-gap tail covers only the real downtime. */
static int64_t s_last_collect_ok_us[RBAMP_MAX_MODULES];   /* last VALID period snapshot (pre-restart flush skip) */
/** Planned-restart hook: flush the open quarter of every present module (see above). */
static void bridge_pre_restart_flush(void)
{
    for (uint8_t m = 0; m < s_dev_count; ++m) {
        if (s_dev[m] == NULL) continue;
        /* A quarter that closed seconds ago has nothing to flush, and the library refuses a second
         * consumed read inside 30 s (valid=0) — a `reboot` right at a quarter boundary would print
         * "FAIL QTR … valid=0" + "flushed (ESP_ERR_INVALID_RESPONSE)" for an empty window. */
        int64_t since = s_last_collect_ok_us[m] ? esp_timer_get_time() - s_last_collect_ok_us[m] : INT64_MAX;
        if (since < 30LL * 1000000LL) {
            rbpower_diag_printf("planned restart: m%u quarter closed %llds ago — nothing to flush", m, (long long)(since / 1000000LL));
            continue;
        }
        esp_err_t e = rbamp_bridge_collect_period(m, RBPOWER_STAT_PERIOD_15MIN);
        rbpower_diag_printf("planned restart: open quarter flushed m%u (%s)", m, esp_err_to_name(e));
    }
}

/* ===== STD module firmware update, hub-driven =====
 * A vendor-PREPARED bank image (signed/sealed by the module tooling, one file per bank) is uploaded to the hub
 * (main/modfw.c) and streamed here page by page. The bus is taken for ONE 256-byte page at a time, so the normal
 * 1-s poll keeps running between pages: metering never stops during the 10–15 min write. Then commit (module
 * verifies the image), the open quarter is closed exactly like a planned hub restart, activate (~14 s off the
 * bus, module-side rollback on a bad image, calibration survives), and the module is re-provisioned in place:
 * identity refresh, CT verify, reboot-gap tail by its own counters (same UID → credited). */
static void provision_module(uint8_t m, const rbamp_module_cfg_t *mc);
/** @brief State of the (single) module firmware update. */
typedef struct {
    volatile uint8_t state; uint8_t m; uint8_t reason; uint8_t rsv;
    volatile uint32_t done; uint32_t total; uint32_t fw_before, fw_after, bank;
    int64_t t0_us; esp_err_t err; char msg[72];
    uint8_t *img; size_t size;
} modota_t;
/** @var modota_t::state
 *  MO_* state.  @var modota_t::m
 *  Module index.  @var modota_t::reason
 *  Module OTA diagnostic reason code.  @var modota_t::rsv
 *  Reserved.  @var modota_t::done
 *  Pages processed.  @var modota_t::total
 *  Pages in the image.  @var modota_t::fw_before
 *  Image version before.  @var modota_t::fw_after
 *  Image version after.  @var modota_t::bank
 *  Target bank address.  @var modota_t::t0_us
 *  Start time (esp_timer µs).  @var modota_t::err
 *  Last error.  @var modota_t::msg
 *  Status / error text.  @var modota_t::img
 *  Image buffer (PSRAM, owned until the task frees it).  @var modota_t::size
 *  Image size (bytes). */
static modota_t s_ota;
static volatile bool s_ota_task_alive;   /* set at start, cleared as the task's last act: one task at a time */
static const char *const MODOTA_ST[] = { "idle", "writing", "committing", "activating", "done", "error" };
enum { MO_IDLE, MO_WRITING, MO_COMMIT, MO_ACTIVATE, MO_DONE, MO_ERROR };

/** Raw read of an STD register block (16-bit BE pointer) for OTA diagnostics the library has no getter for. */
static esp_err_t std_raw_read_locked(uint8_t m, uint16_t reg, uint8_t *buf, size_t n)   /* caller holds the bus */
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = s_cfg.modules[m].addr, .scl_speed_hz = 50000 };
    i2c_master_dev_handle_t d;
    uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    if (!mod_bus(m)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = i2c_master_bus_add_device(mod_bus(m), &dc, &d);
    if (e == ESP_OK) { e = i2c_master_transmit_receive(d, ptr, 2, buf, n, 100); i2c_master_bus_rm_device(d); }
    return e;
}
/** std_raw_read_locked() under the bus lock. */
static esp_err_t std_raw_read(uint8_t m, uint16_t reg, uint8_t *buf, size_t n)
{
    bus_take();
    esp_err_t e = std_raw_read_locked(m, reg, buf, n);
    bus_give();
    return e;
}
/** Read an n-byte little-endian register as uint32 (takes the bus lock); 0xFFFFFFFF on error. */
static uint32_t std_raw_u32(uint8_t m, uint16_t reg, size_t n)
{
    uint8_t b[4] = {0};
    if (std_raw_read(m, reg, b, n) != ESP_OK) return 0xFFFFFFFFu;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
/* module m is inside an OTA write/commit session: it answers BUSY to any other command. The
 * activation phase is not "busy" here — the in-place re-provision after it must be able to verify/set CTs. */
bool rbamp_bridge_module_ota_busy(uint8_t m)
{
    return s_ota.m == m && (s_ota.state == MO_WRITING || s_ota.state == MO_COMMIT);
}

/** Other bank = active ± bank size, by the module's running-bank id (0 = A, the lower one · 1 = B) */
static uint32_t std_inactive_bank(uint8_t m, uint32_t active, uint32_t size)   /* caller holds the bus */
{
    uint8_t id = 0xFF;
    if (size == 0 || active == 0 || std_raw_read_locked(m, RBAMP_STD_REG_FW_ACTIVE_BANK, &id, 1) != ESP_OK || id > 1) return 0;
    return id == 0 ? active + size : active - size;
}

/* Module identity across time: BOOT_COUNT catches a restart that UPTIME cannot (hub offline
 * while the module restarted and then ran longer than before); FW_VER catches an image change by any path. Both are
 * kept in NVS (rbampan/b<m>) so a restart or an image change during a hub outage is still seen. On change: one
 * journal line + q SRC_RESTART (and SRC_FW for a new image) on the open hour of the module's units. */
/** @brief Module identity persisted in NVS (key rbampan/b + module index). */
typedef struct { uint16_t magic; uint16_t boot_count; uint32_t fw; } rbamp_srcid_t;
/** @var rbamp_srcid_t::magic
 *  RBAMP_SRCID_MAGIC.  @var rbamp_srcid_t::boot_count
 *  Module BOOT_COUNT register.  @var rbamp_srcid_t::fw
 *  Module firmware/image version. */
#define RBAMP_SRCID_MAGIC 0x5349u   ///< marks a valid rbamp_srcid_t
/** Compare the module's BOOT_COUNT / firmware with the stored identity; a change → event + hour q bits. */
static void srcid_check(uint8_t m, const char *where)
{
    rbpower_module_desc_t md;
    if (virt_is(m) || s_dev[m] == NULL || !rbpower_get_module_desc(m, &md) || md.line != 1) return;
    uint32_t bc = std_raw_u32(m, RBAMP_STD_REG_BOOT_COUNT, 2);
    if (bc == 0xFFFFFFFFu) return;
    bc &= 0xFFFF;
    nvs_handle_t h;
    if (nvs_open("rbampan", NVS_READWRITE, &h) != ESP_OK) return;
    char key[8]; snprintf(key, sizeof(key), "b%u", (unsigned)m);
    rbamp_srcid_t old = {0}, now = { RBAMP_SRCID_MAGIC, (uint16_t)bc, md.fw };
    size_t len = sizeof(old);
    bool have = nvs_get_blob(h, key, &old, &len) == ESP_OK && len == sizeof(old) && old.magic == RBAMP_SRCID_MAGIC;
    if (have && (old.boot_count != now.boot_count || old.fw != now.fw)) {
        uint32_t rc = std_raw_u32(m, RBAMP_STD_REG_RESET_CAUSE, 1) & 0xFF;
        bool fw_changed = old.fw != now.fw;
        rbpower_diag_printf("m%u module %s (%s): boot_count %u -> %u, reset_cause 0x%02lX, fw 0x%08lX -> 0x%08lX",
                            m, fw_changed ? "firmware changed" : "restarted", where, old.boot_count, now.boot_count,
                            (unsigned long)rc, (unsigned long)old.fw, (unsigned long)now.fw);
        flag_module_hour_bits(m, RBSTATS_Q_SRC_RESTART | (fw_changed ? RBSTATS_Q_SRC_FW : 0));
    }
    if (!have || old.boot_count != now.boot_count || old.fw != now.fw) {
        if (nvs_set_blob(h, key, &now, sizeof(now)) == ESP_OK) nvs_commit(h);
    }
    nvs_close(h);
}

/** Mark the module update failed with a message (event line FAIL modfw). */
static void modota_fail(const char *what, esp_err_t e)
{
    s_ota.err = e; s_ota.state = MO_ERROR;
    snprintf(s_ota.msg, sizeof(s_ota.msg), "%s: %s (reason %u)", what, esp_err_to_name(e), (unsigned)s_ota.reason);
    rbpower_diag_printf("FAIL modfw m%u: %s", s_ota.m, s_ota.msg);
}

/** Module update task: begin → write pages (bus taken per page) → commit → close the quarter → activate → verify
 *  → re-provision in place. Frees the image and clears s_ota_task_alive as its last act. */
static void modota_task(void *arg)
{
    (void)arg;
    const uint8_t m = s_ota.m;
    rbamp_handle_t dev = s_dev[m];
    const uint32_t pages = (uint32_t)(s_ota.size / RBAMP_STD_REG_OTA_PAYLOAD_SIZE);
    s_ota.total = pages; s_ota.done = 0;
    /* pre-checks: the module's OTA session must be IDLE; record loss/boot counters */
    {
        uint8_t st = 0xFF;
        bus_take(); esp_err_t se = rbamp_std_ota_status(dev, &st); bus_give();
        if (se == ESP_OK && st != RBAMP_STD_OTA_IDLE) {
            rbpower_diag_printf("modfw m%u: module OTA state %u (not IDLE) — aborting the stale session first", m, st);
            bus_take(); (void)rbamp_std_ota_abort(dev); se = rbamp_std_ota_status(dev, &st); bus_give();
            if (se != ESP_OK || st != RBAMP_STD_OTA_IDLE) { modota_fail("module OTA session not idle", ESP_ERR_INVALID_STATE); goto out; }
        }
    }
    uint32_t lf0 = std_raw_u32(m, RBAMP_STD_REG_LOST_FRAMES, 4), rd0 = std_raw_u32(m, RBAMP_STD_REG_RING_DROP, 2);
    uint32_t bc0 = std_raw_u32(m, RBAMP_STD_REG_BOOT_COUNT, 2);
    rbpower_diag_printf("modfw m%u: update started — %lu pages into bank 0x%08lX, fw now 0x%08lX (lost_frames %lu, ring_drop %lu, boot_count %lu)",
                        m, (unsigned long)pages, (unsigned long)s_ota.bank, (unsigned long)s_ota.fw_before,
                        (unsigned long)lf0, (unsigned long)rd0, (unsigned long)bc0);
    bus_take();
    esp_err_t e = rbamp_std_ota_begin(dev, 0);
    bus_give();
    if (e != ESP_OK) { modota_fail("begin", e); goto out; }
    for (uint32_t i = 0; i < pages; ++i) {
        const uint8_t *pg = s_ota.img + (size_t)i * RBAMP_STD_REG_OTA_PAYLOAD_SIZE;
        bool blank = (i != 0);
        for (uint32_t k = 0; blank && k < RBAMP_STD_REG_OTA_PAYLOAD_SIZE; ++k) if (pg[k] != 0xFF) blank = false;
        if (!blank) {
            e = ESP_FAIL;
            for (int attempt = 0; attempt < 2 && e != ESP_OK; ++attempt) {
                bus_take();
                e = rbamp_std_ota_write_page(dev, i, pg);
                bus_give();
            }
            if (e != ESP_OK) {
                bus_take(); (void)rbamp_std_ota_diag(dev, &s_ota.reason); (void)rbamp_std_ota_abort(dev); bus_give();
                char w[24]; snprintf(w, sizeof(w), "page %lu", (unsigned long)i);
                modota_fail(w, e); goto out;
            }
        }
        s_ota.done = i + 1;
        /* hand the bus over after EVERY page: the module paces pages at >= 250 ms and the library waits for its
         * slot inside the lock, so without a real gap the 1-s poll would wait 9–13 s per cycle */
        vTaskDelay(pdMS_TO_TICKS(20));
        if (pages >= 4 && (i + 1) % (pages / 4) == 0)
            rbpower_diag_printf("modfw m%u: %lu/%lu pages", m, (unsigned long)(i + 1), (unsigned long)pages);
    }
    {   /* each page program stalls the module's bus ~2 ms → ~6 lost ADC frames per page (energy is gap-compensated);
         * expect +3000…4500 lost frames, and RING_DROP must stay 0 */
        uint32_t lf1 = std_raw_u32(m, RBAMP_STD_REG_LOST_FRAMES, 4), rd1 = std_raw_u32(m, RBAMP_STD_REG_RING_DROP, 2);
        bool drop = (rd1 != 0xFFFFFFFFu && rd0 != 0xFFFFFFFFu && rd1 != rd0);
        rbpower_diag_printf("%smodfw m%u: written — lost_frames +%ld, ring_drop +%ld%s", drop ? "FAIL " : "", m,
                            (long)(lf1 - lf0), (long)(rd1 - rd0), drop ? " (sample loss beyond the page stalls)" : "");
    }
    s_ota.state = MO_COMMIT;
    bus_take();
    e = rbamp_std_ota_commit(dev, 3000);
    if (e != ESP_OK) { (void)rbamp_std_ota_diag(dev, &s_ota.reason); (void)rbamp_std_ota_abort(dev); }
    bus_give();
    if (e != ESP_OK) { modota_fail("commit (image rejected by the module)", e); goto out; }
    /* close the open quarter like a planned hub restart: the anchor is saved, the ~14-s activation gap becomes a tail */
    {
        int64_t since = s_last_collect_ok_us[m] ? esp_timer_get_time() - s_last_collect_ok_us[m] : INT64_MAX;
        if (since >= 30LL * 1000000LL) {
            esp_err_t fe = rbamp_bridge_collect_period(m, RBPOWER_STAT_PERIOD_15MIN);
            rbpower_diag_printf("modfw m%u: open quarter flushed before activation (%s)", m, esp_err_to_name(fe));
        }
    }
    s_ota.state = MO_ACTIVATE;
    rbpower_diag_printf("modfw m%u: image verified and armed — activating (module off the bus ~14 s)", m);
    s_planned_restart_us[m] = esp_timer_get_time();
    flag_module_hour(m);
    bus_take();
    e = rbamp_std_ota_activate(dev);
    bus_give();
    if (e != ESP_OK) { modota_fail("activate (module rolled back or did not confirm)", e); }
    /* the new image confirms itself only after >= 8 s and 25 healthy windows: final verdict at >= 15 s */
    vTaskDelay(pdMS_TO_TICKS(15000));
    {
        uint32_t swap = std_raw_u32(m, RBAMP_STD_REG_SWAP_PENDING, 4);
        uint32_t rc = std_raw_u32(m, RBAMP_STD_REG_RESET_CAUSE, 1) & 0xFF, bc1 = std_raw_u32(m, RBAMP_STD_REG_BOOT_COUNT, 2);
        rbpower_diag_printf("modfw m%u: after activation — swap_pending %lu, reset_cause 0x%02lX, boot_count %lu (was %lu)",
                            m, (unsigned long)swap, (unsigned long)rc, (unsigned long)bc1, (unsigned long)bc0);
        if (e == ESP_OK && swap != 0 && swap != 0xFFFFFFFFu) {
            e = ESP_ERR_INVALID_STATE; modota_fail("swap still pending 15 s after activation", e);
        }
    }
    /* re-provision in place: the handle stays valid, identity was refreshed by the library */
    {
        rbpower_module_desc_t md;
        if (rbpower_get_module_desc(m, &md)) {
            md.fw = rbamp_firmware_version(dev);
            rbamp_std_info_t si;
            bus_take();
            bool got = (rbamp_std_info(dev, &si) == ESP_OK);
            { uint32_t iv = 0; if (rbamp_std_read_image_version(dev, &iv) == ESP_OK && iv) md.fw = iv; }
            if (got) {
                md.map = si.map_version; md.cal = (si.status & RBAMP_STD_ST_CAL_OK) ? 1 : 0;
                if (si.capabilities & RBAMP_STD_CAP_OTA) { md.ota_bank_size = si.ota_bank_size; md.active_bank = si.active_bank;
                md.inactive_bank = std_inactive_bank(m, si.active_bank, si.ota_bank_size);
                uint32_t bw = 0; if (rbamp_std_read_board_id(dev, &bw) == ESP_OK) md.board_id = bw; }
            }
            bus_give();
            /* the module must now run from the bank we wrote — otherwise the bootloader rolled back */
            if (e == ESP_OK && got && si.active_bank != s_ota.bank) {
                e = ESP_ERR_INVALID_STATE;
                char w[64]; snprintf(w, sizeof(w), "rolled back (running bank 0x%08lX)", (unsigned long)si.active_bank);
                modota_fail(w, e);
            }
            rbpower_set_module_desc(m, &md);
            s_ota.fw_after = md.fw;
        }
        s_probe_us[m] = esp_timer_get_time();
        provision_module(m, &s_cfg.modules[m]);
        boot_tail_arm(m);
        srcid_check(m, "after module OTA");
    }
    if (e == ESP_OK) {
        s_ota.state = MO_DONE;
        snprintf(s_ota.msg, sizeof(s_ota.msg), "fw 0x%08lX -> 0x%08lX in %llds", (unsigned long)s_ota.fw_before,
                 (unsigned long)s_ota.fw_after, (long long)((esp_timer_get_time() - s_ota.t0_us) / 1000000LL));
        rbpower_diag_printf("modfw m%u: done — %s", m, s_ota.msg);
    }
out:
    free(s_ota.img); s_ota.img = NULL;
    s_ota_task_alive = false;
    vTaskDelete(NULL);
}

/** @def MO_REFUSE
 *  @brief Refuse the update: write the reason to `why`, free the image, return the code (local to
 *         rbamp_bridge_module_ota_start()). */
esp_err_t rbamp_bridge_module_ota_start(uint8_t m, uint8_t *img, size_t size, uint32_t bank,
                                        bool has_board, uint32_t board_word, char *why, size_t wn)
{
/* refuse the update: write the reason, free the image, return the code */
#define MO_REFUSE(code, ...) do { snprintf(why, wn, __VA_ARGS__); free(img); return code; } while (0)
    /* MO_ERROR / MO_DONE are set before the task finishes (15-s verdict, re-provision, free): a retry in that
     * window would reset s_ota under the live task → its free() would hit the new image (use-after-free, double free) */
    if (s_ota_task_alive || s_ota.state == MO_WRITING || s_ota.state == MO_COMMIT || s_ota.state == MO_ACTIVATE)
        MO_REFUSE(ESP_ERR_INVALID_STATE, "an update of module %u is still finishing", s_ota.m);
    if (m >= s_dev_count || s_dev[m] == NULL) MO_REFUSE(ESP_ERR_NOT_FOUND, "module %u not present", m);
    if (virt_is(m)) MO_REFUSE(ESP_ERR_NOT_SUPPORTED, "module %u is virtual", m);
    rbamp_std_info_t si;
    bus_take();
    esp_err_t e = rbamp_std_info(s_dev[m], &si);
    bus_give();
    if (e != ESP_OK) MO_REFUSE(ESP_ERR_NOT_SUPPORTED, "module %u is not an STD module", m);
    if (!(si.capabilities & RBAMP_STD_CAP_OTA) || si.ota_bank_size == 0) MO_REFUSE(ESP_ERR_NOT_SUPPORTED, "module %u has no OTA capability", m);
    if (size != si.ota_bank_size) MO_REFUSE(ESP_ERR_INVALID_SIZE, "image %u B != bank size %lu B", (unsigned)size, (unsigned long)si.ota_bank_size);
    { uint8_t id = 0xFF; bus_take(); esp_err_t be = std_raw_read_locked(m, RBAMP_STD_REG_FW_ACTIVE_BANK, &id, 1); bus_give();
      uint32_t other = (be == ESP_OK && id <= 1) ? (id == 0 ? si.active_bank + si.ota_bank_size : si.active_bank - si.ota_bank_size) : 0;
      if (other && bank != si.active_bank && bank != other)
          MO_REFUSE(ESP_ERR_INVALID_ARG, "bank 0x%08lX is neither module bank (inactive = 0x%08lX)", (unsigned long)bank, (unsigned long)other); }
    if (bank == si.active_bank) MO_REFUSE(ESP_ERR_INVALID_ARG, "bank 0x%08lX is the RUNNING bank — send the image for the other bank", (unsigned long)bank);
    if (has_board) {
        bus_take(); e = rbamp_std_ota_board_ok(s_dev[m], board_word); bus_give();
        if (e == ESP_ERR_INVALID_ARG) MO_REFUSE(e, "image board word 0x%08lX does not match the module", (unsigned long)board_word);
    }
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.m = m; s_ota.img = img; s_ota.size = size; s_ota.bank = bank;
    s_ota.fw_before = rbamp_firmware_version(s_dev[m]);
    { uint32_t iv = 0; bus_take(); if (rbamp_std_read_image_version(s_dev[m], &iv) == ESP_OK && iv) s_ota.fw_before = iv; bus_give(); }
    s_ota.t0_us = esp_timer_get_time();
    s_ota.state = MO_WRITING;
    s_ota_task_alive = true;
    if (xTaskCreate(modota_task, "modota", 4096, NULL, 3, NULL) != pdPASS) {
        s_ota_task_alive = false;
        s_ota.state = MO_ERROR; s_ota.img = NULL;
        MO_REFUSE(ESP_ERR_NO_MEM, "no task");
    }
    snprintf(why, wn, "started: %u pages", (unsigned)(size / RBAMP_STD_REG_OTA_PAYLOAD_SIZE));
    return ESP_OK;
#undef MO_REFUSE
}

int rbamp_bridge_module_ota_status(char *out, size_t cap)
{
    int64_t el = s_ota.t0_us ? (esp_timer_get_time() - s_ota.t0_us) / 1000000LL : 0;
    return snprintf(out, cap,
        "{\"module\":%u,\"state\":\"%s\",\"done\":%lu,\"total\":%lu,\"elapsed_s\":%lld,"
        "\"fw_before\":%lu,\"fw_after\":%lu,\"reason\":%u,\"message\":\"%s\"}",
        (unsigned)s_ota.m, MODOTA_ST[s_ota.state <= MO_ERROR ? s_ota.state : 0], (unsigned long)s_ota.done,
        (unsigned long)s_ota.total, (long long)el, (unsigned long)s_ota.fw_before, (unsigned long)s_ota.fw_after,
        (unsigned)s_ota.reason, s_ota.msg);
}

/** Phase sequence of three-phase modules (STD 0x7A+): read every 10 s; a change is an event, a wrong
 * sequence (reverse / abnormal) a FAIL — on a real installation it means swapped or doubled phase wiring. */
static void phseq_poll(uint8_t m)
{
    int64_t now = esp_timer_get_time();
    if (s_phseq_us[m] && now - s_phseq_us[m] < 10LL * 1000000LL) return;
    s_phseq_us[m] = now;
    if (virt_is(m)) return;
    rbamp_std_phase_seq_t q = RBAMP_STD_PHASE_SEQ_NA;
    bus_take();
    esp_err_t e = rbamp_std_read_phase_seq(s_dev[m], &q);
    bus_give();
    if (e != ESP_OK) return;                       /* ESP_ERR_NOT_SUPPORTED below 0x7A: stay unknown */
    uint8_t v = (uint8_t)q;
    if (!s_phseq_seen[m] || v != s_phseq[m]) {
        static const char *const N[] = { "n/a", "forward (L1-L2-L3)", "REVERSE (L1-L3-L2)", "ABNORMAL (no 120° pattern)" };
        bool bad = (v == RBAMP_STD_PHASE_SEQ_REVERSE || v == RBAMP_STD_PHASE_SEQ_ABNORMAL);
        rbpower_diag_printf("%sm%u phase sequence: %s", bad ? "FAIL " : "", m, N[v <= 3 ? v : 0]);
    }
    s_phseq[m] = v; s_phseq_seen[m] = true;
}

/** The poll task: healer, requested period collect, then per module read (DRDY-paced when configured), diagnostics,
 *  host energy integration, per-phase reads, grid voltage publish and the 200 ms inject; then sleep
 *  poll_period_ms. The bus lock is taken per transaction, never across an inject. */
static void rbamp_bridge_poll_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "poll task started (%u module(s), period %ums, free heap=%u)",
             s_dev_count, (unsigned)s_cfg.poll_period_ms, (unsigned)esp_get_free_heap_size());

    uint64_t next_reprobe_us = esp_timer_get_time() + RBAMP_REPROBE_PERIOD_US;
    while (s_running) {
        /* Discovery self-heal: any configured module still absent gets re-probed every
         * RBAMP_REPROBE_PERIOD_US until it answers (see the healer block comment above). */
        if (esp_timer_get_time() >= next_reprobe_us) {
            next_reprobe_us = esp_timer_get_time() + RBAMP_REPROBE_PERIOD_US;
            heal_absent_modules();
        }
        if (s_collect_req) {   /* quarter boundary (rbamp_bridge_request_collect): one owner of the collect */
            rbpower_stat_period_type_t per = s_collect_period;
            s_collect_q_end = s_collect_end_req;   /* injected quarters land in the hour of this boundary */
            s_collect_req = false;
            int64_t c0 = esp_timer_get_time();
            for (uint8_t cm = 0; cm < s_dev_count && s_running; ++cm) (void)rbamp_bridge_collect_period(cm, per);
            s_collect_q_end = 0;
            int64_t cms = (esp_timer_get_time() - c0) / 1000;
            if (cms > 3000) rbpower_diag_printf("collect took %lldms (all modules)", (long long)cms);
        }
        int64_t it0 = esp_timer_get_time();
        s_pt_hk = s_pt_bus = s_pt_rd = s_pt_ph = s_pt_inj = 0; s_pt_ph_err = 0;
        { PT_T0(); rbpower_home_housekeeping_tick(); PT_ADD(s_pt_hk); }   /* heartbeat/day-rollover/NVS flush (I2C-only hubs) */
        for (uint8_t m = 0; m < s_dev_count && s_running; ++m) {
            if (s_dev[m] == NULL) continue;
            if (s_poll_pause_until_us[m] && esp_timer_get_time() < s_poll_pause_until_us[m]) continue;   /* `pollpause` verb: software poll hole */
            rbamp_snapshot_t s;
            { PT_T0(); bus_take(); PT_ADD(s_pt_bus); }
            esp_err_t err;
            { PT_T0();
              if (s_cfg.drdy_gpio_p1 && !virt_is(m)) {
                  rbamp_drdy_event_t ev = {0};
                  err = rbamp_wait_data_ready(s_dev[m], 250, &ev);       /* next fresh snapshot … */
                  if (err == ESP_OK) err = br_read_all(m, &s);          /* … read at once (+ ch3+ on a senior module) */
                  if (err == ESP_OK) {
                      if (ev.by_edge) s_drdy_edge[m]++; else s_drdy_poll[m]++;
                      s_drdy_missed[m] += ev.missed;
                      if (ev.alarm) rbpower_diag_printf("m%u module event on the DRDY line: flags 0x%02X", m, ev.event_flags);
                  } else if (err == ESP_ERR_TIMEOUT) {
                      if (s_drdy_timeout[m]++ % 60 == 0)
                          rbpower_diag_printf("FAIL m%u DRDY: no fresh snapshot in 250 ms (%lu times) — plain read", m, (unsigned long)s_drdy_timeout[m]);
                      err = br_read_all(m, &s);
                  }
              } else {
                  err = br_read_all(m, &s);
              }
              if (err == ESP_OK && (++s_rst_poll[m] % 5) == 0) ct_user_reset_watch(m);
              PT_ADD(s_pt_rd); }
            bus_give();
            if (s_ct_cfg_dirty) { s_ct_cfg_dirty = false; (void)rbgrid_storage_save_rbamp(&s_cfg); }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "module %u (0x%02X) read_all failed: %s",
                         m, s_cfg.modules[m].addr, rbamp_err_to_str(err));
                /* The ESP log never reaches the console in these builds (CONSOLE_NONE): a long bus break would
                 * produce no line at all until the `poll hole` at recovery. Say it on #DIAG: first failure at
                 * once, then once a minute while it lasts. */
                if (rbpower_diag_enabled()) {
                    int64_t nowf = esp_timer_get_time();
                    if (s_poll_fail_n[m] == 0) s_poll_fail_t0[m] = nowf;
                    s_poll_fail_n[m]++;
                    if (s_poll_fail_n[m] == 1 || nowf - s_poll_fail_last_us[m] >= 60000000LL) {
                        s_poll_fail_last_us[m] = nowf;
                        rbpower_diag_printf("FAIL m%u poll: %s (%lu failed, %lus) — module unreachable", m,
                                            rbamp_err_to_str(err), (unsigned long)s_poll_fail_n[m],
                                            (unsigned long)((nowf - s_poll_fail_t0[m]) / 1000000LL));
                    }
                }
                continue;
            }
            if (s_poll_fail_n[m]) {                             /* polls are back */
                if (rbpower_diag_enabled())
                    rbpower_diag_printf("m%u poll restored after %lus (%lu failed polls)", m,
                                        (unsigned long)((esp_timer_get_time() - s_poll_fail_t0[m]) / 1000000LL),
                                        (unsigned long)s_poll_fail_n[m]);
                s_poll_fail_n[m] = 0; s_poll_fail_last_us[m] = 0;
                reidentify_module(m);                           /* swapped on the fly? */
                /* the module may have reset during the outage ("latch bank re-armed after a module reset"):
                 * keep its first readings out of the 10-s envelope — otherwise an hour with ~0 W load can get
                 * a p_min of several negative watts from exactly this restore */
                s_env_skip[m] = 2;
            }
            diag_poll(m, &s);                                   /* reconciliation trace */
            ct_limit_check(m, &s);                              /* CT nominal / clipping */
            if (s_env_skip[m]) s_env_skip[m]--;
            else pmin_feed(m, &s);                              /* 10-s rolling minimum for the period record */
            if (s_cfg.log_readings) {
                char line[176];
                int n = snprintf(line, sizeof(line), "m%u@0x%02X U=%6.1fV f=%4.1fHz ",
                                 m, s_cfg.modules[m].addr, (double)s.voltage, (double)s.frequency);
                for (uint8_t ch = 0; ch < ch_n(&s) && n < (int)sizeof(line) - 1; ++ch) {
                    n = rb_scat(line, sizeof(line), n, "[%u]I=%5.2fA P=%6.1fW ",
                                  ch, (double)ch_i(m, &s, ch), (double)ch_p(m, &s, ch));
                }
                ESP_LOGI(TAG, "%s", line);
            }
            /* Cache mains voltage from a voltage-bearing module; HOST-mode modules
             * integrate energy (P = V_bus × I). NONE-mode modules accumulate nothing. */
            if (s_energy_mode[m] == RBAMP_ENERGY_ONDEVICE && !isnan(s.voltage)) {
                s_vbus = s.voltage * s_cfg.voltage_gain;
                /* The voltage module also measures grid frequency — feed it to the system (the 200ms
                 * inject struct carries no frequency field; without this telemetry freq reads 0). */
                if (!isnan(s.frequency) && s.frequency > 0.0f) {
                    rbpower_home_set_system_frequency(s.frequency);
                }
            } else if (s_energy_mode[m] == RBAMP_ENERGY_HOST) {
                host_accumulate(m, &s);
            }
            /* Per-phase block for multi-voltage modules (U3I3): V of phases 1..n-1 (phase 0
             * is the snapshot voltage) + reactive power per channel. Small bursts; NaN on failure. */
            if (s_nv[m] > 1) {
                s_u_ph[m][0] = isnan(s.voltage) ? NAN : s.voltage * s_cfg.voltage_gain;
                for (uint8_t v = 1; v < s_nv[m] && v < 3; ++v) {
                    rbamp_std_voltage_t uv;
                    { PT_T0(); bus_take(); PT_ADD(s_pt_bus); }
                    esp_err_t ve;
                    { PT_T0(); ve = br_std_read_voltage(m, v, &uv); PT_ADD(s_pt_ph); }
                    bus_give();
                    if (ve != ESP_OK) s_pt_ph_err++;
                    s_u_ph[m][v] = (ve == ESP_OK && !isnan(uv.u_rms)) ? uv.u_rms * s_cfg.voltage_gain : NAN;
                    s_ang_ph[m][v] = (ve == ESP_OK && uv.angle_valid) ? uv.angle_deg : NAN;
                }
                s_ang_ph[m][0] = (!isnan(s_ang_ph[m][1]) || !isnan(s_ang_ph[m][2])) ? 0.0f : NAN;   /* L1 is the reference */
                phseq_poll(m);
                for (uint8_t c = 0; c < s_nc[m] && c < 3; ++c) {
                    rbamp_std_channel_t cc;
                    { PT_T0(); bus_take(); PT_ADD(s_pt_bus); }
                    esp_err_t ce;
                    { PT_T0(); ce = br_std_read_channel(m, c, &cc); PT_ADD(s_pt_ph); }
                    bus_give();
                    if (ce != ESP_OK) s_pt_ph_err++;
                    s_q_ph[m][c] = (ce == ESP_OK && !isnan(cc.q)) ? cc.q * s_cfg.current_gain : NAN;
                }
            }
            /* Grid phase voltages: the module measures the GRID, per phase.
             * STD multi-V: V channel c -> L(c+1); single-V (UI1/BASIC): the module's configured phase. */
            {
                float fz = (!isnan(s.frequency) && s.frequency > 0.0f) ? s.frequency : NAN;
                if (s_nv[m] > 1) {
                    for (uint8_t v = 0; v < s_nv[m] && v < 3; ++v)
                        if (!isnan(s_u_ph[m][v]) && s_u_ph[m][v] > 1.0f)
                            rbpower_home_set_grid_phase((uint8_t)(v + 1), s_u_ph[m][v], fz, m, v);
                } else if (s_has_vhw[m] && !isnan(s.voltage) && s.voltage > 1.0f) {
                    uint8_t ph = s_cfg.modules[m].phase ? s_cfg.modules[m].phase : 1;
                    rbpower_home_set_grid_phase(ph, s.voltage * s_cfg.voltage_gain, fz, m, 0);
                }
            }
            if (s_cfg.inject_enabled) {
                PT_T0(); inject_module(m, &s_cfg.modules[m], &s); PT_ADD(s_pt_inj);
            }
        }
        {
            int64_t it = esp_timer_get_time() - it0;
            if (it > 500000LL) vTaskDelay(pdMS_TO_TICKS(20));   /* a long poll (module stretching) never runs back to back */
            if (it > 2000000LL && rbpower_diag_enabled()) {
                int64_t other = it - s_pt_hk - s_pt_bus - s_pt_rd - s_pt_ph - s_pt_inj;
                /* library counters: a read that recovers on a retry returns ESP_OK —
                 * only an EXHAUSTED retry budget (+ bus reset + final try) is counted; print the deltas */
                static uint32_t s_ex_prev[RBAMP_MAX_MODULES], s_sr_prev[RBAMP_MAX_MODULES];
                char lc[64] = ""; int lp = 0;
                for (uint8_t mm = 0; mm < s_dev_count && mm < RBAMP_MAX_MODULES && lp < (int)sizeof(lc) - 16; ++mm) {
                    if (!s_dev[mm] || virt_is(mm)) continue;
                    uint32_t ex = rbamp_retry_exhaustion_count(s_dev[mm]), sr = rbamp_sanity_reject_count(s_dev[mm]);
                    lp = rb_scat(lc, sizeof(lc), lp, " m%u ex+%lu sr+%lu", mm,
                                   (unsigned long)(ex - s_ex_prev[mm]), (unsigned long)(sr - s_sr_prev[mm]));
                    s_ex_prev[mm] = ex; s_sr_prev[mm] = sr;
                }
                /* during a module firmware update the bus is shared with the page writes: expected, not a fault */
                bool modota_on = (s_ota.state == MO_WRITING || s_ota.state == MO_COMMIT || s_ota.state == MO_ACTIVATE);
                rbpower_diag_printf("%spoll slow %lldms%s: hk=%lld bus=%lld rd=%lld ph=%lld(err %u) inj=%lld other=%lld ms |%s",
                                    modota_on ? "" : "FAIL ", (long long)(it / 1000), modota_on ? " (module firmware update)" : "",
                                    (long long)(s_pt_hk / 1000), (long long)(s_pt_bus / 1000),
                                    (long long)(s_pt_rd / 1000), (long long)(s_pt_ph / 1000), (unsigned)s_pt_ph_err,
                                    (long long)(s_pt_inj / 1000), (long long)(other / 1000), lc);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(s_cfg.poll_period_ms));
    }

    ESP_LOGI(TAG, "poll task exiting");
    s_task = NULL;
    vTaskDelete(NULL);
}

static void provision_module(uint8_t m, const rbamp_module_cfg_t *mc);

/* Virtual slot: the probe equivalent without bus traffic (see the virtual module block). */
static bool virt_probe(uint8_t m)
{
    const virt_mod_t *v = &s_virt[m];
    s_has_vhw[m] = true;
    memset(s_hostacc[m], 0, sizeof(s_hostacc[m]));
    s_nv[m] = (v->nch == 3) ? 3 : 1; s_nc[m] = v->nch;
    for (int k = 0; k < 3; ++k) { s_u_ph[m][k] = NAN; s_q_ph[m][k] = NAN; s_ang_ph[m][k] = NAN; }
    rbpower_module_desc_t md = {0};
    md.present = true; md.addr = s_cfg.modules[m].addr; md.line = 1; md.variant = (v->nch == 3) ? 0x63 : 0x11;
    md.n_v = s_nv[m]; md.n_i = s_nc[m]; md.map = 0x36; md.period_src = 2; md.cal = 1; md.fw = 0xEE;
    snprintf(md.model, sizeof(md.model), "VIRT-%s", (v->nch == 3) ? "U3I3" : "UI1");
    rbpower_set_module_desc(m, &md);
    s_dev[m] = (rbamp_handle_t)&s_virt_dummy[m];
    {   /* like rbamp_begin(): the period reference is anchored at probe — the energy before it is the
         * reboot-gap tail's business (boot_tail_arm below), the first collect >= 30 s later is valid */
        virt_mod_t *vw = &s_virt[m];
        virt_tick(m);
        for (int c = 0; c < 3; ++c) { vw->latch_imp[c] = vw->imp_mwh[c]; vw->latch_exp[c] = vw->exp_mwh[c]; }
        s_virt_latch_us[m] = esp_timer_get_time();
        vw->latch_wall_s = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    }
    ESP_LOGW(TAG, "module %u: VIRTUAL %s (gen=%lu, %u ch, %.0f W/ch) — software counters, no bus traffic",
             m, md.model, (unsigned long)v->generation, v->nch, (double)v->p[0]);
    s_probe_us[m] = esp_timer_get_time();
    provision_module(m, &s_cfg.modules[m]);
    boot_tail_arm(m);
    return true;
}

/** Read what the module IS (line, variant, channels, voltage, firmware, OTA banks) into the module descriptor.
 * Caller holds the bus. Shared by the first probe and the in-place re-identification after a swap. */
static void describe_module(uint8_t m, rbamp_handle_t dev)
{
    const uint8_t addr = s_cfg.modules[m].addr;
    {   /* sensor family (REG_SENSOR_CLASS / STD CFG_CLASS). Caller holds the bus. */
        s_sensor_class[m] = 0xFF;
        uint8_t sc = 0xFF;
        if (rbamp_read_sensor_class(dev, &sc) == ESP_OK && sc <= 3) s_sensor_class[m] = sc;   /* rbamp-esp-idf 1.7.2-rc2 */
    }
    s_has_vhw[m] = rbamp_has_voltage_hw(dev);
    memset(s_hostacc[m], 0, sizeof(s_hostacc[m]));   /* fresh join = clean host window state */
    s_nv[m] = 0; s_nc[m] = rbamp_channels(dev);
    for (int k = 0; k < 3; ++k) { s_u_ph[m][k] = NAN; s_q_ph[m][k] = NAN; s_ang_ph[m][k] = NAN; }
    {
        rbpower_module_desc_t md = {0};
        md.present = true; md.addr = addr; md.n_i = s_nc[m]; md.n_v = s_has_vhw[m] ? 1 : 0; md.cal = -1;
        md.fw = rbamp_firmware_version(dev);
        rbamp_std_info_t si;
        if (rbamp_std_info(dev, &si) == ESP_OK) {      /* STD line only; BASIC returns an error */
            s_nv[m] = si.n_voltage;
            if (si.n_current < s_nc[m]) s_nc[m] = si.n_current;
            md.line = 1; md.variant = si.variant; md.n_v = si.n_voltage; md.n_i = si.n_current;
            md.map = si.map_version; md.period_src = (uint8_t)si.period_source;
            md.cal = (si.status & RBAMP_STD_ST_CAL_OK) ? 1 : 0;
            if (si.capabilities & RBAMP_STD_CAP_OTA) { md.ota_bank_size = si.ota_bank_size; md.active_bank = si.active_bank;
                md.inactive_bank = std_inactive_bank(m, si.active_bank, si.ota_bank_size);
                uint32_t bw = 0; if (rbamp_std_read_board_id(dev, &bw) == ESP_OK) md.board_id = bw; }
            { uint32_t iv = 0; if (rbamp_std_read_image_version(dev, &iv) == ESP_OK && iv) md.fw = iv; }   /* STD: image version (0x0001007B), not the map byte */
            snprintf(md.model, sizeof(md.model), "STD-%s",
                     si.variant == 0x63 ? "U3I3" : si.variant == 0x11 ? "UI1" : "?");
            if (md.model[4] == '?') snprintf(md.model, sizeof(md.model), "STD-0x%02X", si.variant);
            ESP_LOGI(TAG, "module %u STD: map=0x%02X variant=0x%02X n_v=%u n_i=%u period_src=%d caps=0x%04X",
                     m, si.map_version, si.variant, si.n_voltage, si.n_current,
                     (int)si.period_source, (unsigned)si.capabilities);
        } else {
            md.line = 0;
            /* rbamp-esp-idf 1.7.1: channel count + voltage come from HW_VARIANT (0x55); the model name from the variant
             * (1.7.0 pinned 3 channels — a BASIC UI1 showed as "BASIC-UI3") */
            static const char *const VN[] = { NULL, "UI1", "UI2", "UI3", "I1", "I2", "I3", "UI5", "UI7" };
            rbamp_variant_t v = rbamp_variant(dev);
            md.variant = (uint8_t)v;
            if ((unsigned)v >= 1 && (unsigned)v < sizeof(VN) / sizeof(VN[0])) snprintf(md.model, sizeof(md.model), "BASIC-%s", VN[v]);
            else snprintf(md.model, sizeof(md.model), "BASIC-%s%u", s_has_vhw[m] ? "UI" : "I", (unsigned)s_nc[m]);
        }
        md.sensor_class = s_sensor_class[m];
        s_line[m] = md.line;
        rbpower_set_module_desc(m, &md);
    }
}

/** After a poll outage: is it still the same module? A module swapped on the fly (e.g. an STD module replaced by a
 * BASIC one at the same address, hub not rebooted) would be read with the wrong protocol: garbage current, a
 * nonsense "generation", negative counters. Side-effect-free checks only — a re-begin on BASIC primes the latch and drops
 * the energy of the running period, so it runs only on a confirmed change.
 *   STD handle:   std_info must still answer with the same variant, and the UID must match.
 *   BASIC handle: HW_VARIANT (0x55) must read the same variant. */
static void reidentify_module(uint8_t m)
{
    rbamp_handle_t dev = s_dev[m];
    rbpower_module_desc_t md;
    if (!dev || !rbpower_get_module_desc(m, &md) || rbamp_bridge_module_ota_busy(m) || s_ota_task_alive) return;
    bool changed = false;
    char why[64] = "";
    bus_take();
    if (md.line == 1) {
        rbamp_std_info_t si;
        if (rbamp_std_info(dev, &si) != ESP_OK) { changed = true; snprintf(why, sizeof(why), "STD identity no longer answers"); }
        else if (si.variant != md.variant)      { changed = true; snprintf(why, sizeof(why), "STD variant 0x%02X -> 0x%02X", md.variant, si.variant); }
        else if (s_uid_ok[m]) {
            uint8_t uid[12];
            if (br_read_uid(m, uid) == ESP_OK && memcmp(uid, s_uid[m], 12) != 0) { changed = true; snprintf(why, sizeof(why), "module UID changed"); }
        }
    } else {
        rbamp_variant_t v = RBAMP_VARIANT_UNKNOWN;
        if (rbamp_read_variant(dev, &v) != ESP_OK || (uint8_t)v != md.variant) {
            changed = true; snprintf(why, sizeof(why), "BASIC variant %u -> %u", (unsigned)md.variant, (unsigned)v);
        }
    }
    if (!changed) { bus_give(); return; }
    esp_err_t be = rbamp_begin(dev);   /* re-detects the line + variant on the same handle (library: "a re-begin may face another module") */
    if (be == ESP_OK) describe_module(m, dev);
    bus_give();
    rbpower_module_desc_t nd;
    (void)rbpower_get_module_desc(m, &nd);
    rbpower_diag_printf("FAIL m%u module REPLACED (%s): %s -> %s, re-provisioned%s", m, why, md.model,
                        be == ESP_OK ? nd.model : "?", be == ESP_OK ? "" : " — begin failed, will retry");
    if (be != ESP_OK) return;
    s_probe_us[m] = esp_timer_get_time();
    s_uid_ok[m] = false;                                  /* boot_tail_arm re-reads the identity */
    provision_module(m, &s_cfg.modules[m]);               /* CT model from the fleet entry */
    boot_tail_arm(m);                                     /* new UID → "replaced": anchor reset, nothing credited */
    srcid_check(m, "after module swap");
    classify_energy_modes();
    publish_presence();
}

/** Probe + provision ONE configured module. Serialized by the bus mutex from the first check
 * through the s_dev[m] assignment, so the poll-task healer and the `modscan` verb can't
 * double-probe; s_dev[m] only ever transitions NULL -> valid. provision_module is called
 * outside the lock (its rbamp_bridge_* helpers take the lock themselves). */
static bool probe_module(uint8_t m)
{
    uint8_t addr = s_cfg.modules[m].addr;
    if (addr == 0) return false;
    bus_take();
    if (s_dev[m] != NULL) { bus_give(); return true; }   /* someone else just probed it */
    if (virt_is(m)) { bus_give(); return virt_probe(m); }
    rbamp_handle_t dev = NULL;
    if (!mod_bus(m)) { bus_give(); return false; }   /* its bus is not configured / failed to start */
    esp_err_t merr = rbamp_new(mod_bus(m), addr, &dev);
    /* resume, not begin: on BASIC no primer latch — the first snapshot carries the window the module accumulated
     * while the hub was down (module_window_ms = commit_count × 200 ms). STD: identical to begin (1.7.2-rc1). */
    if (merr == ESP_OK) merr = rbamp_begin_resume(dev);
    if (merr != ESP_OK) {
        if (dev) rbamp_del(dev);
        bus_give();
        return false;
    }
    describe_module(m, dev);
    s_dev[m] = dev;
    bus_give();
    ESP_LOGI(TAG, "Configured rbamp module %u @0x%02X fw=0x%02X channels=%u voltage_hw=%s (free heap=%u)",
             m, rbamp_address(s_dev[m]), rbamp_firmware_version(s_dev[m]),
             rbamp_channels(s_dev[m]), s_has_vhw[m] ? "yes" : "no",
             (unsigned)esp_get_free_heap_size());
    s_probe_us[m] = esp_timer_get_time();
    drdy_attach(m);
    provision_module(m, &s_cfg.modules[m]);
    boot_tail_arm(m);                     /* reboot-gap energy: compare counters with the NVS anchor */
    hourq_restore(m);                     /* q bits of the open hour flagged before a hub restart */
    srcid_check(m, "at join");
    sct_publish(m);
    return true;
}

/** Provision one module's current channels VERIFY-THEN-SET. set_ct_model_ch reloads
 * AND persists the CT-preset gain into the per-unit gain register (0xF4) — so we
 * only write when the code is actually wrong/unset, else a normal boot would wipe
 * a calibrated module's per-unit gain. */
static void provision_module(uint8_t m, const rbamp_module_cfg_t *mc)
{
    if (ct_factory_paired(m)) {   /* CT- series: the module's own per-channel models are the truth — nothing to provision */
        rbpower_diag_printf("m%u CT- series (sensor class %u): factory-paired CT, CT provisioning skipped", m, s_sensor_class[m]);
        return;
    }
    if (s_sensor_class[m] == 0) {
        rbpower_diag_printf("FAIL m%u has no sensor class — module is not calibrated, CT not provisioned", m);
        return;
    }
    if (s_sensor_class[m] == 1 && !virt_is(m)) {
        /* SCT- module: shipped calibrated with the model of the SCT it was calibrated on — the MODULE is the truth at
         * join. Adopt its per-channel model into the fleet config; the hub writes a model only on an explicit user
         * choice (PATCH ct_code). Writing the config default at join would replace the shipped model. */
        bool adopted = false;
        rbamp_module_cfg_t *wm = &s_cfg.modules[m];
        if (ct_user_any(m)) { bus_take(); (void)ct_user_owner_locked(m); bus_give(); }
        /* every current channel of the module is a sensor in the registry view (UI7: 7) — a channel without an entry
         * gets an UNBOUND one (unit 0) with the module's model, bindable from the app (bind API) */
        for (uint8_t ch = 0; ch < s_nc[m] && ch < RBAMP_MAX_CHANNELS; ++ch) {
            bool have = false;
            for (uint8_t e = 0; e < wm->entry_count && e < RBAMP_MAX_ENTRIES; ++e)
                if (wm->entries[e].source == ch || wm->entries[e].source == RBAMP_ENTRY_PHASES) { have = true; break; }
            if (have || wm->entry_count >= RBAMP_MAX_ENTRIES) continue;
            uint8_t cur = 0;
            (void)rbamp_bridge_read_ct_model(m, ch, &cur);
            wm->entries[wm->entry_count++] = (rbamp_entry_cfg_t){ .source = ch, .ct_code = cur, .unit_id = 0, .ct_user = 0 };
            rbpower_diag_printf("m%u ch%u: unbound sensor added (module's CT model %u)", m, ch, cur);
            adopted = true;
        }
        for (uint8_t e = 0; e < wm->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
            rbamp_entry_cfg_t *en = &wm->entries[e];
            if (en->source == RBAMP_ENTRY_VOLTAGE || en->source == RBAMP_ENTRY_PHASES) continue;
            if (en->ct_user) continue;   /* the user's choice wins over the module's model (re-applied below) */
            uint8_t cur = 0;
            if (rbamp_bridge_read_ct_model(m, en->source, &cur) == ESP_OK && cur != 0 && cur != en->ct_code) {
                rbpower_diag_printf("m%u ch%u: module's CT model %u adopted (hub config had %u)", m, en->source, cur, en->ct_code);
                en->ct_code = cur;
                adopted = true;
            }
        }
        if (adopted || s_ct_cfg_dirty) { s_ct_cfg_dirty = false; (void)rbgrid_storage_save_rbamp(&s_cfg); }
        if (ct_user_any(m)) {   /* a restart before this hub met the module (or a different model in it) */
            bus_take();
            uint8_t cur_ok = 1;
            for (uint8_t e = 0; e < wm->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
                const rbamp_entry_cfg_t *en = &wm->entries[e];
                uint8_t cur = 0;
                if (en->ct_user && en->source < RBAMP_MAX_CHANNELS &&
                    (rbamp_read_ct_model_ch(s_dev[m], en->source, &cur) != ESP_OK || cur != en->ct_code)) cur_ok = 0;
            }
            if (!cur_ok || (rbamp_begin_event_flags(s_dev[m]) & RBAMP_EVENT_RESET_OCCURRED)) {
                (void)rbamp_clear_event_flags(s_dev[m], RBAMP_EVENT_RESET_OCCURRED);
                ct_user_reapply_locked(m, cur_ok ? "a module restart (seen at join)" : "join (model differs)");
            }
            bus_give();
            if (s_ct_cfg_dirty) { s_ct_cfg_dirty = false; (void)rbgrid_storage_save_rbamp(&s_cfg); }
        }
        return;
    }
    for (uint8_t e = 0; e < mc->entry_count; ++e) {
        const rbamp_entry_cfg_t *en = &mc->entries[e];
        if (en->source == RBAMP_ENTRY_VOLTAGE || en->ct_code == 0) continue;
        if (en->source == RBAMP_ENTRY_PHASES) {       /* grouped: provision every current channel */
            for (uint8_t ch = 0; ch < s_nc[m] && ch < 3; ++ch) {
                uint8_t cur = 0xFF;
                if (rbamp_bridge_read_ct_model(m, ch, &cur) == ESP_OK && cur == en->ct_code) continue;
                esp_err_t cterr = rbamp_bridge_set_ct_model(m, ch, en->ct_code);
                ESP_LOGW(TAG, "module %u ch%u (grouped): CT code %u->%u -> %s", m, ch, cur, en->ct_code, rbamp_err_to_str(cterr));
                if (cterr == ESP_OK && s_cfg.ct_persist) (void)rbamp_bridge_save_user_config(m);
            }
            continue;
        }
        uint8_t ch = en->source, cur = 0xFF;
        esp_err_t rerr = rbamp_bridge_read_ct_model(m, ch, &cur);
        if (rerr == ESP_OK && cur == en->ct_code) {
            ESP_LOGI(TAG, "module %u ch%u: CT code=%u already set - skip (preserve gain cal)", m, ch, cur);
        } else {
            esp_err_t cterr = rbamp_bridge_set_ct_model(m, ch, en->ct_code);
            uint8_t rb = 0xFF;
            if (cterr == ESP_OK) (void)rbamp_bridge_read_ct_model(m, ch, &rb);
            ESP_LOGW(TAG, "module %u ch%u: CT code %u->%u (readback=%u) -> %s [loaded preset gain; re-cal AFTER]",
                     m, ch, cur, en->ct_code, rb, rbamp_err_to_str(cterr));
            if (cterr == ESP_OK && s_cfg.ct_persist) (void)rbamp_bridge_save_user_config(m);
        }
    }
}

esp_err_t rbamp_bridge_start(const rbamp_bridge_config_t *cfg)
{
    if (s_running) {
        ESP_LOGW(TAG, "already running");
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg = cfg ? *cfg : RBAMP_BRIDGE_DEFAULT_CONFIG();
    if (s_cfg.module_count == 0 || s_cfg.module_count > RBAMP_MAX_MODULES) {
        ESP_LOGE(TAG, "invalid module_count %u", s_cfg.module_count);
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t m = 0; m < RBAMP_MAX_MODULES; ++m) pmin_reset(m);   /* p_min/p_max start unknown (NAN) */

    if (s_bus_lock == NULL) {
        s_bus_lock = xSemaphoreCreateMutex();
        if (s_bus_lock == NULL) { ESP_LOGE(TAG, "failed to create bus mutex"); return ESP_ERR_NO_MEM; }
    }

    /* 1. Shared I2C master bus. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = s_cfg.i2c_port,
        .sda_io_num                   = s_cfg.sda_gpio,
        .scl_io_num                   = s_cfg.scl_gpio,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = s_cfg.internal_pullup,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err)); return err; }
    if (s_cfg.bus1_sda_p1 && s_cfg.bus1_scl_p1) {   /* bus 1 on the other controller; a failure leaves its modules absent */
        const i2c_master_bus_config_t b1 = {
            .i2c_port = s_cfg.i2c_port == 1 ? 0 : 1, .sda_io_num = s_cfg.bus1_sda_p1 - 1, .scl_io_num = s_cfg.bus1_scl_p1 - 1,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = s_cfg.internal_pullup,
        };
        esp_err_t e1 = i2c_new_master_bus(&b1, &s_bush[1]);
        if (e1 != ESP_OK) s_bush[1] = NULL;
        rbpower_diag_printf("%sI2C bus 1 SDA%u/SCL%u: %s", e1 == ESP_OK ? "" : "FAIL ", s_cfg.bus1_sda_p1 - 1,
                            s_cfg.bus1_scl_p1 - 1, esp_err_to_name(e1));
    }

    /* 2. Probe + provision each module. A module that fails to probe is skipped
     *    (NULL handle) — the rest of the fleet still runs. Brief inline retries cover the
     *    common power-cycle race (module MCU still booting); the poll-task healer covers
     *    the rest (incl. a slave wedged by a reset mid-transaction). ZERO probed modules no
     *    longer kills the bridge — it stays up and the healer keeps trying. */
    s_dev_count = s_cfg.module_count;
    uint8_t ok = 0;
    for (uint8_t m = 0; m < s_dev_count; ++m) {
        if (s_cfg.modules[m].addr == 0) { s_dev[m] = NULL; continue; }
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (probe_module(m)) break;
            ESP_LOGW(TAG, "module %u (0x%02X) probe attempt %d failed — retrying",
                     m, s_cfg.modules[m].addr, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (s_dev[m]) ok++;
        /* event log: a configured module that did not answer gets a timestamped line (the web app's
         * "sensor not responding" card takes its time from it); the healer's "joined late" closes it */
        else rbpower_diag_printf("FAIL m%u (0x%02X) not found at boot — healer re-probes every %us", m,
                                 s_cfg.modules[m].addr, (unsigned)(RBAMP_REPROBE_PERIOD_US / 1000000ULL));
    }
    if (ok == 0) {
        ESP_LOGE(TAG, "no rbAmp modules responded at boot — bridge stays up, healer re-probes every %us",
                 (unsigned)(RBAMP_REPROBE_PERIOD_US / 1000000ULL));
    }

    classify_energy_modes();   /* per-module energy capability by TYPE + fleet composition */
    publish_presence();        /* hardware_present + energy provenance per unit */
    apply_default_phases();    /* unit phase from the binding when the user set none */

    ESP_LOGW(TAG, "fleet probed (%u/%u ok), free heap=%u — creating poll task",
             ok, s_cfg.module_count, (unsigned)esp_get_free_heap_size());

    /* 3. Single poll task drives the whole fleet.
     *
     * Core affinity: the firmware has no native ESP32-ADC pipeline, so
     * APP_CPU (core 1) is otherwise idle — we pin the rbAmp metering loop there,
     * isolated from the WiFi / LWIP / httpd / MQTT stack that lives on PRO_CPU
     * (core 0). This keeps the I2C poll cadence + energy-integration timestamps free
     * of WiFi-interrupt jitter.
     * Cross-core safety: poll writes via rbpower_home_inject_* under the
     * recursive g_home_network mutex; telemetry/httpd read on core 0. */
    const BaseType_t rbamp_poll_core = 1;  /* core 1 free → dedicate to metering */
    s_running = true;
    rbpower_set_pre_restart_hook(bridge_pre_restart_flush);
    if (xTaskCreatePinnedToCore(rbamp_bridge_poll_task, "rbamp_poll", 8192, NULL,
                                tskIDLE_PRIORITY + 1, &s_task, rbamp_poll_core) != pdPASS) {
        ESP_LOGE(TAG, "failed to create poll task");
        s_running = false; err = ESP_ERR_NO_MEM; goto fail_dev;
    }

    // Register the unbind hook: DELETE-ing an rbAmp unit clears its fleet entry's
    // unit_id + persists /rbamp.json so the unit does NOT respawn on the next boot.
    rbpower_register_unit_unbind_cb(rbamp_bridge_unbind_unit);
    rbpower_register_unit_ct_cb(rbamp_bridge_unit_set_ct);
    // Register the bind hook (symmetric): the bind API sets the fleet entry's
    // unit_id + persists /rbamp.json so the source resumes injecting + the unit survives reboot.
    rbpower_register_unit_bind_cb(rbamp_bridge_bind_unit);
    rbpower_register_unit_feed_rebind_cb(rbamp_bridge_feed_rebind);
    return ESP_OK;

fail_dev:
    for (uint8_t m = 0; m < s_dev_count; ++m) if (s_dev[m]) { if (!virt_is(m)) rbamp_del(s_dev[m]); s_dev[m] = NULL; }
    for (int b = 0; b < RBAMP_MAX_BUSES; ++b) if (s_bush[b]) { i2c_del_master_bus(s_bush[b]); s_bush[b] = NULL; }
    return err;
}

i2c_master_bus_handle_t rbamp_bridge_get_bus(void) { return s_bus; }

/* `modscan` verb backend: force an immediate re-probe of absent modules (synchronous — safe
 * vs the poll task, see probe_module/heal_absent_modules), then report per-module state +
 * healer counters. Returns chars written. */
int rbamp_bridge_rescan(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if (!s_running) return snprintf(out, cap, "bridge not running");
    uint8_t joined = heal_absent_modules();
    int n = snprintf(out, cap, "joined_now=%u attempts=%lu late_joins=%lu",
                     joined, (unsigned long)s_reprobe_attempts, (unsigned long)s_late_joins);
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES && n < (int)cap - 1; ++m) {
        if (s_cfg.modules[m].addr == 0) continue;
        if (s_dev[m]) {
            n = rb_scat(out, cap, n, " | m%u@0x%02X UP vhw=%d mode=%s", m, s_cfg.modules[m].addr,
                          s_has_vhw[m] ? 1 : 0,
                          s_energy_mode[m] == RBAMP_ENERGY_ONDEVICE ? "ondev" :
                          s_energy_mode[m] == RBAMP_ENERGY_HOST     ? "host" : "none");
        } else {
            n = rb_scat(out, cap, n, " | m%u@0x%02X ABSENT", m, s_cfg.modules[m].addr);
        }
    }
    return n;
}
void rbamp_bridge_bus_lock(void)   { bus_take(); }
void rbamp_bridge_bus_unlock(void) { bus_give(); }

/* Host-gain field-calibration trim. Live update of the fleet-wide host gains;
 * the poll task reads s_cfg every cycle, so a new gain applies on the next poll (no restart).
 * NaN/non-positive is rejected (a 0/negative gain would zero or invert all readings). Persistence
 * is the caller's job (rbgrid_storage_save_rbamp) — see the `calgain` verb. */
esp_err_t rbamp_bridge_set_host_gains(float v_gain, float i_gain)
{
    if (!(v_gain > 0.0f) || !(i_gain > 0.0f)) return ESP_ERR_INVALID_ARG;  /* also rejects NaN */
    s_cfg.voltage_gain = v_gain;
    s_cfg.current_gain = i_gain;
    ESP_LOGW(TAG, "host gains updated live: v_gain=%.4f i_gain=%.4f (applies next poll)",
             (double)v_gain, (double)i_gain);
    return ESP_OK;
}

void rbamp_bridge_get_host_gains(float *v_gain, float *i_gain)
{
    if (v_gain) *v_gain = s_cfg.voltage_gain;
    if (i_gain) *i_gain = s_cfg.current_gain;
}

/*  Unbind hook (registered with rbpower as the unit-unbind cb): clear the fleet
 * entry bound to unit_id (set unit_id=0) + persist /rbamp.json. Invoked by rbpower_home_remove_unit
 * on delete. No-op if unit_id isn't a fleet entry (e.g. an ESP-NOW unit). After this, boot-time
 * unit-creation skips the unbound entry (the unit doesn't respawn) and the bridge already skips
 * unit_id=0 in poll/inject.
 * @param[in] unit_id  Unit id to unbind. */
void rbamp_bridge_unbind_unit(rbpower_unit_id_t unit_id)
{
    if (unit_id == 0) return;
    bool changed = false;
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m) {
        for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
            if (s_cfg.modules[m].entries[e].unit_id == unit_id) {
                s_cfg.modules[m].entries[e].unit_id = 0;   /* unbind: sensor stays, binding cleared */
                rbpower_set_unit_rbamp_feed(unit_id, 0xFF, 0);   /* drop the source label */
                changed = true;
            }
        }
    }
    if (changed) {
        esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
        ESP_LOGW(TAG, "unbind unit 0x%04X → fleet entry cleared, /rbamp.json: %s",
                 (unsigned)unit_id, esp_err_to_name(se));
    }
}

/* Rebind hook for a bridge-fed unit (e.g. a main input whose definition carries no rbAmp source): re-point the fleet entry from the
 * old channel to (module, ch). The target channel must exist and carry no point. id/point/history stay. */
int rbamp_bridge_feed_rebind(rbpower_unit_id_t unit_id, uint8_t module, uint8_t ch)
{
    if (unit_id == 0 || module >= s_cfg.module_count || module >= RBAMP_MAX_MODULES) return -1;
    rbamp_entry_cfg_t *tgt = NULL;
    for (uint8_t e = 0; e < s_cfg.modules[module].entry_count && e < RBAMP_MAX_ENTRIES; ++e)
        if (s_cfg.modules[module].entries[e].source == ch && ch < RBAMP_MAX_CHANNELS) tgt = &s_cfg.modules[module].entries[e];
    if (!tgt) return -1;
    if (tgt->unit_id == unit_id) return 0;
    if (tgt->unit_id != 0) return -2;
    if (s_dev[module] && ch >= s_nc[module]) return -1;   /* the module in the slot has no such channel */
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m)
        for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e)
            if (s_cfg.modules[m].entries[e].unit_id == unit_id) s_cfg.modules[m].entries[e].unit_id = 0;
    tgt->unit_id = (uint16_t)unit_id;
    apply_default_phase(module, tgt);
    rbpower_set_unit_rbamp_feed(unit_id, module, ch);
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    publish_presence();
    rbpower_diag_printf("unit 0x%04X moved to m%u ch%u (history kept), /rbamp.json: %s", (unsigned)unit_id, module, ch,
                        esp_err_to_name(se));
    return se == ESP_OK ? 0 : -1;
}

/* Bind hook (registered as the unit-bind cb): set the fleet entry for the unit's (module, channel) so the
 * source resumes injecting + persists /rbamp.json. The hook receives the full definition — unit ids are
 * opaque handles, so the fleet entry is located by the unit's PHYSICS (device_uid = module index,
 * channel_idx = channel), not by recomputing an id from (m, source); the stored unit_id is 0 while unbound.
 * Non-I2C sources are a no-op. The poll/inject picks it up next cycle (gated on unit_id!=0); CT calibration
 * persists in the module's own flash, so readings resume immediately. */
void rbamp_bridge_bind_unit(const rbpower_unit_definition_t *def)
{
    if (!def || def->unit_id == 0) return;
    if (def->sources_count == 0 || def->sources[0].source_type != RBPOWER_SOURCE_I2C_SENSOR) return;
    uint8_t module = (uint8_t)def->device_uid;
    uint8_t ch     = def->channel_idx;
    bool changed = false;
    if (module < s_cfg.module_count && module < RBAMP_MAX_MODULES) {
        for (uint8_t e = 0; e < s_cfg.modules[module].entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
            rbamp_entry_cfg_t *en = &s_cfg.modules[module].entries[e];
            if (en->source == RBAMP_ENTRY_VOLTAGE) continue;
            if (en->source == ch && en->unit_id != (uint16_t)def->unit_id) {
                en->unit_id = (uint16_t)def->unit_id;   /* bind: resume inject into this unit */
                changed = true;
                apply_default_phase(module, en);        /* phase from the binding */
            }
        }
    }
    if (changed) {
        esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
        ESP_LOGW(TAG, "bind unit 0x%04X (mod %u ch %u) → fleet entry set, /rbamp.json: %s",
                 (unsigned)def->unit_id, module, ch, esp_err_to_name(se));
    } else {
        ESP_LOGW(TAG, "bind unit 0x%04X: no fleet entry for mod %u ch %u (or already bound)",
                 (unsigned)def->unit_id, module, ch);
    }
}

/** Set one channel's CT model on the user's request. A write happens only when the module holds another model (the
 * library skips an equal one) — then the shipped calibration is gone and entry.ct_user marks the channel for the
 * restart re-apply; choosing the model the channel already has changes nothing (factory calibration kept).
 * Fleet entry ct_code is the source of truth (probe re-provisions from it); applied live + saved on the module. */
static esp_err_t rbamp_bridge_channel_set_ct(uint8_t m, rbamp_entry_cfg_t *en, uint8_t ch, uint8_t ct_code, uint16_t unit_id)
{
    esp_err_t err = ESP_OK;
    bool wrote = false;
    if (s_dev[m] != NULL) {
        uint8_t cur = 0xFF;
        (void)rbamp_bridge_read_ct_model(m, ch, &cur);
        if (cur != ct_code) {
            if (!CONFIG_RBAMP_SCT_RATING_CORRECTION && !virt_is(m) && s_line[m] == 0 && s_sensor_class[m] == 1) {
                /* a stale RESET_OCCURRED (set since the module's power-on, never cleared) would make the restart watch
                 * re-apply right after this write — acknowledge it first; a restart after the clear
                 * sets it again (order: clear, then write) */
                bus_take(); (void)rbamp_clear_event_flags(s_dev[m], RBAMP_EVENT_RESET_OCCURRED); bus_give();
            }
            err = rbamp_bridge_set_ct_model(m, ch, ct_code);
            if (err == ESP_OK) err = rbamp_bridge_save_user_config(m);   /* module keeps it across its own restart */
            wrote = true;
        }
    }
    if (err != ESP_OK) {
        rbpower_diag_printf("FAIL m%u ch%u CT model %u: %s", m, ch, ct_code, esp_err_to_name(err));
        return err;
    }
    en->ct_code = ct_code;
    if (wrote && s_line[m] == 0 && s_sensor_class[m] == 1) {
        uint8_t uid[12];
        bus_take(); esp_err_t ue = br_read_uid(m, uid); bus_give();
        if (ue == ESP_OK) memcpy(s_cfg.modules[m].ct_uid, uid, 12);
        en->ct_user = 1;
    }
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    sct_publish(m);
    rbpower_diag_printf("m%u ch%u CT model -> %u (unit 0x%04X%s), /rbamp.json: %s", m, ch, ct_code,
                        (unsigned)unit_id, en->ct_user ? ", user choice — re-applied after a module restart" : "",
                        esp_err_to_name(se));
    return se;
}

/** CT model of a unit's channel (unit-CT callback: the sensor model is set in the app, not on the console). */
static esp_err_t rbamp_bridge_unit_set_ct(rbpower_unit_id_t unit_id, uint8_t ct_code)
{
    if (unit_id == 0) return ESP_ERR_INVALID_ARG;
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m) {
        for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
            rbamp_entry_cfg_t *en = &s_cfg.modules[m].entries[e];
            if (en->unit_id != (uint16_t)unit_id || en->source == RBAMP_ENTRY_VOLTAGE) continue;
            uint8_t ch = (en->source == RBAMP_ENTRY_PHASES) ? 0 : en->source;
            return rbamp_bridge_channel_set_ct(m, en, ch, ct_code, unit_id);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* `ctmodel <m> <ch> <code>` verb: the same on a channel without a unit (e.g. channels of a 7-channel module before binding) */
int rbamp_bridge_ctmodel(uint8_t m, uint8_t ch, uint8_t code, int mode, char *out, size_t cap)
{
    const bool factory = mode != 0, force = mode == 2;   /* 1 = factory (number only), 2 = force the preset */
    if (m >= s_cfg.module_count || m >= RBAMP_MAX_MODULES) return snprintf(out, cap, "ctmodel: bad module %u", m);
    const rbamp_sensor_model_info_t *mi = rbamp_sensor_model_lookup(s_sensor_class[m], code);
    if (!mi) return snprintf(out, cap, "ctmodel: model %u unknown for sensor class %u", code, s_sensor_class[m]);
    for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
        rbamp_entry_cfg_t *en = &s_cfg.modules[m].entries[e];
        if (en->source != ch) continue;
        if (factory && force) {
            /* the module already holds this model NUMBER but runs another gain (a BASIC restart reloads the shipped
             * calibration — e.g. 100 A chosen over the shipped 30 A model): force-write the preset, mark it the user's
             * choice on this module (re-applied after its restarts) */
            if (s_dev[m] == NULL) return snprintf(out, cap, "ctmodel: m%u not present", m);
            uint8_t uid[12];
            bus_take();
            (void)rbamp_clear_event_flags(s_dev[m], RBAMP_EVENT_RESET_OCCURRED);
            esp_err_t err = rbamp_set_ct_model_ch_force(s_dev[m], ch, code);
            esp_err_t ue = br_read_uid(m, uid);
            bus_give();
            if (err == ESP_OK) {
                en->ct_code = code; en->ct_user = 1;
                if (ue == ESP_OK) memcpy(s_cfg.modules[m].ct_uid, uid, 12);
                err = rbgrid_storage_save_rbamp(&s_cfg);
            }
            uint8_t rb = 0xFF;
            (void)rbamp_bridge_read_ct_model(m, ch, &rb);
            rbpower_diag_printf("m%u ch%u CT model %u force-written (user choice, re-applied after a module restart): %s", m, ch, code,
                                esp_err_to_name(err));
            return snprintf(out, cap, "ctmodel m%u ch%u -> %u %s forced (readback %u, user=1): %s", m, ch, code, mi->descriptor, rb,
                            esp_err_to_name(err));
        }
        if (factory) {
            /* the module was calibrated at the factory for this model: write only the model NUMBER back (the gain/NF in
             * its flash were never overwritten — user-mode SAVE keeps them), no ct_user, no
             * re-apply; the shipped calibration comes back at the module's next restart (`modreset`). */
            if (s_dev[m] == NULL) return snprintf(out, cap, "ctmodel: m%u not present", m);
            esp_err_t err = rbamp_bridge_set_ct_model(m, ch, code);
            if (err == ESP_OK) err = rbamp_bridge_save_user_config(m);
            if (err == ESP_OK) { en->ct_code = code; en->ct_user = 0; err = rbgrid_storage_save_rbamp(&s_cfg); }
            uint8_t rb = 0xFF;
            (void)rbamp_bridge_read_ct_model(m, ch, &rb);
            rbpower_diag_printf("m%u ch%u CT model -> %u (factory calibration, reloaded at the module restart): %s", m, ch, code,
                                esp_err_to_name(err));
            return snprintf(out, cap, "ctmodel m%u ch%u -> %u %s factory (readback %u, user=0): %s — run `modreset %u`", m, ch,
                            code, mi->descriptor, rb, esp_err_to_name(err), m);
        }
        esp_err_t err = rbamp_bridge_channel_set_ct(m, en, ch, code, en->unit_id);
        uint8_t rb = 0xFF;
        (void)rbamp_bridge_read_ct_model(m, ch, &rb);
        return snprintf(out, cap, "ctmodel m%u ch%u -> %u %s (readback %u, user=%u): %s", m, ch, code, mi->descriptor, rb,
                        en->ct_user, esp_err_to_name(err));
    }
    return snprintf(out, cap, "ctmodel: m%u has no entry for ch%u", m, ch);
}

/* `modreset` verb: CMD_RESET of module m (allowed in user mode) — it reloads its calibration from flash. The hub sees
 * RESET_OCCURRED on the next polls (user CT models, if any, are re-applied by the owner check). */
int rbamp_bridge_modreset(uint8_t m, char *out, size_t cap)
{
    if (m >= RBAMP_MAX_MODULES || s_dev[m] == NULL || virt_is(m)) return snprintf(out, cap, "modreset: m%u not present", m);
    if (rbamp_bridge_module_ota_busy(m)) return snprintf(out, cap, "modreset: m%u OTA busy", m);
    bus_take();
    esp_err_t err = rbamp_reset(s_dev[m]);
    bus_give();
    rbpower_diag_printf("m%u module reset (CMD_RESET) by verb: %s", m, esp_err_to_name(err));
    return snprintf(out, cap, "modreset m%u: %s", m, esp_err_to_name(err));
}

/* `drdy` verb: status, or set the line (-1 = off), persisted in /rbamp.json and applied to every module now */
int rbamp_bridge_drdy(int set_gpio, bool do_set, char *out, size_t cap)
{
    if (do_set) {
        s_cfg.drdy_gpio_p1 = (set_gpio >= 0 && set_gpio <= 39) ? (uint8_t)(set_gpio + 1) : 0;
        (void)rbgrid_storage_save_rbamp(&s_cfg);
        for (uint8_t m = 0; m < s_dev_count; ++m) {
            if (s_cfg.drdy_gpio_p1) drdy_attach(m);
            else if (s_dev[m] && !virt_is(m)) (void)rbamp_set_drdy_gpio(s_dev[m], -1);
        }
    }
    int n = snprintf(out, cap, "drdy gpio=%d", s_cfg.drdy_gpio_p1 ? (int)s_cfg.drdy_gpio_p1 - 1 : -1);
    for (uint8_t m = 0; m < s_dev_count; ++m)
        n = rb_scat(out, cap, n, " | m%u edge=%lu poll=%lu timeout=%lu missed=%lu inject calls=%lu ok=%lu nan=%lu err=%lu(last %d)",
                    m, (unsigned long)s_drdy_edge[m], (unsigned long)s_drdy_poll[m], (unsigned long)s_drdy_timeout[m],
                    (unsigned long)s_drdy_missed[m], (unsigned long)s_inj_calls[m], (unsigned long)s_inj_ok[m],
                    (unsigned long)s_inj_nan[m], (unsigned long)s_inj_err[m], s_inj_last_err[m]);
    for (uint8_t m = 0; m < s_dev_count; ++m)
        n = rb_scat(out, cap, n, " | m%u last snapshot: channels=%u implausible=0x%02X I0=%.3f | read ms: read_all %lu (max %lu) ch3+ %lu (max %lu)", m,
                    s_inj_last_nch[m], s_inj_last_impl[m], (double)s_inj_last_i0[m],
                    (unsigned long)s_rd_all_ms_last[m], (unsigned long)s_rd_all_ms_max[m],
                    (unsigned long)s_rd_sen_ms_last[m], (unsigned long)s_rd_sen_ms_max[m]);
    return n;
}

int rbamp_bridge_digest(uint8_t m, int n, char *out, size_t cap)
{
    if (m >= s_dev_count || !s_dev[m] || virt_is(m)) return snprintf(out, cap, "digest: no module %u", m);
    int len = snprintf(out, cap, "digest m%u", m);
    int last_seq = -1;
    for (int i = 0; i < n; ++i) {
        rbamp_drdy_event_t ev = {0};
        rbamp_digest_t d = {0};
        bus_take();
        esp_err_t we = rbamp_wait_data_ready(s_dev[m], 250, &ev);
        int64_t t0 = esp_timer_get_time();
        esp_err_t de = rbamp_read_digest(s_dev[m], &d);
        int64_t us = esp_timer_get_time() - t0;
        bus_give();
        if (de != ESP_OK) { len = rb_scat(out, cap, len, " | #%d wait %s read %s", i, esp_err_to_name(we), esp_err_to_name(de)); if (de == ESP_ERR_NOT_SUPPORTED) break; continue; }
        int step = last_seq < 0 ? 0 : (uint8_t)(d.seq - last_seq);
        last_seq = d.seq;
        len = rb_scat(out, cap, len, " | #%d seq %u (+%d) %s P %d W I %.2f A U %.1f V st 0x%02X %lld us", i, d.seq, step,
                      we == ESP_OK ? (ev.by_edge ? "edge" : "poll") : esp_err_to_name(we), d.p_w[0], d.i_a[0], d.u_v, d.status, (long long)us);
    }
    rbamp_snapshot_t s;
    bus_take();
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = br_read_all(m, &s);
    int64_t us = esp_timer_get_time() - t0;
    bus_give();
    if (e == ESP_OK) len = rb_scat(out, cap, len, " | read_all P %.2f W I %.3f A U %.1f V %lld us", s.power[0], s.current[0], s.voltage, (long long)us);
    else len = rb_scat(out, cap, len, " | read_all %s", esp_err_to_name(e));
    return len;
}

bool rbamp_bridge_module_status(uint8_t m, uint8_t *addr, bool *has_vhw, uint8_t *energy_mode)
{
    if (m >= RBAMP_MAX_MODULES || s_dev[m] == NULL) return false;
    if (addr)        *addr        = s_cfg.modules[m].addr;
    if (has_vhw)     *has_vhw     = s_has_vhw[m];
    if (energy_mode) *energy_mode = (uint8_t)s_energy_mode[m];
    return true;
}

esp_err_t rbamp_bridge_set_ct_model(uint8_t module, uint8_t channel, uint8_t code)
{
    if (module >= RBAMP_MAX_MODULES || s_dev[module] == NULL) return ESP_ERR_INVALID_STATE;
    if (rbamp_bridge_module_ota_busy(module)) return ESP_ERR_INVALID_STATE;
    /* set sensor class (SCT013) BEFORE the CT model; the CT-model write applies the
     * module's factory NF/gain/shape. RAM-only until save_user_config. */
    bus_take();
    esp_err_t err = br_set_ct_model(module, channel, code);
    bus_give();
    ESP_LOGI(TAG, "module %u set_ct_model ch=%u code=%u -> %s", module, channel, code, rbamp_err_to_str(err));
    return err;
}

esp_err_t rbamp_bridge_read_ct_model(uint8_t module, uint8_t channel, uint8_t *out)
{
    if (module >= RBAMP_MAX_MODULES || s_dev[module] == NULL || out == NULL) return ESP_ERR_INVALID_STATE;
    bus_take();
    esp_err_t err = br_read_ct_model(module, channel, out);
    bus_give();
    return err;
}

/* Bring-up diagnostics: STD identity straight from the module + the bridge's per-phase cache. */
esp_err_t rbamp_bridge_std_info(uint8_t module, rbamp_std_info_t *out, float u_ph[3], float q_ph[3], uint8_t *nv, uint8_t *nc)
{
    if (module >= RBAMP_MAX_MODULES || s_dev[module] == NULL || !out) return ESP_ERR_INVALID_STATE;
    bus_take();
    esp_err_t e = br_std_info(module, out);
    bus_give();
    if (u_ph) memcpy(u_ph, s_u_ph[module], sizeof(float) * 3);
    if (q_ph) memcpy(q_ph, s_q_ph[module], sizeof(float) * 3);
    if (nv) *nv = s_nv[module];
    if (nc) *nc = s_nc[module];
    return e;
}

/* One-shot check of the rbamp-esp-idf 1.7.0 STD calls from the hub app (hardware validation of the library).
 * Start current: get, two out-of-range sets, 410 + read back, 400 + read back — the value applies at
 * once and is restored; nothing is saved to the module's NVM. */
/** @def LC
 *  @brief Append formatted text to the libcheck output (local to rbamp_bridge_libcheck()). */
void rbamp_bridge_libcheck(uint8_t m, char *out, size_t n)
{
    int p = 0;
/* append to the libcheck output */
#define LC(...) do { if (p < (int)n) p = rb_scat(out, n, p, __VA_ARGS__); } while (0)
    if (m >= RBAMP_MAX_MODULES || s_dev[m] == NULL || virt_is(m) || rbamp_bridge_module_ota_busy(m)) { LC("m%u: no real module / busy", m); return; }
    rbamp_handle_t dev = s_dev[m];
    uint16_t ua0 = 0, ua = 0;
    bus_take();
    esp_err_t e = rbamp_std_get_start_current(dev, &ua0);
    LC("get_start=%s %u", esp_err_to_name(e), ua0);
    LC(" | set2541=%s", esp_err_to_name(rbamp_std_set_start_current(dev, 2541)));
    LC(" set15=%s", esp_err_to_name(rbamp_std_set_start_current(dev, 15)));
    e = rbamp_std_set_start_current(dev, 410); rbamp_std_get_start_current(dev, &ua);
    { uint8_t r[2] = {0}; esp_err_t re = std_raw_read_locked(m, 0x0084, r, 2);   /* register, independent of the library */
      LC(" set410=%s rb=%u raw0084=%s %u", esp_err_to_name(e), ua, esp_err_to_name(re), (unsigned)(r[0] | (r[1] << 8))); }
    uint16_t back = (ua0 >= 20 && ua0 <= 2540) ? ua0 : 400;
    e = rbamp_std_set_start_current(dev, back); ua = 0; rbamp_std_get_start_current(dev, &ua);
    LC(" set%u=%s rb=%u", back, esp_err_to_name(e), ua);
    for (uint8_t v = 0; v < 3; ++v) {
        rbamp_std_voltage_t uv; memset(&uv, 0, sizeof(uv));
        e = rbamp_std_read_voltage(dev, v, &uv);
        if (e == ESP_OK) LC(" | v%u %.1fV ang=%.2f valid=%d", v, (double)uv.u_rms, (double)uv.angle_deg, (int)uv.angle_valid);
        else LC(" | v%u %s", v, esp_err_to_name(e));
    }
    rbamp_std_phase_seq_t ps = RBAMP_STD_PHASE_SEQ_NA;
    e = rbamp_std_read_phase_seq(dev, &ps);
    LC(" | phase_seq=%s %s", esp_err_to_name(e), ps == RBAMP_STD_PHASE_SEQ_FORWARD ? "FORWARD" : ps == RBAMP_STD_PHASE_SEQ_REVERSE ? "REVERSE" :
                                                 ps == RBAMP_STD_PHASE_SEQ_ABNORMAL ? "ABNORMAL" : "NA");
    rbamp_std_config_t cf; memset(&cf, 0, sizeof(cf));
    e = rbamp_std_read_config(dev, &cf);
    LC(" | config=%s start_ua=%u supported=%d", esp_err_to_name(e), cf.start_ua, (int)cf.start_ua_supported);
    rbamp_std_energy_t en; memset(&en, 0, sizeof(en));
    e = br_read_energy(m, &en);
    LC(" | energy=%s fine=%d fine_reactive=%d", esp_err_to_name(e), (int)en.fine, (int)en.fine_reactive);
    bus_give();
#undef LC
    for (const char *a = out; *a; ) {     /* one journal line per section (journal lines are 200 B) */
        const char *b = strstr(a, " | ");
        int len = b ? (int)(b - a) : (int)strlen(a);
        rbpower_diag_printf("m%u libcheck: %.*s", m, len, a);
        a = b ? b + 3 : a + len;
    }
}

/* SAVE_CAL (rbamp_save_gains): persist the module's CURRENT calibration registers (+ the cal_ok marker on STD
 * 0x7A+). Maintenance-level, explicit only, never automatic. Real modules only. */
esp_err_t rbamp_bridge_save_gains(uint8_t module)
{
    if (module >= RBAMP_MAX_MODULES || s_dev[module] == NULL || virt_is(module)) return ESP_ERR_INVALID_STATE;
    if (rbamp_bridge_module_ota_busy(module)) return ESP_ERR_INVALID_STATE;   /* module answers BUSY to other commands mid-OTA */
    bus_take();
    esp_err_t err = rbamp_save_gains(s_dev[module]);
    bus_give();
    rbpower_diag_printf("m%u SAVE_CAL (save_gains) -> %s", module, rbamp_err_to_str(err));
    if (err == ESP_OK) {   /* refresh the cached cal flag (modules[].cal) — it was read at probe time */
        rbpower_module_desc_t md; rbamp_std_info_t si;
        bus_take(); bool got = (rbamp_std_info(s_dev[module], &si) == ESP_OK); bus_give();
        if (got && rbpower_get_module_desc(module, &md)) { md.cal = (si.status & RBAMP_STD_ST_CAL_OK) ? 1 : 0; rbpower_set_module_desc(module, &md); }
    }
    return err;
}

esp_err_t rbamp_bridge_save_user_config(uint8_t module)
{
    if (module >= RBAMP_MAX_MODULES || s_dev[module] == NULL) return ESP_ERR_INVALID_STATE;
    if (rbamp_bridge_module_ota_busy(module)) return ESP_ERR_INVALID_STATE;
    bus_take();
    esp_err_t err = br_save_user_config(module);
    bus_give();
    ESP_LOGI(TAG, "module %u save_user_config -> %s", module, rbamp_err_to_str(err));
    return err;
}

/* Close one module's period window and inject finished RESULTS into RBgrid (bypasses
 * the 200ms accumulator). Two source paths by module variant:
 *   - voltage-bearing (UI1/2/3): ON-DEVICE period — latch + read avg_p/energy.
 *   - current-only  (I2/I3):     HOST-SIDE — close the V_bus×I integration window.
 * Targets the 15-min slot; higher periods aggregate from accumulated 15-min results. */
esp_err_t rbamp_bridge_collect_period(uint8_t module, rbpower_stat_period_type_t period)
{
    static const char* const STG[RBAMP_MAX_MODULES] = { "collect m0", "collect m1", "collect m2" };
    if (module < RBAMP_MAX_MODULES) rbpower_stats_stage(STG[module]);
    if (module >= RBAMP_MAX_MODULES || s_dev[module] == NULL) return ESP_ERR_INVALID_STATE;
    const rbamp_module_cfg_t *mc = &s_cfg.modules[module];
    float cg = s_cfg.current_gain;

    /* ── No energy capability (current-only, no fleet voltage source): skip cleanly. ── */
    if (s_energy_mode[module] == RBAMP_ENERGY_NONE) {
        ESP_LOGW(TAG, "module %u: no energy capability (current-only, no V_bus source) — period skipped", module);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* ── Current-only + fleet voltage: close the host-side V_bus×I integration window. ── */
    if (s_energy_mode[module] == RBAMP_ENERGY_HOST) {
        uint8_t injected = 0;
        for (uint8_t e = 0; e < mc->entry_count; ++e) {
            const rbamp_entry_cfg_t *en = &mc->entries[e];
            if (en->source == RBAMP_ENTRY_VOLTAGE || en->unit_id == 0) continue;
            /* Atomically SNAPSHOT + RESET the window under the spinlock (the poll task writes it via
             * host_accumulate), then compute + inject OUTSIDE the lock (inject takes the home mutex /
             * can be slow — must not run in a critical section). */
            host_acc_t acc;
            taskENTER_CRITICAL(&s_hostacc_mux);
            acc = s_hostacc[module][e];
            /* the next window continues from the LAST SAMPLE (last_us kept): zeroing last_us would make the next
             * sample only "open" the window, dropping one poll interval per quarter (~0.11 %) */
            s_hostacc[module][e].energy_wh = 0.0; s_hostacc[module][e].p_max = 0.0f;
            s_hostacc[module][e].i_min = INFINITY; s_hostacc[module][e].i_max = 0.0f;
            s_hostacc[module][e].win_start_us = acc.last_us;
            taskEXIT_CRITICAL(&s_hostacc_mux);
            /* avg_p divides energy by the ACTUAL integrated span [win_start, last_us] — NOT up to
             * "now" (the trailing gap has no samples and would dilute avg_p). Need ≥2 samples. */
            if (acc.last_us == 0 || acc.last_us <= acc.win_start_us) continue;
            uint32_t dt_ms = (uint32_t)((acc.last_us - acc.win_start_us) / 1000ULL);
            double dt_h = (double)dt_ms / 3600000.0;
            float avg_p = (dt_h > 0.0) ? (float)(acc.energy_wh / dt_h) : 0.0f;
            float e_wh  = (float)acc.energy_wh;
            /* host-side current envelope: avg current ≈ avg_p / V_bus; min/max from the window. */
            float avg_i = (s_vbus > 0.0f) ? (avg_p / s_vbus) : 0.0f;
            rbpower_home_set_next_period_end((rbpower_unit_id_t)en->unit_id, s_collect_q_end);
            rbpower_err_t ierr = rbpower_home_inject_period_result(
                (rbpower_unit_id_t)en->unit_id, period, avg_p, acc.p_max, e_wh, dt_ms,
                s_vbus, avg_i, acc.i_min, acc.i_max);
            if (ierr == RBPOWER_OK) injected++;
            ESP_LOGI(TAG, "m%u ch%u unit=0x%04X period(host): avg=%.2fW max=%.2fW E=%.4fWh dt=%lums Vbus=%.1f i=%.3f/%.3f -> %s",
                     module, en->source, (unsigned)en->unit_id, (double)avg_p, (double)acc.p_max,
                     (double)e_wh, (unsigned long)dt_ms, (double)s_vbus,
                     (double)acc.i_min, (double)acc.i_max,
                     ierr == RBPOWER_OK ? "ok" : "inject-fail");
        }
        ESP_LOGI(TAG, "module %u current-only: host-side energy, %u unit(s) injected", module, injected);
        return injected > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    /* ── Voltage-bearing module: on-device period snapshot. ──
     * Snapshot with a settle-margin retry. First read LATCHES (closes the window that
     * began at the previous latch, starts a new one). If the device's PERIOD_VALID bit
     * hasn't asserted yet (longer settle on I-only multi-channel variants), retry with
     * skip_latch=true — re-reads the SAME window WITHOUT restarting it, so we don't lose
     * accumulation. NOTE: this only covers the valid-bit settle margin; a genuinely too-
     * short window (< ~30 s since the previous latch on THIS module, per rbAmp's cadence
     * rule) is always stale by design — space period collects ≥30-60 s apart per module. */
    rbamp_period_snapshot_t snap = {0};
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    bool skip_latch = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        bus_take();
        err = br_period_snapshot(module, &snap,
                                         skip_latch ? 0 : 50, skip_latch);
        bus_give();
        if (err == ESP_OK && snap.valid) break;
        skip_latch = true;                       /* keep the window; just wait for VALID */
        vTaskDelay(pdMS_TO_TICKS(60 + 60 * attempt));
    }
    /* UI5/UI7: avg_p of ch3+ from the same latch (the module latches every channel atomically) */
    float hi_avg_p[RBAMP_MAX_CHANNELS];
    for (uint8_t c = 0; c < RBAMP_MAX_CHANNELS; ++c) hi_avg_p[c] = NAN;
    if (err == ESP_OK && snap.valid && !virt_is(module) && s_nc[module] > 3) {
        bus_take();
        for (uint8_t c = 3; c < s_nc[module] && c < RBAMP_MAX_CHANNELS; ++c)
            if (ch_bound(module, c) && rbamp_read_period_avg_power(s_dev[module], c, &hi_avg_p[c]) != ESP_OK) hi_avg_p[c] = NAN;
        bus_give();
    }
    if (err != ESP_OK || !snap.valid) {
        ESP_LOGW(TAG, "module %u (0x%02X) period snapshot stale/failed: %s (valid=%d) "
                      "— need >=30s since last latch on this module",
                 module, mc->addr, rbamp_err_to_str(err), (int)snap.valid);
        {   /* a quarter boundary < 35 s after the module joined is expected to be stale (the library
             * needs >= 30 s between consumed reads) — report it, but it is not a broken invariant */
            bool just_joined = s_probe_us[module] && (esp_timer_get_time() - s_probe_us[module]) < 35LL * 1000000LL;
            rbpower_diag_printf("%sQTR m%u snapshot %s valid=%d — quarter NOT injected%s", just_joined ? "" : "FAIL ",
                                module, rbamp_err_to_str(err), (int)snap.valid, just_joined ? " (module joined < 35 s ago)" : "");
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    s_last_collect_ok_us[module] = esp_timer_get_time();
    rbpower_stats_stage("collect: inject");
    /* BASIC resume window: rbamp-esp-idf 1.7.2-rc5 infers the commit period (100 ms on firmware >= v1.2, 200 before)
     * per snapshot — no hub-side ×100/×200 correction is needed. */

    {   /* BASIC quality: direction-less source; the first window after a hub restart is the
         * module's own (downtime recovered); > 6 h → float-average precision degrades → estimated. */
        rbpower_module_desc_t bd;
        if (!virt_is(module) && rbpower_get_module_desc(module, &bd) && bd.line == 0) {
            uint16_t qb = RBSTATS_Q_UNSIGNED_SRC;
            if (snap.module_window_ms && snap.master_dt_ms == snap.module_window_ms &&
                snap.module_window_ms > (uint32_t)RBAMP_BASIC_RESUME_LOG_MS) {
                rbpower_diag_printf("m%u resume window %lus%s (module uptime %lus): %.3fWh credited from the module's own average",
                                    module, (unsigned long)(snap.module_window_ms / 1000u),
                                    snap.window_from_boot ? " FROM MODULE BOOT" : "", (unsigned long)snap.module_uptime_s, snap.wh_net);
            }
            if (snap.module_restarted || snap.window_from_boot) {
                qb |= RBSTATS_Q_SRC_RESTART;
                rbpower_diag_printf("m%u BASIC module restarted (uptime %lus) — energy before its restart was not measured",
                                    module, (unsigned long)snap.module_uptime_s);
            }
            if (snap.window_long) {
                qb |= RBSTATS_Q_ESTIMATED;
                rbpower_diag_printf("m%u BASIC window %luh > 6 h — module float average, precision reduced (q ESTIMATED)",
                                    module, (unsigned long)(snap.module_window_ms / 3600000u));
            }
            flag_module_hour_bits(module, qb);
        }
    }

    /* ── Reboot-gap tail: energy the module counted while the hub was down. Folded INTO this first
     * regular quarter (energy + window) rather than injected as a period of its own — a separate
     * inject would count as an extra "quarter" (hour closed dur=3600/q=0 with a lost pre-reboot quarter)
     * and carry no electrical envelope (hour S/PF dragged down). ── */
    double   tail_wh[RBAMP_MAX_ENTRIES] = {0};
    uint32_t tail_gap_s = 0;
    /* Deferred arm (the counter read at boot failed): it runs AFTER this period's snapshot, so its tail
     * (NVS anchor → now) spans the downtime AND this whole first quarter (probe → latch). Added on top of the
     * quarter it would count the quarter twice — use the tail INSTEAD of the quarter. */
    bool tail_spans_quarter = false;
    if (s_tail_arm_pending[module]) {
        boot_tail_arm(module);   /* boot read failed → arm now (anchor still in NVS) */
        tail_spans_quarter = s_tail_valid[module];
    }
    /* no wall clock yet: keep the tail pending for a later collect instead of dropping it — the counters are
     * exact, only its placement needs the clock. The anchor is not re-saved while a tail is armed. */
    if (s_tail_valid[module] && !rbtime_is_time_valid()) {
        rbpower_diag_printf("m%u gap pending: no wall clock yet — tail kept for the next collect", module);
    } else if (s_tail_valid[module]) {
        s_tail_valid[module] = false;
        uint32_t now_ts = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
        uint32_t gap_s  = 0;
        bool clock_back = false;                                   /* wall clock now BEFORE the anchor (clock stepped back) */
        if (now_ts > s_tail_anchor_ts[module]) {
            uint32_t span = now_ts - s_tail_anchor_ts[module];
            gap_s = (span > snap.master_dt_ms / 1000u) ? span - snap.master_dt_ms / 1000u : 0;   /* window before the lib's own anchor */
        } else if (now_ts != 0) {
            clock_back = true;
        }
        /* counter-period module whose handle was not re-latched (module OTA): the quarter's own window
         * starts at or before the anchor, so the tail is already inside it. master_dt is truncated to whole
         * seconds → allow 2 s (e.g. span 500 s, dt 499.174 s → gap=1 would credit the tail twice).
         * A hub reboot re-latches at probe, so its gap is the boot outage (>= ~10 s), never this close. */
        rbpower_module_desc_t pdc;
        bool counters = rbpower_get_module_desc(module, &pdc) && pdc.period_src == RBAMP_STD_PERIOD_COUNTERS;
        if (counters && now_ts != 0 && !clock_back && gap_s <= 2) gap_s = 0;
        s_gap_diag[module].gap_s = gap_s;
        if (now_ts == 0 || gap_s == 0 || gap_s > RBAMP_GAP_MAX_S) {
            ESP_LOGW(TAG, "module %u: reboot-gap tail DROPPED (time_valid=%d gap=%lus)", module, (int)(now_ts != 0), (unsigned long)gap_s);
            s_gap_diag[module].state = 3;
            /* The tail is the module's COUNTER delta since the NVS anchor (measured, not estimated); the
             * lib re-latches the quarter at probe, so tail + quarter never overlap. gap=0 is therefore only
             * degenerate: nothing accumulated (tail ≈ 0 → "gap none", not a FAIL) or the wall clock now sits
             * before the anchor (e.g. the `clock` verb stepped it back) → FAIL with clock_back=1. */
            if (now_ts != 0 && gap_s == 0 && !clock_back && fabs(s_gap_diag[module].wh) < 0.01)
                rbpower_diag_printf("m%u gap none: module quarter covers the span (tail %.3fWh)", module, s_gap_diag[module].wh);
            else if (now_ts != 0 && gap_s == 0 && !clock_back && counters)
                /* counter-period module: this quarter IS the counter delta since the previous consumed read, which
                 * is at or before the anchor (module OTA keeps the handle — no re-latch) → the tail is already
                 * inside it; crediting it would count it twice */
                rbpower_diag_printf("m%u gap none: counter quarter already contains the tail %.3fWh", module, s_gap_diag[module].wh);
            else
                rbpower_diag_printf("FAIL m%u gap dropped: time_valid=%d clock_back=%d gap=%lus wh=%.3f", module,
                                    (int)(now_ts != 0), (int)clock_back, (unsigned long)gap_s, s_gap_diag[module].wh);
        } else {
            s_gap_diag[module].state = 2;
            tail_gap_s = gap_s;
            for (uint8_t e2 = 0; e2 < mc->entry_count && e2 < RBAMP_MAX_ENTRIES; ++e2) {
                const rbamp_entry_cfg_t *en = &mc->entries[e2];
                if (en->source == RBAMP_ENTRY_VOLTAGE || en->unit_id == 0) continue;
                double wh;
                if (!tail_entry_wh(module, &s_tail[module], en, &wh)) continue;
                tail_wh[e2] = wh * cg;
                ESP_LOGW(TAG, "m%u unit=0x%04X reboot-gap tail: E=%.3fWh over %lus folded into this quarter",
                         module, (unsigned)en->unit_id, tail_wh[e2], (unsigned long)gap_s);
            }
        }
    }
    /* Persist the counter reading that corresponds to this consumed period read (the next anchor). */
    {
        rbamp_std_energy_t enow;
        bus_take();
        esp_err_t ee = br_read_energy(module, &enow);
        bus_give();
        /* force: this quarter WAS injected, so the anchor must move even without a wall clock (ts 0 = the next
         * reboot gap is not creditable). Keeping the older anchor while quarters were injected would make the
         * next tail re-count all of them (no-NTP boot running for hours, then a reboot). */
        if (ee == ESP_OK) anchor_write(module, &enow, true);
        hourq_restore(module);
        srcid_check(module, "at quarter");
        if (rbpower_diag_enabled()) {                      /* quarter line: counters vs integral vs tail */
            double qcnt = NAN; bool qfine = false;
            if (ee == ESP_OK && s_diag_ref_valid[module]) {
                rbamp_std_energy_t d; bool rs = false;
                if (rbamp_std_energy_delta(&s_diag_ref[module], &enow, &d, &rs) == ESP_OK) { qcnt = diag_cnt_wh(&d); qfine = d.fine; }
            }
            double integ = diag_int_to_now(module), tail = 0.0;
            for (uint8_t e2 = 0; e2 < RBAMP_MAX_ENTRIES; ++e2) tail += tail_wh[e2];
            double base = isnan(qcnt) ? 0.0 : fmax(fabs(qcnt), fabs(integ));
            double dev  = (base > 0.0) ? (integ - qcnt) / base * 100.0 : 0.0;
            bool bad = (snap.master_dt_ms >= 60000u && base > 5.0 && fabs(dev) > 2.0 && s_diag_holes[module] == 0);   /* same window rule as the periodic line; a hole = no verdict */
            char dv[16];
            if (base < (qfine ? 0.001 : 0.1)) snprintf(dv, sizeof(dv), "n/a");   /* below: no verdict (see diag_poll) */
            else snprintf(dv, sizeof(dv), "%+.2f%%", dev);
            rbpower_diag_printf("%sQTR m%u dt=%lums cnt=%+.3fWh int=%+.3fWh dev=%s tail=%.3fWh gap=%lus anchor=%s",
                                bad ? "FAIL " : "", module, (unsigned long)snap.master_dt_ms, qcnt, integ, dv,
                                tail, (unsigned long)tail_gap_s, ee == ESP_OK ? "saved" : esp_err_to_name(ee));
            if (ee == ESP_OK) diag_ref_reset(module, &enow);
            else if (s_line[module] == 0) { s_diag_int_wh[module] = 0.0; s_diag_ref_us[module] = esp_timer_get_time(); s_diag_holes[module] = 0; }   /* BASIC: no counters → restart the integral per quarter (it ran on forever) */
        }
    }

    float dt_h = (float)snap.master_dt_ms / 3600000.0f;   /* ms -> hours (energy dt) */
    uint8_t injected = 0;
    for (uint8_t e = 0; e < mc->entry_count; ++e) {
        const rbamp_entry_cfg_t *en = &mc->entries[e];
        if (en->source == RBAMP_ENTRY_VOLTAGE || en->unit_id == 0) continue;  /* power/energy units only */
        uint8_t ch = en->source;
        float avg_p;
        if (en->source == RBAMP_ENTRY_PHASES) {
            /* grouped unit: Σ over the module's current channels (per-phase avg from the period snapshot) */
            float sum = 0.0f; uint8_t nn = 0;
            for (uint8_t c = 0; c < 3 && c < s_nc[module]; ++c) {
                if (isnan(snap.avg_p[c])) continue;
                sum += snap.avg_p[c] * cg; nn++;
            }
            if (nn == 0) continue;
            avg_p = sum;
            ch = 0;
        } else {
            float ap = (ch < 3) ? snap.avg_p[ch] : (ch < RBAMP_MAX_CHANNELS ? hi_avg_p[ch] : NAN);
            if (isnan(ap)) continue;
            avg_p = ap * cg;                                   /* W */
        }
        /* max_p is channel-0 only per the device spec; no per-channel peak otherwise. */
        float max_p = (ch == 0 && !isnan(snap.max_p)) ? snap.max_p * cg : avg_p;
        float wh    = avg_p * dt_h;                            /* Wh over the actual window */
        uint32_t dt_ms = snap.master_dt_ms;
        if (tail_gap_s && e < RBAMP_MAX_ENTRIES && tail_wh[e] != 0.0) {   /* fold the reboot gap in */
            wh    = tail_spans_quarter ? (float)tail_wh[e] : wh + (float)tail_wh[e];   /* a deferred tail replaces the quarter */
            /* Coverage: only the part of the gap inside the CURRENT hour is this hour's covered time.
             * A gap that began in an earlier hour (e.g. a long outage) would otherwise turn a few-minute
             * forced hour into dur=3600 q=0. The tail ENERGY is still credited
             * here (documented coarsening: the hub cannot split it by hour). */
            uint32_t gap_cov_s = tail_gap_s;
            {
                uint32_t now_ts2 = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
                if (now_ts2 > 2) now_ts2 -= 2;   /* a collect AT the boundary belongs to the ending hour (in_hour 0 would drop the gap from dur) */
                if (now_ts2) {
                    /* stats hours are UTC-aligned (hour bucket = epoch / 3600): localtime would be off by 30/45 min
                     * in IN/NP/AU-style zones */
                    uint32_t in_hour = now_ts2 % 3600u;
                    uint32_t q_s = snap.master_dt_ms / 1000u;
                    uint32_t room = (in_hour > q_s) ? in_hour - q_s : 0;
                    if (gap_cov_s > room) gap_cov_s = room;
                }
            }
            dt_ms += gap_cov_s * 1000u;
            avg_p  = wh / ((float)dt_ms / 3600000.0f);
            if (fabsf(avg_p) > fabsf(max_p)) max_p = avg_p;
        }
        /* BASIC creep gate: the module sums only positive 100 ms windows and has no creep
         * threshold, so an empty line accrues E[max(P,0)] ≈ +2.5 W (SCT-013-050). Below the published
         * indication level (0.2 % of the CT nominal × U) the period energy is 0. PHASES: one level per channel. */
        if (s_line[module] == 0 && !virt_is(module) && wh != 0.0f) {
            float nom = 0, fs_ = 0, pk_ = 0;
            if (rbamp_sensor_model_limits(s_sensor_class[module], en->ct_code, &nom, &fs_, &pk_) && nom > 0 && s_vbus > 0.0f) {
                uint8_t nch = (en->source == RBAMP_ENTRY_PHASES) ? s_nc[module] : 1;
                float creep_w = s_vbus * 0.002f * nom * (float)nch;
                if (fabsf(avg_p) < creep_w) {
                    rbpower_diag_printf("QTR m%u ch%u creep gate: avg=%.2fW < %.1fW, E %.3fWh -> 0",
                                        module, ch, (double)avg_p, (double)creep_w, (double)wh);
                    wh = 0.0f; avg_p = 0.0f; max_p = 0.0f;
                }
            }
        }
        /* envelope: voltage = cached v_bus; avg current ≈ avg_p / V. The device reports no
         * per-period current min/max → degenerate (min=max=avg), still better than a zero envelope. */
        float v_dev = s_vbus;
        float i_dev = (v_dev > 0.0f) ? (avg_p / v_dev) : 0.0f;

        {   /* period-record p_min = minimum 10-s rolling mean of this entry's channel (PHASES: the sum) */
            uint8_t k = (en->source == RBAMP_ENTRY_PHASES || en->source >= RBAMP_MAX_CHANNELS) ? PMIN_SUM : en->source;
            float pm = s_pmin_q[module][k];
            rbpower_home_set_next_power_min((rbpower_unit_id_t)en->unit_id, isnan(pm) ? NAN : pm * cg);
            /* PHASES entry: the module's max_p is CHANNEL 0 only (device spec) — p_max of a 3-channel point
             * would show about a third of the total. Use the hub's 10-s rolling maximum of the channel sum. */
            if (en->source == RBAMP_ENTRY_PHASES && !isnan(s_pmax_q[module][PMIN_SUM])) {
                float px = s_pmax_q[module][PMIN_SUM] * cg;
                max_p = (fabsf(px) > fabsf(avg_p)) ? px : avg_p;
            }
        }
        rbpower_home_set_next_period_end((rbpower_unit_id_t)en->unit_id, s_collect_q_end);
        rbpower_err_t ierr = rbpower_home_inject_period_result(
            (rbpower_unit_id_t)en->unit_id, period, avg_p, max_p, wh, dt_ms,
            v_dev, i_dev, i_dev, i_dev);
        if (ierr == RBPOWER_OK) injected++;
        ESP_LOGI(TAG, "m%u ch%u unit=0x%04X period: avg=%.2fW max=%.2fW E=%.4fWh dt=%lums -> %s",
                 module, ch, (unsigned)en->unit_id, (double)avg_p, (double)max_p,
                 (double)wh, (unsigned long)dt_ms,
                 ierr == RBPOWER_OK ? "ok" : "inject-fail");
    }
    ESP_LOGI(TAG, "module %u period collect: %u unit(s) injected (valid=%d)",
             module, injected, (int)snap.valid);
    pmin_reset(module);
    return injected > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void rbamp_bridge_stop(void)
{
    if (!s_running) return;
    s_running = false;
    for (int i = 0; i < 50 && s_task != NULL; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    for (uint8_t m = 0; m < s_dev_count; ++m) if (s_dev[m]) { if (!virt_is(m)) rbamp_del(s_dev[m]); s_dev[m] = NULL; }
    for (int b = 0; b < RBAMP_MAX_BUSES; ++b) if (s_bush[b]) { i2c_del_master_bus(s_bush[b]); s_bush[b] = NULL; }
    ESP_LOGI(TAG, "stopped");
}

/* `pollpause` verb: pause polling of a bound module for `secs` (0 = resume now). Produces a poll hole without
 * touching the bus — the module keeps counting, its quarter window continues (a hole test without unplugging
 * anything; a REAL disappearance additionally re-probes and arms the reboot-gap tail). */
int rbamp_bridge_poll_pause(uint8_t m, uint32_t secs)
{
    if (m >= RBAMP_MAX_MODULES) return -1;
    s_poll_pause_until_us[m] = secs ? esp_timer_get_time() + (int64_t)secs * 1000000LL : 0;
    return 0;
}

int rbamp_bridge_virt_cmd(int argc, char **argv, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
#if !CONFIG_RBGRID_BENCH_FULL
    (void)argc; (void)argv;
    return snprintf(out, cap, "vmod: development builds only");
#else
    if (argc < 2) {
        int n = snprintf(out, cap, "vmod:");
        for (uint8_t m = 0; m < RBAMP_MAX_MODULES && n < (int)cap - 1; ++m) {
            const virt_mod_t *v = &s_virt[m];
            if (v->magic != VIRT_MAGIC) { n = rb_scat(out, cap, n, " m%u=none", m); continue; }
            n = rb_scat(out, cap, n, " m%u=%s nch=%u p=%.0f/%.0f/%.0f v=%.0f gen=%lu imp=%.3f exp=%.3fWh up=%lus",
                          m, v->on ? "ON" : "off", v->nch, (double)v->p[0], (double)v->p[1], (double)v->p[2], (double)v->v[0],
                          (unsigned long)v->generation,
                          (v->imp_mwh[0] + v->imp_mwh[1] + v->imp_mwh[2]) / 1000.0,
                          (v->exp_mwh[0] + v->exp_mwh[1] + v->exp_mwh[2]) / 1000.0, (unsigned long)v->last_uptime_s);
        }
        return n;
    }
    uint32_t m = (uint32_t)strtoul(argv[1], NULL, 10);
    if (m >= RBAMP_MAX_MODULES || m >= s_cfg.module_count)
        return snprintf(out, cap, "vmod: module 0..%u", (unsigned)(s_cfg.module_count ? s_cfg.module_count - 1 : 0));
    virt_mod_t *v = &s_virt[m];
    const char *op = argc >= 3 ? argv[2] : "status";
    if (strcmp(op, "on") == 0) {
        if (s_dev[m] != NULL && !virt_is(m)) return snprintf(out, cap, "vmod m%lu: a REAL module is present in this slot", (unsigned long)m);
        uint32_t nch = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 3;
        float pw = argc >= 5 ? strtof(argv[4], NULL) : 350.0f;
        float vv = argc >= 6 ? strtof(argv[5], NULL) : 230.0f;
        if (nch < 1 || nch > 3) return snprintf(out, cap, "vmod: nch 1..3");
        if (v->magic != VIRT_MAGIC) { memset(v, 0, sizeof(*v)); v->magic = VIRT_MAGIC; v->generation = 1; }
        v->on = true; v->nch = (uint8_t)nch; v->pf = 1.0f;
        for (int c = 0; c < 3; ++c) { v->v[c] = vv; v->p[c] = (c < (int)nch) ? pw : 0.0f; }
        s_virt_last_us[m] = esp_timer_get_time(); v->last_wall_s = 0;   /* fresh start: no downtime credit */
        uint8_t joined = heal_absent_modules();
        return snprintf(out, cap, "vmod m%lu ON nch=%lu p=%.0fW v=%.0fV gen=%lu — probed=%u (period 0 / etstat / #DIAG apply)",
                        (unsigned long)m, (unsigned long)nch, (double)pw, (double)vv, (unsigned long)v->generation, joined);
    }
    if (v->magic != VIRT_MAGIC) return snprintf(out, cap, "vmod m%lu: not virtual (vmod %lu on [nch] [W] [V])", (unsigned long)m, (unsigned long)m);
    if (strcmp(op, "off") == 0) {
        v->on = false;
        if (s_dev[m] == (rbamp_handle_t)&s_virt_dummy[m]) s_dev[m] = NULL;
        classify_energy_modes(); publish_presence();
        return snprintf(out, cap, "vmod m%lu off (slot absent; counters kept in RTC)", (unsigned long)m);
    }
    if (strcmp(op, "p") == 0 && argc >= 5) {
        float w = strtof(argv[4], NULL);
        if (strcmp(argv[3], "all") == 0) { virt_tick((uint8_t)m); for (int c = 0; c < v->nch; ++c) v->p[c] = w; }
        else { uint32_t c = (uint32_t)strtoul(argv[3], NULL, 10); if (c >= v->nch) return snprintf(out, cap, "vmod: ch 0..%u", v->nch - 1); virt_tick((uint8_t)m); v->p[c] = w; }
        return snprintf(out, cap, "vmod m%lu p=%.0f/%.0f/%.0f W", (unsigned long)m, (double)v->p[0], (double)v->p[1], (double)v->p[2]);
    }
    if (strcmp(op, "v") == 0 && argc >= 4) {
        float vv = strtof(argv[3], NULL); for (int c = 0; c < 3; ++c) v->v[c] = vv;
        return snprintf(out, cap, "vmod m%lu v=%.1f V", (unsigned long)m, (double)vv);
    }
    if (strcmp(op, "restart") == 0) {   /* module reboot: counters persist (FRAM), uptime restarts */
        virt_tick((uint8_t)m);
        v->uptime_acc_s = -(esp_timer_get_time() / 1000000LL); v->last_uptime_s = 0; v->restarted_pending = true;
        return snprintf(out, cap, "vmod m%lu restarted (uptime 0, counters kept, next period reports module_restarted)", (unsigned long)m);
    }
    if (strcmp(op, "gen") == 0) {       /* new counter generation: continuity broken, counters start at 0 */
        v->generation++; for (int c = 0; c < 3; ++c) { v->imp_mwh[c] = v->exp_mwh[c] = 0; v->latch_imp[c] = v->latch_exp[c] = 0; }
        return snprintf(out, cap, "vmod m%lu generation=%lu, counters zeroed", (unsigned long)m, (unsigned long)v->generation);
    }
    return snprintf(out, cap, "vmod m%lu %s nch=%u p=%.0f/%.0f/%.0f v=%.0f gen=%lu imp=%.3f exp=%.3fWh up=%lus | ops: on [nch] [W] [V] · off · p <ch|all> <W> · v <V> · restart · gen",
                    (unsigned long)m, v->on ? "ON" : "off", v->nch, (double)v->p[0], (double)v->p[1], (double)v->p[2], (double)v->v[0],
                    (unsigned long)v->generation,
                    (v->imp_mwh[0] + v->imp_mwh[1] + v->imp_mwh[2]) / 1000.0,
                    (v->exp_mwh[0] + v->exp_mwh[1] + v->exp_mwh[2]) / 1000.0, (unsigned long)v->last_uptime_s);
#endif
}

/* ── I2C fleet management (modules on one or two buses; bus/pins/address chosen in the web app; available in the
 * code, not part of the v1.1.0 release feature set). Board profile: the ESP32 GPIO matrix routes an I2C controller
 * to any GPIO that can drive open-drain, so the choice is "what is free on THIS board". WROVER RBgrid: flash 6-11,
 * PSRAM 16/17, ext NOR 12-15 (12 = VDD_SDIO strap), straps 0/2/5, UART0 1/3, 34-39 input-only, DRDY (from the
 * config, e.g. GPIO33). ── */
static const uint8_t BUS_PINS_OK[] = { 4, 18, 19, 21, 22, 23, 25, 26, 27, 32 };
static const struct { uint8_t gpio; const char *use; } PIN_RSV[] = {
    { 0, "boot strap" }, { 1, "UART0 TX (console)" }, { 2, "boot strap" }, { 3, "UART0 RX (console)" }, { 5, "boot strap" },
    { 6, "SPI flash" }, { 7, "SPI flash" }, { 8, "SPI flash" }, { 9, "SPI flash" }, { 10, "SPI flash" }, { 11, "SPI flash" },
    { 12, "external flash MISO (VDD_SDIO strap)" }, { 13, "external flash MOSI" }, { 14, "external flash CLK" },
    { 15, "external flash CS" }, { 16, "PSRAM" }, { 17, "PSRAM" },
    { 34, "input only" }, { 35, "input only" }, { 36, "input only" },
    { 37, "input only" }, { 38, "input only" }, { 39, "input only" },
};
static volatile bool s_i2c_reboot_pending;

/** GPIO of bus b's SDA or SCL line from the config (-1 = bus 1 not configured). */
static int bus_pin(uint8_t b, bool scl)
{
    if (b == 0) return scl ? s_cfg.scl_gpio : s_cfg.sda_gpio;
    uint8_t p1 = scl ? s_cfg.bus1_scl_p1 : s_cfg.bus1_sda_p1;
    return p1 ? (int)p1 - 1 : -1;
}
/** Why a GPIO cannot carry an I2C line (NULL = it can); `self` = the bus being configured (its own pins are free) */
static const char *pin_blocked(int gpio, int self)
{
    for (size_t i = 0; i < sizeof(PIN_RSV) / sizeof(PIN_RSV[0]); ++i) if (PIN_RSV[i].gpio == gpio) return PIN_RSV[i].use;
    if (s_cfg.drdy_gpio_p1 && gpio == (int)s_cfg.drdy_gpio_p1 - 1) return "DRDY line";
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m)
        if (s_cfg.modules[m].drdy_p1 && gpio == (int)s_cfg.modules[m].drdy_p1 - 1) return "module DRDY line";
    for (int b = 0; b < RBAMP_MAX_BUSES; ++b)
        if (b != self && (gpio == bus_pin((uint8_t)b, false) || gpio == bus_pin((uint8_t)b, true))) return b ? "I2C bus 1" : "I2C bus 0";
    bool ok = false;
    for (size_t i = 0; i < sizeof(BUS_PINS_OK); ++i) if (BUS_PINS_OK[i] == gpio) ok = true;
    return ok ? NULL : "not offered on this board";
}

/** Does anything answer at addr? A 1-byte READ (address+R, one byte, NACK, STOP): no data byte is written, so a
 * PCF8574 only returns its port and an EEPROM its current byte. i2c_master_probe (IDF 5.4.1 + the project driver
 * patches) was seen reporting empty addresses as present — the read path's NACK handling is the proven one. Bus
 * held by the caller. */
static esp_err_t addr_answers_locked(uint8_t b, uint8_t addr)
{
    if (b >= RBAMP_MAX_BUSES || !s_bush[b]) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t d;
    uint8_t x = 0;
    esp_err_t e = i2c_master_bus_add_device(s_bush[b], &dc, &d);
    if (e == ESP_OK) { e = i2c_master_receive(d, &x, 1, 30); i2c_master_bus_rm_device(d); }
    return e;
}
/** Raw BASIC register read at an address the hub does not own yet (8-bit register pointer). Bus held by the caller. */
static esp_err_t raw_basic_read_locked(uint8_t b, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    if (b >= RBAMP_MAX_BUSES || !s_bush[b]) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t d;
    esp_err_t e = i2c_master_bus_add_device(s_bush[b], &dc, &d);
    if (e == ESP_OK) { e = i2c_master_transmit_receive(d, &reg, 1, buf, n, 50); i2c_master_bus_rm_device(d); }
    return e;
}
/** Fleet index of the module configured at (bus, addr), or -1. */
static int module_at(uint8_t b, uint8_t addr)
{
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m)
        if (s_cfg.modules[m].addr == addr && s_cfg.modules[m].bus == b) return m;
    return -1;
}
static void hex12(const uint8_t *u, char out[25]) { for (int i = 0; i < 12; ++i) snprintf(out + 2 * i, 3, "%02x", u[i]); }

cJSON *rbamp_bridge_i2c_state(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON *buses = cJSON_AddArrayToObject(r, "buses");
    for (int b = 0; b < RBAMP_MAX_BUSES; ++b) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "bus", b);
        cJSON_AddNumberToObject(o, "port", b == 0 ? s_cfg.i2c_port : (s_cfg.i2c_port == 1 ? 0 : 1));
        bool cfgd = bus_pin((uint8_t)b, false) >= 0;
        cJSON_AddBoolToObject(o, "configured", cfgd);
        if (cfgd) { cJSON_AddNumberToObject(o, "sda", bus_pin((uint8_t)b, false)); cJSON_AddNumberToObject(o, "scl", bus_pin((uint8_t)b, true)); }
        cJSON_AddBoolToObject(o, "up", s_bush[b] != NULL);
        cJSON_AddItemToArray(buses, o);
    }
    cJSON *ok = cJSON_AddArrayToObject(r, "pins_ok");
    for (size_t i = 0; i < sizeof(BUS_PINS_OK); ++i) cJSON_AddItemToArray(ok, cJSON_CreateNumber(BUS_PINS_OK[i]));
    cJSON *rsv = cJSON_AddArrayToObject(r, "pins_reserved");
    for (int g = 0; g <= 39; ++g) {
        if (g >= 20 && g <= 24 && g != 21 && g != 22 && g != 23) continue;   /* 20/24 do not exist on ESP32 */
        const char *why = pin_blocked(g, -1);
        if (!why || !strcmp(why, "not offered on this board")) continue;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "gpio", g); cJSON_AddStringToObject(p, "use", why);
        cJSON_AddItemToArray(rsv, p);
    }
    if (s_cfg.drdy_gpio_p1) cJSON_AddNumberToObject(r, "drdy_gpio", s_cfg.drdy_gpio_p1 - 1);
    cJSON_AddNumberToObject(r, "max_modules", RBAMP_MAX_MODULES);
    cJSON_AddBoolToObject(r, "reboot_pending", s_i2c_reboot_pending);
    cJSON *mods = cJSON_AddArrayToObject(r, "modules");
    for (uint8_t m = 0; m < s_cfg.module_count && m < RBAMP_MAX_MODULES; ++m) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "module", m);
        cJSON_AddNumberToObject(o, "bus", s_cfg.modules[m].bus);
        cJSON_AddNumberToObject(o, "addr", s_cfg.modules[m].addr);
        cJSON_AddBoolToObject(o, "present", s_dev[m] != NULL);
        int dg = s_cfg.modules[m].drdy_p1 ? s_cfg.modules[m].drdy_p1 - 1 : s_cfg.drdy_gpio_p1 ? s_cfg.drdy_gpio_p1 - 1 : -1;
        if (dg >= 0) cJSON_AddNumberToObject(o, "drdy_gpio", dg); else cJSON_AddNullToObject(o, "drdy_gpio");
        rbpower_module_desc_t d;
        if (rbpower_get_module_desc(m, &d) && d.model[0]) { cJSON_AddStringToObject(o, "model", d.model); cJSON_AddNumberToObject(o, "n_i", d.n_i); }
        int bound = 0;
        for (uint8_t e = 0; e < s_cfg.modules[m].entry_count && e < RBAMP_MAX_ENTRIES; ++e) if (s_cfg.modules[m].entries[e].unit_id) bound++;
        cJSON_AddNumberToObject(o, "points", bound);
        cJSON_AddItemToArray(mods, o);
    }
    return r;
}

/* Scan one bus. Address-only probe everywhere (no data byte reaches a foreign device — e.g. a PCF8574 port
 * expander takes any written byte as its outputs); identity reads (VERSION 0x03, PRODUCT_ID 0x54, UID 0x5C) only in
 * the rbAmp convention range 0x50-0x5F (reads are harmless; an EEPROM there only moves its pointer). Returns an array of {addr, module?, kind, fw?, uid?}. */
esp_err_t rbamp_bridge_i2c_scan(uint8_t b, cJSON *arr)
{
    if (b >= RBAMP_MAX_BUSES || !s_bush[b]) return ESP_ERR_INVALID_STATE;
    /* a slave stuck mid-byte holds SDA low and every address then "answers" (seen with internal pull-ups only):
     * clock it free first, and call a bus where every address answers stuck, not populated */
    bus_take(); esp_err_t re = i2c_master_bus_reset(s_bush[b]); bus_give();
    if (re != ESP_OK) rbpower_diag_printf("FAIL I2C bus %u reset before scan: %s", b, esp_err_to_name(re));
    int answered = 0;
    for (uint8_t a = 0x08; a <= 0x77; ++a) {
        bus_take();
        esp_err_t pe = addr_answers_locked(b, a);
        uint8_t ver = 0, prod = 0, uid[12] = {0};
        bool id_ok = false;
        int m = module_at(b, a);
        if (pe == ESP_OK && a >= 0x50 && a <= 0x5F && !(m >= 0 && s_dev[m] && virt_is((uint8_t)m))) {
            id_ok = raw_basic_read_locked(b, a, RBAMP_V2_REG_VERSION, &ver, 1) == ESP_OK &&
                    raw_basic_read_locked(b, a, RBAMP_V2_REG_PRODUCT_ID, &prod, 1) == ESP_OK &&
                    raw_basic_read_locked(b, a, RBAMP_V2_REG_UID, uid, 12) == ESP_OK;
        }
        bus_give();
        if (pe != ESP_OK) continue;
        if (++answered > 24) break;   /* no real bus carries this many devices: stuck SDA — reported by the caller */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "addr", a);
        if (m >= 0) cJSON_AddNumberToObject(o, "module", m); else cJSON_AddNullToObject(o, "module");
        const char *kind = "device";                      /* answered, not identified (outside 0x50-0x5F) */
        if (a >= 0x50 && a <= 0x5F) {
            if (id_ok && prod == 0x01 && rbamp_basic_fw_name(ver)) kind = "rbamp";
            else if (id_ok && prod == 0x02) kind = "rbdimmer";
            else kind = "unknown";
        }
        cJSON_AddStringToObject(o, "kind", kind);
        if (!strcmp(kind, "rbamp")) {
            char hx[25]; hex12(uid, hx);
            cJSON_AddStringToObject(o, "fw", rbamp_basic_fw_name(ver));
            cJSON_AddStringToObject(o, "uid", hx);
        }
        cJSON_AddItemToArray(arr, o);
        vTaskDelay(1);
    }
    if (answered > 24) {
        rbpower_diag_printf("FAIL I2C bus %u: every address answers — SDA held low (a module stuck mid-byte, or no/weak "
                            "pull-ups: add 4.7 kOhm to 3.3 V on SDA and SCL)", b);
        bus_take(); (void)i2c_master_bus_reset(s_bush[b]); bus_give();
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Add a module the user found by a scan: it must answer at (bus, addr) as an rbAmp. Joins at once (probe +
 * provision: an SCT- module's channels appear as unbound sensors). */
esp_err_t rbamp_bridge_i2c_add(uint8_t b, uint8_t addr, int *m_out, char *why, size_t wn)
{
    if (b >= RBAMP_MAX_BUSES || !s_bush[b]) { snprintf(why, wn, "bus %u is not running", b); return ESP_ERR_INVALID_STATE; }
    if (addr < 0x08 || addr > 0x77) { snprintf(why, wn, "address out of 0x08-0x77"); return ESP_ERR_INVALID_ARG; }
    if (module_at(b, addr) >= 0) { snprintf(why, wn, "a module is already configured at 0x%02X on bus %u", addr, b); return ESP_ERR_INVALID_STATE; }
    if (s_cfg.module_count >= RBAMP_MAX_MODULES) { snprintf(why, wn, "module limit %u reached", RBAMP_MAX_MODULES); return ESP_ERR_NO_MEM; }
    uint8_t prod = 0, ver = 0;
    bus_take();
    esp_err_t e = addr_answers_locked(b, addr);
    if (e == ESP_OK) e = raw_basic_read_locked(b, addr, RBAMP_V2_REG_PRODUCT_ID, &prod, 1);
    if (e == ESP_OK) e = raw_basic_read_locked(b, addr, RBAMP_V2_REG_VERSION, &ver, 1);
    bus_give();
    if (e != ESP_OK) { snprintf(why, wn, "nothing answers at 0x%02X on bus %u", addr, b); return ESP_ERR_NOT_FOUND; }
    if (prod != 0x01 || !rbamp_basic_fw_name(ver)) { snprintf(why, wn, "the device at 0x%02X is not an rbAmp module", addr); return ESP_ERR_INVALID_RESPONSE; }
    uint8_t m = s_cfg.module_count;
    memset(&s_cfg.modules[m], 0, sizeof(s_cfg.modules[m]));
    s_cfg.modules[m].addr = addr;
    s_cfg.modules[m].bus = b;
    s_dev[m] = NULL;
    pmin_reset(m);
    s_cfg.module_count++;
    if (s_dev_count < s_cfg.module_count) s_dev_count = s_cfg.module_count;
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    bool up = probe_module(m);
    classify_energy_modes(); publish_presence();
    rbpower_diag_printf("m%u added: bus %u 0x%02X (%s), /rbamp.json: %s", m, b, addr, up ? "joined" : "not joined yet", esp_err_to_name(se));
    if (m_out) *m_out = m;
    return se;
}

/* Change a module's I2C address (rbAmp procedure: prepare + commit within 5 s; the module saves the address,
 * restarts and answers ~0.3-0.5 s later; factory calibration/models kept). The new address must be free on its bus.
 * Verified by UID at the new address, then the module re-joins (begin_resume, DRDY, provisioning). */
esp_err_t rbamp_bridge_i2c_set_addr(uint8_t m, uint8_t addr, char *why, size_t wn)
{
    if (m >= s_cfg.module_count || !s_dev[m] || virt_is(m)) { snprintf(why, wn, "module %u is not present", m); return ESP_ERR_INVALID_STATE; }
    if (addr < 0x08 || addr > 0x77) { snprintf(why, wn, "address out of 0x08-0x77"); return ESP_ERR_INVALID_ARG; }
    if (rbamp_bridge_module_ota_busy(m)) { snprintf(why, wn, "module %u is updating", m); return ESP_ERR_INVALID_STATE; }
    uint8_t b = s_cfg.modules[m].bus, old = s_cfg.modules[m].addr;
    if (addr == old) return ESP_OK;
    if (module_at(b, addr) >= 0) { snprintf(why, wn, "0x%02X is configured for another module on bus %u", addr, b); return ESP_ERR_INVALID_STATE; }
    rbamp_handle_t dev = s_dev[m];
    uint8_t uid0[12] = {0}, uid1[12] = {0};
    bus_take();
    if (addr_answers_locked(b, addr) == ESP_OK) {
        bus_give(); snprintf(why, wn, "0x%02X is already taken on bus %u", addr, b); return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = rbamp_read_uid(dev, uid0);
    if (e == ESP_OK) e = rbamp_prepare_address_change(dev, addr);
    if (e == ESP_OK) e = rbamp_commit_address_change(dev);
    bool ans = false;
    if (e == ESP_OK) {
        for (int i = 0; i < 10 && !ans; ++i) { vTaskDelay(pdMS_TO_TICKS(150)); ans = rbamp_probe(dev) == ESP_OK; }
        if (ans && (rbamp_read_uid(dev, uid1) != ESP_OK || memcmp(uid0, uid1, 12) != 0)) ans = false;
    }
    if (ans) {
        s_cfg.modules[m].addr = addr;
        (void)rbamp_begin_resume(dev);
    }
    bus_give();
    if (e != ESP_OK || !ans) {
        snprintf(why, wn, "address change %s (%s) — the module may still answer at 0x%02X", e != ESP_OK ? "failed" : "not confirmed by UID",
                 esp_err_to_name(e), old);
        rbpower_diag_printf("FAIL m%u address 0x%02X -> 0x%02X: %s", m, old, addr, why);
        return e != ESP_OK ? e : ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    drdy_attach(m);
    provision_module(m, &s_cfg.modules[m]);
    rbpower_diag_printf("m%u address 0x%02X -> 0x%02X on bus %u (UID kept), /rbamp.json: %s", m, old, addr, b, esp_err_to_name(se));
    rbpower_hw_rev_bump();
    return se;
}

/* The user moved a module to the other bus (same address there): drop its handle and re-join on the new bus.
 * The old handle is not freed — another task may still hold the pointer (modota/libcheck cache it); a move is rare. */
esp_err_t rbamp_bridge_i2c_set_bus(uint8_t m, uint8_t b, char *why, size_t wn)
{
    if (m >= s_cfg.module_count || virt_is(m)) { snprintf(why, wn, "no module %u", m); return ESP_ERR_INVALID_ARG; }
    if (b >= RBAMP_MAX_BUSES || !s_bush[b]) { snprintf(why, wn, "bus %u is not running", b); return ESP_ERR_INVALID_STATE; }
    if (s_cfg.modules[m].bus == b) return ESP_OK;
    if (module_at(b, s_cfg.modules[m].addr) >= 0) {
        snprintf(why, wn, "bus %u already has a module at 0x%02X — change the address first", b, s_cfg.modules[m].addr);
        return ESP_ERR_INVALID_STATE;
    }
    if (rbamp_bridge_module_ota_busy(m)) { snprintf(why, wn, "module %u is updating", m); return ESP_ERR_INVALID_STATE; }
    bus_take();
    s_dev[m] = NULL;
    s_cfg.modules[m].bus = b;
    bus_give();
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    bool up = probe_module(m);
    classify_energy_modes(); publish_presence();
    rbpower_diag_printf("m%u moved to bus %u at 0x%02X (%s), /rbamp.json: %s", m, b, s_cfg.modules[m].addr,
                        up ? "joined" : "not answering there yet — healer re-probes", esp_err_to_name(se));
    return se;
}

/* Bus pins: validated against the board profile, persisted, applied at the next restart (the controllers are
 * created at boot). sda < 0 on bus 1 = bus 1 off (only when no module sits on it). */
esp_err_t rbamp_bridge_i2c_set_pins(uint8_t b, int sda, int scl, char *why, size_t wn)
{
    if (b >= RBAMP_MAX_BUSES) { snprintf(why, wn, "bus 0 or 1"); return ESP_ERR_INVALID_ARG; }
    if (b == 1 && sda < 0) {
        for (uint8_t m = 0; m < s_cfg.module_count; ++m)
            if (s_cfg.modules[m].bus == 1) { snprintf(why, wn, "module %u sits on bus 1 — move it first", m); return ESP_ERR_INVALID_STATE; }
        s_cfg.bus1_sda_p1 = s_cfg.bus1_scl_p1 = 0;
    } else {
        if (sda == scl) { snprintf(why, wn, "SDA and SCL must differ"); return ESP_ERR_INVALID_ARG; }
        const char *w;
        if ((w = pin_blocked(sda, b))) { snprintf(why, wn, "GPIO%d: %s", sda, w); return ESP_ERR_INVALID_ARG; }
        if ((w = pin_blocked(scl, b))) { snprintf(why, wn, "GPIO%d: %s", scl, w); return ESP_ERR_INVALID_ARG; }
        if (b == 0) { s_cfg.sda_gpio = sda; s_cfg.scl_gpio = scl; }
        else { s_cfg.bus1_sda_p1 = (uint8_t)(sda + 1); s_cfg.bus1_scl_p1 = (uint8_t)(scl + 1); }
    }
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    if (se == ESP_OK) s_i2c_reboot_pending = true;
    rbpower_diag_printf("I2C bus %u pins -> SDA%d/SCL%d (applies after restart), /rbamp.json: %s", b, sda, scl, esp_err_to_name(se));
    return se;
}

/* A module's own DRDY line (GPIO, -1 = back to the fleet line). Applied live. */
esp_err_t rbamp_bridge_i2c_set_drdy(uint8_t m, int gpio, char *why, size_t wn)
{
    if (m >= s_cfg.module_count || virt_is(m)) { snprintf(why, wn, "no module %u", m); return ESP_ERR_INVALID_ARG; }
    if (gpio >= 0) {
        const char *w = (s_cfg.modules[m].drdy_p1 && gpio == s_cfg.modules[m].drdy_p1 - 1) ? NULL : pin_blocked(gpio, -1);
        if (w && !(s_cfg.drdy_gpio_p1 && gpio == s_cfg.drdy_gpio_p1 - 1)) { snprintf(why, wn, "GPIO%d: %s", gpio, w); return ESP_ERR_INVALID_ARG; }
    }
    s_cfg.modules[m].drdy_p1 = gpio >= 0 ? (uint8_t)(gpio + 1) : 0;
    esp_err_t se = rbgrid_storage_save_rbamp(&s_cfg);
    if (s_dev[m]) { bus_take(); drdy_attach(m); bus_give(); }
    return se;
}
