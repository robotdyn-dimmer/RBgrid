/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_core_reads.c
 * @brief Read-side JSON builders shared by every transport: economics, summary, schema, the night
 *        baseline (base_w), and the valid-record iterator over the period history.
 *
 * Moved out of rbwebserver_api.c (HTTP handlers there are thin wrappers over these).
 */
#include "rbgrid_core.h"
#include "rbgrid_core_priv.h"
#include "rbstats_migrate.h"
#include "rbhome_types.h"
#include "rbgrid_config_storage.h"
#include "rbgrid_config_api.h"
#include "rbhome_tariff.h"
#include "rbjson.h"
#include "littlefs_raw.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rbstrutil.h"
#include "rbamp_sensor_models.h"


/* JSON string body escape for hand-built responses (" \ and control characters) */
void rbgrid_json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)(in ? in : ""); *c && o + 7 < n; ++c) {
        if (*c == '"' || *c == '\\') { out[o++] = '\\'; out[o++] = (char)*c; }
        else if (*c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", *c);
        else out[o++] = (char)*c;
    }
    out[o] = '\0';
}

/* map "hour".."year" → rbpower_stat_period_type_t (the period rgrid_get_period_history wants). -1 = bad. */
int rbgrid_period_str_to_stat(const char *s)
{
    if (!strcmp(s, "hour"))  return RBPOWER_STAT_PERIOD_1HOUR;
    if (!strcmp(s, "day"))   return RBPOWER_STAT_PERIOD_DAILY;
    if (!strcmp(s, "week"))  return RBPOWER_STAT_PERIOD_WEEKLY;
    if (!strcmp(s, "month")) return RBPOWER_STAT_PERIOD_MONTHLY;
    if (!strcmp(s, "year"))  return RBPOWER_STAT_PERIOD_YEARLY;
    return -1;
}
/* map "hour".."year" → rbpower_period_t (flash period, for the valid-record-count query). -1 = bad. */
int rbgrid_period_str_to_flash(const char *s)
{
    if (!strcmp(s, "hour"))  return RBPOWER_PERIOD_HOUR;
    if (!strcmp(s, "day"))   return RBPOWER_PERIOD_DAY;
    if (!strcmp(s, "week"))  return RBPOWER_PERIOD_WEEK;
    if (!strcmp(s, "month")) return RBPOWER_PERIOD_MONTH;
    if (!strcmp(s, "year"))  return RBPOWER_PERIOD_YEAR;
    return -1;
}

/* Iterate VALID period records (skipping empty 0xFFFFFFFF flash slots), optionally windowed by unix
 * from/to, invoking cb per record. Memory-safe (reads in small batches). Bounded by the VALID record
 * count (rbgrid_history_get_buffer_info) so it never decodes the unwritten 0xFF ring tail — the bug the
 * raw rbpower_flash_read_records would cause (it returns physical slots incl. empties). Returns valid
 * record count visited (pre-window). Shared by export.csv + summary. */
#define RBAPI_REC_BATCH 20            ///< Records read from flash per batch
#define RBAPI_TS_EMPTY  0xFFFFFFFFu   ///< Timestamp of an unwritten (erased) flash slot
uint32_t rbgrid_foreach_valid_record(rbpower_unit_id_t unit, int stat_period, int flash_period,
                                           uint32_t from, uint32_t to,
                                           void (*cb)(const rbpower_period_statistics_t *, void *), void *ctx)
{
    uint32_t valid_total = 0;
    rbgrid_history_get_buffer_info(unit, (rbpower_period_t)flash_period, RBPOWER_DATA_TYPE_ELECTRICAL, &valid_total);
    if (valid_total == 0) return 0;

    rbpower_period_statistics_t *st = malloc(RBAPI_REC_BATCH * sizeof(rbpower_period_statistics_t));
    if (!st) return 0;

    uint32_t emitted = 0, scanned = 0, idx = 0;
    uint32_t last_ts = UINT32_MAX;                  // rows come newest-first: emit strictly decreasing ts only
    const uint32_t scan_cap = valid_total + 4096;   // safety: never loop past the physical ring
    while (emitted < valid_total && scanned < scan_cap) {
        uint32_t got = 0;
        if (rbgrid_get_period_history(unit, (rbpower_stat_period_type_t)stat_period, idx, RBAPI_REC_BATCH, st, &got) != RBGRID_OK)
            break;
        if (got == 0) break;
        for (uint32_t i = 0; i < got && emitted < valid_total; i++) {
            scanned++;
            uint32_t ts = (uint32_t)(st[i].period_start_us / 1000000LL);
            if (ts == 0 || ts == RBAPI_TS_EMPTY) continue;   // unwritten/empty slot — skip (the bug fix)
            if (ts < RBGRID_TS_SANE_MIN) continue;            // garbage: fake-clock / 2024-fallback records
            /* offset paging: a record written between two batches shifts every offset by one, so the next batch
             * repeats the last row — CSV/summary counted that period twice */
            if (ts >= last_ts) continue;
            last_ts = ts;
            emitted++;
            if (ts < from || ts > to) continue;              // outside the requested window
            cb(&st[i], ctx);
        }
        if (got < RBAPI_REC_BATCH) break;                    // physical ring exhausted
        idx += got;
        /* a background (priority-1) caller would otherwise run this flash scan back to back and starve
         * the IDLE task of its core → task watchdog (observed during an OTA upload: starved IDLE0) */
        if (uxTaskPriorityGet(NULL) < 5) vTaskDelay(1);
    }
    free(st);
    return emitted;
}

