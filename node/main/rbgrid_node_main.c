/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbgrid_node_main.c
 * @brief RBgrid Node: a remote metering node that reports to an RBgrid hub over ESP-NOW.
 *
 * - Finds the hub by scanning Wi-Fi channels for its SYNC_BEACON and follows it if the router moves the hub
 *   to another channel. No Wi-Fi association and no channel setting are needed.
 * - Disciplines its clock to the hub (two-step beacon, least-squares servo) and keeps a wall clock from the
 *   hub's TIME_RESP, so 15-minute periods close on the hub's 15-minute boundaries.
 * - Reads rbAmp I2C metering modules at 0x50..0x52 (rbAmp library 1.7.2: BASIC and STD, STD-U3I3 phase by phase)
 *   and sends realtime frames and one PERIOD frame per 15-minute period. Voltage modules: energy from the module;
 *   current-only modules: the period's average current (the hub estimates the energy). Finished periods are kept
 *   in the `rbnhold` flash partition until the hub acknowledges them (about 5 days), across a node power cut.
 * - Pairing: inside the hub's pairing window the node and the hub exchange keys (X25519); the hub's key comes
 *   back wrapped (AES-CCM) and is kept in NVS. Data frames are then encrypted (ESP-NOW CCMP).
 * - Settings from the hub (CONFIG): per-input on/off and the clamp model.
 *
 * Target: ESP32, 4 MB flash, no PSRAM. Wire format: espnow_proto.h (shared with the hub).
 * Other build options in this file (RBN_ROLE_MASTER, RBN_FREERUN, RBN_OUTPUT_NODE) are development variants.
 */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_random.h"
#include "espnow_proto.h"
#include "espnow_crypto.h"   /* B3: PMK + PAIR_REQ auth tag */
#include "nvs.h"             /* configurable PMK (open-source: user key from NVS, not hardcoded) */
#include "esp_partition.h"   /* rbnhold: finished periods kept in flash until the hub ACKs them */
#include "esp_rom_crc.h"
#if !RBN_ROLE_MASTER
#include "driver/i2c_master.h"
#include "rbamp.h"
#endif
#if RBN_OUTPUT_NODE
#include "driver/gpio.h"     /* output-node reference: GPIO relay actuation */
#endif

/* Role: default = SLAVE (the node). Build the test beacon source with -DRBN_ROLE_MASTER=1. */
#ifndef RBN_ROLE_MASTER
#define RBN_ROLE_MASTER 0
#endif

/* Free-running node mode (for beacon-less OPEN receivers like ACRouter, which is passive open-RX and is NOT a
 * beacon/PTP master): broadcast OPEN REALTIME on a FIXED channel, with NO hub-beacon wait, NO channel-follow /
 * PTP time-sync / period-latch / pairing / encryption — a pure RT transmitter. Also sidesteps the perpetual
 * no-hub channel rescan + never-up encrypted-peer churn that starves rbn_rt's core (the task-WDT symptom).
 * Build with -DRBN_FREERUN=1. */
#ifndef RBN_FREERUN
#define RBN_FREERUN 0
#endif

/* Output-node mode (wireless dimmer/relay AT the load, driven by the hub via SET_OUTPUT 0x40). This build =
 * a GPIO-RELAY reference node, OPEN bring-up: broadcast HELLO(family=RELAY) discovery + SET_OUTPUT→GPIO +
 * OUTPUT_STATE ACK + hub-silence failsafe. Build -DRBN_OUTPUT_NODE=1. (DimmerLink dimmer variant swaps the GPIO
 * apply for an I2C write to the PY32 dimmer — added once its driver lands.) */
#ifndef RBN_OUTPUT_NODE
#define RBN_OUTPUT_NODE 0
#endif
#ifndef RBN_OUT_GPIO
#define RBN_OUT_GPIO 2       /* relay control GPIO (override -DRBN_OUT_GPIO=N) */
#endif
#ifndef RBN_OUT_ID
#define RBN_OUT_ID   0       /* this node's single output id (matches HELLO out_cap[0]) */
#endif
/* Output kind: 0 = GPIO relay (default, reference); 1 = DimmerLink dimmer over I2C (PY32 @0x50 on RBN_I2C_SDA/SCL).
 * Build the dimmer node with -DRBN_OUTPUT_NODE=1 -DRBN_OUT_IS_DIMMER=1. */
#ifndef RBN_OUT_IS_DIMMER
#define RBN_OUT_IS_DIMMER 0
#endif

#if RBN_ROLE_MASTER
static const char *TAG = "RBMASTER";
#else
static const char *TAG = "RBNODE";
#endif

#ifndef TEST_CHANNEL
#define TEST_CHANNEL   1     /* both roles must share this channel (override -DTEST_CHANNEL=N) */
#endif
#define SERVO_N        32    /* least-squares window (32 rides out loss + smooths skew at range limit) */
#define BEACON_MS      1000  /* master beacon cadence (fast for the test → more residual samples) */

/* rbAmp I3 on the node (current-only module 0x52). I2C pins = bench rbAmp convention; override with
 * -DRBN_I2C_SDA/SCL if wired differently. Address/CT model are the module's PERSISTED config
 * (calibrated on the bench) — the node does NOT re-provision them. */
#ifndef RBN_I2C_SDA
#define RBN_I2C_SDA    25
#endif
#ifndef RBN_I2C_SCL
#define RBN_I2C_SCL    26
#endif
#define RBN_RBAMP_ADDR 0x52  /* I3 */

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ---- discipline servo state (shared: recv-cb writer, reporter-task reader) ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static double  s_x[SERVO_N], s_y[SERVO_N];   /* x=local_rx_us, y=hub_tx_us */
static int     s_n = 0, s_head = 0;
static uint32_t s_last_beacon_id = 0;
static int64_t  s_last_beacon_rx = 0;
static bool     s_have_beacon = false;
/* residual (sync error) stats via Welford */
static uint32_t s_res_cnt = 0;
static double   s_res_mean = 0, s_res_m2 = 0, s_res_maxabs = 0;
static double   s_skew_ppm = 0, s_offset_us = 0;
static uint32_t s_beacons = 0, s_followups = 0;
static uint32_t s_max_beacon_id = 0;           /* highest beacon_id seen → loss = max_id - beacons */
static uint32_t s_rt_recv = 0;                 /* REALTIME frames received (transport check) */
static uint32_t s_epoch = 0;                   /* current master epoch (slave) */
static uint32_t s_epoch_resets = 0;            /* times the slave re-locked due to master restart */
static int64_t  s_last_beacon_us = 0;          /* esp_timer at last beacon RX (any channel) — for channel-follow */
static volatile bool    s_ch_locked = false;   /* slave: locked onto the hub's channel */
static volatile uint8_t s_locked_ch = 0;
static volatile uint8_t s_hub_channel = 0;     /* the hub's exact channel (from the beacon) */
/* held-until-ACK period state (used in the shared on_recv → must be declared before it) */
static uint32_t s_period_seq = 0;
static volatile uint32_t s_period_last_ack = 0;
/* B2 — OTA pairing (slave): set true once the hub PAIR_ACKs us into its registry. */
static volatile bool s_paired = false;
static uint32_t s_pair_seq = 0;
/* F1 — per-installation key: X25519 key pair of the current PAIR_REQ2 attempt + a received PAIR_ACK2 (handled in
 * pair_task, not in the Wi-Fi callback: it writes NVS). */
static uint8_t  s_kx_priv[32];
static volatile bool s_kx_ready = false;
static rbn_pair_ack2_t s_ack2;
static volatile bool s_ack2_pending = false;
static uint8_t s_ack2_src[6];   /* sender of the pending PAIR_ACK2 */
/* F5 — node settings from the hub (CONFIG): channel on/off mask (bit = flat channel id) + clamp models, kept in NVS. */
static uint16_t s_ch_off_mask = 0;               /* bit set = channel switched off (not reported) */
static uint8_t  s_cfg_buf[sizeof(rbn_config_t) + 16 * sizeof(rbn_cfg_chan_t)];
static volatile bool s_cfg_pending = false;
/* B3b — encrypted unicast data path. Learn the hub MAC from the beacon, add it as a CCMP peer (LMK from our
 * OWN mac, matching the hub's derive-from-our-mac), then send data unicast-encrypted instead of broadcast. */
static uint8_t  s_own_mac[6];
static uint8_t  s_my_lmk[16];                  /* HMAC(PMK, own_mac) — precomputed at boot */
static uint8_t  s_pmk[16];                     /* runtime PMK — NVS-configurable (open-source), dev-key fallback */
static uint8_t  s_cfg_channel = TEST_CHANNEL;  /* runtime WiFi/ESP-NOW channel — NVS-configurable, TEST_CHANNEL fallback */
#if RBN_OUTPUT_NODE
/* output-node runtime state */
static volatile int64_t  s_out_last_cmd_us = 0;  /* esp_timer at last valid SET_OUTPUT → failsafe watchdog */
static volatile uint16_t s_out_value = 0;        /* current applied output value */
static volatile bool     s_out_failsafe = false; /* currently in failsafe (hub lost) */
static uint8_t  s_out_hub_mac[6];                /* hub MAC learned from a SET_OUTPUT recv_info */
static volatile bool s_out_hub_valid = false;
static void output_on_set(const uint8_t *data, int len, const uint8_t *src);  /* fwd: called from on_recv */
#endif
static uint8_t  s_hub_mac[6];
/* The hub this node is paired to (NVS espnow/hubmac, set from the PAIR_ACK2 sender). With it set, beacons and
 * sync frames from any other hub are ignored, so a second RBgrid hub on the same Wi-Fi channel doesn't take over the
 * node's clock or its data path. While re-pairing and our hub has been silent for 8 s, another hub's beacon may lock
 * the channel (moving the node to another hub). */
static uint8_t  s_paired_hub[6];
static bool     s_paired_hub_set = false;
static volatile int64_t s_paired_hub_seen_us = 0;
static volatile bool s_repairing = false;
static volatile bool s_hub_mac_valid = false;
static volatile bool s_hub_enc_peer  = false;
static volatile int64_t s_last_enc_reply_us = 0;   /* last encrypted reply (TIME_RESP/PERIOD_ACK) — liveness */
/* C — hub-commanded LATCH_NOW request (set in on_recv, consumed by period_task). */
static volatile bool    s_latch_now_req = false;
static volatile uint8_t s_latch_reason  = 0;
/* master state */
static uint32_t s_beacon_seq = 0;              /* header seq for master's frames */
static uint32_t s_master_epoch = 0;            /* boot-unique; set once at master boot */
static volatile uint32_t s_pending_beacon_id = 0;

/* Least-squares y = a*x + b over the current window. Returns 1 if solvable. */
static int servo_fit(double *a, double *b)
{
    if (s_n < 4) return 0;
    double sx=0, sy=0, sxx=0, sxy=0;
    for (int i = 0; i < s_n; i++) { sx+=s_x[i]; sy+=s_y[i]; sxx+=s_x[i]*s_x[i]; sxy+=s_x[i]*s_y[i]; }
    double d = s_n*sxx - sx*sx;
    if (fabs(d) < 1e-6) return 0;
    *a = (s_n*sxy - sx*sy) / d;
    *b = (sy - (*a)*sx) / s_n;
    return 1;
}

