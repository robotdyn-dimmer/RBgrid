/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_tariff.c
 * @brief Tariff engine: active-zone resolution, per-unit costing, net metering and progressive tiers.
 *
 * Holds the active tariff plan (a built-in default or the plan pushed by the tariff builder),
 * resolves the active time-of-use zone from local calendar time (rbtime owns the time zone), and
 * converts energy into money: consumption at the all-in buying rate (zone rate + per-kWh fees + VAT),
 * export at the selling/feed-in rate. Costs are booked into period result structures, the per-zone
 * breakdown, the lifetime accumulators and the monthly net-metering balance (persisted in NVS).
 *
 * Money values are in the plan currency (a display label only, no conversion); energy in kWh unless
 * a name says Wh; timestamps in epoch seconds unless the name says _us.
 *
 * Thread safety: module state (plan, zone cache, per-unit tables) is not protected by a lock.
 * Call the API from the statistics/processing task context.
 *
 * @version 1.0.0
 * @date 2025-01-10
 */

#include "rbpower.h"
#include "rbpower_enums.h"
#include "rbpower_constants.h"
#include "rbhome_types.h"
#include "rbhome_tariff.h"
#include "nvs.h"            /* net-metering billing-period state persisted across reboots */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "rbpower_lifetime_api.h"   /* lifetime cost booking */
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR — per-unit tariff arrays to PSRAM (scale with unit count) */
#include <sys/time.h>

//=============================================================================
// GLOBAL TARIFF SYSTEM VARIABLES
//=============================================================================

// Global tariff system configuration
static rbpower_tariff_global_config_t g_tariff_global_config = {0};

/** @brief Currently active tariff plan (default plan at init, replaced by the builder). Not lock-protected. */
rbpower_tariff_plan_t g_current_tariff_plan = {0};

// Extended tariff calendar
/* g_tariff_calendar (rbpower_tariff_calendar_extended_t, ~22 KB) removed — it was write-only
 * dead scaffolding. See the init site in rbpower_tariff_system_init. */

// Active tariff zone cache
static struct {
    rbpower_tariff_zone_type_t current_zone_id;        // Current active zone ID
    char current_zone_name[RBPOWER_TARIFF_NAME_LENGTH]; // Current zone name
    float current_buying_rate;                          // Current buying rate (currency/kWh)
    float current_selling_rate;                         // Current selling rate (currency/kWh)
    uint64_t zone_cache_timestamp;                      // Last cache update time
    bool zone_cache_valid;                              // Cache is valid
    uint64_t zone_activation_timestamp;                 // Current zone activation time
} g_active_zone_cache = {0};

// Tariff configuration for each unit. PSRAM (EXT_RAM_BSS_ATTR) — scales with the unit count.
EXT_RAM_BSS_ATTR static rbpower_tariff_unit_config_t g_unit_tariff_configs[RBPOWER_MAX_HOME_UNITS];

// Direct costs for each unit. PSRAM — scales with the unit count (cost_distribution_by_zone etc.).
EXT_RAM_BSS_ATTR static rbpower_unit_direct_costs_t g_unit_direct_costs[RBPOWER_MAX_HOME_UNITS];

/* Lean resident holiday list (month/day only) used by CONDITION_HOLIDAY and the WEEKEND/
 * HOLIDAY zone priority. ~64 bytes — replaces the 22 KB calendar that was removed; recurs
 * yearly. Configure via rbpower_tariff_set_holidays(). */
/** @brief Capacity of the resident holiday list (month/day entries). */
#define RBPOWER_HOLIDAY_MAX RBPOWER_TARIFF_HOLIDAYS_COUNT
static struct { uint8_t month; uint8_t day; } g_holidays[RBPOWER_HOLIDAY_MAX] = {0};
static uint8_t g_holiday_count = 0;

/* TEST scenario time override for the `tarifftime` diagnostic verb: -1 = use the live (virtual/real) clock;
 * >=0 = freeze the tariff calendar at this UNIX second so any zone/weekend/holiday scenario can be
 * tested instantly. Test-build only in practice (production never sets it). */
static int64_t g_tariff_test_time_override_s = -1;
void rbpower_tariff_set_test_time(int64_t unix_s) { g_tariff_test_time_override_s = unix_s; }

/* === NET-METERING (prosumer energy-credit accounting) ===
 * Per-unit billing-period (monthly) net balance + a carried-over kWh credit bank. Export over the
 * period banks credits (×net_metering_ratio, capped at max_credit_carry_over_kwh); net import draws
 * the credit balance down first, so the BILLABLE net import is reduced. Kept SEPARATE from the
 * per-15min gross cost (which stays as-validated) — net-metering is a billing-period overlay, settled
 * at the month boundary. The per-period gross cost is unchanged; net-metering is reported alongside. */
static struct {
    double period_consumed_kwh;          // import accumulated this billing period (kWh)
    double period_supplied_kwh;          // export accumulated this billing period (kWh)
    double credit_balance_kwh;           // carried-over credit bank (kWh)
    double lifetime_credits_earned_kwh;  // total credits ever banked
    double lifetime_credits_used_kwh;    // total credits ever spent offsetting import
    double last_billable_net_kwh;        // net import billed at the last settlement (after credits)
} g_net_metering[RBPOWER_MAX_HOME_UNITS];
/* Persistence: the billing-period import/export and the credit bank used to live in RAM only, so every
 * reboot dropped the quarters accumulated before it (the period total came up short after a mid-hour
 * restart while the hour record was exact). NVS ns "rbnm", key n<hex unit id>, written on every 15-min
 * costing and at settle; restored lazily on first touch after boot. */
static bool s_nm_restored[RBPOWER_MAX_HOME_UNITS];
static void nm_persist(uint8_t i);
static void nm_restore(uint8_t i);

/* Settle the net-metering balance for one unit at its billing-period boundary (monthly). Nets the
 * period import vs export; a net producer banks the surplus as credit (×ratio, capped); a net consumer
 * draws the credit bank down, reducing the billable net import. Resets the period accumulators. */
void rbpower_tariff_net_metering_settle(rbpower_unit_id_t unit_id)
{
    uint8_t i = find_unit_index_by_id(unit_id);
    if (i == 0xFF || i >= RBPOWER_MAX_HOME_UNITS) return;
    if (!g_current_tariff_plan.net_metering_enabled) return;
    nm_restore(i);

    double net = g_net_metering[i].period_consumed_kwh - g_net_metering[i].period_supplied_kwh;
    double ratio = g_current_tariff_plan.net_metering_default_ratio;
    if (ratio <= 0.0) ratio = 1.0;
    double maxc = g_current_tariff_plan.max_credit_carry_over_kwh;

    if (net < 0.0) {                       // net PRODUCER → bank the surplus as credit
        double earned = (-net) * ratio;
        g_net_metering[i].credit_balance_kwh += earned;
        g_net_metering[i].lifetime_credits_earned_kwh += earned;
        g_net_metering[i].last_billable_net_kwh = 0.0;
    } else {                               // net CONSUMER → offset with banked credits
        double offset = (net < g_net_metering[i].credit_balance_kwh) ? net : g_net_metering[i].credit_balance_kwh;
        g_net_metering[i].credit_balance_kwh -= offset;
        g_net_metering[i].lifetime_credits_used_kwh += offset;
        g_net_metering[i].last_billable_net_kwh = net - offset;   // reduced billable import
    }
    if (g_current_tariff_plan.carry_over_credits) {
        if (maxc > 0.0 && g_net_metering[i].credit_balance_kwh > maxc) g_net_metering[i].credit_balance_kwh = maxc;
    } else {
        g_net_metering[i].credit_balance_kwh = 0.0;   // no carry-over → credits expire each period
    }
    g_net_metering[i].period_consumed_kwh = 0.0;
    g_net_metering[i].period_supplied_kwh = 0.0;
    nm_persist(i);
    ESP_LOGW("RBHOME_TARIFF", "net-metering settle unit[%u]: billable_net=%.4f kWh, credit_bal=%.4f kWh",
             i, g_net_metering[i].last_billable_net_kwh, g_net_metering[i].credit_balance_kwh);
}

/* Read the net-metering state for a unit (`netmeter` diagnostic verb). Any out-pointer may be NULL. */
void rbpower_tariff_get_net_metering(rbpower_unit_id_t unit_id, double* period_consumed_kwh,
                                     double* period_supplied_kwh, double* credit_balance_kwh,
                                     double* lifetime_earned, double* lifetime_used, double* last_billable)
{
    uint8_t i = find_unit_index_by_id(unit_id);
    if (i == 0xFF || i >= RBPOWER_MAX_HOME_UNITS) {
        if (period_consumed_kwh) *period_consumed_kwh = 0.0;
        if (period_supplied_kwh) *period_supplied_kwh = 0.0;
        if (credit_balance_kwh)  *credit_balance_kwh  = 0.0;
        if (lifetime_earned)     *lifetime_earned     = 0.0;
        if (lifetime_used)       *lifetime_used       = 0.0;
        if (last_billable)       *last_billable       = 0.0;
        return;
    }
    nm_restore(i);
    if (period_consumed_kwh) *period_consumed_kwh = g_net_metering[i].period_consumed_kwh;
    if (period_supplied_kwh) *period_supplied_kwh = g_net_metering[i].period_supplied_kwh;
    if (credit_balance_kwh)  *credit_balance_kwh  = g_net_metering[i].credit_balance_kwh;
    if (lifetime_earned)     *lifetime_earned     = g_net_metering[i].lifetime_credits_earned_kwh;
    if (lifetime_used)       *lifetime_used       = g_net_metering[i].lifetime_credits_used_kwh;
    if (last_billable)       *last_billable       = g_net_metering[i].last_billable_net_kwh;
}

/* Progressive (block-tier) pricing — opt-in overlay on top of the zone buying rate.
 * Tiers are ordered by ascending threshold_kwh; the active tier is the highest whose
 * threshold the cumulative monthly consumption has already crossed. Returns the effective
 * buying rate for the next kWh. cumulative_kwh = this billing period's import so far (reuses
 * the net-metering monthly import accumulator → resets on the same monthly settle).
 * No-op (returns base_rate) when the plan's progressive_config is disabled. */
static float progressive_effective_rate(float base_rate, double cumulative_kwh)
{
    const rbpower_tariff_progressive_config_t* pc = &g_current_tariff_plan.progressive_config;
    if (!pc->is_enabled || pc->tiers_count == 0) return base_rate;
    uint8_t n = pc->tiers_count;
    if (n > RBPOWER_TARIFF_PROGRESSIVE_TIERS) n = RBPOWER_TARIFF_PROGRESSIVE_TIERS;
    int sel = 0;
    /* +EPS: thresholds are float (0.3f promotes to 0.30000001192) but the cumulative is a sum of
     * float→double increments (3×0.1f = 0.30000000447), so an exact-boundary threshold can sit just
     * above the accumulated sum and the tier would never engage. 1e-6 ≫ float noise, ≪ tier spacing. */
    for (uint8_t t = 0; t < n; t++) {
        if ((double)pc->tiers[t].threshold_kwh <= cumulative_kwh + 1e-6) sel = (int)t;
    }
    const rbpower_tariff_progressive_tier_t* tier = &pc->tiers[sel];
    if (tier->use_multiplier) return base_rate * tier->rate_multiplier;
    return tier->fixed_rate_per_kwh;
}

/* Test toggle for progressive pricing (`progmode` verb). Enables the tiered overlay in isolation
 * without disturbing the TOU/all-in path (default off in the test plan). */
void rbpower_tariff_set_progressive(bool enabled)
{
    g_current_tariff_plan.progressive_config.is_enabled = enabled;
}

/* Read progressive state for a unit (`progtier` diagnostic verb). Any out-pointer may be NULL. */
void rbpower_tariff_get_progressive(rbpower_unit_id_t unit_id, bool* enabled,
                                    double* cumulative_kwh, int* tier_index, float* multiplier)
{
    const rbpower_tariff_progressive_config_t* pc = &g_current_tariff_plan.progressive_config;
    if (enabled) *enabled = pc->is_enabled;
    double cum = 0.0;
    uint8_t i = find_unit_index_by_id(unit_id);
    if (i != 0xFF && i < RBPOWER_MAX_HOME_UNITS) cum = g_net_metering[i].period_consumed_kwh;
    if (cumulative_kwh) *cumulative_kwh = cum;
    uint8_t n = pc->tiers_count;
    if (n > RBPOWER_TARIFF_PROGRESSIVE_TIERS) n = RBPOWER_TARIFF_PROGRESSIVE_TIERS;
    int sel = 0;
    for (uint8_t t = 0; t < n; t++) {
        if ((double)pc->tiers[t].threshold_kwh <= cum + 1e-6) sel = (int)t;  // +EPS: see progressive_effective_rate
    }
    if (tier_index) *tier_index = sel;
    if (multiplier) *multiplier = (pc->tiers_count && pc->tiers[sel].use_multiplier)
                                   ? pc->tiers[sel].rate_multiplier : 1.0f;
}

static bool date_is_holiday(uint8_t month, uint8_t day) {
    for (uint8_t i = 0; i < g_holiday_count; i++)
        if (g_holidays[i].month == month && g_holidays[i].day == day) return true;
    return false;
}

// Tariff system state flags
static bool g_tariff_system_initialized = false;
static bool g_tariff_system_enabled = false;

/** @brief Per-unit RAM period accumulators (3SEC/15MIN/1HOUR), defined in the statistics module. */
extern rbpower_period_accumulator_t g_period_accumulators[RBPOWER_MAX_HOME_UNITS][RBPOWER_RAM_PERIOD_COUNT];
/** @brief The home electrical network (units, definitions, statistics), defined in rbhome. */
extern rbpower_electrical_network_t g_home_network;

