/**
 * Phase 4 config-lifecycle types — sensor discovery + unit deletion.
 * Mirror of firmware GET /api/v1/sensors and DELETE /api/v1/units/{hexid}.
 * FINAL contract, HW-validated by the firmware team 2026-06-19.
 * See memory `devices-sensor-lifecycle-contract`.
 */

import { BaseResponse } from './api';
import { UnitRoleName, StatClassName } from '../constants/enums';

/** Sensor interface: rbAmp modules on I2C (the product has no built-in ADC — operator 18.10). */
export type SensorIface = 'i2c';

/**
 * One sensor in GET /api/v1/sensors (variant (a): bound sensors only).
 * Addressing fields are per-iface. `unit_id` is currently always set (bound);
 * `null` is reserved for variant (b) (unbound sensor layer — not yet shipped).
 */
export interface SensorListEntry {
  iface: SensorIface;
  /** Hardware presence (false = configured but module absent / dormant). */
  present: boolean;
  /** Bound unit id (hex string). null reserved for variant (b) unbound sensors. */
  unit_id: string | null;

  // I2C / rbAmp addressing
  /** I2C bus index (i2c) */
  bus?: number;
  /** rbAmp module index (i2c) */
  module?: number;
  /** Channel within the module (i2c) */
  ch?: number;
  /** The jack as printed on the module (root 16.10): «I0» current channel, «L1»…«L3» phase, «V» voltage, «L1–L3» grouped input. */
  ch_label?: string;
  /** CT code (i2c) */
  ct_code?: number;
  /** The module line of this channel (root d03f19ed): "basic" | "std". */
  line?: string;
  /** CT model name, rating, clipping start on a sine, peak (root d03f19ed). */
  ct_name?: string;
  ct_rated_a?: number;
  ct_fs_a?: number;
  ct_peak_a?: number;
  /** SCT correction of the channel (rbamp 1.7.2, root 20.10): `corrected` = normal; the others mean
   *  the readings are NOT corrected for the CT. */
  sct_correction?: SctCorrection | string;
  /** The applied correction scale (details only). */
  sct_scale?: number;
  /** 7-bit I2C module address, e.g. 82 (0x52) — display/disambiguation only (i2c). */
  addr?: number;
  /** Field class = schema.fields_by_source key (rbamp | espnow). Additive,
   *  next firmware canon (root 2026-10-12); used for dash-not-0.00 gating of this sensor. */
  src_class?: string;
}

/**
 * One rbAmp module as identified by the hub's bridge at probe (root delta №5, next canon).
 * STD line (e.g. STD-U3I3: 3 voltage + 3 current channels) carries map/period; BASIC
 * (BASIC-UI1 / BASIC-I2) omits them.
 */
export interface SensorModule {
  module: number;
  /** 7-bit I2C address (80 = 0x50). */
  addr?: number;
  present: boolean;
  /** Product line: "std" | "basic". */
  line?: string;
  /** Model name, e.g. "STD-U3I3", "BASIC-UI1", "BASIC-I2". */
  model?: string;
  variant?: number;
  /** Voltage / current channel counts. */
  n_v?: number;
  n_i?: number;
  map?: number;
  /** Energy period source, e.g. "counters" (STD only). */
  period?: string;
  /** Calibration marker: "ok" | "not_asserted" (NOT an error — unconfirmed) | "unknown". */
  cal?: string;
  /** STD: the image version (0x0001007B = 65659, root 534ded9e); BASIC: a byte. */
  fw?: number;
  /** STD modules updatable through the hub (root 534ded9e). */
  ota?: ModuleOta;
  /** BASIC sensor class (62f1c9a2): 1 = SCT-013 (user picks per channel), 2/3 = CT- (factory-paired). */
  sensor_class?: number;
  /** The channels' CT model can be set (SCT-); false = fixed at the factory (CT-). */
  ct_settable?: boolean;
  /** sensor_class 0 (root d03f19ed). */
  uncalibrated?: boolean;
  /** Readable version (afb58e03): BASIC «1.4.19» / ranges for old bytes / «0x..»; STD «0x0001007B». */
  fw_name?: string;
}

export interface ModuleOta {
  /** Bytes; an image must be exactly this size. */
  bank_size: number;
  /** Flash address of the running bank, hex («0x08030000»). */
  active_bank: string;
  /** The bank an image goes to (root 9bc08b40). */
  inactive_bank?: string;
  /** Both banks, ascending. */
  banks?: string[];
  /** Board word of the running image («0x42440211») — compared with the image's .manifest. */
  board?: string;
}

/** GET /api/v1/modules/<m>/firmware (root 534ded9e). */
export type ModuleFwState = 'idle' | 'writing' | 'committing' | 'activating' | 'done' | 'error';
export interface ModuleFwProgress {
  module: number;
  state: ModuleFwState;
  /** Pages written / total (256 B each). */
  done?: number;
  total?: number;
  elapsed_s?: number;
  fw_before?: number;
  fw_after?: number;
  /** A word or a code — .139 answers `reason: 0` when idle. */
  reason?: string | number;
  message?: string;
}
/** POST result: 202 started, or a refusal with its reason (409 busy, 400/404 REFUSED). */
export interface ModuleFwStart { status: 'started' | 'error'; message?: string; reason?: string; http?: number }

/** data payload of GET /api/v1/sensors (BaseResponse envelope, type="sensors_list"). */
export interface SensorsListData {
  sensors: SensorListEntry[];
  /** rbAmp modules (delta №5, next canon) — absent on older firmware. */
  modules?: SensorModule[];
  /** v1.1.2: seconds the hub has polled no module (present only when ≥ 30 s) — a stuck I2C bus. */
  bus_stalled_s?: number;
  /** v1.1.2: seconds a bus line has been held low (≥ 15 s, every module reads absent); clears by itself. */
  bus_line_low_s?: number;
}

