/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbdynprice.c
 * @brief Dynamic (spot) energy prices — see rbdynprice.h and documents/architecture/DYNAMIC_TARIFFS_DESIGN.md.
 *
 * Layout: three provider stores (HA push, primary network, backup network), a config persisted in NVS, a fetch
 * task (one HTTPS request at a time, never in the booking path), and the resolver the tariff engine calls per
 * 15-min period. Stores live in PSRAM when there is one; a spinlock guards them (lookups are a few hundred
 * compares). The stores are saved to NVS after a change, so a night reboot keeps the prices.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "rbdynprice.h"
#include "rbgrid_core.h"      /* rbgrid_nvs_open */
#include "rbhome_tariff.h"    /* plan, currency, zone rates */
#include "rbhome_types.h"     /* rbpower_diag_printf */

static const char *TAG = "RBDYNPRICE";

/* ======================================== provider catalogue ======================================== */

typedef struct { const char *id, *name, *country, *currency; } dp_zone_t;

/* Energy-Charts zones whose day-ahead data is CC BY 4.0 (Bundesnetzagentur | SMARD.de). The other zones on that
 * API are "private and internal use only" and are not offered. Prices are EUR/MWh for every zone. */
static const dp_zone_t EC_ZONES[] = {
    {"AT", "Austria", "AT", "EUR"},          {"BE", "Belgium", "BE", "EUR"},
    {"CH", "Switzerland", "CH", "EUR"},      {"CZ", "Czechia", "CZ", "EUR"},
    {"DE-LU", "Germany / Luxembourg", "DE", "EUR"},
    {"DK1", "Denmark West (DK1)", "DK", "EUR"}, {"DK2", "Denmark East (DK2)", "DK", "EUR"},
    {"FR", "France", "FR", "EUR"},           {"HU", "Hungary", "HU", "EUR"},
    {"IT-North", "Italy North", "IT", "EUR"},{"NL", "Netherlands", "NL", "EUR"},
    {"NO2", "Norway South-West (NO2)", "NO", "EUR"},
    {"PL", "Poland", "PL", "EUR"},           {"SE4", "Sweden South (SE4)", "SE", "EUR"},
    {"SI", "Slovenia", "SI", "EUR"},
};
#define EC_NZONES (sizeof(EC_ZONES) / sizeof(EC_ZONES[0]))

typedef enum { CLS_SPOT = 0, CLS_ALL_IN = 1 } dp_class_t;

typedef struct {
    const char *id, *name;
    dp_class_t  cls;              /* CLS_SPOT / CLS_ALL_IN; the HA provider takes its class from the config */
    bool        local;            /* local = pushed to the hub (HA), not fetched */
    bool        needs_token;
    uint32_t    resolution_s;
    const dp_zone_t *zones; size_t nzones;
    const char *terms_url, *attribution;
} dp_provider_t;

static const dp_provider_t PROVIDERS[] = {
    {"energy_charts", "Energy-Charts (Fraunhofer ISE)", CLS_SPOT, false, false, 900, EC_ZONES, EC_NZONES,
     "https://api.energy-charts.info/", "Bundesnetzagentur | SMARD.de / Energy-Charts (CC BY 4.0)"},
    {"ha", "Home Assistant", CLS_SPOT, true, false, 0, NULL, 0, "", ""},
};
#define NPROV (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

static const dp_provider_t *provider_by_id(const char *id)
{
    for (size_t i = 0; id && i < NPROV; i++) if (!strcmp(PROVIDERS[i].id, id)) return &PROVIDERS[i];
    return NULL;
}
static const dp_zone_t *zone_of(const dp_provider_t *p, const char *zone)
{
    for (size_t i = 0; p && zone && i < p->nzones; i++) if (!strcmp(p->zones[i].id, zone)) return &p->zones[i];
    return NULL;
}

/* ============================================== config ============================================== */

enum { SELL_SOURCE = 0, SELL_PLAN = 1, SELL_FORMULA = 2 };
static const char *const SELL_MODES[] = {"source", "plan", "formula"};

#define DP_CFG_VER 1
typedef struct {
    uint8_t ver;
    bool    enabled;
    char    source[16];           /* provider id */
    char    backup[16];           /* provider id or "none" */
    char    zone[12];             /* zone of the network provider(s) */
    uint8_t ha_class;             /* CLS_SPOT / CLS_ALL_IN */
    float   factor, markup;       /* spot → energy rate */
    bool    zones_stack;          /* add the plan zone's rate to the spot (TOU grid fees) */
    bool    hourly_mean;          /* bill the hourly mean of the 15-min prices */
    float   floor_buy, floor_sell;/* NAN = none */
    uint8_t sell_mode;            /* SELL_* */
    float   k_sell, feedin_fee;   /* formula sell = raw × k_sell − feedin_fee */
} dp_cfg_t;

static dp_cfg_t s_cfg;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

