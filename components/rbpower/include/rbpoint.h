/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file  rbpoint.h
 * @brief Point-slug utilities and source-to-capability mapping (pure functions, no state).
 *
 * A point is the unit's STABLE external key (telemetry `units{}`, Home Assistant unique_id, CSV export):
 * a slug `[a-z0-9_.-]{1,32}` generated once when the unit is bound (from the display name, with Cyrillic
 * transliteration) and persisted in units.json. Renames go through the explicit rename operation
 * (the old slug becomes a tombstone); numeric ids stay internal.
 *
 * All functions are reentrant and may be called from any task.
 */
#ifndef RBPOINT_H
#define RBPOINT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rbpower_enums.h"
#include "rbhome_types.h"

#define RBPOINT_SLUG_MAX 32          ///< Maximum slug length in chars, excluding the NUL
#define RBPOINT_TOMBSTONE_MAX 64     ///< Number of retired slugs kept to block silent identity reuse

/**
 * @brief Strict validation of a user-supplied slug.
 *
 * Accepted form: 1 to 32 characters from a-z, 0-9, '_', '.', '-'.
 *
 * @param[in] s NUL-terminated slug (NULL allowed).
 * @return true if @p s is a valid slug, false otherwise (including NULL or empty).
 */
bool rbpoint_slug_valid(const char* s);

/**
 * @brief Generate a slug from a display name.
 *
 * UTF-8 input; Cyrillic letters are transliterated, uppercase ASCII is folded, every other character
 * not in `[a-z0-9_.-]` collapses to '_'; leading, trailing and repeated separators are squeezed; an
 * empty result becomes "unit". The result is NOT made unique; the caller resolves collisions.
 *
 * @param[in]  name Display name (UTF-8, NULL treated as empty).
 * @param[out] out  Buffer of at least RBPOINT_SLUG_MAX+1 bytes; receives the NUL-terminated slug.
 */
void rbpoint_slug_from_name(const char* name, char* out);

/**
 * @brief Make a slug unique by appending "-2", "-3", … (first free n, base truncated to fit).
 *
 * If no free suffix is found below n = 1000 the (truncated) base is returned unchanged.
 *
 * @param[in]  base  Candidate slug.
 * @param[out] out   Buffer of at least RBPOINT_SLUG_MAX+1 bytes; may be the same buffer as @p base.
 * @param[in]  taken Predicate returning true if a slug is already in use; NULL means nothing is taken.
 * @param[in]  arg   Opaque context passed to @p taken.
 */
void rbpoint_slug_uniquify(const char* base, char* out,
                           bool (*taken)(const char* slug, void* arg), void* arg);

/**
 * @brief Capability bitmask (RBUNIT_CAP_*) of a unit, derived from its source and metric type.
 * @param[in] source_type   Measurement source of the unit.
 * @param[in] metric_type   Metric type (voltage bus or current/power channel).
 * @param[in] bidirectional true if the unit measures power flow in both directions.
 * @return Bitmask of RBUNIT_CAP_* flags.
 */
uint32_t rbpoint_caps_for_source(rbpower_source_type_t source_type,
                                 rbpower_metric_type_t metric_type,
                                 bool bidirectional);

#endif /* RBPOINT_H */
