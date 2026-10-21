/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file espnow_crypto.h
 * @brief RBgrid ESP-NOW security helpers. Shared by the hub and the node firmware; keep both copies identical.
 *
 * - Pairing authentication: an HMAC tag authenticates the broadcast PAIR_REQ (the one control frame that
 *   must travel before any encrypted peer exists). A rogue node cannot get itself registered without the
 *   shared pairing key; a rogue hub cannot forge a PAIR_REQ that a node would accept.
 * - Data path: native ESP-NOW CCMP on the unicast frames, keyed by a per-node LMK = HMAC(PMK, mac)
 *   (rbn_derive_lmk(), used by the hub when it adds a node with esp_now_add_peer()).
 *
 * Keys come from espnow_keys.h (see espnow_keys.example.h). Keep your real keys out of version control;
 * without that header the hub refuses to start ESP-NOW.
 */
#ifndef RBGRID_ESPNOW_CRYPTO_H
#define RBGRID_ESPNOW_CRYPTO_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "mbedtls/md.h"
#if defined(__has_include) && __has_include("espnow_keys.h")
#  include "espnow_keys.h"   /* RBN_PMK16 (16B) + RBN_PAIRKEY16 (16B) initializers — gitignored */
/** @brief 1 when espnow_keys.h was found at build time, 0 otherwise (ESP-NOW then refuses to start). */
#  define RBN_HAVE_KEYS 1
#else
/* Clean tree / release image without the dev keys header: no baked-in key. The hub refuses to start
 * ESP-NOW (esp_now_hub_start → ESP_ERR_NOT_SUPPORTED) instead of running with a known all-zero key. */
/** @brief 16-byte ESP-NOW Primary Master Key initializer (normally from espnow_keys.h). */
#  define RBN_PMK16      { 0 }
/** @brief 16-byte pairing-authentication key initializer (normally from espnow_keys.h). */
#  define RBN_PAIRKEY16  { 0 }
/** @brief 0: no espnow_keys.h at build time, ESP-NOW refuses to start. */
#  define RBN_HAVE_KEYS 0
#endif

/** @brief Key length (bytes) of the PMK, LMK and pairing key. */
#define RBN_KEY_LEN   16
/** @brief PAIR_REQ authentication tag length (bytes): truncated HMAC-SHA256 appended to PAIR_REQ. */
#define RBN_TAG_LEN    8   /* truncated HMAC-SHA256 appended to PAIR_REQ */

/**
 * @brief Compute HMAC-SHA256 with mbedTLS.
 * @param[in]  key      Key bytes.
 * @param[in]  key_len  Key length (bytes).
 * @param[in]  msg      Message bytes.
 * @param[in]  msg_len  Message length (bytes).
 * @param[out] out32    32-byte digest.
 */
static inline void rbn_hmac_sha256(const uint8_t *key, size_t key_len,
                                   const uint8_t *msg, size_t msg_len, uint8_t out32[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, key, key_len, msg, msg_len, out32);
}

/**
 * @brief Derive the per-node link key for ESP-NOW CCMP: LMK = HMAC-SHA256(PMK, node_mac)[:16].
 *
 * The hub derives it from the node's MAC, the node from its own MAC, so both compute the same key without it
 * ever crossing the air.
 * @param[in]  mac      Node MAC address (6 bytes).
 * @param[out] out_lmk  16-byte LMK.
 */
static inline void rbn_derive_lmk(const uint8_t mac[6], uint8_t out_lmk[RBN_KEY_LEN])
{
    static const uint8_t pmk[RBN_KEY_LEN] = RBN_PMK16;
    uint8_t full[32];
    rbn_hmac_sha256(pmk, RBN_KEY_LEN, mac, 6, full);
    memcpy(out_lmk, full, RBN_KEY_LEN);
}

/**
 * @brief Compute the authentication tag over a PAIR_REQ prefix: tag = HMAC-SHA256(PAIRKEY, prefix)[:8].
 *
 * The prefix is the whole frame EXCEPT the trailing tag (header + fw_ver + chan_count + chans[]).
 * @param[in]  prefix      Frame bytes covered by the tag.
 * @param[in]  prefix_len  Prefix length (bytes), see RBN_PAIR_REQ_PREFIX_LEN().
 * @param[out] out_tag     RBN_TAG_LEN-byte tag.
 */
static inline void rbn_pair_tag(const uint8_t *prefix, size_t prefix_len, uint8_t out_tag[RBN_TAG_LEN])
{
    static const uint8_t key[RBN_KEY_LEN] = RBN_PAIRKEY16;
    uint8_t full[32];
    rbn_hmac_sha256(key, RBN_KEY_LEN, prefix, prefix_len, full);
    memcpy(out_tag, full, RBN_TAG_LEN);
}

/**
 * @brief Constant-time comparison of two tags (avoids a timing oracle on the auth check).
 * @param[in] a  First tag (RBN_TAG_LEN bytes).
 * @param[in] b  Second tag (RBN_TAG_LEN bytes).
 * @return 1 if equal, 0 otherwise.
 */
static inline int rbn_tag_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < RBN_TAG_LEN; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

#endif /* RBGRID_ESPNOW_CRYPTO_H */