static void servo_add(int64_t local_rx_us, uint64_t hub_tx_us)
{
    /* measure BEFORE adding: residual = actual - predicted (the sync error you'd have now) */
    double a, b;
    if (servo_fit(&a, &b)) {
        double pred = a*(double)local_rx_us + b;
        double res = (double)hub_tx_us - pred;
        double abr = fabs(res);
        s_res_cnt++;
        double delta = res - s_res_mean;
        s_res_mean += delta / s_res_cnt;
        s_res_m2 += delta * (res - s_res_mean);
        if (abr > s_res_maxabs) s_res_maxabs = abr;
        s_skew_ppm = (a - 1.0) * 1e6;
        s_offset_us = b + (a-1.0)*(double)local_rx_us;  /* hub_time - local at 'now' ~ */
    }
    /* add sample (ring) */
    int idx = s_head;
    s_x[idx] = (double)local_rx_us;
    s_y[idx] = (double)hub_tx_us;
    s_head = (s_head + 1) % SERVO_N;
    if (s_n < SERVO_N) s_n++;
}

/* current disciplined hub-time estimate (µs) = a·local_esp_timer + b (servo fit); raw esp_timer if no fit. */
static int64_t node_hub_time_us(void)
{
    double a, b;
    portENTER_CRITICAL(&s_mux);
    int ok = servo_fit(&a, &b);
    portEXIT_CRITICAL(&s_mux);
    int64_t t = esp_timer_get_time();
    return ok ? (int64_t)(a * (double)t + b) : t;
}

/* ---- A: wall-clock from the hub (TIME-sync) → calendar-aligned period boundaries ---- */
static volatile bool     s_wall_valid = false;
static int64_t  s_wall_anchor_unix_ms = 0;   /* hub Unix ms at the anchor */
static int64_t  s_wall_anchor_local_us = 0;  /* this node's esp_timer when that TIME_RESP arrived */

/* node wall-clock (Unix ms): anchor + this node's own elapsed time since the anchor. It runs on the local timer,
 * not on the servo's hub-time estimate: that estimate is the raw local timer until the first fit, so an anchor
 * taken before the fit and read after it put the clock ahead by the hub's uptime (up to an hour) for a minute
 * after every node start or hub radio restart. The local timer also keeps running through a radio gap and a
 * hub epoch change. Re-anchored every minute, so local drift (≤ ~40 ppm) stays at ms level. */
static int64_t node_wall_ms(void)
{
    if (!s_wall_valid) return 0;
    return s_wall_anchor_unix_ms + (esp_timer_get_time() - s_wall_anchor_local_us) / 1000;
}
static inline uint32_t node_wall_sec(void) { int64_t ms = node_wall_ms(); return ms > 0 ? (uint32_t)(ms / 1000) : 0; }

/* recv callback — runs in the WiFi task. Timestamp FIRST. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    int64_t t_rx = esp_timer_get_time();   /* capture ASAP */
    if (len < (int)sizeof(rbn_hdr_t)) return;
    const rbn_hdr_t *h = (const rbn_hdr_t *)data;
    if (!rbn_hdr_ok(h)) return;

    /* hub filter: sync frames only from the hub we're paired to (see s_paired_hub) */
    if ((h->msg_type == RBN_MSG_SYNC_BEACON || h->msg_type == RBN_MSG_SYNC_FOLLOWUP) && s_paired_hub_set && info) {
        if (memcmp(info->src_addr, s_paired_hub, 6) == 0) s_paired_hub_seen_us = t_rx;
        else if (!(s_repairing && t_rx - s_paired_hub_seen_us > 8000000LL)) return;
    }
    if (h->msg_type == RBN_MSG_SYNC_BEACON && len >= (int)sizeof(rbn_sync_beacon_t)) {
        const rbn_sync_beacon_t *b = (const rbn_sync_beacon_t *)data;
        portENTER_CRITICAL(&s_mux);
        if (b->epoch != s_epoch) {
            /* master restarted (reboot/OTA) → flush the servo + counters so old-epoch samples don't
             * poison the regression. Clean re-lock from here. */
            s_n = 0; s_head = 0; s_res_cnt = 0; s_res_mean = 0; s_res_m2 = 0; s_res_maxabs = 0;
            s_beacons = 0; s_followups = 0; s_max_beacon_id = 0; s_skew_ppm = 0; s_offset_us = 0;
            s_have_beacon = false; s_epoch = b->epoch; s_epoch_resets++;
            /* the wall clock stays valid: it runs on this node's timer, not on the hub's (see node_wall_ms) */
            s_hub_enc_peer = false; /* hub re-init'd ESP-NOW (reboot/OTA/re-flash) → its CCMP context is fresh;
                                     * re-establish our encrypted peer so the data path re-keys and resumes.
                                     * (Without this, after a long hub downtime the node re-locks sync but its
                                     * encrypted REALTIME/NODE_STATS no longer reach the hub until a node reset.) */
        }
        s_last_beacon_id = b->beacon_id;
        s_last_beacon_rx = t_rx;
        s_last_beacon_us = t_rx;          /* channel-follow heartbeat */
        s_have_beacon = true;
        s_beacons++;
        if (b->beacon_id > s_max_beacon_id) s_max_beacon_id = b->beacon_id;
        /* exact channel = the hub's own advertised WiFi channel (rx_ctrl can report an adjacent
         * overlapping channel when the node sits one channel off; trust the hub's value). The
         * channel-follow task re-pins the radio to it. */
        s_hub_channel = b->hub_channel;
        s_locked_ch = b->hub_channel;
        portEXIT_CRITICAL(&s_mux);
        if (info && info->src_addr && (!s_hub_mac_valid || memcmp(s_hub_mac, info->src_addr, 6) != 0)) {
            /* B3b: the hub MAC for the encrypted peer = the hub whose beacons we follow (the filter above keeps that
             * our paired hub); a change re-keys the peer */
            memcpy(s_hub_mac, info->src_addr, 6);
            s_hub_mac_valid = true;
            s_hub_enc_peer = false;
        }
    } else if (h->msg_type == RBN_MSG_SYNC_FOLLOWUP && len >= (int)sizeof(rbn_sync_followup_t)) {
        const rbn_sync_followup_t *f = (const rbn_sync_followup_t *)data;
        portENTER_CRITICAL(&s_mux);
        s_followups++;
        if (s_have_beacon && f->beacon_id == s_last_beacon_id) {
            servo_add(s_last_beacon_rx, f->hub_tx_us);
            s_have_beacon = false;
        }
        portEXIT_CRITICAL(&s_mux);
    } else if (h->msg_type == RBN_MSG_REALTIME && len >= (int)sizeof(rbn_realtime_t)) {
        portENTER_CRITICAL(&s_mux);
        s_rt_recv++;                 /* master counts the node's REALTIME frames */
        portEXIT_CRITICAL(&s_mux);
        /* master logs the node's measured I3 currents so standfw can compare vs the reference load */
        const rbn_realtime_t *m = (const rbn_realtime_t *)data;
        int n = m->rec_count; if (n > 3) n = 3;
        if (n >= 1 && len >= (int)(sizeof(rbn_realtime_t) + (size_t)n * sizeof(rbn_rt_rec_t))) {
            float i0 = m->recs[0].i_rms, i1 = (n > 1) ? m->recs[1].i_rms : 0, i2 = (n > 2) ? m->recs[2].i_rms : 0;
            ESP_LOGI(TAG, "RBN_RT node ch_count=%d | I3 i0=%.3fA i1=%.3fA i2=%.3fA v0=%.1fV p0=%.1fW",
                     m->rec_count, i0, i1, i2, m->recs[0].v_rms, m->recs[0].p_active);
        }
    } else if (h->msg_type == RBN_MSG_PERIOD_ACK && len >= (int)sizeof(rbn_period_ack_t)) {
        const rbn_period_ack_t *a = (const rbn_period_ack_t *)data;   /* slave: clear held PERIOD */
        s_period_last_ack = a->ack_seq;
        s_paired = true;   /* B3b: an encrypted reply proves the hub holds us as a registered peer */
        s_last_enc_reply_us = t_rx;
    } else if (h->msg_type == RBN_MSG_CONFIG && len >= (int)sizeof(rbn_config_t)) {
        const rbn_config_t *c = (const rbn_config_t *)data;   /* F5: applied in pair_task (module I2C + NVS) */
        size_t need = sizeof(rbn_config_t) + (size_t)c->chan_count * sizeof(rbn_cfg_chan_t);
        if (!s_cfg_pending && c->chan_count <= 16 && len >= (int)need) { memcpy(s_cfg_buf, data, need); s_cfg_pending = true; }
        s_last_enc_reply_us = t_rx;
    } else if (h->msg_type == RBN_MSG_LATCH_NOW && len >= (int)sizeof(rbn_latch_now_t)) {
        const rbn_latch_now_t *l = (const rbn_latch_now_t *)data;     /* C: hub says close the period now */
        s_latch_reason = l->reason;
        s_latch_now_req = true;
        s_last_enc_reply_us = t_rx;
    } else if (h->msg_type == RBN_MSG_PAIR_ACK2 && len >= (int)sizeof(rbn_pair_ack2_t)) {
        const rbn_pair_ack2_t *a = (const rbn_pair_ack2_t *)data;     /* F1: our key, wrapped for us */
        if (!s_ack2_pending && memcmp(a->node_mac, s_own_mac, 6) == 0) {
            s_ack2 = *a; s_ack2_pending = true;
            if (info) memcpy(s_ack2_src, info->src_addr, 6);   /* the hub that paired us */
        }
    } else if (h->msg_type == RBN_MSG_PAIR_ACK && len >= (int)sizeof(rbn_pair_ack_t)) {
        const rbn_pair_ack_t *a = (const rbn_pair_ack_t *)data;       /* B2: hub registered us */
        if (a->accepted) s_paired = true;
    } else if (h->msg_type == RBN_MSG_TIME_RESP && len >= (int)sizeof(rbn_time_resp_t)) {
        const rbn_time_resp_t *r = (const rbn_time_resp_t *)data;     /* A: anchor wall-clock to hub time */
        if (r->hub_unix_ms > 0) {
            s_wall_anchor_unix_ms  = (int64_t)r->hub_unix_ms;
            s_wall_anchor_local_us = t_rx;
            s_wall_valid = true;
        }
        s_paired = true;   /* B3b: an encrypted TIME_RESP proves the hub holds us as a registered peer */
        s_last_enc_reply_us = t_rx;
    } else if (h->msg_type == RBN_MSG_NODE_STATS && len >= (int)sizeof(rbn_node_stats_t)) {
        /* master prints the remote node's sync health → read a no-COM node via the hub's UART.
         * loss is reported as a ROLLING window (delta between consecutive NODE_STATS) — the cumulative
         * (from-boot) ratio is misleading after a transient drop while the node is carried (standfw). */
        const rbn_node_stats_t *s = (const rbn_node_stats_t *)data;
        static uint32_t prev_beacons = 0, prev_maxid = 0; static bool prev_valid = false;  /* single-node prototype */
        float win_loss = 0.0f;
        if (prev_valid && s->max_beacon_id >= prev_maxid && s->beacons_recv >= prev_beacons) {
            uint32_t d_exp = s->max_beacon_id - prev_maxid;
            uint32_t d_rcv = s->beacons_recv - prev_beacons;
            if (d_exp > 0) win_loss = 100.0f * (float)(d_exp - (d_rcv > d_exp ? d_exp : d_rcv)) / (float)d_exp;
        }
        if (s->max_beacon_id < prev_maxid) prev_valid = false;   /* node rebooted → resync window */
        prev_beacons = s->beacons_recv; prev_maxid = s->max_beacon_id; prev_valid = true;
        float cum_loss = (s->max_beacon_id > 0)
            ? 100.0f * (float)(s->max_beacon_id - (s->beacons_recv > s->max_beacon_id ? s->max_beacon_id : s->beacons_recv)) / (float)s->max_beacon_id : 0.0f;
        ESP_LOGI(TAG, "RBN_NODESTATS recv: beacons=%u/%u | loss win=%.1f%% cum=%.1f%% | fits=%u | resid_us mean=%.1f sd=%.1f maxabs=%.1f | skew_ppm=%.2f",
                 (unsigned)s->beacons_recv, (unsigned)s->max_beacon_id, win_loss, cum_loss, (unsigned)s->fits,
                 s->resid_mean_us, s->resid_sd_us, s->resid_maxabs_us, s->skew_ppm);
#if RBN_OUTPUT_NODE
    } else if (h->msg_type == RBN_MSG_SET_OUTPUT && len >= (int)sizeof(rbn_set_output_t) && info) {
        output_on_set(data, len, info->src_addr);   /* hub->node output command → apply + ACK */
#endif
    }
}

