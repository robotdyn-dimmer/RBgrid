/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_logic_test.c
 * @brief On-device regression suite for the deterministic billing and statistics logic.
 *
 * Self-contained mini-harness (no Unity), per the project's inline-test convention.
 * Emits "RBTEST logic SUMMARY: <P> passed, <F> failed" for an automated verdict.
 *
 * SCOPE: pure, deterministic functions only (no globals/HW/timing): the 4-flow merit-order
 * attribution (rbpower_compute_energy_balance), local-calendar period buckets, v3->v4 record
 * converters and point slugs. Stateful/integration logic (TOU zone selection, net-metering
 * banking, progressive tiers, period aggregation) is exercised on hardware with console verbs
 * and an accelerated statistics clock; see documents/development/TEST_MATRIX.md.
 */

#include "rbpower_logic_test.h"
#include "rbhome_types.h"       /* rbpower_compute_energy_balance + 4-flow structs */
#include "rbstats_v4_types.h"   /* suite 3: v3->v4 record converters */

#include <math.h>
#include <time.h>     /* suite 2: localtime_r/mktime/tzset under per-case TZ */
#include <stdlib.h>   /* setenv/getenv */
#include <string.h>
#include "esp_log.h"

static const char *TAG = "RBLOGIC_TEST";

static int s_pass;
static int s_fail;

/* Failure names, readable AFTER the run via rbpower_logic_fail_name(): a console that owns UART0
 * with ESP_LOG disabled (CONSOLE_NONE) never shows the PASS/FAIL logs, so a bare FAIL count is not
 * debuggable there. Diagnostics must flow through a verb reply. */
#define RBT_MAX_FAIL_NAMES 16 ///< Number of failing-check names kept for rbpower_logic_fail_name()
static char s_fail_names[RBT_MAX_FAIL_NAMES][64];

/** Record one check: count it, log PASS/FAIL, and keep the name of a failing check. */
#define RBT_CHECK(cond, name)                       \
    do {                                            \
        if (cond) { s_pass++; ESP_LOGI(TAG, "PASS: %s", (name)); }   \
        else      {                                                   \
            if (s_fail < RBT_MAX_FAIL_NAMES) {                        \
                strncpy(s_fail_names[s_fail], (name), sizeof(s_fail_names[0]) - 1); \
                s_fail_names[s_fail][sizeof(s_fail_names[0]) - 1] = '\0';           \
            }                                                         \
            s_fail++; ESP_LOGE(TAG, "FAIL: %s", (name));              \
        }                                                             \
    } while (0)

/**
 * @brief Name of the i-th failing check of the LAST rbpower_logic_run_tests() run.
 * @param[in] i Failure index (0-based).
 * @return Static name string, or NULL past the end (or beyond RBT_MAX_FAIL_NAMES).
 */
const char *rbpower_logic_fail_name(int i)
{
    if (i < 0 || i >= s_fail || i >= RBT_MAX_FAIL_NAMES) return NULL;
    return s_fail_names[i];
}

/* float near-equality for energy-flow comparisons (Watts; 0.01 W is far below any real signal). */
static inline int near(float a, float b) { return fabsf(a - b) < 0.01f; }

/* Run rbpower_compute_energy_balance for one scenario and assert all 8 outputs. */
static void check_balance(const char *name,
                          float p_grid, float p_pv, float p_batt, float p_load,
                          float e_pv2load, float e_pv2grid, float e_chg_pv, float e_chg_grid,
                          float e_dis2load, float e_dis2grid, float e_grid2load, float e_scr)
{
    rbpower_energy_balance_input_t in = { .p_grid = p_grid, .p_pv = p_pv, .p_batt = p_batt, .p_load = p_load };
    rbpower_energy_4flow_t o;
    rbpower_compute_energy_balance(&in, &o);
    int ok = near(o.pv_to_load, e_pv2load) && near(o.pv_to_grid, e_pv2grid) &&
             near(o.charge_from_pv, e_chg_pv) && near(o.charge_from_grid, e_chg_grid) &&
             near(o.discharge_to_load, e_dis2load) && near(o.discharge_to_grid, e_dis2grid) &&
             near(o.grid_to_load, e_grid2load) && near(o.self_consumption_ratio, e_scr);
    if (!ok) {
        ESP_LOGE(TAG, "  got: pv2l=%.2f pv2g=%.2f chgpv=%.2f chggrid=%.2f dis2l=%.2f dis2g=%.2f grid2l=%.2f scr=%.3f",
                 o.pv_to_load, o.pv_to_grid, o.charge_from_pv, o.charge_from_grid,
                 o.discharge_to_load, o.discharge_to_grid, o.grid_to_load, o.self_consumption_ratio);
    }
    RBT_CHECK(ok, name);
}

