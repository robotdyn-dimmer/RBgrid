/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbactivation.c
 * @brief Cheap windows + price_signal — see rbactivation.h.
 *
 * Evaluation runs in the dynprice task every ~30 s (rbact_tick). A rule's window is recomputed from the resolved
 * prices each time, so tomorrow's prices arriving at ~13:00 CET move the windows by themselves. Prices not known
 * yet fall back to the plan zone (what the hub would book), so a window may move once they arrive.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "rbactivation.h"
#include "rbdynprice.h"
#include "rbgrid_core.h"      /* rbgrid_nvs_open */
#include "rbgrid_tz.h"        /* rbgrid_localtime_r */
#include "rbhome_tariff.h"    /* rbpower_tariff_zone_rates_at, rbpower_tariff_currency */
#include "rbhome_types.h"     /* rbpower_diag_printf */
#include "rbmqtt.h"           /* rbmqtt_publish_nb (inline stub when MQTT is off) */

enum { RT_THRESHOLD = 0, RT_CHEAPEST = 1, RT_DEADLINE = 2 };
static const char *const RT_NAMES[] = {"threshold", "cheapest", "deadline"};

typedef struct {
    uint8_t  id, type, enabled, consecutive;
    char     name[24];
    float    on_below, off_above;   /* threshold, all-in currency/kWh */
    uint8_t  from_h, to_h;          /* cheapest: daily local window [from_h, to_h); to_h ≤ from_h crosses midnight */
    uint8_t  deadline_h;            /* deadline: local hour */
    uint8_t  pad;
    uint16_t periods;               /* cheapest / deadline: number of 15-min periods */
} act_rule_t;

#define ACT_VER 1
typedef struct {
    uint8_t    ver, n; uint16_t pad;
    float      buy_cheap_below, sell_low_below;   /* signal flags; NAN = off */
    act_rule_t r[RBACT_MAX_RULES];
} act_cfg_t;

typedef struct { bool active; uint32_t active_until, next_start, win_from, win_until; } act_state_t;