#if RBN_ROLE_MASTER
/* send callback — fires after a frame leaves; capture egress esp_timer for the PTP TX stamp.
 * If the just-sent frame was a beacon, immediately send its SYNC_FOLLOWUP with that timestamp. */
static void on_send(const uint8_t *mac, esp_now_send_status_t status)
{
    int64_t t_tx = esp_timer_get_time();
    uint32_t bid;
    portENTER_CRITICAL(&s_mux);
    bid = s_pending_beacon_id; s_pending_beacon_id = 0;
    portEXIT_CRITICAL(&s_mux);
    if (bid) {
        rbn_sync_followup_t f;
        rbn_hdr_init(&f.h, RBN_MSG_SYNC_FOLLOWUP, s_beacon_seq++, 0);
        f.beacon_id = bid;
        f.hub_tx_us = (uint64_t)t_tx;
        esp_now_send(BCAST, (uint8_t *)&f, sizeof(f));   /* its own send-cb sees pending=0 → no loop */
    }
}

static void beacon_task(void *arg)
{
    uint32_t bid = 0;
    while (1) {
        bid++;
        rbn_sync_beacon_t b;
        rbn_hdr_init(&b.h, RBN_MSG_SYNC_BEACON, s_beacon_seq++, 0);
        b.beacon_id = bid;
        b.epoch = s_master_epoch;
        portENTER_CRITICAL(&s_mux);
        s_pending_beacon_id = bid;
        portEXIT_CRITICAL(&s_mux);
        esp_now_send(BCAST, (uint8_t *)&b, sizeof(b));    /* on_send → SYNC_FOLLOWUP */
        vTaskDelay(pdMS_TO_TICKS(BEACON_MS));
    }
}

static void master_report_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        portENTER_CRITICAL(&s_mux);
        uint32_t rt = s_rt_recv;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "RBN_MASTER beacon_cadence_ms=%d realtime_recv=%u (transport ok if rising)",
                 BEACON_MS, (unsigned)rt);
    }
}
#endif

#if !RBN_ROLE_MASTER
/* ---- rbAmp mini-fleet on the node (energy module(s) + current-only, mirrors the hub's fleet) ---- */
#ifndef RBN_NODE_CHANS
#define RBN_NODE_CHANS 3     /* all three channels of a module (U3I3 / UI3); unused inputs are switched off in the node settings */
#endif
#ifndef RBN_PERIOD_S
#define RBN_PERIOD_S   30    /* on-node period-latch cadence (test). >=30s per the rbAmp cadence rule */
#endif
#ifndef RBN_BOUNDARY_S
#define RBN_BOUNDARY_S 900   /* A: calendar period boundary, wall-clock sec. PRODUCTION 900 (the hub rolls these up as quarters; -DRBN_BOUNDARY_S=60 for a fast test build);
                              * PRODUCTION 900 (=15min). The node latches at multiples of this. */
#endif
#ifndef RBN_PERIOD_TYPE
#define RBN_PERIOD_TYPE 1    /* 15MIN — the hub's native rollup (unit_id-based, shared w/ rbAmp) aggregates
                              * 15min→1HOUR→flash + computes the tariff at HOUR finalize. (0=3SEC realtime,
                              * 2=1HOUR would be overwritten by the accumulator recompute — don't.) */
#endif
#define RBN_FLEET_N 3
static const uint8_t RBN_FLEET_ADDR[RBN_FLEET_N] = {0x50, 0x51, 0x52};
static rbamp_handle_t s_dev[RBN_FLEET_N];
static uint8_t s_dev_chans[RBN_FLEET_N];
static bool    s_dev_vhw[RBN_FLEET_N];
static int     s_fleet_present = 0;
static bool    s_fleet_has_v = false;
static SemaphoreHandle_t s_rbamp_lock = NULL;

/* Current-only modules (no voltage on the module): the node averages each channel's RMS current over the period
 * from the 1-s live reads and sends it with voltage 0 / energy 0; the hub estimates the energy from its nominal
 * voltage and flags the hour ESTIMATED. */
typedef struct { double sum; uint32_t n; float min, max; } rbn_iacc_t;
static rbn_iacc_t s_iacc[3][3];
static int64_t    s_iacc_start_us = 0;      /* start of the window being averaged (node boot for the first one) */
static portMUX_TYPE s_iacc_mux = portMUX_INITIALIZER_UNLOCKED;
static void iacc_add(int mod, uint8_t ch, float i_a)
{
    if (mod < 0 || mod >= 3 || ch >= 3 || !isfinite(i_a)) return;
    portENTER_CRITICAL(&s_iacc_mux);
    rbn_iacc_t *a = &s_iacc[mod][ch];
    if (a->n == 0) { a->min = a->max = i_a; } else { if (i_a < a->min) a->min = i_a; if (i_a > a->max) a->max = i_a; }
    a->sum += i_a; a->n++;
    portEXIT_CRITICAL(&s_iacc_mux);
}

static inline uint8_t node_chan_id(int mod, uint8_t ch) { return (uint8_t)(mod * 4 + ch); }
static inline bool node_chan_on(int mod, uint8_t ch) { return !(s_ch_off_mask & (1u << (mod * 4 + ch))); }
/** @brief Lowest voltage taken as mains on a voltage input; below it the channel is handled as current-only. */
#define RBN_V_MIN 50.0f

/* Voltage seen by channel @p ch: the matching phase on a three-phase module (STD-U3I3: phase = channel; a dead
 * phase reads low and that channel goes current-only), else the module's single voltage. Call with s_rbamp_lock held. */
static float mod_voltage(int i, uint8_t ch)
{
    float v = 0.0f;
    if (ch > 0 && rbamp_read_voltage(s_dev[i], ch, &v) == ESP_OK) return v;   /* this phase, even if dead (low) */
    v = 0.0f;
    rbamp_read_voltage(s_dev[i], 0, &v);
    return v;
}
static inline uint8_t mod_chans(int i) { uint8_t n = s_dev_chans[i]; if (n > RBN_NODE_CHANS) n = RBN_NODE_CHANS; if (n > 3) n = 3; return n; }
/** @brief Inputs a hub takes per node (its registry holds 4 channels per node; a longer pairing list is rejected). */
#define RBN_HUB_MAX_CH 4
/** @brief Whether input @p ch of module @p i is one of the node's first RBN_HUB_MAX_CH inputs (modules in address
 *  order, channels in order). The rest are never paired, sent or configured, the same in every frame. */
static bool chan_reported(int i, uint8_t ch)
{
    unsigned before = 0;
    for (int k = 0; k < i; k++) if (s_dev[k]) before += mod_chans(k);
    return ch < mod_chans(i) && before + ch < RBN_HUB_MAX_CH;
}

static void rbamp_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = RBN_I2C_SDA, .scl_io_num = RBN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGW(TAG, "I2C bus init failed (SDA%d/SCL%d)", RBN_I2C_SDA, RBN_I2C_SCL); return; }
    s_rbamp_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < RBN_FLEET_N; i++) {
        rbamp_handle_t d = NULL;
        /* begin_resume: keep the window the module accumulated while the node was off (node reboot with the module
         * still powered): that energy goes into the first period instead of being discarded. */
        if (rbamp_new(bus, RBN_FLEET_ADDR[i], &d) == ESP_OK && rbamp_begin_resume(d) == ESP_OK) {
            s_dev[i] = d; s_dev_chans[i] = rbamp_channels(d); s_dev_vhw[i] = rbamp_has_voltage_hw(d);
            if (s_dev_vhw[i]) s_fleet_has_v = true;
            s_fleet_present++;
            ESP_LOGI(TAG, "rbAmp 0x%02X UP: fw=0x%02X ch=%u voltage_hw=%s", RBN_FLEET_ADDR[i],
                     rbamp_firmware_version(d), s_dev_chans[i], s_dev_vhw[i] ? "YES (energy module)" : "no (current-only)");
        }
    }
    if (s_fleet_present == 0) ESP_LOGW(TAG, "no rbAmp on SDA%d/SCL%d — SYNTHETIC REALTIME", RBN_I2C_SDA, RBN_I2C_SCL);
    else ESP_LOGI(TAG, "node fleet: %d module(s), fleet_voltage=%s", s_fleet_present, s_fleet_has_v ? "yes" : "NO");
}

/* B3b — once the hub MAC is known, add it as a CCMP-encrypted unicast peer (idempotent). After this the node
 * sends all data frames encrypted to the hub instead of broadcast. The broadcast peer (beacon RX + PAIR_REQ
 * TX) stays as-is. */
static void node_ensure_hub_peer(void)
{
    if (s_hub_enc_peer || !s_hub_mac_valid) return;
    esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = true };
    memcpy(p.peer_addr, s_hub_mac, 6);
    memcpy(p.lmk, s_my_lmk, 16);
    esp_err_t e = esp_now_is_peer_exist(s_hub_mac) ? esp_now_mod_peer(&p) : esp_now_add_peer(&p);
    if (e == ESP_OK) {
        s_hub_enc_peer = true;
        ESP_LOGI(TAG, "B3b: encrypted hub peer %02X:%02X:%02X:%02X:%02X:%02X added (data path now CCMP)",
                 s_hub_mac[0],s_hub_mac[1],s_hub_mac[2],s_hub_mac[3],s_hub_mac[4],s_hub_mac[5]);
    } else {
        ESP_LOGW(TAG, "B3b: enc hub peer add failed: %s", esp_err_to_name(e));
    }
}

/* Send a data frame to the hub: B3b → unicast-encrypted (only once the encrypted peer is up; held otherwise,
 * a ~1s gap after lock — data is best-effort/held-until-ACK so this is safe and never leaks plaintext). */
