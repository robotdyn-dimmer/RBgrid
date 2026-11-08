/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_core_priv.h
 * @brief Helpers shared by the core files and the HTTP API inside the rbpower component (not public).
 */
#ifndef RBGRID_CORE_PRIV_H
#define RBGRID_CORE_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rbpower.h"
#include "rbamp_sensor_models.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Oldest record timestamp accepted as real history, epoch s (2025-01-01). Earlier = fake-clock artifact. */
#define RBGRID_TS_SANE_MIN 1735689600u

#define RBGRID_TOKEN_FILE      "/api_token"      ///< littlefs file holding the ADMIN token
#define RBGRID_TOKEN_READ_FILE "/api_token_read" ///< littlefs file holding the READ token

/**
 * @brief Read an API token file.
 *
 * With the config store not mounted it reports a token nothing can match, so a failed mount never
 * reads as "no token = everything open".
 *
 * @param[in]  file  RBGRID_TOKEN_FILE or RBGRID_TOKEN_READ_FILE.
 * @param[out] out   Token, NUL-terminated (48 bytes).
 * @return true if a token is set.
 */
bool rbgrid_token_load(const char *file, char out[48]);

/** @brief Notify the unit-deleted hook (main/ ESP-NOW hub). @param unit_id Deleted point. */
void rbgrid_core_notify_unit_deleted(uint16_t unit_id);

/** @brief "hour".."year" → rbpower_stat_period_type_t. @param s Name. @return Enum, or -1. */
int rbgrid_period_str_to_stat(const char *s);
/** @brief "hour".."year" → rbpower_period_t (flash period). @param s Name. @return Enum, or -1. */
int rbgrid_period_str_to_flash(const char *s);
/**
 * @brief Visit the VALID period records of a point, newest first, inside [from, to].
 * @param unit Point. @param stat_period Stat period. @param flash_period Flash period. @param from Start, epoch s.
 * @param to End, epoch s. @param cb Callback per record. @param ctx Callback context.
 * @return Number of valid records walked (before the window).
 */
uint32_t rbgrid_foreach_valid_record(rbpower_unit_id_t unit, int stat_period, int flash_period,
                                     uint32_t from, uint32_t to,
                                     void (*cb)(const rbpower_period_statistics_t *, void *), void *ctx);

/** @brief Escape a string for a hand-built JSON body (" \ and control chars). @param in Input. @param out Output. @param n Size. */
void rbgrid_json_escape(const char *in, char *out, size_t n);

/** @brief Whether an SCT-013 model code may be selected by the user. @param mi Model info. @return true if selectable. */
bool rbgrid_ct_model_selectable(const rbamp_sensor_model_info_t *mi);

#ifdef __cplusplus
}
#endif

#endif /* RBGRID_CORE_PRIV_H */
