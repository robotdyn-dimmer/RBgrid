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
#include "littlefs_raw.h"
#include "rbpeak.h"
#include "rbactivation.h"     /* price log: one file per UTC day on the config partition */

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

/* Nord Pool day-ahead (data portal, unofficial endpoint): Nordic + Baltic areas, prices in the area's currency. */
static const dp_zone_t NP_ZONES[] = {
    {"SE1", "Sweden North (SE1)", "SE", "SEK"}, {"SE2", "Sweden North-Central (SE2)", "SE", "SEK"},
    {"SE3", "Sweden South-Central (SE3)", "SE", "SEK"}, {"SE4", "Sweden South (SE4)", "SE", "SEK"},
    {"NO1", "Norway South-East (NO1)", "NO", "NOK"}, {"NO2", "Norway South-West (NO2)", "NO", "NOK"},
    {"NO3", "Norway Central (NO3)", "NO", "NOK"}, {"NO4", "Norway North (NO4)", "NO", "NOK"},
    {"NO5", "Norway West (NO5)", "NO", "NOK"},
    {"DK1", "Denmark West (DK1)", "DK", "DKK"}, {"DK2", "Denmark East (DK2)", "DK", "DKK"},
    {"FI", "Finland", "FI", "EUR"}, {"EE", "Estonia", "EE", "EUR"}, {"LV", "Latvia", "LV", "EUR"},
    {"LT", "Lithuania", "LT", "EUR"},
};
static const dp_zone_t SE_ZONES[] = {
    {"SE1", "Sweden North (SE1)", "SE", "SEK"}, {"SE2", "Sweden North-Central (SE2)", "SE", "SEK"},
    {"SE3", "Sweden South-Central (SE3)", "SE", "SEK"}, {"SE4", "Sweden South (SE4)", "SE", "SEK"},
};
static const dp_zone_t NO_ZONES[] = {
    {"NO1", "Norway South-East (NO1)", "NO", "NOK"}, {"NO2", "Norway South-West (NO2)", "NO", "NOK"},
    {"NO3", "Norway Central (NO3)", "NO", "NOK"}, {"NO4", "Norway North (NO4)", "NO", "NOK"},
    {"NO5", "Norway West (NO5)", "NO", "NOK"},
};
static const dp_zone_t EZ_ZONES[] = { {"NL", "Netherlands", "NL", "EUR"} };
/* Octopus Agile: GSP regions (the tariff code's last letter). */
static const dp_zone_t OC_ZONES[] = {
    {"A", "Eastern England", "GB", "GBP"}, {"B", "East Midlands", "GB", "GBP"}, {"C", "London", "GB", "GBP"},
    {"D", "Merseyside and North Wales", "GB", "GBP"}, {"E", "West Midlands", "GB", "GBP"},
    {"F", "North East England", "GB", "GBP"}, {"G", "North West England", "GB", "GBP"},
    {"H", "Southern England", "GB", "GBP"}, {"J", "South East England", "GB", "GBP"},
    {"K", "South Wales", "GB", "GBP"}, {"L", "South West England", "GB", "GBP"}, {"M", "Yorkshire", "GB", "GBP"},
    {"N", "South Scotland", "GB", "GBP"}, {"P", "North Scotland", "GB", "GBP"},
};
#define NZ(a) (sizeof(a) / sizeof(a[0]))