static inline void node_send_hub(const void *buf, size_t len)
{
    if (s_hub_enc_peer) esp_now_send(s_hub_mac, buf, len);
}

/* REALTIME: all present modules' channels (energy module → real v/i/p; current-only → I). */
static void realtime_task(void *arg)
{
    uint32_t seq = 0;
    uint8_t buf[sizeof(rbn_realtime_t) + 8 * sizeof(rbn_rt_rec_t)];
    while (1) {
        node_ensure_hub_peer();   /* B3b: bring up the encrypted hub peer once its MAC is known */
        rbn_realtime_t *m = (rbn_realtime_t *)buf;
        rbn_hdr_init(&m->h, RBN_MSG_REALTIME, seq++, 0);
        uint8_t rc = 0;
        for (int i = 0; i < RBN_FLEET_N && rc < 8; i++) {
            if (!s_dev[i]) continue;
            uint8_t n = mod_chans(i);
            for (uint8_t ch = 0; ch < n && rc < 8; ch++) {
                if (!node_chan_on(i, ch) || !chan_reported(i, ch)) continue;   /* F5: switched off by the hub */
                float ii = 0, v = 0, p = 0, pf = 0, f = 0;
                /* timeout (not portMAX_DELAY): never block forever on the rbAmp lock — if period_task
                 * holds it (e.g. during a slow snapshot after a hub reboot), skip this cycle and keep
                 * realtime flowing. This is the realtime-resume guarantee standfw flagged. */
                if (xSemaphoreTake(s_rbamp_lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;
                rbamp_read_current(s_dev[i], ch, &ii);
                if (s_dev_vhw[i]) { v = mod_voltage(i, ch); rbamp_read_power(s_dev[i], ch, &p);
                                    rbamp_read_power_factor(s_dev[i], ch, &pf); rbamp_read_frequency(s_dev[i], &f); }
                xSemaphoreGive(s_rbamp_lock);
                /* A voltage module with no mains on its voltage input (V below RBN_V_MIN) is handled as current-only:
                 * live V/P sent as 0 (the hub estimates from its nominal voltage), the period from the averaged current. */
                if (v < RBN_V_MIN) { v = 0; p = 0; pf = 0; }
                iacc_add(i, ch, ii);   /* period average current (used when the module has no live voltage) */
                m->recs[rc++] = (rbn_rt_rec_t){ .channel_id = node_chan_id(i, ch), .v_rms = v, .i_rms = ii,
                    .p_active = p, .pf_x1000 = (int16_t)(pf * 1000.0f), .freq_x100 = (uint16_t)(f * 100.0f), .flags = 0x01 };
            }
        }
        if (rc == 0 && s_fleet_present == 0) {   /* synthetic fallback ONLY when there's no rbAmp at all
                                                  * (a fleet-present-but-lock-busy cycle just skips, not fakes) */
            float ph = (float)(seq % 50) / 50.0f * 6.2832f;
            m->recs[0] = (rbn_rt_rec_t){ .channel_id = 0, .v_rms = 230.0f, .i_rms = 1.0f + 0.2f * sinf(ph),
                .p_active = 230.0f, .pf_x1000 = 1000, .freq_x100 = 5000, .flags = 0x01 };
            rc = 1;
        }
        m->rec_count = rc;
        size_t len = sizeof(rbn_realtime_t) + (size_t)rc * sizeof(rbn_rt_rec_t);
#if RBN_FREERUN
        if (rc > 0) esp_now_send(BCAST, buf, len);            /* free-run: broadcast OPEN — no hub/beacon/crypto */
#else
        if (s_ch_locked && rc > 0) node_send_hub(buf, len);   /* B3b: unicast-encrypted */
#endif
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* A: ask the hub for wall-clock periodically (TIME_REQ) so the node can anchor its clock + latch periods
 * on calendar boundaries. Fast until synced, then once a minute. */
static void time_task(void *arg)
{
    uint32_t tok = 0;
    while (1) {
        if (s_ch_locked) {
            rbn_time_req_t q;
            rbn_hdr_init(&q.h, RBN_MSG_TIME_REQ, ++tok, node_wall_sec());
            q.req_token = tok;
            node_send_hub(&q, sizeof(q));   /* B3b: unicast-encrypted; held until the hub peer is up */
        }
        vTaskDelay(pdMS_TO_TICKS(s_wall_valid ? 60000 : 5000));
    }
}

/* PERIOD (P3): on the voltage-bearing ENERGY modules, latch a finished period via the rbAmp on-device
 * snapshot (real avg_p / W·h over the integrated window), send a PERIOD frame, and HOLD it until a
 * PERIOD_ACK so billing energy survives a dropped frame. Current-only modules (I3) stream realtime only
 * in v1 (host V×I energy needs a node voltage reference — a later refinement). */
/* Held-until-ACK queue. A finished PERIOD frame stays here (RAM) until the hub ACKs it — through a hub restart
 * or a radio gap of any length — and is resent unchanged (same seq + node_ts: the hub recognizes a resend of a
 * frame it already counted and only ACKs it again). Frames go out in order, one in flight. Before, a frame was
 * dropped after 5 tries (~10 s) while the log said "kept". Full queue: the oldest frame is dropped, loudly. */
#define RBN_HELD_MAX 8
static uint8_t  s_held_buf[RBN_HELD_MAX][sizeof(rbn_period_t) + 8 * sizeof(rbn_period_rec_t)];
static uint16_t s_held_len[RBN_HELD_MAX];
static uint32_t s_held_seq[RBN_HELD_MAX];
static uint8_t  s_held_head = 0, s_held_n = 0;
static uint32_t s_held_dropped = 0;
static SemaphoreHandle_t s_held_lock;

/* ---- rbnhold: the held queue in flash ----------------------------------------------------------------------
 * Each finished PERIOD frame is written to a 512-byte slot of the `rbnhold` partition (256 KB = 512 slots, about
 * 5 days at one period per 15 min) before it is sent, and marked ACKed in place when the hub acknowledges it, so
 * frames survive a node power cut and a hub outage of days. Slot: state word (FFFFFFFF empty, FFFFFF00 pending,
 * 00000000 ACKed: bits only cleared, no erase) + magic + seq + len + CRC32 + frame. Writing into a new 4 KB sector
 * erases it first; pending frames still in it (the ring is full) are the oldest and are dropped, loudly.
 * Without the partition (an old partition table) the RAM queue of 8 frames above is used. */
#define RBNH_SLOT        512u
#define RBNH_SECTOR      4096u
#define RBNH_PER_SECTOR  (RBNH_SECTOR / RBNH_SLOT)
#define RBNH_MAGIC       0x484E4252u   /* 'RBNH' */
#define RBNH_EMPTY       0xFFFFFFFFu
#define RBNH_PENDING     0xFFFFFF00u
#define RBNH_ACKED       0x00000000u
typedef struct __attribute__((packed)) { uint32_t state, magic, seq; uint16_t len, rsv; uint32_t crc; } rbnh_hdr_t;
static const esp_partition_t *s_hold_part = NULL;
static uint16_t  s_hold_slots = 0;
static uint16_t *s_hold_q = NULL;          /* pending slot indices, oldest first (ring) */
static uint16_t  s_hold_q_head = 0, s_hold_q_n = 0;
static uint16_t  s_hold_next = 0;          /* next slot to write */

static void hold_q_push(uint16_t slot) { s_hold_q[(s_hold_q_head + s_hold_q_n) % s_hold_slots] = slot; s_hold_q_n++; }

/* Boot: scan the partition, queue the pending frames in seq order, continue the period seq after the highest. */
static void hold_init(void)
{
    if (!s_held_lock) s_held_lock = xSemaphoreCreateMutex();   /* held_flush needs it for frames left from before boot */
    s_hold_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "rbnhold");
    if (!s_hold_part) { ESP_LOGW(TAG, "no rbnhold partition — held periods in RAM only (8)"); return; }
    s_hold_slots = (uint16_t)(s_hold_part->size / RBNH_SLOT);
    s_hold_q = calloc(s_hold_slots, sizeof(uint16_t));
    uint32_t *seqs = calloc(s_hold_slots, sizeof(uint32_t));
    if (!s_hold_q || !seqs) { free(seqs); free(s_hold_q); s_hold_q = NULL; s_hold_part = NULL; return; }
    uint32_t max_seq = 0; uint16_t max_slot = 0, pend = 0; bool any = false;
    for (uint16_t k = 0; k < s_hold_slots; k++) {
        rbnh_hdr_t h;
        if (esp_partition_read(s_hold_part, (size_t)k * RBNH_SLOT, &h, sizeof(h)) != ESP_OK) continue;
        if (h.magic != RBNH_MAGIC || h.len == 0 || h.len > RBNH_SLOT - sizeof(h)) continue;
        if (!any || h.seq > max_seq) { max_seq = h.seq; max_slot = k; any = true; }
        if (h.state == RBNH_PENDING) {
            uint8_t fb[RBNH_SLOT];
            if (esp_partition_read(s_hold_part, (size_t)k * RBNH_SLOT + sizeof(h), fb, h.len) != ESP_OK) continue;
            if (esp_rom_crc32_le(0, fb, h.len) != h.crc) continue;
            seqs[pend] = h.seq; s_hold_q[pend] = k; pend++;
        }
    }
    /* order the pending ones by seq (insertion sort: a few hundred at most) */
    for (uint16_t a = 1; a < pend; a++) {
        uint32_t sv = seqs[a]; uint16_t kv = s_hold_q[a]; int b = a - 1;
        while (b >= 0 && seqs[b] > sv) { seqs[b + 1] = seqs[b]; s_hold_q[b + 1] = s_hold_q[b]; b--; }
        seqs[b + 1] = sv; s_hold_q[b + 1] = kv;
    }
    free(seqs);
    s_hold_q_head = 0; s_hold_q_n = pend;
    s_hold_next = any ? (uint16_t)((max_slot + 1) % s_hold_slots) : 0;
    if (any && max_seq > s_period_seq) s_period_seq = max_seq;
    ESP_LOGI(TAG, "rbnhold: %u slots, %u pending period frame(s), next seq %u", (unsigned)s_hold_slots, (unsigned)pend,
             (unsigned)(s_period_seq + 1));
}

static bool hold_write(const uint8_t *frame, size_t len, uint32_t seq)
{
    if (len > RBNH_SLOT - sizeof(rbnh_hdr_t)) return false;
    uint16_t slot = s_hold_next;
    for (uint16_t tries = 0; ; tries++) {
        if (tries >= s_hold_slots) return false;
        if (slot % RBNH_PER_SECTOR == 0) break;            /* sector start: erased below */
        /* Mid-sector: write only into a fully erased slot. A power cut during a write leaves a slot partly
         * written with no valid header; writing over it (NOR only clears bits) would corrupt the new frame. */
        uint8_t chk[RBNH_SLOT]; bool clean = esp_partition_read(s_hold_part, (size_t)slot * RBNH_SLOT, chk, sizeof(chk)) == ESP_OK;
        for (size_t k = 0; clean && k < sizeof(chk); k++) clean = (chk[k] == 0xFF);
        if (clean) break;
        slot = (uint16_t)((slot + 1) % s_hold_slots);
    }
    if (slot % RBNH_PER_SECTOR == 0) {   /* entering a sector: drop its pending frames (the oldest), then erase */
        uint16_t dropped = 0;
        while (s_hold_q_n && s_hold_q[s_hold_q_head] / RBNH_PER_SECTOR == slot / RBNH_PER_SECTOR) {
            s_hold_q_head = (uint16_t)((s_hold_q_head + 1) % s_hold_slots); s_hold_q_n--; dropped++;
        }
        if (dropped) { s_held_dropped += dropped; ESP_LOGE(TAG, "rbnhold full — %u oldest unACKed period frame(s) DROPPED", dropped); }
        if (esp_partition_erase_range(s_hold_part, (size_t)slot * RBNH_SLOT, RBNH_SECTOR) != ESP_OK) return false;
    }
    /* frame first, then the header (already PENDING) in one write: a cut before or during the header leaves a slot
     * whose magic/CRC don't check, which the boot scan skips */
    rbnh_hdr_t h = { .state = RBNH_PENDING, .magic = RBNH_MAGIC, .seq = seq, .len = (uint16_t)len, .rsv = 0xFFFF,
                     .crc = esp_rom_crc32_le(0, frame, len) };
    size_t off = (size_t)slot * RBNH_SLOT;
    if (esp_partition_write(s_hold_part, off + sizeof(h), frame, len) != ESP_OK) return false;
    if (esp_partition_write(s_hold_part, off, &h, sizeof(h)) != ESP_OK) return false;
    hold_q_push(slot);
    s_hold_next = (uint16_t)((slot + 1) % s_hold_slots);
    return true;
}

static void held_push(const uint8_t *frame, size_t len, uint32_t seq)
{
    if (!s_held_lock) s_held_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_held_lock, portMAX_DELAY);
    if (s_hold_part && hold_write(frame, len, seq)) { xSemaphoreGive(s_held_lock); return; }
    if (s_hold_part) ESP_LOGE(TAG, "rbnhold write failed — PERIOD seq=%u kept in RAM", (unsigned)seq);
    if (s_held_n == RBN_HELD_MAX) {   /* drop the oldest */
        ESP_LOGE(TAG, "held queue full — PERIOD seq=%u DROPPED (never ACKed)", (unsigned)s_held_seq[s_held_head]);
        s_held_head = (uint8_t)((s_held_head + 1) % RBN_HELD_MAX); s_held_n--; s_held_dropped++;
    }
    uint8_t slot = (uint8_t)((s_held_head + s_held_n) % RBN_HELD_MAX);
    memcpy(s_held_buf[slot], frame, len); s_held_len[slot] = (uint16_t)len; s_held_seq[slot] = seq;
    s_held_n++;
    xSemaphoreGive(s_held_lock);
}

/* Send one frame and wait for its ACK: up to 3 tries × 2 s. */
static bool held_send_wait(const uint8_t *frame, size_t len, uint32_t seq)
{
    for (int t = 0; t < 3; t++) {
        node_send_hub(frame, len);   /* B3b: unicast-encrypted */
        for (int w = 0; w < 20; w++) { if (s_period_last_ack == seq) return true; vTaskDelay(pdMS_TO_TICKS(100)); }
    }
    return false;
}

/* Send held frames in order (flash first, then the RAM fallback). Stops at the first one the hub doesn't ACK (it is
 * retried on the next call). Called after a latch and every few seconds from period_task. */
#define RBN_FLUSH_MAX 4   /* frames per call: a backlog of days drains over successive calls, boundaries stay on time */
static void held_flush(void)
{
    if (!s_held_lock || !s_hub_enc_peer) return;
    xSemaphoreTake(s_held_lock, portMAX_DELAY);
    unsigned sent = 0;
    while (s_hold_part && s_hold_q_n && sent < RBN_FLUSH_MAX) {
        uint16_t slot = s_hold_q[s_hold_q_head];
        size_t off = (size_t)slot * RBNH_SLOT;
        rbnh_hdr_t h; uint8_t fb[RBNH_SLOT];
        bool ok = esp_partition_read(s_hold_part, off, &h, sizeof(h)) == ESP_OK && h.magic == RBNH_MAGIC &&
                  h.state == RBNH_PENDING && h.len <= RBNH_SLOT - sizeof(h) &&
                  esp_partition_read(s_hold_part, off + sizeof(h), fb, h.len) == ESP_OK &&
                  esp_rom_crc32_le(0, fb, h.len) == h.crc;
        if (!ok) { s_hold_q_head = (uint16_t)((s_hold_q_head + 1) % s_hold_slots); s_hold_q_n--; continue; }   /* unreadable: skip */
        if (!held_send_wait(fb, h.len, h.seq)) {
            ESP_LOGW(TAG, "PERIOD seq=%u not ACKed yet — held (%u in flash)", (unsigned)h.seq, (unsigned)s_hold_q_n);
            xSemaphoreGive(s_held_lock);
            return;
        }
        uint32_t st = RBNH_ACKED;
        esp_partition_write(s_hold_part, off, &st, sizeof(st));
        ESP_LOGW(TAG, "PERIOD seq=%u ACKed", (unsigned)h.seq);
        s_hold_q_head = (uint16_t)((s_hold_q_head + 1) % s_hold_slots); s_hold_q_n--; sent++;
    }
    while (s_held_n && sent < RBN_FLUSH_MAX) {
        uint8_t slot = s_held_head; uint32_t seq = s_held_seq[slot];
        if (!held_send_wait(s_held_buf[slot], s_held_len[slot], seq)) {
            ESP_LOGW(TAG, "PERIOD seq=%u not ACKed yet — held (%u in queue)", (unsigned)seq, (unsigned)s_held_n); break;
        }
        ESP_LOGW(TAG, "PERIOD seq=%u ACKed", (unsigned)seq);
        s_held_head = (uint8_t)((s_held_head + 1) % RBN_HELD_MAX); s_held_n--; sent++;
    }
    xSemaphoreGive(s_held_lock);
}

static inline unsigned held_pending(void) { return (unsigned)s_held_n + (unsigned)s_hold_q_n; }

/* Latch every energy module's finished period NOW, build a PERIOD frame, send it held-until-ACK. Shared by
 * the calendar-boundary tick (period_task) and the hub's LATCH_NOW command (C). wsec = wall-clock to stamp. */
static void node_latch_and_send(uint32_t wsec, uint8_t reason)
{
    uint8_t buf[sizeof(rbn_period_t) + 8 * sizeof(rbn_period_rec_t)];
    rbn_period_t *m = (rbn_period_t *)buf;
    uint8_t rc = 0;
    bool no_v[RBN_FLEET_N][3] = { { false } };   /* input of a voltage module with no mains on its phase this period */
    for (int i = 0; i < RBN_FLEET_N && rc < 8; i++) {
        if (!s_dev[i] || !s_dev_vhw[i]) continue;          /* voltage modules here; current-only below */
        rbamp_period_snapshot_t snap = {0};
        esp_err_t err = ESP_FAIL; bool skip = false; float v = 0;
        for (int a = 0; a < 4; a++) {
            /* take/RELEASE the lock per attempt (don't hold it across the retry delay → realtime keeps
             * reading between attempts; never starve realtime_task). */
            if (xSemaphoreTake(s_rbamp_lock, pdMS_TO_TICKS(500)) != pdTRUE) break;
            err = rbamp_read_period_snapshot(s_dev[i], &snap, skip ? 0 : 50, skip);
            if (err == ESP_OK && snap.valid) { v = mod_voltage(i, 0); xSemaphoreGive(s_rbamp_lock); break; }
            xSemaphoreGive(s_rbamp_lock);
            skip = true; vTaskDelay(pdMS_TO_TICKS(60 + 60 * a));
        }
        if (err != ESP_OK || !snap.valid) { ESP_LOGW(TAG, "module 0x%02X period stale (need >=30s)", RBN_FLEET_ADDR[i]); continue; }
        float dt_h = (float)snap.master_dt_ms / 3600000.0f;
        uint8_t n = mod_chans(i);
        for (uint8_t ch = 0; ch < n && rc < 8; ch++) {
            if (ch >= 3 || !node_chan_on(i, ch) || !chan_reported(i, ch)) continue;
            float vch = v;   /* three-phase module: each channel's own phase voltage */
            if (ch > 0 && xSemaphoreTake(s_rbamp_lock, pdMS_TO_TICKS(200)) == pdTRUE) { vch = mod_voltage(i, ch); xSemaphoreGive(s_rbamp_lock); }
            if (vch < RBN_V_MIN) { no_v[i][ch] = true; continue; }   /* no mains on this input's voltage: averaged current below */
            if (isnan(snap.avg_p[ch])) continue;
            float avg_p = snap.avg_p[ch];
            float max_p = (ch == 0 && !isnan(snap.max_p)) ? snap.max_p : avg_p;
            float i_a = (vch > 0.0f) ? avg_p / vch : 0.0f;
            m->recs[rc++] = (rbn_period_rec_t){ .channel_id = node_chan_id(i, ch), .period_type = RBN_PERIOD_TYPE,
                .period_ms = snap.master_dt_ms, .avg_power_w = avg_p, .max_power_w = max_p,
                .energy_wh = avg_p * dt_h, .voltage_v = vch, .current_avg_a = i_a, .current_min_a = i_a, .current_max_a = i_a };
        }
    }
    /* current-only modules: the period's average current; the hub estimates the energy (voltage 0, energy 0) */
    {
        int64_t now_us = esp_timer_get_time();
        uint32_t win_ms = (uint32_t)((now_us - s_iacc_start_us) / 1000);
        rbn_iacc_t take[3][3];
        portENTER_CRITICAL(&s_iacc_mux);
        memcpy(take, s_iacc, sizeof(take)); memset(s_iacc, 0, sizeof(s_iacc)); s_iacc_start_us = now_us;
        portEXIT_CRITICAL(&s_iacc_mux);
        for (int i = 0; i < RBN_FLEET_N && rc < 8; i++) {
            if (!s_dev[i]) continue;
            uint8_t n = mod_chans(i);
            for (uint8_t ch = 0; ch < n && rc < 8; ch++) {
                const rbn_iacc_t *a = &take[i][ch];
                if (s_dev_vhw[i] && !no_v[i][ch]) continue;   /* recorded from the module's energy above */
                if (!node_chan_on(i, ch) || !chan_reported(i, ch) || a->n == 0) continue;
                float ia = (float)(a->sum / a->n);
                m->recs[rc++] = (rbn_period_rec_t){ .channel_id = node_chan_id(i, ch), .period_type = RBN_PERIOD_TYPE,
                    .period_ms = win_ms, .avg_power_w = 0, .max_power_w = 0, .energy_wh = 0, .voltage_v = 0,
                    .current_avg_a = ia, .current_min_a = a->min, .current_max_a = a->max };
            }
        }
    }
    if (rc == 0) return;
    uint32_t pseq = ++s_period_seq;
    rbn_hdr_init(&m->h, RBN_MSG_PERIOD, pseq, wsec);   /* node_ts = wall-clock of the boundary */
    m->rec_count = rc;
    size_t len = sizeof(rbn_period_t) + (size_t)rc * sizeof(rbn_period_rec_t);
    held_push(buf, len, pseq);
    const char *why = (reason == RBN_LATCH_REASON_ZONE) ? " [LATCH/zone]"
                    : (reason == RBN_LATCH_REASON_MANUAL) ? " [LATCH/manual]" : "";
    ESP_LOGW(TAG, "PERIOD seq=%u recs=%u latched%s — %u frame(s) held", (unsigned)pseq, rc, why, held_pending());
    held_flush();
}

static void period_task(void *arg)
{
    uint32_t last_boundary = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));   /* poll; latch only when a calendar boundary is crossed */
        /* C: hub-commanded LATCH_NOW — close the period immediately so it aligns with a tariff-zone boundary,
         * then rebase the calendar schedule to now (avoid a double-latch at the imminent grid boundary). */
        if (s_latch_now_req && s_ch_locked && s_fleet_present && s_wall_valid) {
            s_latch_now_req = false;
            uint32_t wsec = node_wall_sec();
            node_latch_and_send(wsec, s_latch_reason);
            last_boundary = wsec / RBN_BOUNDARY_S;
            continue;
        }
        {   /* retry frames the hub hasn't ACKed yet (hub restart / radio gap), every ~5 s */
            static uint32_t div = 0;
            if (held_pending() && s_ch_locked && ++div >= 5) { div = 0; held_flush(); }
        }
        /* No s_ch_locked here: a boundary passed while the hub is out of reach (radio off, out of range) is
         * latched on time and held until the hub ACKs it, so each 15-min period keeps its own record. */
        if (s_fleet_present == 0 || !s_wall_valid) continue;
        /* A: latch on the wall-clock calendar boundary (aligned across nodes + the hub), not a free interval. */
        uint32_t wsec = node_wall_sec();
        uint32_t boundary = wsec / RBN_BOUNDARY_S;
        if (boundary <= last_boundary) continue;          /* same window — wait */
        if (last_boundary == 0) {
            /* Clock just known, somewhere inside a 15-min window: nothing is latched now. The first record is latched at
             * the next boundary and covers everything since the node's start (or since the module's last latch when the
             * node restarted with the module powered), so no energy is lost and the record is stamped at a real boundary. */
            last_boundary = boundary;
            continue;
        }
        last_boundary = boundary;
        node_latch_and_send(boundary * RBN_BOUNDARY_S, 0xFF /* calendar: node_ts = the boundary itself */);
    }
}