export type SensorsListResponse = BaseResponse<SensorsListData>;

/**
 * Flat result of DELETE /api/v1/units/{hexid} (NOT a BaseResponse).
 * 200 → {status:"ok", removed, persist}; 4xx → {status:"error", message}.
 * 404 = not found (also returned on idempotent repeat-delete); 409 = has children
 * (units are flat for now, this is defensive); 400 = invalid hex id.
 */
export interface UnitDeleteResult {
  status: 'ok' | 'error';
  /** Removed unit id (on success) */
  removed?: string;
  /** Persistence result, e.g. "ESP_OK" (on success) */
  persist?: string;
  /** Point slug, phase and channel index of the new unit (delta №5: ch 254 → phase 4). */
  point?: string;
  phase?: number;
  channel_idx?: number;
  /** Error description (on 4xx) */
  message?: string;
}

/**
 * Sensor reference for a bind request — identifies the sensor by its addressing
 * (same fields as the sensor-list entry), NOT a separate id. unit_id is firmware-derived.
 */
export interface CreateUnitSensorRef {
  iface: SensorIface;
  // i2c addressing:
  bus?: number;
  module?: number;
  ch?: number;
  ch_label?: string;
}

/**
 * Body of POST /api/v1/units — create/re-bind a unit from an (unbound) sensor.
 * Inverse of DELETE. unit_id is NOT sent (firmware derives it from the addressing).
 */
export interface CreateUnitRequest {
  sensor: CreateUnitSensorRef;
  /** Unit display name (required). */
  name: string;
  /** Unit role; firmware-exact string. Defaults to DEVICE_LOAD if omitted. */
  role?: UnitRoleName;
  /** Persisted stat-class period; defaults to 1HOUR if omitted. */
  stat_class?: StatClassName;
  /** Physical location (optional). */
  location?: string;
  /** Explicit point slug `[a-z0-9_.-]{1,32}` (T7); omitted → the hub derives it from the name. */
  point?: string;
  /** Revive a removed (tombstoned) slug — the point continues its history (T7). */
  reuse?: boolean;
}

/**
 * Flat result of POST /api/v1/units (NOT a BaseResponse).
 * 201 → {status:"ok", unit_id, persist}; 4xx → {status:"error", message}.
 * 400 = bad body / sensor-not-found-or-unsupported-iface; 409 = already bound.
 * 507 ("unit network full") is a server error — handle as unexpected (re-thrown by the client).
 */
export interface UnitCreateResult {
  status: 'ok' | 'error';
  /** Assigned unit id, hex (on success) */
  unit_id?: string;
  /** Persistence result, e.g. "ESP_OK" (on success) */
  persist?: string;
  /** Error description (on 4xx) */
  message?: string;
  /** 409 code (T7): POINT_TAKEN · POINT_TOMBSTONED · SENSOR_BOUND. */
  code?: string;
  /** The bound point's slug (201, T7). */
  point?: string;
}

/** POST /api/v1/units/<input>/rebind {module, ch:254} (v1.1.2): the input regrouped on a U3I3. */
export interface UnitRebindResult {
  status: 'ok' | 'error';
  unit_id?: string;
  grouped?: boolean;
  /** Always true on 200 — the layout applies after a restart (also on an idempotent repeat). */
  requires_reboot?: boolean;
  /** 409 SENSOR_BOUND · 400 NOT_PHASE_MODULE */
  code?: string;
  message?: string;
}

export type SctCorrection = 'corrected' | 'uncalibrated' | 'gain_unexpected' | 'unknown_model';

/** The channel's readings are not corrected for its CT: a status the hub gives other than `corrected`. */
export const sctUncorrected = (s: Pick<SensorListEntry, 'sct_correction'> | null | undefined): boolean =>
  typeof s?.sct_correction === 'string' && s.sct_correction !== 'corrected';

/** The hardware channel of a sensor entry (bus · module/addr · ch). */
const chanKey = (s: SensorListEntry): string => `${s.iface ?? ''}|${s.bus ?? ''}|${s.addr ?? s.module ?? ''}|${s.ch ?? ''}`;

/**
 * Free sensors — minus any whose channel a bound point already uses. A fresh hub (.191, 17.10)
 * listed I0 of module 0 both as point 0006 and as a free sensor: «bind» there would double the input.
 * Also minus current channels the fitted module doesn't have (`ch ≥ n_i`): a UI3 in place of a
 * UI7 (.139, 18.10) leaves I3…I6 in the list, `present:false` — nothing to bind there.
 */
export function freeSensors(sensors: SensorListEntry[], modules: SensorModule[] = []): SensorListEntry[] {
  const taken = new Set(sensors.filter((s) => s.unit_id != null).map(chanKey));
  return sensors.filter((s) => s.unit_id == null && !taken.has(chanKey(s)) && !channelMissing(s, modules));
}

/** A current channel the fitted module doesn't have (`ch ≥ n_i` of a present module). */
export function channelMissing(s: SensorListEntry, modules: SensorModule[]): boolean {
  const m = modules.find((x) => x.module === s.module);
  // ch 254 = the module's phase group, 255 = its voltage input — not current channels.
  return !!m && m.present !== false && m.n_i != null && s.ch != null && s.ch < 254 && s.ch >= m.n_i;
}

/** Points bound to such a channel (upper-case unit keys): kept, history and all, but nothing measures them. */
export function noChannelUnitKeys(sensors: SensorListEntry[], modules: SensorModule[]): Set<string> {
  return new Set(sensors.filter((s) => s.unit_id && channelMissing(s, modules)).map((s) => String(s.unit_id).toUpperCase()));
}