static bool nm_key(uint8_t i, char key[12])
{
    if (i >= RBPOWER_MAX_HOME_UNITS || !g_home_network.units[i].has_valid_definition) return false;
    snprintf(key, 12, "n%04X", (unsigned)g_home_network.units[i].definition.unit_id);
    return true;
}
static void nm_persist(uint8_t i)
{
    char key[12]; nvs_handle_t h;
    if (!nm_key(i, key) || nvs_open("rbnm", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_blob(h, key, &g_net_metering[i], sizeof(g_net_metering[i]));
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) {
        ESP_LOGE("RBHOME_TARIFF", "net-metering unit[%u] NOT persisted (%s)", i, esp_err_to_name(e));
        rbpower_diag_printf("FAIL nm persist u%u: %s", i, esp_err_to_name(e));
    }
    nvs_close(h);
}

/* system.cost_day/cost_total/rev_day used to stay 0 for rbAmp-fed units: the lifetime accumulators got
 * ENERGY from the module counters but COST only from the 3-sec accumulation path. Book every 15-min
 * costing into the unit's lifetime (day/total) cost here. `idx` is the home-unit INDEX (the costing
 * functions are index-based). */
static void lifetime_cost_add(uint8_t idx, double cost, double revenue)
{
    if (idx >= RBPOWER_MAX_HOME_UNITS || !g_home_network.units[idx].has_valid_definition) return;
    (void)rbpower_lifetime_add_cost(g_home_network.units[idx].definition.unit_id, (float)cost, (float)revenue);
}
static void nm_restore(uint8_t i)
{
    if (i >= RBPOWER_MAX_HOME_UNITS || s_nm_restored[i]) return;
    s_nm_restored[i] = true;
    char key[12]; nvs_handle_t h;
    if (!nm_key(i, key) || nvs_open("rbnm", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(g_net_metering[i]);
    esp_err_t e = nvs_get_blob(h, key, &g_net_metering[i], &len);
    nvs_close(h);
    if (e == ESP_OK && len == sizeof(g_net_metering[i])) {
        ESP_LOGI("RBHOME_TARIFF", "net-metering unit[%u] restored: period %.4f/%.4f kWh, credit %.4f kWh",
                 i, g_net_metering[i].period_consumed_kwh, g_net_metering[i].period_supplied_kwh,
                 g_net_metering[i].credit_balance_kwh);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        memset(&g_net_metering[i], 0, sizeof(g_net_metering[i]));
    }
}
extern bool g_home_network_initialized; ///< Home network initialization flag (defined in rbhome).
//=============================================================================
// TARIFF SYSTEM INITIALIZATION
//=============================================================================

static void validate_tariff_plan(const rbpower_tariff_plan_t* plan); /* defined below */

/* The built-in "Test Day/Night" plan is a placeholder (a fresh hub must compute SOMETHING, not a silent flat
 * rate) — not the user's tariff. Recognised by its fixed names; the moment a real plan is saved it is gone.
 * Lets the UI avoid presenting placeholder costs, or a configured tariff, on a fresh hub. */
bool rbpower_tariff_plan_is_placeholder(const char* plan_name, const char* provider_name)
{
    return plan_name && provider_name && !strcmp(plan_name, "Test Day/Night") && !strcmp(provider_name, "Test Provider");
}

/** @brief Fill @p plan with the built-in two-zone "Test Day/Night" placeholder plan (EUR). */
static esp_err_t create_test_day_night_tariff_plan(rbpower_tariff_plan_t* plan)
{
    if (!plan) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD("RBHOME_TARIFF", "Creating test 'Day/Night' tariff plan");

    // Clear plan structure
    memset(plan, 0, sizeof(rbpower_tariff_plan_t));

    // === BASIC PLAN INFORMATION ===
    strncpy(plan->plan_name, "Test Day/Night", sizeof(plan->plan_name) - 1);
    strncpy(plan->provider_name, "Test Provider", sizeof(plan->provider_name) - 1);
    strncpy(plan->description, "Simple two-zone test plan", sizeof(plan->description) - 1);
    plan->plan_type = RBPOWER_TARIFF_PLAN_TWO_ZONE;
    
    // === NIGHT ZONE SETUP (23:00 - 07:00) ===
    rbpower_tariff_zone_t* night_zone = &plan->zones[0];
    night_zone->zone_id = RBPOWER_TARIFF_ZONE_NIGHT;
    strncpy(night_zone->zone_name, "Night", sizeof(night_zone->zone_name) - 1);
    strncpy(night_zone->description, "Night tariff (23:00-07:00)", sizeof(night_zone->description) - 1);

    // Night zone rates — realistic EU defaults (overridden by builder config in production).
    night_zone->rate_buying_per_kwh = 0.20f;               // 0.20 EUR/kWh
    night_zone->rate_selling_per_kwh = 0.12f;              // 0.12 EUR/kWh feed-in
    night_zone->net_metering_ratio = 0.6f;                 // 60% credit
    night_zone->base_multiplier = 1.0f;
    night_zone->seasonal_multiplier = 1.0f;
    night_zone->critical_peak_multiplier = 1.0f;
    
    // Night zone time conditions
    night_zone->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;
    night_zone->conditions[0].params.time_range.start_hour = 23;
    night_zone->conditions[0].params.time_range.start_minute = 0;
    night_zone->conditions[0].params.time_range.end_hour = 7;
    night_zone->conditions[0].params.time_range.end_minute = 0;
    night_zone->conditions[0].params.time_range.spans_midnight = true;
    night_zone->conditions[0].is_active = true;
    night_zone->conditions[0].priority = 10;
    night_zone->conditions_count = 1;
    night_zone->conditions_and_logic = true;

    // Night zone settings
    night_zone->min_duration_minutes = 5.0f;
    night_zone->unlimited_energy = true;
    night_zone->is_enabled = true;

    // === DAY ZONE SETUP (07:00 - 23:00) ===
    rbpower_tariff_zone_t* day_zone = &plan->zones[1];
    day_zone->zone_id = RBPOWER_TARIFF_ZONE_DAY;
    strncpy(day_zone->zone_name, "Day", sizeof(day_zone->zone_name) - 1);
    strncpy(day_zone->description, "Day tariff (07:00-23:00)", sizeof(day_zone->description) - 1);

    // Day zone rates — realistic EU defaults (overridden by builder config in production).
    day_zone->rate_buying_per_kwh = 0.30f;                  // 0.30 EUR/kWh
    day_zone->rate_selling_per_kwh = 0.12f;                 // 0.12 EUR/kWh feed-in
    day_zone->net_metering_ratio = 0.67f;                  // 67% credit
    day_zone->base_multiplier = 1.0f;
    day_zone->seasonal_multiplier = 1.0f;
    day_zone->critical_peak_multiplier = 1.0f;
    
    // Day zone time conditions
    day_zone->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;
    day_zone->conditions[0].params.time_range.start_hour = 7;
    day_zone->conditions[0].params.time_range.start_minute = 0;
    day_zone->conditions[0].params.time_range.end_hour = 23;
    day_zone->conditions[0].params.time_range.end_minute = 0;
    day_zone->conditions[0].params.time_range.spans_midnight = false;
    day_zone->conditions[0].is_active = true;
    day_zone->conditions[0].priority = 5;
    day_zone->conditions_count = 1;
    day_zone->conditions_and_logic = true;

    // Day zone settings
    day_zone->min_duration_minutes = 5.0f;
    day_zone->unlimited_energy = true;
    day_zone->is_enabled = true;

    // === COMMON PLAN SETTINGS ===
    plan->active_zones_count = 2;
    plan->default_zone_id = RBPOWER_TARIFF_ZONE_DAY;  // Day tariff by default

    // Currency and region
    strncpy(plan->currency_code, "EUR", sizeof(plan->currency_code) - 1);
    strncpy(plan->region_code, "EU", sizeof(plan->region_code) - 1);

    // Base taxes
    plan->vat_percent = 21.0f;                          // VAT 21%
    plan->energy_tax_per_kwh = 0.0025f;                 // Energy tax 0.0025 EUR/kWh
    plan->grid_usage_fee_per_kwh = 0.015f;              // Grid fee 0.015 EUR/kWh
    plan->renewable_levy_per_kwh = 0.0065f;             // Renewable levy 0.0065 EUR/kWh

    // Net metering settings
    plan->net_metering_enabled = true;
    plan->net_metering_default_ratio = 0.7f;            // 70% credit by default
    plan->net_metering_period = RBPOWER_STAT_PERIOD_MONTHLY;
    plan->carry_over_credits = true;
    plan->max_credit_carry_over_kwh = 1000.0f;          // Max 1000 kWh carry over

    // Supported features
    plan->supported_features = RBPOWER_TARIFF_FEATURES_STANDARD;
    plan->calculation_method = RBPOWER_TARIFF_CALC_TIME_OF_USE;
    
    // Temporal validity
    plan->valid_from_timestamp = rbtime_get_unix_timestamp();
    plan->valid_until_timestamp = plan->valid_from_timestamp + (365ULL * 24 * 3600); // 1 year
    plan->plan_version = 1;

    // Plan state
    plan->is_active = true;
    plan->is_default = true;
    plan->created_timestamp = rbtime_get_unix_timestamp();
    plan->last_modified_timestamp = plan->created_timestamp;

    // Optimization
    plan->optimization_priority = RBPOWER_TARIFF_PRIORITY_COST;
    plan->enable_load_shifting_recommendations = true;
    plan->load_shifting_threshold_percent = 15.0f;      // 15% savings for recommendations

    ESP_LOGD("RBHOME_TARIFF", "Test plan created: Night=%.3f EUR/kWh, Day=%.3f EUR/kWh",
             night_zone->rate_buying_per_kwh,
             day_zone->rate_buying_per_kwh);
    
    return ESP_OK;
}

#if RBGRID_STATS_TIME_SCALE > 1
/* COMPREHENSIVE TEST tariff plan (used only in the accelerated test build, RBGRID_STATS_TIME_SCALE>1)
 * to exercise the FULL calendar logic in minutes: TOU night/day, an EVENING-PEAK overlap,
 * a WEEKEND zone (Sat/Sun) and a HOLIDAY zone — each with distinct buy + feed-in rates. Combined with
 * the scale-aware tariff clock (virtual time advances ×scale), a test run can validate zone transitions,
 * weekend/holiday variation, and feed-in pricing without waiting real days. Production uses the simple
 * day/night default (this function is not selected when scale==1). Holidays are set separately. */
static esp_err_t create_comprehensive_test_tariff_plan(rbpower_tariff_plan_t* plan)
{
    if (!plan) return ESP_ERR_INVALID_ARG;
    memset(plan, 0, sizeof(rbpower_tariff_plan_t));

    strncpy(plan->plan_name, "TEST Comprehensive", sizeof(plan->plan_name) - 1);
    strncpy(plan->provider_name, "Bench", sizeof(plan->provider_name) - 1);
    plan->plan_type = RBPOWER_TARIFF_PLAN_MULTI_ZONE;

    // [0] NIGHT 23:00–07:00 — cheap
    rbpower_tariff_zone_t* z = &plan->zones[0];
    z->zone_id = RBPOWER_TARIFF_ZONE_NIGHT; strncpy(z->zone_name, "Night", sizeof(z->zone_name)-1);
    z->rate_buying_per_kwh = 0.20f; z->rate_selling_per_kwh = 0.12f; z->net_metering_ratio = 0.6f;
    z->base_multiplier = z->seasonal_multiplier = z->critical_peak_multiplier = 1.0f;
    z->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;
    z->conditions[0].params.time_range.start_hour = 23; z->conditions[0].params.time_range.end_hour = 7;
    z->conditions[0].params.time_range.spans_midnight = true;
    z->conditions[0].is_active = true; z->conditions[0].priority = 10;
    z->conditions_count = 1; z->conditions_and_logic = true; z->unlimited_energy = true; z->is_enabled = true;

    // [1] DAY 07:00–23:00 — standard
    z = &plan->zones[1];
    z->zone_id = RBPOWER_TARIFF_ZONE_DAY; strncpy(z->zone_name, "Day", sizeof(z->zone_name)-1);
    z->rate_buying_per_kwh = 0.30f; z->rate_selling_per_kwh = 0.12f; z->net_metering_ratio = 0.67f;
    z->base_multiplier = z->seasonal_multiplier = z->critical_peak_multiplier = 1.0f;
    z->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;
    z->conditions[0].params.time_range.start_hour = 7; z->conditions[0].params.time_range.end_hour = 23;
    z->conditions[0].is_active = true; z->conditions[0].priority = 5;
    z->conditions_count = 1; z->conditions_and_logic = true; z->unlimited_energy = true; z->is_enabled = true;

    // [2] EVENING PEAK 18:00–21:00 — expensive, overlaps DAY, higher priority to win the slot
    z = &plan->zones[2];
    z->zone_id = RBPOWER_TARIFF_ZONE_EVENING_PEAK; strncpy(z->zone_name, "Peak", sizeof(z->zone_name)-1);
    z->rate_buying_per_kwh = 0.45f; z->rate_selling_per_kwh = 0.12f; z->net_metering_ratio = 0.5f;
    z->base_multiplier = z->seasonal_multiplier = z->critical_peak_multiplier = 1.0f;
    z->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;
    z->conditions[0].params.time_range.start_hour = 18; z->conditions[0].params.time_range.end_hour = 21;
    z->conditions[0].is_active = true; z->conditions[0].priority = 50;
    z->conditions_count = 1; z->conditions_and_logic = true; z->unlimited_energy = true; z->is_enabled = true;

    // [3] WEEKEND (Sat/Sun) — flat cheap, overrides TOU on weekends
    z = &plan->zones[3];
    z->zone_id = RBPOWER_TARIFF_ZONE_WEEKEND; strncpy(z->zone_name, "Weekend", sizeof(z->zone_name)-1);
    z->rate_buying_per_kwh = 0.15f; z->rate_selling_per_kwh = 0.10f; z->net_metering_ratio = 0.6f;
    z->base_multiplier = z->seasonal_multiplier = z->critical_peak_multiplier = 1.0f;
    z->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_WEEKEND;
    z->conditions[0].is_active = true; z->conditions[0].priority = 80;
    z->conditions_count = 1; z->conditions_and_logic = true; z->unlimited_energy = true; z->is_enabled = true;

    // [4] HOLIDAY — cheapest, highest priority (overrides everything on a configured holiday)
    z = &plan->zones[4];
    z->zone_id = RBPOWER_TARIFF_ZONE_HOLIDAY; strncpy(z->zone_name, "Holiday", sizeof(z->zone_name)-1);
    z->rate_buying_per_kwh = 0.10f; z->rate_selling_per_kwh = 0.10f; z->net_metering_ratio = 0.6f;
    z->base_multiplier = z->seasonal_multiplier = z->critical_peak_multiplier = 1.0f;
    z->conditions[0].condition_type = RBPOWER_TARIFF_CONDITION_HOLIDAY;
    z->conditions[0].is_active = true; z->conditions[0].priority = 90;
    z->conditions_count = 1; z->conditions_and_logic = true; z->unlimited_energy = true; z->is_enabled = true;

    plan->active_zones_count = 5;
    plan->default_zone_id = RBPOWER_TARIFF_ZONE_DAY;
    strncpy(plan->currency_code, "EUR", sizeof(plan->currency_code)-1);
    strncpy(plan->region_code, "EU", sizeof(plan->region_code)-1);
    plan->vat_percent = 21.0f; plan->energy_tax_per_kwh = 0.0025f;
    plan->grid_usage_fee_per_kwh = 0.015f; plan->renewable_levy_per_kwh = 0.0065f;
    plan->net_metering_enabled = true; plan->net_metering_default_ratio = 0.7f;
    plan->net_metering_period = RBPOWER_STAT_PERIOD_MONTHLY;
    plan->carry_over_credits = true; plan->max_credit_carry_over_kwh = 1000.0f;  // bank earned credits across periods (else settle would zero credit_bal)
    // Progressive block tiers (opt-in; default OFF — enable with `progmode 1` to test in
    // isolation). Thresholds sized for injected test magnitudes (Wh-scale) so cumulative monthly
    // import crosses tiers within a short session: ×1.0 → ×1.2 → ×1.5 → ×2.0.
    plan->progressive_config.is_enabled = false;
    plan->progressive_config.tiers_count = 4;
    plan->progressive_config.calculation_period = RBPOWER_STAT_PERIOD_MONTHLY;
    plan->progressive_config.reset_monthly = true; plan->progressive_config.reset_day = 1;
    plan->progressive_config.tiers[0].threshold_kwh = 0.00f; plan->progressive_config.tiers[0].rate_multiplier = 1.0f; plan->progressive_config.tiers[0].use_multiplier = true;
    plan->progressive_config.tiers[1].threshold_kwh = 0.10f; plan->progressive_config.tiers[1].rate_multiplier = 1.2f; plan->progressive_config.tiers[1].use_multiplier = true;
    plan->progressive_config.tiers[2].threshold_kwh = 0.20f; plan->progressive_config.tiers[2].rate_multiplier = 1.5f; plan->progressive_config.tiers[2].use_multiplier = true;
    plan->progressive_config.tiers[3].threshold_kwh = 0.30f; plan->progressive_config.tiers[3].rate_multiplier = 2.0f; plan->progressive_config.tiers[3].use_multiplier = true;
    plan->supported_features = RBPOWER_TARIFF_FEATURES_STANDARD;
    plan->calculation_method = RBPOWER_TARIFF_CALC_TIME_OF_USE;
    plan->valid_from_timestamp = rbtime_get_unix_timestamp();
    plan->valid_until_timestamp = plan->valid_from_timestamp + (365ULL * 24 * 3600);
    plan->plan_version = 1; plan->is_active = true; plan->is_default = true;
    plan->created_timestamp = plan->last_modified_timestamp = rbtime_get_unix_timestamp();
    plan->optimization_priority = RBPOWER_TARIFF_PRIORITY_COST;

    ESP_LOGW("RBHOME_TARIFF", "⏩ TEST tariff plan: 5 zones (Night.20/Day.30/Peak.45/Weekend.15/Holiday.10 €/kWh) + feed-in");
    return ESP_OK;
}
#endif // RBGRID_STATS_TIME_SCALE > 1

/**
 * @brief Initialize the tariff configuration of one unit from its role and metric type.
 */
static esp_err_t init_unit_tariff_config(uint8_t unit_id, const rbpower_unit_definition_t* unit_def)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        ESP_LOGE("RBHOME_TARIFF", "init_unit_tariff_config: unit_id=%d >= MAX=%d",
                 unit_id, RBPOWER_MAX_HOME_UNITS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!unit_def) {
        ESP_LOGE("RBHOME_TARIFF", "init_unit_tariff_config: unit_def is NULL for unit_id=%d", unit_id);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD("RBHOME_TARIFF", "Init tariff config for Unit[%d] '%s'",
                 unit_id, unit_def->name);
    rbpower_tariff_unit_config_t* config = &g_unit_tariff_configs[unit_id];
    memset(config, 0, sizeof(rbpower_tariff_unit_config_t));

    // Base settings
    config->unit_id = unit_id;
    strncpy(config->device_name, unit_def->name, sizeof(config->device_name) - 1);
    config->rated_power_w = unit_def->rated_power_w;
    config->tariff_tracking_enabled = true;
    // === FIXED TARIFF DETERMINATION LOGIC ===
    // Voltage Bus is NOT tariffed individually
    if (unit_def->type == RBPOWER_METRIC_VOLTAGE_BUS) {
        //config->tariff_tracking_enabled = false;
        config->device_profile = RBPOWER_DEVICE_PROFILE_CUSTOM;
        config->billing_direction = RBPOWER_TARIFF_DIRECTION_CONSUMPTION;

        ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Voltage Bus - tariff DISABLED",
                 unit_id, unit_def->name);
        return ESP_OK;
    }

    // Device profile determination by role AND type
    switch (unit_def->role) {
        case RBPOWER_ROLE_MAIN_SUPPLY:
            if (unit_def->type == RBPOWER_METRIC_CURRENT_LOAD) {
                // Main system meter - MUST be tariffed
                //config->tariff_tracking_enabled = true;
                config->device_profile = RBPOWER_DEVICE_PROFILE_ALWAYS_ON;
                config->billing_direction = RBPOWER_TARIFF_DIRECTION_BIDIRECTIONAL;

                ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Main Supply - tariff ENABLED",
                         unit_id, unit_def->name);
            } else {
                // Main Supply but not Current Load - possibly Voltage Bus
                // config->tariff_tracking_enabled = false;
                config->device_profile = RBPOWER_DEVICE_PROFILE_CUSTOM;
                config->billing_direction = RBPOWER_TARIFF_DIRECTION_CONSUMPTION;
            }
            break;

        case RBPOWER_ROLE_ALTERNATIVE_SUPPLY:
            // Alternative power source (inverter, generator, second input)
            //config->tariff_tracking_enabled = true;
            config->device_profile = RBPOWER_DEVICE_PROFILE_ALWAYS_ON;
            config->billing_direction = RBPOWER_TARIFF_DIRECTION_BIDIRECTIONAL;

            ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Alternative Supply - tariff ENABLED",
                    unit_id, unit_def->name);
            break;

            case RBPOWER_ROLE_ZONE_LOAD:
            // Zone loads - tariff
            // config->tariff_tracking_enabled = true;
            config->device_profile = RBPOWER_DEVICE_PROFILE_ON_DEMAND;
            config->billing_direction = RBPOWER_TARIFF_DIRECTION_CONSUMPTION;

            ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Zone Load - tariff ENABLED",
                     unit_id, unit_def->name);
            break;

        case RBPOWER_ROLE_RENEWABLE_SOURCE:
            // Renewable sources - tariff
            // config->tariff_tracking_enabled = true;
            config->device_profile = RBPOWER_DEVICE_PROFILE_GENERATION;
            config->billing_direction = RBPOWER_TARIFF_DIRECTION_SUPPLY;

            ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Renewable Source - tariff ENABLED",
                     unit_id, unit_def->name);
            break;

        case RBPOWER_ROLE_STORAGE_SYSTEM:
            // Storage systems - tariff
            // config->tariff_tracking_enabled = true;
            config->device_profile = RBPOWER_DEVICE_PROFILE_STORAGE;
            config->billing_direction = RBPOWER_TARIFF_DIRECTION_BIDIRECTIONAL;

            ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Storage System - tariff ENABLED",
                     unit_id, unit_def->name);
            break;

        default:
            // Other roles - no tariff by default
            // config->tariff_tracking_enabled = false;
            config->device_profile = RBPOWER_DEVICE_PROFILE_CUSTOM;
            config->billing_direction = RBPOWER_TARIFF_DIRECTION_CONSUMPTION;

            ESP_LOGD("RBHOME_TARIFF", "Unit[%d] '%s': Default role - tariff DISABLED",
                     unit_id, unit_def->name);
            break;
    }

    // Other settings only for tariffed units
    if (config->tariff_tracking_enabled) {
        // Optimization settings
        config->enable_individual_optimization = true;
        config->enable_session_tracking = (unit_def->role != RBPOWER_ROLE_MAIN_SUPPLY);
        config->enable_cost_prediction = true;

        // Session thresholds
        config->thresholds.session_start_power_threshold_w = unit_def->rated_power_w * 0.05f;
        config->thresholds.session_end_power_threshold_w = unit_def->rated_power_w * 0.02f;
        config->thresholds.session_timeout_seconds = 300;

        // Optimization
        config->optimization.priority = RBPOWER_TARIFF_PRIORITY_COST;
        config->optimization.allow_load_shifting = (unit_def->role == RBPOWER_ROLE_ZONE_LOAD);
        config->optimization.max_shift_delay_minutes = 60;
        config->optimization.min_savings_threshold_percent = 10.0f;
        
        // Notifications
        config->notifications.enable_unit_alerts = true;
        config->notifications.cost_alert_threshold = 1.0f;
        config->notifications.notify_on_peak_usage = true;
    }
    
    return ESP_OK;
}

/**
 * @brief Initialize direct costs for unit
 */
static esp_err_t init_unit_direct_costs(uint8_t unit_id)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    rbpower_unit_direct_costs_t* costs = &g_unit_direct_costs[unit_id];
    memset(costs, 0, sizeof(rbpower_unit_direct_costs_t));
    
    // Basic settings
    costs->unit_id = unit_id;
    costs->device_profile = g_unit_tariff_configs[unit_id].device_profile;
    costs->cost_tracking_enabled = g_unit_tariff_configs[unit_id].tariff_tracking_enabled;
    costs->last_cost_calculation_timestamp = rbtime_get_unix_timestamp();

    // Initialize current session
    costs->current_session.session_start_timestamp = 0;
    costs->current_session.current_session_cost = 0.0;

    // Initialize analytics
    for (uint8_t hour = 0; hour < 24; hour++) {
        costs->device_analytics.usage_minutes_by_hour[hour] = 0;
    }
    for (uint8_t zone = 0; zone < RBPOWER_TARIFF_ZONE_COUNT; zone++) {
        costs->device_analytics.usage_count_by_zone[zone] = 0;
        costs->device_analytics.cost_distribution_by_zone[zone] = 0.0f;
        costs->device_analytics.revenue_distribution_by_zone[zone] = 0.0f;
    }
    
    ESP_LOGD("RBHOME_TARIFF", "Unit[%d] direct costs initialized", unit_id);
    
    return ESP_OK;
}

/**
 * @brief Helper function to get statistical period name for debugging
 */
