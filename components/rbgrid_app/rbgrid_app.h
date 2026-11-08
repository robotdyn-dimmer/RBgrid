/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_app.h
 * @brief RBgrid application entry points: the standalone firmware and a hosted start (e.g. ESPHome).
 */
#ifndef RBGRID_APP_H
#define RBGRID_APP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Standalone firmware: the whole boot (Wi-Fi, core, interfaces, ESP-NOW). Called from app_main(). */
void rbgrid_app_main(void);

/**
 * @brief Hosted start: core init + core start + ESP-NOW hub, no RBgrid network or interfaces of its own.
 *
 * Call once, after the host has NVS, netif, the default event loop and (for ESP-NOW) Wi-Fi in STA mode.
 * The host confirms the running image itself and then calls rbgrid_core_image_confirmed().
 *
 * @return ESP_OK, or ESP_FAIL if the core could not start.
 */
esp_err_t rbgrid_host_start(void);

#ifdef __cplusplus
}
#endif

#endif /* RBGRID_APP_H */
