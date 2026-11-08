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
/** @brief Per-node link key LMK = HMAC-SHA256(PMK, node MAC)[0..15], from the installation PMK in use. */
static inline void rbn_derive_lmk_from(const uint8_t pmk[RBN_KEY_LEN], const uint8_t mac[6], uint8_t out_lmk[RBN_KEY_LEN])
{
    uint8_t full[32];
    rbn_hmac_sha256(pmk, RBN_KEY_LEN, mac, 6, full);
    memcpy(out_lmk, full, RBN_KEY_LEN);
}
/** @brief LMK from the compile-time dev PMK (dev builds only; kept for the legacy pairing path). */
static inline void rbn_derive_lmk(const uint8_t mac[6], uint8_t out_lmk[RBN_KEY_LEN])
{
    static const uint8_t pmk[RBN_KEY_LEN] = RBN_PMK16;
    rbn_derive_lmk_from(pmk, mac, out_lmk);
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

/* ===== Per-installation keys (PAIR_REQ2 / PAIR_ACK2) ===== */
#include "mbedtls/ecdh.h"
#include "mbedtls/ccm.h"
#include "esp_random.h"

static inline int rbn_rng(void *ctx, unsigned char *buf, size_t len) { (void)ctx; esp_fill_random(buf, len); return 0; }

/** @brief Fresh X25519 key pair: priv (32, little-endian scalar) and pub (32). Returns 0 on success. */
static inline int rbn_x25519_keypair(uint8_t priv[32], uint8_t pub[32])
{
    mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q; size_t ol = 0; int r;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);
    r = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (!r) r = mbedtls_ecdh_gen_public(&grp, &d, &Q, rbn_rng, NULL);
    if (!r) r = mbedtls_mpi_write_binary_le(&d, priv, 32);
    if (!r) r = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &ol, pub, 32);
    mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&d); mbedtls_ecp_point_free(&Q);
    return (r || ol != 32) ? -1 : 0;
}

/** @brief X25519 shared secret (32) from our priv and the peer's pub. Returns 0 on success. */
static inline int rbn_x25519_shared(const uint8_t priv[32], const uint8_t peer_pub[32], uint8_t shared[32])
{
    mbedtls_ecp_group grp; mbedtls_mpi d, z; mbedtls_ecp_point Qp; int r;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_mpi_init(&z); mbedtls_ecp_point_init(&Qp);
    r = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (!r) r = mbedtls_mpi_read_binary_le(&d, priv, 32);
    if (!r) r = mbedtls_ecp_point_read_binary(&grp, &Qp, peer_pub, 32);
    if (!r) r = mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, rbn_rng, NULL);
    if (!r) r = mbedtls_mpi_write_binary_le(&z, shared, 32);
    mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&d); mbedtls_mpi_free(&z); mbedtls_ecp_point_free(&Qp);
    return r ? -1 : 0;
}

static inline void rbn_wrap_key(const uint8_t shared[32], uint8_t key[16])
{
    static const char info[] = "rbgrid-pmk-v1";
    uint8_t full[32];
    rbn_hmac_sha256(shared, 32, (const uint8_t *)info, sizeof(info) - 1, full);
    memcpy(key, full, 16);
}
static inline void rbn_wrap_nonce(const uint8_t node_mac[6], uint32_t seq, uint8_t nonce[13])
{
    memset(nonce, 0, 13);
    memcpy(nonce, node_mac, 6);
    memcpy(nonce + 6, &seq, 4);
}

/** @brief Hub: encrypt the PMK for one node. Returns 0 on success. */
static inline int rbn_pmk_wrap(const uint8_t shared[32], const uint8_t node_mac[6], uint32_t seq,
                               const uint8_t pmk[16], uint8_t enc[16], uint8_t tag[16])
{
    uint8_t k[16], n[13]; mbedtls_ccm_context c; int r;
    rbn_wrap_key(shared, k); rbn_wrap_nonce(node_mac, seq, n);
    mbedtls_ccm_init(&c);
    r = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, k, 128);
    if (!r) r = mbedtls_ccm_encrypt_and_tag(&c, 16, n, 13, node_mac, 6, pmk, enc, tag, 16);
    mbedtls_ccm_free(&c);
    memset(k, 0, sizeof(k));
    return r ? -1 : 0;
}

/** @brief Node: decrypt + authenticate the PMK. Returns 0 when the tag verifies. */
static inline int rbn_pmk_unwrap(const uint8_t shared[32], const uint8_t node_mac[6], uint32_t seq,
                                 const uint8_t enc[16], const uint8_t tag[16], uint8_t pmk[16])
{
    uint8_t k[16], n[13]; mbedtls_ccm_context c; int r;
    rbn_wrap_key(shared, k); rbn_wrap_nonce(node_mac, seq, n);
    mbedtls_ccm_init(&c);
    r = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, k, 128);
    if (!r) r = mbedtls_ccm_auth_decrypt(&c, 16, n, 13, node_mac, 6, enc, pmk, tag, 16);
    mbedtls_ccm_free(&c);
    memset(k, 0, sizeof(k));
    return r ? -1 : 0;
}

#endif /* RBGRID_ESPNOW_CRYPTO_H */