/* Channel-follow: ESP-NOW shares ONE radio with WiFi, so the hub's channel = the client's router channel
 * (arbitrary, uncontrollable). The node has no WiFi association, so it scans channels for the hub's
 * SYNC_BEACON and locks onto whichever channel carries it; re-scans if the hub goes silent (channel change
 * or hub reboot onto a different channel). This is the production-correct answer to ESP-NOW↔WiFi coexistence. */
#define RBN_LOCK_LOSS_MS 8000
static void channel_follow_task(void *arg)
{
    static const uint8_t chans[] = {1,6,11,2,3,4,5,7,8,9,10,12,13};   /* common channels first */
    while (1) {
        if (s_ch_locked) {
            portENTER_CRITICAL(&s_mux);
            int64_t last = s_last_beacon_us;
            portEXIT_CRITICAL(&s_mux);
            if (esp_timer_get_time() - last > (int64_t)RBN_LOCK_LOSS_MS * 1000) {
                s_ch_locked = false;
                ESP_LOGW(TAG, "hub silent >%dms on ch%u → rescanning", RBN_LOCK_LOSS_MS, s_locked_ch);
            } else {
                /* re-pin to the hub's exact advertised channel if the radio drifted to an adjacent one */
                uint8_t hc = s_hub_channel, prim = 0; wifi_second_chan_t sec;
                if (hc && esp_wifi_get_channel(&prim, &sec) == ESP_OK && prim != hc)
                    esp_wifi_set_channel(hc, WIFI_SECOND_CHAN_NONE);
                vTaskDelay(pdMS_TO_TICKS(1000)); continue;
            }
        }
        for (size_t k = 0; k < sizeof(chans) && !s_ch_locked; k++) {
            esp_wifi_set_channel(chans[k], WIFI_SECOND_CHAN_NONE);
            int64_t t_set = esp_timer_get_time();
            portENTER_CRITICAL(&s_mux);
            int64_t before = s_last_beacon_us;
            portEXIT_CRITICAL(&s_mux);
            vTaskDelay(pdMS_TO_TICKS(400));            /* listen for a beacon on this channel */
            portENTER_CRITICAL(&s_mux);
            int64_t after = s_last_beacon_us;
            portEXIT_CRITICAL(&s_mux);
            /* require the beacon to have arrived >50ms AFTER the channel settled — rejects a straggler
             * delivered from the previous channel right after switching (caused a wrong-channel lock). */
            if (after > before && after > t_set + 50000) {
                s_ch_locked = true;   /* s_locked_ch comes from the beacon's rx_ctrl (ground truth) */
                ESP_LOGW(TAG, "LOCKED to hub (channel via rx_ctrl: %u)", s_locked_ch);
            }
        }
        if (!s_ch_locked) { ESP_LOGW(TAG, "no hub beacon found — rescanning"); vTaskDelay(pdMS_TO_TICKS(500)); }
    }
}