static void cfg_defaults(dp_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->ver = DP_CFG_VER;
    c->enabled = false;           /* off: only HA pushes apply, as before (spot class, factor 1, no markup) */
    strcpy(c->source, "ha");
    strcpy(c->backup, "none");
    c->ha_class = CLS_SPOT;
    c->factor = 1.0f; c->markup = 0.0f;
    c->floor_buy = NAN; c->floor_sell = NAN;
    c->sell_mode = SELL_SOURCE;
    c->k_sell = 1.0f; c->feedin_fee = 0.0f;
}
static void cfg_get(dp_cfg_t *out) { portENTER_CRITICAL(&s_cfg_mux); *out = s_cfg; portEXIT_CRITICAL(&s_cfg_mux); }

/* ============================================== stores ============================================== */

typedef struct { uint32_t from, until; float price, sell; } dp_entry_t;
typedef struct { dp_entry_t *e; int n; char provider[16]; char zone[12]; bool dirty; } dp_store_t;

enum { ST_HA = 0, ST_NET_P = 1, ST_NET_B = 2, ST_N = 3 };
static dp_store_t s_st[ST_N];
static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_gen = 1;
static volatile bool s_ha_present = false, s_ha_seen = false;

static bool clock_ok(void) { return time(NULL) > 1600000000; }

/* Put one entry (caller holds s_st_mux). Same `from` replaces; entries that ended over an hour ago drop; full →
 * the oldest start goes. */
static void st_put_locked(dp_store_t *s, uint32_t from, uint32_t until, float price, float sell)
{
    uint32_t now = (uint32_t)time(NULL);
    uint32_t keep_after = now > 1600000000u ? now - 3600u : 0u;
    int k = 0;
    for (int i = 0; i < s->n; i++)
        if (s->e[i].until > keep_after && s->e[i].from != from) s->e[k++] = s->e[i];
    s->n = k;
    if (s->n >= RBDP_STORE_MAX) {
        int o = 0;
        for (int i = 1; i < s->n; i++) if (s->e[i].from < s->e[o].from) o = i;
        s->e[o] = s->e[--s->n];
    }
    s->e[s->n++] = (dp_entry_t){from, until, price, sell};
    s->dirty = true;
}

/* The latest-starting entry covering ts (caller holds s_st_mux). */
static const dp_entry_t *st_find_locked(const dp_store_t *s, uint32_t ts)
{
    const dp_entry_t *best = NULL;
    for (int i = 0; i < s->n; i++)
        if (s->e[i].from <= ts && ts < s->e[i].until && (!best || s->e[i].from >= best->from)) best = &s->e[i];
    return best;
}

static bool st_covers(int st, uint32_t ts)
{
    portENTER_CRITICAL(&s_st_mux);
    bool c = s_st[st].e && st_find_locked(&s_st[st], ts) != NULL;
    portEXIT_CRITICAL(&s_st_mux);
    return c;
}

static void st_clear(int st)
{
    portENTER_CRITICAL(&s_st_mux);
    s_st[st].n = 0; s_st[st].dirty = true;
    portEXIT_CRITICAL(&s_st_mux);
    s_gen++;
}

/* Which store plays which role under a config: *p / *b get ST_* or -1. Disabled → HA only (legacy push). */
static void roles(const dp_cfg_t *c, int *p, int *b)
{
    if (!c->enabled) { *p = ST_HA; *b = -1; return; }
    *p = !strcmp(c->source, "ha") ? ST_HA : ST_NET_P;
    if (!strcmp(c->backup, "none") || !c->backup[0]) *b = -1;
    else *b = !strcmp(c->backup, "ha") ? ST_HA : ST_NET_B;
}
static const char *store_provider(const dp_cfg_t *c, int st)
{
    return st == ST_HA ? "ha" : st == ST_NET_P ? c->source : c->backup;
}

/* ============================================== resolver ============================================= */

static void plan_fees(float *fees, float *vat)
{
    const rbpower_tariff_plan_t *pl = rbpower_tariff_get_current_plan();
    *fees = pl ? pl->grid_usage_fee_per_kwh + pl->energy_tax_per_kwh + pl->renewable_levy_per_kwh : 0.0f;
    *vat  = pl ? pl->vat_percent / 100.0f : 0.0f;
}

/* Raw price (and raw sell, NAN if none) of store st at ts, hourly mean if asked. */
static bool st_price_at(int st, uint32_t ts, bool hourly, float *raw, float *raw_sell, uint32_t *until, bool *is_mean)
{
    bool hit = false;
    *is_mean = false;
    portENTER_CRITICAL(&s_st_mux);
    const dp_store_t *s = &s_st[st];
    const dp_entry_t *e = s->e ? st_find_locked(s, ts) : NULL;
    if (e) {
        hit = true; *raw = e->price; *raw_sell = e->sell; *until = e->until;
        if (hourly && e->until - e->from < 3600) {
            uint32_t h = ts - ts % 3600;
            float sum = 0, ssum = 0; int n = 0, sn = 0;
            for (int k = 0; k < 4; k++) {
                const dp_entry_t *q = st_find_locked(s, h + 450 + 900 * k);
                if (!q) continue;
                sum += q->price; n++;
                if (!isnan(q->sell)) { ssum += q->sell; sn++; }
            }
            if (n > 0) { *raw = sum / n; *is_mean = n > 1; }
            if (sn > 0) *raw_sell = ssum / sn;
            *until = h + 3600 < e->until ? e->until : h + 3600;
        }
    }
    portEXIT_CRITICAL(&s_st_mux);
    return hit;
}