/* ===== Suite 1: 4-flow merit-order attribution (PV -> load -> battery -> grid) ===== */
static void suite_energy_balance(void)
{
    ESP_LOGI(TAG, "--- suite: energy_balance (4-flow merit-order) ---");

    /*                name                 grid    pv  batt  load | pv2l pv2g chgpv chggrid dis2l dis2g grid2l  scr */
    /* 1. Degenerate grid+load (no pv/batt) collapses to grid-only. */
    check_balance("grid-only: load fully from grid",
                  1000,    0,    0, 1000,   0,   0,    0,     0,    0,    0,  1000, 0.0f);
    /* 2. PV exactly covers load -> 100% self-consumption, no export, no grid. */
    check_balance("pv == load: full self-consumption",
                     0, 1000,    0, 1000, 1000,  0,    0,     0,    0,    0,     0, 1.0f);
    /* 3. PV surplus, no battery -> surplus exports. */
    check_balance("pv > load, no batt: surplus exports",
                     0, 1500,    0, 1000, 1000, 500,   0,     0,    0,    0,     0, 1.0f);
    /* 4. PV surplus charges battery first, remainder exports. */
    check_balance("pv surplus charges batt then exports",
                     0, 1500,  300, 1000, 1000, 200, 300,     0,    0,    0,     0, 1.0f);
    /* 5. PV surplus smaller than charge demand -> grid tops up the charge. */
    check_balance("batt charge topped up from grid",
                     0, 1200,  500, 1000, 1000,   0, 200,   300,    0,    0,     0, 1.0f);
    /* 6. Battery discharge covers part of load, grid covers the rest. */
    check_balance("batt discharge + grid serve load",
                   600,    0, -400, 1000,    0,   0,   0,     0,  400,    0,   600, 0.4f);
    /* 7. Battery discharge exceeds load -> serves load then exports. */
    check_balance("batt discharge serves load then exports",
                     0,    0, -500,  300,    0,   0,   0,     0,  300,  200,     0, 1.0f);
    /* 8. Grid-only battery charge (no pv, no load). */
    check_balance("grid charges battery, no load",
                   200,    0,  200,    0,    0,   0,   0,   200,    0,    0,     0, 0.0f);
    /* 9. Negative/garbage inputs are clamped to the physical domain (no NaN, no negative flows). */
    check_balance("negative inputs clamped to zero",
                     0, -100,    0,  -50,    0,   0,   0,     0,    0,    0,     0, 0.0f);
    /* 10. NaN inputs are rejected by the > comparisons (clamp to 0) -> all-zero, no NaN out. */
    {
        rbpower_energy_balance_input_t in = { .p_grid = 0, .p_pv = NAN, .p_batt = NAN, .p_load = NAN };
        rbpower_energy_4flow_t o;
        rbpower_compute_energy_balance(&in, &o);
        int clean = !isnan(o.pv_to_load) && !isnan(o.grid_to_load) && !isnan(o.self_consumption_ratio) &&
                    near(o.pv_to_load, 0) && near(o.grid_to_load, 0) && near(o.self_consumption_ratio, 0);
        RBT_CHECK(clean, "NaN inputs -> all-zero, no NaN propagated");
    }
    /* 11. NULL out is a safe no-op (no crash); NULL in zeroes out. */
    {
        rbpower_energy_4flow_t o;
        rbpower_compute_energy_balance(NULL, &o);
        RBT_CHECK(near(o.pv_to_load, 0) && near(o.grid_to_load, 0), "NULL in -> zeroed out (no crash)");
        rbpower_compute_energy_balance(NULL, NULL);   /* must not crash */
        RBT_CHECK(1, "NULL in & out -> no crash");
    }
    /* 12. Conservation: pv_to_load+pv_to_grid+charge_from_pv == p_pv, and load is fully served. */
    {
        rbpower_energy_balance_input_t in = { .p_grid = 0, .p_pv = 1500, .p_batt = 300, .p_load = 1000 };
        rbpower_energy_4flow_t o;
        rbpower_compute_energy_balance(&in, &o);
        float pv_accounted   = o.pv_to_load + o.pv_to_grid + o.charge_from_pv;
        float load_served    = o.pv_to_load + o.discharge_to_load + o.grid_to_load;
        RBT_CHECK(near(pv_accounted, 1500) && near(load_served, 1000),
                  "conservation: all PV accounted + load fully served");
    }
}