/* slave → master: send sync/link health over ESP-NOW so a node with NO COM (moved several meters
 * away) can still be read via the master's UART. Also reveals real loss at distance. */
static void stats_task(void *arg)
{
    uint32_t seq = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        rbn_node_stats_t m;
        rbn_hdr_init(&m.h, RBN_MSG_NODE_STATS, seq++, 0);
        portENTER_CRITICAL(&s_mux);
        m.beacons_recv = s_beacons; m.followups_recv = s_followups; m.max_beacon_id = s_max_beacon_id;
        m.fits = s_res_cnt; m.resid_mean_us = (float)s_res_mean; m.resid_maxabs_us = (float)s_res_maxabs;
        double var = (s_res_cnt > 1) ? s_res_m2 / (s_res_cnt - 1) : 0.0;
        m.skew_ppm = (float)s_skew_ppm;
        portEXIT_CRITICAL(&s_mux);
        m.resid_sd_us = (float)sqrt(var);
        if (s_ch_locked) node_send_hub(&m, sizeof(m));   /* B3b: unicast-encrypted */
    }
}

/* reporter — prints the measured sync precision every 2 s */
static void report_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        portENTER_CRITICAL(&s_mux);
        uint32_t bc = s_beacons, fu = s_followups, rc = s_res_cnt, er = s_epoch_resets, ep = s_epoch;
        double mean = s_res_mean, maxabs = s_res_maxabs, ppm = s_skew_ppm;
        double var = (rc > 1) ? s_res_m2 / (rc - 1) : 0.0;
        portEXIT_CRITICAL(&s_mux);
        double sd = sqrt(var);
        /* RBN_SYNC line is machine-parseable for standfw */
        ESP_LOGI(TAG, "RBN_SYNC ch=%u/%s%s wall=%u%s beacons=%u followups=%u fits=%u epoch=0x%08X relocks=%u | resid_us mean=%.1f sd=%.1f maxabs=%.1f | skew_ppm=%.2f",
                 (unsigned)s_locked_ch, s_ch_locked ? "locked" : "SCAN", s_paired ? " PAIRED" : "",
                 (unsigned)node_wall_sec(), s_wall_valid ? "" : "(unsynced)",
                 (unsigned)bc, (unsigned)fu, (unsigned)rc, (unsigned)ep, (unsigned)er, mean, sd, maxabs, ppm);
    }
}

/* B2 — pairing (slave): while unpaired + locked to a hub, broadcast PAIR_REQ (identity + per-channel ct_code)
 * every few seconds. The hub registers us only inside its open pairing window; PAIR_ACK(accepted) → s_paired,
 * we stop. Outside a window the PAIR_REQ is simply ignored (harmless) — data (realtime/period) flows regardless;
 * pairing only writes us into the hub's persistent node registry. */
/* F5: apply the hub's CONFIG — clamp model per channel into the module, the on/off mask into NVS — and answer
 * CONFIG_ACK with the period this build really uses (RBN_BOUNDARY_S). */