static const char* rbpower_get_stat_period_name(rbpower_stat_period_type_t period)
{
    switch (period) {
        case RBPOWER_STAT_PERIOD_3SEC:    return "3sec";
        case RBPOWER_STAT_PERIOD_15MIN:   return "15min";
        case RBPOWER_STAT_PERIOD_1HOUR:   return "1hour";
        case RBPOWER_STAT_PERIOD_DAILY:   return "1day";
        case RBPOWER_STAT_PERIOD_WEEKLY:  return "1week";
        case RBPOWER_STAT_PERIOD_MONTHLY: return "1month";
        case RBPOWER_STAT_PERIOD_YEARLY:  return "1year";
        default:                          return "unknown";
    }
}

/**
 * @brief Initialize the RAM period accumulators of one unit based on its role.
 */
static esp_err_t init_unit_period_accumulators(uint8_t unit_id, const rbpower_unit_definition_t* unit_def)
{
    if (unit_id >= RBPOWER_MAX_HOME_UNITS || !unit_def) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD("RBHOME_TARIFF", "=== Initializing period accumulators Unit[%d] '%s' ===",
             unit_id, unit_def->name);

    // Define active periods mask by role and type
    uint32_t enabled_periods_mask = 0;

    if (unit_def->type == RBPOWER_METRIC_VOLTAGE_BUS) {
        // Voltage Bus does NOT need economics - only voltage reference
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC); // Disable all periods
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Voltage Bus - economics DISABLED (voltage reference)", unit_id);
    } else if (unit_def->role == RBPOWER_ROLE_MAIN_SUPPLY) {
        // Main Supply - critical unit, up to 1 hour in RAM
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC) |
                              (1 << RBPOWER_STAT_PERIOD_15MIN) |
                              (1 << RBPOWER_STAT_PERIOD_1HOUR);
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Main Supply - up to 1 hour in RAM (critical)", unit_id);
    } else if (unit_def->role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY) {
        // Alternative Supply - backup/alternative input, up to 1 hour in RAM
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC) |
                              (1 << RBPOWER_STAT_PERIOD_15MIN) |
                              (1 << RBPOWER_STAT_PERIOD_1HOUR);
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Alternative Supply - up to 1 hour in RAM (alternative input)", unit_id);
    } else if (unit_def->role == RBPOWER_ROLE_RENEWABLE_SOURCE) {
        // Renewable sources (solar panels) - up to 15 min in RAM
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC) |
                              (1 << RBPOWER_STAT_PERIOD_15MIN);
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Renewable Source - up to 15 min in RAM (solar panels)", unit_id);
    } else if (unit_def->role == RBPOWER_ROLE_STORAGE_SYSTEM) {
        // Storage systems (batteries) - up to 15 min in RAM
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC) |
                              (1 << RBPOWER_STAT_PERIOD_15MIN);
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Storage System - up to 15 min in RAM (batteries)", unit_id);
    } else if (unit_def->role == RBPOWER_ROLE_ZONE_LOAD) {
        // Zone loads - up to 15 min in RAM
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC) |
                              (1 << RBPOWER_STAT_PERIOD_15MIN);
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Zone Load - up to 15 min in RAM (zone load)", unit_id);
    } else {
        // Other roles - basic periods up to 15 min
        enabled_periods_mask = (1 << RBPOWER_STAT_PERIOD_3SEC) |
                              (1 << RBPOWER_STAT_PERIOD_15MIN);
        ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Role %d - up to 15 min in RAM (basic periods)",
                 unit_id, unit_def->role);
    }

    // DAY/WEEK/MONTH/YEAR are never force-enabled here: day+ periods are recomputed from flash
    // (no RAM accumulator), and g_period_accumulators only holds the 3 RAM periods, so only
    // 3SEC/15MIN/1HOUR are touched.

    // Activate accumulators for enabled periods (RAM-accumulated only: 3SEC/15MIN/1HOUR)
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_RAM_PERIOD_COUNT; period++) {
        rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period];

        if (enabled_periods_mask & (1 << period)) {
            // Enable period
            acc->is_enabled = true;
            acc->is_active = false; // Will be activated on period start

            // Initialize economics accumulators for this period
            memset(&acc->economics, 0, sizeof(acc->economics));

            // Initialize energy accumulators
            memset(&acc->energy, 0, sizeof(acc->energy));

            // Initialize metadata
            acc->metadata.samples_count = 0;
            acc->metadata.valid_samples_count = 0;
            acc->metadata.last_update_us = 0;
            acc->metadata.input_periods_expected = 0;    // Will be set during aggregation
            acc->metadata.input_periods_received = 0;
            acc->metadata.ready_for_finalization = false;

            /*
            // Initialize timing data
            acc->period_start_us = 0;
            acc->period_end_us = 0;
            acc->period_duration_us = 0;
            */

            ESP_LOGD("RBHOME_TARIFF", "  → Period %d (%s) ENABLED",
                     period, rbpower_get_stat_period_name(period));
        } else {
            // Disable period
            acc->is_enabled = false;
            acc->is_active = false;
            ESP_LOGD("RBHOME_TARIFF", "  → Period %d (%s) DISABLED",
                     period, rbpower_get_stat_period_name(period));
        }
    }

    // Count and output memory usage statistics
    uint32_t enabled_periods_count = 0;
    for (rbpower_stat_period_type_t period = 0; period < RBPOWER_STAT_PERIOD_COUNT; period++) {
        if (enabled_periods_mask & (1 << period)) {
            enabled_periods_count++;
        }
    }

    uint32_t memory_usage_bytes = enabled_periods_count * sizeof(rbpower_period_accumulator_t);
    ESP_LOGD("RBHOME_TARIFF", "Unit[%d]: Enabled %lu periods, RAM usage: %lu bytes",
             unit_id, enabled_periods_count, memory_usage_bytes);

    return ESP_OK;
}

/* Check if aggregation to the specified period is allowed (disabled, kept for reference). */
// static bool rbpower_tariff_should_aggregate_to_period(uint8_t unit_id, rbpower_stat_period_type_t target_period)
// {
//     ESP_LOGI("RBHOME_TARIFF", "rbpower_tariff_should_aggregate_to_period - start Unit[%d]: Period %d", unit_id, target_period);
//     if (unit_id >= RBPOWER_MAX_HOME_UNITS || target_period >= RBPOWER_STAT_PERIOD_COUNT) {
//         return false;
//     }
//     
//     // Check that the target period is enabled for this unit
//     rbpower_period_accumulator_t* target_acc = &g_period_accumulators[unit_id][target_period];
//     if (!target_acc->is_enabled) {
//         ESP_LOGI("RBHOME_TARIFF", "Unit[%d]: Period %d not enabled for aggregation", unit_id, target_period);
//         return false;
//     }
//     
//     // Check that tariffing is enabled for this unit
//     if (!g_unit_tariff_configs[unit_id].tariff_tracking_enabled) {
//         ESP_LOGI("RBHOME_TARIFF", "Unit[%d]: Tariffing disabled", unit_id);
//         return false;
//     }
//     
//     ESP_LOGI("RBHOME_TARIFF", "Unit[%d]: Aggregation to period %d allowed", unit_id, target_period);
//     return true;
// }

/* Configure the yearly-recurring holiday list (month/day pairs); invalid entries are skipped. */
esp_err_t rbpower_tariff_set_holidays(const uint8_t* months, const uint8_t* days, uint8_t count)
{
    if (count > RBPOWER_HOLIDAY_MAX) count = RBPOWER_HOLIDAY_MAX;
    if (count > 0 && (!months || !days)) return ESP_ERR_INVALID_ARG;
    g_holiday_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (months[i] < 1 || months[i] > 12 || days[i] < 1 || days[i] > 31) continue;
        g_holidays[g_holiday_count].month = months[i];
        g_holidays[g_holiday_count].day   = days[i];
        g_holiday_count++;
    }
    ESP_LOGI("RBHOME_TARIFF", "Holidays configured: %u entries", g_holiday_count);
    return ESP_OK;
}

/* Main tariff system initialization: global config, default plan, zone cache, per-unit tables. */
esp_err_t rbpower_tariff_system_init(void)
{
    if (g_tariff_system_initialized) {
        ESP_LOGD("RBHOME_TARIFF", "Tariff system already initialized");
        return ESP_OK;
    }

    ESP_LOGI("RBHOME_TARIFF", "=== Initializing rbpower tariff system ===");

    // TZ is owned by rbtime, NOT here: the stored time-config zone is validated and applied
    // in rbtime_init (invalid → UTC0 + warning), runtime changes go through rbtime_set_timezone.
    // A hard-coded CET here used to override the config for every localtime_r user (tariff
    // zones AND calendar stats buckets) — removed.

    // === INITIALIZE GLOBAL CONFIGURATION ===
    memset(&g_tariff_global_config, 0, sizeof(rbpower_tariff_global_config_t));

    // Basic settings
    g_tariff_global_config.tariff_system_enabled = true;
    g_tariff_global_config.real_time_calculation = true;
    g_tariff_global_config.store_detailed_history = true;

    // Performance
    g_tariff_global_config.calculation_priority = 5;
    g_tariff_global_config.max_memory_usage_kb = 64;    // 64 KB for tariffing
    g_tariff_global_config.calculation_timeout_ms = 10; // 10 ms timeout
    g_tariff_global_config.enable_background_optimization = false; // Disabled for now

    // Time synchronization
    g_tariff_global_config.use_system_time = true;
    g_tariff_global_config.auto_timezone_detection = false;
    g_tariff_global_config.manual_timezone_offset_hours = 0; // unused: rbtime owns the time zone
    g_tariff_global_config.dst_auto_adjustment = true;
    g_tariff_global_config.time_sync_check_interval_minutes = 60;

    // Calculation periods
    g_tariff_global_config.enabled_periods_mask = RBPOWER_PERIODS_EXTENDED; // 3sec + 15min + 1hour + day
    g_tariff_global_config.primary_billing_period = RBPOWER_STAT_PERIOD_MONTHLY;
    g_tariff_global_config.optimization_period = RBPOWER_STAT_PERIOD_DAILY;
    g_tariff_global_config.enable_real_time_alerts = true;

    // Supported features
    g_tariff_global_config.supported_features = RBPOWER_TARIFF_FEATURES_STANDARD;
    g_tariff_global_config.enable_progressive_tariffs = false;  // Disabled for now
    g_tariff_global_config.enable_net_metering = true;
    g_tariff_global_config.enable_critical_peak_pricing = false; // Disabled for now
    g_tariff_global_config.enable_device_level_tracking = true;
    g_tariff_global_config.enable_load_optimization = true;

    // Precision and thresholds
    g_tariff_global_config.min_power_threshold_w = 1.0f;   // 1 W minimum
    g_tariff_global_config.min_cost_threshold = 0.00001f;     // 0.00001 EUR minimum
    g_tariff_global_config.calculation_precision_digits = 6; // 6 decimal places
    g_tariff_global_config.rounding_threshold = 0.0001f;

    // Notifications
    g_tariff_global_config.alerts.enable_cost_alerts = true;
    g_tariff_global_config.alerts.daily_cost_alert_threshold = 10.0f;   // 10 EUR/day
    g_tariff_global_config.alerts.monthly_cost_alert_threshold = 200.0f; // 200 EUR/month
    g_tariff_global_config.alerts.enable_peak_alerts = true;
    g_tariff_global_config.alerts.enable_optimization_suggestions = true;
    g_tariff_global_config.alerts.alert_cooldown_minutes = 30;

    ESP_LOGD("RBHOME_TARIFF", "Global configuration initialized");
    
    // === DEFAULT DAY/NIGHT TARIFF PLAN ===
    // A valid plan must exist so the cost path uses real TOU rates instead of silently
    // falling back to a flat 0.15 EUR/kWh. The builder (rbpower_tariff_apply_from_builder_safe)
    // overrides this with the user's configured plan when one is pushed.
#if RBGRID_STATS_TIME_SCALE > 1
    /* Accelerated TEST build: load the comprehensive 5-zone plan (TOU + peak + weekend + holiday) so
     * a test run can validate the full calendar logic fast, and arm test holidays. Jan 1 = the virtual
     * epoch start (1970-01-01) → the HOLIDAY zone is active right at boot under the virtual clock. */
    esp_err_t plan_err = create_comprehensive_test_tariff_plan(&g_current_tariff_plan);
    { const uint8_t hol_m[] = {1, 12}; const uint8_t hol_d[] = {1, 25};   // Jan 1, Dec 25
      rbpower_tariff_set_holidays(hol_m, hol_d, 2); }
#else
    esp_err_t plan_err = create_test_day_night_tariff_plan(&g_current_tariff_plan);
#endif
    if (plan_err != ESP_OK) {
        ESP_LOGE("RBHOME_TARIFF", "Failed to create default tariff plan: %d", plan_err);
        return plan_err;
    }
    validate_tariff_plan(&g_current_tariff_plan);   // surface overlap/gap/duplicate/rate conflicts
    // === INITIALIZE ACTIVE ZONE CACHE ===
    memset(&g_active_zone_cache, 0, sizeof(g_active_zone_cache));
    g_active_zone_cache.current_zone_id = g_current_tariff_plan.default_zone_id;
    strncpy(g_active_zone_cache.current_zone_name, "Unknown", sizeof(g_active_zone_cache.current_zone_name) - 1);
    g_active_zone_cache.zone_cache_valid = false;

    // Extended tariff calendar (seasonal week templates, hourly forecast, optimal periods,
    // text recommendations) was a 22 KB write-only global — initialized here but never read
    // (cost uses g_current_tariff_plan zones directly). Removed to save DRAM.
    // When those features are actually implemented, re-introduce the calendar
    // with only the fields used, ideally flash-backed rather than fully resident.

    ESP_LOGD("RBHOME_TARIFF", "Zone cache initialized");
    
    // === INITIALIZE UNIT CONFIGURATIONS ===
    // Initialize for all existing units
    //if (rbpower_home_is_initialized()) {
        //rbpower_electrical_network_t* network;
        //if (rbpower_get_network(&network) == RBPOWER_OK) {
        for (uint8_t i = 0; i < g_home_network.units_count; i++) {
            ESP_LOGD("RBHOME_TARIFF", "Processing Unit[%d] of %d", i, g_home_network.units_count);
            if (g_home_network.units[i].has_valid_definition) {
                ESP_LOGD("RBHOME_TARIFF", "Unit[%d] has valid definition", i);

                // Initialize tariff configuration
                esp_err_t config_err = init_unit_tariff_config(i, &g_home_network.units[i].definition);
                if (config_err == ESP_OK) {
                    // Initialize direct costs
                    init_unit_direct_costs(i);

                    // Initialize period accumulators
                    esp_err_t periods_err = init_unit_period_accumulators(i, &g_home_network.units[i].definition);
                    if (periods_err == ESP_OK) {
                        ESP_LOGI("RBHOME_TARIFF", "Unit[%d] '%s' fully ready for tariffing",
                                i, g_home_network.units[i].definition.name);
                    } else {
                        ESP_LOGE("RBHOME_TARIFF", "Unit[%d]: Failed to init period accumulators: %d",
                                i, periods_err);
                    }
                } else {
                    ESP_LOGE("RBHOME_TARIFF", "Unit[%d]: Failed to init tariff config: %d",
                            i, config_err);
                }
            }
        }
        //}
    //}
    
    // === SET READY FLAGS ===
    g_tariff_system_initialized = true;
    g_tariff_system_enabled = g_tariff_global_config.tariff_system_enabled;

    ESP_LOGI("RBHOME_TARIFF", "=== Tariff system initialized successfully ===");
    ESP_LOGI("RBHOME_TARIFF", "- Tariff plan: '%s' (%d zones)",
             g_current_tariff_plan.plan_name, g_current_tariff_plan.active_zones_count);
    ESP_LOGI("RBHOME_TARIFF", "- Currency: %s, Region: %s",
             g_current_tariff_plan.currency_code, g_current_tariff_plan.region_code);
    ESP_LOGI("RBHOME_TARIFF", "- Features: 0x%08X", (char)g_current_tariff_plan.supported_features);
    
    return ESP_OK;
}

//=============================================================================
// BUILDER STRUCTURES (copy of the rbgrid_config_api.c layout, used for mapping)
// Defined here so the forward declarations below can see them
//=============================================================================

/**
 * @brief Tariff zone as delivered by the tariff builder (must match the rbgrid_config_api.c layout).
 */
typedef struct {
    char zone_name[32];                     ///< Zone name (also used to infer the zone type)
    float rate_per_kwh;                     ///< Buying rate (currency/kWh)
    char time_range[16];                    ///< Time window "HH:MM-HH:MM", e.g. "23:00-07:00"
    uint8_t weekday_mask;                   ///< Weekday bit mask (bit0 = Mon … bit6 = Sun)
    uint8_t start_month;                    ///< Seasonal zones: first month (1-12)
    uint8_t end_month;                      ///< Seasonal zones: last month (1-12)
    rbpower_tariff_zone_type_t zone_type;   ///< Explicit zone type (0 = infer from name/time)
    bool is_time_based;                     ///< Zone has a time-of-day window
    bool is_weekday_based;                  ///< Zone has a weekday condition
    bool is_seasonal;                       ///< Zone has a month-range condition
    float selling_rate_per_kwh;             ///< Selling/feed-in rate (currency/kWh; 0 = not set)
} rbgrid_tariff_builder_zone_t;

/**
 * @brief Holiday as delivered by the tariff builder.
 */
typedef struct {
    uint16_t day_of_year;                   ///< Day of year (1-366)
    char name[32];                          ///< Holiday name
    float rate_per_kwh;                     ///< Rate applied on this day (currency/kWh)
} rbgrid_tariff_builder_holiday_t;

// Forward declarations of the mapping functions
static esp_err_t convert_builder_zone_to_tariff_zone(
    const rbgrid_tariff_builder_zone_t* builder_zone,
    rbpower_tariff_zone_t* tariff_zone,
    uint8_t zone_index);

/**
 * @brief Replace the active tariff plan with one built from tariff-builder data.
 *
 * Converts each builder zone (name/time/weekday/season) into a tariff zone, copies up to 8 fixed
 * fees, configures net metering and selling rates, then validates the plan (warnings only).
 * Not declared in a public header; the config API declares it locally.
 *
 * @param[in] currency                     Currency code (display label, e.g. "EUR").
 * @param[in] region                       Region code (e.g. "EU").
 * @param[in] zones_count                  Number of entries in @p zones_data (must be > 0;
 *                                         clipped to RBPOWER_TARIFF_ZONE_COUNT).
 * @param[in] zones_data                   Array of rbgrid_tariff_builder_zone_t.
 * @param[in] holidays_count               Number of builder holidays (logged only, not applied here).
 * @param[in] holidays_data                Array of rbgrid_tariff_builder_holiday_t (unused here).
 * @param[in] fixed_fees_count             Number of fixed fees in @p fixed_fees_data (max 8 used).
 * @param[in] fixed_fees_data              Array of builder fee records (name, amount in currency, type); may be NULL.
 * @param[in] net_metering_enabled         Enable net metering for the plan.
 * @param[in] default_selling_rate_per_kwh Fixed selling rate for all zones (currency/kWh); <= 0 means
 *                                         selling rate = buying rate × @p net_ratio.
 * @param[in] net_ratio                    Net-metering credit ratio (0..1).
 * @retval ESP_OK              Plan applied.
 * @retval ESP_ERR_INVALID_ARG @p zones_count is 0.
 * @note Writes the global plan without a lock; call from the config/processing task.
 */