static const dp_provider_t PROVIDERS[] = {
    {"energy_charts", "Energy-Charts (Fraunhofer ISE)", CLS_SPOT, false, false, 900, EC_ZONES, EC_NZONES,
     "https://api.energy-charts.info/", "Bundesnetzagentur | SMARD.de / Energy-Charts (CC BY 4.0)"},
    {"nordpool", "Nord Pool (unofficial data portal)", CLS_SPOT, false, false, 900, NP_ZONES, NZ(NP_ZONES),
     "https://www.nordpoolgroup.com/", "Day-ahead prices: Nord Pool (unofficial endpoint)"},
    {"elprisetjustnu", "Elpriset just nu (SE)", CLS_SPOT, false, false, 900, SE_ZONES, NZ(SE_ZONES),
     "https://www.elprisetjustnu.se/elpris-api", "Elpriser tillhandahålls av Elpriset just nu.se"},
    {"hvakosterstrommen", "Hva koster strømmen (NO, hourly)", CLS_SPOT, false, false, 3600, NO_ZONES, NZ(NO_ZONES),
     "https://www.hvakosterstrommen.no/strompris-api", "Strømpriser levert av Hva koster strømmen.no"},
    {"energyzero", "EnergyZero (NL, all-in)", CLS_ALL_IN, false, false, 900, EZ_ZONES, NZ(EZ_ZONES),
     "https://www.energyzero.nl/", "Prices: EnergyZero (all-in incl. VAT)"},
    {"octopus", "Octopus Agile (UK, all-in)", CLS_ALL_IN, false, false, 1800, OC_ZONES, NZ(OC_ZONES),
     "https://octopus.energy/", "Prices: Octopus Energy Agile (incl. VAT)"},
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

#define DP_CFG_VER 3
typedef struct {
    uint8_t ver;
    bool    enabled;
    char    source[24];           /* provider id */
    char    backup[24];           /* provider id or "none" */
    char    zone[12];             /* zone of the network provider(s) */
    uint8_t ha_class;             /* CLS_SPOT / CLS_ALL_IN */
    float   factor, markup;       /* spot → energy rate */
    bool    zones_stack;          /* add the plan zone's rate to the spot (TOU grid fees) */
    bool    hourly_mean;          /* bill the hourly mean of the 15-min prices */
    float   floor_buy, floor_sell;/* NAN = none */
    uint8_t sell_mode;            /* SELL_* */
    float   k_sell, feedin_fee;   /* formula sell = raw × k_sell − feedin_fee */
    char    product[32];          /* Octopus import product code */
    char    export_product[32];   /* Octopus export product code ("" = none) */
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
    strcpy(c->product, "AGILE-24-10-01");
    strcpy(c->export_product, "AGILE-OUTGOING-19-05-13");
}
static void cfg_get(dp_cfg_t *out)
{
    portENTER_CRITICAL(&s_cfg_mux); *out = s_cfg; portEXIT_CRITICAL(&s_cfg_mux);
    if (out->ver != DP_CFG_VER) cfg_defaults(out);       /* before rbdynprice_init (a first boot formats storage first) */
}

/* ============================================== stores ============================================== */

typedef struct { uint32_t from, until; float price, sell; } dp_entry_t;
typedef struct { dp_entry_t *e; int n; char provider[24]; char zone[12]; bool dirty; uint32_t gen; } dp_store_t;
/* gen: bumped whenever the store is handed to another provider / zone / product; a fetch started under an older
 * gen must not write (a slow fetch across a config change). */

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

static void plan_fees_of(const struct rbpower_tariff_plan_s *pl, float *fees, float *vat)
{
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
    return rbdynprice_resolve_plan(ts, zone_buy, zone_sell, rbpower_tariff_get_current_plan(), out);
}

bool rbdynprice_resolve_plan(uint32_t ts, float zone_buy, float zone_sell, const struct rbpower_tariff_plan_s *plan,
                             rbdp_price_t *out)
{
    dp_cfg_t c; cfg_get(&c);
    float fees, vat; plan_fees_of(plan, &fees, &vat);
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
        if (st != ST_HA && pv) {                       /* the plan's currency may have changed since the config was set */
            const dp_zone_t *zz = zone_of(pv, c.zone);
            const char *cur = rbpower_tariff_currency();
            if (!zz || (cur && cur[0] && strcmp(cur, zz->currency))) continue;
        }
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
#define DP_ST_MAGIC 0x50445232u   /* "2RDP" */
typedef struct { uint32_t magic; char provider[24]; char zone[12]; uint16_t n; } dp_st_hdr_t;

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
        c.product[sizeof(c.product) - 1] = 0; c.export_product[sizeof(c.export_product) - 1] = 0;
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
        /* a file, not an NVS blob: the stores are ~3 KB each and rewritten on every fetch; in the standalone
         * firmware's 24 KB NVS they starved the hour persistence and the energy anchors (1.2.0, .139) */
        char path[16]; snprintf(path, sizeof(path), "/dp_st%d", st);
        littlefs_raw_write(path, buf, sizeof(*hd) + hd->n * sizeof(dp_entry_t));
        free(buf);
    }
}