/* ===== Suite 2: local-calendar period buckets (five-time-zone matrix) =====
 * Pure test of rbpower_stats_calc_buckets under real POSIX TZ strings: the DAY bucket must flip
 * exactly at LOCAL midnight (incl. both DST transition days), WEEK at local Monday 00:00, and the
 * HOUR bucket must stay the UTC hour (visible on the half-hour zone). mktime() under the same TZ
 * builds the probe epochs, so expectations need no hand-computed constants. Saves/restores TZ. */
static uint64_t bk_of(time_t t, rbstats_bucket_idx_t which)
{
    struct tm lt;
    localtime_r(&t, &lt);
    uint64_t bk[RBSTATS_BK_COUNT];
    rbpower_stats_calc_buckets((int64_t)t, &lt, bk);
    return bk[which];
}

/* epoch of a local calendar moment under the CURRENT TZ */
static time_t local_epoch(int y, int mon, int d, int h, int mi, int s)
{
    struct tm lt = { .tm_year = y - 1900, .tm_mon = mon - 1, .tm_mday = d,
                     .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = -1 };
    return mktime(&lt);
}

static void tz_case(const char *name, const char *tz,
                    int y, int mon, int d /* a local date whose 00:00 we probe */,
                    int expect_day_len_h /* 24, 23 (spring-fwd) or 25 (fall-back); 0 = skip */)
{
    setenv("TZ", tz, 1);
    tzset();
    time_t mid = local_epoch(y, mon, d, 0, 0, 0);
    /* 1. DAY bucket flips exactly at local midnight. */
    RBT_CHECK(bk_of(mid - 1, RBSTATS_BK_DAY) + 1 == bk_of(mid, RBSTATS_BK_DAY), name);
    /* 2. The whole local day is ONE bucket (incl. 23/25h DST days): probe last second of the day. */
    time_t next_mid = local_epoch(y, mon, d + 1, 0, 0, 0);
    RBT_CHECK(bk_of(mid, RBSTATS_BK_DAY) == bk_of(next_mid - 1, RBSTATS_BK_DAY) &&
              bk_of(next_mid - 1, RBSTATS_BK_DAY) + 1 == bk_of(next_mid, RBSTATS_BK_DAY),
              "  day is one bucket to its last local second");
    /* 3. DST day length via the epoch distance between consecutive local midnights. */
    if (expect_day_len_h) {
        RBT_CHECK((int)((next_mid - mid) / 3600) == expect_day_len_h, "  DST day length (h)");
    }
}