static void node_config_apply(void)
{
    const rbn_config_t *c = (const rbn_config_t *)s_cfg_buf;
    uint8_t result = 0;
    uint16_t mask = s_ch_off_mask;
    for (uint8_t i = 0; i < c->chan_count; i++) {
        const rbn_cfg_chan_t *k = &c->chans[i];
        int mod = k->channel_id / 4; uint8_t ch = k->channel_id % 4;
        if (mod >= RBN_FLEET_N || !s_dev[mod] || !chan_reported(mod, ch)) { result = 3; continue; }
        if (k->enabled) mask &= (uint16_t)~(1u << k->channel_id); else mask |= (uint16_t)(1u << k->channel_id);
        if (k->ct_code) {
            uint8_t cur = 0;
            if (xSemaphoreTake(s_rbamp_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                rbamp_read_ct_model_ch(s_dev[mod], ch, &cur);
                if (cur != k->ct_code && rbamp_set_ct_model_ch(s_dev[mod], ch, k->ct_code) != ESP_OK) result = 2;
                xSemaphoreGive(s_rbamp_lock);
            } else result = 2;
        }
    }
    if (c->period_s != RBN_BOUNDARY_S && result == 0) result = 1;   /* this build closes periods at RBN_BOUNDARY_S */
    s_ch_off_mask = mask;
    nvs_handle_t nh;
    if (nvs_open("espnow", NVS_READWRITE, &nh) == ESP_OK) { nvs_set_u16(nh, "choff", mask); nvs_commit(nh); nvs_close(nh); }
    rbn_config_ack_t a; memset(&a, 0, sizeof(a));
    rbn_hdr_init(&a.h, RBN_MSG_CONFIG_ACK, s_pair_seq++, node_wall_sec());
    a.cfg_seq = c->cfg_seq; a.result = result; a.period_s = RBN_BOUNDARY_S; a.fw_ver = 0x0105;
    node_send_hub(&a, sizeof(a));
    s_cfg_pending = false;
    ESP_LOGW(TAG, "CONFIG %u applied: result=%u off-mask=0x%04X", (unsigned)c->cfg_seq, result, mask);
}

static void pair_ack2_apply(void)
{
    uint8_t src[6]; memcpy(src, s_ack2_src, 6);
    rbn_pair_ack2_t a = s_ack2;
    s_ack2_pending = false;
    if (!a.accepted || !s_kx_ready) { ESP_LOGW(TAG, "PAIR_ACK2: rejected by the hub (registry full?)"); return; }
    uint8_t shared[32], pmk[16];
    bool ok = rbn_x25519_shared(s_kx_priv, a.hub_pub, shared) == 0 &&
              rbn_pmk_unwrap(shared, s_own_mac, a.h.seq, a.enc_pmk, a.tag, pmk) == 0;
    memset(shared, 0, sizeof(shared));
    if (!ok) { ESP_LOGE(TAG, "PAIR_ACK2: key did not verify — ignored"); return; }
    memcpy(s_pmk, pmk, 16);
    memset(pmk, 0, sizeof(pmk));
    nvs_handle_t nh;
    /* a new pairing starts from all channels on; a hub that holds settings for us sends them again (CONFIG) */
    s_ch_off_mask = 0;
    if (nvs_open("espnow", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_blob(nh, "pmk", s_pmk, 16); nvs_set_u16(nh, "choff", 0); nvs_set_blob(nh, "hubmac", src, 6);
        nvs_commit(nh); nvs_close(nh);
    }
    memcpy(s_paired_hub, src, 6); s_paired_hub_set = true; s_paired_hub_seen_us = esp_timer_get_time();
    s_repairing = false;
    memcpy(s_hub_mac, src, 6); s_hub_mac_valid = true;   /* data path to the hub that paired us, not the first beacon heard */
    esp_now_set_pmk(s_pmk);
    rbn_derive_lmk_from(s_pmk, s_own_mac, s_my_lmk);
    s_hub_enc_peer = false;              /* node_ensure_hub_peer re-keys the hub peer with the new LMK */
    s_kx_ready = false;
    memset(s_kx_priv, 0, sizeof(s_kx_priv));
    s_paired = true;
    s_last_enc_reply_us = esp_timer_get_time();
    ESP_LOGW(TAG, "PAIRED with hub %02X:%02X:%02X:%02X:%02X:%02X (installation key received and stored)",
             src[0], src[1], src[2], src[3], src[4], src[5]);
}

/* Pairing: while unpaired and locked to a hub, broadcast PAIR_REQ2 every 5 s with a fresh X25519 public key. The
 * hub answers only inside its pairing window (opened by the user); PAIR_ACK2 carries the installation key. */
static void pair_task(void *arg)
{
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_ack2_pending) pair_ack2_apply();
        if (s_cfg_pending) node_config_apply();
        if (++tick % 10) continue;   /* the rest every 5 s */
        /* Re-pair on stale link: no encrypted reply for >150 s while locked → the hub dropped us (removed) or uses
         * another key (a different hub) — pair again. */
        if (s_paired && s_ch_locked && s_last_enc_reply_us != 0 &&
            (esp_timer_get_time() - s_last_enc_reply_us) > 150LL * 1000000LL) {
            s_paired = false;
            s_hub_enc_peer = false;
            s_repairing = true;   /* another hub's beacon may now lock the channel if ours stays silent (moving the node) */
            ESP_LOGW(TAG, "no encrypted hub reply >150s → pairing again");
        }
        /* Our paired hub gone while we scan (it was switched off or replaced, and the node restarted): with no beacon
         * from it for >150 s, let another hub's beacon lock the channel and pair again. Without this, a node whose
         * hub disappeared ignored every other hub and scanned forever (re-pairing started only when locked). */
        if (!s_ch_locked && s_paired_hub_set && !s_repairing &&
            (esp_timer_get_time() - s_paired_hub_seen_us) > 150LL * 1000000LL) {
            s_paired = false;
            s_hub_enc_peer = false;
            s_repairing = true;
            ESP_LOGW(TAG, "paired hub silent >150s while scanning → another hub may take the node (pairing again)");
        }
        if (s_paired || !s_ch_locked) continue;
        uint8_t buf[sizeof(rbn_pair_req2_t) + RBN_FLEET_N * 4 * sizeof(rbn_pair_chan_t)];
        rbn_pair_req2_t *p = (rbn_pair_req2_t *)buf;
        uint8_t n = 0;
        for (int i = 0; i < RBN_FLEET_N; i++) {
            if (!s_dev[i]) continue;
            uint8_t nc = mod_chans(i);
            for (uint8_t ch = 0; ch < nc; ch++) {
                if (!chan_reported(i, ch)) continue;
                uint8_t code = 0;
                rbamp_read_ct_model_ch(s_dev[i], ch, &code);   /* 0 if module has none set → hub defaults */
                p->chans[n].channel_id = node_chan_id(i, ch);
                p->chans[n].ct_code    = code;
                n++;
            }
        }
        if (n == 0) continue;                          /* no fleet → nothing to register (synthetic-only) */
        if (rbn_x25519_keypair(s_kx_priv, p->node_pub) != 0) { ESP_LOGE(TAG, "X25519 keypair failed"); continue; }
        s_kx_ready = true;
        rbn_hdr_init(&p->h, RBN_MSG_PAIR_REQ2, s_pair_seq++, node_wall_sec());
        p->fw_ver = 0x0105;
        p->chan_count = n;
        esp_now_send(BCAST, buf, sizeof(rbn_pair_req2_t) + (size_t)n * sizeof(rbn_pair_chan_t));
        ESP_LOGI(TAG, "PAIR_REQ2 sent (%u ch) — waiting for the hub's pairing window", n);
    }
}
#endif /* !RBN_ROLE_MASTER */

#if (!RBN_ROLE_MASTER && RBN_OUTPUT_NODE)
/* ===== Output node (GPIO-relay reference OR DimmerLink dimmer, OPEN bring-up) ================
 * HELLO(family) discovery + SET_OUTPUT(0x40)→apply + OUTPUT_STATE(0x41) ACK + hub-silence failsafe.
 * OPEN (unencrypted) for first bring-up — matches ACRouter's open RX and needs no encrypted link-up. The
 * encrypted path (PMK/LMK, already provisioned via s_pmk) is a later hardening step.
 * RBN_OUT_IS_DIMMER selects the actuator: relay (GPIO, 0|1) or DimmerLink dimmer (I2C, 0..1000 per-mille). */
#if RBN_OUT_IS_DIMMER
#define RBN_OUT_KIND_THIS  RBN_OUT_KIND_DIMMER
#define RBN_OUT_MAX        1000              /* per-mille */
#define RBN_OUT_FAMILY     RBN_FAMILY_DIMMER
/* DimmerLink PY32 register map (I2C-slave @0x50, 100kHz) — per acrouter's driver recipe. */
#define DL_ADDR            0x50
#define DL_REG_LEVEL       0x10              /* W uint8 0..100 % */
#define DL_REG_CURVE       0x11              /* W 0=LIN 1=RMS 2=LOG — RMS for a heater */
#define DL_REG_FADE        0x18              /* W uint8 fade in 100ms units */
static i2c_master_dev_handle_t s_dl_dev = NULL;
static void dl_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_dl_dev) return;
    uint8_t b[2] = { reg, val };
    i2c_master_transmit(s_dl_dev, b, sizeof(b), 100);
}
#else
#define RBN_OUT_KIND_THIS  RBN_OUT_KIND_RELAY
#define RBN_OUT_MAX        1                 /* on/off */
#define RBN_OUT_FAMILY     RBN_FAMILY_RELAY
#endif

/* Drive the physical output. Relay: 0|1 on RBN_OUT_GPIO. Dimmer: value(per-mille)→percent I2C write to the
 * DimmerLink (fade first if ramp_ms). s_out_value = the value actually applied, back in wire units. */
static void output_drive(uint16_t value, uint16_t ramp_ms)
{
#if RBN_OUT_IS_DIMMER
    if (ramp_ms) { uint32_t f = ramp_ms / 100; if (f > 255) f = 255; dl_write_reg(DL_REG_FADE, (uint8_t)f); }
    uint8_t percent = (value >= 1000) ? 100 : (uint8_t)((value + 5) / 10);   /* per-mille → % (rounded) */
    dl_write_reg(DL_REG_LEVEL, percent);
    s_out_value = (uint16_t)percent * 10;    /* applied, back in per-mille */
#else
    (void)ramp_ms;
    s_out_value = value ? 1 : 0;
    gpio_set_level(RBN_OUT_GPIO, s_out_value);
#endif
}

/* Send OUTPUT_STATE (0x41) to `dst`. ack_seq = the acknowledged SET_OUTPUT seq (0 = unsolicited). OPEN. */
static void output_send_state(uint32_t ack_seq, uint8_t result, uint8_t flags, const uint8_t *dst)
{
    if (!dst) return;
    rbn_output_state_t s;
    rbn_hdr_init(&s.h, RBN_MSG_OUTPUT_STATE, 0, 0);
    s.output_id = RBN_OUT_ID; s.kind = RBN_OUT_KIND_THIS;
    s.value = s_out_value; s.ack_seq = ack_seq; s.flags = flags; s.result = result;
    esp_now_send(dst, (uint8_t *)&s, sizeof(s));
}

/* SET_OUTPUT (0x40) received (WiFi task — keep short): learn the hub, apply, ACK. */
static void output_on_set(const uint8_t *data, int len, const uint8_t *src)
{
    const rbn_set_output_t *c = (const rbn_set_output_t *)data;
    if (!s_out_hub_valid) {   /* learn the hub + add it as a (unencrypted) peer so we can reply */
        esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
        memcpy(p.peer_addr, src, 6);
        if (!esp_now_is_peer_exist(src)) esp_now_add_peer(&p);
    }
    memcpy(s_out_hub_mac, src, 6); s_out_hub_valid = true;
    s_out_last_cmd_us = esp_timer_get_time();          /* liveness → resets the failsafe watchdog */
    if (c->output_id != RBN_OUT_ID)          { output_send_state(c->h.seq, 2, 0, src); return; }  /* unknown id */
    if (c->kind != RBN_OUT_KIND_THIS)        { output_send_state(c->h.seq, 3, 0, src); return; }  /* kind mismatch */
    uint8_t result = 0; uint16_t v = c->value;
    if (v > RBN_OUT_MAX) { v = RBN_OUT_MAX; result = 1; }   /* clamp to this output's range */
    s_out_failsafe = false;                            /* a valid command clears failsafe */
    output_drive(v, c->ramp_ms);
    output_send_state(c->h.seq, result, 0, src);
    ESP_LOGI(TAG, "SET_OUTPUT id=%u val=%u ramp=%u → applied=%u (ack seq=%u res=%u)",
             c->output_id, c->value, c->ramp_ms, s_out_value, (unsigned)c->h.seq, result);
}

/* HELLO (0x01) broadcast-OPEN: identity + output capability → hub/registry classifies us. BURST at boot
 * (every RBN_HELLO_FAST_MS for RBN_HELLO_FAST_WINDOW_MS) so a hub that just (re)started discovers us in a
 * couple of seconds instead of waiting a full slow period (acrouter finding, 2026-07-14); then slow keep-alive. */
#define RBN_HELLO_FAST_MS         2500
#define RBN_HELLO_FAST_WINDOW_MS  30000
#define RBN_HELLO_SLOW_MS         30000
static void output_hello_task(void *arg)
{
    uint8_t buf[RBN_HELLO_LEN(1)];
    int64_t t0 = esp_timer_get_time();
    while (1) {
        rbn_hello_t *m = (rbn_hello_t *)buf;
        rbn_hdr_init(&m->h, RBN_MSG_HELLO, 0, 0);
        m->fw_ver = 0x0105; m->hw_model = 0x0001;
        m->family = RBN_OUT_FAMILY;
        m->flags = RBN_HELLO_F_MAINS_POWERED | RBN_HELLO_F_HAS_OUTPUTS;
        m->channel_count = 0; m->out_count = 1;
        m->out_cap[0] = (rbn_out_cap_t){ .output_id = RBN_OUT_ID, .kind = RBN_OUT_KIND_THIS,
                                         .range_min = 0, .range_max = RBN_OUT_MAX, .failsafe_value = 0 };
        esp_now_send(BCAST, buf, RBN_HELLO_LEN(1));
        bool fast = (esp_timer_get_time() - t0) < (int64_t)RBN_HELLO_FAST_WINDOW_MS * 1000;
        vTaskDelay(pdMS_TO_TICKS(fast ? RBN_HELLO_FAST_MS : RBN_HELLO_SLOW_MS));
    }
}

