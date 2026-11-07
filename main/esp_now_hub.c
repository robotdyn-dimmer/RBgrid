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
#include "nvs.h"            /* last injected PERIOD frame per node (dedup across a restart) */
#include "rbfault.h"        /* test builds: lost-ACK simulation */
#include "rbgrid_config_api.h"   /* live point registration on pairing */
#include "rbjson.h"              /* rbgrid_ha_discovery_request */
#include <string.h>
#include <stdlib.h>
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
    int8_t   rssi;                                 /* dBm of the last REALTIME frame (0 = none yet) */
    /* link loss over a sliding window: snapshot of (max_beacon_id, beacons_recv) ≥ 60 s ago */
    uint32_t w_max0, w_recv0; int64_t w_t0;
    float    loss_win;                             /* % lost beacons in the last window; < 0 = no window yet */
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
        if (!s_nodes[i].used) { memset(&s_nodes[i], 0, sizeof(s_nodes[i])); s_nodes[i].used = true; s_nodes[i].loss_win = -1.0f;
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
static void espnow_publish_event(const char *ev, const uint8_t mac[6]);

/* Pending node PERIOD frames (queued whole in the recv cb, injected by the inject task, ACKed only AFTER the
 * inject). The node holds a frame until its PERIOD_ACK and resends the SAME frame (same h.seq + h.node_ts)
 * meanwhile, so:
 *  - the ACK must not go out before the records are in the accounting (a power cut in between lost them:
 *    the node had already deleted its copy) — review H5;
 *  - a resend of a frame already injected (its ACK was lost) must be ACKed again but NOT injected again —
 *    review H6. Key = (node MAC, seq, node_ts): a node reboot restarts seq but carries a new boundary ts. */
#define ENH_MAX_FRAMES  4
#define ENH_FRAME_RECS  8
/** @brief One queued node PERIOD frame. */
typedef struct {
    bool     used;                          ///< slot in use
    uint8_t  mac[6];                        ///< sender node MAC
    uint32_t seq;                           ///< frame h.seq (what the ACK acknowledges)
    uint32_t ts;                            ///< frame h.node_ts (the period boundary, wall-clock s)
    uint8_t  n;                             ///< records in @c recs
    rbn_period_rec_t recs[ENH_FRAME_RECS];  ///< the frame's period records
} enh_frame_t;
static enh_frame_t s_frames[ENH_MAX_FRAMES];
static uint32_t s_periods_recv = 0, s_periods_injected = 0, s_periods_dropped = 0, s_periods_dup = 0;

#define ENH_DEDUP_N 8   /* frames remembered per node (a node retries one frame at a time) */
/** @brief Recently injected frames of one node. */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint8_t  head;
    uint32_t seq[ENH_DEDUP_N], ts[ENH_DEDUP_N];
} enh_dedup_t;
static enh_dedup_t s_dedup[ESPNOW_REG_MAX_NODES];

/* Caller holds s_units_mux. */
static enh_dedup_t *enh_dedup_of(const uint8_t *mac, bool create)
{
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (s_dedup[i].used && memcmp(s_dedup[i].mac, mac, 6) == 0) return &s_dedup[i];
    if (!create) return NULL;
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (!s_dedup[i].used) { memset(&s_dedup[i], 0, sizeof(s_dedup[i])); s_dedup[i].used = true;
                                memcpy(s_dedup[i].mac, mac, 6); return &s_dedup[i]; }
    return NULL;
}
static bool enh_dedup_seen(const uint8_t *mac, uint32_t seq, uint32_t ts)
{
    enh_dedup_t *d = enh_dedup_of(mac, false);
    if (!d) return false;
    for (int k = 0; k < ENH_DEDUP_N; k++) if (d->seq[k] == seq && d->ts[k] == ts && (seq || ts)) return true;
    return false;
}
static void enh_dedup_add(const uint8_t *mac, uint32_t seq, uint32_t ts)
{
    enh_dedup_t *d = enh_dedup_of(mac, true);
    if (!d) return;
    d->seq[d->head] = seq; d->ts[d->head] = ts;
    d->head = (uint8_t)((d->head + 1) % ENH_DEDUP_N);
}
/* The last injected frame per node also lives in NVS, so a resend that arrives after a hub restart (its ACK was
 * lost in the restart) is still recognized. Key "d" + MAC[3..5] hex. */
