/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file esp_now_hub.c
 * @brief Hub-side ESP-NOW transport: beacon time master, node data receiver and injector.
 *
 * Coexists with the hub's running Wi-Fi (web server, MQTT). The hub is the ESP-NOW beacon master (the
 * time-sync source that nodes discipline their clocks to). It receives node REALTIME, PERIOD, TIME_REQ,
 * PAIR_REQ and NODE_STATS frames and feeds the data into the configured units through the same injection
 * API that the rbAmp bridge uses. ESP-NOW wireless nodes are available in the code but are not part of the
 * v1.1.0 release feature set.
 *
 * Calling contexts:
 *  - on_send() / on_recv() run in the Wi-Fi task: they only copy data into tables under s_units_mux.
 *  - beacon_task sends the 1 s beacon, finishes pairings (flash write) and watches the tariff zone.
 *  - inject_task feeds REALTIME/PERIOD data into rbpower and runs the presence-timeout sweep.
 *  - The public functions are called from the console, the HTTP server and the HA discovery task.
 */
#include "esp_now_hub.h"
#include "espnow_proto.h"
#include "espnow_crypto.h"  /* PMK + PAIR_REQ auth-tag verification */
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rbhome_types.h"   /* rbpower_home_create_unit / inject_200ms / set_present / set_energy_mode */
#include "rbgrid.h"         /* rbgrid_create_espnow_unit_id */
#include "rbpower.h"        /* rbtime_get_unix_timestamp / RBTIME_IS_SYNCED (TIME_RESP) */
#include "rbhome_tariff.h"  /* rbpower_tariff_get_current_zone — auto LATCH_NOW on a zone transition */
#include "littlefs_raw.h"   /* persisted node registry /espnow_nodes.bin */
#include "rbmqtt.h"         /* publish a node online/offline event */
#include "cJSON.h"          /* /api/v1/espnow status JSON */
#include "esp_mac.h"        /* esp_read_mac — hub MAC for the HA node sensor device id */
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "ESPNOW_HUB";

/** @brief Sync beacon period (ms). */
#define HUB_BEACON_MS 1000

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static bool     s_running = false;
static uint8_t  s_channel = 0;
static uint32_t s_epoch = 0;
static uint32_t s_beacon_seq = 0;
static uint32_t s_beacon_id = 0;
static volatile uint32_t s_pending_beacon_id = 0;
static TaskHandle_t s_beacon_task = NULL;

/* status counters */
static uint32_t s_beacons_sent = 0;
static uint32_t s_rt_recv = 0, s_stats_recv = 0;
static float    s_last_i0 = 0, s_last_i1 = 0, s_last_i2 = 0;
static float    s_last_node_sd = 0, s_last_node_skew = 0;
static uint32_t s_last_node_fits = 0;

/* Per (node MAC, channel) unit table. The recv callback (WiFi task) stores the latest sample; a
 * dedicated inject task creates the unit on first sight + injects into the real home network (the heavy
 * create/inject must not run in the recv callback). Handles BOTH node types: a voltage-bearing "energy
 * module" sends real v/p (energy ON_DEVICE); a current-only module sends I only → hub computes P=V×I (HOST). */
/* One slot for every channel the registry can hold. A smaller table left some units without realtime data,
 * and a quarter-hour boundary where all nodes latch together overflowed the period queue. */
/** @brief Unit and period table size: one slot per registry channel. */
#define ENH_MAX_UNITS    (ESPNOW_REG_MAX_NODES * ESPNOW_REG_MAX_CH)
/** @brief Fallback voltage (V) for current-only nodes with no voltage source (unused; see rbgrid_nominal_voltage_v()). */
#define ENH_NOMINAL_V    230.0f   /* fallback voltage for current-only nodes with no V source */
/** @brief Nominal mains voltage (V) from the hardware config, defined in main.cpp (fallback 230 V).
 *  @return Nominal voltage in V. */
extern float rbgrid_nominal_voltage_v(void);   /* main.cpp: hardware-config nominal (fallback 230) */
/** @brief No REALTIME for this long (ms) marks the unit absent (nodes send REALTIME about every 1 s). */
#define ESPNOW_PRESENCE_TIMEOUT_MS 15000   /* no REALTIME for this long → mark the unit absent (REALTIME ~1s) */
/** @brief Latest REALTIME sample of one (node MAC, channel), written by on_recv, consumed by inject_task. */
typedef struct {
    bool     used, created, has_v;
    uint8_t  mac[6], ch;
    rbpower_unit_id_t unit_id;
    float    i, v, p, pf;
    int64_t  last_us;          /* esp_timer at last REALTIME — drives the presence-timeout sweep */
    volatile bool fresh;
} enh_unit_t;
/** @var enh_unit_t::used
 *  Slot in use.  @var enh_unit_t::created
 *  Unused (units are config units).  @var enh_unit_t::has_v
 *  The node reported a voltage (energy module), so its own power is used.
 *  @var enh_unit_t::mac
 *  Node MAC.  @var enh_unit_t::ch
 *  Node channel_id.  @var enh_unit_t::unit_id
 *  Unused (the unit_id is derived from MAC and channel).
 *  @var enh_unit_t::i
 *  RMS current (A).  @var enh_unit_t::v
 *  RMS voltage (V).  @var enh_unit_t::p
 *  Active power (W).  @var enh_unit_t::pf
 *  Power factor (0..1).  @var enh_unit_t::last_us
 *  esp_timer time (µs) of the last REALTIME.  @var enh_unit_t::fresh
 *  A new sample is waiting for inject_task. */
static enh_unit_t s_units[ENH_MAX_UNITS];
static portMUX_TYPE s_units_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_units_created = 0;
static TaskHandle_t s_inject_task = NULL;

/* Per-node runtime state: liveness (online), user disable, and the latest sync health from
 * NODE_STATS. Keyed by MAC; the authoritative node/channel list is the persisted registry. */