/* Apply one stored blob to store st (len checked). */
static bool st_apply(int st, const uint8_t *buf, size_t len)
{
    const dp_st_hdr_t *hd = (const dp_st_hdr_t *)buf;
    if (len < sizeof(*hd) || hd->magic != DP_ST_MAGIC || hd->n > RBDP_STORE_MAX ||
        len != sizeof(*hd) + hd->n * sizeof(dp_entry_t)) return false;
    portENTER_CRITICAL(&s_st_mux);
    memcpy(s_st[st].provider, hd->provider, sizeof(s_st[st].provider));
    memcpy(s_st[st].zone, hd->zone, sizeof(s_st[st].zone));
    s_st[st].provider[23] = 0; s_st[st].zone[11] = 0;
    memcpy(s_st[st].e, buf + sizeof(*hd), hd->n * sizeof(dp_entry_t));
    s_st[st].n = hd->n;
    portEXIT_CRITICAL(&s_st_mux);
    if (st == ST_HA && hd->n) s_ha_seen = true;
    return true;
}

static void st_load_all(void)
{
    size_t sz = sizeof(dp_st_hdr_t) + RBDP_STORE_MAX * sizeof(dp_entry_t);
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(sz);
    if (!buf) return;
    nvs_handle_t h; bool nvs_ok = rbgrid_nvs_open(DP_NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    bool erased = false;
    for (int st = 0; st < ST_N; st++) {
        char path[16]; snprintf(path, sizeof(path), "/dp_st%d", st);
        size_t got = 0;
        bool from_file = littlefs_raw_exists(path) && littlefs_raw_read(path, buf, sz, &got) == ESP_OK && st_apply(st, buf, got);
        if (!nvs_ok) continue;
        char key[6]; snprintf(key, sizeof(key), "st%d", st);
        size_t len = sz;
        if (nvs_get_blob(h, key, buf, &len) == ESP_OK) {      /* a 1.2.0 store in NVS: move it out, free the space */
            if (!from_file && st_apply(st, buf, len)) littlefs_raw_write(path, buf, len);
            nvs_erase_key(h, key); erased = true;
        }
    }
    if (nvs_ok) { if (erased) nvs_commit(h); nvs_close(h); }
    if (erased) rbpower_diag_printf("price: stores moved from NVS to the file system (NVS space freed)");
    free(buf);
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

/* ============================================= price log ============================================ */
/* The price the hub applied to each closed 15-min period (system-wide): one file per UTC day, /plog/<day number>,
 * 12 B per period, rewritten whole at each boundary (≤ 1.2 KB). 40 days kept (a monthly bill + margin). */

#define PLOG_DIR   "/plog"
#define PLOG_DAYS  40
#define PLOG_SLOTS 100            /* 96 + DST days */
typedef struct { uint16_t slot; uint8_t prov; uint8_t flags; float buy, sell; } plog_rec_t;   /* flags bit0 hourly_mean */
static plog_rec_t s_plog[PLOG_SLOTS];
static int s_plog_n = 0;
static uint32_t s_plog_day = 0;   /* UTC day number of s_plog[] */
static uint32_t s_plog_last_b = 0;

static void plog_path(uint32_t day, char *out, size_t n) { snprintf(out, n, PLOG_DIR "/%lu", (unsigned long)day); }

static void plog_load_day(uint32_t day)
{
    char path[32]; plog_path(day, path, sizeof(path));
    size_t got = 0;
    s_plog_n = 0;
    if (littlefs_raw_exists(path) && littlefs_raw_read(path, s_plog, sizeof(s_plog), &got) == ESP_OK)
        s_plog_n = (int)(got / sizeof(plog_rec_t));
    s_plog_day = day;
}

/* Log the 15-min period that closed at boundary b. */
static void plog_record(uint32_t b)
{
    if (!littlefs_raw_is_mounted()) return;
    uint32_t from = b - 900, day = from / 86400;
    if (day != s_plog_day) {
        littlefs_raw_mkdir(PLOG_DIR);
        plog_load_day(day);
        for (uint32_t d = day - PLOG_DAYS - 5; d <= day - PLOG_DAYS; d++) {   /* retire old days (a few, in case the hub was off) */
            char old[32]; plog_path(d, old, sizeof(old));
            if (littlefs_raw_exists(old)) littlefs_raw_remove(old);
        }
    }
    float zb, zs;
    rbpower_tariff_zone_rates_at(from + 450, &zb, &zs);
    rbdp_price_t pr;
    rbdynprice_resolve(from + 450, zb, zs, &pr);
    plog_rec_t r = { (uint16_t)((from % 86400) / 900), (uint8_t)pr.prov, (uint8_t)(pr.hourly_mean ? 1 : 0), pr.buy, pr.sell };
    int i;
    for (i = 0; i < s_plog_n && s_plog[i].slot != r.slot; i++) {}
    if (i == s_plog_n) { if (s_plog_n >= PLOG_SLOTS) return; s_plog_n++; }
    s_plog[i] = r;
    char path[32]; plog_path(day, path, sizeof(path));
    littlefs_raw_write(path, s_plog, s_plog_n * sizeof(plog_rec_t));
}

static void plog_tick(uint32_t now)
{
    uint32_t b = now - now % 900;
    if (!s_plog_last_b) { s_plog_last_b = b; return; }        /* boot: the period in progress is not logged */
    if (b != s_plog_last_b && now - b >= 5) { plog_record(b); s_plog_last_b = b; }
}

cJSON *rbdynprice_price_log_json(uint32_t from, uint32_t until)
{
    uint32_t now = (uint32_t)time(NULL);
    if (!until) until = now;
    if (!from) from = until > 86400 ? until - 86400 : 0;
    if (until > from && until - from > (uint32_t)PLOG_DAYS * 86400u) from = until - (uint32_t)PLOG_DAYS * 86400u;   /* kept span only */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "price_log");
    cJSON_AddStringToObject(o, "currency", rbpower_tariff_currency());
    cJSON *cols = cJSON_AddArrayToObject(o, "cols");
    const char *cn[] = {"from", "buy", "sell", "prov"};
    for (int k = 0; k < 4; k++) cJSON_AddItemToArray(cols, cJSON_CreateString(cn[k]));
    cJSON *rows = cJSON_AddArrayToObject(o, "rows");
    plog_rec_t *buf = malloc(sizeof(s_plog));
    int count = 0; bool more = false;
    for (uint32_t day = from / 86400; buf && day <= until / 86400 && !more; day++) {
        char path[32]; plog_path(day, path, sizeof(path));
        size_t got = 0;
        if (!littlefs_raw_exists(path) || littlefs_raw_read(path, buf, sizeof(s_plog), &got) != ESP_OK) continue;
        int n = (int)(got / sizeof(plog_rec_t));
        /* files hold slots in write order: emit sorted by slot */
        for (int slot = 0; slot < PLOG_SLOTS && !more; slot++) {
            for (int i = 0; i < n; i++) {
                if (buf[i].slot != slot) continue;
                uint32_t f = day * 86400 + slot * 900;
                if (f < from || f >= until) break;
                if (count >= 400) { more = true; break; }
                cJSON *r = cJSON_CreateArray();
                cJSON_AddItemToArray(r, cJSON_CreateNumber(f));
                cJSON_AddItemToArray(r, cJSON_CreateNumber(round((double)buf[i].buy * 1e6) / 1e6));
                cJSON_AddItemToArray(r, cJSON_CreateNumber(round((double)buf[i].sell * 1e6) / 1e6));
                char pc[24]; snprintf(pc, sizeof(pc), "%s%s", rbdynprice_prov_code((rbdp_prov_t)buf[i].prov),
                                      (buf[i].flags & 1) ? "+hourly_mean" : "");
                cJSON_AddItemToArray(r, cJSON_CreateString(pc));
                cJSON_AddItemToArray(rows, r);
                count++;
                break;
            }
        }
    }
    free(buf);
    cJSON_AddBoolToObject(o, "more", more);
    return o;
}

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
    esp_err_t oe = esp_http_client_open(h, 0);
    if (oe != ESP_OK) {   /* the reason goes to the event log (TLS needs ~40 KB of internal RAM on the classic ESP32) */
        rbpower_diag_printf("price: https open failed: %s (internal free %u, largest %u)", esp_err_to_name(oe),
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (oe == ESP_OK) {
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
static char s_fetch_prov[24], s_fetch_zone[12];
static uint32_t s_fetch_gen[ST_N];
static int s_fetch_st = -1;
static bool store_is_for_fetch_locked(int st)
{
    return st == s_fetch_st && s_st[st].gen == s_fetch_gen[st] &&
           !strcmp(s_st[st].provider, s_fetch_prov) && !strcmp(s_st[st].zone, s_fetch_zone);
}

static void put_entry(int st, uint32_t from, uint32_t until, float price, int *stored);
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
    /* walk both arrays once (cJSON_GetArrayItem is O(i)), then store in short critical sections via put_entry */
    const cJSON *a = ts->child, *p = pr->child;
    uint32_t prev_from = 0;
    for (; a && p; a = a->next, p = p->next) {
        if (!cJSON_IsNumber(a) || !cJSON_IsNumber(p)) continue;      /* null = no price for the slot */
        uint32_t from = (uint32_t)a->valuedouble, until;
        const cJSON *nx = a->next;
        if (nx && cJSON_IsNumber(nx) && (uint32_t)nx->valuedouble > from && (uint32_t)nx->valuedouble - from <= 3600)
            until = (uint32_t)nx->valuedouble;
        else if (prev_from && from > prev_from && from - prev_from <= 3600) until = from + (from - prev_from);
        else until = from + 900;
        prev_from = from;
        put_entry(st, from, until, (float)(p->valuedouble / div), stored);
    }
    cJSON_Delete(j);
    return *stored ? NULL : "no_data_for_zone";
}

/* ---- helpers for the date-based providers ---- */

static int32_t dp_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}
static void dp_civil_from_days(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)yoe + era * 400 + (*m <= 2);
}

/* "YYYY-MM-DDTHH:MM[:SS[.fff]](Z|±HH:MM)" → UTC s; 0 on a bad string. */
static uint32_t parse_iso(const char *s)
{
    int Y, M, D, h, mi, sec = 0, n = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d%n", &Y, &M, &D, &h, &mi, &n) != 5) return 0;
    const char *p = s + n;
    if (*p == ':') { int k = 0; if (sscanf(p, ":%2d%n", &sec, &k) != 1) return 0; p += k; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    long off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) return 0;
        off = (oh * 3600L + om * 60L) * (*p == '-' ? -1 : 1);
    } else if (*p != 'Z' && *p != 0) return 0;
    int64_t t = (int64_t)dp_days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec - off;
    return t > 0 ? (uint32_t)t : 0;
}