static void suite_local_buckets(void)
{
    ESP_LOGI(TAG, "--- suite: local-calendar buckets (FR-C3) ---");
    char *old_tz = getenv("TZ");
    char saved[64] = {0};
    if (old_tz) { strncpy(saved, old_tz, sizeof(saved) - 1); }

    /* Europe/Berlin: spring-forward 2026-03-29 (23h) and fall-back 2026-10-25 (25h). */
    tz_case("Berlin: DAY flips at local midnight (spring-fwd day)",
            "CET-1CEST,M3.5.0,M10.5.0/3", 2026, 3, 29, 23);
    tz_case("Berlin: fall-back day", "CET-1CEST,M3.5.0,M10.5.0/3", 2026, 10, 25, 25);
    /* Monday alignment: 2026-03-30 is a Monday — WEEK bucket flips at ITS local midnight. */
    {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();
        time_t mon_mid = local_epoch(2026, 3, 30, 0, 0, 0);
        RBT_CHECK(bk_of(mon_mid - 1, RBSTATS_BK_WEEK) + 1 == bk_of(mon_mid, RBSTATS_BK_WEEK),
                  "Berlin: WEEK flips at local Monday 00:00");
        RBT_CHECK(bk_of(mon_mid, RBSTATS_BK_WEEK) == bk_of(local_epoch(2026, 4, 5, 23, 59, 59), RBSTATS_BK_WEEK),
                  "Berlin: Sunday 23:59:59 is still the same week");
        /* MONTH flips at local 1st 00:00. */
        time_t apr1 = local_epoch(2026, 4, 1, 0, 0, 0);
        RBT_CHECK(bk_of(apr1 - 1, RBSTATS_BK_MONTH) + 1 == bk_of(apr1, RBSTATS_BK_MONTH),
                  "Berlin: MONTH flips at local Apr 1 00:00");
    }
    /* Asia/Shanghai (+8, no DST): the flag-5 scenario — local midnight is 16:00 UTC. */
    tz_case("Shanghai (+8): DAY flips at local midnight", "CST-8", 2026, 10, 11, 24);
    /* America/New_York: transition at 02:00 local. */
    tz_case("New York: spring-fwd day", "EST5EDT,M3.2.0,M11.1.0", 2026, 3, 8, 23);
    /* Australia/Sydney: southern hemisphere, DST reversed (October = spring-forward). */
    tz_case("Sydney: spring-fwd day (southern)", "AEST-10AEDT,M10.1.0,M4.1.0/3", 2026, 10, 4, 23);
    /* Asia/Kolkata (+5:30): DAY local, HOUR stays the UTC hour (half-hour offset visible). */
    {
        setenv("TZ", "IST-5:30", 1); tzset();
        time_t mid = local_epoch(2026, 10, 11, 0, 0, 0);
        RBT_CHECK(bk_of(mid - 1, RBSTATS_BK_DAY) + 1 == bk_of(mid, RBSTATS_BK_DAY),
                  "Kolkata (+5:30): DAY flips at local midnight");
        /* local midnight = 18:30 UTC → inside UTC hour 18: the HOUR bucket must NOT flip there. */
        RBT_CHECK(bk_of(mid - 1, RBSTATS_BK_HOUR) == bk_of(mid, RBSTATS_BK_HOUR),
                  "Kolkata: HOUR bucket is the UTC hour (no flip at local midnight)");
    }

    /* Child-record mid-point attribution (the DAY+ recompute filter; guards a month-boundary bug):
     * a child record's ts = period END (store lags by up to ~1 quarter), so the recompute filter
     * attributes it by ts − half_span. The child ending exactly at a boundary — plus worst-case
     * lateness < half_span — must land in the CLOSING bucket, not the new one. */
    {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();
        time_t apr1 = local_epoch(2026, 4, 1, 0, 0, 0);
        uint64_t march = bk_of(apr1 - 1, RBSTATS_BK_MONTH);
        /* last HOUR of March 31: ts = midnight (+900s late) − 1800 half-span → March's DAY+ buckets */
        RBT_CHECK(bk_of(apr1 - 1800, RBSTATS_BK_DAY) + 1 == bk_of(apr1, RBSTATS_BK_DAY) &&
                  bk_of(apr1 + 900 - 1800, RBSTATS_BK_DAY) + 1 == bk_of(apr1, RBSTATS_BK_DAY),
                  "mid-point: boundary HOUR (even 900s late) lands in the closing DAY");
        /* last DAY of March: ts = Apr 1 00:00 (+900s) − 43200 → March MONTH bucket */
        RBT_CHECK(bk_of(apr1 - 43200, RBSTATS_BK_MONTH) == march &&
                  bk_of(apr1 + 900 - 43200, RBSTATS_BK_MONTH) == march,
                  "mid-point: boundary DAY lands in the closing MONTH");
        /* March itself as a MONTH child of Q-year: ts = Apr 1 − 15d → March, and year unchanged */
        RBT_CHECK(bk_of(apr1 - 1296000, RBSTATS_BK_MONTH) == march,
                  "mid-point: boundary MONTH (−15d) lands in the closing MONTH");
    }

    /* bucket spans (calendar-exact PARTIAL reference): DST day 23h, month across the
     * transition 31d−1h, week containing it 7d−1h, Kolkata plain 24h day. */
    {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();
        time_t d = local_epoch(2026, 3, 29, 12, 0, 0);   /* spring-fwd day */
        int64_t s = 0, e = 0;
        RBT_CHECK(rbpower_stats_bucket_span(RBSTATS_BK_DAY, bk_of(d, RBSTATS_BK_DAY), &s, &e) &&
                  (e - s) == 23 * 3600 && s == local_epoch(2026, 3, 29, 0, 0, 0),
                  "span: Berlin spring-fwd DAY = 23h from its local midnight");
        RBT_CHECK(rbpower_stats_bucket_span(RBSTATS_BK_MONTH, bk_of(d, RBSTATS_BK_MONTH), &s, &e) &&
                  (e - s) == 31 * 86400 - 3600,
                  "span: Berlin March = 31d - 1h (DST inside)");
        RBT_CHECK(rbpower_stats_bucket_span(RBSTATS_BK_WEEK, bk_of(d, RBSTATS_BK_WEEK), &s, &e) &&
                  (e - s) == 7 * 86400 - 3600,
                  "span: Berlin DST week = 7d - 1h");
        setenv("TZ", "IST-5:30", 1); tzset();
        time_t k = local_epoch(2026, 10, 11, 12, 0, 0);
        RBT_CHECK(rbpower_stats_bucket_span(RBSTATS_BK_DAY, bk_of(k, RBSTATS_BK_DAY), &s, &e) &&
                  (e - s) == 86400 && s == local_epoch(2026, 10, 11, 0, 0, 0),
                  "span: Kolkata DAY = 24h at half-hour offset");
    }

    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();
}