esp_err_t rbpower_tariff_apply_from_builder_safe(
    const char* currency, const char* region,
    uint8_t zones_count, const void* zones_data,
    uint8_t holidays_count, const void* holidays_data,
    uint8_t fixed_fees_count, const void* fixed_fees_data,
    bool net_metering_enabled, float default_selling_rate_per_kwh, float net_ratio)
{
    ESP_LOGI("RBHOME_TARIFF", "=== Safe tariff application ===");
    ESP_LOGI("RBHOME_TARIFF", "Currency: %s, Region: %s", currency, region);
    ESP_LOGI("RBHOME_TARIFF", "Zones: %d, Holidays: %d, Fixed fees: %d",
             zones_count, holidays_count, fixed_fees_count);
    ESP_LOGI("RBHOME_TARIFF", "Net metering: %s, rate: %.3f, ratio: %.2f",
             net_metering_enabled ? "enabled" : "disabled",
             default_selling_rate_per_kwh, net_ratio);

    if (zones_count == 0) {
        ESP_LOGE("RBHOME_TARIFF", "ERROR: No zones provided! Cannot create tariff plan without zones.");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear the current plan
    memset(&g_current_tariff_plan, 0, sizeof(rbpower_tariff_plan_t));
    
    // === BASIC PLAN INFORMATION ===
    snprintf(g_current_tariff_plan.plan_name, sizeof(g_current_tariff_plan.plan_name), 
             "Builder Plan %s", region);
    strncpy(g_current_tariff_plan.provider_name, "Custom Provider", 
            sizeof(g_current_tariff_plan.provider_name) - 1);
    snprintf(g_current_tariff_plan.description, sizeof(g_current_tariff_plan.description),
             "Generated from tariff builder for region %s", region);
    strncpy(g_current_tariff_plan.currency_code, currency, 
            sizeof(g_current_tariff_plan.currency_code) - 1);
    strncpy(g_current_tariff_plan.region_code, region, 
            sizeof(g_current_tariff_plan.region_code) - 1);
    g_current_tariff_plan.plan_type = RBPOWER_TARIFF_PLAN_MULTI_ZONE;
    
    // === ZONE CONVERSION VIA MAPPING ===
    g_current_tariff_plan.active_zones_count = zones_count > RBPOWER_TARIFF_ZONE_COUNT ? 
                                               RBPOWER_TARIFF_ZONE_COUNT : zones_count;
    
    // Convert builder zones into tariff-system zones
    const rbgrid_tariff_builder_zone_t* builder_zones = (const rbgrid_tariff_builder_zone_t*)zones_data;
    
    for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count; i++) {
        esp_err_t convert_result = convert_builder_zone_to_tariff_zone(
            &builder_zones[i],
            &g_current_tariff_plan.zones[i],
            i);
        
        if (convert_result != ESP_OK) {
            ESP_LOGW("RBHOME_TARIFF", "Failed to convert zone[%d], using defaults", i);
            // Fall back to basic values
            if (i == 0) {
                g_current_tariff_plan.zones[i].zone_id = RBPOWER_TARIFF_ZONE_NIGHT;
                strncpy(g_current_tariff_plan.zones[i].zone_name, "Night", 
                       sizeof(g_current_tariff_plan.zones[i].zone_name) - 1);
                g_current_tariff_plan.zones[i].rate_buying_per_kwh = 0.08f;
            } else {
                g_current_tariff_plan.zones[i].zone_id = RBPOWER_TARIFF_ZONE_DAY;
                strncpy(g_current_tariff_plan.zones[i].zone_name, "Day", 
                       sizeof(g_current_tariff_plan.zones[i].zone_name) - 1);
                g_current_tariff_plan.zones[i].rate_buying_per_kwh = 0.15f;
            }
            g_current_tariff_plan.zones[i].rate_selling_per_kwh = default_selling_rate_per_kwh;
            g_current_tariff_plan.zones[i].is_enabled = true;
        }
        
        ESP_LOGI("RBHOME_TARIFF", "Zone[%d]: %s, %.3f EUR/kWh (type=%d)", 
                i, g_current_tariff_plan.zones[i].zone_name, 
                g_current_tariff_plan.zones[i].rate_buying_per_kwh,
                g_current_tariff_plan.zones[i].zone_id);
    }
    
    // === DEFAULT ZONE ===
    // Use the first zone, or DAY if no zones are defined
    if (g_current_tariff_plan.active_zones_count > 0) {
        g_current_tariff_plan.default_zone_id = g_current_tariff_plan.zones[0].zone_id;
    } else {
        g_current_tariff_plan.default_zone_id = RBPOWER_TARIFF_ZONE_DAY;
    }

    // === FIXED PAYMENTS ===
    g_current_tariff_plan.fixed_payments_count = 0;

    if (fixed_fees_count > 0 && fixed_fees_data != NULL) {
        // Builder fee record layout (must match the builder) for the mapping
        typedef struct {
            char fee_name[32];
            float amount;
            rbpower_tariff_fixed_type_t fee_type;
        } rbgrid_tariff_builder_fee_t;

        const rbgrid_tariff_builder_fee_t* builder_fees = (const rbgrid_tariff_builder_fee_t*)fixed_fees_data;

        // Copy fixed payments (at most 8)
        uint8_t max_fees = fixed_fees_count > 8 ? 8 : fixed_fees_count;

        for (uint8_t i = 0; i < max_fees; i++) {
            rbpower_tariff_fixed_payment_t* payment = &g_current_tariff_plan.fixed_payments[i];
            const rbgrid_tariff_builder_fee_t* builder_fee = &builder_fees[i];

            // Basic fields
            payment->payment_type = builder_fee->fee_type;
            strncpy(payment->payment_name, builder_fee->fee_name, RBPOWER_TARIFF_NAME_LENGTH - 1);
            payment->payment_name[RBPOWER_TARIFF_NAME_LENGTH - 1] = '\0';

            // Amount (currency)
            payment->amount = builder_fee->amount;

            // Currency
            strncpy(payment->currency, currency, RBPOWER_TARIFF_CURRENCY_LENGTH - 1);
            payment->currency[RBPOWER_TARIFF_CURRENCY_LENGTH - 1] = '\0';

            // Fill the calculation union according to the fee type
            switch (builder_fee->fee_type) {
                case RBPOWER_TARIFF_FIXED_MONTHLY:
                    payment->calculation.time_based.monthly_amount = builder_fee->amount;
                    payment->billing_period = RBPOWER_STAT_PERIOD_MONTHLY;
                    payment->billing_day = 1;  // Default: 1st day of the month
                    break;

                case RBPOWER_TARIFF_FIXED_DAILY:
                    payment->calculation.time_based.daily_amount = builder_fee->amount;
                    payment->billing_period = RBPOWER_STAT_PERIOD_DAILY;
                    payment->billing_day = 0;
                    break;

                case RBPOWER_TARIFF_FIXED_POWER_BASED:
                    payment->calculation.power_based.per_kw_installed = builder_fee->amount;
                    payment->calculation.power_based.minimum_charge = 0.0f;  // Default
                    payment->billing_period = RBPOWER_STAT_PERIOD_MONTHLY;
                    payment->billing_day = 1;
                    break;

                case RBPOWER_TARIFF_FIXED_GRID_USAGE:
                case RBPOWER_TARIFF_FIXED_RENEWABLE_LEVY:
                case RBPOWER_TARIFF_FIXED_TAX:
                case RBPOWER_TARIFF_FIXED_CUSTOM:
                    // These types are billed as a monthly amount
                    payment->calculation.time_based.monthly_amount = builder_fee->amount;
                    payment->billing_period = RBPOWER_STAT_PERIOD_MONTHLY;
                    payment->billing_day = 1;
                    break;

                default:
                    ESP_LOGW("RBHOME_TARIFF", "Unknown fixed fee type %d", builder_fee->fee_type);
                    continue;
            }

            // Enable the payment
            payment->is_enabled = true;

            g_current_tariff_plan.fixed_payments_count++;

            ESP_LOGI("RBHOME_TARIFF", "Added fixed fee[%d]: %s = %.2f %s (%s)",
                     i, payment->payment_name, payment->amount, currency,
                     payment->payment_type == RBPOWER_TARIFF_FIXED_MONTHLY ? "monthly" :
                     payment->payment_type == RBPOWER_TARIFF_FIXED_DAILY ? "daily" :
                     payment->payment_type == RBPOWER_TARIFF_FIXED_POWER_BASED ? "per kW" :
                     payment->payment_type == RBPOWER_TARIFF_FIXED_GRID_USAGE ? "grid usage" :
                     payment->payment_type == RBPOWER_TARIFF_FIXED_RENEWABLE_LEVY ? "renewable levy" :
                     payment->payment_type == RBPOWER_TARIFF_FIXED_TAX ? "tax" : "custom");
        }

        ESP_LOGI("RBHOME_TARIFF", "Total fixed fees added: %d", g_current_tariff_plan.fixed_payments_count);
    }

    // === PLAN SETTINGS ===
    g_current_tariff_plan.is_active = true;
    g_current_tariff_plan.calculation_method = RBPOWER_TARIFF_CALC_TIME_OF_USE;
    g_current_tariff_plan.supported_features = RBPOWER_TARIFF_FEATURES_STANDARD;

    // === NET METERING ===
    g_current_tariff_plan.net_metering_enabled = net_metering_enabled;
    if (net_metering_enabled) {
        g_current_tariff_plan.supported_features |= RBPOWER_TARIFF_FEATURE_NET_METERING;
        g_current_tariff_plan.net_metering_default_ratio = net_ratio;
        g_current_tariff_plan.net_metering_period = RBPOWER_STAT_PERIOD_MONTHLY;
        g_current_tariff_plan.carry_over_credits = true;  // Allow credit carry-over by default
        g_current_tariff_plan.max_credit_carry_over_kwh = 1000.0f;  // At most 1000 kWh carried over

        // Set the selling rate for all zones
        for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count; i++) {
            // Set the net-metering ratio
            g_current_tariff_plan.zones[i].net_metering_ratio = net_ratio;

            // Two ways to set the selling rate:
            // Option 1: a fixed rate for all zones (if default_selling_rate_per_kwh > 0)
            // Option 2: a fraction of the buying rate (if default_selling_rate_per_kwh <= 0)

            if (default_selling_rate_per_kwh > 0) {
                // Fixed selling rate for all zones
                g_current_tariff_plan.zones[i].rate_selling_per_kwh = default_selling_rate_per_kwh;
                ESP_LOGD("RBHOME_TARIFF", "Zone[%d]: buying=%.3f, selling=%.3f (fixed rate)",
                         i,
                         g_current_tariff_plan.zones[i].rate_buying_per_kwh,
                         g_current_tariff_plan.zones[i].rate_selling_per_kwh);
            } else {
                // Selling rate = buying rate × ratio
                g_current_tariff_plan.zones[i].rate_selling_per_kwh =
                    g_current_tariff_plan.zones[i].rate_buying_per_kwh * net_ratio;
                ESP_LOGD("RBHOME_TARIFF", "Zone[%d]: buying=%.3f, selling=%.3f (ratio=%.2f)",
                         i,
                         g_current_tariff_plan.zones[i].rate_buying_per_kwh,
                         g_current_tariff_plan.zones[i].rate_selling_per_kwh,
                         net_ratio);
            }
        }

        ESP_LOGI("RBHOME_TARIFF", "Net metering enabled: %.3f EUR/kWh, ratio %.1f",
                 default_selling_rate_per_kwh, net_ratio);
    } else {
        g_current_tariff_plan.net_metering_default_ratio = 0.0f;
        g_current_tariff_plan.carry_over_credits = false;
        g_current_tariff_plan.max_credit_carry_over_kwh = 0.0f;
    }
    
    // === SYSTEM ACTIVATION ===
    //g_tariff_system_initialized = true;
    g_tariff_system_enabled = true;

    validate_tariff_plan(&g_current_tariff_plan);   // surface overlap/gap/duplicate/rate conflicts

    ESP_LOGI("RBHOME_TARIFF", "✅ Tariff plan applied successfully with REAL MAPPING!");
    ESP_LOGI("RBHOME_TARIFF", "Plan: %s, Currency: %s, Region: %s", 
             g_current_tariff_plan.plan_name,
             g_current_tariff_plan.currency_code,
             g_current_tariff_plan.region_code);
    ESP_LOGI("RBHOME_TARIFF", "Active zones: %d, Default zone ID: %d", 
             g_current_tariff_plan.active_zones_count,
             g_current_tariff_plan.default_zone_id);
    
    // Print a zone summary
    for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count && i < 3; i++) {
        ESP_LOGI("RBHOME_TARIFF", "  Zone[%d]: %s - %.3f/%.3f EUR/kWh (buy/sell), conditions: %d", 
                 i, 
                 g_current_tariff_plan.zones[i].zone_name,
                 g_current_tariff_plan.zones[i].rate_buying_per_kwh,
                 g_current_tariff_plan.zones[i].rate_selling_per_kwh,
                 g_current_tariff_plan.zones[i].conditions_count);
    }
    
    return ESP_OK;
}

//=============================================================================
// MAPPING FUNCTIONS BETWEEN THE BUILDER AND THE TARIFF SYSTEM
//=============================================================================

/** @brief Case-insensitive substring search (ASCII); returns the match or NULL. */
static char* strcasestr_local(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char*)haystack;
    
    for (const char* p = haystack; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return (char*)p;
        }
    }
    return NULL;
}

/**
 * @brief Parse a "HH:MM-HH:MM" time window into a TIME_RANGE tariff condition.
 * @param[in]  time_range Time window string (e.g. "23:00-07:00").
 * @param[out] condition  Condition to fill.
 * @retval ESP_OK              Parsed.
 * @retval ESP_ERR_INVALID_ARG NULL argument, bad format or out-of-range values.
 */
