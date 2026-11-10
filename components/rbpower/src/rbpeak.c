/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpeak.c
 * @brief Import peak accumulators per billing cycle — see rbpeak.h.
 *
 * Runs in the booking path (rbpeak_add_period, under the home lock) and the dynprice task (rbpeak_tick); a
 * spinlock guards the state; NVS saves happen outside it.
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "nvs.h"
#include "rbpeak.h"
#include "rbgrid_core.h"   /* rbgrid_nvs_open */
#include "rbgrid_tz.h"     /* rbgrid_localtime_r */

#define PK_VER   2
#define PK_DAYS  31

typedef struct {
    uint32_t cycle_start;              /* local midnight that starts the cycle (UTC s; informative) */
    int32_t  cycle_key;                /* civil date of the cycle start, days since 1970 (DST-proof identity) */
    float    q_max_w;  uint32_t q_max_ts;   /* highest 15-min mean import, period end */
    float    h_max_w;  uint32_t h_max_ts;   /* highest hourly mean import, hour start */
    float    day_h_w[PK_DAYS];         /* highest hourly mean per day of the cycle */
    uint32_t day_h_ts[PK_DAYS];
} pk_cycle_t;

typedef struct {
    uint8_t    ver, cycle_day; uint16_t pad;
    pk_cycle_t cur, prev;
    uint32_t   hour_start; float hour_wh; uint8_t hour_n; uint8_t pad2[3];   /* the hour being summed */
    uint32_t   pend_q; float pend_wh;                                       /* the 15-min period being summed */
} pk_state_t;

static pk_state_t s;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty = false;

/* Days since 1970-01-01 for a civil date (H. Hinnant's algorithm). */
static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/* UTC instant of the local midnight that starts the billing cycle containing ts (cycle starts on cycle_day). A DST
 * change between that midnight and ts shifts the result by its hour; billing cycles are compared, not timed. */
static uint32_t cycle_start_of(uint32_t ts, uint8_t cycle_day)
{
    time_t t = (time_t)ts; struct tm tm;
    if (!rbgrid_localtime_r(&t, &tm)) return ts - ts % 86400;
    uint32_t midnight = ts - (uint32_t)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    if (tm.tm_mday < cycle_day) { if (--m == 0) { m = 12; y--; } }
    int32_t back = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) - days_from_civil(y, m, cycle_day);
    return midnight - (uint32_t)back * 86400u;
}

/* Civil date (days since 1970) of the cycle start for ts, and the local date of ts: compared as dates, so a DST
 * change inside the cycle doesn't make it look like a new cycle. */
static int32_t cycle_key_of(uint32_t ts, uint8_t cycle_day, int32_t *day_of_ts)
{
    time_t t = (time_t)ts; struct tm tm;
    if (!rbgrid_localtime_r(&t, &tm)) { if (day_of_ts) *day_of_ts = (int32_t)(ts / 86400); return (int32_t)(ts / 86400); }
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    int32_t today = days_from_civil(y, m, tm.tm_mday);
    if (day_of_ts) *day_of_ts = today;
    if (tm.tm_mday < cycle_day) { if (--m == 0) { m = 12; y--; } }
    return days_from_civil(y, m, cycle_day);
}

static void cycle_reset(pk_cycle_t *c, uint32_t start, int32_t key)
{
    memset(c, 0, sizeof(*c)); c->cycle_start = start; c->cycle_key = key;
}

/* Close the hour being summed (caller holds s_mux). */
static void hour_close_locked(void)
{
    if (!s.hour_start || s.hour_n == 0) return;
    float w = s.hour_wh;                            /* Wh in one hour = mean W (missing periods count as booked) */
    if (w > s.cur.h_max_w) { s.cur.h_max_w = w; s.cur.h_max_ts = s.hour_start; }
    int32_t day = 0;
    (void)cycle_key_of(s.hour_start, s.cycle_day, &day);
    int32_t d = day - s.cur.cycle_key;                 /* day of the cycle by local date (DST-proof) */
    if (d >= 0 && d < PK_DAYS && w > s.cur.day_h_w[d]) { s.cur.day_h_w[d] = w; s.cur.day_h_ts[d] = s.hour_start; }
    s.hour_start = 0; s.hour_wh = 0; s.hour_n = 0;
}

/* Finalize the pending 15-min period (caller holds s_mux). */
static void period_close_locked(void)
{
    if (!s.pend_q) return;
    uint32_t q = s.pend_q; float wh = s.pend_wh;
    s.pend_q = 0; s.pend_wh = 0;
    uint32_t mid = q - 450;
    int32_t key = cycle_key_of(mid, s.cycle_day, NULL);
    if (key != s.cur.cycle_key || !s.cur.cycle_start) {   /* new billing cycle (by civil date) */
        hour_close_locked();
        if (s.cur.cycle_start) s.prev = s.cur;
        cycle_reset(&s.cur, cycle_start_of(mid, s.cycle_day), key);
    }
    float w = wh * 4.0f;                            /* 15-min mean W */
    if (w > s.cur.q_max_w) { s.cur.q_max_w = w; s.cur.q_max_ts = q; }
    uint32_t hs = mid - mid % 3600;
    if (hs != s.hour_start) { hour_close_locked(); s.hour_start = hs; }
    s.hour_wh += wh; s.hour_n++;
    if (s.hour_n >= 4) hour_close_locked();
    s_dirty = true;
}