/* Market day (CET date) for ts + k days. */
static void market_date(uint32_t ts, int k, int *y, int *m, int *d)
{
    dp_civil_from_days((int32_t)((ts + 3600) / 86400) + k, y, m, d);
}

static char *dp_buf(int cap)
{
    char *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return b ? b : malloc(cap);
}

/* GET and parse JSON. *status = HTTP status; returns NULL on transport/parse failure (or non-200). */
static cJSON *get_json(const char *url, int cap, int *status, const char **err)
{
    char *buf = dp_buf(cap);
    if (!buf) { *err = "fetch_failed"; return NULL; }
    int n = http_get(url, buf, cap, status);
    cJSON *j = NULL;
    if (n < 0) *err = "fetch_failed";
    else if (*status == 429) *err = "rate_limited";
    else if (*status == 401 || *status == 403) *err = "auth";
    else if (*status == 204 || *status == 404) *err = "no_data_for_zone";
    else if (*status != 200) *err = "http_status";
    else if (n >= cap - 1) *err = "parse";                   /* truncated */
    else if (!(j = cJSON_ParseWithLength(buf, n))) *err = "parse";
    free(buf);
    return j;
}

/* The provider + zone the running fetch is for. A config change during a slow fetch hands the store to another
 * provider; the fetch's late entries must not land there. Written only by the fetch task. */

