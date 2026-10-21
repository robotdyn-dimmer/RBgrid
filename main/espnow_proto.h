/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file espnow_proto.h
 * @brief RBgrid ESP-NOW wire protocol: canonical frame definitions (proto_ver 1).
 *
 * Reference: documents/architecture/ESPNOW_PROTOCOL.md. The header is shared by the hub and the node
 * firmware; keep both copies in sync and bump RBN_PROTO_VER on any breaking layout change (additive fields
 * go at the end of a record).
 *
 * Little-endian (ESP32 native): all multi-byte fields are LE. All frame structs are packed.
 * ESP-NOW wireless nodes are available in the code but are not part of the v1.1.0 release feature set.
 */
#ifndef RBGRID_ESPNOW_PROTO_H
#define RBGRID_ESPNOW_PROTO_H

#include <stdint.h>

/** @brief First magic byte of every frame. */
#define RBN_MAGIC0     0x42  ///< 'B'
/** @brief Second magic byte of every frame. */
#define RBN_MAGIC1     0x52  ///< 'R'  -> wire bytes 0x42 0x52 ("BR" LE of "RB")
/** @brief Protocol version carried in rbn_hdr_t::proto_ver. */
#define RBN_PROTO_VER  1

/** @brief Message types (rbn_hdr_t::msg_type). */
enum {
    RBN_MSG_HELLO         = 0x01,  ///< node->hub: identity and capabilities (rbn_hello_t)
    RBN_MSG_REALTIME      = 0x02,  ///< node->hub: realtime samples (rbn_realtime_t)
    RBN_MSG_PERIOD        = 0x03,  ///< node->hub: finished periods (rbn_period_t)
    RBN_MSG_PERIOD_ACK    = 0x04,  ///< hub->node: PERIOD acknowledgement (rbn_period_ack_t)
    RBN_MSG_NODE_STATS    = 0x05,  ///< node->hub: link/sync health (so a no-COM node can be read via the hub)
    RBN_MSG_PAIR_REQ      = 0x10,  ///< node->hub: pairing request (rbn_pair_req_t)
    RBN_MSG_PAIR_ACK      = 0x11,  ///< hub->node: pairing result (rbn_pair_ack_t)
    RBN_MSG_TIME_REQ      = 0x20,  ///< node->hub: wall-clock request (rbn_time_req_t)
    RBN_MSG_TIME_RESP     = 0x21,  ///< hub->node: wall-clock response (rbn_time_resp_t)
    RBN_MSG_SYNC_BEACON   = 0x22,  ///< hub->broadcast: sync beacon (rbn_sync_beacon_t)
    RBN_MSG_SYNC_FOLLOWUP = 0x23,  ///< hub->broadcast: beacon TX timestamp (rbn_sync_followup_t)
    RBN_MSG_CONFIG        = 0x30,  ///< reserved: node configuration
    RBN_MSG_CONFIG_ACK    = 0x31,  ///< reserved: node configuration acknowledgement
    RBN_MSG_LATCH_NOW     = 0x32,  ///< hub->node: close the current period now (rbn_latch_now_t)
    RBN_MSG_SET_OUTPUT    = 0x40,  ///< hub->node: command a dimmer/relay output (encrypted unicast)
    RBN_MSG_OUTPUT_STATE  = 0x41,  ///< node->hub: output ACK + applied state (held-until-ACK)
};

/** @brief Common 12-byte header (every frame begins with this). */
typedef struct __attribute__((packed)) {
    uint8_t  magic0;     ///< RBN_MAGIC0
    uint8_t  magic1;     ///< RBN_MAGIC1
    uint8_t  proto_ver;  ///< RBN_PROTO_VER
    uint8_t  msg_type;   ///< RBN_MSG_*
    uint32_t seq;        ///< per-sender monotonic
    uint32_t node_ts;    ///< sender Unix-sec clock (0 if not yet time-synced)
} rbn_hdr_t;