bool rbdynprice_resolve(uint32_t ts, float zone_buy, float zone_sell, rbdp_price_t *out)
{
    dp_cfg_t c; cfg_get(&c);
    float fees, vat; plan_fees(&fees, &vat);
    memset(out, 0, sizeof(*out));
    out->prov = RBDP_PROV_PLAN;
    out->buy  = (zone_buy + fees) * (1.0f + vat);
    out->sell = zone_sell;
    if (!c.enabled) {             /* legacy push: spot class, factor 1, no markup, sell from the entry or the plan */
        c.factor = 1.0f; c.markup = 0.0f; c.zones_stack = false; c.hourly_mean = false;
        c.floor_buy = NAN; c.floor_sell = NAN; c.sell_mode = SELL_SOURCE; c.ha_class = CLS_SPOT;
    }
    int p, b; roles(&c, &p, &b);
    int order[2] = {p, b};
    for (int r = 0; r < 2; r++) {
        int st = order[r];
        if (st < 0) continue;
        float raw, raw_sell; uint32_t until; bool mean;
        if (!st_price_at(st, ts, c.hourly_mean, &raw, &raw_sell, &until, &mean)) continue;
        const dp_provider_t *pv = provider_by_id(store_provider(&c, st));
        dp_class_t cls = st == ST_HA ? (dp_class_t)c.ha_class : (pv ? pv->cls : CLS_SPOT);
        float buy;
        if (cls == CLS_ALL_IN) buy = raw;
        else buy = (raw * c.factor + c.markup + (c.zones_stack ? zone_buy : 0.0f) + fees) * (1.0f + vat);
        if (!isnan(c.floor_buy) && buy < c.floor_buy) buy = c.floor_buy;
        float sell;
        switch (c.sell_mode) {
            case SELL_PLAN:    sell = zone_sell; break;
            case SELL_FORMULA: sell = raw * c.k_sell - c.feedin_fee; break;
            default:           sell = isnan(raw_sell) ? zone_sell : raw_sell; break;
        }
        if (!isnan(c.floor_sell) && sell < c.floor_sell) sell = c.floor_sell;
        out->covered = true;
        out->backup = (r == 1);
        out->hourly_mean = mean;
        out->prov = st == ST_HA ? RBDP_PROV_HA : (r == 0 ? RBDP_PROV_SOURCE : RBDP_PROV_BACKUP);
        out->raw = raw; out->buy = buy; out->sell = sell; out->until = until;
        return true;
    }
    return false;
}

const char *rbdynprice_prov_code(rbdp_prov_t p)
{
    switch (p) {
        case RBDP_PROV_SOURCE: return "source";
        case RBDP_PROV_BACKUP: return "backup";
        case RBDP_PROV_HA:     return "ha_push";
        default:               return "plan";
    }
}

uint32_t rbdynprice_generation(void) { return s_gen; }

/* ============================================== state =============================================== */

static const char *volatile s_last_error = "none";
static volatile uint32_t s_last_fetch = 0, s_next_fetch = 0;

uint32_t rbdynprice_valid_until(void)
{
    if (!clock_ok()) return 0;
    uint32_t t = (uint32_t)time(NULL), lim = t + 3 * 86400;
    rbdp_price_t pr;
    uint32_t last = 0;
    while (t < lim && rbdynprice_resolve(t, 0, 0, &pr) && pr.until > t) { last = pr.until; t = pr.until; }
    return last;
}

const char *rbdynprice_source_now(bool *is_backup)
{
    if (is_backup) *is_backup = false;
    if (!clock_ok()) return "plan";
    dp_cfg_t c; cfg_get(&c);
    rbdp_price_t pr;
    if (!rbdynprice_resolve((uint32_t)time(NULL), 0, 0, &pr)) return "plan";
    if (is_backup) *is_backup = pr.backup;
    int p, b; roles(&c, &p, &b);
    int st = pr.backup ? b : p;
    if (st == ST_HA) return "ha";
    const dp_provider_t *pv = provider_by_id(store_provider(&c, st));
    return pv ? pv->id : "plan";
}

const char *rbdynprice_state(void)
{
    dp_cfg_t c; cfg_get(&c);
    if (!clock_ok()) return c.enabled ? "plan_fallback" : "disabled";
    uint32_t now = (uint32_t)time(NULL);
    rbdp_price_t pr;
    bool cov = rbdynprice_resolve(now, 0, 0, &pr);
    if (!c.enabled) return cov ? "ok" : "disabled";
    if (cov && pr.backup) return "backup";
    if (cov) {
        int p, b; roles(&c, &p, &b);
        /* after the usual publication (15:00 UTC) without tomorrow's noon covered → today_only */
        uint32_t tomorrow_noon = now - now % 86400 + 86400 + 12 * 3600;
        if ((now % 86400) >= 15 * 3600 && p != ST_HA && !rbdynprice_resolve(tomorrow_noon, 0, 0, &pr)) return "today_only";
        return "ok";
    }
    return !strcmp(c.source, "ha") ? "stale" : "plan_fallback";
}

/* ============================================ persistence =========================================== */