static void put_entry(int st, uint32_t from, uint32_t until, float price, int *stored)
{
    if (!from || until <= from || until - from > 3600 || isnan(price)) return;
    portENTER_CRITICAL(&s_st_mux);
    bool ok = store_is_for_fetch_locked(st);
    if (ok) st_put_locked(&s_st[st], from, until, price, NAN);
    portEXIT_CRITICAL(&s_st_mux);
    if (ok) (*stored)++;
}

/* Nord Pool data portal: {"multiAreaEntries":[{"deliveryStart","deliveryEnd","entryPerArea":{"SE3":1887.2}}]}, /MWh. */
static const char *fetch_nordpool(int st, const dp_zone_t *z, uint32_t now, int *stored)
{
    const char *last = "no_data_for_zone";
    for (int k = 0; k < 2; k++) {
        int y, m, d; market_date(now, k, &y, &m, &d);
        char url[200];
        snprintf(url, sizeof(url), "https://dataportal-api.nordpoolgroup.com/api/DayAheadPrices?date=%04d-%02d-%02d"
                 "&market=DayAhead&deliveryArea=%s&currency=%s", y, m, d, z->id, z->currency);
        int status; const char *err = NULL;
        cJSON *j = get_json(url, 32 * 1024, &status, &err);
        if (!j) { if (k == 0) last = err; continue; }
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "multiAreaEntries")) {
            const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(e, "entryPerArea"), z->id);
            if (!cJSON_IsNumber(v)) continue;
            put_entry(st, parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "deliveryStart"))),
                      parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "deliveryEnd"))),
                      (float)(v->valuedouble / 1000.0), stored);
        }
        cJSON_Delete(j);
    }
    return *stored ? NULL : last;
}