static esp_err_t parse_time_range_to_condition(const char* time_range, 
                                              rbpower_tariff_condition_t* condition)
{
    if (!time_range || !condition) {
        ESP_LOGE("TARIFF_MAPPER", "Invalid arguments for time range parsing");
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t start_hour, start_min, end_hour, end_min;
    
    // Parse the "HH:MM-HH:MM" format
    if (sscanf(time_range, "%hhu:%hhu-%hhu:%hhu", 
               &start_hour, &start_min, &end_hour, &end_min) != 4) {
        ESP_LOGW("TARIFF_MAPPER", "Invalid time range format: %s", time_range);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate the values
    if (start_hour > 23 || end_hour > 23 || start_min > 59 || end_min > 59) {
        ESP_LOGW("TARIFF_MAPPER", "Invalid time values in range: %s", time_range);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Fill the condition structure
    condition->condition_type = RBPOWER_TARIFF_CONDITION_TIME_RANGE;
    condition->params.time_range.start_hour = start_hour;
    condition->params.time_range.start_minute = start_min;
    condition->params.time_range.end_hour = end_hour;
    condition->params.time_range.end_minute = end_min;
    
    // Detect a window that wraps midnight
    condition->params.time_range.spans_midnight = 
        (start_hour > end_hour) || 
        (start_hour == end_hour && start_min > end_min);
    
    condition->is_active = true;
    condition->priority = 10; // Medium priority for time conditions
    
    ESP_LOGD("TARIFF_MAPPER", "Parsed time range %02d:%02d-%02d:%02d, spans_midnight=%d",
             start_hour, start_min, end_hour, end_min, 
             condition->params.time_range.spans_midnight);
    
    return ESP_OK;
}

/**
 * @brief Infer the tariff zone type from a builder zone's name and parameters.
 * @param[in] builder_zone Builder zone.
 * @return Zone type (RBPOWER_TARIFF_ZONE_DAY when nothing matches or on NULL).
 */
static rbpower_tariff_zone_type_t determine_zone_type(
    const rbgrid_tariff_builder_zone_t* builder_zone)
{
    if (!builder_zone) {
        return RBPOWER_TARIFF_ZONE_DAY;  // DAY is the default
    }
    
    // Match the zone name (case-insensitive; also accepts Russian "night"/"day" names)
    if (strcasestr_local(builder_zone->zone_name, "night") || 
        strcasestr_local(builder_zone->zone_name, "ночь")) {
        return RBPOWER_TARIFF_ZONE_NIGHT;
    }
    if (strcasestr_local(builder_zone->zone_name, "day") || 
        strcasestr_local(builder_zone->zone_name, "день")) {
        return RBPOWER_TARIFF_ZONE_DAY;
    }
    if (strcasestr_local(builder_zone->zone_name, "morning")) {
        return RBPOWER_TARIFF_ZONE_MORNING_PEAK;
    }
    if (strcasestr_local(builder_zone->zone_name, "evening")) {
        return RBPOWER_TARIFF_ZONE_EVENING_PEAK;
    }
    if (strcasestr_local(builder_zone->zone_name, "peak")) {
        // Plain "peak": decide morning/evening by the start time
        if (builder_zone->is_time_based && strlen(builder_zone->time_range) > 0) {
            uint8_t start_hour;
            if (sscanf(builder_zone->time_range, "%hhu:", &start_hour) == 1) {
                if (start_hour >= 6 && start_hour < 12) {
                    return RBPOWER_TARIFF_ZONE_MORNING_PEAK;
                } else if (start_hour >= 17 && start_hour < 22) {
                    return RBPOWER_TARIFF_ZONE_EVENING_PEAK;
                }
            }
        }
        return RBPOWER_TARIFF_ZONE_EVENING_PEAK;  // Default: evening peak
    }
    if (strcasestr_local(builder_zone->zone_name, "late")) {
        return RBPOWER_TARIFF_ZONE_LATE_EVENING;
    }
    if (strcasestr_local(builder_zone->zone_name, "weekend")) {
        return RBPOWER_TARIFF_ZONE_WEEKEND;
    }
    if (strcasestr_local(builder_zone->zone_name, "holiday")) {
        return RBPOWER_TARIFF_ZONE_HOLIDAY;
    }
    if (strcasestr_local(builder_zone->zone_name, "critical")) {
        return RBPOWER_TARIFF_ZONE_CRITICAL_PEAK;
    }
    
    // Infer from the start time (heuristic)
    if (builder_zone->is_time_based && strlen(builder_zone->time_range) > 0) {
        uint8_t start_hour;
        if (sscanf(builder_zone->time_range, "%hhu:", &start_hour) == 1) {
            if (start_hour >= 22 || start_hour < 6) {
                return RBPOWER_TARIFF_ZONE_NIGHT;
            } else if (start_hour >= 6 && start_hour < 12) {
                return RBPOWER_TARIFF_ZONE_MORNING_PEAK;
            } else if (start_hour >= 17 && start_hour < 22) {
                return RBPOWER_TARIFF_ZONE_EVENING_PEAK;
            }
        }
    }
    
    // Use the zone type given explicitly by the builder
    if (builder_zone->zone_type != 0) {
        return builder_zone->zone_type;
    }
    
    // Default
    return RBPOWER_TARIFF_ZONE_DAY;
}

/**
 * @brief Convert one builder zone into a tariff-system zone (rates, conditions, state).
 * @param[in]  builder_zone Input builder zone.
 * @param[out] tariff_zone  Output tariff zone (fully overwritten).
 * @param[in]  zone_index   Zone index (logging only).
 * @retval ESP_OK              Converted.
 * @retval ESP_ERR_INVALID_ARG NULL argument.
 */
static esp_err_t convert_builder_zone_to_tariff_zone(
    const rbgrid_tariff_builder_zone_t* builder_zone,
    rbpower_tariff_zone_t* tariff_zone,
    uint8_t zone_index)
{
    if (!builder_zone || !tariff_zone) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI("TARIFF_MAPPER", "Converting zone[%d]: %s", 
             zone_index, builder_zone->zone_name);
    
    // Clear the structure
    memset(tariff_zone, 0, sizeof(rbpower_tariff_zone_t));
    
    // === BASIC INFORMATION ===
    tariff_zone->zone_id = determine_zone_type(builder_zone);
    strncpy(tariff_zone->zone_name, builder_zone->zone_name, 
            RBPOWER_TARIFF_NAME_LENGTH - 1);
    
    // Build the description (truncated to avoid overflow)
    if (builder_zone->is_time_based && strlen(builder_zone->time_range) > 0) {
        snprintf(tariff_zone->description, RBPOWER_TARIFF_DESCRIPTION_LENGTH,
                 "%.20s %s", builder_zone->zone_name, builder_zone->time_range);
    } else if (builder_zone->is_seasonal) {
        snprintf(tariff_zone->description, RBPOWER_TARIFF_DESCRIPTION_LENGTH,
                 "%.30s (seasonal)", builder_zone->zone_name);
    } else if (builder_zone->is_weekday_based) {
        snprintf(tariff_zone->description, RBPOWER_TARIFF_DESCRIPTION_LENGTH,
                 "%.30s (weekday)", builder_zone->zone_name);
    } else {
        snprintf(tariff_zone->description, RBPOWER_TARIFF_DESCRIPTION_LENGTH,
                 "%.40s", builder_zone->zone_name);
    }
    
    // === RATES ===
    tariff_zone->rate_buying_per_kwh = builder_zone->rate_per_kwh;

    // The selling rate is set later from net_metering_ratio;
    // here it is only initialized when given explicitly
    tariff_zone->rate_selling_per_kwh = builder_zone->selling_rate_per_kwh > 0 ?
                                        builder_zone->selling_rate_per_kwh :
                                        0.0f; // Computed later from net_metering_ratio

    // net_metering_ratio is set later from the net-metering parameters
    tariff_zone->net_metering_ratio = 0.0f; // Initial value, overridden later
    
    // === MULTIPLIERS ===
    tariff_zone->base_multiplier = 1.0f;
    tariff_zone->seasonal_multiplier = 1.0f;
    tariff_zone->critical_peak_multiplier = 1.0f;
    
    // === APPLICABILITY CONDITIONS ===
    uint8_t condition_idx = 0;
    
    // 1. Time condition
    if (builder_zone->is_time_based && strlen(builder_zone->time_range) > 0) {
        esp_err_t err = parse_time_range_to_condition(
            builder_zone->time_range, 
            &tariff_zone->conditions[condition_idx]);
        if (err == ESP_OK) {
            condition_idx++;
            ESP_LOGI("TARIFF_MAPPER", "  Added time condition: %s", 
                     builder_zone->time_range);
        }
    }
    
    // 2. Weekday condition
    if (builder_zone->is_weekday_based && builder_zone->weekday_mask > 0) {
        rbpower_tariff_condition_t* condition = &tariff_zone->conditions[condition_idx];
        condition->condition_type = RBPOWER_TARIFF_CONDITION_WEEKDAY;
        condition->params.weekday.weekday_mask = builder_zone->weekday_mask;
        condition->is_active = true;
        condition->priority = 5; // Low priority for weekday conditions
        condition_idx++;
        ESP_LOGI("TARIFF_MAPPER", "  Added weekday condition: mask=0x%02X", 
                 builder_zone->weekday_mask);
    }
    
    // 3. Seasonal condition
    if (builder_zone->is_seasonal && 
        (builder_zone->start_month > 0 || builder_zone->end_month > 0)) {
        rbpower_tariff_condition_t* condition = &tariff_zone->conditions[condition_idx];
        condition->condition_type = RBPOWER_TARIFF_CONDITION_DATE_RANGE;
        condition->params.date_range.start_month = builder_zone->start_month;
        condition->params.date_range.start_day = 1;
        condition->params.date_range.end_month = builder_zone->end_month;
        condition->params.date_range.end_day = 31; // Last day of the month
        condition->params.date_range.recurring_annually = true;
        condition->is_active = true;
        condition->priority = 8; // Medium priority for seasons
        condition_idx++;
        ESP_LOGI("TARIFF_MAPPER", "  Added seasonal condition: months %d-%d", 
                 builder_zone->start_month, builder_zone->end_month);
    }
    
    tariff_zone->conditions_count = condition_idx;
    tariff_zone->conditions_and_logic = true; // Default: AND logic
    
    // === LIMITS ===
    tariff_zone->min_duration_minutes = 0.0f; // No minimum duration
    tariff_zone->unlimited_energy = true; // Unlimited energy by default
    tariff_zone->max_energy_kwh = 0.0f;
    
    // === STATE ===
    tariff_zone->is_enabled = true;
    tariff_zone->is_currently_active = false;
    tariff_zone->last_activation_timestamp = 0;
    tariff_zone->activations_count_today = 0;
    
    ESP_LOGI("TARIFF_MAPPER", "  Zone converted: type=%d, rate=%.3f EUR/kWh, conditions=%d",
             tariff_zone->zone_id, tariff_zone->rate_buying_per_kwh, 
             tariff_zone->conditions_count);
    
    return ESP_OK;
}

//=============================================================================
// ACTIVE TARIFF ZONE RESOLUTION
//=============================================================================

/**
 * @brief Check whether a time of day falls in a half-open [start, end) window.
 * @param[in] current_hour   Current hour (0-23).
 * @param[in] current_minute Current minute (0-59).
 * @param[in] start_hour     Window start hour.
 * @param[in] start_minute   Window start minute.
 * @param[in] end_hour       Window end hour.
 * @param[in] end_minute     Window end minute.
 * @param[in] spans_midnight Window wraps midnight (also inferred when start > end).
 * @return true if the time is inside the window.
 */
static bool is_time_in_range(uint8_t current_hour, uint8_t current_minute,
                             uint8_t start_hour, uint8_t start_minute,
                             uint8_t end_hour, uint8_t end_minute,
                             bool spans_midnight)
{
    uint16_t current_minutes = current_hour * 60 + current_minute;
    uint16_t start_minutes = start_hour * 60 + start_minute;
    uint16_t end_minutes = end_hour * 60 + end_minute;

    // Robustness: a window where start > end inherently wraps midnight, even if the config
    // forgot to set spans_midnight. Without this, e.g. 23:00→07:00 with the flag unset
    // evaluates to `>=1380 && <420` = always false → the zone is silently never active.
    bool spanning = spans_midnight || (start_minutes > end_minutes);

    if (spanning) {
        // Wrap range (e.g. 23:00-07:00): half-open [start, 24:00) ∪ [00:00, end).
        return (current_minutes >= start_minutes) || (current_minutes < end_minutes);
    } else {
        // Normal range (e.g. 07:00-23:00): half-open [start, end).
        return (current_minutes >= start_minutes) && (current_minutes < end_minutes);
    }
}

/**
 * @brief Evaluate one tariff zone condition against a local time.
 * @param[in] condition Condition to check.
 * @param[in] timeinfo  Local time.
 * @return true if the condition is met (false for inactive/unknown conditions or NULL).
 */
static bool check_tariff_condition(const rbpower_tariff_condition_t* condition,
                                  const struct tm* timeinfo)
{
    if (!condition || !condition->is_active || !timeinfo) {
        return false;
    }
    
    switch (condition->condition_type) {
        case RBPOWER_TARIFF_CONDITION_TIME_RANGE: {
            // Time window check
            return is_time_in_range(
                timeinfo->tm_hour, timeinfo->tm_min,
                condition->params.time_range.start_hour,
                condition->params.time_range.start_minute,
                condition->params.time_range.end_hour,
                condition->params.time_range.end_minute,
                condition->params.time_range.spans_midnight
            );
        }
        
        case RBPOWER_TARIFF_CONDITION_WEEKDAY: {
            // weekday_mask is Mon-origin (bit0=Mon … bit6=Sun) — the convention the builder
            // and the header use. tm_wday is 0=Sun..6=Sat, so remap before testing the bit
            // (previously used 1<<tm_wday = Sun-origin → selected the wrong days).
            uint8_t mon_idx = (timeinfo->tm_wday == 0) ? 6 : (uint8_t)(timeinfo->tm_wday - 1);
            uint8_t weekday_bit = (uint8_t)(1u << mon_idx);
            return (condition->params.weekday.weekday_mask & weekday_bit) != 0;
        }
        
        case RBPOWER_TARIFF_CONDITION_WEEKEND: {
            // Weekend check (Saturday or Sunday)
            return (timeinfo->tm_wday == 0 || timeinfo->tm_wday == 6);
        }
        
        case RBPOWER_TARIFF_CONDITION_DATE_RANGE: {
            // Date range check (seasonal tariffs)
            uint8_t current_month = timeinfo->tm_mon + 1; // tm_mon: 0-11
            uint8_t current_day = timeinfo->tm_mday;
            
            uint8_t start_month = condition->params.date_range.start_month;
            uint8_t start_day = condition->params.date_range.start_day;
            uint8_t end_month = condition->params.date_range.end_month;
            uint8_t end_day = condition->params.date_range.end_day;
            
            // Simple check for yearly-recurring ranges
            if (start_month <= end_month) {
                // Range within one calendar year
                if (current_month < start_month || current_month > end_month) {
                    return false;
                }
                if (current_month == start_month && current_day < start_day) {
                    return false;
                }
                if (current_month == end_month && current_day > end_day) {
                    return false;
                }
                return true;
            } else {
                // Cross-year range (e.g. Nov 15 → Feb 10). The boundary months must ALSO
                // pass the day filter — previously the day was ignored, so Nov 1-14 and
                // Feb 11-28 wrongly matched.
                if (current_month == start_month) return current_day >= start_day;
                if (current_month == end_month)   return current_day <= end_day;
                return (current_month > start_month) || (current_month < end_month);
            }
        }
        
        case RBPOWER_TARIFF_CONDITION_HOLIDAY: {
            // Holiday = current date is in the configured (yearly-recurring) holiday list.
            return date_is_holiday((uint8_t)(timeinfo->tm_mon + 1), (uint8_t)timeinfo->tm_mday);
        }
        
        default:
            return false;
    }
}

/**
 * @brief Evaluate all conditions of a tariff zone (AND or OR logic per zone).
 * @param[in] zone     Tariff zone to check.
 * @param[in] timeinfo Local time.
 * @return true if the zone is active (a zone without conditions is always active).
 */
static bool is_tariff_zone_active(const rbpower_tariff_zone_t* zone,
                                 const struct tm* timeinfo)
{
    if (!zone || !zone->is_enabled || !timeinfo) {
        return false;
    }
    
    // No conditions: the zone is always active
    if (zone->conditions_count == 0) {
        return true;
    }
    
    // Evaluate the conditions with the zone's logic (AND/OR)
    bool any_condition_met = false;
    bool all_conditions_met = true;
    
    for (uint8_t i = 0; i < zone->conditions_count; i++) {
        bool condition_result = check_tariff_condition(&zone->conditions[i], timeinfo);
        
        if (condition_result) {
            any_condition_met = true;
        } else {
            all_conditions_met = false;
        }
        
        // Early exit
        if (zone->conditions_and_logic && !all_conditions_met) {
            return false; // AND logic: one condition failed
        }
        if (!zone->conditions_and_logic && any_condition_met) {
            return true; // OR logic: one condition met
        }
    }
    
    return zone->conditions_and_logic ? all_conditions_met : any_condition_met;
}

/**
 * @brief Resolve the active tariff zone for a local time.
 *
 * Among active zones the one with the highest type priority wins; with no active zone the
 * plan's default zone (or zone 0) is returned.
 * @param[in] timeinfo Local time.
 * @return Pointer into the active plan's zones, or NULL (no plan / no zones / NULL time).
 */
static rbpower_tariff_zone_t* find_active_tariff_zone(const struct tm* timeinfo)
{
    if (!timeinfo || !g_current_tariff_plan.is_active) {
        return NULL;
    }
    
    rbpower_tariff_zone_t* best_zone = NULL;
    uint8_t best_priority = 0;
    
    // Walk all zones and pick the active one with the highest priority
    for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count; i++) {
        rbpower_tariff_zone_t* zone = &g_current_tariff_plan.zones[i];
        
        bool zone_active = is_tariff_zone_active(zone, timeinfo);
        
        if (zone_active) {
            // Zone priority
            uint8_t zone_priority = 0;
            
            // Priority by zone type
            switch (zone->zone_id) {
                case RBPOWER_TARIFF_ZONE_CRITICAL_PEAK:
                    zone_priority = 100; // Highest priority
                    break;
                case RBPOWER_TARIFF_ZONE_HOLIDAY:
                    zone_priority = 90;
                    break;
                case RBPOWER_TARIFF_ZONE_WEEKEND:
                    // The weekend zone has a high priority only on weekends
                    if (timeinfo->tm_wday == 0 || timeinfo->tm_wday == 6) {
                        zone_priority = 80;
                    } else {
                        zone_priority = 20; // Low priority on weekdays
                    }
                    break;
                case RBPOWER_TARIFF_ZONE_EVENING_PEAK:
                    zone_priority = 70;
                    break;
                case RBPOWER_TARIFF_ZONE_MORNING_PEAK:
                    zone_priority = 65;
                    break;
                case RBPOWER_TARIFF_ZONE_NIGHT:
                    zone_priority = 60;
                    break;
                case RBPOWER_TARIFF_ZONE_DAY:
                    zone_priority = 50;
                    break;
                case RBPOWER_TARIFF_ZONE_LATE_EVENING:
                    zone_priority = 55;
                    break;
                default:
                    zone_priority = 10;
                    break;
            }
            
            // NOTE: zone priority is determined by zone TYPE only (above). The previous
            // `+= conditions_count*2` could invert ordering — a multi-condition DAY zone
            // (base 50) would outrank a NIGHT/PEAK zone — picking the wrong (often cheaper)
            // rate on an overlap. Specificity is not priority; removed.

            if (zone_priority > best_priority) {
                best_zone = zone;
                best_priority = zone_priority;
            }
        }
    }
    
    // No active zone found: return the default zone
    if (!best_zone && g_current_tariff_plan.active_zones_count > 0) {
        for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count; i++) {
            if (g_current_tariff_plan.zones[i].zone_id == g_current_tariff_plan.default_zone_id) {
                best_zone = &g_current_tariff_plan.zones[i];
                break;
            }
        }
        // No default zone found: take the first one
        if (!best_zone) {
            best_zone = &g_current_tariff_plan.zones[0];
        }
    }
    
    return best_zone;
}

/* Validate a tariff plan and LOG collisions/conflicts (non-fatal) — surfaces config
 * problems the runtime would otherwise resolve silently via priority/default:
 *  - negative buying/selling rates,
 *  - duplicate zone ids,
 *  - 24h coverage gaps (hour billed at the default zone) and overlaps (priority-resolved).
 * Coverage is sampled on a representative weekday (mid-year, no DST edge); weekend/seasonal
 * zones are intentionally not flagged as gaps here. */
static void validate_tariff_plan(const rbpower_tariff_plan_t* plan)
{
    if (!plan) return;
    uint8_t n = plan->active_zones_count;
    if (n > RBPOWER_TARIFF_ZONE_COUNT) n = RBPOWER_TARIFF_ZONE_COUNT;

    for (uint8_t i = 0; i < n; i++) {
        const rbpower_tariff_zone_t* z = &plan->zones[i];
        if (z->rate_buying_per_kwh < 0.0f)
            ESP_LOGW("RBHOME_TARIFF", "validate: zone %u '%s' NEGATIVE buying rate %.4f", i, z->zone_name, z->rate_buying_per_kwh);
        if (z->rate_selling_per_kwh < 0.0f)
            ESP_LOGW("RBHOME_TARIFF", "validate: zone %u '%s' NEGATIVE selling rate %.4f", i, z->zone_name, z->rate_selling_per_kwh);
        for (uint8_t j = (uint8_t)(i + 1); j < n; j++)
            if (z->zone_id == plan->zones[j].zone_id)
                ESP_LOGW("RBHOME_TARIFF", "validate: DUPLICATE zone_id %d (zones %u and %u)", z->zone_id, i, j);
    }

    struct tm s; memset(&s, 0, sizeof(s));
    s.tm_year = 125; s.tm_mon = 5; s.tm_mday = 18; s.tm_wday = 3; // Wed, mid-year
    uint8_t gaps = 0, overlaps = 0;
    for (uint8_t h = 0; h < 24; h++) {
        s.tm_hour = h; s.tm_min = 30;
        uint8_t active = 0;
        for (uint8_t i = 0; i < n; i++)
            if (is_tariff_zone_active(&plan->zones[i], &s)) active++;
        if (active == 0) { gaps++;     ESP_LOGW("RBHOME_TARIFF", "validate: COVERAGE GAP at %02u:30 → default rate", h); }
        else if (active > 1) { overlaps++; ESP_LOGW("RBHOME_TARIFF", "validate: OVERLAP at %02u:30 (%u zones, priority-resolved)", h, active); }
    }
    ESP_LOGI("RBHOME_TARIFF", "validate: plan '%s': %u zones, %u gap-hour(s), %u overlap-hour(s)",
             plan->plan_name, n, gaps, overlaps);
}


/* Tariff system readiness check (initialized, enabled, plan active, time valid). */
bool rbpower_tariff_system_is_ready(void)
{
    bool initialized = g_tariff_system_initialized;
    bool enabled = g_tariff_system_enabled;
    bool active = g_current_tariff_plan.is_active;
    bool time_valid = rbtime_is_time_valid();
    
    ESP_LOGI("RBHOME_TARIFF", "Tariff system status check:");
    ESP_LOGI("RBHOME_TARIFF", "  initialized: %s", initialized ? "YES" : "NO");
    ESP_LOGI("RBHOME_TARIFF", "  enabled: %s", enabled ? "YES" : "NO");
    ESP_LOGI("RBHOME_TARIFF", "  plan active: %s", active ? "YES" : "NO");
    ESP_LOGI("RBHOME_TARIFF", "  time valid: %s", time_valid ? "YES" : "NO");
    
    // All readiness conditions must hold
    bool ready = initialized && enabled && active && time_valid;
    
    if (!time_valid) {
        ESP_LOGW("RBHOME_TARIFF", "Time not valid - tariff system waiting for time synchronization");
    }
    
    return ready;
}

/* Current tariff plan (read-only), NULL before init. */
const rbpower_tariff_plan_t* rbpower_tariff_get_current_plan(void)
{
    if (!g_tariff_system_initialized) {
        return NULL;
    }
    return &g_current_tariff_plan;
}

const char* rbpower_tariff_currency(void)
{
    // The configured currency is a pure DISPLAY label (no conversion anywhere). Returns the
    // plan's currency_code, or a default if unset. Use in user-facing cost messages.
    if (g_current_tariff_plan.currency_code[0] != '\0') {
        return g_current_tariff_plan.currency_code;
    }
    return "EUR";
}

//=============================================================================
// ACTIVE TARIFF ZONE DETERMINATION
//=============================================================================

/**
 * @brief Get the tariff clock: epoch seconds and local calendar time (time zone set by rbtime).
 *
 * Uses the `tarifftime` override when set, the accelerated virtual clock in a time-scaled test
 * build, otherwise rbtime's real time.
 */
static void get_current_local_time(struct tm* timeinfo, uint64_t* unix_timestamp)
{
    if (!timeinfo || !unix_timestamp) {
        return;
    }
    
    // Unix timestamp (UTC).
    if (g_tariff_test_time_override_s >= 0) {
        /* TEST scenario override (set via the `tarifftime` verb): freeze the tariff calendar clock at a
         * chosen instant so a test can jump to ANY scenario — a weekend Saturday, a holiday, a peak
         * hour, deep night — and instantly verify the resolved zone & rate without waiting for the clock
         * to advance there. Maximizes scenario coverage. -1 clears it. */
        *unix_timestamp = (uint64_t)g_tariff_test_time_override_s;
        time_t ovr = (time_t)(*unix_timestamp);
        struct tm ovr_tm;
        if (localtime_r(&ovr, &ovr_tm)) memcpy(timeinfo, &ovr_tm, sizeof(struct tm));
        return;
    }
#if RBGRID_STATS_TIME_SCALE > 1
    /* Accelerated TEST mode: use the SAME virtual clock as the stats boundary dispatcher
     * (uptime × scale, anchored at boot = epoch 0). This makes the tariff CALENDAR advance at the
     * accelerated rate too, so zone transitions (TOU day/night), weekend (virtual Sat/Sun) and holiday
     * (virtual MM-DD) become testable in MINUTES. Production (scale=1) uses real SNTP time
     * for correct billing. NOTE: epoch 0 = 1970-01-01 (a Thursday) → virtual weekend ≈ days 2-3. */
    *unix_timestamp = (uint64_t)esp_timer_get_time() * RBGRID_STATS_TIME_SCALE / 1000000ULL;
#else
    *unix_timestamp = rbtime_get_unix_timestamp();
#endif
    time_t current_time = (time_t)(*unix_timestamp);

    // Local time WITH DST via the C library: rbtime sets the TZ env var from the
    // configured time zone, so localtime_r applies the correct UTC offset AND
    // daylight-saving transitions automatically. Reentrant (this is called from the
    // 3-sec stats path) — no static-buffer race like gmtime().
    struct tm local_tm;
    if (localtime_r(&current_time, &local_tm)) {
        memcpy(timeinfo, &local_tm, sizeof(struct tm));
    }
}

/* The divergent duplicate condition-checker family (check_time_range/_weekday/_date_range/
 * _zone_condition/_zone_conditions) was removed here — it was never called and disagreed
 * with the live path (check_tariff_condition). One source of truth. */

/**
 * @brief Determine the active tariff zone and its effective buying/selling rates (currency/kWh).
 */
static rbpower_tariff_status_t determine_active_tariff_zone(rbpower_tariff_zone_type_t* zone_id,
                                                           float* buying_rate,
                                                           float* selling_rate)
{
    if (!zone_id || !buying_rate || !selling_rate) {
        return RBPOWER_TARIFF_STATUS_CALCULATION_ERROR;
    }
    
    // Check system readiness
    if (!g_tariff_system_initialized || !g_current_tariff_plan.is_active) {
        return RBPOWER_TARIFF_STATUS_NO_PLAN;
    }

    // Get current time
    struct tm current_time;
    uint64_t current_timestamp;
    get_current_local_time(&current_time, &current_timestamp);

    // Check time validity - use fallback if SNTP not ready
    bool sntp_ready = rbtime_is_time_valid();
    if (!sntp_ready) {
        ESP_LOGD("RBHOME_TARIFF", "SNTP not ready - using fallback time for tariffing");
        // Continue with fallback time from config
        // Don't return error - use available time
    }

    // Use new unified function to determine active zone
    rbpower_tariff_zone_t* best_zone = find_active_tariff_zone(&current_time);

    if (!best_zone) {
        ESP_LOGE("RBHOME_TARIFF", "No suitable tariff zone found");
        return RBPOWER_TARIFF_STATUS_ZONE_NOT_FOUND;
    }

    // Fill results
    *zone_id = best_zone->zone_id;
    *buying_rate = best_zone->rate_buying_per_kwh * best_zone->base_multiplier *
                   best_zone->seasonal_multiplier;
    *selling_rate = best_zone->rate_selling_per_kwh * best_zone->base_multiplier *
                    best_zone->seasonal_multiplier;

    ESP_LOGD("RBHOME_TARIFF", "Active tariff zone determined: %d (buy: %.3f, sell: %.3f)",
             *zone_id, *buying_rate, *selling_rate);
    
    return RBPOWER_TARIFF_STATUS_OK;
}

/**
 * @brief Update active tariff zone cache
 */
static esp_err_t update_active_zone_cache(void)
{
    if (!g_tariff_system_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Prevent watchdog timeout
    vTaskDelay(1);

    rbpower_tariff_zone_type_t new_zone_id;
    float new_buying_rate, new_selling_rate;

    // Determine new active zone
    rbpower_tariff_status_t status = determine_active_tariff_zone(&new_zone_id,
                                                                 &new_buying_rate,
                                                                 &new_selling_rate);

    if (status != RBPOWER_TARIFF_STATUS_OK) {
        // Log spam protection - only log if time not synchronized
        static uint32_t last_log_time = 0;
        uint32_t current_time = esp_timer_get_time() / 1000000; // seconds

        if (status == RBPOWER_TARIFF_STATUS_INVALID_TIME) {
            // For time error, log less frequently - once per 60 seconds
            if (current_time - last_log_time > 60) {
                ESP_LOGI("RBHOME_TARIFF", "Tariffing using fallback time (SNTP not synced)");
                last_log_time = current_time;
            }
        } else {
            // For other errors, log once per 10 seconds
            if (current_time - last_log_time > 10) {
                ESP_LOGD("RBHOME_TARIFF", "Tariff zone determination error: %d", status);
                last_log_time = current_time;
            }
        }

        // On error, use last valid values
        if (g_active_zone_cache.zone_cache_valid) {
            return ESP_OK;  // Keep old cache
        } else {
            return ESP_FAIL;
        }
    }

    // Update cache
    uint64_t current_timestamp = rbtime_get_unix_timestamp();
    bool zone_changed = (g_active_zone_cache.current_zone_id != new_zone_id) ||
                       !g_active_zone_cache.zone_cache_valid;
    const char* zone_name = "Unknown";
    if (zone_changed) {
        // Find zone name

        for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count; i++) {
            if (g_current_tariff_plan.zones[i].zone_id == new_zone_id) {
                zone_name = g_current_tariff_plan.zones[i].zone_name;
                break;
            }
        }
        
        ESP_LOGI("RBHOME_TARIFF", "Tariff zone change: %s -> %s (%.3f %s/kWh)",
                 g_active_zone_cache.current_zone_name, zone_name, new_buying_rate,
                 rbpower_tariff_currency());

        g_active_zone_cache.zone_activation_timestamp = current_timestamp;
    }
    
    // Update all cache fields
    g_active_zone_cache.current_zone_id = new_zone_id;
    /* The per-zone `is_currently_active` flag reported in tariff-plan-config used to stay false
     * (while system.tariff.zone said DAY). Keep it in step with the cache. */
    for (uint8_t i = 0; i < g_current_tariff_plan.active_zones_count && i < RBPOWER_TARIFF_ZONE_COUNT; i++) {
        rbpower_tariff_zone_t* z = &g_current_tariff_plan.zones[i];
        bool now_active = (z->zone_id == new_zone_id);
        if (now_active && !z->is_currently_active && zone_changed) {
            z->last_activation_timestamp = current_timestamp;
            z->activations_count_today++;
        }
        z->is_currently_active = now_active;
    }
    strncpy(g_active_zone_cache.current_zone_name,
            (zone_changed && zone_name) ? zone_name : g_active_zone_cache.current_zone_name,
            sizeof(g_active_zone_cache.current_zone_name) - 1);
    g_active_zone_cache.current_buying_rate = new_buying_rate;
    g_active_zone_cache.current_selling_rate = new_selling_rate;
    g_active_zone_cache.zone_cache_timestamp = current_timestamp;
    g_active_zone_cache.zone_cache_valid = true;

    // Log on first successful update
    static bool first_success_logged = false;
    if (!first_success_logged) {
        bool sntp_ready = rbtime_is_time_valid();
        ESP_LOGI("RBHOME_TARIFF", "✅ Tariffing active (mode: %s)",
                 sntp_ready ? "SNTP synced" : "fallback time");
        first_success_logged = true;
    }
    
    return ESP_OK;
}

/* Current active tariff zone from the cache (refreshed when older than 60 s). */
rbpower_err_t rbpower_tariff_get_current_zone(rbpower_tariff_zone_type_t* zone_id,
                                             char* zone_name,
                                             float* current_rate)
{
    if (!zone_id) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!g_tariff_system_initialized) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Update cache (if needed)
    uint64_t current_time = rbtime_get_unix_timestamp();
    uint64_t cache_age_seconds = current_time - g_active_zone_cache.zone_cache_timestamp;

    // Update cache every minute or if invalid
    if (!g_active_zone_cache.zone_cache_valid || cache_age_seconds > 60) {
        esp_err_t err = update_active_zone_cache();
        if (err != ESP_OK && !g_active_zone_cache.zone_cache_valid) {
            return RBPOWER_ERR_INVALID_STATE;
        }
    }

    // Return data from cache
    *zone_id = g_active_zone_cache.current_zone_id;
    
    if (zone_name) {
        strncpy(zone_name, g_active_zone_cache.current_zone_name, 
                RBPOWER_TARIFF_NAME_LENGTH - 1);
        zone_name[RBPOWER_TARIFF_NAME_LENGTH - 1] = '\0';
    }
    
    if (current_rate) {
        *current_rate = g_active_zone_cache.current_buying_rate;
    }
    
    return RBPOWER_OK;
}