/** @brief Per-node runtime state (liveness, disable flag, latest NODE_STATS sync health). */
typedef struct {
    bool     used, online, disabled;
    uint8_t  mac[6];
    int64_t  last_rt_us, last_stats_us;
    float    resid_sd, resid_maxabs, skew_ppm;
    uint32_t fits, beacons_recv, max_beacon_id;   /* loss% = (max_id - beacons_recv)/max_id */
} enh_node_t;
/** @var enh_node_t::used
 *  Slot in use.  @var enh_node_t::online
 *  Node sent REALTIME within ESPNOW_PRESENCE_TIMEOUT_MS.  @var enh_node_t::disabled
 *  Disabled by the user (data ignored).  @var enh_node_t::mac
 *  Node MAC.  @var enh_node_t::last_rt_us
 *  esp_timer time (µs) of the last REALTIME.  @var enh_node_t::last_stats_us
 *  esp_timer time (µs) of the last NODE_STATS.  @var enh_node_t::resid_sd
 *  Clock-fit residual standard deviation (µs).  @var enh_node_t::resid_maxabs
 *  Clock-fit maximum absolute residual (µs).  @var enh_node_t::skew_ppm
 *  Node clock skew against the hub (ppm).  @var enh_node_t::fits
 *  Number of clock fits done by the node.  @var enh_node_t::beacons_recv
 *  Beacons received by the node.  @var enh_node_t::max_beacon_id
 *  Highest beacon id seen by the node (loss % = (max_id - received) / max_id). */
static enh_node_t s_nodes[ESPNOW_REG_MAX_NODES];

static enh_node_t *enh_node_slot(const uint8_t *mac)
{
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (!s_nodes[i].used) { memset(&s_nodes[i], 0, sizeof(s_nodes[i])); s_nodes[i].used = true;
                                memcpy(s_nodes[i].mac, mac, 6); return &s_nodes[i]; }
    return NULL;
}
static bool enh_node_is_disabled(const uint8_t *mac)
{
    enh_node_t *n = NULL;
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) { n = &s_nodes[i]; break; }
    return n && n->disabled;
}

/* forward decls (used in beacon_task, defined later near the other MQTT/HA helpers) */
static void espnow_publish_ha_node(const uint8_t mac[6], bool remove);
static void espnow_publish_status(const uint8_t mac[6], bool online);

/* Pending node PERIOD records (queued in the recv cb, injected by the inject task). */
/** @brief One queued node PERIOD record. */
typedef struct { bool used; uint8_t mac[6]; rbn_period_rec_t rec; } enh_period_t;
/** @var enh_period_t::used
 *  Slot in use.  @var enh_period_t::mac
 *  Sender node MAC.  @var enh_period_t::rec
 *  The period record as received. */
static enh_period_t s_periods[ENH_MAX_UNITS];
static uint32_t s_periods_recv = 0, s_periods_injected = 0;

/* Over-the-air pairing. A pairing window accepts PAIR_REQ from new nodes → writes the registry. The recv cb
 * just stashes the request; the beacon task does the flash write + PAIR_ACK (off the WiFi callback). */
static int64_t  s_pair_window_until = 0;     /* esp_timer deadline (0 = closed) */
static uint32_t s_paired_count = 0;          /* nodes paired this session → reboot to register */
static volatile bool s_pending_pair = false;
static uint8_t  s_pending_pair_mac[6];
static espnow_reg_chan_t s_pending_pair_ch[ESPNOW_REG_MAX_CH];
static uint8_t  s_pending_pair_n = 0;
/* Tariff-zone watch for auto LATCH_NOW (poll in beacon_task; throttled to ~every 3s). */
static rbpower_tariff_zone_type_t s_last_zone_id = 0;
static bool     s_zone_valid = false;
static uint32_t s_zone_poll_div = 0;
static const uint32_t s_zone_poll_period = (3000 / HUB_BEACON_MS) ? (3000 / HUB_BEACON_MS) : 1;

/* find or allocate a table slot for (mac,ch). Called from the recv callback — keep it short. */
static enh_unit_t *enh_slot(const uint8_t *mac, uint8_t ch)
{
    for (int i = 0; i < ENH_MAX_UNITS; i++)
        if (s_units[i].used && s_units[i].ch == ch && memcmp(s_units[i].mac, mac, 6) == 0) return &s_units[i];
    for (int i = 0; i < ENH_MAX_UNITS; i++)
        if (!s_units[i].used) { memset(&s_units[i], 0, sizeof(s_units[i])); s_units[i].used = true;
                                memcpy(s_units[i].mac, mac, 6); s_units[i].ch = ch; return &s_units[i]; }
    return NULL;  /* table full */
}

/* ===== Persisted ESP-NOW node registry (/espnow_nodes.bin) ===== */
/** @brief LittleFS path of the node registry. */
#define ESPNOW_REG_FILE "/espnow_nodes.bin"

bool esp_now_registry_load(espnow_registry_t *reg)
{
    memset(reg, 0, sizeof(*reg));
    if (!littlefs_raw_exists(ESPNOW_REG_FILE)) return false;
    size_t act = 0;
    if (littlefs_raw_read(ESPNOW_REG_FILE, reg, sizeof(*reg), &act) != ESP_OK || act < 2) {
        memset(reg, 0, sizeof(*reg)); return false;
    }
    if (reg->version != ESPNOW_REG_VERSION) {   /* format bump (e.g. v1→v2 node flags) — byte layout changed */
        ESP_LOGW(TAG, "ESP-NOW registry format v%u != v%u → discarding; re-pair nodes",
                 reg->version, ESPNOW_REG_VERSION);
        memset(reg, 0, sizeof(*reg)); return false;
    }
    if (reg->node_count > ESPNOW_REG_MAX_NODES) reg->node_count = ESPNOW_REG_MAX_NODES;
    return true;
}

esp_err_t esp_now_registry_save(const espnow_registry_t *reg)
{
    return littlefs_raw_write(ESPNOW_REG_FILE, (const void *)reg, sizeof(*reg));
}

esp_err_t esp_now_registry_add_node(const uint8_t mac[6], const espnow_reg_chan_t *channels, uint8_t n)
{
    if (!mac || !channels || n == 0) return ESP_ERR_INVALID_ARG;
    if (n > ESPNOW_REG_MAX_CH) n = ESPNOW_REG_MAX_CH;
    espnow_registry_t reg; esp_now_registry_load(&reg); reg.version = ESPNOW_REG_VERSION;
    int idx = -1;
    for (int i = 0; i < reg.node_count; i++) if (memcmp(reg.nodes[i].mac, mac, 6) == 0) { idx = i; break; }
    if (idx < 0) {
        if (reg.node_count >= ESPNOW_REG_MAX_NODES) return ESP_ERR_NO_MEM;
        idx = reg.node_count++;
    }
    memset(&reg.nodes[idx], 0, sizeof(reg.nodes[idx]));
    memcpy(reg.nodes[idx].mac, mac, 6);
    reg.nodes[idx].ch_count = n;
    for (int i = 0; i < n; i++) reg.nodes[idx].channels[i] = channels[i];
    return esp_now_registry_save(&reg);
}

