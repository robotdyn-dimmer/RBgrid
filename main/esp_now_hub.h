/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file esp_now_hub.h
 * @brief Hub-side ESP-NOW transport: beacon time master and receiver of node REALTIME/PERIOD/NODE_STATS
 *        frames, plus the persisted node registry.
 *
 * The hub runs ESP-NOW next to its Wi-Fi connection. It broadcasts a 1 s sync beacon (the time source the
 * nodes discipline to), receives node data and injects it into the configured units, the same way
 * main/rbamp_bridge.c feeds wired rbAmp modules. Each (node MAC, channel) maps to one config unit.
 *
 * ESP-NOW wireless nodes are available in the code but are not part of the v1.1.0 release feature set.
 *
 * Threading: the functions below are called from the console, the HTTP server and the HA discovery task.
 * Runtime tables are guarded by an internal spinlock; registry functions read/write a LittleFS file and must
 * not be called from an ISR or the Wi-Fi callback.
 */
#ifndef ESP_NOW_HUB_H
#define ESP_NOW_HUB_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persisted ESP-NOW node registry (/espnow_nodes.bin, LittleFS): paired or added nodes are stored here and
 * registered as standard config sensor units at boot. */
/** @brief Maximum number of nodes in the registry. */
#define ESPNOW_REG_MAX_NODES 6
/** @brief Maximum number of measurement channels per node. */
#define ESPNOW_REG_MAX_CH    4
/** @brief Registry file format version. v2 added node flags; a blob with another version is discarded on load
 *  (the nodes must be paired again). */
#define ESPNOW_REG_VERSION   2          /* v2 added node flags; an older blob is discarded on load (re-pair) */
/** @brief espnow_reg_node_t::flags bit 0: node disabled by the user (persists across reboot). */
#define ESPNOW_REG_FLAG_DISABLED 0x01   /* node->flags bit0: user-disabled (persists across reboot) */
/** @brief One registered node channel. */
typedef struct { uint8_t ch; uint8_t ct_code; } espnow_reg_chan_t;        /* ch = node flat channel_id, ct_code 1/2/3/4/6 */
/** @var espnow_reg_chan_t::ch
 *  Node flat channel_id (as sent in the node's REALTIME/PERIOD records).
 *  @var espnow_reg_chan_t::ct_code
 *  Current-transformer code of the channel (1, 2, 3, 4 or 6). */
/** @brief One registered node. */
typedef struct { uint8_t mac[6]; uint8_t ch_count; espnow_reg_chan_t channels[ESPNOW_REG_MAX_CH]; uint8_t flags; } espnow_reg_node_t;
/** @var espnow_reg_node_t::mac
 *  Node Wi-Fi MAC address (identity of the node).
 *  @var espnow_reg_node_t::ch_count
 *  Number of valid entries in @c channels (1..ESPNOW_REG_MAX_CH).
 *  @var espnow_reg_node_t::channels
 *  The node's channels; each one becomes a config unit.
 *  @var espnow_reg_node_t::flags
 *  Node flags (ESPNOW_REG_FLAG_DISABLED). */
/** @brief The whole registry as stored in /espnow_nodes.bin. */
typedef struct { uint8_t version; uint8_t node_count; espnow_reg_node_t nodes[ESPNOW_REG_MAX_NODES]; } espnow_registry_t;
/** @var espnow_registry_t::version
 *  File format version (ESPNOW_REG_VERSION).
 *  @var espnow_registry_t::node_count
 *  Number of valid entries in @c nodes (0..ESPNOW_REG_MAX_NODES).
 *  @var espnow_registry_t::nodes
 *  Registered nodes. */

/**
 * @brief Load the persisted registry from LittleFS.
 * @param[out] reg  Registry to fill; zeroed (node_count = 0) when no valid registry exists.
 * @retval true   A registry with the current format version was loaded.
 * @retval false  No file, a read error, or a different format version (the blob is discarded).
 */
bool esp_now_registry_load(espnow_registry_t *reg);
/**
 * @brief Write the registry to LittleFS (/espnow_nodes.bin).
 * @param[in] reg  Registry to persist.
 * @return ESP_OK, or the error from the LittleFS write.
 */
esp_err_t esp_now_registry_save(const espnow_registry_t *reg);
/**
 * @brief Add a node or replace an existing one (matched by MAC) in the registry and persist it.
 *
 * Reboot to register the node's units.
 * @param[in] mac       Node MAC address (6 bytes).
 * @param[in] channels  Node channels; at most ESPNOW_REG_MAX_CH are used.
 * @param[in] n         Number of entries in @p channels (must be > 0).
 * @retval ESP_OK               Added or replaced and saved.
 * @retval ESP_ERR_INVALID_ARG  @p mac or @p channels is NULL, or @p n is 0.
 * @retval ESP_ERR_NO_MEM       The registry is full (ESPNOW_REG_MAX_NODES).
 * @return Any other code comes from the LittleFS write.
 */
esp_err_t esp_now_registry_add_node(const uint8_t mac[6], const espnow_reg_chan_t *channels, uint8_t n);
/**
 * @brief Remove a node (by MAC) from the registry and persist it. Its units are not removed; see
 *        esp_now_hub_delnode_cascade().
 * @param[in] mac  Node MAC address (6 bytes).
 * @retval ESP_OK             Removed and saved.
 * @retval ESP_ERR_NOT_FOUND  No registry, or the node is not in it.
 * @return Any other code comes from the LittleFS write.
 */
