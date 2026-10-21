/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#pragma once
/**
 * @file rbstrutil.h
 * @brief Bounded string-append helper for snprintf chains.
 *
 * A chain of "pos += snprintf(buf + pos, cap - pos, ...)" is unsafe: snprintf returns the length it
 * WANTED to write, so once the chain truncates, pos passes cap, the next "cap - pos" wraps (size_t)
 * and the following snprintf writes past the buffer (this caused a stack overflow on the Wi-Fi status
 * page with a long SSID in APSTA mode). rb_scat() clamps the position instead.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/**
 * @brief Append formatted text at @p buf + @p at without ever overrunning the buffer.
 *
 * Reentrant; no shared state.
 *
 * @param[in,out] buf Destination buffer.
 * @param[in]     cap Size of @p buf in bytes.
 * @param[in]     at  Current write position (negative is treated as 0).
 * @param[in]     fmt printf-style format string, followed by its arguments.
 * @return The new write position, clamped to cap-1 (the terminating NUL always stays inside the
 *         buffer); 0 if @p cap is 0; @p at unchanged on an encoding error.
 */
static inline int rb_scat(char *buf, size_t cap, int at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static inline int rb_scat(char *buf, size_t cap, int at, const char *fmt, ...)
{
    if (cap == 0) return 0;
    if (at < 0) at = 0;
    if ((size_t)at >= cap - 1) return (int)(cap - 1);
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + at, cap - (size_t)at, fmt, ap);
    va_end(ap);
    if (w < 0) return at;
    size_t end = (size_t)at + (size_t)w;
    return (int)(end > cap - 1 ? cap - 1 : end);
}