static act_cfg_t s_cfg;
static act_state_t s_st[RBACT_MAX_RULES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------ prices ------------------------------------------------ */

static float buy_at(uint32_t period_from)
{
    float zb, zs; rbdp_price_t p;
    rbpower_tariff_zone_rates_at(period_from + 450, &zb, &zs);
    rbdynprice_resolve(period_from + 450, zb, zs, &p);
    return p.buy;
}
static float sell_at(uint32_t period_from)
{
    float zb, zs; rbdp_price_t p;
    rbpower_tariff_zone_rates_at(period_from + 450, &zb, &zs);
    rbdynprice_resolve(period_from + 450, zb, zs, &p);
    return p.sell;
}

static uint32_t local_midnight(uint32_t ts)
{
    time_t t = (time_t)ts; struct tm tm;
    if (!rbgrid_localtime_r(&t, &tm)) return ts - ts % 86400;
    return ts - (uint32_t)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
}

int rbact_rank(uint32_t ts, bool sell, int *of_n)
{
    uint32_t d0 = local_midnight(ts), d1 = local_midnight(d0 + 86400 + 7200);   /* next local midnight (DST-safe) */
    uint32_t q = ts - ts % 900;
    float me = sell ? sell_at(q) : buy_at(q);
    int rank = 1, n = 0;
    for (uint32_t f = d0; f < d1 && n < 100; f += 900, n++) {
        float v = sell ? sell_at(f) : buy_at(f);
        if (v < me) rank++;
    }
    if (of_n) *of_n = n;
    return n ? rank : 0;
}

/* --------------------------------------------- rule windows --------------------------------------------- */

/* The window instance [S, E) of a daily window that contains now, else the next one. */
static void window_instance(uint32_t now, int from_h, int to_h, uint32_t *S, uint32_t *E)
{
    int len_h = to_h > from_h ? to_h - from_h : to_h + 24 - from_h;
    uint32_t m = local_midnight(now);
    for (int back = 1; back >= -1; back--) {              /* yesterday's (overnight), today's, tomorrow's */
        uint32_t s = m - back * 86400 + from_h * 3600, e = s + len_h * 3600;
        if (now < e) { *S = s; *E = e; if (now >= s || back <= 0) return; }
    }
}

/* Select the n cheapest periods of [S, E) (or the cheapest consecutive block). sel[] marks chosen periods. */
static int select_cheapest(uint32_t S, uint32_t E, int n, bool consecutive, uint8_t sel[], int max)
{
    int cnt = (int)((E - S) / 900);
    if (cnt > max) cnt = max;
    if (n > cnt) n = cnt;
    float price[100];
    for (int i = 0; i < cnt; i++) { price[i] = buy_at(S + i * 900); sel[i] = 0; }
    if (n <= 0) return cnt;
    if (consecutive) {
        int best = 0; float bs = INFINITY;
        for (int i = 0; i + n <= cnt; i++) {
            float sum = 0; for (int k = 0; k < n; k++) sum += price[i + k];
            if (sum < bs) { bs = sum; best = i; }
        }
        for (int k = 0; k < n; k++) sel[best + k] = 1;
    } else {
        for (int k = 0; k < n; k++) {                      /* n passes: the cheapest not yet chosen (earliest on a tie) */
            int b = -1;
            for (int i = 0; i < cnt; i++) if (!sel[i] && (b < 0 || price[i] < price[b])) b = i;
            if (b >= 0) sel[b] = 1;
        }
    }
    return cnt;
}

static void eval_rule(const act_rule_t *r, act_state_t *st, uint32_t now)
{
    uint32_t q = now - now % 900;
    st->active_until = 0; st->next_start = 0;
    if (!r->enabled) { st->active = false; st->win_from = st->win_until = 0; return; }
    if (r->type == RT_THRESHOLD) {
        float b = buy_at(q);
        if (st->active) st->active = b < r->off_above; else st->active = b <= r->on_below;
        /* look ahead (through the stored prices, ≤ 2 days) for the end of the run / the next start */
        bool act = st->active;
        uint32_t lim = q + 2 * 86400, vu = rbdynprice_valid_until();
        if (vu && vu < lim) lim = vu;
        for (uint32_t f = q + 900; f < lim; f += 900) {
            float v = buy_at(f);
            bool next = act ? v < r->off_above : v <= r->on_below;
            if (st->active && !next && !st->active_until) st->active_until = f;
            if (!act && next && !st->next_start) { st->next_start = f; }
            act = next;
            if ((st->active ? st->active_until : 1) && st->next_start) break;
        }
        if (st->active && !st->active_until) st->active_until = lim;
        st->win_from = st->win_until = 0;
        return;
    }
    int from_h = r->type == RT_DEADLINE ? r->deadline_h : r->from_h;
    int to_h   = r->type == RT_DEADLINE ? r->deadline_h : r->to_h;
    uint32_t S, E; window_instance(now, from_h, to_h, &S, &E);
    uint8_t sel[100];
    int cnt = select_cheapest(S, E, r->periods, r->consecutive, sel, 100);
    st->win_from = S; st->win_until = E;
    int cur = now >= S ? (int)((q - S) / 900) : -1;
    st->active = cur >= 0 && cur < cnt && sel[cur];
    if (st->active) {
        int k = cur; while (k < cnt && sel[k]) k++;
        st->active_until = S + k * 900;
    }
    int k = cur < 0 ? 0 : cur + 1;
    if (st->active) while (k < cnt && sel[k]) k++;      /* skip the rest of the current run */
    for (; k < cnt; k++) if (sel[k]) { st->next_start = S + k * 900; break; }
    if (!st->next_start && rbdynprice_valid_until() >= E + 86400) {   /* the next day's pick, only once its prices are in */
        uint32_t S2 = S + 86400, E2 = E + 86400;
        int c2 = select_cheapest(S2, E2, r->periods, r->consecutive, sel, 100);
        for (int k = 0; k < c2; k++) if (sel[k]) { st->next_start = S2 + k * 900; break; }
    }
}

/* ------------------------------------------------ signal ------------------------------------------------ */

cJSON *rbact_signal_json(void)
{
    uint32_t now = (uint32_t)time(NULL), q = now - now % 900;
    float zb, zs; rbdp_price_t p;
    rbpower_tariff_zone_rates_at(q + 450, &zb, &zs);
    rbdynprice_resolve(q + 450, zb, zs, &p);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddNumberToObject(o, "ts", now);
    cJSON_AddNumberToObject(o, "period_from", q);
    cJSON_AddNumberToObject(o, "period_until", q + 900);
    cJSON_AddStringToObject(o, "currency", rbpower_tariff_currency());
    cJSON_AddNumberToObject(o, "buy", round((double)p.buy * 1e5) / 1e5);
    cJSON_AddNumberToObject(o, "sell", round((double)p.sell * 1e5) / 1e5);
    cJSON_AddStringToObject(o, "price_state", rbdynprice_state());
    cJSON_AddStringToObject(o, "provenance", rbdynprice_prov_code(p.prov));
    int nb = 0, ns = 0;
    cJSON_AddNumberToObject(o, "rank_buy", rbact_rank(now, false, &nb));
    cJSON_AddNumberToObject(o, "rank_sell", rbact_rank(now, true, &ns));
    cJSON_AddNumberToObject(o, "periods_today", nb);
    cJSON *w = cJSON_AddArrayToObject(o, "windows");
    act_cfg_t c; act_state_t st[RBACT_MAX_RULES];
    portENTER_CRITICAL(&s_mux); c = s_cfg; memcpy(st, s_st, sizeof(st)); portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < c.n && i < 8; i++) {
        if (!c.r[i].enabled) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "rule_id", c.r[i].id);
        cJSON_AddStringToObject(e, "name", c.r[i].name);
        cJSON_AddBoolToObject(e, "active", st[i].active);
        cJSON_AddNumberToObject(e, "active_until", st[i].active ? st[i].active_until : 0);
        cJSON_AddNumberToObject(e, "next_start", st[i].next_start);
        cJSON_AddItemToArray(w, e);
    }
    cJSON_AddBoolToObject(o, "sell_low", !isnan(c.sell_low_below) && p.sell <= c.sell_low_below);
    cJSON_AddBoolToObject(o, "buy_cheap", !isnan(c.buy_cheap_below) && p.buy <= c.buy_cheap_below);
    return o;
}

