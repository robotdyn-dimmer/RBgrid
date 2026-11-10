/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_tz.c
 * @brief RBgrid's own local time: a POSIX TZ rule evaluated without the C library's TZ / tzset().
 *
 * Tariff zones, day boundaries and the daily records follow the hub's configured time zone. With the C library
 * that zone lives in the process-wide TZ variable, which a host (ESPHome's `time:` component) may set to its own
 * value. This module keeps RBgrid's rule apart: rbgrid_tz_set() parses the configured POSIX string, and
 * rbgrid_localtime_r() / rbgrid_mktime() use it.
 *
 * Supported: std offset [dst [offset] [,start[/time],end[/time]]] with names in letters or <...>, offsets
 * [+|-]hh[:mm[:ss]], rules Mm.w.d, Jn and n, rule times [+|-]h[:mm[:ss]] (0..167 h). A DST name without rules uses
 * the US rule (M3.2.0,M11.1.0), as the C library does. Pure code, no locking: the rule is swapped in one
 * assignment.
 */
#include "rbgrid_tz.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/** @brief One transition rule. */
typedef struct {
    char kind;      ///< 'M' month.week.day, 'J' Julian day 1..365 (no Feb 29), 'D' zero-based day 0..365
    int  m, w, d;   ///< M: month 1..12, week 1..5 (5 = last), weekday 0..6 (Sunday = 0); J/D: day in d
    long secs;      ///< Local time of day of the change, s (default 7200)
} rbtz_rule_t;

/** @brief Parsed zone. */
typedef struct {
    long std_off;   ///< Seconds to ADD to UTC for standard time (POSIX sign inverted)
    long dst_off;   ///< Seconds to ADD to UTC for summer time
    int  has_dst;   ///< 1 if the zone has summer time
    rbtz_rule_t start, end;
} rbtz_t;

static rbtz_t s_tz;          /* UTC until set */
static int    s_tz_set = 0;

/* ---- parsing ---- */
static const char *parse_name(const char *p)
{
    if (*p == '<') {
        const char *q = strchr(p, '>');
        return (q && q - p >= 4) ? q + 1 : NULL;
    }
    const char *q = p;
    while (isalpha((unsigned char)*q)) q++;
    return (q - p >= 3) ? q : NULL;
}

/* [+|-]h[:mm[:ss]] → seconds; returns pointer after, or NULL */
static const char *parse_hms(const char *p, long *out, int max_h)
{
    int sign = 1;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) return NULL;
    long h = strtol(p, (char **)&p, 10), m = 0, s = 0;
    if (h > max_h) return NULL;
    if (*p == ':') {
        p++; if (!isdigit((unsigned char)*p)) return NULL;
        m = strtol(p, (char **)&p, 10); if (m > 59) return NULL;
        if (*p == ':') {
            p++; if (!isdigit((unsigned char)*p)) return NULL;
            s = strtol(p, (char **)&p, 10); if (s > 59) return NULL;
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return p;
}

static const char *parse_rule(const char *p, rbtz_rule_t *r)
{
    memset(r, 0, sizeof(*r));
    r->secs = 7200;
    if (*p == 'M') {
        r->kind = 'M'; p++;
        if (!isdigit((unsigned char)*p)) return NULL;
        r->m = (int)strtol(p, (char **)&p, 10); if (*p++ != '.') return NULL;
        if (!isdigit((unsigned char)*p)) return NULL;
        r->w = (int)strtol(p, (char **)&p, 10); if (*p++ != '.') return NULL;
        if (!isdigit((unsigned char)*p)) return NULL;
        r->d = (int)strtol(p, (char **)&p, 10);
        if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 || r->d > 6) return NULL;
    } else if (*p == 'J') {
        r->kind = 'J'; p++;
        if (!isdigit((unsigned char)*p)) return NULL;
        r->d = (int)strtol(p, (char **)&p, 10);
        if (r->d < 1 || r->d > 365) return NULL;
    } else if (isdigit((unsigned char)*p)) {
        r->kind = 'D';
        r->d = (int)strtol(p, (char **)&p, 10);
        if (r->d < 0 || r->d > 365) return NULL;
    } else {
        return NULL;
    }
    if (*p == '/') {
        p = parse_hms(p + 1, &r->secs, 167);
        if (!p) return NULL;
    }
    return p;
}

bool rbgrid_tz_parse_ok(const char *posix)
{
    rbtz_t t;
    return rbgrid_tz_parse_into(posix, &t);
}

bool rbgrid_tz_parse_into(const char *posix, void *out_tz)
{
    rbtz_t *t = (rbtz_t *)out_tz;
    memset(t, 0, sizeof(*t));
    if (!posix || !*posix) return false;
    const char *p = parse_name(posix);
    if (!p) return false;
    long off;
    p = parse_hms(p, &off, 24);
    if (!p) return false;
    t->std_off = -off;                     /* POSIX: offset is west-positive */
    if (*p == '\0') return true;
    p = parse_name(p);
    if (!p) return false;
    t->has_dst = 1;
    t->dst_off = t->std_off + 3600;
    if (*p && *p != ',') {
        p = parse_hms(p, &off, 24);
        if (!p) return false;
        t->dst_off = -off;
    }
    if (*p == '\0') {                      /* no rule: the C library's default (US rule) */
        t->start = (rbtz_rule_t){ 'M', 3, 2, 0, 7200 };
        t->end   = (rbtz_rule_t){ 'M', 11, 1, 0, 7200 };
        return true;
    }
    if (*p++ != ',') return false;
    p = parse_rule(p, &t->start);
    if (!p || *p++ != ',') return false;
    p = parse_rule(p, &t->end);
    return p && *p == '\0';
}

bool rbgrid_tz_set(const char *posix)
{
    rbtz_t t;
    if (!rbgrid_tz_parse_into(posix, &t)) return false;
    s_tz = t;
    s_tz_set = 1;
    return true;
}

bool rbgrid_tz_is_set(void) { return s_tz_set != 0; }

/* ---- calendar arithmetic (proleptic Gregorian, UTC) ---- */
static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* days since 1970-01-01 for y-m-d (m 1..12) — Howard Hinnant's days_from_civil */
static long days_from_civil(long y, long m, long d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, long *m, long *d)
{
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yoe + era * 400 + (*m <= 2);
}

static void utc_tm(long long t, struct tm *out)
{
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    long y, m, d;
    civil_from_days((long)days, &y, &m, &d);
    memset(out, 0, sizeof(*out));
    out->tm_year = (int)(y - 1900);
    out->tm_mon  = (int)(m - 1);
    out->tm_mday = (int)d;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min  = (int)(rem % 3600 / 60);
    out->tm_sec  = (int)(rem % 60);
    out->tm_wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday (4) */
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
}

/* UTC instant (s) of a rule in year y, given the offset in force before the change */
static long long rule_utc(const rbtz_rule_t *r, long y, long off_before)
{
    long day;   /* days since epoch of the change date */
    if (r->kind == 'M') {
        long first = days_from_civil(y, r->m, 1);
        int wd_first = (int)(((first % 7) + 11) % 7);
        long dd = 1 + ((r->d - wd_first + 7) % 7) + 7L * (r->w - 1);
        static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        long ml = mdays[r->m - 1] + (r->m == 2 && is_leap(y));
        while (dd > ml) dd -= 7;               /* week 5 = the last such weekday */
        day = first + dd - 1;
    } else if (r->kind == 'J') {
        long n = r->d;                         /* 1..365, Feb 29 never counted */
        if (is_leap(y) && n >= 60) n++;
        day = days_from_civil(y, 1, 1) + n - 1;
    } else {
        day = days_from_civil(y, 1, 1) + r->d; /* 0..365, Feb 29 counted */
    }
    return (long long)day * 86400 + r->secs - off_before;
}

/* offset (s to add to UTC) in force at UTC instant t, and whether it is summer time */
static long offset_at(long long t, int *isdst)
{
    const rbtz_t *z = &s_tz;
    *isdst = 0;
    if (!z->has_dst) return z->std_off;
    struct tm u; utc_tm(t, &u);
    long y = u.tm_year + 1900;
    for (long yy = y - 1; yy <= y + 1; yy++) {   /* the window around t can start in the previous year */
        long long s = rule_utc(&z->start, yy, z->std_off);
        long long e = rule_utc(&z->end, yy, z->dst_off);
        if (s < e) {                            /* northern: summer inside the year */
            if (t >= s && t < e) { *isdst = 1; return z->dst_off; }
        } else {                                /* southern: summer across the new year */
            long long e_next = rule_utc(&z->end, yy + 1, z->dst_off);
            if (t >= s && t < e_next) { *isdst = 1; return z->dst_off; }
        }
    }
    return z->std_off;
}

struct tm *rbgrid_localtime_r(const time_t *timep, struct tm *result)
{
    if (!timep || !result) return NULL;
    int dst = 0;
    long off = s_tz_set ? offset_at((long long)*timep, &dst) : 0;
    utc_tm((long long)*timep + off, result);
    result->tm_isdst = dst;
    return result;
}

time_t rbgrid_mktime(struct tm *tm)
{
    if (!tm) return (time_t)-1;
    /* normalize the fields as mktime does (months / days / hours may be out of range) */
    long long y = tm->tm_year + 1900LL, mo = tm->tm_mon;
    y += mo / 12; mo %= 12; if (mo < 0) { mo += 12; y--; }
    long long days = days_from_civil((long)y, (long)mo + 1, 1) + (tm->tm_mday - 1);
    long long local = days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    long long t;
    if (!s_tz_set || !s_tz.has_dst) {
        t = local - (s_tz_set ? s_tz.std_off : 0);
    } else {
        long long ts = local - s_tz.std_off, td = local - s_tz.dst_off;
        int ds, dd;
        offset_at(ts, &ds); offset_at(td, &dd);
        int ok_s = !ds, ok_d = dd;              /* each reading consistent with the offset it assumed */
        if (ok_s && ok_d)                       /* repeated hour: as told, else the earlier instant (the C library's pick) */
            t = (tm->tm_isdst == 0) ? ts : (tm->tm_isdst > 0) ? td : (td < ts ? td : ts);
        else if (ok_d) t = td;
        else if (ok_s) t = ts;
        else t = (tm->tm_isdst > 0) ? td : ts;                 /* skipped hour: read it with the offset given */
    }
    rbgrid_localtime_r((const time_t *)&(time_t){ (time_t)t }, tm);
    return (time_t)t;
}

struct tm *rbgrid_localtime(const time_t *timep)
{
    static struct tm s_tm;   /* like localtime(): one shared result */
    return rbgrid_localtime_r(timep, &s_tm);
}
