/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file ap_provision.example.h
 * @brief Template of the hub's access-point provisioning file.
 *
 * Copy this file to `ap_provision.h` (same directory; it is git-ignored) and set your own value before
 * building firmware for real devices. Without `ap_provision.h` the build falls back to this template and
 * prints a compiler warning.
 *
 * RBGRID_AP_SETUP_PASSWORD — password of the hub's own Wi-Fi network (`rbgrid-<mac4>`) while the hub has no
 * home Wi-Fi configured (first setup). Any printable ASCII string of **8 to 63 characters** (WPA2-PSK limits;
 * the build fails outside that range). The password stays in force until the user sets their own access-point
 * password in the app (`POST /api/system/ap`); the hub never generates or changes it by itself.
 */
#pragma once

/** @brief First-setup password of the hub's own access point (8..63 printable ASCII chars, WPA2-PSK). */
#define RBGRID_AP_SETUP_PASSWORD "RBGRID123"