/* Update the tariff zone cache (called from the 3-sec period handler).
 *
 * Call periodically (e.g. every 3 s) to refresh the active zone cache, so the
 * high-frequency rbpower_get_current_tariff_rate_simple() needs no time checks. */
esp_err_t rbpower_tariff_update_zone_cache(void)
{
    // Check system readiness
    if (!g_tariff_system_initialized || !g_tariff_system_enabled || !g_current_tariff_plan.is_active) {
        return ESP_ERR_INVALID_STATE;
    }

    // Get current time
    struct tm timeinfo_struct;
    uint64_t unix_timestamp;
    get_current_local_time(&timeinfo_struct, &unix_timestamp);
    struct tm* timeinfo = &timeinfo_struct;

    // Use function to determine active zone
    rbpower_tariff_zone_t* active_zone = find_active_tariff_zone(timeinfo);

    if (active_zone) {
        // Save previous value for logging
        rbpower_tariff_zone_type_t prev_zone_id = g_active_zone_cache.current_zone_id;
        
        // Log zone change only if actually changed
        if (!g_active_zone_cache.zone_cache_valid || prev_zone_id != active_zone->zone_id) {
            if (!g_active_zone_cache.zone_cache_valid) {
                ESP_LOGI("RBHOME_TARIFF", "Initializing tariff zone: %s (%.3f EUR/kWh)",
                        active_zone->zone_name, active_zone->rate_buying_per_kwh);
            } else if (prev_zone_id != active_zone->zone_id) {
                ESP_LOGI("RBHOME_TARIFF", "Tariff zone change: %s -> %s (%.3f EUR/kWh)",
                        prev_zone_id < RBPOWER_TARIFF_ZONE_COUNT ?
                            rbpower_tariff_zone_type_text[prev_zone_id] : "UNKNOWN",
                        active_zone->zone_id < RBPOWER_TARIFF_ZONE_COUNT ?
                            rbpower_tariff_zone_type_text[active_zone->zone_id] : "UNKNOWN",
                        active_zone->rate_buying_per_kwh);
            }
        }

        // Update global active zone cache
        g_active_zone_cache.current_zone_id = active_zone->zone_id;
        strncpy(g_active_zone_cache.current_zone_name, active_zone->zone_name,
               RBPOWER_TARIFF_NAME_LENGTH - 1);
        g_active_zone_cache.current_buying_rate = active_zone->rate_buying_per_kwh;
        g_active_zone_cache.current_selling_rate = active_zone->rate_selling_per_kwh;
        g_active_zone_cache.zone_cache_timestamp = esp_timer_get_time();
        g_active_zone_cache.zone_cache_valid = true;
        
        if (!g_active_zone_cache.zone_cache_valid || prev_zone_id != active_zone->zone_id) {
            g_active_zone_cache.zone_activation_timestamp = rbtime_get_unix_timestamp();   /* epoch s, as on the other path */
        }
        
        return ESP_OK;
    } else {
        ESP_LOGW("RBHOME_TARIFF", "Failed to determine active zone");
        g_active_zone_cache.zone_cache_valid = false;
        return ESP_ERR_NOT_FOUND;
    }
}

/* Force an update of the active zone cache. */
rbpower_err_t rbpower_tariff_update_active_zone(void)
{
    esp_err_t err = update_active_zone_cache();
    return (err == ESP_OK) ? RBPOWER_OK : RBPOWER_ERR_INVALID_STATE;
}

/* Check whether the tariff zone changed within the last N seconds. */
bool rbpower_tariff_zone_recently_changed(uint32_t seconds_threshold)
{
    if (!g_active_zone_cache.zone_cache_valid) {
        return false;
    }
    
    uint64_t current_time = rbtime_get_unix_timestamp();
    uint64_t activation_age = current_time - g_active_zone_cache.zone_activation_timestamp;
    
    return (activation_age <= seconds_threshold);
}

//=============================================================================
// INSTANTANEOUS COST CALCULATION
//=============================================================================

/* Current buying rate (currency/kWh) from the zone cache; 0.15 when the tariff system is not ready. */
float rbpower_get_current_tariff_rate_simple(void) {
    if (!g_tariff_system_initialized || !g_tariff_system_enabled || !g_current_tariff_plan.is_active) {
        // Log only on first call to avoid spam
        static bool warning_logged = false;
        if (!warning_logged) {
            ESP_LOGW("RBHOME_TARIFF", "  Tariff system not ready, using base rate 0.15 EUR/kWh");
            warning_logged = true;
        }
        return 0.15f; // Default base rate
    }

    // Check cache validity
    if (!g_active_zone_cache.zone_cache_valid) {
        // If cache not valid, try to update once on first call
        esp_err_t err = rbpower_tariff_update_zone_cache();
        if (err != ESP_OK) {
            // If update failed, return default value
            return 0.15f;
        }
    }

    // Return cached value
    // Cache is updated periodically from 3-second handler
    return g_active_zone_cache.current_buying_rate;
}


//=============================================================================
// DIRECT ECONOMICS UPDATE
//=============================================================================

/* Direct economics update of a unit result (all-in consumption cost or feed-in revenue). */
void rbpower_update_economics_direct(rbpower_unit_id_t unit_id,
                                    float energy_kwh,
                                    float buying_rate,
                                    float selling_rate,
                                    rbgrid_snapshot_base_t* result_base) {

    if (!result_base) {
        ESP_LOGE("RBHOME_TARIFF", "    Unit[%d]: Economics structure NULL", unit_id);
        return;
    }

    // Update accumulators by energy direction. Consumption is costed at the BUYING rate;
    // export (negative energy) is credited at the SELLING / feed-in rate — previously both
    // used the buying rate, so export was over-credited and the selling rate was dead.
    if (energy_kwh > 0.0f) {
        /* ALL-IN billable cost (not just the energy rate): the real bill = energy×zone_rate
         * + per-kWh regulated fees (grid usage + energy tax + renewable levy) + VAT on the lot.
         * Fees/VAT come from the active plan (0 if a plan isn't configured → graceful = energy cost
         * only). cost_consumed therefore holds the amount actually payable; the breakdown
         * (energy / fees / VAT) is reconstructable from consumed_wh + the plan rates on demand. */
        const rbpower_tariff_plan_t* pl = &g_current_tariff_plan;
        // Progressive (block-tier) overlay: scale the zone buying rate by the tier the cumulative
        // monthly import has reached (opt-in; no-op when progressive_config is disabled). Uses the
        // import-so-far BEFORE this event is added below, so this kWh is priced at the tier it enters.
        if (pl->progressive_config.is_enabled && unit_id < RBPOWER_MAX_HOME_UNITS) {
            buying_rate = progressive_effective_rate(buying_rate, g_net_metering[unit_id].period_consumed_kwh);
        }
        double energy_cost  = (double)energy_kwh * buying_rate;
        double per_kwh_fees = (double)energy_kwh *
            ((double)pl->grid_usage_fee_per_kwh + (double)pl->energy_tax_per_kwh + (double)pl->renewable_levy_per_kwh);
        double subtotal = energy_cost + per_kwh_fees;
        double cost = subtotal * (1.0 + (double)pl->vat_percent / 100.0);   // VAT-inclusive all-in
        result_base->cost_consumed += cost;
        lifetime_cost_add(unit_id, cost, 0.0);          /* system/unit cost_day / cost_total */
        // Real per-zone cost distribution (replaces the faked single-zone split in reports):
        // attribute this all-in consumption cost to whichever zone is active right now.
        rbpower_tariff_zone_type_t z = g_active_zone_cache.current_zone_id;
        if (z < RBPOWER_TARIFF_ZONE_COUNT && unit_id < RBPOWER_MAX_HOME_UNITS) {
            g_unit_direct_costs[unit_id].device_analytics.cost_distribution_by_zone[z] += (float)cost;
        }
        // Net-metering: accumulate this billing period's IMPORT (settled at the month boundary).
        if (unit_id < RBPOWER_MAX_HOME_UNITS) { nm_restore(unit_id); g_net_metering[unit_id].period_consumed_kwh += (double)energy_kwh; nm_persist(unit_id); }
    } else if (energy_kwh < 0.0f) {
        double revenue = (double)(-energy_kwh) * selling_rate;
        result_base->revenue_supplied += revenue;
        lifetime_cost_add(unit_id, 0.0, revenue);        /* system/unit rev_day / rev_total */
        // Per-zone feed-in REVENUE distribution (symmetric to cost_distribution_by_zone) — the "energy
        // return statistics": how much was earned exporting in each tariff zone.
        rbpower_tariff_zone_type_t z = g_active_zone_cache.current_zone_id;
        if (z < RBPOWER_TARIFF_ZONE_COUNT && unit_id < RBPOWER_MAX_HOME_UNITS) {
            g_unit_direct_costs[unit_id].device_analytics.revenue_distribution_by_zone[z] += (float)revenue;
        }
        // Net-metering: accumulate this billing period's EXPORT (settled at the month boundary).
        if (unit_id < RBPOWER_MAX_HOME_UNITS) { nm_restore(unit_id); g_net_metering[unit_id].period_supplied_kwh += (double)(-energy_kwh); nm_persist(unit_id); }
        ESP_LOGD("RBHOME_TARIFF", "      Supply: +%.4f revenue @%.4f sell (total: %.4f)",
                 revenue, selling_rate, result_base->revenue_supplied);
    } else {
        ESP_LOGD("RBHOME_TARIFF", "      Zero energy, cost unchanged");
    }

    // Average tariff = energy-weighted EFFECTIVE rate. For consumption it is the all-in rate
    // (zone rate + per-kWh fees, VAT-inclusive) so it matches the all-in cost_consumed; for export
    // it is the plain sell rate (feed-in revenue carries no fees/VAT).
    if (fabsf(energy_kwh) > 0.001f) {
        float ref_rate;
        if (energy_kwh >= 0.0f) {
            const rbpower_tariff_plan_t* pl = &g_current_tariff_plan;
            float per_kwh_fees = pl->grid_usage_fee_per_kwh + pl->energy_tax_per_kwh + pl->renewable_levy_per_kwh;
            ref_rate = (buying_rate + per_kwh_fees) * (1.0f + pl->vat_percent / 100.0f);
        } else {
            ref_rate = selling_rate;
        }
        if (result_base->samples_count == 0) {
            result_base->avg_tariff_per_kwh = ref_rate;
        } else {
            result_base->avg_tariff_per_kwh =
                (result_base->avg_tariff_per_kwh * result_base->samples_count + ref_rate) /
                (result_base->samples_count + 1);
        }
        result_base->samples_count++;
    }

    // Update time
    result_base->period_end_us = esp_timer_get_time();

    ESP_LOGD("RBHOME_TARIFF", "      Result: net cost=%.4f EUR, avg tariff=%.4f EUR/kWh, samples=%lu",
             (result_base->cost_consumed - result_base->revenue_supplied), result_base->avg_tariff_per_kwh, result_base->samples_count);
}