/** @brief Summary accumulator (callback context for rbgrid_foreach_valid_record). */
struct rbapi_sum_ctx {
    double e_in  /**< Imported (consumed) energy, Wh */,
           e_out /**< Exported (supplied) energy, Wh */,
           cost  /**< Cost of consumed energy, major currency units */,
           rev;  ///< Revenue for supplied energy, major currency units
    uint32_t records;   ///< Number of records summed
};
static void rbapi_sum_emit(const rbpower_period_statistics_t *s, void *vctx)
{
    struct rbapi_sum_ctx *c = (struct rbapi_sum_ctx *)vctx;
    c->e_in += s->energy_period.consumed_wh;
    c->e_out += s->energy_period.supplied_wh;
    c->cost += s->result_base.cost_consumed;
    c->rev += s->result_base.revenue_supplied;
    c->records++;
}


/* "Home vampires" baseline (standby load): median of the hourly average power of the
 * night hours 01:00–05:00 local (record midpoint) over the last 7 days, full hours only (dur ≥ 3540 s). One
 * value for the app and HA. Recomputed at most once an hour (flash scan of ≤168 HOUR records). */
/** @brief Collector for the night-baseline scan (callback context for rbgrid_foreach_valid_record). */
struct rbapi_base_ctx {
    float v[40];            ///< Hourly average powers of the qualifying night hours, W
    int n;                  ///< Number of values in @c v
    uint8_t nights_seen[8]; ///< Day-of-year (low byte) of each distinct night seen
    int nights;             ///< Number of distinct nights
};
static void rbapi_base_emit(const rbpower_period_statistics_t *s, void *vctx)
{
    struct rbapi_base_ctx *c = (struct rbapi_base_ctx *)vctx;
    uint32_t ts = (uint32_t)(s->period_start_us / 1000000LL), dur = s->v4_dur_s ? s->v4_dur_s : 3600;
    if (dur < 3540 || c->n >= 40) return;
    time_t mid = (time_t)(ts - dur / 2); struct tm lt; localtime_r(&mid, &lt);
    if (lt.tm_hour < 1 || lt.tm_hour >= 5) return;
    c->v[c->n++] = (float)(s->energy_period.consumed_wh * 3600.0 / dur);
    uint8_t day = (uint8_t)lt.tm_yday;   /* distinct nights (yday is enough inside a 7-day window) */
    for (int i = 0; i < c->nights && i < 8; ++i) if (c->nights_seen[i] == day) return;
    if (c->nights < 8) c->nights_seen[c->nights++] = day;
}
static int rbapi_cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
/* The scan walks every valid HOUR record of the unit (whole history, then windows to 7 days): 13–18 s of
 * flash reads on a hub with a long history. It used to run INLINE in the telemetry builder, i.e. under the
 * home lock — the 3-s stats chain and every HTTP request stalled for that long, every 2 min while the result
 * was empty (period events paused 10–13 s; a telemetry GET took 18 s).
 * Now a low-priority task owns the scan; the telemetry path only reads the cached result. */