/* Failsafe watchdog: no SET_OUTPUT for RBN_OUTPUT_FAILSAFE_MS → drive the output to its safe value (OFF/0)
 * + emit an unsolicited OUTPUT_STATE(failsafe_active). Recovery on the next valid SET_OUTPUT. */
static void output_failsafe_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_out_failsafe || s_out_last_cmd_us == 0) continue;   /* nothing to fail-safe from yet */
        if (esp_timer_get_time() - s_out_last_cmd_us > (int64_t)RBN_OUTPUT_FAILSAFE_MS * 1000) {
            s_out_failsafe = true;
            output_drive(0, 0);   /* → OFF/0 (safe default) */
            ESP_LOGW(TAG, "FAILSAFE: no SET_OUTPUT >%dms → output OFF", RBN_OUTPUT_FAILSAFE_MS);
            if (s_out_hub_valid) output_send_state(0, 0, RBN_OUTSTATE_F_FAILSAFE_ACTIVE, s_out_hub_mac);
        }
    }
}

static void output_node_init(void)
{
#if RBN_OUT_IS_DIMMER
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = RBN_I2C_SDA, .scl_io_num = RBN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) == ESP_OK) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = DL_ADDR,
                                   .scl_speed_hz = 100000 };
        if (i2c_master_bus_add_device(bus, &dc, &s_dl_dev) == ESP_OK) {
            dl_write_reg(DL_REG_CURVE, 1);   /* RMS curve — linear-in-power for a heater */
            ESP_LOGI(TAG, "DimmerLink @0x%02X up (SDA%d/SCL%d, RMS curve)", DL_ADDR, RBN_I2C_SDA, RBN_I2C_SCL);
        } else ESP_LOGW(TAG, "DimmerLink add_device failed @0x%02X", DL_ADDR);
    } else ESP_LOGW(TAG, "DimmerLink I2C bus init failed (SDA%d/SCL%d)", RBN_I2C_SDA, RBN_I2C_SCL);
#else
    gpio_config_t io = { .pin_bit_mask = 1ULL << RBN_OUT_GPIO, .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&io);
#endif
    output_drive(0, 0);                    /* boot = safe/OFF */
    s_out_failsafe = false; s_out_last_cmd_us = 0;
}
#endif /* output node */

/* Load the ESP-NOW PMK: a user-configured value from NVS ("espnow"/"pmk", 16 B) if present, else the
 * compile-time dev default (bring-up). Open-source requirement (ACRouter): the key is user-settable, NOT
 * hardcoded — a provisioning tool / future UI writes the NVS blob; hub + node must hold the SAME PMK. */
static void load_ch_mask(void)
{
    nvs_handle_t nh;
    if (nvs_open("espnow", NVS_READONLY, &nh) == ESP_OK) { nvs_get_u16(nh, "choff", &s_ch_off_mask); nvs_close(nh); }
    if (s_ch_off_mask) ESP_LOGW(TAG, "channels switched off by the hub: mask 0x%04X", s_ch_off_mask);
}

static void load_pmk(void)
{
    static const uint8_t dev_pmk[16] = RBN_PMK16;
    memcpy(s_pmk, dev_pmk, 16);
    nvs_handle_t nh;
    if (nvs_open("espnow", NVS_READONLY, &nh) == ESP_OK) {
        size_t sz = 16;
        esp_err_t r = nvs_get_blob(nh, "pmk", s_pmk, &sz);
        size_t hz = 6;
        if (nvs_get_blob(nh, "hubmac", s_paired_hub, &hz) == ESP_OK && hz == 6) {
            s_paired_hub_set = true;
            memcpy(s_hub_mac, s_paired_hub, 6); s_hub_mac_valid = true;
            ESP_LOGI(TAG, "paired hub %02X:%02X:%02X:%02X:%02X:%02X (other hubs' beacons ignored)", s_paired_hub[0],
                     s_paired_hub[1], s_paired_hub[2], s_paired_hub[3], s_paired_hub[4], s_paired_hub[5]);
        }
        nvs_close(nh);
        if (r == ESP_OK && sz == 16) { ESP_LOGI(TAG, "PMK: user-configured (NVS espnow/pmk)"); return; }
    }
    ESP_LOGW(TAG, "PMK: compile-time DEV default (set NVS espnow/pmk for production)");
}

/* Resolve the WiFi/ESP-NOW channel: NVS ("espnow"/"channel", uint8 1..13) if set, else the compile-time
 * TEST_CHANNEL default. ESP-NOW needs hub channel == node channel; the hub's STA follows the router channel,
 * so the node must be set to it. NVS-configurable → change without a rebuild (survives a router roam). */
static void load_channel(void)
{
    s_cfg_channel = TEST_CHANNEL;
    nvs_handle_t nh;
    if (nvs_open("espnow", NVS_READONLY, &nh) == ESP_OK) {
        uint8_t ch = 0;
        esp_err_t r = nvs_get_u8(nh, "channel", &ch);
        nvs_close(nh);
        if (r == ESP_OK && ch >= 1 && ch <= 13) { s_cfg_channel = ch; ESP_LOGI(TAG, "channel: %u (NVS espnow/channel)", ch); return; }
    }
    ESP_LOGI(TAG, "channel: %u (compile default; set NVS espnow/channel to override)", s_cfg_channel);
}

static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_cfg_channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    /* B3: provision the Primary Master Key (shared hub+node) — runtime s_pmk (NVS-configurable, dev fallback).
     * Enables CCMP on the unicast data path (B3b); harmless to set even where only OPEN frames are used. */
    ESP_ERROR_CHECK(esp_now_set_pmk(s_pmk));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
#if RBN_ROLE_MASTER
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
#endif
    /* channel = 0 → send on whatever channel the interface is currently on (channel-follow picks it). */
    esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(peer.peer_addr, BCAST, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    load_channel();                        /* runtime WiFi/ESP-NOW channel (NVS-configurable) — before wifi init */
    ESP_LOGI(TAG, "RBgrid Node sync-prototype boot. MAC=%02X:%02X:%02X:%02X:%02X:%02X ch=%d",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], s_cfg_channel);
    memcpy(s_own_mac, mac, 6);
    load_pmk();                                /* runtime PMK (NVS-configurable) — before set_pmk + LMK derive */
    load_ch_mask();                            /* F5: channels the hub switched off */
    rbn_derive_lmk_from(s_pmk, s_own_mac, s_my_lmk);   /* B3b: our link key = HMAC(PMK, own_mac); hub derives the same */
    wifi_espnow_init();
#if RBN_ROLE_MASTER
    s_master_epoch = esp_random();   /* boot-unique → nodes detect a master restart and re-lock cleanly */
    xTaskCreate(beacon_task,        "rbn_beacon", 4096, NULL, 5, NULL);
    xTaskCreate(master_report_task, "rbn_mrep",   4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "MASTER up: epoch=0x%08X, SYNC_BEACON+FOLLOWUP every %d ms, recv REALTIME/STATS",
             (unsigned)s_master_epoch, BEACON_MS);
#elif RBN_OUTPUT_NODE
    /* Wireless output node (GPIO-relay reference, OPEN): fixed ch (set in wifi_espnow_init), broadcast HELLO
     * discovery + SET_OUTPUT→GPIO + OUTPUT_STATE ACK + hub-silence failsafe. No rbAmp / sensor / PTP tasks. */
    output_node_init();
    xTaskCreate(output_hello_task,    "rbn_hello", 3072, NULL, 3, NULL);
    xTaskCreate(output_failsafe_task, "rbn_fsafe", 3072, NULL, 4, NULL);
#if RBN_OUT_IS_DIMMER
    ESP_LOGW(TAG, "NODE OUTPUT (DimmerLink dimmer @0x50 SDA%d/SCL%d): ch%d, OPEN — HELLO+SET_OUTPUT+failsafe(%dms)",
             RBN_I2C_SDA, RBN_I2C_SCL, s_cfg_channel, RBN_OUTPUT_FAILSAFE_MS);
#else
    ESP_LOGW(TAG, "NODE OUTPUT (relay): GPIO%d, ch%d, OPEN — HELLO+SET_OUTPUT+failsafe(%dms)",
             RBN_OUT_GPIO, s_cfg_channel, RBN_OUTPUT_FAILSAFE_MS);
#endif
    (void)rbamp_init; (void)realtime_task; (void)channel_follow_task; (void)report_task;
    (void)time_task; (void)period_task; (void)stats_task; (void)pair_task;   /* not used in output build */
#else
    s_iacc_start_us = esp_timer_get_time();   /* current-only averages start at boot */
    hold_init();    /* finished periods still unACKed from before this boot (rbnhold partition) */
    rbamp_init();   /* real I3 source if present, else synthetic */
#if RBN_FREERUN
    /* ACRouter / beacon-less OPEN receiver: radio stays pinned to the fixed TEST_CHANNEL set in
     * wifi_espnow_init (no association drags it, no channel-follow moves it). Start ONLY the RT transmitter
     * (broadcast OPEN) + the diagnostic reporter. NONE of the hub-dependent tasks run → no perpetual rescan,
     * no PTP/period/pairing/encryption → rbn_rt no longer starves its core (the task-WDT standfw saw). */
    s_ch_locked = true; s_locked_ch = s_cfg_channel; s_hub_channel = s_cfg_channel;   /* cosmetic (report line) */
    xTaskCreate(realtime_task, "rbn_rt",     4096, NULL, 3, NULL);
    xTaskCreate(report_task,   "rbn_report", 4096, NULL, 4, NULL);
    ESP_LOGW(TAG, "NODE FREE-RUN: broadcast OPEN REALTIME on fixed ch%d @1Hz, no hub-beacon/PTP/pairing (ACRouter mode)",
             s_cfg_channel);
    (void)channel_follow_task; (void)time_task; (void)period_task; (void)stats_task; (void)pair_task;  /* not used in free-run */
#else
    xTaskCreate(channel_follow_task, "rbn_chfollow", 4096, NULL, 6, NULL);  /* find + lock the hub's channel */
    xTaskCreate(report_task,   "rbn_report", 4096, NULL, 4, NULL);
    xTaskCreate(realtime_task, "rbn_rt",     4096, NULL, 3, NULL);
    xTaskCreate(time_task,     "rbn_time",   4096, NULL, 4, NULL);   /* A: wall-clock TIME-sync with the hub */
    xTaskCreate(period_task,   "rbn_period", 4096, NULL, 3, NULL);   /* P3: on-node period latch → PERIOD (held-until-ACK) */
    xTaskCreate(stats_task,    "rbn_stats",  4096, NULL, 4, NULL);   /* health upstream → master (no-COM read) */
    xTaskCreate(pair_task,     "rbn_pair",   4096, NULL, 2, NULL);   /* B2: OTA pairing → hub node registry */
    ESP_LOGI(TAG, "NODE up: channel-follow scan → disciplining to hub beacon, reading rbAmp I3, REALTIME + NODE_STATS");
#endif
#endif
}