esp_err_t esp_now_registry_remove_node(const uint8_t mac[6])
{
    espnow_registry_t reg;
    if (!esp_now_registry_load(&reg)) return ESP_ERR_NOT_FOUND;
    int idx = -1;
    for (int i = 0; i < reg.node_count; i++) if (memcmp(reg.nodes[i].mac, mac, 6) == 0) { idx = i; break; }
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    for (int i = idx; i < reg.node_count - 1; i++) reg.nodes[i] = reg.nodes[i + 1];
    reg.node_count--;
    return esp_now_registry_save(&reg);
}

/* persist a node's disabled flag (survives reboot). */
static esp_err_t esp_now_registry_set_disabled(const uint8_t mac[6], bool disabled)
{
    espnow_registry_t reg;
    if (!esp_now_registry_load(&reg)) return ESP_ERR_NOT_FOUND;
    for (int i = 0; i < reg.node_count; i++)
        if (memcmp(reg.nodes[i].mac, mac, 6) == 0) {
            if (disabled) reg.nodes[i].flags |= ESPNOW_REG_FLAG_DISABLED;
            else          reg.nodes[i].flags &= ~ESPNOW_REG_FLAG_DISABLED;
            return esp_now_registry_save(&reg);
        }
    return ESP_ERR_NOT_FOUND;
}

static wifi_interface_t hub_ifidx(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_wifi_get_mode(&m);
    return (m == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
}

/** Add or refresh a node as an ENCRYPTED (CCMP) unicast peer, keyed by LMK = HMAC(PMK, node_mac). The node
 * derives the same LMK from its own MAC, so encrypted data frames flow both ways without a key on the air.
 * The broadcast beacon/followup stay on the separate unencrypted BCAST peer. */
static void hub_add_enc_peer(const uint8_t mac[6])
{
    esp_now_peer_info_t p = { .channel = s_channel, .ifidx = hub_ifidx(), .encrypt = true };
    memcpy(p.peer_addr, mac, 6);
    rbn_derive_lmk(mac, p.lmk);
    esp_err_t e = esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&p) : esp_now_add_peer(&p);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "enc-peer %02X:%02X:%02X:%02X:%02X:%02X: %s", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
                 esp_err_to_name(e));
}

/** TX done (Wi-Fi task): if it was the beacon, send its SYNC_FOLLOWUP with the egress esp_timer (two-step PTP). */
static void on_send(const uint8_t *mac, esp_now_send_status_t status)
{
    int64_t t_tx = esp_timer_get_time();
    uint32_t bid = s_pending_beacon_id;
    s_pending_beacon_id = 0;
    if (bid) {
        rbn_sync_followup_t f;
        rbn_hdr_init(&f.h, RBN_MSG_SYNC_FOLLOWUP, s_beacon_seq++, 0);
        f.beacon_id = bid;
        f.hub_tx_us = (uint64_t)t_tx;
        esp_now_send(BCAST, (uint8_t *)&f, sizeof(f));
    }
}