/* elprisetjustnu.se / hvakosterstrommen.no: one static file per day and zone,
 * [{"<CUR>_per_kWh":…,"time_start":"…+01:00","time_end":"…"}], excl. VAT. */
static const char *fetch_daily_file(int st, const dp_zone_t *z, const char *host, uint32_t now, int *stored)
{
    const char *last = "no_data_for_zone";
    char key[16]; snprintf(key, sizeof(key), "%s_per_kWh", z->currency);
    for (int k = 0; k < 2; k++) {
        int y, m, d; market_date(now, k, &y, &m, &d);
        char url[160];
        snprintf(url, sizeof(url), "https://%s/api/v1/prices/%04d/%02d-%02d_%s.json", host, y, m, d, z->id);
        int status; const char *err = NULL;
        cJSON *j = get_json(url, 24 * 1024, &status, &err);
        if (!j) { if (k == 0) last = err; continue; }
        const cJSON *e;
        cJSON_ArrayForEach(e, j) {
            const cJSON *v = cJSON_GetObjectItem(e, key);
            if (!cJSON_IsNumber(v)) continue;
            put_entry(st, parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "time_start"))),
                      parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "time_end"))), (float)v->valuedouble, stored);
        }
        cJSON_Delete(j);
    }
    return *stored ? NULL : last;
}

/* EnergyZero public API: {"all_in_with_vat":[{"start","end","price":{"value":"0.25"}}]} — the all-in user price. */
static const char *fetch_energyzero(int st, uint32_t now, int *stored)
{
    /* A date's answer covers the day before it too (~94 KB for two days): ask for tomorrow first — it brings today
     * along — and for today only when tomorrow is not published yet (404). Today's answer after publication spans
     * three days (~140 KB), so it is not asked for then. */
    const char *last = "no_data_for_zone";
    for (int k = 1; k >= 0; k--) {
        int y, m, d; market_date(now, k, &y, &m, &d);
        char url[200];
        snprintf(url, sizeof(url), "https://public.api.energyzero.nl/public/v1/prices?date=%02d-%02d-%04d"
                 "&interval=INTERVAL_QUARTER&energyType=ENERGY_TYPE_ELECTRICITY", d, m, y);
        int status; const char *err = NULL;
        cJSON *j = get_json(url, 256 * 1024, &status, &err);
        if (!j) { last = err; continue; }
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "all_in_with_vat")) {
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "price"), "value"));
            char *endp = NULL;
            float pv = v ? strtof(v, &endp) : NAN;
            if (!v || endp == v || isnan(pv)) continue;   /* "" / "n/a" is no price, not 0.00 */
            put_entry(st, parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "start"))),
                      parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "end"))), pv, stored);
        }
        cJSON_Delete(j);
        if (*stored) break;                              /* tomorrow's answer had today too */
    }
    return *stored ? NULL : last;
}