/** @brief 0x22 SYNC_BEACON (hub->broadcast): body = beacon_id + epoch.
 *
 * epoch = a boot-unique id of the master/hub. When it changes (hub rebooted/OTA'd, beacon_id restarts
 * from 1 on a fresh esp_timer base), the node MUST reset its discipline servo — otherwise the old-epoch
 * samples poison the regression for ~one window. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint32_t  beacon_id;     ///< beacon counter, starts at 1 for each epoch
    uint32_t  epoch;         ///< boot-unique hub id; a change resets the node's servo
    uint8_t   hub_channel;   /**< the hub's exact WiFi channel — the node sets its radio to this (rx_ctrl can
                              * report an adjacent overlapping channel, so trust the hub's own value). */
} rbn_sync_beacon_t;

/** @brief 0x23 SYNC_FOLLOWUP (hub->broadcast): body = beacon_id + the hub's esp_timer (µs) at beacon TX. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint32_t  beacon_id;     ///< id of the beacon this follow-up times
    uint64_t  hub_tx_us;   ///< esp_timer_get_time() captured in the hub's send callback
} rbn_sync_followup_t;

/** @brief 0x02 REALTIME record (one per channel). */
typedef struct __attribute__((packed)) {
    uint8_t  channel_id;   ///< 0..N; 0xFF = node voltage bus
    float    v_rms;        ///< RMS voltage (V); 0 for a current-only channel
    float    i_rms;        ///< RMS current (A)
    float    p_active;     ///< signed: + import / - export
    int16_t  pf_x1000;     ///< power factor × 1000
    uint16_t freq_x100;    ///< mains frequency (Hz × 100)
    uint8_t  flags;        ///< bit0 valid, bit1 direction_export
} rbn_rt_rec_t;

/** @brief 0x02 REALTIME frame: header + rec_count + rec_count*records. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t   h;         ///< common header
    uint8_t     rec_count; ///< number of records that follow
    rbn_rt_rec_t recs[];   ///< flexible
} rbn_realtime_t;

/** @brief 0x10 PAIR_REQ (node->hub, broadcast while unpaired): the node announces its identity + channels so
 * the hub (in its pairing window) can register it into the node registry.
 *
 * An 8-byte HMAC auth tag (rbn_pair_tag, keyed by the shared pairing key) trails the chans[] array —
 *   the hub rejects a PAIR_REQ whose tag doesn't verify, so only a node holding the key can self-register.
 *   Wire layout: [hdr][fw_ver][chan_count][chans × chan_count][tag(8)]. The tag covers everything before it. */
typedef struct __attribute__((packed)) { uint8_t channel_id; uint8_t ct_code; } rbn_pair_chan_t;
/** @var rbn_pair_chan_t::channel_id
 *  Node flat channel_id.  @var rbn_pair_chan_t::ct_code
 *  Current-transformer code (1, 2, 3, 4 or 6). */
/** @brief PAIR_REQ frame; see the description above for the trailing tag. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t       h;          ///< common header
    uint16_t        fw_ver;     ///< node firmware version
    uint8_t         chan_count; ///< number of entries in chans[]
    rbn_pair_chan_t chans[];   ///< chan_count × {node flat channel_id, ct_code 1/2/3/4/6}, THEN uint8_t tag[8]
} rbn_pair_req_t;
/** @brief Byte length of a PAIR_REQ carrying @p n channels + the trailing auth tag. */
#define RBN_PAIR_REQ_LEN(n)        (sizeof(rbn_pair_req_t) + (size_t)(n) * sizeof(rbn_pair_chan_t) + 8)
/** @brief Length of the tag-covered prefix for @p n channels (everything except the trailing 8-byte tag). */
#define RBN_PAIR_REQ_PREFIX_LEN(n) (sizeof(rbn_pair_req_t) + (size_t)(n) * sizeof(rbn_pair_chan_t))

