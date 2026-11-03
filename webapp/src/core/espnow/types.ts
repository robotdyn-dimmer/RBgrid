/**
 * ESP-NOW topology types — the hub's wireless remote metering nodes. Shape confirmed by
 * root 2026-06-23 (GET /api/v1/espnow, BARE). The hub is the Thing; each node is a
 * group member; a node's channels are its metering units.
 */

export interface EspnowNodeSync {
  resid_sd_us: number;
  skew_ppm: number;
  loss_pct: number;
  fits: number;
}

export interface EspnowNodeChannel {
  ch: number;
  ct_code: number;
  /** Bound unit id, e.g. "0x6A00". */
  unit_id: string;
  present: boolean;
}

export interface EspnowNode {
  /** "AA:BB:CC:DD:EE:FF". */
  mac: string;
  online: boolean;
  disabled: boolean;
  /** Seconds since last seen; -1 = never. */
  last_seen_s: number;
  sync: EspnowNodeSync;
  channels: EspnowNodeChannel[];
}

/** GET /api/v1/espnow — hub status + nodes. */
export interface EspnowStatus {
  running: boolean;
  channel: number;
  pairing_open: boolean;
  paired_count: number;
  beacons_sent: number;
  rt_recv: number;
  stats_recv: number;
  nodes: EspnowNode[];
}

export type EspnowAction = 'pair' | 'add' | 'remove' | 'disable' | 'enable' | 'latch';

export interface EspnowActionParams {
  mac?: string;
  /** pair: how long to keep the pairing window open. */
  seconds?: number;
  /** add: channel definitions. */
  channels?: { ch: number; ct_code: number }[];
}

/** add + pair register node units on boot → require a reboot to converge. */
export const ESPNOW_REBOOT_ACTIONS: ReadonlySet<EspnowAction> = new Set<EspnowAction>(['pair', 'add']);

export interface EspnowActionResult {
  ok: boolean;
  message?: string;
  removed_units?: number;
  latched?: boolean;
  reboot_to_register?: boolean;
}