/* Octopus: {"results":[{"value_inc_vat":35.1,"valid_from","valid_to"}]} in p/kWh incl. VAT; the export product's
 * rates become the entries' sell. */
static const char *fetch_octopus(int st, const dp_zone_t *z, const char *product, const char *export_product,
                                 uint32_t now, int *stored)
{
    uint32_t from = now - now % 86400 - 3600, to = from + 3 * 86400;
    int y1, m1, d1, y2, m2, d2;
    dp_civil_from_days((int32_t)(from / 86400), &y1, &m1, &d1);
    dp_civil_from_days((int32_t)(to / 86400), &y2, &m2, &d2);
    const char *last = "no_data_for_zone";
    for (int pass = 0; pass < 2; pass++) {
        const char *prod = pass ? export_product : product;
        if (!prod || !prod[0]) continue;
        char url[300];
        snprintf(url, sizeof(url), "https://api.octopus.energy/v1/products/%s/electricity-tariffs/E-1R-%s-%s/"
                 "standard-unit-rates/?period_from=%04d-%02d-%02dT00:00Z&period_to=%04d-%02d-%02dT00:00Z&page_size=200",
                 prod, prod, z->id, y1, m1, d1, y2, m2, d2);
        int status; const char *err = NULL;
        cJSON *j = get_json(url, 48 * 1024, &status, &err);
        if (!j) { if (!pass) last = err; continue; }
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "results")) {
            const cJSON *v = cJSON_GetObjectItem(e, "value_inc_vat");
            uint32_t f = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "valid_from")));
            uint32_t u = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(e, "valid_to")));
            if (!cJSON_IsNumber(v) || !f) continue;
            if (!pass) put_entry(st, f, u, (float)(v->valuedouble / 100.0), stored);
            else {                                           /* export rate → sell of the matching entry */
                portENTER_CRITICAL(&s_st_mux);
                for (int i = 0; store_is_for_fetch_locked(st) && i < s_st[st].n; i++)
                    if (s_st[st].e[i].from == f) { s_st[st].e[i].sell = (float)(v->valuedouble / 100.0); s_st[st].dirty = true; }
                portEXIT_CRITICAL(&s_st_mux);
            }
        }
        cJSON_Delete(j);
    }
    return *stored ? NULL : last;
}

/* Fetch the network store st now for provider/zone. Returns the error code or NULL. */
static const char *fetch_store(int st, const char *provider, const char *zone)
{
    uint32_t now = (uint32_t)time(NULL);
    uint32_t start = now - now % 86400 - 3600;          /* from yesterday 23:00 UTC = today 00:00 CET */
    uint32_t end   = start + 3 * 86400;
    int stored = 0;
    const char *err = "no_data_for_zone";
    const dp_provider_t *pv = provider_by_id(provider);
    const dp_zone_t *z = zone_of(pv, zone);
    dp_cfg_t c; cfg_get(&c);
    portENTER_CRITICAL(&s_st_mux);
    snprintf(s_fetch_prov, sizeof(s_fetch_prov), "%s", provider);
    snprintf(s_fetch_zone, sizeof(s_fetch_zone), "%s", zone);
    s_fetch_st = st; s_fetch_gen[st] = s_st[st].gen;
    portEXIT_CRITICAL(&s_st_mux);
    if (!z) err = "no_data_for_zone";
    else if (!strcmp(provider, "energy_charts"))     err = fetch_energy_charts(st, zone, start, end, &stored);
    else if (!strcmp(provider, "nordpool"))          err = fetch_nordpool(st, z, now, &stored);
    else if (!strcmp(provider, "elprisetjustnu"))    err = fetch_daily_file(st, z, "www.elprisetjustnu.se", now, &stored);
    else if (!strcmp(provider, "hvakosterstrommen")) err = fetch_daily_file(st, z, "www.hvakosterstrommen.no", now, &stored);
    else if (!strcmp(provider, "energyzero"))        err = fetch_energyzero(st, now, &stored);
    else if (!strcmp(provider, "octopus"))           err = fetch_octopus(st, z, c.product, c.export_product, now, &stored);
    s_last_fetch = now;
    if (!err) {
        rbpower_diag_printf("price: %s %s stored %d periods (valid until %lu)", provider, zone, stored,
                            (unsigned long)rbdynprice_valid_until());
        s_gen++;
    }
    return err;
}