/** Receive callback (Wi-Fi task): validate the header and dispatch by message type. Only copies data into
 *  the unit/period/node tables (under s_units_mux) or sends a short reply; heavy work runs in the tasks. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(rbn_hdr_t)) return;
    const rbn_hdr_t *h = (const rbn_hdr_t *)data;
    if (!rbn_hdr_ok(h)) return;
    const uint8_t *mac = info->src_addr;

    if (h->msg_type == RBN_MSG_REALTIME && len >= (int)sizeof(rbn_realtime_t)) {
        const rbn_realtime_t *m = (const rbn_realtime_t *)data;
        int n = m->rec_count; if (n > 3) n = 3;
        if (n >= 1 && len >= (int)(sizeof(rbn_realtime_t) + (size_t)n * sizeof(rbn_rt_rec_t))) {
            s_rt_recv++;
            s_last_i0 = m->recs[0].i_rms;
            s_last_i1 = (n > 1) ? m->recs[1].i_rms : 0;
            s_last_i2 = (n > 2) ? m->recs[2].i_rms : 0;
            /* Stash each channel's latest sample for the inject task (create unit + inject off-cb). */
            int64_t now = esp_timer_get_time();
            portENTER_CRITICAL(&s_units_mux);
            for (int r = 0; r < n; r++) {
                const rbn_rt_rec_t *rec = &m->recs[r];
                enh_unit_t *u = enh_slot(mac, rec->channel_id);
                if (u) { u->i = rec->i_rms; u->v = rec->v_rms; u->p = rec->p_active;
                         u->pf = rec->pf_x1000 / 1000.0f; u->has_v = (rec->v_rms > 0.5f);
                         u->last_us = now; u->fresh = true; }
            }
            enh_node_t *nd = enh_node_slot(mac);    /* node liveness heartbeat */
            if (nd) nd->last_rt_us = now;
            portEXIT_CRITICAL(&s_units_mux);
            ESP_LOGI(TAG, "RBN_RT from %02X:%02X:%02X:%02X:%02X:%02X ch=%d i0=%.3fA i1=%.3fA i2=%.3fA v0=%.1f p0=%.1f",
                     mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], m->rec_count,
                     s_last_i0, s_last_i1, s_last_i2, m->recs[0].v_rms, m->recs[0].p_active);
        }
    } else if (h->msg_type == RBN_MSG_PERIOD && len >= (int)sizeof(rbn_period_t)) {
        const rbn_period_t *m = (const rbn_period_t *)data;
        int n = m->rec_count; if (n > ENH_MAX_UNITS) n = ENH_MAX_UNITS;
        if (n >= 1 && len >= (int)(sizeof(rbn_period_t) + (size_t)n * sizeof(rbn_period_rec_t))) {
            /* All or nothing, and ACK only what is queued: the node deletes a held record on ACK, so an ACK for a
             * record dropped on a full queue would lose that energy. Not stored → no ACK → the node resends. */
            bool stored = false;
            portENTER_CRITICAL(&s_units_mux);
            int free_slots = 0;
            for (int k = 0; k < ENH_MAX_UNITS; k++) if (!s_periods[k].used) free_slots++;
            if (free_slots >= n) {
                for (int r = 0; r < n; r++)
                    for (int k = 0; k < ENH_MAX_UNITS; k++)
                        if (!s_periods[k].used) { s_periods[k].used = true; memcpy(s_periods[k].mac, mac, 6);
                                                  s_periods[k].rec = m->recs[r]; break; }
                s_periods_recv++;
                stored = true;
            }
            portEXIT_CRITICAL(&s_units_mux);
            if (!stored) { ESP_LOGW(TAG, "PERIOD from %02X:..:%02X: queue full — not ACKed, node will resend", mac[0], mac[5]); return; }
            /* held-until-ACK: acknowledge receipt immediately (the inject happens in the task). Reply
             * UNICAST to the sender (a registered encrypted peer) → the ACK is CCMP-protected too. */
            rbn_period_ack_t ack; rbn_hdr_init(&ack.h, RBN_MSG_PERIOD_ACK, s_beacon_seq++, 0);
            ack.ack_seq = h->seq;
            esp_now_send(mac, (uint8_t *)&ack, sizeof(ack));
        }
    } else if (h->msg_type == RBN_MSG_PAIR_REQ && len >= (int)sizeof(rbn_pair_req_t)) {
        /* Accept a new node only inside the pairing window; stash it for the beacon task (flash write). */
        if (esp_timer_get_time() < s_pair_window_until && !s_pending_pair) {
            const rbn_pair_req_t *p = (const rbn_pair_req_t *)data;
            int n = p->chan_count;
            /* The full frame (incl. the 8-byte auth tag) must be present, AND the tag must verify against
             * the shared pairing key — else this is a malformed or rogue PAIR_REQ; drop it silently. */
            if (n >= 1 && n <= ESPNOW_REG_MAX_CH && len >= (int)RBN_PAIR_REQ_LEN(n)) {
                uint8_t tag[RBN_TAG_LEN];
                rbn_pair_tag(data, RBN_PAIR_REQ_PREFIX_LEN(n), tag);
                if (rbn_tag_equal(tag, data + RBN_PAIR_REQ_PREFIX_LEN(n))) {
                    memcpy(s_pending_pair_mac, mac, 6);
                    for (int i = 0; i < n; i++) { s_pending_pair_ch[i].ch = p->chans[i].channel_id;
                                                  s_pending_pair_ch[i].ct_code = p->chans[i].ct_code; }
                    s_pending_pair_n = (uint8_t)n;
                    s_pending_pair = true;
                } else {
                    ESP_LOGW(TAG, "PAIR_REQ from %02X:%02X:%02X:%02X:%02X:%02X REJECTED — bad auth tag",
                             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                }
            }
        }
    } else if (h->msg_type == RBN_MSG_TIME_REQ && len >= (int)sizeof(rbn_time_req_t)) {
        /* Give the node the hub's wall-clock so it can latch periods on calendar boundaries. */
        const rbn_time_req_t *q = (const rbn_time_req_t *)data;
        rbn_time_resp_t r;
        rbn_hdr_init(&r.h, RBN_MSG_TIME_RESP, s_beacon_seq++, 0);
        r.req_token = q->req_token;
        r.hub_unix_ms = RBTIME_IS_SYNCED() ? (uint64_t)rbtime_get_unix_timestamp() * 1000ULL : 0;
        esp_now_send(mac, (uint8_t *)&r, sizeof(r));   /* unicast, encrypted to the registered node */
    } else if (h->msg_type == RBN_MSG_NODE_STATS && len >= (int)sizeof(rbn_node_stats_t)) {
        const rbn_node_stats_t *s = (const rbn_node_stats_t *)data;
        s_stats_recv++;
        s_last_node_sd = s->resid_sd_us; s_last_node_skew = s->skew_ppm; s_last_node_fits = s->fits;
        portENTER_CRITICAL(&s_units_mux);            /* store per-node sync health for /api/v1/espnow */
        enh_node_t *nd = enh_node_slot(mac);
        if (nd) { nd->last_stats_us = esp_timer_get_time(); nd->resid_sd = s->resid_sd_us;
                  nd->resid_maxabs = s->resid_maxabs_us; nd->skew_ppm = s->skew_ppm; nd->fits = s->fits;
                  nd->beacons_recv = s->beacons_recv; nd->max_beacon_id = s->max_beacon_id; }
        portEXIT_CRITICAL(&s_units_mux);
        ESP_LOGI(TAG, "RBN_NODESTATS from %02X:%02X:%02X:%02X:%02X:%02X fits=%u resid_sd=%.1fus maxabs=%.1fus skew_ppm=%.2f",
                 mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
                 (unsigned)s->fits, s->resid_sd_us, s->resid_maxabs_us, s->skew_ppm);
    }
}

/** Beacon task (1 s period): finish a pending pairing, watch the tariff zone, send the SYNC_BEACON. */
static void beacon_task(void *arg)
{
    while (s_running) {
        /* Finalize a pending pairing off the recv callback (flash write + ACK). */
        if (s_pending_pair) {
            esp_err_t e = esp_now_registry_add_node(s_pending_pair_mac, s_pending_pair_ch, s_pending_pair_n);
            rbn_pair_ack_t ack; rbn_hdr_init(&ack.h, RBN_MSG_PAIR_ACK, s_beacon_seq++, 0);
            ack.accepted = (e == ESP_OK) ? 1 : 0;
            esp_now_send(BCAST, (uint8_t *)&ack, sizeof(ack));
            if (e == ESP_OK) {
                s_paired_count++;
                hub_add_enc_peer(s_pending_pair_mac);   /* encrypt this node's link immediately */
                espnow_publish_ha_node(s_pending_pair_mac, false);   /* HA "Online" sensor for the new node */
                ESP_LOGW(TAG, "PAIRED node %02X:%02X:%02X:%02X:%02X:%02X (%u ch) → registry; run `reboot` to register its units",
                         s_pending_pair_mac[0],s_pending_pair_mac[1],s_pending_pair_mac[2],
                         s_pending_pair_mac[3],s_pending_pair_mac[4],s_pending_pair_mac[5], s_pending_pair_n);
            }
            s_pending_pair = false;
        }
        /* Watch the active tariff zone; on a transition, command all nodes to latch so their period boundary
         * coincides with the zone boundary (the node can't know zones). Throttled to ~every 3s. */
        if (++s_zone_poll_div >= s_zone_poll_period) {
            s_zone_poll_div = 0;
            if (RBTIME_IS_SYNCED()) {
                rbpower_tariff_zone_type_t zid = 0; char zname[RBPOWER_TARIFF_NAME_LENGTH]; float rate = 0;
                if (rbpower_tariff_get_current_zone(&zid, zname, &rate) == RBPOWER_OK) {
                    if (s_zone_valid && zid != s_last_zone_id) {
                        ESP_LOGW(TAG, "tariff zone %d→%d → LATCH_NOW all nodes", (int)s_last_zone_id, (int)zid);
                        esp_now_hub_latch_all(RBN_LATCH_REASON_ZONE);
                    }
                    s_last_zone_id = zid; s_zone_valid = true;
                }
            }
        }
        s_beacon_id++;
        rbn_sync_beacon_t b;
        rbn_hdr_init(&b.h, RBN_MSG_SYNC_BEACON, s_beacon_seq++, 0);
        b.beacon_id = s_beacon_id;
        b.epoch = s_epoch;
        b.hub_channel = s_channel;
        s_pending_beacon_id = s_beacon_id;
        esp_now_send(BCAST, (uint8_t *)&b, sizeof(b));   /* on_send → SYNC_FOLLOWUP */
        s_beacons_sent++;
        vTaskDelay(pdMS_TO_TICKS(HUB_BEACON_MS));
    }
    s_beacon_task = NULL;
    vTaskDelete(NULL);
}