#define DP_NVS_NS   "rbdynp"
#define DP_ST_MAGIC 0x50445231u   /* "1RDP" */
typedef struct { uint32_t magic; char provider[16]; char zone[12]; uint16_t n; } dp_st_hdr_t;

static void cfg_save(void)
{
    nvs_handle_t h;
    if (rbgrid_nvs_open(DP_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    dp_cfg_t c; cfg_get(&c);
    nvs_set_blob(h, "cfg", &c, sizeof(c));
    nvs_commit(h); nvs_close(h);
}

static void cfg_load(void)
{
    cfg_defaults(&s_cfg);
    nvs_handle_t h;
    if (rbgrid_nvs_open(DP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    dp_cfg_t c; size_t len = sizeof(c);
    if (nvs_get_blob(h, "cfg", &c, &len) == ESP_OK && len == sizeof(c) && c.ver == DP_CFG_VER) {
        c.source[sizeof(c.source) - 1] = 0; c.backup[sizeof(c.backup) - 1] = 0; c.zone[sizeof(c.zone) - 1] = 0;
        s_cfg = c;
    }
    nvs_close(h);
}

static void st_save_dirty(void)
{
    for (int st = 0; st < ST_N; st++) {
        if (!s_st[st].dirty || !s_st[st].e) continue;
        size_t sz = sizeof(dp_st_hdr_t) + RBDP_STORE_MAX * sizeof(dp_entry_t);
        uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = malloc(sz);
        if (!buf) return;
        dp_st_hdr_t *hd = (dp_st_hdr_t *)buf;
        portENTER_CRITICAL(&s_st_mux);
        hd->magic = DP_ST_MAGIC;
        memcpy(hd->provider, s_st[st].provider, sizeof(hd->provider));
        memcpy(hd->zone, s_st[st].zone, sizeof(hd->zone));
        hd->n = (uint16_t)s_st[st].n;
        memcpy(buf + sizeof(*hd), s_st[st].e, s_st[st].n * sizeof(dp_entry_t));
        s_st[st].dirty = false;
        portEXIT_CRITICAL(&s_st_mux);
        nvs_handle_t h;
        if (rbgrid_nvs_open(DP_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            char key[6]; snprintf(key, sizeof(key), "st%d", st);
            nvs_set_blob(h, key, buf, sizeof(*hd) + hd->n * sizeof(dp_entry_t));
            nvs_commit(h); nvs_close(h);
        }
        free(buf);
    }
}

static void st_load_all(void)
{
    nvs_handle_t h;
    if (rbgrid_nvs_open(DP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(dp_st_hdr_t) + RBDP_STORE_MAX * sizeof(dp_entry_t);
    uint8_t *buf = malloc(sz);
    for (int st = 0; buf && st < ST_N; st++) {
        char key[6]; snprintf(key, sizeof(key), "st%d", st);
        size_t len = sz;
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK || len < sizeof(dp_st_hdr_t)) continue;
        dp_st_hdr_t *hd = (dp_st_hdr_t *)buf;
        if (hd->magic != DP_ST_MAGIC || hd->n > RBDP_STORE_MAX || len != sizeof(*hd) + hd->n * sizeof(dp_entry_t)) continue;
        portENTER_CRITICAL(&s_st_mux);
        memcpy(s_st[st].provider, hd->provider, sizeof(s_st[st].provider));
        memcpy(s_st[st].zone, hd->zone, sizeof(s_st[st].zone));
        s_st[st].provider[15] = 0; s_st[st].zone[11] = 0;
        memcpy(s_st[st].e, buf + sizeof(*hd), hd->n * sizeof(dp_entry_t));
        s_st[st].n = hd->n;
        portEXIT_CRITICAL(&s_st_mux);
        if (st == ST_HA && hd->n) s_ha_seen = true;
    }
    free(buf);
    nvs_close(h);
}

/* ============================================ HA store API ========================================== */

esp_err_t rbdynprice_ha_put(float buy, float sell, uint32_t from, uint32_t until)
{
    if (isnan(buy) || buy < -100.0f || buy > 100.0f || until <= from || (!isnan(sell) && (sell < -100.0f || sell > 100.0f)))
        return ESP_ERR_INVALID_ARG;
    if (!s_st[ST_HA].e) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_st_mux);
    st_put_locked(&s_st[ST_HA], from, until, buy, sell);
    portEXIT_CRITICAL(&s_st_mux);
    s_ha_seen = true;
    s_gen++;
    return ESP_OK;
}

void rbdynprice_ha_clear(void) { st_clear(ST_HA); }
void rbdynprice_set_ha_present(bool present) { s_ha_present = present; }

/* =============================================== fetch ============================================== */

/* GET url into buf (NUL-terminated). Returns bytes or -1; *status = HTTP status. */
static int http_get(const char *url, char *buf, int cap, int *status)
{
    *status = 0;
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048, .user_agent = "RBgrid-hub",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;
    int n = -1;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        n = 0;
        while (n < cap - 1) {
            int r = esp_http_client_read(h, buf + n, cap - 1 - n);
            if (r <= 0) break;
            n += r;
        }
        buf[n] = 0;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return n;
}

/* Energy-Charts: {"unix_seconds":[…],"price":[…],"unit":"EUR / MWh"} — period starts, EUR/MWh. */
static const char *fetch_energy_charts(int st, const char *zone, uint32_t start, uint32_t end, int *stored)
{
    *stored = 0;
    const int CAP = 32 * 1024;
    char *buf = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(CAP);
    if (!buf) return "fetch_failed";
    char url[160];
    snprintf(url, sizeof(url), "https://api.energy-charts.info/price?bzn=%s&start=%lu&end=%lu",
             zone, (unsigned long)start, (unsigned long)end);
    int status = 0;
    int n = http_get(url, buf, CAP, &status);
    const char *err = NULL;
    if (n < 0) err = "fetch_failed";
    else if (status == 429) err = "rate_limited";
    else if (status == 401 || status == 403) err = "auth";
    else if (status == 404) err = "no_data_for_zone";
    else if (status != 200) err = "http_status";
    cJSON *j = err ? NULL : cJSON_ParseWithLength(buf, n);
    free(buf);
    if (err) return err;
    if (!j) return "parse";
    const cJSON *ts = cJSON_GetObjectItem(j, "unix_seconds"), *pr = cJSON_GetObjectItem(j, "price");
    const char *unit = cJSON_GetStringValue(cJSON_GetObjectItem(j, "unit"));
    int cnt = cJSON_IsArray(ts) && cJSON_IsArray(pr) ? cJSON_GetArraySize(ts) : -1;
    if (cnt < 0 || cnt != cJSON_GetArraySize(pr)) { cJSON_Delete(j); return "parse"; }
    if (cnt == 0) { cJSON_Delete(j); return "no_data_for_zone"; }
    float div = (unit && strstr(unit, "kWh")) ? 1.0f : 1000.0f;
    portENTER_CRITICAL(&s_st_mux);
    for (int i = 0; i < cnt; i++) {
        const cJSON *a = cJSON_GetArrayItem(ts, i), *p = cJSON_GetArrayItem(pr, i);
        if (!cJSON_IsNumber(a) || !cJSON_IsNumber(p)) continue;      /* null = no price for the slot */
        uint32_t from = (uint32_t)a->valuedouble, until;
        const cJSON *nx = (i + 1 < cnt) ? cJSON_GetArrayItem(ts, i + 1) : NULL;
        if (nx && cJSON_IsNumber(nx) && (uint32_t)nx->valuedouble > from && (uint32_t)nx->valuedouble - from <= 3600)
            until = (uint32_t)nx->valuedouble;
        else if (i > 0 && cJSON_IsNumber(cJSON_GetArrayItem(ts, i - 1)))
            until = from + (from - (uint32_t)cJSON_GetArrayItem(ts, i - 1)->valuedouble);
        else until = from + 900;
        if (until - from > 3600) until = from + 900;
        st_put_locked(&s_st[st], from, until, (float)(p->valuedouble / div), NAN);
        (*stored)++;
    }
    portEXIT_CRITICAL(&s_st_mux);
    cJSON_Delete(j);
    return *stored ? NULL : "no_data_for_zone";
}

/* Fetch the network store st now for provider/zone. Returns the error code or NULL. */
static const char *fetch_store(int st, const char *provider, const char *zone)
{
    uint32_t now = (uint32_t)time(NULL);
    uint32_t start = now - now % 86400 - 3600;          /* from yesterday 23:00 UTC = today 00:00 CET */
    uint32_t end   = start + 3 * 86400;
    int stored = 0;
    const char *err = "no_data_for_zone";
    if (!strcmp(provider, "energy_charts")) err = fetch_energy_charts(st, zone, start, end, &stored);
    s_last_fetch = now;
    if (!err) {
        rbpower_diag_printf("price: %s %s stored %d periods (valid until %lu)", provider, zone, stored,
                            (unsigned long)rbdynprice_valid_until());
        s_gen++;
    }
    return err;
}

static volatile bool s_refetch = true;
static uint32_t s_next_try[ST_N];

/* Does network store st need a fetch now? today = now not covered; tomorrow = after 11:00 UTC (12:00 CET winter,
 * 13:00 CEST) tomorrow's noon not covered. */
static bool need_fetch(int st, uint32_t now)
{
    if (!st_covers(st, now)) return true;
    uint32_t tomorrow_noon = now - now % 86400 + 86400 + 12 * 3600;
    return (now % 86400) >= 11 * 3600 && !st_covers(st, tomorrow_noon);
}

static void fetch_task(void *arg)
{
    (void)arg;
    uint32_t last_save = 0;
    const char *prev_err = "none";
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_refetch ? 2000 : 30000));
        uint32_t now = (uint32_t)time(NULL);
        if (now - last_save >= 60) { st_save_dirty(); last_save = now; }
        if (!clock_ok()) continue;
        dp_cfg_t c; cfg_get(&c);
        if (s_refetch) { memset(s_next_try, 0, sizeof(s_next_try)); s_refetch = false; }
        if (!c.enabled) { s_next_fetch = 0; continue; }
        int p, b; roles(&c, &p, &b);
        int net[2] = {p == ST_NET_P ? ST_NET_P : -1, b == ST_NET_B ? ST_NET_B : -1};
        uint32_t next = 0;
        for (int i = 0; i < 2; i++) {
            int st = net[i];
            if (st < 0) continue;
            const char *prov = store_provider(&c, st);
            if (!need_fetch(st, now)) continue;
            if (now < s_next_try[st]) { if (!next || s_next_try[st] < next) next = s_next_try[st]; continue; }
            const char *err = fetch_store(st, prov, c.zone);
            bool still = need_fetch(st, (uint32_t)time(NULL));
            /* retry: every 15 min until 15:00 UTC, then hourly; a hard error waits 15 min too */
            uint32_t step = ((now % 86400) < 15 * 3600) ? 900 : 3600;
            s_next_try[st] = (err || still) ? now + step : 0;
            if (s_next_try[st] && (!next || s_next_try[st] < next)) next = s_next_try[st];
            const char *code = err ? err : "none";
            if (strcmp(code, prev_err)) {
                if (err) rbpower_diag_printf("FAIL price: %s %s fetch failed (%s), retry in %lu min", prov, c.zone, err,
                                             (unsigned long)(step / 60));
                prev_err = code;
            }
            s_last_error = code;
            st_save_dirty(); last_save = (uint32_t)time(NULL);
        }
        s_next_fetch = next;
    }
}

/* =============================================== init =============================================== */

void rbdynprice_init(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    for (int st = 0; st < ST_N; st++) {
        s_st[st].e = heap_caps_calloc(RBDP_STORE_MAX, sizeof(dp_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_st[st].e) s_st[st].e = calloc(RBDP_STORE_MAX, sizeof(dp_entry_t));
    }
    strcpy(s_st[ST_HA].provider, "ha");
    cfg_load();
    st_load_all();
    /* a store saved for another provider/zone than the config now names is stale */
    dp_cfg_t c; cfg_get(&c);
    if (strcmp(s_st[ST_NET_P].provider, c.source) || strcmp(s_st[ST_NET_P].zone, c.zone)) st_clear(ST_NET_P);
    if (strcmp(s_st[ST_NET_B].provider, c.backup) || strcmp(s_st[ST_NET_B].zone, c.zone)) st_clear(ST_NET_B);
    strncpy(s_st[ST_NET_P].provider, c.source, 15); strncpy(s_st[ST_NET_P].zone, c.zone, 11);
    strncpy(s_st[ST_NET_B].provider, c.backup, 15); strncpy(s_st[ST_NET_B].zone, c.zone, 11);
    if (xTaskCreate(fetch_task, "rbdynprice", 8192, NULL, 3, NULL) != pdPASS) ESP_LOGE(TAG, "no fetch task");
    ESP_LOGI(TAG, "dynamic prices: %s, source %s, backup %s, zone %s", c.enabled ? "on" : "off", c.source, c.backup,
             c.zone[0] ? c.zone : "-");
}

/* =============================================== JSON =============================================== */

static void add_num_or_null(cJSON *o, const char *k, float v)
{
    if (isnan(v)) cJSON_AddNullToObject(o, k); else cJSON_AddNumberToObject(o, k, round((double)v * 1e6) / 1e6);
}

cJSON *rbdynprice_config_json(void)
{
    dp_cfg_t c; cfg_get(&c);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "dynprice");
    cJSON_AddBoolToObject(o, "enabled", c.enabled);
    cJSON_AddStringToObject(o, "source", c.source);
    cJSON_AddStringToObject(o, "backup_source", c.backup);
    cJSON_AddStringToObject(o, "zone", c.zone);
    const dp_zone_t *z = NULL;
    for (size_t i = 0; i < NPROV && !z; i++) z = zone_of(&PROVIDERS[i], c.zone);
    cJSON_AddStringToObject(o, "country", z ? z->country : "");
    cJSON_AddStringToObject(o, "ha_class", c.ha_class == CLS_ALL_IN ? "all_in" : "spot");
    add_num_or_null(o, "factor", c.factor);
    add_num_or_null(o, "markup", c.markup);
    cJSON_AddBoolToObject(o, "zones_stack", c.zones_stack);
    cJSON_AddBoolToObject(o, "hourly_mean", c.hourly_mean);
    add_num_or_null(o, "floor_buy", c.floor_buy);
    add_num_or_null(o, "floor_sell", c.floor_sell);
    cJSON_AddStringToObject(o, "sell_mode", SELL_MODES[c.sell_mode < 3 ? c.sell_mode : 0]);
    add_num_or_null(o, "k_sell", c.k_sell);
    add_num_or_null(o, "feedin_fee", c.feedin_fee);
    cJSON_AddBoolToObject(o, "token_set", false);
    cJSON_AddBoolToObject(o, "backup_token_set", false);
    cJSON_AddStringToObject(o, "currency", rbpower_tariff_currency());
    /* status */
    bool bk = false;
    cJSON_AddStringToObject(o, "price_state", rbdynprice_state());
    cJSON_AddStringToObject(o, "source_now", rbdynprice_source_now(&bk));
    cJSON_AddBoolToObject(o, "backup_now", bk);
    cJSON_AddNumberToObject(o, "valid_until", rbdynprice_valid_until());
    cJSON_AddNumberToObject(o, "last_fetch", s_last_fetch);
    cJSON_AddNumberToObject(o, "next_fetch", s_next_fetch);
    cJSON_AddStringToObject(o, "last_error", s_last_error);
    /* catalogue */
    cJSON *pa = cJSON_AddArrayToObject(o, "providers");
    for (size_t i = 0; i < NPROV; i++) {
        const dp_provider_t *p = &PROVIDERS[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", p->id);
        cJSON_AddStringToObject(e, "name", p->name);
        bool is_ha = !strcmp(p->id, "ha");
        cJSON_AddStringToObject(e, "class", (is_ha ? c.ha_class : p->cls) == CLS_ALL_IN ? "all_in" : "spot");
        cJSON_AddBoolToObject(e, "local", p->local);
        cJSON_AddBoolToObject(e, "available", is_ha ? (s_ha_present || s_ha_seen) : true);
        cJSON_AddBoolToObject(e, "needs_token", p->needs_token);
        cJSON_AddNumberToObject(e, "resolution_s", p->resolution_s);
        cJSON *za = cJSON_AddArrayToObject(e, "zones");
        for (size_t k = 0; k < p->nzones; k++) {
            cJSON *zj = cJSON_CreateObject();
            cJSON_AddStringToObject(zj, "id", p->zones[k].id);
            cJSON_AddStringToObject(zj, "name", p->zones[k].name);
            cJSON_AddStringToObject(zj, "country", p->zones[k].country);
            cJSON_AddStringToObject(zj, "currency", p->zones[k].currency);
            cJSON_AddItemToArray(za, zj);
        }
        cJSON_AddStringToObject(e, "terms_url", p->terms_url);
        cJSON_AddStringToObject(e, "attribution", p->attribution);
        cJSON_AddItemToArray(pa, e);
    }
    return o;
}

static bool get_float(const cJSON *d, const char *k, float lo, float hi, bool nullable, float *out, bool *bad)
{
    const cJSON *v = cJSON_GetObjectItem(d, k);
    if (!v) return false;
    if (nullable && cJSON_IsNull(v)) { *out = NAN; return true; }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) { *bad = true; return false; }
    *out = (float)v->valuedouble;
    return true;
}

esp_err_t rbdynprice_set_config(const cJSON *d, char *ef, size_t efl, char *em, size_t eml)
{
    #define REJECT(f, ...) do { snprintf(ef, efl, "%s", f); snprintf(em, eml, __VA_ARGS__); return ESP_ERR_INVALID_ARG; } while (0)
    if (!cJSON_IsObject(d)) REJECT("", "body must be an object");
    dp_cfg_t c, old; cfg_get(&c); old = c;
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(d, "enabled"))) { if (!cJSON_IsBool(v)) REJECT("enabled", "enabled must be true/false"); c.enabled = cJSON_IsTrue(v); }
    if ((v = cJSON_GetObjectItem(d, "source"))) {
        if (!cJSON_IsString(v) || !provider_by_id(v->valuestring)) REJECT("source", "unknown provider");
        snprintf(c.source, sizeof(c.source), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(d, "backup_source"))) {
        if (!cJSON_IsString(v) || (strcmp(v->valuestring, "none") && !provider_by_id(v->valuestring)))
            REJECT("backup_source", "unknown provider (or \"none\")");
        snprintf(c.backup, sizeof(c.backup), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(d, "zone"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.zone)) REJECT("zone", "zone must be a zone id");
        snprintf(c.zone, sizeof(c.zone), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(d, "ha_class"))) {
        if (!cJSON_IsString(v) || (strcmp(v->valuestring, "spot") && strcmp(v->valuestring, "all_in")))
            REJECT("ha_class", "ha_class: spot | all_in");
        c.ha_class = strcmp(v->valuestring, "all_in") ? CLS_SPOT : CLS_ALL_IN;
    }
    if ((v = cJSON_GetObjectItem(d, "sell_mode"))) {
        int m = -1;
        for (int i = 0; i < 3 && cJSON_IsString(v); i++) if (!strcmp(v->valuestring, SELL_MODES[i])) m = i;
        if (m < 0) REJECT("sell_mode", "sell_mode: source | plan | formula");
        c.sell_mode = (uint8_t)m;
    }
    if ((v = cJSON_GetObjectItem(d, "zones_stack"))) { if (!cJSON_IsBool(v)) REJECT("zones_stack", "true/false"); c.zones_stack = cJSON_IsTrue(v); }
    if ((v = cJSON_GetObjectItem(d, "hourly_mean"))) { if (!cJSON_IsBool(v)) REJECT("hourly_mean", "true/false"); c.hourly_mean = cJSON_IsTrue(v); }
    bool bad = false;
    get_float(d, "factor", 0, 10, false, &c.factor, &bad);          if (bad) REJECT("factor", "factor 0..10");
    get_float(d, "markup", -100, 100, false, &c.markup, &bad);      if (bad) REJECT("markup", "markup -100..100");
    get_float(d, "floor_buy", -100, 100, true, &c.floor_buy, &bad); if (bad) REJECT("floor_buy", "floor_buy -100..100 or null");
    get_float(d, "floor_sell", -100, 100, true, &c.floor_sell, &bad); if (bad) REJECT("floor_sell", "floor_sell -100..100 or null");
    get_float(d, "k_sell", 0, 10, false, &c.k_sell, &bad);          if (bad) REJECT("k_sell", "k_sell 0..10");
    get_float(d, "feedin_fee", -100, 100, false, &c.feedin_fee, &bad); if (bad) REJECT("feedin_fee", "feedin_fee -100..100");
    /* cross-field checks */
    if (!strcmp(c.source, c.backup)) REJECT("backup_source", "backup must differ from the source");
    const char *net[2] = {c.source, c.backup};
    for (int i = 0; i < 2; i++) {
        const dp_provider_t *p = provider_by_id(net[i]);
        if (!p || p->local || !c.enabled) continue;
        const dp_zone_t *z = zone_of(p, c.zone);
        if (!z) REJECT("zone", "zone %s is not offered by %s", c.zone[0] ? c.zone : "(none)", p->id);
        const char *cur = rbpower_tariff_currency();
        if (cur && cur[0] && strcmp(cur, z->currency))
            REJECT("zone", "%s prices are in %s, the tariff plan is in %s", p->id, z->currency, cur);
    }
    portENTER_CRITICAL(&s_cfg_mux); s_cfg = c; portEXIT_CRITICAL(&s_cfg_mux);
    cfg_save();
    /* a network store follows its provider + zone: changed → drop it and fetch again */
    if (strcmp(old.source, c.source) || strcmp(old.zone, c.zone)) st_clear(ST_NET_P);
    if (strcmp(old.backup, c.backup) || strcmp(old.zone, c.zone)) st_clear(ST_NET_B);
    portENTER_CRITICAL(&s_st_mux);
    strncpy(s_st[ST_NET_P].provider, c.source, 15); strncpy(s_st[ST_NET_P].zone, c.zone, 11);
    strncpy(s_st[ST_NET_B].provider, c.backup, 15); strncpy(s_st[ST_NET_B].zone, c.zone, 11);
    portEXIT_CRITICAL(&s_st_mux);
    s_refetch = true;
    s_gen++;
    rbpower_diag_printf("price: config %s, source %s, backup %s, zone %s", c.enabled ? "on" : "off", c.source, c.backup,
                        c.zone[0] ? c.zone : "-");
    return ESP_OK;
    #undef REJECT
}