/* Cost a single INJECTED period RESULT (rbAmp / external-source path). The 3-sec accumulation path costs
 * at the 3-sec finalize (rbpower_tariff_process_3sec_period_for_unit → rbpower_update_economics_direct);
 * the rbAmp inject path bypasses 3-sec accumulation, so without this its result_base economics would stay
 * 0. Applies the SAME cost model — signed kWh (sign = direction) ×
 * the CURRENT zone's buying (consume) / selling (feed-in) rate. The zone cache is refreshed for the
 * inject's wall-clock instant (it runs out of phase with the 3-sec handler that normally refreshes it),
 * so a TOU/weekend/holiday rate in effect at inject time is applied.
 * ⚠ unit_id here is the units[] ARRAY INDEX, not definition.unit_id — the caller (rbhome.c inject)
 * passes unit_index, and the g_unit_direct_costs/g_net_metering tables downstream are index-keyed
 * (the read side, get_zone_stats, resolves via find_unit_index_by_id to the same space). */
void rbpower_tariff_cost_injected_period(rbpower_unit_id_t unit_id, float energy_wh,
                                         rbgrid_snapshot_base_t* result_base) {
    if (!result_base) {
        return;
    }
    /* The quarter is injected just AFTER it ends: resolving the zone for "now" priced the quarter that ends at a
     * zone boundary (06:45–07:00, injected 07:00:0x) at the NEXT zone's rate, and attributed it to that zone
     * (cost, per-zone split, net metering) — 2–4 of 96 quarters a day under TOU.
     * Price at the quarter's midpoint. The `tarifftime` test override freezes the instant — no shift then. */
    rbpower_tariff_zone_t* mid_zone = NULL;
    if (g_tariff_test_time_override_s < 0 && g_tariff_system_initialized && g_tariff_system_enabled &&
        g_current_tariff_plan.is_active) {
        struct tm tm_now; uint64_t ts_now = 0;
        memset(&tm_now, 0, sizeof(tm_now));
        get_current_local_time(&tm_now, &ts_now);
        if (ts_now > 450) {
            time_t mid = (time_t)(ts_now - 450);
            struct tm tm_mid;
            if (localtime_r(&mid, &tm_mid)) mid_zone = find_active_tariff_zone(&tm_mid);
        }
    }
    rbpower_tariff_update_zone_cache();                          // resolve the active zone for "now"
    rbpower_tariff_zone_type_t saved_id = g_active_zone_cache.current_zone_id;
    float saved_buy = g_active_zone_cache.current_buying_rate, saved_sell = g_active_zone_cache.current_selling_rate;
    if (mid_zone && g_active_zone_cache.zone_cache_valid) {      /* costing + per-zone attribution read the cache */
        g_active_zone_cache.current_zone_id = mid_zone->zone_id;
        g_active_zone_cache.current_buying_rate = mid_zone->rate_buying_per_kwh;
        g_active_zone_cache.current_selling_rate = mid_zone->rate_selling_per_kwh;
    }
    float buying  = rbpower_get_current_tariff_rate_simple();    // zone-aware cached buying rate
    float selling = g_active_zone_cache.current_selling_rate;    // zone-aware selling / feed-in rate
    if (buying  < 0.0f) buying  = 0.0f;
    if (selling < 0.0f) selling = 0.0f;
    rbpower_update_economics_direct(unit_id, energy_wh / 1000.0f, buying, selling, result_base);
    g_active_zone_cache.current_zone_id = saved_id;              /* the cache keeps describing "now" */
    g_active_zone_cache.current_buying_rate = saved_buy;
    g_active_zone_cache.current_selling_rate = saved_sell;
}

/* Diagnostic snapshot of the tariff state for the `tariffdiag` verb (rbbench_reply, since ESP_LOG
 * is invisible on the rbbench console). Reports the CURRENT (virtual in accel) calendar time + the
 * resolved active zone & rates — lets a tester watch zone transitions / weekend / holiday switching as
 * the virtual clock advances. */
void rbpower_tariff_get_diag(int* hour, int* wday, int* mday, int* mon,
                             int* zone_id, float* buying, float* selling, int* is_holiday)
{
    struct tm ti; uint64_t ts = 0;
    memset(&ti, 0, sizeof(ti));
    get_current_local_time(&ti, &ts);
    rbpower_tariff_update_zone_cache();   // resolve the active zone for "now"
    if (hour)    *hour    = ti.tm_hour;
    if (wday)    *wday    = ti.tm_wday;          // 0=Sun .. 6=Sat
    if (mday)    *mday    = ti.tm_mday;
    if (mon)     *mon     = ti.tm_mon + 1;       // 1..12
    if (zone_id) *zone_id = (int)g_active_zone_cache.current_zone_id;
    if (buying)  *buying  = g_active_zone_cache.current_buying_rate;
    if (selling) *selling = g_active_zone_cache.current_selling_rate;
    if (is_holiday) *is_holiday = date_is_holiday(ti.tm_mon + 1, ti.tm_mday) ? 1 : 0;
}

/* Per-zone cost + feed-in-revenue breakdown for a unit (`zonestats` diagnostic verb) — the tariff
 * "statistics" view: how much was spent / earned in each tariff zone. Fills cost_by_zone[] and
 * rev_by_zone[] (caller sizes >= RBPOWER_TARIFF_ZONE_COUNT). Zeroed if the unit isn't bound. */
void rbpower_tariff_get_zone_stats(rbpower_unit_id_t unit_id, float* cost_by_zone, float* rev_by_zone)
{
    uint8_t idx = find_unit_index_by_id(unit_id);
    for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        float c = 0.0f, r = 0.0f;
        if (idx != 0xFF && idx < RBPOWER_MAX_HOME_UNITS) {
            c = g_unit_direct_costs[idx].device_analytics.cost_distribution_by_zone[z];
            r = g_unit_direct_costs[idx].device_analytics.revenue_distribution_by_zone[z];
        }
        if (cost_by_zone) cost_by_zone[z] = c;
        if (rev_by_zone)  rev_by_zone[z]  = r;
    }
}

//=============================================================================
// SIMPLIFIED 3-SEC PERIOD PROCESSING
//=============================================================================

/* Simplified tariff processing of a completed 3-sec period for one unit. */
esp_err_t rbpower_tariff_process_3sec_period_for_unit(rbpower_unit_id_t unit_id) {
    //ESP_LOGI("RBHOME_TARIFF", "=== SIMPLIFIED 3-SEC PERIOD TARIFF PROCESSING for UNIT ===");

    // Check system readiness
    if (!g_tariff_system_initialized || !g_tariff_system_enabled) {
        ESP_LOGD("RBHOME_TARIFF", "Tariff system not ready (init=%s, enabled=%s)",
                 g_tariff_system_initialized ? "YES" : "NO",
                 g_tariff_system_enabled ? "YES" : "NO");
        return ESP_OK;
    }

    // Get access to home network
    rbpower_electrical_network_t* network;
    rbpower_err_t net_err = rbpower_get_network(&network);
    if (net_err != RBPOWER_OK) {
        ESP_LOGE("RBHOME_TARIFF", "Failed to get network access: %d", net_err);
        return ESP_ERR_INVALID_STATE;
    }

    // Get current tariff rate (once for all units). Rate 0 is VALID (a free zone → cost 0);
    // only a NEGATIVE rate is a config error. Previously rate<=0 aborted the whole period,
    // which dropped all cost accounting for a legitimately-free zone.
    float current_tariff_rate = rbpower_get_current_tariff_rate_simple();
    if (current_tariff_rate < 0.0f) {
        ESP_LOGW("RBHOME_TARIFF", "Negative tariff rate: %.4f EUR/kWh — clamping to 0", current_tariff_rate);
        current_tariff_rate = 0.0f;
    }

    // Single-unit processing. "Nothing to do" cases (invalid/inactive unit, tariffing off,
    // period still accumulating, energy below threshold → cost 0) are NORMAL and return
    // ESP_OK — they used to return ESP_ERR_INVALID_STATE, which the caller logged as an error.
    if (!network->units[unit_id].has_valid_definition ||
        !network->units[unit_id].definition.is_active) {
        return ESP_OK;
    }
    if (!g_unit_tariff_configs[unit_id].tariff_tracking_enabled) {
        return ESP_OK;
    }

    // 3-sec statistics for this unit (non-const: we write the cost result back into it).
    rbpower_period_statistics_t* stats_3sec =
        &network->units[unit_id].statistics.periods[RBPOWER_STAT_PERIOD_3SEC];

    if (!stats_3sec->period_complete) {
        return ESP_OK;  // period still accumulating — not an error
    }

    float energy_kwh = stats_3sec->energy_period.net_wh / 1000.0f; // Wh → kWh
    if (fabsf(energy_kwh) < 0.0001f) { // < 0.1 Wh → cost 0, nothing to accumulate
        return ESP_OK;
    }

    // Direct economics update into the result struct (no separate accumulators).
    rbpower_update_economics_direct(unit_id, energy_kwh, current_tariff_rate,
                                    g_active_zone_cache.current_selling_rate,
                                    &stats_3sec->result_base);
    return ESP_OK;
}


//=============================================================================
// TARIFF RESULT API FUNCTIONS
//=============================================================================

/* Instant cost for a power sample over a short duration (used by get_instant_result and
 * get_forecast). Consumption is priced at the active zone's buying rate; export at the selling rate. */
rbpower_tariff_status_t rbpower_tariff_calculate_instant_cost(rbpower_unit_id_t unit_id,
                                                              float power_kw,
                                                              float period_duration_seconds,
                                                              rbpower_current_direction_t direction,
                                                              rbpower_tariff_instant_result_t *result)
{
    if (!result) {
        return RBPOWER_TARIFF_STATUS_CALCULATION_ERROR;
    }
    memset(result, 0, sizeof(*result));
    result->unit_id = unit_id;
    result->calculation_timestamp = (uint64_t)esp_timer_get_time();
    result->instant_power_kw = power_kw;
    result->power_direction = direction;

    if (!g_tariff_system_initialized || !g_tariff_system_enabled || !g_current_tariff_plan.is_active) {
        result->status = RBPOWER_TARIFF_STATUS_NO_PLAN;
        return RBPOWER_TARIFF_STATUS_NO_PLAN;
    }
    if (!g_active_zone_cache.zone_cache_valid) {
        rbpower_tariff_update_zone_cache();
    }

    float buying  = g_active_zone_cache.current_buying_rate;
    float selling = g_active_zone_cache.current_selling_rate;
    result->active_zone_id      = g_active_zone_cache.current_zone_id;
    result->current_buying_rate = buying;
    result->current_selling_rate = selling;

    float hours      = period_duration_seconds / 3600.0f;
    float energy_kwh = power_kw * hours;
    bool  supplying  = (direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING);
    float rate       = supplying ? selling : buying;

    result->instant_cost_rate_per_hour = power_kw * rate;   // currency/hour
    result->instant_cost_for_period    = energy_kwh * rate; // currency over the period

    if (supplying) {
        result->period_totals.energy_supplied_kwh = energy_kwh;
        result->period_totals.revenue_supplied    = (double)energy_kwh * selling;
        result->period_totals.net_cost            = -result->period_totals.revenue_supplied;
    } else {
        result->period_totals.energy_consumed_kwh = energy_kwh;
        result->period_totals.cost_consumed       = (double)energy_kwh * buying;
        result->period_totals.net_cost            = result->period_totals.cost_consumed;
    }

    result->status = RBPOWER_TARIFF_STATUS_OK;
    return RBPOWER_TARIFF_STATUS_OK;
}

/* Instant tariff result for a unit from its latest 200 ms average. */
rbpower_err_t rbpower_tariff_get_instant_result(rbpower_unit_id_t unit_id,
                                               rbpower_tariff_instant_result_t* result)
{
    if (!result || unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!rbpower_tariff_system_is_ready()) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Check that the unit exists and is active
    rbpower_electrical_network_t* network;
    if (rbpower_get_network(&network) != RBPOWER_OK) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    if (!network->units[unit_id].has_valid_definition || 
        !network->units[unit_id].definition.is_active) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Current real-time data of the unit
    const rbpower_unit_realtime_average_200ms_t* realtime_200ms = 
        &network->units[unit_id].realtime_average_200ms;
    
    if (!realtime_200ms->is_valid) {
        return RBPOWER_ERR_INSUFFICIENT_DATA;
    }
    
    // Extract the inputs for the calculation
    float power_avg_kw = realtime_200ms->average_200ms.power_active_avg_w / 1000.0f;
    rbpower_current_direction_t direction = realtime_200ms->zc_analysis.current_direction;
    
    // Run the tariff calculation
    rbpower_tariff_status_t status = rbpower_tariff_calculate_instant_cost(
        unit_id,
        power_avg_kw,
        0.2f,  // 200 ms period
        direction,
        result
    );
    
    return (status == RBPOWER_TARIFF_STATUS_OK) ? RBPOWER_OK : RBPOWER_ERR_CALCULATION_FAILED;
}

/* Tariff result of a unit for a RAM period (3SEC/15MIN/1HOUR). */
rbpower_err_t rbpower_tariff_get_period_result(rbpower_unit_id_t unit_id,
                                              rbpower_stat_period_type_t period_type,
                                              rbpower_tariff_period_result_t* result)
{
    if (!result || unit_id >= RBPOWER_MAX_HOME_UNITS || period_type >= RBPOWER_RAM_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;   // economics live in the RAM accumulator (3 periods); day+ via flash
    }
    
    if (!rbpower_tariff_system_is_ready()) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Get the network and check the unit
    rbpower_electrical_network_t* network;
    if (rbpower_get_network(&network) != RBPOWER_OK) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    if (!network->units[unit_id].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Period statistics
    const rbpower_period_statistics_t* stats = 
        &network->units[unit_id].statistics.periods[period_type];
    
    if (!stats->period_complete) {
        return RBPOWER_ERR_INSUFFICIENT_DATA;
    }
    
    // Economics accumulator
    const rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period_type];
    
    if (!acc->is_enabled) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    // Clear the result
    memset(result, 0, sizeof(rbpower_tariff_period_result_t));
    
    // === BASIC INFORMATION ===
    result->unit_id = unit_id;
    result->period_type = period_type;
    result->period_start_timestamp = stats->period_start_us;
    result->period_end_timestamp = stats->period_end_us;
    result->period_duration_minutes = stats->period_duration_ms / 60000;
    
    // === TOTALS ===
    result->totals.total_cost = acc->economics.net_cost;
    result->totals.energy_cost = acc->economics.cost_consumed;
    result->totals.fixed_charges = 0.0; // Not implemented yet
    result->totals.taxes_and_fees = 0.0; // Not implemented yet

    result->totals.consumed_cost = acc->economics.cost_consumed;
    result->totals.supplied_revenue = acc->economics.revenue_supplied;
    result->totals.net_cost = acc->economics.net_cost;

    // Energy volumes
    result->totals.total_energy_consumed_kwh = (float)(acc->energy.consumed_wh_accumulator / 1000.0);
    result->totals.total_energy_supplied_kwh = (float)(acc->energy.supplied_wh_accumulator / 1000.0);
    result->totals.net_energy_kwh = result->totals.total_energy_consumed_kwh - 
                                   result->totals.total_energy_supplied_kwh;
    
    result->totals.avg_power_kw = stats->power_active_avg / 1000.0f;
    
    // === PER-ZONE COST BREAKDOWN (real distribution) ===
    // Built from the per-unit cost_distribution_by_zone[] accumulator that the cost path
    // populates per 3-sec sample (replaces the old fake that put 100% in the current zone).
    const float* zcost = g_unit_direct_costs[unit_id].device_analytics.cost_distribution_by_zone;
    double total_zone_cost = 0.0;
    for (uint8_t z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) total_zone_cost += zcost[z];

    uint8_t used = 0;
    for (uint8_t z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        if (zcost[z] <= 0.0f) continue;
        // Apportion the period's energy by the zone's cost share (energy isn't tracked
        // per-zone separately; cost share is the available proxy).
        float share = (total_zone_cost > 0.0) ? (float)(zcost[z] / total_zone_cost) : 0.0f;
        float zkwh  = result->totals.total_energy_consumed_kwh * share;
        result->zone_breakdown[used].zone_id = (rbpower_tariff_zone_type_t)z;
        strncpy(result->zone_breakdown[used].zone_name, rbpower_tariff_zone_type_text[z],
                sizeof(result->zone_breakdown[used].zone_name) - 1);
        result->zone_breakdown[used].kwh_consumed = zkwh;
        result->zone_breakdown[used].cost_in_zone = zcost[z];
        result->zone_breakdown[used].net_cost_in_zone = zcost[z];
        result->zone_breakdown[used].zone_utilization_percent = share * 100.0f;
        result->zone_breakdown[used].avg_rate_in_zone = (zkwh > 0.0001f)
                                   ? (float)(zcost[z] / zkwh) : 0.0f;
        used++;
    }
    result->zones_used_count = used;

    // Fallback: no per-zone data yet (e.g. just after a reset) → report the current zone.
    if (used == 0) {
        rbpower_tariff_zone_type_t current_zone;
        char zone_name[RBPOWER_TARIFF_NAME_LENGTH];
        float current_rate;
        if (rbpower_tariff_get_current_zone(&current_zone, zone_name, &current_rate) == RBPOWER_OK) {
            result->zone_breakdown[0].zone_id = current_zone;
            strncpy(result->zone_breakdown[0].zone_name, zone_name, sizeof(result->zone_breakdown[0].zone_name) - 1);
            result->zone_breakdown[0].cost_in_zone = result->totals.energy_cost;
            result->zone_breakdown[0].avg_rate_in_zone = current_rate;
            result->zone_breakdown[0].zone_utilization_percent = 100.0f;
            result->zones_used_count = 1;
        }
    }
    
    // === PROGRESSIVE PRICING ===
    result->progressive.progressive_applied = false; // Not reported per period yet
    
    // === FIXED PAYMENTS ===
    result->fixed_charges.total_fixed_charges = 0.0; // Not implemented yet
    
    // === NET METERING ===
    if (g_current_tariff_plan.net_metering_enabled) {
        result->net_metering.net_metering_applied = true;
        result->net_metering.net_metering_efficiency_percent = 
            g_current_tariff_plan.net_metering_default_ratio * 100.0f;
        // Other fields are not implemented yet
    }
    
    // === ANALYTICS / OPTIMIZATION (not implemented → 0, not a fabricated "average") ===
    result->optimization.potential_savings = 0.0;
    result->optimization.achieved_savings = 0.0;
    result->optimization.optimization_score_percent = 0.0f;
    
    // === METADATA ===
    result->metadata.total_calculations = acc->metadata.samples_count;
    result->metadata.successful_calculations = acc->metadata.valid_samples_count;
    result->metadata.data_completeness_percent = stats->data_completeness_percent;
    result->metadata.period_complete = stats->period_complete;
    
    return RBPOWER_OK;
}

