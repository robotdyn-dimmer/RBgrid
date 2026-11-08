/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_core.c
 * @brief Core context, lifecycle hooks (3 s callbacks, reboot notifiers, image confirmed) and the
 *        boot-time factory reset. See rbgrid_core.h.
 */
#include "rbgrid_core.h"
#include "rbstats_migrate.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "RBGRID_CORE";

/* ---- Context ---- */
static rbpower_extended_config_t *s_ext_cfg = NULL;
static rbgrid_json_context_t *s_json_ctx = NULL;

void rbgrid_core_set_context(rbpower_extended_config_t *ext_cfg, rbgrid_json_context_t *json_ctx)
{
    if (ext_cfg) s_ext_cfg = ext_cfg;
    if (json_ctx) s_json_ctx = json_ctx;
}
rbpower_extended_config_t *rbgrid_core_ext_cfg(void) { return s_ext_cfg; }
rbgrid_json_context_t *rbgrid_core_json_ctx(void) { return s_json_ctx; }

/* ---- 3 s callbacks ----
 * Registered at init by the transports (telemetry today). The table is written before the statistics
 * timers start and read on the esp_timer task; an entry is written once and never removed. */
#define RBGRID_3S_CB_MAX 4   ///< Max 3 s callbacks
static volatile rbgrid_3s_cb_t s_3s_cbs[RBGRID_3S_CB_MAX];

bool rbgrid_core_register_3s_cb(rbgrid_3s_cb_t cb)
{
    if (!cb) return false;
    for (int i = 0; i < RBGRID_3S_CB_MAX; i++) {
        if (s_3s_cbs[i] == cb) return true;
        if (s_3s_cbs[i] == NULL) { s_3s_cbs[i] = cb; return true; }
    }
    ESP_LOGE(TAG, "3 s callback table full");
    return false;
}

void rbgrid_core_run_3s_cbs(uint64_t period_end_us)
{
    for (int i = 0; i < RBGRID_3S_CB_MAX; i++) {
        rbgrid_3s_cb_t cb = s_3s_cbs[i];
        if (!cb) break;
        cb(period_end_us);
    }
}

/* ---- Reboot notifiers ---- */
#define RBGRID_REBOOT_NOTIFY_MAX 4   ///< Max reboot notifiers
static void (*volatile s_reboot_fns[RBGRID_REBOOT_NOTIFY_MAX])(void);

bool rbgrid_core_register_reboot_notify(void (*fn)(void))
{
    if (!fn) return false;
    for (int i = 0; i < RBGRID_REBOOT_NOTIFY_MAX; i++) {
        if (s_reboot_fns[i] == fn) return true;
        if (s_reboot_fns[i] == NULL) { s_reboot_fns[i] = fn; return true; }
    }
    ESP_LOGE(TAG, "reboot notifier table full");
    return false;
}

void rbgrid_core_notify_reboot(void)
{
    for (int i = 0; i < RBGRID_REBOOT_NOTIFY_MAX; i++) {
        void (*fn)(void) = s_reboot_fns[i];
        if (!fn) break;
        fn();
    }
}

/* ---- Image confirmed ---- */
void rbgrid_core_image_confirmed(void)
{
    /* A deferred stats-format migration waits for exactly this (never touch the old-format data while a
     * rollback to the old firmware is still possible). Idempotent. */
    rbstats_migrate_notify_image_valid();
}

/* ---- Factory reset ----
 * rbgrid_core_factory_reset_arm() sets a marker in RTC no-init memory (it survives esp_restart, not a power
 * cut); the caller restarts the hub. The erase runs at the very start of the next boot
 * (rbgrid_core_factory_reset_at_boot, first thing in app_main), before NVS and the config file system are
 * opened, so nothing is erased under a mounted file system. A power cut between the request and the restart
 * cancels the reset (the marker is lost): the hub then just boots as it was. */
#define RBGRID_FRESET_MAGIC 0x46525354u   ///< 'FRST'
static RTC_NOINIT_ATTR uint32_t s_freset_magic;   ///< RBGRID_FRESET_MAGIC when armed
static RTC_NOINIT_ATTR uint32_t s_freset_check;   ///< ~RBGRID_FRESET_MAGIC (power-on garbage guard)
static RTC_NOINIT_ATTR uint32_t s_freset_flags;   ///< bit 0: erase the stats (history) partition too

void rbgrid_core_factory_reset_arm(bool erase_history)
{
    s_freset_flags = erase_history ? 1u : 0u;
    s_freset_check = ~RBGRID_FRESET_MAGIC;
    s_freset_magic = RBGRID_FRESET_MAGIC;
}

bool rbgrid_core_factory_reset_at_boot(void)
{
    if (s_freset_magic != RBGRID_FRESET_MAGIC || s_freset_check != ~RBGRID_FRESET_MAGIC) return false;
    const bool erase_history = (s_freset_flags & 1u) != 0;
    s_freset_magic = 0; s_freset_check = 0; s_freset_flags = 0;   /* one shot, even if an erase below fails */
    if (esp_reset_reason() != ESP_RST_SW) {
        ESP_LOGW(TAG, "factory reset marker found after a non-software reset — ignored");
        return false;
    }
    ESP_LOGW(TAG, "FACTORY RESET: erasing settings%s", erase_history ? " and history" : " (history kept)");
    const esp_partition_t *cfg = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "config");
    esp_err_t ec = cfg ? esp_partition_erase_range(cfg, 0, cfg->size) : ESP_ERR_NOT_FOUND;
    esp_err_t en = nvs_flash_erase();
    esp_err_t es = ESP_OK;
    if (erase_history) {
        /* internal stats partition (internal-first media); an old partition table without it keeps its history */
        const esp_partition_t *st = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "stats");
        es = st ? esp_partition_erase_range(st, 0, st->size) : ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "FACTORY RESET done: config %s, nvs %s, history %s", esp_err_to_name(ec), esp_err_to_name(en),
             erase_history ? esp_err_to_name(es) : "kept");
    return true;
}