static volatile float s_bw_w = NAN;
static volatile int s_bw_h = 0, s_bw_n = 0;
static volatile rbpower_unit_id_t s_bw_unit = 0, s_bw_done_unit = 0xFFFF;
static TaskHandle_t s_bw_task = NULL;
static void rbapi_base_w_task(void *arg)
{
    (void)arg;
    for (;;) {
        rbpower_unit_id_t unit = s_bw_unit;
        time_t now = time(NULL);
        if (unit == 0 || now < 1600000000) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        struct rbapi_base_ctx *c = calloc(1, sizeof(*c));
        if (!c) { vTaskDelay(pdMS_TO_TICKS(60000)); continue; }
        int64_t t0 = esp_timer_get_time();
        uint32_t seen = rbgrid_foreach_valid_record(unit, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR,
                                                   (uint32_t)now - 7u * 86400u, (uint32_t)now, rbapi_base_emit, c);
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        if (ms > 2000) rbpower_diag_printf("base_w scan %lu records %lldms (background)", (unsigned long)seen, (long long)ms);
        float w = NAN;
        if (c->n > 0) { qsort(c->v, c->n, sizeof(float), rbapi_cmp_f);
                        w = (c->n & 1) ? c->v[c->n / 2] : 0.5f * (c->v[c->n / 2 - 1] + c->v[c->n / 2]); }
        s_bw_w = w; s_bw_h = c->n; s_bw_n = c->nights; s_bw_done_unit = unit;
        int h = c->n;
        free(c);
        /* An EMPTY result (right after boot the stats store / clock may not be ready yet) is retried after
         * 2 min; a non-empty result is refreshed hourly (otherwise base_w stayed null for an hour after boot). */
        vTaskDelay(pdMS_TO_TICKS(h > 0 ? 3600000 : 120000));
    }
}
/**
 * @brief Night-time standby baseline ("home vampires") of a unit, from the cached background scan.
 *
 * Non-blocking: the first call starts a low-priority task that scans the HOUR records; until it has a
 * result for @p unit the outputs are NAN / 0. Called from the telemetry builder.
 *
 * @param[in]  unit    Unit id to compute the baseline for (becomes the scan target).
 * @param[out] base_w  Median night-hour average power, W (NAN when not available; may be NULL).
 * @param[out] hours   Number of night hours used (may be NULL).
 * @param[out] nights  Number of distinct nights used (may be NULL).
 * @return true if a non-empty result for @p unit is available.
 */
bool rbgrid_base_w(rbpower_unit_id_t unit, float *base_w, int *hours, int *nights)
{
    s_bw_unit = unit;
    if (!s_bw_task) xTaskCreate(rbapi_base_w_task, "base_w", 4096, NULL, 1, &s_bw_task);
    bool mine = (s_bw_done_unit == unit);
    if (base_w) *base_w = mine ? s_bw_w : NAN;
    if (hours)  *hours = mine ? s_bw_h : 0;
    if (nights) *nights = mine ? s_bw_n : 0;
    return mine && s_bw_h > 0;
}

/* GET /api/v1/economics?unit=<hex> — the tariff/economics state that is otherwise only visible through
 * console diagnostics (zonestats/netmeter/progtier): per-zone cost+revenue, net-metering credit
 * bank, progressive-tier state. Reads the already-computed getters in rbhome_tariff.c (no new compute).
 * Tier-2 composite. See documents/development/API_EVOLUTION_GUIDE.md. */
/**
 * @brief Build the /api/v1/economics JSON for a unit.
 *
 * Shared by the HTTP handler and the MQTT `cmd get economics` path. Reads the tariff getters
 * (read-only); call from task context.
 *
 * @param[in] unit  Unit id.
 * @return malloc'd JSON string (caller frees), or NULL on OOM.
 */