/* Device session result (simplified: no full session tracking). */
rbpower_err_t rbpower_tariff_get_session_result(rbpower_unit_id_t unit_id,
                                               uint32_t session_id,
                                               rbpower_tariff_session_result_t* result)
{
    if (!result || unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!rbpower_tariff_system_is_ready()) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Simplified implementation without full session tracking
    
    memset(result, 0, sizeof(rbpower_tariff_session_result_t));
    
    result->unit_id = unit_id;
    result->session_id = session_id;
    result->device_profile = g_unit_tariff_configs[unit_id].device_profile;
    result->session_completed = false;
    
    // Basic data from the direct costs
    const rbpower_unit_direct_costs_t* unit_costs = &g_unit_direct_costs[unit_id];
    result->cost_breakdown.total_session_cost = unit_costs->current_session.current_session_cost;
    result->total_energy_consumed_kwh = unit_costs->current_session.session_energy_kwh;
    result->session_duration_minutes = unit_costs->current_session.session_duration_seconds / 60;
    
    if (result->session_duration_minutes > 0) {
        result->cost_breakdown.cost_per_hour = 
            (float)(result->cost_breakdown.total_session_cost * 60.0 / result->session_duration_minutes);
        result->cost_breakdown.cost_per_minute = 
            (float)(result->cost_breakdown.total_session_cost / result->session_duration_minutes);
    }
    
    if (result->total_energy_consumed_kwh > 0.0f) {
        result->cost_breakdown.cost_per_kwh = 
            (float)(result->cost_breakdown.total_session_cost / result->total_energy_consumed_kwh);
    }
    
    // Session optimization
    result->session_optimization.was_optimal_timing = false; // Not implemented yet
    result->session_optimization.suggestions_count = 0;
    
    // Comparative analysis
    result->comparative_analysis.avg_cost_last_10_sessions =
        unit_costs->lifetime_totals.cost_per_session_avg;
    result->comparative_analysis.efficiency_score_percent = 0.0f; // efficiency scoring not implemented
    
    return RBPOWER_OK;
}

/* System tariff summary report for a RAM period. */
rbpower_err_t rbpower_tariff_get_summary_report(rbpower_stat_period_type_t period_type,
                                               rbpower_tariff_summary_report_t* report)
{
    if (!report || period_type >= RBPOWER_RAM_PERIOD_COUNT) {   // accumulator-backed report → RAM periods only
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!rbpower_tariff_system_is_ready()) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    rbpower_electrical_network_t* network;
    if (rbpower_get_network(&network) != RBPOWER_OK) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    // Clear the report
    memset(report, 0, sizeof(rbpower_tariff_summary_report_t));
    
    // === GENERAL INFORMATION ===
    report->report_period = period_type;
    report->report_generated_timestamp = rbtime_get_unix_timestamp();
    
    // === AGGREGATE ACROSS ALL UNITS ===
    uint8_t active_units = 0;
    double total_system_cost = 0.0;
    double total_energy_cost = 0.0;
    float total_energy_consumed = 0.0f;
    float total_energy_supplied = 0.0f;
    uint64_t total_samples = 0;        // real calculation count across units
    uint32_t completeness_sum = 0;     // for the real average completeness

    for (uint8_t unit_id = 0; unit_id < network->units_count; unit_id++) {
        if (!network->units[unit_id].has_valid_definition || 
            !g_unit_tariff_configs[unit_id].tariff_tracking_enabled) {
            continue;
        }
        
        const rbpower_period_accumulator_t* acc = &g_period_accumulators[unit_id][period_type];
        
        if (!acc->is_enabled) {
            continue;
        }
        
        // Aggregate the totals
        total_system_cost += acc->economics.net_cost;
        total_energy_cost += acc->economics.cost_consumed;
        total_energy_consumed += (float)(acc->energy.consumed_wh_accumulator / 1000.0);   /* kWh */
        total_energy_supplied += (float)(acc->energy.supplied_wh_accumulator / 1000.0);   /* kWh */
        total_samples += acc->metadata.samples_count;
        completeness_sum += (acc->metadata.input_periods_expected > 0)
            ? (acc->metadata.input_periods_received * 100u / acc->metadata.input_periods_expected)
            : 0u;
        
        // Per-unit breakdown
        if (active_units < RBPOWER_MAX_HOME_UNITS) {
            report->units_breakdown[active_units].unit_id = unit_id;
            strncpy(report->units_breakdown[active_units].unit_name, 
                   network->units[unit_id].definition.name,
                   sizeof(report->units_breakdown[active_units].unit_name) - 1);
            report->units_breakdown[active_units].device_profile = 
                g_unit_tariff_configs[unit_id].device_profile;
            report->units_breakdown[active_units].unit_total_cost = acc->economics.net_cost;
            report->units_breakdown[active_units].unit_energy_kwh =
                (float)(acc->energy.consumed_wh_accumulator / 1000.0);
            // unit_efficiency_score: optimization analytics not implemented → left 0 (memset),
            // not a fabricated "average". Compute when the optimizer exists.
        }
        
        active_units++;
    }
    
    // === SYSTEM TOTALS ===
    report->system_totals.active_units_count = active_units;
    report->system_totals.total_system_cost = total_system_cost;
    report->system_totals.total_energy_cost = total_energy_cost;
    report->system_totals.total_energy_consumed_kwh = total_energy_consumed;
    report->system_totals.total_energy_supplied_kwh = total_energy_supplied;
    report->system_totals.net_energy_balance_kwh = total_energy_consumed - total_energy_supplied;
    
    float total_energy_kwh = total_energy_consumed + total_energy_supplied;
    if (total_energy_kwh > 0.0f) {
        report->system_totals.avg_system_cost_per_kwh = (float)(total_system_cost / total_energy_kwh);
    }
    
    // Per-unit shares
    for (uint8_t i = 0; i < active_units && i < RBPOWER_MAX_HOME_UNITS; i++) {
        if (total_system_cost > 0.0) {
            report->units_breakdown[i].unit_cost_share_percent = 
                (float)(report->units_breakdown[i].unit_total_cost / total_system_cost * 100.0);
        }
        if (total_energy_kwh > 0.0f) {
            report->units_breakdown[i].unit_energy_share_percent = 
                report->units_breakdown[i].unit_energy_kwh / total_energy_kwh * 100.0f;
        }
    }
    
    report->units_count = active_units;
    
    // === TARIFF ZONES ===
    // Simplified: only the current zone
    rbpower_tariff_zone_type_t current_zone;
    char zone_name[RBPOWER_TARIFF_NAME_LENGTH];
    float current_rate;
    
    if (rbpower_tariff_get_current_zone(&current_zone, zone_name, &current_rate) == RBPOWER_OK) {
        report->zones_summary[0].zone_id = current_zone;
        strncpy(report->zones_summary[0].zone_name, zone_name, 
               sizeof(report->zones_summary[0].zone_name) - 1);
        report->zones_summary[0].total_kwh_in_zone = total_energy_kwh;
        report->zones_summary[0].total_cost_in_zone = total_system_cost;
        report->zones_summary[0].zone_cost_share_percent = 100.0f;
        report->zones_summary[0].zone_energy_share_percent = 100.0f;
        report->zones_summary[0].zone_utilization_percent = 100.0f;
        report->zones_used_count = 1;
    }
    
    // === OPTIMIZATION / RECOMMENDATIONS ===
    // Optimizer not implemented → no fabricated score or made-up "15% savings" recommendation.
    // Fields stay 0 (memset); recommendations_count = 0. Populate when the optimizer exists.
    report->optimization_summary.total_potential_savings = 0.0;
    report->optimization_summary.achieved_savings = 0.0;
    report->optimization_summary.system_optimization_score = 0.0f;
    report->optimization_summary.recommendations_count = 0;

    // === TREND ANALYSIS ===
    // Trends not implemented → 0 (not fabricated). Simple flat projection from this period.
    report->trend_analysis.cost_trend_percent = 0.0f;
    report->trend_analysis.energy_trend_percent = 0.0f;
    report->trend_analysis.efficiency_trend_percent = 0.0f;
    report->trend_analysis.projected_next_period_cost = total_system_cost;
    report->trend_analysis.projected_next_period_energy = total_energy_kwh;
    report->trend_analysis.forecast_confidence_percent = 0; // no trend model yet

    // === REPORT QUALITY (real, measured) ===
    report->quality_metrics.data_completeness_percent =
        (active_units > 0) ? (uint8_t)(completeness_sum / active_units) : 0;
    report->quality_metrics.total_calculations_performed = (uint32_t)total_samples;
    report->quality_metrics.calculation_errors_count = 0;
    report->quality_metrics.report_reliability_high = (report->quality_metrics.data_completeness_percent > 80);
    report->quality_metrics.report_generation_time_ms = 10; // Nominal value, not measured
    strncpy(report->quality_metrics.report_version, "v1.0", 
           sizeof(report->quality_metrics.report_version) - 1);
    
    return RBPOWER_OK;
}

/* Hourly tariff forecast at the unit's current power (flat power, zone-dependent rate). */
rbpower_err_t rbpower_tariff_get_forecast(rbpower_unit_id_t unit_id,
                                         uint32_t forecast_hours,
                                         rbpower_tariff_instant_result_t* forecast_results,
                                         uint8_t* results_count)
{
    if (!forecast_results || !results_count || unit_id >= RBPOWER_MAX_HOME_UNITS) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    if (!rbpower_tariff_system_is_ready()) {
        return RBPOWER_ERR_NOT_INIT;
    }
    
    // Simplified forecast: no history-based prediction
    
    uint8_t max_results = (forecast_hours > 24) ? 24 : forecast_hours; // At most 24 hours
    *results_count = 0;
    
    // Current data of the unit
    rbpower_electrical_network_t* network;
    if (rbpower_get_network(&network) != RBPOWER_OK) {
        return RBPOWER_ERR_INVALID_STATE;
    }
    
    if (!network->units[unit_id].has_valid_definition) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    // Simple forecast from the current data
    const rbpower_unit_realtime_average_200ms_t* current_data = 
        &network->units[unit_id].realtime_average_200ms;
    
    if (!current_data->is_valid) {
        return RBPOWER_ERR_INSUFFICIENT_DATA;
    }
    
    float base_power_kw = current_data->average_200ms.power_active_avg_w / 1000.0f;
    rbpower_current_direction_t base_direction = current_data->zc_analysis.current_direction;
    bool supplying = (base_direction == RBPOWER_CURRENT_DIRECTION_SUPPLYING);

    // Honest TOU forecast: hold power FLAT at the current level ("if consumption continues")
    // and vary the RATE by the tariff zone active in each future hour (from the real
    // schedule) — replaces the previous rand()-based fabricated power variation.
    uint64_t now_unix = rbtime_get_unix_timestamp();
    for (uint8_t hour = 0; hour < max_results; hour++) {
        time_t future = (time_t)(now_unix + (uint64_t)hour * 3600);
        struct tm ftm;
        localtime_r(&future, &ftm);
        rbpower_tariff_zone_t* zone = find_active_tariff_zone(&ftm);

        rbpower_tariff_instant_result_t* fr = &forecast_results[hour];
        memset(fr, 0, sizeof(*fr));
        fr->unit_id = unit_id;
        fr->calculation_timestamp = (uint64_t)future;
        fr->instant_power_kw = base_power_kw;
        fr->power_direction = base_direction;

        float buying  = zone ? zone->rate_buying_per_kwh  : 0.0f;
        float selling = zone ? zone->rate_selling_per_kwh : 0.0f;
        float rate    = supplying ? selling : buying;
        fr->active_zone_id       = zone ? zone->zone_id : RBPOWER_TARIFF_ZONE_DAY;
        fr->current_buying_rate  = buying;
        fr->current_selling_rate = selling;
        fr->instant_cost_rate_per_hour = base_power_kw * rate;        // currency/hour
        fr->instant_cost_for_period    = base_power_kw * rate;        // 1-hour horizon → same
        if (supplying) {
            fr->period_totals.energy_supplied_kwh = base_power_kw;
            fr->period_totals.revenue_supplied    = (double)base_power_kw * selling;
            fr->period_totals.net_cost            = -fr->period_totals.revenue_supplied;
        } else {
            fr->period_totals.energy_consumed_kwh = base_power_kw;
            fr->period_totals.cost_consumed       = (double)base_power_kw * buying;
            fr->period_totals.net_cost            = fr->period_totals.cost_consumed;
        }
        fr->status = RBPOWER_TARIFF_STATUS_OK;
        (*results_count)++;
    }
    
    return (*results_count > 0) ? RBPOWER_OK : RBPOWER_ERR_CALCULATION_FAILED;
}


/* Tariff zone switching test: polls the current zone 10 times at 1 s intervals (blocks ~10 s). */
void rbpower_tariff_test_zone_switching(void)
{
    ESP_LOGI("RBHOME_TARIFF", "");
    ESP_LOGI("RBHOME_TARIFF", "=== TARIFF ZONE SWITCHING TEST ===");

    rbpower_tariff_zone_type_t prev_zone = RBPOWER_TARIFF_ZONE_COUNT;
    char prev_zone_name[RBPOWER_TARIFF_NAME_LENGTH] = "";
    // float prev_rate = 0.0f;

    // Check zone 10 times with 1 second interval
    for (uint8_t i = 0; i < 10; i++) {
        rbpower_tariff_zone_type_t current_zone;
        char zone_name[RBPOWER_TARIFF_NAME_LENGTH];
        float current_rate;

        if (rbpower_tariff_get_current_zone(&current_zone, zone_name, &current_rate) == RBPOWER_OK) {

            if (i == 0 || current_zone != prev_zone) {
                ESP_LOGI("RBHOME_TARIFF", "Iteration %d: Zone %s (%.4f EUR/kWh)",
                         i, zone_name, current_rate);

                if (i > 0) {
                    ESP_LOGI("RBHOME_TARIFF", "  -> SWITCH: %s -> %s", prev_zone_name, zone_name);
                }

                prev_zone = current_zone;
                strncpy(prev_zone_name, zone_name, sizeof(prev_zone_name) - 1);
                // prev_rate = current_rate;
            }
        } else {
            ESP_LOGI("RBHOME_TARIFF", "Iteration %d: ERROR getting zone", i);
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // 1 second
    }

    ESP_LOGI("RBHOME_TARIFF", "=== END OF SWITCHING TEST ===");
}


/* Total system cost for a RAM period, taken from the Main Supply unit. */
rbpower_err_t rbpower_tariff_get_system_total_cost(rbpower_stat_period_type_t period_type,
                                                   double* total_cost,
                                                   float* total_energy_kwh,
                                                   rbpower_unit_id_t* main_supply_unit_id)
{
    if (!total_cost || !total_energy_kwh || period_type >= RBPOWER_RAM_PERIOD_COUNT) {
        return RBPOWER_ERR_INVALID_ARG;   // economics from RAM accumulator (3 periods); day+ has none
    }

    if (!rbpower_tariff_system_is_ready()) {
        return RBPOWER_ERR_NOT_INIT;
    }

    rbpower_electrical_network_t* network;
    if (rbpower_get_network(&network) != RBPOWER_OK) {
        return RBPOWER_ERR_INVALID_STATE;
    }

    // Find Main Supply unit
    rbpower_unit_id_t main_supply_id = RBPOWER_HOME_INVALID_ID;
    for (uint8_t i = 0; i < network->units_count; i++) {
        if (network->units[i].has_valid_definition &&
            network->units[i].definition.role == RBPOWER_ROLE_MAIN_SUPPLY &&
            network->units[i].definition.type == RBPOWER_METRIC_CURRENT_LOAD &&
            g_unit_tariff_configs[i].tariff_tracking_enabled) {
            main_supply_id = i;
            break;
        }
    }

    if (main_supply_id == RBPOWER_HOME_INVALID_ID) {
        ESP_LOGD("RBHOME_TARIFF", "Main Supply unit with tariffing not found");
        return RBPOWER_ERR_NOT_CONFIGURED;
    }

    // Get accumulator data
    const rbpower_period_accumulator_t* acc = &g_period_accumulators[main_supply_id][period_type];

    if (!acc->is_enabled) {
        return RBPOWER_ERR_INVALID_STATE;
    }

    // Return results
    *total_cost = acc->economics.net_cost;
    *total_energy_kwh = (float)((acc->energy.consumed_wh_accumulator - acc->energy.supplied_wh_accumulator) / 1000.0);

    if (main_supply_unit_id) {
        *main_supply_unit_id = main_supply_id;
    }

    ESP_LOGD("RBHOME_TARIFF", "Total system cost: %.4f EUR, %.3f kWh (Unit[%d])",
             *total_cost, *total_energy_kwh, main_supply_id);
    
    return RBPOWER_OK;
}

/* Detailed system cost breakdown for a RAM period (Main Supply unit). */
rbpower_err_t rbpower_tariff_get_system_cost_breakdown(rbpower_stat_period_type_t period_type,
                                                      rbpower_tariff_system_cost_breakdown_t* breakdown)
{
    if (!breakdown) {
        return RBPOWER_ERR_INVALID_ARG;
    }
    
    memset(breakdown, 0, sizeof(rbpower_tariff_system_cost_breakdown_t));
    
    // Total cost
    double total_cost;
    float total_energy_kwh;
    rbpower_unit_id_t main_supply_id;
    
    rbpower_err_t err = rbpower_tariff_get_system_total_cost(period_type, &total_cost, 
                                                           &total_energy_kwh, &main_supply_id);
    if (err != RBPOWER_OK) {
        return err;
    }
    
    // Fill the breakdown
    breakdown->period_type = period_type;
    breakdown->main_supply_unit_id = main_supply_id;
    breakdown->total_system_cost = total_cost;
    breakdown->total_energy_kwh = total_energy_kwh;
    breakdown->timestamp = rbtime_get_unix_timestamp();
    
    // Accumulator data for the details
    const rbpower_period_accumulator_t* acc = &g_period_accumulators[main_supply_id][period_type];
    
    breakdown->consumed_cost = acc->economics.cost_consumed;
    breakdown->supplied_revenue = acc->economics.revenue_supplied;
    breakdown->consumed_energy_kwh = (float)(acc->energy.consumed_wh_accumulator / 1000.0);
    breakdown->supplied_energy_kwh = (float)(acc->energy.supplied_wh_accumulator / 1000.0);
    breakdown->net_energy_kwh = breakdown->consumed_energy_kwh - breakdown->supplied_energy_kwh;
  
    return RBPOWER_OK;
}

/* Log the system costs for the 3sec/15min/1hour/1day periods. */
void rbpower_tariff_print_system_costs(void)
{
    ESP_LOGI("RBHOME_TARIFF", "");
    ESP_LOGI("RBHOME_TARIFF", "=== SYSTEM COSTS ===");

    rbpower_tariff_system_cost_breakdown_t breakdown;

    // Check different periods
    rbpower_stat_period_type_t periods[] = {
        RBPOWER_STAT_PERIOD_3SEC,
        RBPOWER_STAT_PERIOD_15MIN,
        RBPOWER_STAT_PERIOD_1HOUR,
        RBPOWER_STAT_PERIOD_DAILY
    };

    const char* period_names[] = {"3sec", "15min", "1hour", "1day"};

    for (uint8_t i = 0; i < sizeof(periods)/sizeof(periods[0]); i++) {
        if (rbpower_tariff_get_system_cost_breakdown(periods[i], &breakdown) == RBPOWER_OK) {
            ESP_LOGI("RBHOME_TARIFF", "%s period (Unit[%d]):",
                     period_names[i], breakdown.main_supply_unit_id);
            const char* cur = rbpower_tariff_currency();
            ESP_LOGI("RBHOME_TARIFF", "  Total cost: %.4f %s", breakdown.total_system_cost, cur);
            ESP_LOGI("RBHOME_TARIFF", "  Energy: %.3f kWh", breakdown.total_energy_kwh);
            ESP_LOGI("RBHOME_TARIFF", "  Consumption: %.4f %s (%.3f kWh)",
                     breakdown.consumed_cost, cur, breakdown.consumed_energy_kwh);
            ESP_LOGI("RBHOME_TARIFF", "  Supply: %.4f %s (%.3f kWh)",
                     breakdown.supplied_revenue, cur, breakdown.supplied_energy_kwh);
            ESP_LOGI("RBHOME_TARIFF", "  Avg cost: %.4f %s/kWh", breakdown.avg_cost_per_kwh, cur);
        }
    }

    ESP_LOGI("RBHOME_TARIFF", "======================");
}