static void save(void)
{
    if (!s_dirty) return;
    pk_state_t copy;
    portENTER_CRITICAL(&s_mux); copy = s; s_dirty = false; portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (rbgrid_nvs_open("rbpeak", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "st", &copy, sizeof(copy));
    nvs_commit(h); nvs_close(h);
}

void rbpeak_init(void)
{
    memset(&s, 0, sizeof(s));
    s.ver = PK_VER; s.cycle_day = 1;
    nvs_handle_t h;
    if (rbgrid_nvs_open("rbpeak", NVS_READONLY, &h) == ESP_OK) {
        pk_state_t t; size_t len = sizeof(t);
        if (nvs_get_blob(h, "st", &t, &len) == ESP_OK && len == sizeof(t) && t.ver == PK_VER && t.cycle_day >= 1 && t.cycle_day <= 28)
            s = t;
        nvs_close(h);
    }
}

void rbpeak_add_period(uint32_t q_end_ts, float import_wh)
{
    if (q_end_ts < 1600000000u || isnan(import_wh)) return;
    if (import_wh < 0) import_wh = 0;
    q_end_ts -= q_end_ts % 900;
    portENTER_CRITICAL(&s_mux);
    if (s.pend_q && q_end_ts != s.pend_q) {
        if (q_end_ts > s.pend_q) period_close_locked();
        else { portEXIT_CRITICAL(&s_mux); return; }  /* a late add for an already-closed period: ignored */
    }
    s.pend_q = q_end_ts; s.pend_wh += import_wh;
    portEXIT_CRITICAL(&s_mux);
}

void rbpeak_tick(uint32_t now)
{
    portENTER_CRITICAL(&s_mux);
    if (s.pend_q && now > s.pend_q + 120) period_close_locked();   /* every point has booked by then */
    portEXIT_CRITICAL(&s_mux);
    save();
}

static void add_cycle(cJSON *o, const pk_cycle_t *c)
{
    cJSON_AddNumberToObject(o, "cycle_start", c->cycle_start);
    cJSON_AddNumberToObject(o, "max_15min_w", round(c->q_max_w));
    cJSON_AddNumberToObject(o, "max_15min_end", c->q_max_ts);
    cJSON_AddNumberToObject(o, "max_hour_w", round(c->h_max_w));
    cJSON_AddNumberToObject(o, "max_hour_start", c->h_max_ts);
    /* the 3 highest daily hourly peaks (each on a different day) and their mean */
    int idx[3] = {-1, -1, -1};
    for (int d = 0; d < PK_DAYS; d++) {
        if (c->day_h_w[d] <= 0) continue;
        for (int k = 0; k < 3; k++) {
            if (idx[k] < 0 || c->day_h_w[d] > c->day_h_w[idx[k]]) {
                for (int m = 2; m > k; m--) idx[m] = idx[m - 1];
                idx[k] = d; break;
            }
        }
    }
    cJSON *a = cJSON_AddArrayToObject(o, "top_days");
    float sum = 0; int n = 0;
    for (int k = 0; k < 3; k++) {
        if (idx[k] < 0) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "hour_start", c->day_h_ts[idx[k]]);
        cJSON_AddNumberToObject(e, "w", round(c->day_h_w[idx[k]]));
        cJSON_AddItemToArray(a, e);
        sum += c->day_h_w[idx[k]]; n++;
    }
    cJSON_AddNumberToObject(o, "top3_days_mean_w", n ? round(sum / n) : 0);
}

cJSON *rbpeak_json(void)
{
    pk_state_t c;
    portENTER_CRITICAL(&s_mux); c = s; portEXIT_CRITICAL(&s_mux);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "peaks");
    cJSON_AddNumberToObject(o, "cycle_day", c.cycle_day);
    add_cycle(o, &c.cur);
    if (c.hour_start) {
        cJSON_AddNumberToObject(o, "hour_start", c.hour_start);
        cJSON_AddNumberToObject(o, "hour_wh_so_far", round(c.hour_wh * 10) / 10);
    }
    if (c.prev.cycle_start) add_cycle(cJSON_AddObjectToObject(o, "prev"), &c.prev);
    return o;
}

esp_err_t rbpeak_set(const cJSON *d, char *err, size_t err_len)
{
    const cJSON *cd = cJSON_GetObjectItem(d, "cycle_day");
    if (cd && (!cJSON_IsNumber(cd) || cd->valueint < 1 || cd->valueint > 28)) {
        snprintf(err, err_len, "cycle_day 1..28"); return ESP_ERR_INVALID_ARG;
    }
    /* With the clock known the new cycle opens at once (its start shown right away); without it the next booked
     * period opens it. Local-time maths stays outside the spinlock. */
    const bool reset = cJSON_IsTrue(cJSON_GetObjectItem(d, "reset"));
    uint8_t new_day = cd ? (uint8_t)cd->valueint : s.cycle_day;
    uint32_t now = (uint32_t)time(NULL), start = 0; int32_t key = 0;
    if (now > 1600000000u) { start = cycle_start_of(now, new_day); key = cycle_key_of(now, new_day, NULL); }
    portENTER_CRITICAL(&s_mux);
    if (cd && new_day != s.cycle_day) {   /* the current figures become "prev" (an empty cycle doesn't replace them) */
        s.cycle_day = new_day;
        if (s.cur.cycle_start && (s.cur.q_max_ts || s.cur.h_max_ts)) s.prev = s.cur;
        cycle_reset(&s.cur, start, key);
    }
    if (reset) { cycle_reset(&s.cur, start, key); cycle_reset(&s.prev, 0, 0);
                 s.hour_start = 0; s.hour_wh = 0; s.hour_n = 0; }
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
    save();
    return ESP_OK;
}
