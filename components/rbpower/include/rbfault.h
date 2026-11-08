/* SPDX-FileCopyrightText: 2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/**
 * @file rbfault.h
 * @brief Fault injection for crash-consistency tests. TEST BUILDS ONLY (RBGRID_FAULT_INJECT): in a release
 *        build every rbfault_hit() is a constant false and the debug endpoint does not exist.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Places a fault can be injected. */
typedef enum {
    RBFAULT_FLASH_RECORD_WRITE = 0,   ///< rbpower_flash_write_record → ESP_FAIL (history record set)
    RBFAULT_LFS_SYNC,                 ///< littlefs file sync → error (config/token writes)
    RBFAULT_LFS_MOUNT_IO,             ///< littlefs mount → LFS_ERR_IO (armed in NVS, consumed at the next boot)
    RBFAULT_ESPNOW_ACK_DROP,          ///< ESP-NOW hub: the PERIOD_ACK after an inject is not sent (a lost ACK)
    RBFAULT_GAP_PLUS_DAY,             ///< rbAmp reboot gap: +25 h added to the measured gap (armed in NVS for the next boot)
    RBFAULT_COUNT
} rbfault_point_t;

#ifdef RBGRID_FAULT_INJECT
/** @brief True when the point is armed: consumes one of its remaining hits. */
bool rbfault_hit(rbfault_point_t p);
/** @brief Arm a point for @p count hits (RBFAULT_LFS_MOUNT_IO is stored in NVS for the next boot). */
void rbfault_arm(rbfault_point_t p, uint32_t count);
/** @brief As rbfault_arm, letting the first @p skip calls through. */
void rbfault_arm_skip(rbfault_point_t p, uint32_t count, uint32_t skip);
/** @brief Name → point (flash_write | lfs_sync | lfs_mount_io | espnow_ack_drop); -1 when unknown. */
int  rbfault_from_name(const char *name);
/** @brief Load NVS-armed points (call before the littlefs mount). */
void rbfault_load_boot(void);
#else
static inline bool rbfault_hit(rbfault_point_t p) { (void)p; return false; }
static inline void rbfault_load_boot(void) {}
#endif

#ifdef __cplusplus
}
#endif