static volatile bool s_refetch = true;
static TaskHandle_t s_fetch_task_h = NULL;
void rbdynprice_kick(void) { s_refetch = true; if (s_fetch_task_h) xTaskNotifyGive(s_fetch_task_h); }
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
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_refetch ? 2000 : 30000));   /* a kick ends the wait at once */
        uint32_t now = (uint32_t)time(NULL);
        if (now - last_save >= 60) { st_save_dirty(); last_save = now; }
        if (!clock_ok()) continue;
        plog_tick(now);
        rbpeak_tick(now);
        rbact_tick(now);
        {   /* one event line per state change (reaches the event log, HA events, the web app's log view) */
            static char prev_state[16] = "";
            const char *st_now = rbdynprice_state();
            if (strcmp(prev_state, st_now)) {
                bool bk = false;
                const char *src = rbdynprice_source_now(&bk);
                if (prev_state[0])
                    rbpower_diag_printf("price: state %s -> %s (source %s%s, valid until %lu)", prev_state, st_now, src,
                                        bk ? " backup" : "", (unsigned long)rbdynprice_valid_until());
                snprintf(prev_state, sizeof(prev_state), "%s", st_now);
                s_gen++;
            }
        }
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
    rbpeak_init();
    rbact_init();
    /* a store saved for another provider/zone than the config now names is stale */
    dp_cfg_t c; cfg_get(&c);
    if (strcmp(s_st[ST_NET_P].provider, c.source) || strcmp(s_st[ST_NET_P].zone, c.zone)) st_clear(ST_NET_P);
    if (strcmp(s_st[ST_NET_B].provider, c.backup) || strcmp(s_st[ST_NET_B].zone, c.zone)) st_clear(ST_NET_B);
    strncpy(s_st[ST_NET_P].provider, c.source, 23); strncpy(s_st[ST_NET_P].zone, c.zone, 11);
    strncpy(s_st[ST_NET_B].provider, c.backup, 23); strncpy(s_st[ST_NET_B].zone, c.zone, 11);
    /* static internal stack: the task writes the price log (flash), and a heap stack may not be available at boot */
    static StackType_t s_fetch_stack[8192];
    static StaticTask_t s_fetch_tcb;
    if (!s_fetch_task_h) s_fetch_task_h = xTaskCreateStatic(fetch_task, "rbdynprice", sizeof(s_fetch_stack), NULL, 3, s_fetch_stack, &s_fetch_tcb);
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
    cJSON_AddStringToObject(o, "product", c.product);
    cJSON_AddStringToObject(o, "export_product", c.export_product);
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
    if ((v = cJSON_GetObjectItem(d, "product"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.product) || strpbrk(v->valuestring, "/?&# "))
            REJECT("product", "product: a tariff product code");
        snprintf(c.product, sizeof(c.product), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(d, "export_product"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.export_product) || strpbrk(v->valuestring, "/?&# "))
            REJECT("export_product", "export_product: a product code or \"\"");
        snprintf(c.export_product, sizeof(c.export_product), "%s", v->valuestring);
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
    bool prod_changed = strcmp(old.product, c.product) || strcmp(old.export_product, c.export_product);
    bool chg_p = strcmp(old.source, c.source) || strcmp(old.zone, c.zone) || prod_changed;
    bool chg_b = strcmp(old.backup, c.backup) || strcmp(old.zone, c.zone) || prod_changed;
    portENTER_CRITICAL(&s_st_mux);                   /* clear + hand over in one step: no fetch can land in between */
    if (chg_p) { s_st[ST_NET_P].n = 0; s_st[ST_NET_P].dirty = true; s_st[ST_NET_P].gen++; }
    if (chg_b) { s_st[ST_NET_B].n = 0; s_st[ST_NET_B].dirty = true; s_st[ST_NET_B].gen++; }
    strncpy(s_st[ST_NET_P].provider, c.source, 23); strncpy(s_st[ST_NET_P].zone, c.zone, 11);
    strncpy(s_st[ST_NET_B].provider, c.backup, 23); strncpy(s_st[ST_NET_B].zone, c.zone, 11);
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
