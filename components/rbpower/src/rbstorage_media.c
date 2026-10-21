/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_media.c
 * @brief Storage media routing (internal `stats` partition or external NOR); see rbstorage_media.h.
 */
#include "rbstorage_media.h"
#include "partition_layout.h"
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "rbmedia";

static rbmedia_mode_t         s_mode = RBMEDIA_NONE;
static const esp_partition_t *s_stats_part = NULL;   /* internal mode */
static esp_flash_t           *s_ext_handle = NULL;   /* external mode (optional) */

esp_err_t rbmedia_init_internal(void)
{
    if (s_mode == RBMEDIA_INTERNAL) return ESP_OK;
    s_stats_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_ANY, "stats");
    if (!s_stats_part) {
        ESP_LOGE(TAG, "internal `stats` partition not found — storage stays RAM-only "
                      "(old partition table? full reflash with the new layout required)");
        return ESP_ERR_NOT_FOUND;
    }
    s_mode = RBMEDIA_INTERNAL;
    ESP_LOGI(TAG, "media = INTERNAL: stats partition @0x%06lX, %lu KB (%lu slots of %u KB); "
                  "external NOR = optional, not used",
             (unsigned long)s_stats_part->address,
             (unsigned long)(s_stats_part->size / 1024),
             (unsigned long)(s_stats_part->size / (208u * 1024u)), 208u);
    return ESP_OK;
}

esp_err_t rbmedia_attach_external(esp_flash_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    s_ext_handle = handle;
    s_mode = RBMEDIA_EXTERNAL;
    ESP_LOGI(TAG, "media = EXTERNAL NOR (optional path)");
    return ESP_OK;
}

rbmedia_mode_t rbmedia_mode(void) { return s_mode; }

uint32_t rbmedia_stats_region_size(void)
{
    if (s_mode == RBMEDIA_INTERNAL) return (uint32_t)s_stats_part->size;
    if (s_mode == RBMEDIA_EXTERNAL) return PARTITION_STATS_SIZE;  /* overridden by chip size upstream */
    return 0;
}

/* abs (external-layout) → internal-partition-relative, with bounds check */
static inline esp_err_t xlat(uint32_t abs_off, size_t len, uint32_t *rel)
{
    if (abs_off < PARTITION_STATS_OFFSET) return ESP_ERR_INVALID_ARG;
    uint32_t r = abs_off - PARTITION_STATS_OFFSET;
    if (!s_stats_part || r + len > s_stats_part->size) return ESP_ERR_INVALID_SIZE;
    *rel = r;
    return ESP_OK;
}

esp_err_t rbmedia_stats_read(uint32_t abs_off, void *dst, size_t len)
{
    if (s_mode == RBMEDIA_EXTERNAL) return esp_flash_read(s_ext_handle, dst, abs_off, len);
    if (s_mode != RBMEDIA_INTERNAL) return ESP_ERR_INVALID_STATE;
    uint32_t rel; esp_err_t e = xlat(abs_off, len, &rel);
    return (e == ESP_OK) ? esp_partition_read(s_stats_part, rel, dst, len) : e;
}

esp_err_t rbmedia_stats_write(uint32_t abs_off, const void *src, size_t len)
{
    if (s_mode == RBMEDIA_EXTERNAL) return esp_flash_write(s_ext_handle, src, abs_off, len);
    if (s_mode != RBMEDIA_INTERNAL) return ESP_ERR_INVALID_STATE;
    uint32_t rel; esp_err_t e = xlat(abs_off, len, &rel);
    return (e == ESP_OK) ? esp_partition_write(s_stats_part, rel, src, len) : e;
}

esp_err_t rbmedia_stats_erase(uint32_t abs_off, size_t len)
{
    if (s_mode == RBMEDIA_EXTERNAL) return esp_flash_erase_region(s_ext_handle, abs_off, len);
    if (s_mode != RBMEDIA_INTERNAL) return ESP_ERR_INVALID_STATE;
    uint32_t rel; esp_err_t e = xlat(abs_off, len, &rel);
    return (e == ESP_OK) ? esp_partition_erase_range(s_stats_part, rel, len) : e;
}

esp_flash_t *rbmedia_stats_handle_token(void)
{
    if (s_mode == RBMEDIA_EXTERNAL) return s_ext_handle;
    if (s_mode == RBMEDIA_INTERNAL) return esp_flash_default_chip;  /* token only, never dereferenced */
    return NULL;
}