esp_err_t esp_now_registry_remove_node(const uint8_t mac[6]);


/**
 * @brief Start the ESP-NOW hub on the running Wi-Fi: optionally pin the channel, add the registered nodes as
 *        encrypted peers, start the beacon task (1 s) and the inject task (200 ms).
 * @param[in] channel  If > 0, force the Wi-Fi/ESP-NOW channel to this value (nodes must match; may affect
 *                     the STA link); 0 keeps the current channel.
 * @retval ESP_OK                 Started, or already running.
 * @retval ESP_ERR_NOT_SUPPORTED  The image was built without ESP-NOW keys (espnow_keys.h absent).
 * @return Any other code is the esp_now_init() error.
 */
esp_err_t esp_now_hub_start(uint8_t channel);

/** @brief Stop the ESP-NOW hub: end the tasks, unregister the callbacks and deinit ESP-NOW. Blocks for about
 *         1.1 s while the beacon task exits. */
void esp_now_hub_stop(void);

/** @brief Log a one-line status (channel, beacons sent, node frames received, last node current and sync
 *         quality) through ESP_LOG. */
void esp_now_hub_status(void);

/** @brief Report whether the hub is running.
 *  @return true if esp_now_hub_start() succeeded and the hub has not been stopped. */
bool esp_now_hub_running(void);

/* Over-the-air pairing window. While it is open the hub accepts PAIR_REQ from new nodes; on a successful pair
 * the node is written to the registry (reboot to register its units). */
/** @brief Open the pairing window and reset the paired counter.
 *  @param[in] seconds  Window length in seconds. */
void     esp_now_hub_open_pairing(uint32_t seconds);
/** @brief Number of nodes paired since the window was last opened (non-zero means a reboot is needed).
 *  @return Paired node count. */
uint32_t esp_now_hub_paired_count(void);   /* nodes paired this window → reboot needed */
/** @brief Report whether the pairing window is open.
 *  @return true while the window has not expired. */
bool     esp_now_hub_pairing_open(void);

/**
 * @brief Command every registered node to latch its period NOW (encrypted unicast LATCH_NOW).
 *
 * Fires automatically on a tariff-zone transition (beacon task); also called by the `espnow latch` verb.
 * @param[in] reason  0 = manual, 1 = tariff zone (RBN_LATCH_REASON_*).
 * @return Number of nodes commanded (0 when the hub is not running or no nodes are registered).
 */
uint32_t esp_now_hub_latch_all(uint8_t reason);   /* reason: 0=manual, 1=zone (RBN_LATCH_REASON_*) */

/* Operations: node liveness, removal/disable and status JSON. */
/**
 * @brief Remove a node AND its units without a reboot: remove each channel's config unit (and its flash slot),
 *        the registry entry, the runtime state, the encrypted peer and the node's HA "Online" sensor.
 * @param[in] mac  Node MAC address (6 bytes).
 * @return Number of units removed.
 */
uint32_t esp_now_hub_delnode_cascade(const uint8_t mac[6]);          /* remove node + its units (reboot-free); returns units removed */
/**
 * @brief Disable (silence) or re-enable a node without deleting it. The flag is persisted in the registry; a
 *        disabled node's data is ignored and its units are marked absent.
 * @param[in] mac       Node MAC address (6 bytes).
 * @param[in] disabled  true to disable, false to enable.
 */
void     esp_now_hub_set_node_disabled(const uint8_t mac[6], bool disabled);  /* runtime silence/resume a node */
/**
 * @brief Build the ESP-NOW status JSON (GET /api/v1/espnow): hub counters, and per registered node its
 *        online/disabled state, last-seen age (s), sync health and per-channel unit presence.
 * @return malloc'd JSON string that the caller must free(), or NULL on allocation failure.
 */
char    *esp_now_hub_status_json(void);                              /* malloc'd status+health JSON; caller free()s */
/** @brief (Re)publish the HA "Online" connectivity sensor and current state of every registered node. Call
 *         from the HA discovery task on each MQTT (re)connect; paced at 20 ms per node. */
void     esp_now_hub_publish_ha_nodes(void);                        /* (re)publish per-node HA "Online" sensors (call on MQTT connect) */

/**
 * @brief Fetch the status counters for the console verb (ESP_LOG output is not on the hub UART).
 *
 * Any output pointer may be NULL.
 * @param[out] channel           Current ESP-NOW channel.
 * @param[out] beacons_sent      Beacons sent since start.
 * @param[out] rt_recv           REALTIME frames received.
 * @param[out] stats_recv        NODE_STATS frames received.
 * @param[out] last_i0           Last node current, record 0 (A).
 * @param[out] last_i1           Last node current, record 1 (A).
 * @param[out] node_sd           Last node clock-fit residual standard deviation (µs).
 * @param[out] units_created     Units created at runtime (kept for compatibility; units are config units).
 * @param[out] periods_recv      PERIOD frames queued.
 * @param[out] periods_injected  PERIOD records injected into rbpower.
 */
void esp_now_hub_get_stats(uint8_t *channel, uint32_t *beacons_sent, uint32_t *rt_recv,
                           uint32_t *stats_recv, float *last_i0, float *last_i1, float *node_sd,
                           uint32_t *units_created, uint32_t *periods_recv, uint32_t *periods_injected);

#ifdef __cplusplus
}
#endif

#endif /* ESP_NOW_HUB_H */