/** @brief 0x11 PAIR_ACK (hub->node): accepted=1 → the hub registered the node (units activate on its next reboot). */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint8_t   accepted;      ///< 1 = registered, 0 = rejected (registry full or write error)
} rbn_pair_ack_t;

/** @brief 0x20 TIME_REQ (node->hub): request the hub's wall-clock. body = req_token (echoed back for RTT). */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint32_t  req_token;     ///< node token, echoed in TIME_RESP
} rbn_time_req_t;

/** @brief 0x21 TIME_RESP (hub->node): body = req_token + the hub's Unix time (ms). Node anchors its wall-clock
 * to this (+ RTT/2) and tracks it with the disciplined clock rate → calendar-aligned period boundaries. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint32_t  req_token;     ///< token copied from TIME_REQ
    uint64_t  hub_unix_ms;   ///< hub Unix time (ms); 0 when the hub clock is not synced
} rbn_time_resp_t;

/** @brief 0x05 NODE_STATS (node->hub): link + sync health, so a node with no COM can be read via the hub.
 * loss is inferable: max_beacon_id is monotonic from 1, so missed = max_beacon_id - beacons_recv. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;                ///< common header
    uint32_t  beacons_recv;     ///< beacons received in the current epoch
    uint32_t  followups_recv;   ///< follow-ups received in the current epoch
    uint32_t  max_beacon_id;    ///< highest beacon_id seen
    uint32_t  fits;             ///< residual sample count
    float     resid_mean_us;    ///< clock-fit residual mean (µs)
    float     resid_sd_us;      ///< clock-fit residual standard deviation (µs)
    float     resid_maxabs_us;  ///< clock-fit maximum absolute residual (µs)
    float     skew_ppm;         ///< node clock skew against the hub (ppm)
} rbn_node_stats_t;

/** @brief 0x03 PERIOD record (one per channel — a finished, on-node-integrated period for billing). */
typedef struct __attribute__((packed)) {
    uint8_t  channel_id;     ///< node flat channel_id
    uint8_t  period_type;    ///< rbpower_stat_period_type_t
    uint32_t period_ms;      ///< integrated window length (the energy dt)
    float    avg_power_w;    ///< average active power (W)
    float    max_power_w;    ///< maximum active power (W)
    float    energy_wh;      ///< energy over the period (Wh)
    float    voltage_v;      ///< representative period voltage (0 = unknown / current-only)
    float    current_avg_a;  ///< average RMS current (A)
    float    current_min_a;  ///< minimum RMS current (A)
    float    current_max_a;  ///< maximum RMS current (A)
} rbn_period_rec_t;

/** @brief 0x03 PERIOD frame (node->hub): header (h.seq identifies the frame for the ACK) + records.
 * The node HOLDS each frame until a matching PERIOD_ACK (held-until-ACK) so billing energy is never
 * lost to a dropped frame. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header (h.seq is acknowledged)
    uint8_t   rec_count;     ///< number of records that follow
    rbn_period_rec_t recs[]; ///< period records (flexible)
} rbn_period_t;

/** @brief 0x04 PERIOD_ACK (hub->node): body = the PERIOD frame's h.seq being acknowledged. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint32_t  ack_seq;       ///< h.seq of the acknowledged PERIOD frame
} rbn_period_ack_t;

/* 0x32 LATCH_NOW (hub->node): the hub commands the node to immediately close its current period and
 * emit a PERIOD frame, then restart accumulation — so a period boundary coincides with a tariff-ZONE boundary
 * (which the node can't know; tariff zones live only in the master). The hub sends this on a zone transition
 * (auto) or on demand (verb). Encrypted unicast (the node is a registered peer). */