cJSON *rbdynprice_price_json(uint32_t from, uint32_t until)
{
    uint32_t now = (uint32_t)time(NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "price");
    cJSON_AddNumberToObject(o, "now", now);
    rbdp_price_t pr;
    float zb = 0, zs = 0;
    bool on = clock_ok() && rbdynprice_resolve(now, 0, 0, &pr);
    cJSON_AddBoolToObject(o, "active", on);
    bool bk = false;
    cJSON_AddStringToObject(o, "price_state", rbdynprice_state());
    cJSON_AddStringToObject(o, "source_now", rbdynprice_source_now(&bk));
    uint32_t vu = rbdynprice_valid_until();
    cJSON_AddNumberToObject(o, "valid_until", vu);
    cJSON_AddStringToObject(o, "currency", rbpower_tariff_currency());
    /* the HA store, legacy shape (svc-ha reads it back) */
    cJSON *a = cJSON_AddArrayToObject(o, "prices");
    dp_entry_t *tmp = malloc(RBDP_STORE_MAX * sizeof(dp_entry_t));
    int n = 0;
    if (tmp && s_st[ST_HA].e) {
        portENTER_CRITICAL(&s_st_mux);
        n = s_st[ST_HA].n;
        memcpy(tmp, s_st[ST_HA].e, n * sizeof(dp_entry_t));
        portEXIT_CRITICAL(&s_st_mux);
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "from", tmp[i].from);
        cJSON_AddNumberToObject(e, "until", tmp[i].until);
        cJSON_AddNumberToObject(e, "buy", round((double)tmp[i].price * 1e6) / 1e6);
        if (!isnan(tmp[i].sell)) cJSON_AddNumberToObject(e, "sell", round((double)tmp[i].sell * 1e6) / 1e6);
        cJSON_AddItemToArray(a, e);
    }
    free(tmp);
    /* the price per 15-min period as the hub will book it */
    cJSON *r = cJSON_AddArrayToObject(o, "resolved");
    if (clock_ok()) {
        if (!from) from = now - 3600;
        if (!until) until = vu > now ? vu : now + 900;
        from -= from % 900;
        for (int k = 0; k < 200 && from < until; k++, from += 900) {
            uint32_t mid = from + 450;
            rbpower_tariff_zone_rates_at(mid, &zb, &zs);
            rbdynprice_resolve(mid, zb, zs, &pr);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "from", from);
            cJSON_AddNumberToObject(e, "until", from + 900);
            cJSON_AddNumberToObject(e, "buy", round((double)pr.buy * 1e6) / 1e6);
            cJSON_AddNumberToObject(e, "sell", round((double)pr.sell * 1e6) / 1e6);
            if (pr.covered) cJSON_AddNumberToObject(e, "raw", round((double)pr.raw * 1e6) / 1e6);
            cJSON_AddStringToObject(e, "prov", rbdynprice_prov_code(pr.prov));
            if (pr.hourly_mean) cJSON_AddBoolToObject(e, "hourly_mean", true);
            cJSON_AddItemToArray(r, e);
        }
    }
    return o;
}
