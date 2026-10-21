/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstats_v4_read.c
 * @brief v4-facing statistics record reader.
 *
 * Consumers read rbstats_*_v4_t regardless of the ON-FLASH format:
 *  - with on-flash format v4 (RBPOWER_RECORD_FORMAT_VERSION >= 4, the current format) this is a
 *    raw read at the v4 stride, without conversion;
 *  - with on-flash format v3 it reads raw v3 transfer structs in small chunks and lifts them
 *    through the pure converters (dur = nominal, q |= MIGRATED semantics — callers that
 *    aggregate must treat MIGRATED as "pre-v4 record", not propagate it to parents).
 * Readers therefore depend only on the v4 structs, independent of the stored format.
 */
#include "rbextflash_operations.h"
#include "rbstats_v4_types.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "RBSTATS_V4READ";

#define V4READ_CHUNK 16   ///< Records per raw read in the v3 path

/*
 * Read records as v4 structs (declared in rbextflash_operations.h).
 */
esp_err_t rbstats_flash_read_v4(rbpower_unit_id_t unit_id,
                                rbpower_period_t period,
                                rbpower_data_type_t data_type,
                                uint32_t start_index,
                                uint32_t count,
                                void *out_v4,
                                uint32_t *records_read_out)
{
    if (records_read_out) {
        *records_read_out = 0;
    }
    if (!out_v4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_type != RBPOWER_DATA_TYPE_ELECTRICAL &&
        data_type != RBPOWER_DATA_TYPE_ENERGY &&
        data_type != RBPOWER_DATA_TYPE_ECONOMICS) {
        ESP_LOGE(TAG, "v4 read: unsupported data type %d", (int)data_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

#if RBPOWER_RECORD_FORMAT_VERSION >= 4
    /* On-flash format IS v4 — raw read at the live stride, no conversion. */
    return rbpower_flash_read_records(unit_id, period, data_type, start_index, count,
                                      out_v4, records_read_out);
#else
    /* On-flash format v3: chunked raw read + per-record lift. */
    union {
        rbpower_electrical_transfer_t elec[V4READ_CHUNK];
        rbpower_energy_transfer_t     enrg[V4READ_CHUNK];
        rbpower_economics_transfer_t  econ[V4READ_CHUNK];
    } raw;

    uint32_t done = 0;
    while (done < count) {
        uint32_t ask = count - done;
        if (ask > V4READ_CHUNK) {
            ask = V4READ_CHUNK;
        }
        uint32_t got = 0;
        esp_err_t e = rbpower_flash_read_records(unit_id, period, data_type,
                                                 start_index + done, ask, &raw, &got);
        if (e != ESP_OK && got == 0 && done == 0) {
            return e;            /* nothing at all (flash offline / buffer missing) */
        }
        for (uint32_t i = 0; i < got; i++) {
            switch (data_type) {
                case RBPOWER_DATA_TYPE_ELECTRICAL:
                    rbstats_v4_conv_electrical(&raw.elec[i], period,
                                               &((rbstats_electrical_v4_t *)out_v4)[done + i]);
                    break;
                case RBPOWER_DATA_TYPE_ENERGY:
                    rbstats_v4_conv_energy(&raw.enrg[i], period,
                                           &((rbstats_energy_v4_t *)out_v4)[done + i]);
                    break;
                default: /* ECONOMICS */
                    rbstats_v4_conv_econ(&raw.econ[i], period,
                                         &((rbstats_econ_v4_t *)out_v4)[done + i]);
                    break;
            }
        }
        done += got;
        if (got < ask) {
            break;               /* ring exhausted / read error mid-way: return what we have */
        }
    }

    if (records_read_out) {
        *records_read_out = done;
    }
    return ESP_OK;
#endif
}