/** @brief LATCH_NOW reasons (rbn_latch_now_t::reason). */
enum { RBN_LATCH_REASON_MANUAL = 0, RBN_LATCH_REASON_ZONE = 1 };
/** @brief LATCH_NOW frame. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint8_t   reason;        ///< RBN_LATCH_REASON_*
    uint32_t  boundary_unix; ///< hub wall-clock second of the boundary (reference/logging)
} rbn_latch_now_t;

/* ===== HELLO + output-node extension (proto_ver 1, additive) =====================
 * Beacon-INDEPENDENT by design so it works with a beacon-less OPEN hub (e.g. ACRouter) as well as the
 * RBgrid beaconing hub: discovery = periodic broadcast-OPEN HELLO; control = SET_OUTPUT/OUTPUT_STATE over
 * encrypted unicast; liveness/failsafe = SET_OUTPUT-silence (NOT beacon-silence). MAC (from recv_info) is the
 * node's stable UID / registry key. Doc: documents/architecture/ESPNOW_PROTOCOL.md §2.1/§2.10/§2.11. */

/** @brief Node family (rbn_hello_t::family): classifies the node as a registry entity. */
enum { RBN_FAMILY_SENSOR = 0x01,   ///< rbAmp metering node (REALTIME/PERIOD upstream)
       RBN_FAMILY_DIMMER = 0x02,   ///< wireless dimmer output node (DimmerLink at the load)
       RBN_FAMILY_RELAY  = 0x03 }; /* wireless GPIO relay output node */
/* rbn_hello_t.flags */
#define RBN_HELLO_F_HAS_VOLTAGE_HW 0x01   ///< HELLO flag: the node measures voltage
#define RBN_HELLO_F_MAINS_POWERED  0x02   ///< HELLO flag: the node is mains powered
#define RBN_HELLO_F_TIME_SYNCED    0x04   ///< HELLO flag: the node clock is synced
#define RBN_HELLO_F_HAS_OUTPUTS    0x08   ///< out_count>0 → out_cap[] descriptors trail
/** @brief Output kind (rbn_out_cap_t::kind, rbn_set_output_t::kind): RBN_OUT_KIND_DIMMER or RBN_OUT_KIND_RELAY. */
enum { RBN_OUT_KIND_DIMMER = 0x01, RBN_OUT_KIND_RELAY = 0x02 };

/** @brief Per-output capability descriptor, advertised in HELLO. */
typedef struct __attribute__((packed)) {
    uint8_t  output_id;      ///< stable 0..N id for this output on this node
    uint8_t  kind;           ///< RBN_OUT_KIND_*
    uint16_t range_min;      ///< min value (dimmer 0 ; relay 0)
    uint16_t range_max;      ///< max value (dimmer 1000 = per-mille ; relay 1)
    uint16_t failsafe_value; ///< value the node drives on hub-silence (0 = OFF, safest for a heater)
} rbn_out_cap_t;

/** @brief 0x01 HELLO (node->hub) — identity + capabilities. Sent broadcast-OPEN periodically (~30s) so a beacon-less
 * OPEN hub can discover+classify the node; also valid unicast-encrypted once paired. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint16_t  fw_ver;        ///< e.g. 0x0104 = 1.4
    uint16_t  hw_model;      ///< vendor model enum
    uint8_t   family;        ///< RBN_FAMILY_*
    uint8_t   flags;         ///< RBN_HELLO_F_*
    uint8_t   channel_count; ///< sensor input channels (0 for a pure output node)
    uint8_t   out_count;     ///< number of rbn_out_cap_t trailing (0 if none)
    rbn_out_cap_t out_cap[]; ///< out_count descriptors (present iff RBN_HELLO_F_HAS_OUTPUTS)
} rbn_hello_t;
/** @brief Byte length of a HELLO carrying @p nout output descriptors. */
#define RBN_HELLO_LEN(nout) (sizeof(rbn_hello_t) + (size_t)(nout) * sizeof(rbn_out_cap_t))