char *rbgrid_build_economics_json(rbpower_unit_id_t unit)
{
    // Read the computed economics state (all getters are read-only, NULL-safe).
    float cost_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0}, rev_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0};
    rbpower_tariff_get_zone_stats(unit, cost_zone, rev_zone);
    double nm_in = 0, nm_out = 0, nm_bal = 0, nm_earn = 0, nm_used = 0, nm_bill = 0;
    rbpower_tariff_get_net_metering(unit, &nm_in, &nm_out, &nm_bal, &nm_earn, &nm_used, &nm_bill);
    bool pg_en = false; double pg_cum = 0; int pg_tier = 0; float pg_mult = 1.0f;
    rbpower_tariff_get_progressive(unit, &pg_en, &pg_cum, &pg_tier, &pg_mult);

    static const char *ZNAME[RBPOWER_TARIFF_ZONE_COUNT] = {
        "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak"
    };

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "v", 1);
    char uidhex[8]; snprintf(uidhex, sizeof(uidhex), "%04X", (unsigned)unit);
    cJSON_AddStringToObject(root, "unit", uidhex);
    cJSON_AddStringToObject(root, "currency", rbpower_tariff_currency());

    cJSON *zones = cJSON_AddArrayToObject(root, "zones");
    for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        cJSON *zo = cJSON_CreateObject();
        cJSON_AddStringToObject(zo, "zone", ZNAME[z]);
        cJSON_AddNumberToObject(zo, "cost", round(cost_zone[z] * 10000) / 10000);
        cJSON_AddNumberToObject(zo, "revenue", round(rev_zone[z] * 10000) / 10000);
        cJSON_AddItemToArray(zones, zo);
    }
    cJSON *nm = cJSON_AddObjectToObject(root, "net_metering");
    cJSON_AddNumberToObject(nm, "period_import_kwh", round(nm_in * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "period_export_kwh", round(nm_out * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "credit_balance_kwh", round(nm_bal * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "lifetime_earned_kwh", round(nm_earn * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "lifetime_used_kwh", round(nm_used * 10000) / 10000);
    cJSON_AddNumberToObject(nm, "last_billable_net_kwh", round(nm_bill * 10000) / 10000);
    cJSON *pg = cJSON_AddObjectToObject(root, "progressive");
    cJSON_AddBoolToObject(pg, "enabled", pg_en);
    cJSON_AddNumberToObject(pg, "cumulative_import_kwh", round(pg_cum * 10000) / 10000);
    cJSON_AddNumberToObject(pg, "tier", pg_tier);
    cJSON_AddNumberToObject(pg, "rate_mult", round(pg_mult * 100) / 100);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* GET /api/v1/summary?unit=<hex>&period=hour&from=&to= — the "real-bill" roll-up:
 * range totals (energy in/out/net + cost/revenue/net summed over the period records in the window) +
 * per-zone cost/revenue (current cumulative, from the tariff getter). Tier-2 composite (additive). */
/**
 * @brief Build the /api/v1/summary JSON (range totals + per-zone cost/revenue).
 *
 * Shared by the HTTP handler and the MQTT `cmd get summary` path. Scans flash records; call from
 * task context.
 *
 * @param[in] unit        Unit id.
 * @param[in] sp          Stat-period enum (from rbgrid_period_str_to_stat).
 * @param[in] period_str  Period name ("hour".."year"); echoed and mapped to the flash period.
 * @param[in] from        Window start, epoch s (inclusive).
 * @param[in] to          Window end, epoch s (inclusive).
 * @return malloc'd JSON string (caller frees), or NULL on OOM.
 */
char *rbgrid_build_summary_json(rbpower_unit_id_t unit, int sp, const char *period_str, uint32_t from, uint32_t to)
{
    // Range totals: sum the VALID period records in [from,to] (bounded + skips empties — same helper as CSV).
    struct rbapi_sum_ctx sc = {0};
    rbgrid_foreach_valid_record(unit, sp, rbgrid_period_str_to_flash(period_str), from, to, rbapi_sum_emit, &sc);

    float cost_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0}, rev_zone[RBPOWER_TARIFF_ZONE_COUNT] = {0};
    rbpower_tariff_get_zone_stats(unit, cost_zone, rev_zone);
    static const char *ZN[RBPOWER_TARIFF_ZONE_COUNT] = {
        "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak" };

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "v", 1);
    char uidhex[8]; snprintf(uidhex, sizeof(uidhex), "%04X", (unsigned)unit);
    cJSON_AddStringToObject(root, "unit", uidhex);
    cJSON_AddStringToObject(root, "period", period_str);
    cJSON_AddStringToObject(root, "currency", rbpower_tariff_currency());
    cJSON *tot = cJSON_AddObjectToObject(root, "totals");
    cJSON_AddNumberToObject(tot, "records", sc.records);
    cJSON_AddNumberToObject(tot, "energy_in_kwh", round(sc.e_in / 1000.0 * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "energy_out_kwh", round(sc.e_out / 1000.0 * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "net_kwh", round((sc.e_in - sc.e_out) / 1000.0 * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "cost", round(sc.cost * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "revenue", round(sc.rev * 10000) / 10000);
    cJSON_AddNumberToObject(tot, "net_cost", round((sc.cost - sc.rev) * 10000) / 10000);
    // by-zone = current cumulative tariff-zone distribution (NOT windowed by from/to).
    cJSON *bz = cJSON_AddArrayToObject(root, "by_zone");
    for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        cJSON *zo = cJSON_CreateObject();
        cJSON_AddStringToObject(zo, "zone", ZN[z]);
        cJSON_AddNumberToObject(zo, "cost", round(cost_zone[z] * 10000) / 10000);
        cJSON_AddNumberToObject(zo, "revenue", round(rev_zone[z] * 10000) / 10000);
        cJSON_AddItemToArray(bz, zo);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* GET /api/v1/schema: machine-readable contract descriptor. Lists the API
 * version + the endpoint surface (path, methods, tier, params/keys) + the on-flash/telemetry schema versions.
 * This is the drift baseline: a client (or a contract test) fetches /schema + each endpoint and
 * diffs against a recorded golden. Tier 1 = 1:1 struct mirror (immutable); composite = aggregated.
 * See documents/development/API_EVOLUTION_GUIDE.md. Bump RBGRID_API_VERSION on any breaking surface change. */
/* RBGRID_API_VERSION lives in rbgrid_core.h (shared with the ui-commit guard) */

/* ISO-4217 minor-unit digits for money display formatting (`money.money_decimals` in /schema).
 * Default 2; the 3-digit dinar/rial group and the common 0-digit currencies are enumerated. */
static int iso4217_minor_digits(const char *code)
{
    static const char *THREE[] = { "BHD", "IQD", "JOD", "KWD", "LYD", "OMR", "TND" };
    static const char *ZERO[]  = { "BIF", "CLP", "DJF", "GNF", "ISK", "JPY", "KMF", "KRW",
                                   "PYG", "RWF", "UGX", "VND", "VUV", "XAF", "XOF", "XPF" };
    for (unsigned i = 0; i < sizeof(THREE) / sizeof(THREE[0]); i++)
        if (strcmp(code, THREE[i]) == 0) return 3;
    for (unsigned i = 0; i < sizeof(ZERO) / sizeof(ZERO[0]); i++)
        if (strcmp(code, ZERO[i]) == 0) return 0;
    return 2;
}

/**
 * @brief Build the /api/v1/schema JSON (API contract descriptor).
 *
 * Shared by the HTTP handler and the MQTT `cmd get schema` path.
 *
 * @return malloc'd JSON string (caller frees), or NULL on OOM.
 */
char *rbgrid_build_schema_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "api_version", RBGRID_API_VERSION);
    cJSON_AddNumberToObject(root, "telemetry_schema", RBGRID_TELEMETRY_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "telemetry_profile", rbgrid_telemetry_get_lean() ? "lean" : "full");
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw", (ad && ad->version[0]) ? ad->version : "0.0.0");

    /* API contract v4 (additive): build flavor, hardware profile, storage state, the
     * per-source electrical key sets and the quality-bit dictionary. */
    cJSON_AddStringToObject(root, "build", "full");
    cJSON_AddStringToObject(root, "hw_profile", "esp32-wrover-psram");
    cJSON_AddStringToObject(root, "storage_state", rbstats_store_state_str());
    cJSON_AddNumberToObject(root, "history_schema_ver", 4);   /* history record JSON forms */
    {   /* time_quality — same source as the telemetry frame */
        extern uint8_t rbtime_quality(uint32_t *sync_age_s_out);
        uint32_t sync_age_s = 0; uint8_t tq = rbtime_quality(&sync_age_s);
        cJSON *tqo = cJSON_AddObjectToObject(root, "time_quality");
        if (tqo) { cJSON_AddNumberToObject(tqo, "tq", tq); cJSON_AddStringToObject(tqo, "time_src", rbtime_clock_source_str(tq)); cJSON_AddNumberToObject(tqo, "sync_age_s", sync_age_s); }
    }
    {
        /* fields_by_source (per-source electrical key sets; keys will not change) */
        cJSON *fbs = cJSON_AddObjectToObject(root, "fields_by_source");
        cJSON *rbamp = cJSON_AddArrayToObject(fbs, "rbamp");
        static const char *RBAMP_KEYS[] = { "v", "i", "p_act", "v_min", "v_max", "i_min", "i_max", "p_min", "p_max" };
        for (unsigned k = 0; k < sizeof(RBAMP_KEYS) / sizeof(RBAMP_KEYS[0]); k++)
            cJSON_AddItemToArray(rbamp, cJSON_CreateString(RBAMP_KEYS[k]));
        cJSON *esn = cJSON_AddArrayToObject(fbs, "espnow");
        static const char *ESN_KEYS[] = { "v", "i", "p_act" };
        for (unsigned k = 0; k < sizeof(ESN_KEYS) / sizeof(ESN_KEYS[0]); k++)
            cJSON_AddItemToArray(esn, cJSON_CreateString(ESN_KEYS[k]));
        /* quality_bits (contract §1): bit index by name; q=0 = GOOD */
        cJSON *qb = cJSON_AddObjectToObject(root, "quality_bits");
        cJSON_AddNumberToObject(qb, "PARTIAL", 0);
        cJSON_AddNumberToObject(qb, "TIME_UNSYNC", 1);
        cJSON_AddNumberToObject(qb, "CLOCK_STEP", 2);
        cJSON_AddNumberToObject(qb, "SRC_GAP", 3);
        cJSON_AddNumberToObject(qb, "RECOMPUTED", 4);
        cJSON_AddNumberToObject(qb, "ESTIMATED", 5);
        cJSON_AddNumberToObject(qb, "UNSIGNED_SRC", 6);
        cJSON_AddNumberToObject(qb, "OVERRANGE", 7);
        cJSON_AddNumberToObject(qb, "MIGRATED", 8);
    }

    // Explicit capability flags (schema-driven UI; more reliable than inferring from endpoints[]).
    cJSON *cap = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddBoolToObject(cap, "espnow", true);
    cJSON_AddBoolToObject(cap, "economics", true);
    cJSON_AddBoolToObject(cap, "summary", true);
    cJSON_AddBoolToObject(cap, "exportCsv", true);
    cJSON_AddBoolToObject(cap, "telemetryConfigPost", true);
    cJSON_AddBoolToObject(cap, "mqttConfig", true);
    cJSON_AddBoolToObject(cap, "adcUnboundDiscovery", false);   // known gap: /api/v1/sensors lists i2c-unbound only
    cJSON_AddBoolToObject(cap, "historyTimeRange", true);
    cJSON_AddBoolToObject(cap, "auth", true);
    cJSON_AddBoolToObject(cap, "https", true);
    cJSON_AddBoolToObject(cap, "provisioning", true);
    cJSON_AddBoolToObject(cap, "configBackup", true);
    cJSON_AddBoolToObject(cap, "inputAggregate", true);   /* history/economics ?unit=input: the grid input summed over its points (1.1.6) */
    cJSON_AddBoolToObject(cap, "wsTicket", true);         /* POST /api/v1/ws-ticket → /ws-telemetry?ticket= (1.1.7); ?token= still accepted */

    /* Configurable-surface descriptor: units of measure, key format,
     * canonical enums and money formatting — so the UI renders from data instead of hardcoding.
     * Wire invariants these declare: energy = float Wh on EVERY endpoint (mWh exists only inside
     * the v4 flash records); money = float in MAJOR currency units on EVERY endpoint (cents exist
     * only inside the v4 flash records). */
    {
        cJSON *uom = cJSON_AddObjectToObject(root, "units_of_measure");
        cJSON_AddStringToObject(uom, "energy", "Wh");
        cJSON_AddStringToObject(uom, "power", "W");
        cJSON_AddStringToObject(uom, "voltage", "V");
        cJSON_AddStringToObject(uom, "current", "A");
        cJSON_AddStringToObject(uom, "frequency", "Hz");
        cJSON_AddStringToObject(uom, "money", "major");         /* float, major currency units */
        cJSON_AddStringToObject(uom, "rate", "major_per_kwh");  /* tariff rates: currency per kWh */

        cJSON *kf = cJSON_AddObjectToObject(root, "key_format");
        cJSON_AddStringToObject(kf, "unit_key", "HEX4");        /* %04X upper-case, e.g. "0006" */
        /* telemetry still emits legacy "main"/"alt" supply keys; flips to hex at the v2 frame */
        cJSON_AddBoolToObject(kf, "legacy_supply_keys", true);

        cJSON *en = cJSON_AddObjectToObject(root, "enums");
        cJSON *roles = cJSON_AddArrayToObject(en, "role");
        for (unsigned r = 0; rbpower_load_role_name(r); r++)
            cJSON_AddItemToArray(roles, cJSON_CreateString(rbpower_load_role_name(r)));
        cJSON *sp = cJSON_AddArrayToObject(en, "site_profile");
        cJSON_AddItemToArray(sp, cJSON_CreateString("home"));
        cJSON_AddItemToArray(sp, cJSON_CreateString("industrial"));
        cJSON_AddItemToArray(sp, cJSON_CreateString("ev"));
        /* UI language chosen by the installer (system.language; "" = unset → UI falls back to the
         * browser language). The web app applies the device language only when this enum exists. */
        /* empty: any ISO 639-1 code is accepted (language packs on demand) — the web app greys
         * out codes only when this list is non-empty */
        (void)cJSON_AddArrayToObject(en, "language");
        {   /* CT models from the rbAmp model registry (rbamp_sensor_models.h):
             *  ct_model     — SCT- series, user-selectable per channel (class 1, characterized codes only — reserved ones the
             *                 module refuses with ERR_PARAM); PATCH /api/v1/units/{hex} ct_code
             *  ct_model_ct  — CT- series names (class 2, CT shipped with the module, calibrated in pair) — display only */
            cJSON *ct  = cJSON_AddObjectToObject(en, "ct_model");
            cJSON *ct2 = cJSON_AddObjectToObject(en, "ct_model_ct");
            char k[4];
            for (unsigned i = 0; i < RBAMP_SENSOR_MODEL_COUNT; i++) {
                const rbamp_sensor_model_info_t *mi = &RBAMP_SENSOR_MODELS[i];
                if (!rbgrid_ct_model_selectable(mi)) continue;
                snprintf(k, sizeof(k), "%u", (unsigned)mi->code);
                if (mi->sensor_class == RBAMP_SENSOR_CLASS_SCT_013) cJSON_AddStringToObject(ct, k, mi->descriptor);
                else if (mi->sensor_class == RBAMP_SENSOR_CLASS_WIRED_CT) cJSON_AddStringToObject(ct2, k, mi->descriptor);
            }
        }
        cJSON *ph = cJSON_AddObjectToObject(en, "phase");
        cJSON_AddStringToObject(ph, "0", "UNSET");
        cJSON_AddStringToObject(ph, "1", "L1");
        cJSON_AddStringToObject(ph, "2", "L2");
        cJSON_AddStringToObject(ph, "3", "L3");
        cJSON_AddStringToObject(ph, "4", "3P");
        /* phase topology: two-pole / line-to-line loads. 5 = L1L2 (also the US
         * split-phase 240 V load), 6/7 = the other 3P pairs. Valid set per grid.system is a client
         * contract rule (1P: 0,1 · SPLIT: 0,1,2,5 · 3P: all) — firmware validates range only. */
        cJSON_AddStringToObject(ph, "5", "L1L2");
        cJSON_AddStringToObject(ph, "6", "L2L3");
        cJSON_AddStringToObject(ph, "7", "L3L1");
        /* site mains system (system.grid.system; "" = unset → client derives 1P) */
        cJSON *gs = cJSON_AddArrayToObject(en, "grid_system");
        cJSON_AddItemToArray(gs, cJSON_CreateString("1P"));
        cJSON_AddItemToArray(gs, cJSON_CreateString("SPLIT"));
        cJSON_AddItemToArray(gs, cJSON_CreateString("3P"));
        /* Which grid systems were validated on hardware — the UI labels the others "not validated"
         * from HERE, not from a hardcoded list. */
        cJSON *gv = cJSON_AddArrayToObject(en, "grid_system_validated");
        cJSON_AddItemToArray(gv, cJSON_CreateString("1P"));
        /* 3P validated on hardware (an rbAmp STD-U3I3 module on a live three-phase installation):
         * module check (U per phase, CT↔phase pairing, CT direction, Q sign on a real inductive
         * load) + hub: per-phase ph[] = module, Σ phases = total, quarter/hour/day records, planned reboot,
         * poll pause, cold downtime across local midnight. */
        cJSON_AddItemToArray(gv, cJSON_CreateString("3P"));

        /* input-field limits (BYTES, utf-8) — UI renders counters from here, no hardcode */
        cJSON *lim = cJSON_AddObjectToObject(root, "limits");
        cJSON_AddNumberToObject(lim, "short_name_bytes", 24);
        cJSON_AddNumberToObject(lim, "name_bytes", 31);
        cJSON_AddNumberToObject(lim, "origins_max", 8);
        rbgrid_json_add_config_ranges(lim);          /* limits.ranges — numeric config bounds */
        cJSON *tz = cJSON_AddArrayToObject(en, "tariff_zone");
        for (unsigned z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++)
            cJSON_AddItemToArray(tz, cJSON_CreateString(rbpower_tariff_zone_type_text[z]));
        cJSON *pt = cJSON_AddArrayToObject(en, "tariff_plan_type");
        for (unsigned t = 0; t < sizeof(rbpower_tariff_plan_type_text) / sizeof(rbpower_tariff_plan_type_text[0]); t++)
            cJSON_AddItemToArray(pt, cJSON_CreateString(rbpower_tariff_plan_type_text[t]));
        cJSON *ct = cJSON_AddArrayToObject(en, "tariff_condition_type");
        for (unsigned c = 0; c < sizeof(rbpower_tariff_condition_type_text) / sizeof(rbpower_tariff_condition_type_text[0]); c++)
            cJSON_AddItemToArray(ct, cJSON_CreateString(rbpower_tariff_condition_type_text[c]));

        /* money formatting: currency comes from the ACTIVE tariff plan (display label, no conversion);
         * decimals follow ISO-4217 minor units; rates keep up to 4 decimals. */
        cJSON *money = cJSON_AddObjectToObject(root, "money");
        const char *cur = rbpower_tariff_currency();
        cJSON_AddStringToObject(money, "currency_code", cur);
        cJSON_AddNumberToObject(money, "money_decimals", iso4217_minor_digits(cur));
        cJSON_AddNumberToObject(money, "rate_decimals", 4);
    }

    // Curated endpoint surface (the contract baseline). tier: "1"=1:1 struct mirror, "composite"=aggregated.
    // auth = the write (POST/DELETE) is token-guarded; reboot = a POST to it needs a device restart to apply.
    // reboot_fields (mixed endpoints): only these fields apply at restart, the rest are live — the POST answer's
    // requires_reboot is true only when one of them changed (system-config: rbwebserver.c needs_reboot;
    // time-sync: enable_time_sync/primary_source/sntp_server, timezone is live).
    static const struct { const char *path; const char *methods; const char *tier; bool auth; bool reboot; const char *params; const char *reboot_fields; } EP[] = {
        { "/api/v1/history",            "GET",      "1+env",     false, false, "unit_id,period,type(electrical|energy|economics|all),offset,limit,from,to,since" },
        { "/api/v1/economics",          "GET",      "composite", false, false, "unit" },
        { "/api/v1/summary",            "GET",      "composite", false, false, "unit,period,from,to" },
        { "/api/v1/export.csv",         "GET",      "1-csv",     false, false, "unit,period,type,from,to" },
        { "/api/v1/espnow",             "GET,POST", "composite", true,  false, "action(pair|add|remove|disable|enable|latch),mac,seconds,channels" },
        { "/api/v1/units",              "POST",     "n/a",       true,  false, "sensor,name,role,stat_class,point,reuse" },
        { "/api/v1/units/{hex}",        "POST,DELETE","n/a",     true,  false, "name,short_name,phase,rated_power_w,max_current_a,role,group_id,role_trunk,invert_sign,charge_threshold_w,idle_timeout_min,ct_code (POST=params patch; invert_sign + ct_code apply live; ct_code = enums.ct_model, rbAmp current channels; EV overrides 0 = system.ev)" },
        { "/api/v1/units/{hex}/rebind", "POST",     "n/a",       true,  false, "module,ch" },
        { "/api/v1/units/{hex}/rename", "POST",     "n/a",       true,  false, "point,reuse" },
        { "/api/v1/sensors",            "GET",      "composite", false, false, "" },
        { "/api/telemetry/latest",      "GET",      "composite", false, false, "" },
        { "/api/system/provisioning",   "GET",      "composite", false, false, "" },
        { "/api/system/token",          "GET,POST", "n/a",       true,  false, "token,role(admin|read)" },
        { "/api/system/security",       "GET,POST", "n/a",       true,  false, "dev_cors,origins" },
        { "/api/v1/ws-ticket",          "POST",     "n/a",       false, false, "→ {ticket,expires_s}; open /ws-telemetry?ticket=<ticket> once within expires_s" },
        { "/api/system/backup",         "GET,POST", "n/a",       true,  false, "" },
        { "/api/system/restore",        "POST",     "n/a",       true,  true,  "" },
        { "/api/system/system-config",  "GET,POST", "composite", true,  false, "" },   /* all fields live */
        { "/api/system/hardware-config","GET,POST", "composite", true,  true,  "" },
        { "/api/system/units-config",   "GET,POST", "composite", true,  false, "" },
        { "/api/system/tariff-plan-config","GET,POST","1",       true,  false, "" },
        { "/api/system/telemetry-config","GET,POST", "composite", true,  false, "profile(lean|full)" },
        { "/api/system/mqtt-config",    "GET,POST", "composite", true,  true,  "broker_uri" },
        { "/api/system/time-sync",      "GET,POST", "composite", true,  true,  "", "enable_time_sync,primary_source,sntp_server" },
        { "/api/system/reboot",         "POST",     "n/a",       true,  false, "delay_ms" },
        { "/api/system/factory-reset",  "POST",     "n/a",       true,  true,  "history(erase|keep)" },
        { "/api/system/version",        "GET",      "1",         false, false, "" },
        { "/api/system/ota",            "POST",     "n/a",       true,  true,  "(binary)" },
        { "/api/system/ui-upload",      "POST",     "n/a",       true,  false, "path (query); body=raw file" },
        { "/api/system/ui-commit",      "POST",     "n/a",       true,  false, "force (query); body=ui-manifest.json" },
        { "/api/system/ap",             "GET,POST", "n/a",       true,  false, "GET: AP ssid/password (admin token, armed hub only); POST {password}: the user sets the AP password (\"\" = setup password)" },
        { "/api/system/lang",           "GET,POST", "n/a",       true,  false, "code (query, ISO 639-1); body=language pack JSON ≤ 200 KB; one pack kept; served at /lang/<code>.json" },
    };
    cJSON *eps = cJSON_AddArrayToObject(root, "endpoints");
    for (unsigned i = 0; i < sizeof(EP) / sizeof(EP[0]); i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "path", EP[i].path);
        cJSON_AddStringToObject(e, "methods", EP[i].methods);
        cJSON_AddStringToObject(e, "tier", EP[i].tier);
        cJSON_AddBoolToObject(e, "auth_required", EP[i].auth);
        cJSON_AddBoolToObject(e, "requires_reboot", EP[i].reboot);
        if (EP[i].params[0]) cJSON_AddStringToObject(e, "params", EP[i].params);
        if (EP[i].reboot_fields) {
            cJSON *rf = cJSON_AddArrayToObject(e, "reboot_fields");
            char buf[160]; strlcpy(buf, EP[i].reboot_fields, sizeof(buf));
            for (char *sv = NULL, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                cJSON_AddItemToArray(rf, cJSON_CreateString(t));
        }
        cJSON_AddItemToArray(eps, e);
    }
    cJSON *tr = cJSON_AddObjectToObject(root, "transports");
    cJSON_AddStringToObject(tr, "ws", "/ws-telemetry (push + get/start/stop_telemetry)");
    cJSON_AddStringToObject(tr, "mqtt", "rbgrid/<mac>/{telemetry,status,cmd,cmd/resp} + HA discovery");

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}