/** Build a node's status topic. Node topics live UNDER the hub device: rbgrid/<dev>/node/<NODEMAC>/status
 * (<dev> = hub MAC lowercase, same id as the hub's telemetry/status/cmd topics). Older firmware used
 * rbgrid/espnow/<MAC>/status; retained messages on that topic should be cleared once (mosquitto_pub -r -n),
 * otherwise they linger as orphans. The event/alarms topic names under rbgrid/<dev>/ are reserved. */
static void espnow_node_status_topic(const uint8_t mac[6], char* out, size_t cap)
{
    uint8_t hub[6]; esp_read_mac(hub, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "rbgrid/%02x%02x%02x%02x%02x%02x/node/%02X%02X%02X%02X%02X%02X/status",
             hub[0],hub[1],hub[2],hub[3],hub[4],hub[5],
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

/** Publish a node's online state to its retained status topic (web app/HA + the HA connectivity sensor). */
static void espnow_publish_status(const uint8_t mac[6], bool online)
{
    if (!rbmqtt_is_connected()) return;
    char topic[64], payload[24];
    espnow_node_status_topic(mac, topic, sizeof(topic));
    snprintf(payload, sizeof(payload), "{\"online\":%s}", online ? "true" : "false");
    rbmqtt_publish(topic, payload, 1, true);   /* retained → last-known state for new subscribers */
}
/** Signal an online/offline transition: log + the retained status event. */
static void espnow_emit_presence(const uint8_t mac[6], bool online)
{
    ESP_LOGW(TAG, "node %02X:%02X:%02X:%02X:%02X:%02X is %s",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], online ? "ONLINE" : "OFFLINE");
    espnow_publish_status(mac, online);
}

/** Publish a HA Discovery config for a node's "Online" connectivity binary_sensor (sub-device under
 * the hub). State comes from the node's retained status topic (value_json.online). remove=true clears it
 * (empty retained config) so a deleted node's entity disappears from HA. Self-contained: uses the hub MAC +
 * the node MAC + rbmqtt — no dependency on the rbpower HA-discovery code. */
static void espnow_publish_ha_node(const uint8_t mac[6], bool remove)
{
    if (!rbmqtt_is_connected()) return;
    uint8_t hub[6]; esp_read_mac(hub, ESP_MAC_WIFI_STA);
    char hubhex[13], nlo[13];
    snprintf(hubhex, sizeof(hubhex), "%02x%02x%02x%02x%02x%02x", hub[0],hub[1],hub[2],hub[3],hub[4],hub[5]);
    snprintf(nlo, sizeof(nlo), "%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    char topic[120];
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/rbgrid_%s_espnow_%s/config", hubhex, nlo);
    if (remove) { rbmqtt_publish(topic, "", 0, true); return; }   /* empty retained → delete the entity */
    cJSON *c = cJSON_CreateObject();
    char buf[48], st[64];
    snprintf(buf, sizeof(buf), "rbgrid_%s_espnow_%s", hubhex, nlo);
    cJSON_AddStringToObject(c, "uniq_id", buf);
    snprintf(buf, sizeof(buf), "Node %02X:%02X:%02X:%02X:%02X:%02X Online",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    cJSON_AddStringToObject(c, "name", buf);
    cJSON_AddStringToObject(c, "dev_cla", "connectivity");
    espnow_node_status_topic(mac, st, sizeof(st));   /* rbgrid/<dev>/node/<MAC>/status */
    cJSON_AddStringToObject(c, "stat_t", st);
    cJSON_AddStringToObject(c, "val_tpl", "{{ 'ON' if value_json.online else 'OFF' }}");
    cJSON_AddStringToObject(c, "pl_on", "ON");
    cJSON_AddStringToObject(c, "pl_off", "OFF");
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    snprintf(buf, sizeof(buf), "%s_espnow_%s", hubhex, nlo);
    cJSON_AddItemToArray(ids, cJSON_CreateString(buf));
    cJSON_AddItemToObject(dev, "ids", ids);
    snprintf(buf, sizeof(buf), "RBGrid Node %02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    cJSON_AddStringToObject(dev, "name", buf);
    cJSON_AddStringToObject(dev, "mf", "RBGrid");
    cJSON_AddStringToObject(dev, "mdl", "ESP-NOW Node");
    cJSON_AddStringToObject(dev, "via_device", hubhex);   /* nest under the hub device */
    cJSON_AddItemToObject(c, "dev", dev);
    char *json = cJSON_PrintUnformatted(c);
    cJSON_Delete(c);
    if (json) { rbmqtt_publish(topic, json, 0, true); free(json); }
}

/* (Re)publish the HA "Online" sensor + current state for every registered node. Called from the HA
 * discovery task on each MQTT (re)connect, alongside rbgrid_json_publish_ha_discovery. */
void esp_now_hub_publish_ha_nodes(void)
{
    espnow_registry_t reg;
    if (!esp_now_registry_load(&reg)) return;
    for (int i = 0; i < reg.node_count; i++) {
        espnow_publish_ha_node(reg.nodes[i].mac, false);
        bool online = false;
        portENTER_CRITICAL(&s_units_mux);
        for (int k = 0; k < ESPNOW_REG_MAX_NODES; k++)
            if (s_nodes[k].used && memcmp(s_nodes[k].mac, reg.nodes[i].mac, 6) == 0) { online = s_nodes[k].online; break; }
        portEXIT_CRITICAL(&s_units_mux);
        espnow_publish_status(reg.nodes[i].mac, online);   /* initial state so the sensor isn't 'unknown' */
        vTaskDelay(pdMS_TO_TICKS(20));                       /* pace, like the rbpower HA publisher */
    }
}

/** Inject task (200 ms loop) — runs OFF the recv callback. Drains REALTIME (200ms inject) + PERIOD (period_result).
 * Units are CONFIG units (rbgrid_config_add_espnow_module) — we only compute the unit_id and feed; the
 * inject_* APIs no-op if the unit isn't configured (i.e. an unpaired/unknown node). No runtime create. */
static void inject_task(void *arg)
{
    uint32_t sweep_div = 0;
    while (s_running) {
        /* --- REALTIME --- */
        for (int i = 0; i < ENH_MAX_UNITS; i++) {
            portENTER_CRITICAL(&s_units_mux);
            bool go = s_units[i].used && s_units[i].fresh;
            enh_unit_t snap = s_units[i];
            s_units[i].fresh = false;
            portEXIT_CRITICAL(&s_units_mux);
            if (!go) continue;
            if (enh_node_is_disabled(snap.mac)) continue;   /* disabled node: ignore its data */

            /* unit_id is deterministic from (node MAC, channel); the unit is a CONFIG unit registered at
             * boot (rbgrid_config_add_espnow_module). inject_* no-ops if it isn't configured. No runtime create. */
            rbpower_unit_id_t uid = rbgrid_create_espnow_unit_id(snap.mac, snap.ch);
            /* mark the config unit present + its energy provenance now that its node is live (idempotent). */
            rbpower_set_unit_hardware_present(uid, true);
            rbpower_set_unit_energy_mode(uid, snap.has_v ? RBPOWER_ENERGY_MODE_ON_DEVICE
                                                         : RBPOWER_ENERGY_MODE_HOST);

            /* energy module → real v/p; current-only → P = V_nominal × I. */
            float v   = snap.has_v ? snap.v : rbgrid_nominal_voltage_v();   /* was: fixed ENH_NOMINAL_V */
            float i_a = snap.i;
            float p   = (fabsf(snap.p) > 0.01f) ? snap.p : (v * i_a);
            float pf  = (snap.pf > 0.01f) ? snap.pf : 1.0f;
            rbpower_unit_realtime_average_200ms_t rt; memset(&rt, 0, sizeof(rt));
            rt.is_valid = true;
            rt.last_update_us = esp_timer_get_time();
            rt.average_200ms.samples_count         = 1;
            rt.average_200ms.voltage_rms_avg       = v;
            rt.average_200ms.current_rms_avg       = i_a;
            rt.average_200ms.power_active_avg_w    = p;
            rt.average_200ms.power_apparent_avg_va = v * i_a;
            rt.average_200ms.power_factor_avg      = pf;
            rt.instant_power_net_w = p;
            rbpower_home_inject_200ms(uid, &rt);
        }
        /* --- PERIOD: node's finished W/Wh → history/billing --- */
        for (int i = 0; i < ENH_MAX_UNITS; i++) {
            portENTER_CRITICAL(&s_units_mux);
            bool pgo = s_periods[i].used;
            enh_period_t ps = s_periods[i];
            if (pgo) s_periods[i].used = false;
            portEXIT_CRITICAL(&s_units_mux);
            if (!pgo) continue;
            /* A disabled node's finished periods must not flow into history/billing either:
             * same gate as for REALTIME. */
            if (enh_node_is_disabled(ps.mac)) continue;

            rbpower_unit_id_t uid = rbgrid_create_espnow_unit_id(ps.mac, ps.rec.channel_id);
            /* Feed the node's finished 15MIN period into the native rollup (unit_id-based, shared with the
             * rbAmp inject path): inject_period_result(15MIN) → rbpower_rollup_after_15min → 1HOUR accumulator
             * → finalize_1hour → flash + tariff/economics. Uniform with config-time rbAmp units (the unit
             * MUST be a fully-registered config unit so it has a flash buffer — runtime auto-create gave none). */
            rbpower_home_inject_period_result(uid, (rbpower_stat_period_type_t)ps.rec.period_type,
                ps.rec.avg_power_w, ps.rec.max_power_w, ps.rec.energy_wh, ps.rec.period_ms,
                ps.rec.voltage_v, ps.rec.current_avg_a, ps.rec.current_min_a, ps.rec.current_max_a);
            s_periods_injected++;
            ESP_LOGW(TAG, "PERIOD inject unit=0x%04X type=%u avg=%.2fW E=%.4fWh dt=%lums", (unsigned)uid,
                     ps.rec.period_type, ps.rec.avg_power_w, ps.rec.energy_wh, (unsigned long)ps.rec.period_ms);
        }

        /* Presence-timeout sweep (~1s): a unit with no REALTIME for ESPNOW_PRESENCE_TIMEOUT_MS is flipped
         * absent (so telemetry/HA stop showing it live); per-node online↔offline edges fire an alert. */
        if (++sweep_div >= 5) {
            sweep_div = 0;
            int64_t now = esp_timer_get_time();
            const int64_t to_us = (int64_t)ESPNOW_PRESENCE_TIMEOUT_MS * 1000;
            for (int i = 0; i < ENH_MAX_UNITS; i++) {
                portENTER_CRITICAL(&s_units_mux);
                bool used = s_units[i].used; uint8_t mac[6]; uint8_t ch = s_units[i].ch; int64_t lu = s_units[i].last_us;
                if (used) memcpy(mac, s_units[i].mac, 6);
                portEXIT_CRITICAL(&s_units_mux);
                if (!used || lu == 0) continue;
                if ((now - lu) > to_us) {
                    rbpower_unit_id_t uid = rbgrid_create_espnow_unit_id(mac, ch);
                    if (rbpower_get_unit_hardware_present(uid)) rbpower_set_unit_hardware_present(uid, false);
                }
            }
            for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++) {
                portENTER_CRITICAL(&s_units_mux);
                bool used = s_nodes[i].used, was = s_nodes[i].online, dis = s_nodes[i].disabled;
                uint8_t mac[6]; int64_t lr = s_nodes[i].last_rt_us;
                if (used) memcpy(mac, s_nodes[i].mac, 6);
                bool now_on = used && !dis && lr != 0 && (now - lr) <= to_us;
                if (used) s_nodes[i].online = now_on;
                portEXIT_CRITICAL(&s_units_mux);
                if (used && now_on != was) espnow_emit_presence(mac, now_on);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_inject_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp_now_hub_start(uint8_t channel)
{
#if !RBN_HAVE_KEYS
    (void)channel;
    ESP_LOGW(TAG, "ESP-NOW hub not started: this image has no ESP-NOW keys (espnow_keys.h absent at build)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_running) { ESP_LOGW(TAG, "already running"); return ESP_OK; }

    if (channel > 0) {
        esp_err_t ce = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        ESP_LOGW(TAG, "forced WiFi/ESP-NOW channel %u: %s (nodes must match; may affect STA link)",
                 channel, esp_err_to_name(ce));
    }
    uint8_t prim = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&prim, &sec);
    s_channel = prim;

    esp_err_t e = esp_now_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(e)); return e; }
    /* Provision the shared Primary Master Key (used for the CCMP-encrypted unicast peers). */
    { static const uint8_t pmk[16] = RBN_PMK16; esp_now_set_pmk(pmk); }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);

    esp_now_peer_info_t peer = { .channel = s_channel, .ifidx = hub_ifidx(), .encrypt = false };
    memcpy(peer.peer_addr, BCAST, 6);
    esp_now_add_peer(&peer);

    /* Add every registered node as an encrypted unicast peer so its data frames (REALTIME/PERIOD/
     * TIME_REQ/NODE_STATS) are CCMP-protected. Unregistered nodes have no peer here → their encrypted data
     * can't be decrypted (dropped); they reach the hub only via the broadcast PAIR_REQ. */
    {
        espnow_registry_t reg;
        if (esp_now_registry_load(&reg)) {
            for (int i = 0; i < reg.node_count; i++) {
                hub_add_enc_peer(reg.nodes[i].mac);
                if (reg.nodes[i].flags & ESPNOW_REG_FLAG_DISABLED) {   /* apply persisted disable */
                    enh_node_t *nd = enh_node_slot(reg.nodes[i].mac);
                    if (nd) nd->disabled = true;
                    for (int c = 0; c < reg.nodes[i].ch_count; c++)
                        rbpower_set_unit_hardware_present(
                            rbgrid_create_espnow_unit_id(reg.nodes[i].mac, reg.nodes[i].channels[c].ch), false);
                    ESP_LOGW(TAG, "node %02X:%02X:%02X:%02X:%02X:%02X is DISABLED (persisted)",
                             reg.nodes[i].mac[0],reg.nodes[i].mac[1],reg.nodes[i].mac[2],
                             reg.nodes[i].mac[3],reg.nodes[i].mac[4],reg.nodes[i].mac[5]);
                }
            }
            if (reg.node_count) ESP_LOGW(TAG, "%d encrypted node peer(s) added", reg.node_count);
        }
    }

    /* epoch must differ every start (hub reboot/OTA) so nodes flush their servo. esp_random() alone can be
     * deterministic before the RNG is well-seeded; mixing esp_timer (uptime at the call, always different
     * across boots/invocations) guarantees uniqueness. */
    s_epoch = esp_random() ^ (uint32_t)esp_timer_get_time() ^ ((uint32_t)(esp_timer_get_time() >> 8));
    s_running = true;
    s_beacons_sent = s_rt_recv = s_stats_recv = 0;
    memset(s_units, 0, sizeof(s_units)); s_units_created = 0;
    memset(s_periods, 0, sizeof(s_periods)); s_periods_recv = s_periods_injected = 0;
    xTaskCreate(beacon_task, "espnow_beacon", 4096, NULL, 5, &s_beacon_task);
    xTaskCreate(inject_task, "espnow_inject", 4096, NULL, 4, &s_inject_task);
    ESP_LOGW(TAG, "ESP-NOW hub UP: ch=%u epoch=0x%08X beacon=%dms (beacon + recv + INJECT into units)",
             s_channel, (unsigned)s_epoch, HUB_BEACON_MS);
    return ESP_OK;
}

void esp_now_hub_stop(void)
{
    if (!s_running) return;
    s_running = false;                 /* beacon_task exits + self-deletes */
    vTaskDelay(pdMS_TO_TICKS(HUB_BEACON_MS + 100));
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    ESP_LOGW(TAG, "ESP-NOW hub stopped");
}

void esp_now_hub_status(void)
{
    ESP_LOGW(TAG, "espnow status: running=%d ch=%u epoch=0x%08X | beacons_sent=%u rt_recv=%u stats_recv=%u | "
                  "last node i0=%.3fA i1=%.3fA i2=%.3fA | node sync sd=%.1fus skew=%.2fppm fits=%u",
             s_running, s_channel, (unsigned)s_epoch, (unsigned)s_beacons_sent, (unsigned)s_rt_recv,
             (unsigned)s_stats_recv, s_last_i0, s_last_i1, s_last_i2, s_last_node_sd, s_last_node_skew,
             (unsigned)s_last_node_fits);
}

bool esp_now_hub_running(void) { return s_running; }

/* Open the pairing window for `seconds` (the hub accepts new-node PAIR_REQ during it). */
void esp_now_hub_open_pairing(uint32_t seconds)
{
    s_pair_window_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_paired_count = 0;
}
uint32_t esp_now_hub_paired_count(void) { return s_paired_count; }
bool     esp_now_hub_pairing_open(void) { return esp_timer_get_time() < s_pair_window_until; }

/* Command every registered node to latch its period NOW (encrypted unicast to each). Used on a tariff-zone
 * transition (auto) or from the `espnow latch` verb. @return node count commanded. */
uint32_t esp_now_hub_latch_all(uint8_t reason)
{
    if (!s_running) return 0;
    espnow_registry_t reg;
    if (!esp_now_registry_load(&reg) || reg.node_count == 0) return 0;
    rbn_latch_now_t l;
    rbn_hdr_init(&l.h, RBN_MSG_LATCH_NOW, s_beacon_seq++, 0);
    l.reason = reason;
    l.boundary_unix = RBTIME_IS_SYNCED() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    for (int i = 0; i < reg.node_count; i++) esp_now_send(reg.nodes[i].mac, (uint8_t *)&l, sizeof(l));
    ESP_LOGW(TAG, "LATCH_NOW → %d node(s) (reason=%u)", reg.node_count, reason);
    return reg.node_count;
}

/* Remove a node AND its units in one shot (reboot-free). For each of the node's registry channels, remove
 * the config sensor-unit (+ its flash slot), then drop the persistent registry entry + runtime state + peer. */
uint32_t esp_now_hub_delnode_cascade(const uint8_t mac[6])
{
    espnow_registry_t reg;
    uint32_t removed = 0;
    if (esp_now_registry_load(&reg)) {
        for (int i = 0; i < reg.node_count; i++) {
            if (memcmp(reg.nodes[i].mac, mac, 6) != 0) continue;
            for (int c = 0; c < reg.nodes[i].ch_count; c++) {
                rbpower_unit_id_t uid = rbgrid_create_espnow_unit_id(mac, reg.nodes[i].channels[c].ch);
                if (rbpower_home_remove_unit(uid) == RBPOWER_OK) removed++;
            }
            break;
        }
    }
    esp_now_registry_remove_node(mac);          /* drop the persistent entry */
    portENTER_CRITICAL(&s_units_mux);           /* clear runtime state */
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) s_nodes[i].used = false;
    for (int i = 0; i < ENH_MAX_UNITS; i++)
        if (s_units[i].used && memcmp(s_units[i].mac, mac, 6) == 0) s_units[i].used = false;
    portEXIT_CRITICAL(&s_units_mux);
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);   /* drop the encrypted peer */
    espnow_publish_ha_node(mac, true);                       /* remove the node's HA "Online" sensor */
    ESP_LOGW(TAG, "delnode cascade %02X:%02X:%02X:%02X:%02X:%02X → %u unit(s) removed",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], (unsigned)removed);
    return removed;
}

/* Disable: silence a node without deleting it (runtime; permanent removal = delnode_cascade).
 * Disabled → inject_task ignores its data and its units are forced absent. enable resumes the data flow. */
void esp_now_hub_set_node_disabled(const uint8_t mac[6], bool disabled)
{
    esp_now_registry_set_disabled(mac, disabled);   /* persist across reboot */
    portENTER_CRITICAL(&s_units_mux);
    enh_node_t *n = enh_node_slot(mac);
    if (n) n->disabled = disabled;
    portEXIT_CRITICAL(&s_units_mux);
    if (disabled) {                              /* force its units absent now */
        espnow_registry_t reg;
        if (esp_now_registry_load(&reg))
            for (int i = 0; i < reg.node_count; i++)
                if (memcmp(reg.nodes[i].mac, mac, 6) == 0)
                    for (int c = 0; c < reg.nodes[i].ch_count; c++)
                        rbpower_set_unit_hardware_present(
                            rbgrid_create_espnow_unit_id(mac, reg.nodes[i].channels[c].ch), false);
    }
    ESP_LOGW(TAG, "node %02X:%02X:%02X:%02X:%02X:%02X %s (persisted)",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], disabled ? "DISABLED" : "ENABLED");
}

