/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file espnow_keys.example.h
 * @brief Template for espnow_keys.h, the ESP-NOW key header (hub and node must use the same bytes).
 *
 * Copy this file to main/espnow_keys.h and replace both placeholder keys with your own random values.
 * espnow_keys.h is listed in .gitignore: keep your real keys out of version control. Without
 * espnow_keys.h the build has no keys and the hub refuses to start ESP-NOW (see espnow_crypto.h).
 */
#ifndef RBGRID_ESPNOW_KEYS_H
#define RBGRID_ESPNOW_KEYS_H

/** @brief 16-byte ESP-NOW Primary Master Key (used to derive the per-node CCMP link keys). Placeholder value. */
#define RBN_PMK16  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00  /* REPLACE */ }

/** @brief 16-byte pairing-authentication key (HMAC tag on PAIR_REQ). Placeholder value; must differ from the PMK. */
#define RBN_PAIRKEY16  { 0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11  /* REPLACE */ }

#endif /* RBGRID_ESPNOW_KEYS_H */