/* ===== Suite 3: v3->v4 record converters (record contract §1/§2/App.A) ===== */
static void suite_v4_convert(void)
{
    ESP_LOGI(TAG, "--- suite: v3->v4 record converters (T2.1) ---");

    /* Binary layout is pinned by _Static_asserts in rbstats_v4_types.h (11/41/43/31). */

    /* 1. energy: Wh -> mWh, MIGRATED set, dur = nominal, ts kept for wall-clock-era records. */
    {
        rbpower_energy_transfer_t v3 = { .timestamp = 1760140800u,
                                         .energy_consumed_wh = 1234, .energy_supplied_wh = 56,
                                         .energy_net_wh = 1178 };
        rbstats_energy_v4_t v4;
        rbstats_v4_conv_energy(&v3, RBPOWER_PERIOD_HOUR, &v4);
        RBT_CHECK(v4.ec_mwh == 1234000 && v4.es_mwh == 56000, "v4 energy: Wh -> mWh");
        RBT_CHECK(v4.h.ts == 1760140800u && v4.h.dur == 3600 && v4.h.q == RBSTATS_Q_MIGRATED &&
                  v4.h.res == 0, "v4 energy: hdr = {ts kept, dur nominal, q=MIGRATED, res=0}");
        RBT_CHECK(v4.et_in_mwh == 0 && v4.et_out_mwh == 0, "v4 energy: no et_* for migrated");
    }
    /* 2. uptime-era ts (never-synced v3 boot): ts PRESERVED (read-shim transparency) +
     * TIME_UNSYNC flagged — the region migrator shelves such records to ts=0 at rewrite. */
    {
        rbpower_energy_transfer_t v3 = { .timestamp = 7200u, .energy_consumed_wh = 10,
                                         .energy_supplied_wh = 0 };
        rbstats_energy_v4_t v4;
        rbstats_v4_conv_energy(&v3, RBPOWER_PERIOD_DAY, &v4);
        RBT_CHECK(v4.h.ts == 7200u && (v4.h.q & RBSTATS_Q_TIME_UNSYNC) && (v4.h.q & RBSTATS_Q_MIGRATED),
                  "v4 energy: uptime-era ts preserved + TIME_UNSYNC flag");
        RBT_CHECK(v4.h.dur == 86400, "v4 energy: DAY nominal dur");
        /* blank slot (erased flash) -> ts=0, readers skip */
        rbpower_energy_transfer_t blank = { .timestamp = 0xFFFFFFFFu,
                                            .energy_consumed_wh = 0xFFFFFFFFu,
                                            .energy_supplied_wh = 0xFFFFFFFFu };
        rbstats_v4_conv_energy(&blank, RBPOWER_PERIOD_DAY, &v4);
        RBT_CHECK(v4.h.ts == 0 && (v4.h.q & RBSTATS_Q_TIME_UNSYNC),
                  "v4 energy: blank slot -> ts=0 (reader-skip)");
    }
    /* 3. 0xFFFFFFFF field (uninitialized flash) maps to 0 + SRC_GAP, not 4.2 TWh. */
    {
        rbpower_energy_transfer_t v3 = { .timestamp = 1760140800u,
                                         .energy_consumed_wh = 0xFFFFFFFFu, .energy_supplied_wh = 5 };
        rbstats_energy_v4_t v4;
        rbstats_v4_conv_energy(&v3, RBPOWER_PERIOD_HOUR, &v4);
        RBT_CHECK(v4.ec_mwh == 0 && (v4.h.q & RBSTATS_Q_SRC_GAP) && v4.es_mwh == 5000,
                  "v4 energy: 0xFFFFFFFF field -> 0 + SRC_GAP");
    }
    /* 4. econ: kopecks are cents 1:1 (wider type), plan unknown -> 0. */
    {
        rbpower_economics_transfer_t v3 = { .timestamp = 1760140800u,
                                            .cost_consumed_kopecks = 992,
                                            .revenue_supplied_kopecks = 15,
                                            .net_cost_kopecks = 977 };
        rbstats_econ_v4_t v4;
        rbstats_v4_conv_econ(&v3, RBPOWER_PERIOD_MONTH, &v4);
        RBT_CHECK(v4.c_cents == 992 && v4.r_cents == 15 && v4.plan_id == 0 && v4.plan_ver == 0,
                  "v4 econ: cents 1:1, plan 0");
        RBT_CHECK(v4.h.dur == 2592000, "v4 econ: MONTH nominal dur (30d)");
    }
    /* 5. electrical: payload copied verbatim, present = all v3 fields. */
    {
        rbpower_electrical_transfer_t v3 = { .timestamp = 1760140800u,
                                             .voltage_rms_v10 = 2213, .current_rms_ma = 1144,
                                             .power_active_w10 = 2529, .power_reactive_var10 = -17,
                                             .power_apparent_va10 = 2531, .power_factor_100 = 100,
                                             .frequency_hz10 = 50, .voltage_min_v10 = 2207,
                                             .voltage_max_v10 = 2228, .power_min_w10 = -100,
                                             .power_max_w10 = 2516 };
        rbstats_electrical_v4_t v4;
        rbstats_v4_conv_electrical(&v3, RBPOWER_PERIOD_HOUR, &v4);
        RBT_CHECK(v4.voltage_rms_v10 == 2213 && v4.power_reactive_var10 == -17 &&
                  v4.power_min_w10 == -100 && v4.present == RBSTATS_EL_ALL_V3,
                  "v4 electrical: payload verbatim + present=ALL_V3");
        RBT_CHECK(v4.h.q == RBSTATS_Q_MIGRATED, "v4 electrical: q = MIGRATED only");
    }
    /* 6. YEAR/WEEK nominal durations. */
    RBT_CHECK(rbstats_v4_nominal_dur(RBPOWER_PERIOD_WEEK) == 604800 &&
              rbstats_v4_nominal_dur(RBPOWER_PERIOD_YEAR) == 31536000,
              "v4 nominal dur: WEEK 7d, YEAR 365d");
}

