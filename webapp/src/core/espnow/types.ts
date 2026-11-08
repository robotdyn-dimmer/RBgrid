/**
 * ESP-NOW topology types — the hub's wireless remote metering nodes. Shape confirmed by
 * root 2026-06-23 (GET /api/v1/espnow, BARE). The hub is the Thing; each node is a
 * group member; a node's channels are its metering units.
 * fw 1.1.7 contract: hub repo documents/api/ESPNOW_API.md (root espnow-nodes-ui). Fields marked
 * planned there are optional here and shown only when a hub sends them.
 */

export interface EspnowNodeSync {
  resid_sd_us: number;
  skew_ppm: number;
  /** Lost beacons over the last ≥ 60 s (fw e163afd). Absent until the first window completes. */
  loss_pct?: number;
  fits: number;
}

export interface EspnowNodeChannel {
  ch: number;
  ct_code: number;
  /** Bound unit id, e.g. "0x6A00". */
  unit_id: string;
  present: boolean;
  /** planned (F3/F5): the channel measures; absent = on. */
  enabled?: boolean;
}

/** planned (F3/F5) `configure` body: the node takes it on its next contact and confirms. Channels
 *  only — the period is fixed at 900 s (the hub rolls node periods up as 15-min quarters; root
 *  espnow-test-scenarios Q1). ct_code is `enums.ct_model` (Q2). */
export interface EspnowNodeConfig {
  channels: { ch: number; enabled: boolean; ct_code: number }[];
}

export interface EspnowNode {
  /** "AA:BB:CC:DD:EE:FF". */
  mac: string;
  online: boolean;
  disabled: boolean;
  /** Seconds since last seen; -1 or absent = not heard since the hub started. */
  last_seen_s?: number;
  /** Only for a node heard since the hub started (root espnow-panel-bench, 07.11). */
  sync?: EspnowNodeSync;
  channels?: EspnowNodeChannel[];
  // planned (F3/F5) — shown when present
  fw?: string;
  rssi_dbm?: number;
  held_frames?: number;
  period_s?: number;
  /** none = never configured (ESPNOW_API.md aeacb18). */
  config_state?: 'none' | 'applied' | 'pending' | 'failed';
  /** The channel settings the hub holds for the node (from its CONFIG_ACK); preferred over channels[]. */
  config?: { ch: number; enabled?: boolean; ct_code?: number }[];
}

/** auto = the radio starts by itself once a node is registered (fw 1.1.7). */
export type EspnowEnabled = 'auto' | 'on' | 'off';

/** GET /api/v1/espnow — hub status + nodes. */
export interface EspnowStatus {
  running: boolean;
  channel: number;
  pairing_open: boolean;
  paired_count: number;
  // fw 1.1.7 — absent on older hubs
  enabled?: EspnowEnabled;
  /** false: this image can't run ESP-NOW. Absent: a hub before 1.1.7. */
  keys?: boolean;
  max_nodes?: number;
  max_channels_per_node?: number;
  pairing_left_s?: number;
  periods_recv?: number;
  periods_injected?: number;
  periods_dropped?: number;
  periods_dup?: number;
  beacons_sent: number;
  rt_recv: number;
  stats_recv: number;
  nodes: EspnowNode[];
}

export type EspnowAction = 'pair' | 'pair_cancel' | 'hub_on' | 'hub_off' | 'add' | 'remove' | 'disable' | 'enable' | 'latch' | 'configure';

export interface EspnowActionParams {
  mac?: string;
  /** pair: how long to keep the pairing window open. */
  seconds?: number;
  /** add: channel definitions. */
  channels?: { ch: number; ct_code: number }[];
  /** configure (planned F3/F5). */
  config?: EspnowNodeConfig;
}


/** A restart is needed only where the reply says `reboot_to_register:true` (fw 1.1.7 F2: pairing and
 *  `add` register a node's points at once, with history). */

/** Error codes of `{ok:false, error, message}` (HTTP 400/409/503, or the MQTT ack's reply). */
export type EspnowErrorCode = 'BAD_REQUEST' | 'NOT_FOUND' | 'PERIOD_UNSUPPORTED' | 'ESPNOW_DISABLED' | 'ESPNOW_OFF' | 'ESPNOW_START_FAILED' | 'REGISTRY';

export interface EspnowActionResult {
  ok: boolean;
  error?: EspnowErrorCode | string;
  message?: string;
  running?: boolean;
  enabled?: EspnowEnabled | boolean;
  pairing_open_s?: number;
  paired?: number;
  config_state?: 'none' | 'applied' | 'pending' | 'failed';
  cfg_seq?: number;
  removed_units?: number;
  latched?: boolean;
  reboot_to_register?: boolean;
}