static void enh_dedup_nvs_key(const uint8_t *mac, char key[12])
{
    snprintf(key, 12, "d%02x%02x%02x", mac[3], mac[4], mac[5]);
}
static void enh_dedup_persist(const uint8_t *mac, uint32_t seq, uint32_t ts)
{
    nvs_handle_t h; char key[12]; uint32_t v[2] = { seq, ts };
    enh_dedup_nvs_key(mac, key);
    if (nvs_open("espnow_dd", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, v, sizeof(v)); nvs_commit(h); nvs_close(h);
}
static void enh_dedup_restore(const uint8_t *mac)
{
    nvs_handle_t h; char key[12]; uint32_t v[2]; size_t l = sizeof(v);
    enh_dedup_nvs_key(mac, key);
    if (nvs_open("espnow_dd", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, key, v, &l) == ESP_OK && l == sizeof(v)) {
        portENTER_CRITICAL(&s_units_mux);
        if (!enh_dedup_seen(mac, v[0], v[1])) enh_dedup_add(mac, v[0], v[1]);
        portEXIT_CRITICAL(&s_units_mux);
    }
    nvs_close(h);
}

/* Over-the-air pairing. A pairing window accepts PAIR_REQ from new nodes → writes the registry. The recv cb
 * just stashes the request; the beacon task does the flash write + PAIR_ACK (off the WiFi callback). */
static int64_t  s_pair_window_until = 0;     /* esp_timer deadline (0 = closed) */
static uint32_t s_paired_count = 0;          /* nodes paired this session → reboot to register */
static volatile bool s_pending_pair = false;
static uint8_t  s_pending_pair_mac[6];
static espnow_reg_chan_t s_pending_pair_ch[ESPNOW_REG_MAX_CH];
static uint8_t  s_pending_pair_n = 0;
static bool     s_pending_pair_v2 = false;       /* PAIR_REQ2 (key exchange) */
static uint8_t  s_pending_node_pub[32];
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

/* A point deleted over the API: if it is a node channel, drop that channel from the registry so it is not
 * registered again at the next boot. The node keeps its pairing (and its other channels). */
void esp_now_on_unit_deleted(uint16_t unit_id)
{
    espnow_registry_t reg;
    if (!esp_now_registry_load(&reg)) return;
    for (int i = 0; i < reg.node_count; i++) {
        espnow_reg_node_t *n = &reg.nodes[i];
        for (int c = 0; c < n->ch_count; c++) {
            if ((uint16_t)rbgrid_create_espnow_unit_id(n->mac, n->channels[c].ch) != unit_id) continue;
            for (int k = c; k < n->ch_count - 1; k++) n->channels[k] = n->channels[k + 1];
            n->ch_count--;
            esp_err_t e = esp_now_registry_save(&reg);
            rbpower_diag_printf("espnow: point %04X deleted — node %02X:..:%02X channel removed from the registry (%s)",
                                (unsigned)unit_id, n->mac[0], n->mac[5], esp_err_to_name(e));
            return;
        }
    }
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
/* ===== Node settings (F5) =====
 * The hub keeps each node's desired settings (NVS espnow_nc/n<MAC[3..5]>) and resends CONFIG until the node answers
 * CONFIG_ACK with the same cfg_seq. Period is fixed at 900 s (the hub rolls node periods up as quarters). */
typedef struct {
    uint32_t       cfg_seq;
    uint16_t       period_s;
    uint8_t        n;
    rbn_cfg_chan_t chans[ESPNOW_REG_MAX_CH];
    uint8_t        state;        /* 0 none, 1 pending, 2 applied, 3 failed */
    uint8_t        result;       /* node's CONFIG_ACK result */
    uint16_t       node_period;  /* period the node reports */
    uint16_t       fw;           /* node firmware version (0xMMmm) */
} enh_nodecfg_t;
typedef struct { bool used, loaded, dirty; uint8_t mac[6]; enh_nodecfg_t c; } enh_nodecfg_slot_t;
static enh_nodecfg_slot_t s_ncfg[ESPNOW_REG_MAX_NODES];

static void ncfg_key(const uint8_t mac[6], char key[12]) { snprintf(key, 12, "n%02x%02x%02x", mac[3], mac[4], mac[5]); }
/* Caller doesn't hold s_units_mux (NVS read). */
static enh_nodecfg_slot_t *ncfg_get(const uint8_t mac[6], bool create)
{
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (s_ncfg[i].used && memcmp(s_ncfg[i].mac, mac, 6) == 0) return &s_ncfg[i];
    if (!create) {
        /* not cached yet: load if stored */
        nvs_handle_t h; char key[12]; enh_nodecfg_t c; size_t l = sizeof(c);
        ncfg_key(mac, key);
        if (nvs_open("espnow_nc", NVS_READONLY, &h) != ESP_OK) return NULL;
        esp_err_t e = nvs_get_blob(h, key, &c, &l);
        nvs_close(h);
        if (e != ESP_OK || l != sizeof(c)) return NULL;
        for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
            if (!s_ncfg[i].used) { s_ncfg[i].used = true; s_ncfg[i].loaded = true; memcpy(s_ncfg[i].mac, mac, 6); s_ncfg[i].c = c; return &s_ncfg[i]; }
        return NULL;
    }
    enh_nodecfg_slot_t *f = ncfg_get(mac, false);
    if (f) return f;
    for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
        if (!s_ncfg[i].used) { memset(&s_ncfg[i], 0, sizeof(s_ncfg[i])); s_ncfg[i].used = true; s_ncfg[i].loaded = true;
                               memcpy(s_ncfg[i].mac, mac, 6); return &s_ncfg[i]; }
    return NULL;
}
static void ncfg_save(enh_nodecfg_slot_t *sl)
{
    nvs_handle_t h; char key[12];
    ncfg_key(sl->mac, key);
    if (nvs_open("espnow_nc", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, &sl->c, sizeof(sl->c)); nvs_commit(h); nvs_close(h);
    sl->dirty = false;
}
static void ncfg_send(const enh_nodecfg_slot_t *sl)
{
    uint8_t buf[sizeof(rbn_config_t) + ESPNOW_REG_MAX_CH * sizeof(rbn_cfg_chan_t)];
    rbn_config_t *m = (rbn_config_t *)buf;
    rbn_hdr_init(&m->h, RBN_MSG_CONFIG, s_beacon_seq++, 0);
    m->cfg_seq = sl->c.cfg_seq; m->period_s = sl->c.period_s; m->chan_count = sl->c.n;
    memcpy(m->chans, sl->c.chans, (size_t)sl->c.n * sizeof(rbn_cfg_chan_t));
    esp_now_send(sl->mac, buf, sizeof(rbn_config_t) + (size_t)sl->c.n * sizeof(rbn_cfg_chan_t));   /* encrypted peer */
}

/* ===== Installation key (PMK) =====
 * One random 16-byte PMK per hub, created at the first ESP-NOW start and kept in NVS (espnow_cfg/pmk). Nodes get it
 * at pairing (PAIR_REQ2/PAIR_ACK2: X25519 + AES-CCM), so no key is built into any firmware image. A dev build that
 * still carries espnow_keys.h seeds the PMK with that key once, so nodes paired the old way keep working. */
static uint8_t s_pmk[16];
static const char *s_pmk_source = "none";
static void hub_pmk_load(void)
{
    nvs_handle_t h; size_t sz = sizeof(s_pmk);
    if (nvs_open("espnow_cfg", NVS_READWRITE, &h) != ESP_OK) { esp_fill_random(s_pmk, sizeof(s_pmk)); s_pmk_source = "ram"; return; }
    if (nvs_get_blob(h, "pmk", s_pmk, &sz) == ESP_OK && sz == sizeof(s_pmk)) {
        s_pmk_source = "stored";
    } else {
#if RBN_HAVE_KEYS
        static const uint8_t dev[16] = RBN_PMK16;
        memcpy(s_pmk, dev, sizeof(s_pmk));
        s_pmk_source = "dev";
#else
        esp_fill_random(s_pmk, sizeof(s_pmk));
        s_pmk_source = "generated";
#endif
        nvs_set_blob(h, "pmk", s_pmk, sizeof(s_pmk)); nvs_commit(h);
        rbpower_diag_printf("espnow: installation key created (%s)", s_pmk_source);
    }
    nvs_close(h);
}

/* Live registration of a node's points runs in its own short-lived task: creating a point, saving units.json and
 * describing its history buffers need far more stack than the beacon task (4 KB) — it overflowed there on the bench —
 * and the HTTP / MQTT tasks calling the `add` action. */
typedef struct { uint8_t mac[6]; uint8_t n; uint8_t ids[ESPNOW_REG_MAX_CH]; uint8_t cts[ESPNOW_REG_MAX_CH]; } enh_live_job_t;
static void enh_live_task(void *arg)
{
    enh_live_job_t *j = (enh_live_job_t *)arg;
    int made = rbgrid_config_espnow_node_live(j->mac, j->ids, j->cts, j->n);
    rbgrid_ha_discovery_request();
    rbpower_diag_printf("espnow: node %02X:%02X:%02X:%02X:%02X:%02X — %d point(s) registered live, no restart needed",
                        j->mac[0], j->mac[1], j->mac[2], j->mac[3], j->mac[4], j->mac[5], made);
    free(j);
    vTaskDelete(NULL);
}
static void enh_register_live(const uint8_t mac[6], const espnow_reg_chan_t *ch, uint8_t n)
{
    enh_live_job_t *j = (enh_live_job_t *)calloc(1, sizeof(*j));
    if (!j) return;
    memcpy(j->mac, mac, 6);
    j->n = n > ESPNOW_REG_MAX_CH ? ESPNOW_REG_MAX_CH : n;
    for (int c = 0; c < j->n; c++) { j->ids[c] = ch[c].ch; j->cts[c] = ch[c].ct_code; }
    if (xTaskCreate(enh_live_task, "espnow_live", 8192, j, 4, NULL) != pdPASS) {
        free(j);
        rbpower_diag_printf("FAIL espnow: live registration task not started — points appear after a restart");
    }
}

static void hub_add_enc_peer(const uint8_t mac[6])
{
    esp_now_peer_info_t p = { .channel = s_channel, .ifidx = hub_ifidx(), .encrypt = true };
    memcpy(p.peer_addr, mac, 6);
    rbn_derive_lmk_from(s_pmk, mac, p.lmk);
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
            if (nd) { nd->last_rt_us = now; if (info->rx_ctrl) nd->rssi = (int8_t)info->rx_ctrl->rssi; }
            portEXIT_CRITICAL(&s_units_mux);
            ESP_LOGI(TAG, "RBN_RT from %02X:%02X:%02X:%02X:%02X:%02X ch=%d i0=%.3fA i1=%.3fA i2=%.3fA v0=%.1f p0=%.1f",
                     mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], m->rec_count,
                     s_last_i0, s_last_i1, s_last_i2, m->recs[0].v_rms, m->recs[0].p_active);
        }
    } else if (h->msg_type == RBN_MSG_PERIOD && len >= (int)sizeof(rbn_period_t)) {
        const rbn_period_t *m = (const rbn_period_t *)data;
        int n = m->rec_count; if (n > ENH_FRAME_RECS) n = ENH_FRAME_RECS;
        if (n >= 1 && len >= (int)(sizeof(rbn_period_t) + (size_t)n * sizeof(rbn_period_rec_t))) {
            /* Already injected (its ACK was lost) → ACK again, inject nothing. Already queued → the ACK follows
             * the inject. Otherwise queue the whole frame; no free slot → no ACK → the node resends. */
            bool dup = false, queued = false, stored = false;
            portENTER_CRITICAL(&s_units_mux);
            dup = enh_dedup_seen(mac, h->seq, h->node_ts);
            for (int k = 0; !dup && k < ENH_MAX_FRAMES; k++)
                if (s_frames[k].used && s_frames[k].seq == h->seq && s_frames[k].ts == h->node_ts &&
                    memcmp(s_frames[k].mac, mac, 6) == 0) queued = true;
            for (int k = 0; !dup && !queued && k < ENH_MAX_FRAMES; k++)
                if (!s_frames[k].used) {
                    enh_frame_t *f = &s_frames[k];
                    f->used = true; memcpy(f->mac, mac, 6); f->seq = h->seq; f->ts = h->node_ts; f->n = (uint8_t)n;
                    memcpy(f->recs, m->recs, (size_t)n * sizeof(rbn_period_rec_t));
                    s_periods_recv++;
                    stored = true;
                    break;
                }
            if (dup) s_periods_dup++;
            portEXIT_CRITICAL(&s_units_mux);
            if (dup) {
                rbn_period_ack_t ack; rbn_hdr_init(&ack.h, RBN_MSG_PERIOD_ACK, s_beacon_seq++, 0);
                ack.ack_seq = h->seq;
                if (!rbfault_hit(RBFAULT_ESPNOW_ACK_DROP))          /* test builds: lost re-ACK too */
                    esp_now_send(mac, (uint8_t *)&ack, sizeof(ack));   /* unicast → CCMP like the first ACK */
                ESP_LOGW(TAG, "PERIOD seq=%u from %02X:..:%02X already injected — re-ACKed, not counted again",
                         (unsigned)h->seq, mac[0], mac[5]);
                return;
            }
            if (!queued && !stored) { ESP_LOGW(TAG, "PERIOD from %02X:..:%02X: queue full — not ACKed, node will resend", mac[0], mac[5]); return; }
        }
    } else if (h->msg_type == RBN_MSG_CONFIG_ACK && len >= (int)sizeof(rbn_config_ack_t)) {
        const rbn_config_ack_t *a = (const rbn_config_ack_t *)data;
        for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++) {
            enh_nodecfg_slot_t *sl = &s_ncfg[i];
            if (!sl->used || memcmp(sl->mac, mac, 6) != 0) continue;
            sl->c.node_period = a->period_s; sl->c.fw = a->fw_ver; sl->c.result = a->result;
            if (sl->c.state == 1 && a->cfg_seq == sl->c.cfg_seq) { sl->c.state = a->result == 0 ? 2 : 3; sl->dirty = true; }
            break;
        }
    } else if (h->msg_type == RBN_MSG_PAIR_REQ2 && len >= (int)sizeof(rbn_pair_req2_t)) {
        /* Key-exchange pairing: accepted only inside the window the user opened; stashed for the beacon task. */
        const rbn_pair_req2_t *p = (const rbn_pair_req2_t *)data;
        int n = p->chan_count;
        if (esp_timer_get_time() < s_pair_window_until && !s_pending_pair &&
            n >= 1 && n <= ESPNOW_REG_MAX_CH && len >= (int)(sizeof(rbn_pair_req2_t) + (size_t)n * sizeof(rbn_pair_chan_t))) {
            memcpy(s_pending_pair_mac, mac, 6);
            for (int i = 0; i < n; i++) { s_pending_pair_ch[i].ch = p->chans[i].channel_id;
                                          s_pending_pair_ch[i].ct_code = p->chans[i].ct_code; }
            s_pending_pair_n = (uint8_t)n;
            memcpy(s_pending_node_pub, p->node_pub, 32);
            s_pending_pair_v2 = true;
            s_pending_pair = true;
        }
    } else if (h->msg_type == RBN_MSG_PAIR_REQ && len >= (int)sizeof(rbn_pair_req_t)) {
#if !RBN_HAVE_KEYS
        return;   /* legacy pre-shared-key pairing: dev builds only */
#endif
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
                  nd->beacons_recv = s->beacons_recv; nd->max_beacon_id = s->max_beacon_id;
                  /* Loss over the last ≥60 s from the increments. The raw counters are since different starts (the
                   * hub's beacon id since the HUB booted, the node's count since the NODE booted or relocked), so
                   * their ratio read ~99 % right after a node restart and took hours to decay. Restart of either
                   * side (a counter going backwards) restarts the window. */
                  int64_t tn = nd->last_stats_us;
                  if (nd->w_t0 == 0 || s->max_beacon_id < nd->w_max0 || s->beacons_recv < nd->w_recv0) {
                      nd->w_t0 = tn; nd->w_max0 = s->max_beacon_id; nd->w_recv0 = s->beacons_recv; nd->loss_win = -1.0f;
                  } else if (tn - nd->w_t0 >= 60LL * 1000000LL) {
                      uint32_t dm = s->max_beacon_id - nd->w_max0, dr = s->beacons_recv - nd->w_recv0;
                      if (dr > dm) dr = dm;
                      nd->loss_win = dm ? 100.0f * (float)(dm - dr) / (float)dm : 0.0f;
                      nd->w_t0 = tn; nd->w_max0 = s->max_beacon_id; nd->w_recv0 = s->beacons_recv;
                  } }
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
            if (s_pending_pair_v2) {
                /* hand the installation key over: fresh hub X25519 pair, shared secret, PMK wrapped for this node */
                rbn_pair_ack2_t a2; memset(&a2, 0, sizeof(a2));
                rbn_hdr_init(&a2.h, RBN_MSG_PAIR_ACK2, s_beacon_seq++, 0);
                memcpy(a2.node_mac, s_pending_pair_mac, 6);
                uint8_t priv[32], shared[32];
                bool kx = (e == ESP_OK) && rbn_x25519_keypair(priv, a2.hub_pub) == 0 &&
                          rbn_x25519_shared(priv, s_pending_node_pub, shared) == 0 &&
                          rbn_pmk_wrap(shared, s_pending_pair_mac, a2.h.seq, s_pmk, a2.enc_pmk, a2.tag) == 0;
                memset(priv, 0, sizeof(priv)); memset(shared, 0, sizeof(shared));
                a2.accepted = kx ? 1 : 0;
                if (!kx && e == ESP_OK) { esp_now_registry_remove_node(s_pending_pair_mac); e = ESP_FAIL;
                                          rbpower_diag_printf("FAIL espnow: key exchange failed — node not paired"); }
                esp_now_send(BCAST, (uint8_t *)&a2, sizeof(a2));
            } else {
                rbn_pair_ack_t ack; rbn_hdr_init(&ack.h, RBN_MSG_PAIR_ACK, s_beacon_seq++, 0);
                ack.accepted = (e == ESP_OK) ? 1 : 0;
                esp_now_send(BCAST, (uint8_t *)&ack, sizeof(ack));
            }
            s_pending_pair_v2 = false;
            if (e == ESP_OK) {
                s_paired_count++;
                hub_add_enc_peer(s_pending_pair_mac);   /* encrypt this node's link immediately */
                espnow_publish_ha_node(s_pending_pair_mac, false);   /* HA "Online" sensor for the new node */
                espnow_publish_event("paired", s_pending_pair_mac);
                {   enh_nodecfg_slot_t *sl = ncfg_get(s_pending_pair_mac, false);
                    if (sl && sl->c.state != 1) { sl->c.state = 1; sl->dirty = false; }   /* re-send on the next ~3 s tick */
                }
                /* Its points now, with history slots: no restart (pairing used to say "reboot to register"). */
                enh_register_live(s_pending_pair_mac, s_pending_pair_ch, s_pending_pair_n);
            }
            s_pending_pair = false;
        }
        /* Watch the active tariff zone; on a transition, command all nodes to latch so their period boundary
         * coincides with the zone boundary (the node can't know zones). Throttled to ~every 3s. */
        {   /* node settings: resend pending CONFIGs, persist results (every ~3 s) */
            static uint32_t cfg_div = 0;
            if (++cfg_div >= s_zone_poll_period) {
                cfg_div = 0;
                for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++) {
                    enh_nodecfg_slot_t *sl = &s_ncfg[i];
                    if (!sl->used) continue;
                    if (sl->dirty) {
                        ncfg_save(sl);
                        rbpower_diag_printf("espnow: node %02X:..:%02X settings %s (cfg %u, period %u s, fw %u.%u)",
                                            sl->mac[0], sl->mac[5], sl->c.state == 2 ? "applied" : "FAILED",
                                            (unsigned)sl->c.cfg_seq, (unsigned)sl->c.node_period,
                                            (unsigned)(sl->c.fw >> 8), (unsigned)(sl->c.fw & 0xFF));
                    }
                    if (sl->c.state == 1) ncfg_send(sl);
                }
            }
        }
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
    char topic[64], payload[128];
    espnow_node_status_topic(mac, topic, sizeof(topic));
    /* online + link health (rssi, loss, last heard) for HA sensors and MQTT clients */
    int8_t rssi = 0; float loss = -1; int64_t lr = 0;
    portENTER_CRITICAL(&s_units_mux);
    for (int k = 0; k < ESPNOW_REG_MAX_NODES; k++)
        if (s_nodes[k].used && memcmp(s_nodes[k].mac, mac, 6) == 0) {
            rssi = s_nodes[k].rssi; lr = s_nodes[k].last_rt_us;
            loss = s_nodes[k].loss_win;   /* last ≥60 s window; < 0 = none yet (omitted) */
            break;
        }
    portEXIT_CRITICAL(&s_units_mux);
    int n = snprintf(payload, sizeof(payload), "{\"online\":%s", online ? "true" : "false");
    if (rssi) n += snprintf(payload + n, sizeof(payload) - n, ",\"rssi_dbm\":%d", (int)rssi);
    if (loss >= 0) n += snprintf(payload + n, sizeof(payload) - n, ",\"loss_pct\":%.1f", (double)loss);
    if (lr) n += snprintf(payload + n, sizeof(payload) - n, ",\"last_seen_s\":%lld", (long long)((esp_timer_get_time() - lr) / 1000000));
    snprintf(payload + n, sizeof(payload) - n, "}");
    rbmqtt_publish(topic, payload, 1, true);   /* retained → last-known state for new subscribers */
}