static void publish_signal(void)
{
    static char topic[40] = "";
    if (!topic[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(topic, sizeof(topic), "rbgrid/%02x%02x%02x%02x%02x%02x/price", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    cJSON *o = rbact_signal_json();
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (s) { rbmqtt_publish_nb(topic, s, 0, true); free(s); }
}

/* ------------------------------------------------- tick ------------------------------------------------- */

void rbact_tick(uint32_t now)
{
    static uint32_t last_q = 0, last_gen = 0;
    static uint8_t prev_active[RBACT_MAX_RULES];
    act_cfg_t c;
    portENTER_CRITICAL(&s_mux); c = s_cfg; portEXIT_CRITICAL(&s_mux);
    act_state_t st[RBACT_MAX_RULES];
    portENTER_CRITICAL(&s_mux); memcpy(st, s_st, sizeof(st)); portEXIT_CRITICAL(&s_mux);
    bool changed = false;
    for (int i = 0; i < c.n; i++) {
        eval_rule(&c.r[i], &st[i], now);
        if ((uint8_t)st[i].active != prev_active[i]) {
            changed = true;
            prev_active[i] = st[i].active;
            if (st[i].active) rbpower_diag_printf("price: window '%s' on until %lu", c.r[i].name, (unsigned long)st[i].active_until);
            else rbpower_diag_printf("price: window '%s' off, next %lu", c.r[i].name, (unsigned long)st[i].next_start);
        }
    }
    portENTER_CRITICAL(&s_mux); memcpy(s_st, st, sizeof(st)); portEXIT_CRITICAL(&s_mux);
    uint32_t q = now - now % 900, gen = rbdynprice_generation();
    if (q != last_q || gen != last_gen || changed) { publish_signal(); last_q = q; last_gen = gen; }
}

/* ------------------------------------------------ config ------------------------------------------------ */

static void cfg_save(void)
{
    act_cfg_t c; portENTER_CRITICAL(&s_mux); c = s_cfg; portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (rbgrid_nvs_open("rbact", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &c, sizeof(c));
    nvs_commit(h); nvs_close(h);
}

void rbact_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.ver = ACT_VER; s_cfg.buy_cheap_below = NAN; s_cfg.sell_low_below = NAN;
    nvs_handle_t h;
    if (rbgrid_nvs_open("rbact", NVS_READONLY, &h) == ESP_OK) {
        act_cfg_t c; size_t len = sizeof(c);
        if (nvs_get_blob(h, "cfg", &c, &len) == ESP_OK && len == sizeof(c) && c.ver == ACT_VER && c.n <= RBACT_MAX_RULES) {
            for (int i = 0; i < c.n; i++) c.r[i].name[sizeof(c.r[i].name) - 1] = 0;
            s_cfg = c;
        }
        nvs_close(h);
    }
}

static void add_f(cJSON *o, const char *k, float v) { if (isnan(v)) cJSON_AddNullToObject(o, k); else cJSON_AddNumberToObject(o, k, round((double)v * 1e6) / 1e6); }

cJSON *rbact_rules_json(void)
{
    act_cfg_t c; act_state_t st[RBACT_MAX_RULES];
    portENTER_CRITICAL(&s_mux); c = s_cfg; memcpy(st, s_st, sizeof(st)); portEXIT_CRITICAL(&s_mux);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "rules");
    cJSON *sg = cJSON_AddObjectToObject(o, "signal");
    add_f(sg, "buy_cheap_below", c.buy_cheap_below);
    add_f(sg, "sell_low_below", c.sell_low_below);
    cJSON *a = cJSON_AddArrayToObject(o, "rules");
    for (int i = 0; i < c.n; i++) {
        const act_rule_t *r = &c.r[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", r->id);
        cJSON_AddStringToObject(e, "name", r->name);
        cJSON_AddStringToObject(e, "kind", RT_NAMES[r->type < 3 ? r->type : 0]);
        cJSON_AddBoolToObject(e, "enabled", r->enabled);
        if (r->type == RT_THRESHOLD) { add_f(e, "on_below", r->on_below); add_f(e, "off_above", r->off_above); }
        else {
            if (r->type == RT_CHEAPEST) { cJSON_AddNumberToObject(e, "from_h", r->from_h); cJSON_AddNumberToObject(e, "to_h", r->to_h); }
            else cJSON_AddNumberToObject(e, "deadline_h", r->deadline_h);
            cJSON_AddNumberToObject(e, "periods", r->periods);
            cJSON_AddBoolToObject(e, "consecutive", r->consecutive);
        }
        cJSON *s = cJSON_AddObjectToObject(e, "state");
        cJSON_AddBoolToObject(s, "active", st[i].active);
        cJSON_AddNumberToObject(s, "active_until", st[i].active ? st[i].active_until : 0);
        cJSON_AddNumberToObject(s, "next_start", st[i].next_start);
        if (st[i].win_until) { cJSON_AddNumberToObject(s, "window_from", st[i].win_from); cJSON_AddNumberToObject(s, "window_until", st[i].win_until); }
        cJSON_AddItemToArray(a, e);
    }
    return o;
}

esp_err_t rbact_set(const cJSON *d, char *ef, size_t efl, char *em, size_t eml)
{
    #define REJECT(f, ...) do { snprintf(ef, efl, "%s", f); snprintf(em, eml, __VA_ARGS__); return ESP_ERR_INVALID_ARG; } while (0)
    act_cfg_t c; portENTER_CRITICAL(&s_mux); c = s_cfg; portEXIT_CRITICAL(&s_mux);
    const cJSON *sg = cJSON_GetObjectItem(d, "signal");
    if (sg) {
        const char *keys[2] = {"buy_cheap_below", "sell_low_below"};
        float *dst[2] = {&c.buy_cheap_below, &c.sell_low_below};
        for (int k = 0; k < 2; k++) {
            const cJSON *v = cJSON_GetObjectItem(sg, keys[k]);
            if (!v) continue;
            if (cJSON_IsNull(v)) *dst[k] = NAN;
            else if (cJSON_IsNumber(v) && fabs(v->valuedouble) <= 100) *dst[k] = (float)v->valuedouble;
            else REJECT(keys[k], "%s: -100..100 or null", keys[k]);
        }
    }
    const cJSON *rs = cJSON_GetObjectItem(d, "rules");
    if (rs) {
        if (!cJSON_IsArray(rs) || cJSON_GetArraySize(rs) > RBACT_MAX_RULES) REJECT("rules", "rules: array of up to %d", RBACT_MAX_RULES);
        act_cfg_t n = c; n.n = 0; memset(n.r, 0, sizeof(n.r));
        const cJSON *e;
        cJSON_ArrayForEach(e, rs) {
            act_rule_t *r = &n.r[n.n];
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(e, "kind"));
            int t = -1;
            for (int k = 0; k < 3 && kind; k++) if (!strcmp(kind, RT_NAMES[k])) t = k;
            if (t < 0) REJECT("kind", "rule %d: kind threshold | cheapest | deadline", n.n);
            r->type = (uint8_t)t; r->id = (uint8_t)(n.n + 1);
            const cJSON *v = cJSON_GetObjectItem(e, "id");
            if (cJSON_IsNumber(v) && v->valueint > 0 && v->valueint < 256) r->id = (uint8_t)v->valueint;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
            snprintf(r->name, sizeof(r->name), "%s", name ? name : RT_NAMES[t]);
            r->enabled = !cJSON_IsFalse(cJSON_GetObjectItem(e, "enabled"));
            if (t == RT_THRESHOLD) {
                const cJSON *on = cJSON_GetObjectItem(e, "on_below"), *off = cJSON_GetObjectItem(e, "off_above");
                if (!cJSON_IsNumber(on)) REJECT("on_below", "rule %d: on_below (all-in currency/kWh)", n.n);
                r->on_below = (float)on->valuedouble;
                r->off_above = cJSON_IsNumber(off) ? (float)off->valuedouble : r->on_below;
                if (r->off_above < r->on_below) REJECT("off_above", "rule %d: off_above must be >= on_below", n.n);
            } else {
                const cJSON *p = cJSON_GetObjectItem(e, "periods");
                if (!cJSON_IsNumber(p) || p->valueint < 1 || p->valueint > 96) REJECT("periods", "rule %d: periods 1..96 (15-min each)", n.n);
                r->periods = (uint16_t)p->valueint;
                r->consecutive = cJSON_IsTrue(cJSON_GetObjectItem(e, "consecutive"));
                if (t == RT_CHEAPEST) {
                    const cJSON *f = cJSON_GetObjectItem(e, "from_h"), *to = cJSON_GetObjectItem(e, "to_h");
                    if (!cJSON_IsNumber(f) || !cJSON_IsNumber(to) || f->valueint < 0 || f->valueint > 23 || to->valueint < 0 || to->valueint > 24 ||
                        (f->valueint == to->valueint && to->valueint != 24))
                        REJECT("from_h", "rule %d: from_h 0..23, to_h 0..24 (from_h = to_h only as 0..24 = all day)", n.n);
                    r->from_h = (uint8_t)f->valueint; r->to_h = (uint8_t)(to->valueint % 24);
                    int len = r->to_h > r->from_h ? r->to_h - r->from_h : r->to_h + 24 - r->from_h;
                    if (r->periods > len * 4) REJECT("periods", "rule %d: more periods than the window holds (%d)", n.n, len * 4);
                } else {
                    const cJSON *dl = cJSON_GetObjectItem(e, "deadline_h");
                    if (!cJSON_IsNumber(dl) || dl->valueint < 0 || dl->valueint > 23) REJECT("deadline_h", "rule %d: deadline_h 0..23", n.n);
                    r->deadline_h = (uint8_t)dl->valueint;
                }
            }
            n.n++;
        }
        c = n;
    }
    portENTER_CRITICAL(&s_mux); s_cfg = c; memset(s_st, 0, sizeof(s_st)); portEXIT_CRITICAL(&s_mux);
    cfg_save();
    rbdynprice_kick();                                   /* the dynprice task re-evaluates within ~2 s */
    return ESP_OK;
    #undef REJECT
}