/** @brief 0x40 SET_OUTPUT (hub->node, ENCRYPTED UNICAST) — command an output. h.seq = command id (echoed in the ACK).
 * Idempotent: re-sending the same (output_id,value) is safe and doubles as the failsafe keep-alive. */
#define RBN_SETOUT_F_SET_FAILSAFE 0x01   ///< also store `value` as this output's failsafe default
/** @brief SET_OUTPUT frame. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header (h.seq = command id)
    uint8_t   output_id;     ///< target output (from HELLO out_cap[])
    uint8_t   kind;          ///< expected kind — node rejects on mismatch (result=3)
    uint16_t  value;         ///< dimmer 0..1000 per-mille ; relay 0|1 (clamped to range)
    uint16_t  ramp_ms;       ///< dimmer fade duration to value (0 = immediate); relay: ignored
    uint8_t   flags;         ///< RBN_SETOUT_F_*
} rbn_set_output_t;

/** @brief 0x41 OUTPUT_STATE (node->hub, ENCRYPTED UNICAST) — ACK + applied state. Sent as the ACK to SET_OUTPUT
 * (ack_seq = the command's h.seq) AND unsolicited on any local change (manual override / failsafe trip,
 * ack_seq=0). The hub holds each SET_OUTPUT until it sees the matching ack_seq (held-until-ACK, retry). */
#define RBN_OUTSTATE_F_FAILSAFE_ACTIVE 0x01  ///< output currently in failsafe (hub lost)
#define RBN_OUTSTATE_F_LOCAL_OVERRIDE  0x02  ///< value set locally, not by the hub
/** @brief OUTPUT_STATE frame. */
typedef struct __attribute__((packed)) {
    rbn_hdr_t h;             ///< common header
    uint8_t   output_id;     ///< output reported
    uint8_t   kind;          ///< RBN_OUT_KIND_*
    uint16_t  value;         ///< actual applied value now
    uint32_t  ack_seq;       ///< SET_OUTPUT h.seq acked (0 = unsolicited state report)
    uint8_t   flags;         ///< RBN_OUTSTATE_F_*
    uint8_t   result;        ///< 0 OK · 1 clamped · 2 unknown output_id · 3 kind mismatch
} rbn_output_state_t;

/** @brief Output failsafe timeout (ms): if the node gets no authenticated hub frame (SET_OUTPUT keep-alive) for longer than this,
 * every output drives to its rbn_out_cap_t.failsafe_value and emits an unsolicited 0x41 with
 * RBN_OUTSTATE_F_FAILSAFE_ACTIVE. The hub MUST re-assert desired outputs at <= RBN_OUTPUT_FAILSAFE_MS/2 to
 * hold a non-failsafe state. Recovery: the next valid SET_OUTPUT clears failsafe. */
#define RBN_OUTPUT_FAILSAFE_MS 5000

/**
 * @brief Fill a frame header with the magic, protocol version and the given fields.
 * @param[out] h        Header to fill.
 * @param[in]  type     Message type (RBN_MSG_*).
 * @param[in]  seq      Per-sender sequence number.
 * @param[in]  node_ts  Sender Unix time (s), 0 if not synced.
 */
static inline void rbn_hdr_init(rbn_hdr_t *h, uint8_t type, uint32_t seq, uint32_t node_ts)
{
    h->magic0 = RBN_MAGIC0; h->magic1 = RBN_MAGIC1; h->proto_ver = RBN_PROTO_VER;
    h->msg_type = type; h->seq = seq; h->node_ts = node_ts;
}

/**
 * @brief Check the magic bytes and protocol version of a received header.
 * @param[in] h  Header to check.
 * @return Non-zero if the header is valid, 0 otherwise.
 */
static inline int rbn_hdr_ok(const rbn_hdr_t *h)
{
    return h->magic0 == RBN_MAGIC0 && h->magic1 == RBN_MAGIC1 && h->proto_ver == RBN_PROTO_VER;
}

#endif /* RBGRID_ESPNOW_PROTO_H */
