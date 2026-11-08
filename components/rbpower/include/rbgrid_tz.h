/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_tz.h
 * @brief RBgrid's own local time (POSIX TZ rule), independent of the process-wide TZ variable.
 */
#ifndef RBGRID_TZ_H
#define RBGRID_TZ_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set RBgrid's time zone from a POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3").
 * @param[in] posix  Zone string.
 * @return false (zone unchanged) if the string can't be parsed.
 */
bool rbgrid_tz_set(const char *posix);
/** @brief Whether a zone has been set (until then local time = UTC). */
bool rbgrid_tz_is_set(void);
/** @brief Whether @p posix parses (nothing changes). */
bool rbgrid_tz_parse_ok(const char *posix);
/** @brief Parse into an opaque zone object (internal and tests). @param posix Zone. @param out_tz Zone object. */
bool rbgrid_tz_parse_into(const char *posix, void *out_tz);

/** @brief localtime_r() on RBgrid's zone. @param timep UTC seconds. @param result Out. @return @p result. */
struct tm *rbgrid_localtime_r(const time_t *timep, struct tm *result);
/** @brief localtime() on RBgrid's zone (shared static result, like localtime()). */
struct tm *rbgrid_localtime(const time_t *timep);
/** @brief mktime() on RBgrid's zone; normalizes @p tm (tm_isdst -1 = unknown). @return UTC seconds. */
time_t rbgrid_mktime(struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif /* RBGRID_TZ_H */