/* Node events for MQTT clients (not retained): rbgrid/<hub>/event {"type":"espnow","event":…,"mac":…}. */
static void espnow_publish_event(const char *ev, const uint8_t mac[6])
{
    if (!rbmqtt_is_connected()) return;
    uint8_t hub[6]; esp_read_mac(hub, ESP_MAC_WIFI_STA);
    char topic[48], payload[112];
    snprintf(topic, sizeof(topic), "rbgrid/%02x%02x%02x%02x%02x%02x/event", hub[0],hub[1],hub[2],hub[3],hub[4],hub[5]);
    snprintf(payload, sizeof(payload), "{\"type\":\"espnow\",\"event\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
             ev, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    rbmqtt_publish(topic, payload, 1, false);
}
/** Signal an online/offline transition: log + the retained status event. */
static void espnow_emit_presence(const uint8_t mac[6], bool online)
{
    ESP_LOGW(TAG, "node %02X:%02X:%02X:%02X:%02X:%02X is %s",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], online ? "ONLINE" : "OFFLINE");
    espnow_publish_status(mac, online);
    espnow_publish_event(online ? "node_online" : "node_offline", mac);
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
    char topic_rssi[128];
    snprintf(topic_rssi, sizeof(topic_rssi), "homeassistant/sensor/rbgrid_%s_espnow_%s_rssi/config", hubhex, nlo);
    if (remove) { rbmqtt_publish(topic, "", 0, true); rbmqtt_publish(topic_rssi, "", 0, true); return; }   /* empty retained → delete */
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
    if (json) { rbmqtt_publish(topic, json, 0, true); free(json); }
    /* second entity on the same device: link signal (dBm) from the same status topic */
    snprintf(buf, sizeof(buf), "rbgrid_%s_espnow_%s_rssi", hubhex, nlo);
    cJSON_ReplaceItemInObject(c, "uniq_id", cJSON_CreateString(buf));
    snprintf(buf, sizeof(buf), "Node %02X:%02X:%02X:%02X:%02X:%02X Signal", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    cJSON_ReplaceItemInObject(c, "name", cJSON_CreateString(buf));
    cJSON_ReplaceItemInObject(c, "dev_cla", cJSON_CreateString("signal_strength"));
    cJSON_ReplaceItemInObject(c, "val_tpl", cJSON_CreateString("{{ value_json.rssi_dbm | default(none) }}"));
    cJSON_DeleteItemFromObject(c, "pl_on"); cJSON_DeleteItemFromObject(c, "pl_off");
    cJSON_AddStringToObject(c, "unit_of_meas", "dBm");
    cJSON_AddStringToObject(c, "stat_cla", "measurement");
    cJSON_AddStringToObject(c, "ent_cat", "diagnostic");
    json = cJSON_PrintUnformatted(c);
    cJSON_Delete(c);
    if (json) { rbmqtt_publish(topic_rssi, json, 0, true); free(json); }
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
extern bool rbwebserver_ota_in_progress(void);   /* rbwebserver_ota.c */
static void inject_task(void *arg)
{
    uint32_t sweep_div = 0;
    while (s_running) {
        /* Firmware upload in progress: hold the injects (they take the home lock and commit the hour image to NVS,
         * which waits behind the OTA flash writes — the stats task then starved on that lock and the task watchdog
         * reset the hub mid-upload, on the bench, twice). PERIOD frames stay queued and un-ACKed; the node keeps
         * them and resends. */
        if (rbwebserver_ota_in_progress()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
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
        /* --- PERIOD: node's finished W/Wh → history/billing, then the ACK --- */
        for (int i = 0; i < ENH_MAX_FRAMES; i++) {
            portENTER_CRITICAL(&s_units_mux);
            bool pgo = s_frames[i].used;
            enh_frame_t f = s_frames[i];
            portEXIT_CRITICAL(&s_units_mux);
            if (!pgo) continue;
            /* A disabled node's finished periods must not flow into history/billing either (same gate as
             * REALTIME): ACKed and dropped on purpose, so the node doesn't keep resending them. */
            bool disabled = enh_node_is_disabled(f.mac);
            uint8_t ok = 0, bad = 0;
            /* Mark the frame as counted in NVS BEFORE the inject. The NVS commit is the slow step (it waits on flash,
             * e.g. behind an OTA write); a crash inside it leaves the frame neither marked nor injected, so the
             * node's resend is counted once. Marking AFTER the inject left a window where a crash (watchdog during
             * an OTA, seen on the bench) re-counted the resend: one quarter twice. Remaining window: a crash
             * between this commit and the RAM inject that follows loses that frame (ms, no flash I/O). */
            enh_dedup_persist(f.mac, f.seq, f.ts);
            for (uint8_t r = 0; r < f.n; r++) {
                const rbn_period_rec_t *rec = &f.recs[r];
                if (disabled) { bad++; continue; }
                rbpower_unit_id_t uid = rbgrid_create_espnow_unit_id(f.mac, rec->channel_id);
                /* A channel without a point (the user deleted it: an unused input) is ignored quietly, not
                 * reported as a dropped record every period. */
                if (!rbpower_home_get_unit_definition(uid)) continue;
                /* Feed the node's finished 15MIN period into the native rollup (unit_id-based, shared with the
                 * rbAmp inject path): inject_period_result(15MIN) → rbpower_rollup_after_15min → 1HOUR accumulator
                 * → finalize_1hour → flash + tariff/economics. The unit MUST be a fully-registered config unit. */
                /* the period's own boundary (node_ts, wall clock) picks its tariff zone and hour, not the time the
                 * frame arrived — a frame held through a hub restart or a radio gap used to be priced at delivery */
                if (f.ts > 1600000000u) rbpower_home_set_next_period_end(uid, f.ts);
                rbpower_err_t ie = rbpower_home_inject_period_result(uid, (rbpower_stat_period_type_t)rec->period_type,
                    rec->avg_power_w, rec->max_power_w, rec->energy_wh, rec->period_ms,
                    rec->voltage_v, rec->current_avg_a, rec->current_min_a, rec->current_max_a);
                if (ie == RBPOWER_OK) ok++; else bad++;
                ESP_LOGW(TAG, "PERIOD inject unit=0x%04X type=%u avg=%.2fW E=%.4fWh dt=%lums -> %s", (unsigned)uid,
                         rec->period_type, rec->avg_power_w, rec->energy_wh, (unsigned long)rec->period_ms,
                         ie == RBPOWER_OK ? "ok" : "REJECTED");
            }
            /* Remember it in RAM BEFORE the ACK (a resend after a lost ACK is then recognized), then ACK. */
            portENTER_CRITICAL(&s_units_mux);
            enh_dedup_add(f.mac, f.seq, f.ts);
            s_periods_injected += ok;
            s_periods_dropped  += bad;
            s_frames[i].used = false;
            portEXIT_CRITICAL(&s_units_mux);
            if (bad) rbpower_diag_printf("FAIL espnow: node %02X:..:%02X frame seq=%u — %u record(s) not counted (%s)",
                                         f.mac[0], f.mac[5], (unsigned)f.seq, (unsigned)bad,
                                         disabled ? "node disabled" : "unit rejected the period");
            rbn_period_ack_t ack; rbn_hdr_init(&ack.h, RBN_MSG_PERIOD_ACK, s_beacon_seq++, 0);
            ack.ack_seq = f.seq;
            if (!rbfault_hit(RBFAULT_ESPNOW_ACK_DROP))          /* test builds: simulate a lost ACK */
                esp_now_send(f.mac, (uint8_t *)&ack, sizeof(ack));
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
            static uint32_t s_health_div = 0;   /* link health (rssi / loss / last heard) for HA, once a minute */
            if (++s_health_div >= 60) {
                s_health_div = 0;
                for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++) {
                    portENTER_CRITICAL(&s_units_mux);
                    bool used = s_nodes[i].used, on = s_nodes[i].online; uint8_t mac[6];
                    if (used) memcpy(mac, s_nodes[i].mac, 6);
                    portEXIT_CRITICAL(&s_units_mux);
                    if (used) espnow_publish_status(mac, on);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_inject_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp_now_hub_start(uint8_t channel)
{
    if (s_running) { ESP_LOGW(TAG, "already running"); return ESP_OK; }
    hub_pmk_load();   /* the installation key: NVS, generated at the first start */

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
    esp_now_set_pmk(s_pmk);
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
                /* not heard yet in this run: its points read "no data" until its first REALTIME (they read present
                 * from the config default, so an unplugged node looked "receiving" after a hub restart) */
                for (int c = 0; c < reg.nodes[i].ch_count; c++)
                    rbpower_set_unit_hardware_present(rbgrid_create_espnow_unit_id(reg.nodes[i].mac, reg.nodes[i].channels[c].ch), false);
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
    memset(s_frames, 0, sizeof(s_frames)); s_periods_recv = s_periods_injected = s_periods_dropped = s_periods_dup = 0;
    {   /* last injected frame per registered node (NVS) → a resend after this restart is recognized */
        espnow_registry_t reg;
        if (esp_now_registry_load(&reg))
            for (int i = 0; i < reg.node_count && i < ESPNOW_REG_MAX_NODES; i++) enh_dedup_restore(reg.nodes[i].mac);
    }
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
/* ===== Hub ESP-NOW switch (persisted) =====
 * NVS espnow_cfg/enabled: unset = automatic (start when the registry has nodes, as before), 1 = on, 0 = off.
 * "off" wins over a non-empty registry; "on" starts the radio even with no node yet (ready to pair). */
int8_t esp_now_hub_enabled_setting(void)
{
    nvs_handle_t h; uint8_t v = 0xFF;
    if (nvs_open("espnow_cfg", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "enabled", &v); nvs_close(h); }
    return v == 0xFF ? -1 : (int8_t)(v ? 1 : 0);
}
static void esp_now_hub_set_enabled_setting(bool on)
{
    nvs_handle_t h;
    if (nvs_open("espnow_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "enabled", on ? 1 : 0); nvs_commit(h); nvs_close(h);
}

static bool enh_parse_mac(const char *s, uint8_t mac[6])
{
    return s && (sscanf(s, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) == 6 ||
                 sscanf(s, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) == 6);
}

/* One implementation of the node actions for HTTP (POST /api/v1/espnow) and MQTT (cmd set espnow). @p req is the
 * request object {action, mac?, seconds?, channels?}. Returns a malloc'd JSON reply; *status = HTTP status. */
char *esp_now_hub_action(const cJSON *req, int *status)
{
    const cJSON *ja = req ? cJSON_GetObjectItem(req, "action") : NULL;
    const cJSON *jm = req ? cJSON_GetObjectItem(req, "mac") : NULL;
    const char *action = cJSON_IsString(ja) ? ja->valuestring : "";
    uint8_t mac[6];
    bool have_mac = cJSON_IsString(jm) && enh_parse_mac(jm->valuestring, mac);
    char out[200];
    int st = 200;
#define ENH_ERR(code, ...) do { st = (code); snprintf(out, sizeof(out), __VA_ARGS__); } while (0)
    if (!strcmp(action, "hub_on")) {
        esp_now_hub_set_enabled_setting(true);
        esp_err_t e = esp_now_hub_running() ? ESP_OK : esp_now_hub_start(0);
        if (e != ESP_OK) ENH_ERR(503, "{\"ok\":false,\"error\":\"ESPNOW_START_FAILED\",\"message\":\"%s\"}", esp_err_to_name(e));
        else snprintf(out, sizeof(out), "{\"ok\":true,\"running\":true,\"enabled\":true}");
    } else if (!strcmp(action, "hub_off")) {
        esp_now_hub_set_enabled_setting(false);
        esp_now_hub_stop();
        snprintf(out, sizeof(out), "{\"ok\":true,\"running\":false,\"enabled\":false}");
    } else if (!strcmp(action, "pair")) {
        const cJSON *js = cJSON_GetObjectItem(req, "seconds");
        uint32_t sec = cJSON_IsNumber(js) ? (uint32_t)js->valuedouble : 120;
        if (sec < 10) sec = 10;
        if (sec > 600) sec = 600;
        if (esp_now_hub_enabled_setting() == 0) {
            ENH_ERR(409, "{\"ok\":false,\"error\":\"ESPNOW_DISABLED\",\"message\":\"Remote nodes are switched off on this hub (action hub_on).\"}");
        } else {
            esp_err_t e = esp_now_hub_running() ? ESP_OK : esp_now_hub_start(0);   /* first node: start on demand */
            if (e != ESP_OK) ENH_ERR(503, "{\"ok\":false,\"error\":\"ESPNOW_START_FAILED\",\"message\":\"%s\"}", esp_err_to_name(e));
            else { esp_now_hub_open_pairing(sec); snprintf(out, sizeof(out), "{\"ok\":true,\"pairing_open_s\":%u}", (unsigned)sec); }
        }
    } else if (!strcmp(action, "pair_cancel")) {
        s_pair_window_until = 0;
        snprintf(out, sizeof(out), "{\"ok\":true,\"pairing_open\":false,\"paired\":%u}", (unsigned)s_paired_count);
    } else if (!strcmp(action, "latch")) {
        if (!esp_now_hub_running()) ENH_ERR(409, "{\"ok\":false,\"error\":\"ESPNOW_OFF\"}");
        else snprintf(out, sizeof(out), "{\"ok\":true,\"latched\":%u}", (unsigned)esp_now_hub_latch_all(0));
    } else if (!strcmp(action, "remove") && have_mac) {
        snprintf(out, sizeof(out), "{\"ok\":true,\"removed_units\":%u}", (unsigned)esp_now_hub_delnode_cascade(mac));
    } else if ((!strcmp(action, "disable") || !strcmp(action, "enable")) && have_mac) {
        esp_now_hub_set_node_disabled(mac, !strcmp(action, "disable"));
        snprintf(out, sizeof(out), "{\"ok\":true,\"disabled\":%s}", !strcmp(action, "disable") ? "true" : "false");
    } else if (!strcmp(action, "configure") && have_mac) {
        const cJSON *cfg = cJSON_GetObjectItem(req, "config");
        const cJSON *jp = cfg ? cJSON_GetObjectItem(cfg, "period_s") : NULL;
        const cJSON *jch = cfg ? cJSON_GetObjectItem(cfg, "channels") : NULL;
        espnow_registry_t reg; int ni = -1;
        if (esp_now_registry_load(&reg))
            for (int i = 0; i < reg.node_count; i++) if (memcmp(reg.nodes[i].mac, mac, 6) == 0) { ni = i; break; }
        if (ni < 0) ENH_ERR(404, "{\"ok\":false,\"error\":\"NOT_FOUND\",\"message\":\"no such node\"}");
        else if (!cfg || (jp && (!cJSON_IsNumber(jp) || (int)jp->valuedouble != 900)))
            ENH_ERR(400, "{\"ok\":false,\"error\":\"PERIOD_UNSUPPORTED\",\"message\":\"period_s must be 900 (15 min)\"}");
        else {
            enh_nodecfg_slot_t *sl = ncfg_get(mac, true);
            enh_nodecfg_t nc; memset(&nc, 0, sizeof(nc));
            if (sl) nc = sl->c;
            /* start from the registry's channels (all on, registry clamp), then apply the request */
            nc.n = reg.nodes[ni].ch_count;
            for (int c = 0; c < nc.n; c++) {
                bool known = false;
                for (int k = 0; sl && k < sl->c.n; k++) if (sl->c.chans[k].channel_id == reg.nodes[ni].channels[c].ch) { nc.chans[c] = sl->c.chans[k]; known = true; }
                if (!known) { nc.chans[c].channel_id = reg.nodes[ni].channels[c].ch; nc.chans[c].enabled = 1; nc.chans[c].ct_code = reg.nodes[ni].channels[c].ct_code; }
            }
            bool bad = false; int on = 0; const cJSON *e = NULL;
            if (cJSON_IsArray(jch)) cJSON_ArrayForEach(e, jch) {
                const cJSON *cc = cJSON_GetObjectItem(e, "ch"), *en = cJSON_GetObjectItem(e, "enabled"), *ct = cJSON_GetObjectItem(e, "ct_code");
                int idx = -1;
                for (int c = 0; cJSON_IsNumber(cc) && c < nc.n; c++) if (nc.chans[c].channel_id == (uint8_t)cc->valuedouble) idx = c;
                if (idx < 0) { bad = true; break; }
                if (cJSON_IsBool(en)) nc.chans[idx].enabled = cJSON_IsTrue(en) ? 1 : 0;
                if (cJSON_IsNumber(ct)) { int v = (int)ct->valuedouble; if (v < 1 || v > 7) { bad = true; break; } nc.chans[idx].ct_code = (uint8_t)v; }
            }
            for (int c = 0; c < nc.n; c++) on += nc.chans[c].enabled;
            if (bad) ENH_ERR(400, "{\"ok\":false,\"error\":\"BAD_REQUEST\",\"message\":\"unknown channel or ct_code (1..7)\"}");
            else if (on == 0) ENH_ERR(400, "{\"ok\":false,\"error\":\"BAD_REQUEST\",\"message\":\"at least one channel must stay on\"}");
            else if (!sl) ENH_ERR(500, "{\"ok\":false,\"error\":\"REGISTRY\",\"message\":\"no slot\"}");
            else {
                nc.period_s = 900; nc.cfg_seq++; nc.state = 1; nc.result = 0;
                sl->c = nc; ncfg_save(sl);
                if (esp_now_hub_running()) ncfg_send(sl);
                snprintf(out, sizeof(out), "{\"ok\":true,\"config_state\":\"pending\",\"cfg_seq\":%u}", (unsigned)nc.cfg_seq);
            }
        }
    } else if (!strcmp(action, "add") && have_mac) {
        espnow_reg_chan_t ch[ESPNOW_REG_MAX_CH]; uint8_t n = 0;
        const cJSON *jch = cJSON_GetObjectItem(req, "channels"), *e = NULL;
        if (cJSON_IsArray(jch)) cJSON_ArrayForEach(e, jch) {
            if (n >= ESPNOW_REG_MAX_CH) break;
            const cJSON *cc = cJSON_GetObjectItem(e, "ch"), *ct = cJSON_GetObjectItem(e, "ct_code");
            if (cJSON_IsNumber(cc) && cJSON_IsNumber(ct)) { ch[n].ch = (uint8_t)cc->valuedouble; ch[n].ct_code = (uint8_t)ct->valuedouble; n++; }
        }
        if (n == 0) ENH_ERR(400, "{\"ok\":false,\"error\":\"BAD_REQUEST\",\"message\":\"channels[] required\"}");
        else {
            esp_err_t e2 = esp_now_registry_add_node(mac, ch, n);
            if (e2 != ESP_OK) ENH_ERR(500, "{\"ok\":false,\"error\":\"REGISTRY\",\"message\":\"%s\"}", esp_err_to_name(e2));
            else {
                enh_register_live(mac, ch, n);
                if (esp_now_hub_running()) hub_add_enc_peer(mac);
                snprintf(out, sizeof(out), "{\"ok\":true,\"reboot_to_register\":false,\"channels\":%u}", n);
            }
        }
    } else {
        ENH_ERR(400, "{\"ok\":false,\"error\":\"BAD_REQUEST\",\"message\":\"action = hub_on|hub_off|pair|pair_cancel|latch|remove|disable|enable|add|configure (+ mac)\"}");
    }
#undef ENH_ERR
    if (status) *status = st;
    return strdup(out);
}

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
    espnow_publish_event("removed", mac);
    {   /* forget its settings: a node paired again starts from all channels on (the node resets too) */
        nvs_handle_t h; char key[12]; ncfg_key(mac, key);
        if (nvs_open("espnow_nc", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, key); nvs_commit(h); nvs_close(h); }
        for (int i = 0; i < ESPNOW_REG_MAX_NODES; i++)
            if (s_ncfg[i].used && memcmp(s_ncfg[i].mac, mac, 6) == 0) memset(&s_ncfg[i], 0, sizeof(s_ncfg[i]));
    }
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
    {   int8_t en = esp_now_hub_enabled_setting();
        cJSON_AddStringToObject(root, "enabled", en < 0 ? "auto" : (en ? "on" : "off"));   /* hub_on / hub_off / auto */
        cJSON_AddBoolToObject(root, "keys", true);                    /* per-installation key (F1): every image can run it */
        cJSON_AddStringToObject(root, "key_source", s_pmk_source);    /* generated | stored | dev | none (not started yet) */
        cJSON_AddNumberToObject(root, "max_nodes", ESPNOW_REG_MAX_NODES);
        cJSON_AddNumberToObject(root, "max_channels_per_node", ESPNOW_REG_MAX_CH);
        cJSON_AddNumberToObject(root, "pairing_left_s", esp_now_hub_pairing_open()
                                ? (double)((s_pair_window_until - esp_timer_get_time()) / 1000000) : 0); }
    cJSON_AddNumberToObject(root, "channel", s_channel);
    cJSON_AddBoolToObject(root, "pairing_open", esp_now_hub_pairing_open());
    cJSON_AddNumberToObject(root, "paired_count", s_paired_count);
    cJSON_AddNumberToObject(root, "beacons_sent", s_beacons_sent);
    cJSON_AddNumberToObject(root, "periods_recv", s_periods_recv);          /* PERIOD frames queued */
    cJSON_AddNumberToObject(root, "periods_injected", s_periods_injected);  /* records counted */
    cJSON_AddNumberToObject(root, "periods_dropped", s_periods_dropped);    /* records ACKed but not counted (disabled/rejected) */
    cJSON_AddNumberToObject(root, "periods_dup", s_periods_dup);            /* resends of an injected frame, re-ACKed only */
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
            if (have && hs.rssi) cJSON_AddNumberToObject(jn, "rssi_dbm", hs.rssi);
            {
                enh_nodecfg_slot_t *sl = ncfg_get(rn->mac, false);
                if (sl) {
                    static const char *const ST[] = { "none", "pending", "applied", "failed" };
                    cJSON_AddStringToObject(jn, "config_state", ST[sl->c.state & 3]);
                    if (sl->c.node_period) cJSON_AddNumberToObject(jn, "period_s", sl->c.node_period);
                    if (sl->c.fw) { char fv[12]; snprintf(fv, sizeof(fv), "%u.%u", (unsigned)(sl->c.fw >> 8), (unsigned)(sl->c.fw & 0xFF));
                                    cJSON_AddStringToObject(jn, "fw", fv); }
                    cJSON *cc = cJSON_AddArrayToObject(jn, "config");
                    for (int k = 0; k < sl->c.n; k++) {
                        cJSON *o = cJSON_CreateObject();
                        cJSON_AddNumberToObject(o, "ch", sl->c.chans[k].channel_id);
                        cJSON_AddBoolToObject(o, "enabled", sl->c.chans[k].enabled);
                        cJSON_AddNumberToObject(o, "ct_code", sl->c.chans[k].ct_code);
                        cJSON_AddItemToArray(cc, o);
                    }
                }
            }
            if (have) {
                cJSON *sync = cJSON_AddObjectToObject(jn, "sync");
                cJSON_AddNumberToObject(sync, "resid_sd_us", hs.resid_sd);
                cJSON_AddNumberToObject(sync, "skew_ppm", hs.skew_ppm);
                cJSON_AddNumberToObject(sync, "fits", hs.fits);
                /* lost beacons over the last ≥60 s window (absent until the first window completes) */
                if (hs.loss_win >= 0.0f) cJSON_AddNumberToObject(sync, "loss_pct", hs.loss_win);
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