/* ===== Suite 4: point slugs — validation, translit generation, uniquify ===== */
#include "rbpoint.h"

static bool slug_taken_fixed(const char* slug, void* arg)
{
    const char** list = (const char**)arg;
    for (int i = 0; list[i]; i++) if (strcmp(list[i], slug) == 0) return true;
    return false;
}

static void suite_point_slug(void)
{
    ESP_LOGI(TAG, "--- suite: point slugs (T7) ---");
    char out[RBPOINT_SLUG_MAX + 1];

    /* validation */
    RBT_CHECK(rbpoint_slug_valid("kitchen_oven-2.a"), "valid slug accepted");
    RBT_CHECK(!rbpoint_slug_valid(""), "empty rejected");
    RBT_CHECK(!rbpoint_slug_valid("Kitchen"), "uppercase rejected");
    RBT_CHECK(!rbpoint_slug_valid("a b"), "space rejected");
    RBT_CHECK(!rbpoint_slug_valid("123456789012345678901234567890123"), ">32 rejected");
    RBT_CHECK(rbpoint_slug_valid("12345678901234567890123456789012"), "=32 accepted");

    /* generation: latin passthrough, fold + squeeze */
    rbpoint_slug_from_name("Kitchen  Oven (5A)", out);
    RBT_CHECK(strcmp(out, "kitchen_oven_5a") == 0, "latin fold+squeeze");
    /* Cyrillic transliteration (the Cyrillic input strings are intentional test data) */
    rbpoint_slug_from_name("Бойлер", out);
    RBT_CHECK(strcmp(out, "boyler") == 0, "cyrillic translit");
    rbpoint_slug_from_name("Щиток №2", out);
    RBT_CHECK(strcmp(out, "schitok_2") == 0, "translit sch + symbol squeeze");
    /* default name shape from add_rbamp_module */
    rbpoint_slug_from_name("rbAmp I2 ch0 (5A)", out);
    RBT_CHECK(strcmp(out, "rbamp_i2_ch0_5a") == 0, "bench default name");
    /* empty / all-garbage → "unit" */
    rbpoint_slug_from_name("***", out);
    RBT_CHECK(strcmp(out, "unit") == 0, "garbage -> unit");
    rbpoint_slug_from_name(NULL, out);
    RBT_CHECK(strcmp(out, "unit") == 0, "NULL -> unit");
    /* every generated slug must self-validate */
    rbpoint_slug_from_name("Ёмкостный датчик θ", out);
    RBT_CHECK(rbpoint_slug_valid(out), "generated slug always valid");

    /* uniquify */
    const char* taken1[] = { "boiler", NULL };
    rbpoint_slug_uniquify("boiler", out, slug_taken_fixed, (void*)taken1);
    RBT_CHECK(strcmp(out, "boiler-2") == 0, "uniquify -2");
    const char* taken2[] = { "boiler", "boiler-2", "boiler-3", NULL };
    rbpoint_slug_uniquify("boiler", out, slug_taken_fixed, (void*)taken2);
    RBT_CHECK(strcmp(out, "boiler-4") == 0, "uniquify first free");
    const char* taken0[] = { NULL };
    rbpoint_slug_uniquify("fridge", out, slug_taken_fixed, (void*)taken0);
    RBT_CHECK(strcmp(out, "fridge") == 0, "uniquify no-op when free");
    /* suffix must fit the 32-char cap */
    const char* taken32[] = { "12345678901234567890123456789012", NULL };
    rbpoint_slug_uniquify("12345678901234567890123456789012", out, slug_taken_fixed, (void*)taken32);
    RBT_CHECK(strlen(out) <= RBPOINT_SLUG_MAX && rbpoint_slug_valid(out), "uniquify respects cap");

    /* caps mapping */
    uint32_t c = rbpoint_caps_for_source(RBPOWER_SOURCE_I2C_SENSOR, RBPOWER_METRIC_CURRENT_LOAD, false);
    RBT_CHECK((c & (RBUNIT_CAP_CURRENT | RBUNIT_CAP_ACTIVE_POWER | RBUNIT_CAP_ENERGY |
                    RBUNIT_CAP_PF | RBUNIT_CAP_FREQ)) ==
              (RBUNIT_CAP_CURRENT | RBUNIT_CAP_ACTIVE_POWER | RBUNIT_CAP_ENERGY |
               RBUNIT_CAP_PF | RBUNIT_CAP_FREQ) && !(c & RBUNIT_CAP_REMOTE), "caps i2c current");
    c = rbpoint_caps_for_source(RBPOWER_SOURCE_ESP_NOW, RBPOWER_METRIC_CURRENT_LOAD, true);
    RBT_CHECK((c & RBUNIT_CAP_REMOTE) && (c & RBUNIT_CAP_BIDIRECTIONAL), "caps espnow bidi");
    c = rbpoint_caps_for_source(RBPOWER_SOURCE_ADC_SENSOR, RBPOWER_METRIC_VOLTAGE_BUS, false);
    RBT_CHECK((c & RBUNIT_CAP_VOLTAGE) && !(c & RBUNIT_CAP_CURRENT), "caps voltage bus");
}

int rbpower_logic_run_tests(void)
{
    s_pass = 0;
    s_fail = 0;
    ESP_LOGI(TAG, "=== rbpower deterministic logic regression suite ===");
    suite_energy_balance();
    suite_local_buckets();
    suite_v4_convert();
    suite_point_slug();
    ESP_LOGI(TAG, "RBTEST logic SUMMARY: %d passed, %d failed", s_pass, s_fail);
    return s_fail;
}