/* Full ESP-NOW status + per-node health + per-channel presence as a malloc'd JSON string (caller frees).
 * Built from the registry (authoritative node/channel list) × runtime health × live unit-present flags. */
char *esp_now_hub_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", s_running);
    cJSON_AddNumberToObject(root, "channel", s_channel);
    cJSON_AddBoolToObject(root, "pairing_open", esp_now_hub_pairing_open());
    cJSON_AddNumberToObject(root, "paired_count", s_paired_count);
    cJSON_AddNumberToObject(root, "beacons_sent", s_beacons_sent);
    cJSON_AddNumberToObject(root, "rt_recv", s_rt_recv);
    cJSON_AddNumberToObject(root, "stats_recv", s_stats_recv);
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    int64_t now = esp_timer_get_time();
    espnow_registry_t reg;
    if (esp_now_registry_load(&reg)) {
        for (int i = 0; i < reg.node_count; i++) {
            const espnow_reg_node_t *rn = &reg.nodes[i];
            enh_node_t hs = {0}; bool have = false;
            portENTER_CRITICAL(&s_units_mux);
            for (int k = 0; k < ESPNOW_REG_MAX_NODES; k++)
                if (s_nodes[k].used && memcmp(s_nodes[k].mac, rn->mac, 6) == 0) { hs = s_nodes[k]; have = true; break; }
            portEXIT_CRITICAL(&s_units_mux);
            cJSON *jn = cJSON_CreateObject();
            char macs[18];
            snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
                     rn->mac[0],rn->mac[1],rn->mac[2],rn->mac[3],rn->mac[4],rn->mac[5]);
            cJSON_AddStringToObject(jn, "mac", macs);
            cJSON_AddBoolToObject(jn, "online", have && hs.online);
            cJSON_AddBoolToObject(jn, "disabled", have && hs.disabled);
            cJSON_AddNumberToObject(jn, "last_seen_s",
                                    (have && hs.last_rt_us) ? (double)((now - hs.last_rt_us) / 1000000) : -1);
            if (have) {
                cJSON *sync = cJSON_AddObjectToObject(jn, "sync");
                cJSON_AddNumberToObject(sync, "resid_sd_us", hs.resid_sd);
                cJSON_AddNumberToObject(sync, "skew_ppm", hs.skew_ppm);
                cJSON_AddNumberToObject(sync, "fits", hs.fits);
                uint32_t rcv = hs.beacons_recv > hs.max_beacon_id ? hs.max_beacon_id : hs.beacons_recv;
                float loss = hs.max_beacon_id ? 100.0f * (float)(hs.max_beacon_id - rcv) / (float)hs.max_beacon_id : 0.0f;
                cJSON_AddNumberToObject(sync, "loss_pct", loss);
            }
            cJSON *chs = cJSON_AddArrayToObject(jn, "channels");
            for (int c = 0; c < rn->ch_count; c++) {
                rbpower_unit_id_t uid = rbgrid_create_espnow_unit_id(rn->mac, rn->channels[c].ch);
                cJSON *jc = cJSON_CreateObject();
                char uids[8]; snprintf(uids, sizeof(uids), "0x%04X", (unsigned)uid);
                cJSON_AddNumberToObject(jc, "ch", rn->channels[c].ch);
                cJSON_AddNumberToObject(jc, "ct_code", rn->channels[c].ct_code);
                cJSON_AddStringToObject(jc, "unit_id", uids);
                cJSON_AddBoolToObject(jc, "present", rbpower_get_unit_hardware_present(uid));
                cJSON_AddItemToArray(chs, jc);
            }
            cJSON_AddItemToArray(nodes, jn);
        }
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void esp_now_hub_get_stats(uint8_t *channel, uint32_t *beacons_sent, uint32_t *rt_recv,
                           uint32_t *stats_recv, float *last_i0, float *last_i1, float *node_sd,
                           uint32_t *units_created, uint32_t *periods_recv, uint32_t *periods_injected)
{
    if (channel)          *channel          = s_channel;
    if (beacons_sent)     *beacons_sent     = s_beacons_sent;
    if (rt_recv)          *rt_recv          = s_rt_recv;
    if (stats_recv)       *stats_recv       = s_stats_recv;
    if (last_i0)          *last_i0          = s_last_i0;
    if (last_i1)          *last_i1          = s_last_i1;
    if (node_sd)          *node_sd          = s_last_node_sd;
    if (units_created)    *units_created    = s_units_created;
    if (periods_recv)     *periods_recv     = s_periods_recv;
    if (periods_injected) *periods_injected = s_periods_injected;
}
