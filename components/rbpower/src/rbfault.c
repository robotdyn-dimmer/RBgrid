/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Fault injection (see rbfault.h). Compiled empty unless RBGRID_FAULT_INJECT. */
#include "rbfault.h"
#ifdef RBGRID_FAULT_INJECT
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "esp_log.h"

static uint32_t s_left[RBFAULT_COUNT];
static uint32_t s_skip[RBFAULT_COUNT];   /* calls to let through before the hits start */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

bool rbfault_hit(rbfault_point_t p)
{
    if (p >= RBFAULT_COUNT) return false;
    bool hit = false;
    portENTER_CRITICAL(&s_mux);
    if (s_left[p]) { if (s_skip[p]) s_skip[p]--; else { s_left[p]--; hit = true; } }
    portEXIT_CRITICAL(&s_mux);
    if (hit) ESP_LOGW("RBFAULT", "fault injected at point %d", (int)p);
    return hit;
}

void rbfault_arm(rbfault_point_t p, uint32_t count)
{
    rbfault_arm_skip(p, count, 0);
}

void rbfault_arm_skip(rbfault_point_t p, uint32_t count, uint32_t skip)
{
    if (p >= RBFAULT_COUNT) return;
    if (p == RBFAULT_LFS_MOUNT_IO) {
        nvs_handle_t h;
        if (nvs_open("rbfault", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u32(h, "mount_io", count); nvs_commit(h); nvs_close(h); }
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_left[p] = count;
    s_skip[p] = skip;
    portEXIT_CRITICAL(&s_mux);
}

void rbfault_load_boot(void)
{
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open("rbfault", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_u32(h, "mount_io", &n) == ESP_OK && n) {
        s_left[RBFAULT_LFS_MOUNT_IO] = n;
        nvs_erase_key(h, "mount_io");    /* one boot only */
        nvs_commit(h);
    }
    nvs_close(h);
}

int rbfault_from_name(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "flash_write"))  return RBFAULT_FLASH_RECORD_WRITE;
    if (!strcmp(name, "lfs_sync"))     return RBFAULT_LFS_SYNC;
    if (!strcmp(name, "lfs_mount_io")) return RBFAULT_LFS_MOUNT_IO;
    return -1;
}
#endif
