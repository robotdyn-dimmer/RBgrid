/**
 * RBGrid entity model — webapp internal normalization of live telemetry into the
 * CANONICAL HA-style entity model shared with the Home Assistant integration.
 *
 * Source of truth: the entity model of the RBgrid Home Assistant integration
 * (one model so native UI ↔ HA Energy Dashboard ↔ HA history describe the same objects).
 *
 * Each metric → one entity (entity_id `sensor.rbgrid_<unit>_<metric>`, numeric/enum state,
 * device_class, state_class, unit). Non-state fields (src/bus/module/ch/energy_mode) are
 * attributes. Two device tiers: a system device + per-unit devices (via_device the system).
 */

import {
  TelemetryFrame, TelemetryPhase, PhaseSeq, TelemetryUnit, TelemetryUnitSrc, TelemetrySystemTotals, resolveSrcClass,
} from '../types/telemetry-units';

export type DeviceClass =
  | 'voltage' | 'current' | 'power' | 'reactive_power' | 'power_factor'
  | 'energy' | 'frequency' | 'enum';

export type StateClass = 'measurement' | 'total' | 'total_increasing';

export type MetricKey =
  | 'voltage' | 'current' | 'power' | 'reactive_power' | 'power_factor'
  | 'energy_in' | 'energy_out' | 'energy_day_in' | 'energy_day_out' | 'direction';

/** Descriptor for a per-unit metric → entity mapping (the ENTITY_MODEL.md table). */
export interface MetricDescriptor {
  key: MetricKey;
  /** object_id suffix → `sensor.rbgrid_<unit>_<suffix>` */
  suffix: string;
  label: string;
  /** Display unit (V/A/W/var/Wh) or null (power_factor / enum). */
  unit: string | null;
  device_class: DeviceClass;
  state_class: StateClass | null;
  /** Pull the raw numeric value from a unit's telemetry (undefined → unavailable). */
  read: (u: TelemetryUnit) => number | undefined;
}

/** The numeric per-unit metrics. `direction` is handled separately (enum). */
export const UNIT_METRICS: MetricDescriptor[] = [
  { key: 'voltage',        suffix: 'voltage',        label: 'Voltage',        unit: 'V',   device_class: 'voltage',        state_class: 'measurement',      read: (u) => u.v },
  { key: 'current',        suffix: 'current',        label: 'Current',        unit: 'A',   device_class: 'current',        state_class: 'measurement',      read: (u) => u.i },
  { key: 'power',          suffix: 'power',          label: 'Power',          unit: 'W',   device_class: 'power',          state_class: 'measurement',      read: (u) => u.p_act },
  { key: 'reactive_power', suffix: 'reactive_power', label: 'Reactive power', unit: 'var', device_class: 'reactive_power', state_class: 'measurement',      read: (u) => u.p_rea },
  { key: 'power_factor',   suffix: 'power_factor',   label: 'Power factor',   unit: null,  device_class: 'power_factor',   state_class: 'measurement',      read: (u) => u.pf },
  { key: 'energy_in',      suffix: 'energy_in',      label: 'Energy in',      unit: 'Wh',  device_class: 'energy',         state_class: 'total_increasing', read: (u) => u.e_in },
  { key: 'energy_out',     suffix: 'energy_out',     label: 'Energy out',     unit: 'Wh',  device_class: 'energy',         state_class: 'total_increasing', read: (u) => u.e_out },
  { key: 'energy_day_in',  suffix: 'energy_day_in',  label: 'Energy in (day)',  unit: 'Wh', device_class: 'energy',       state_class: 'total',            read: (u) => u.e_day_in },
  { key: 'energy_day_out', suffix: 'energy_day_out', label: 'Energy out (day)', unit: 'Wh', device_class: 'energy',       state_class: 'total',            read: (u) => u.e_day_out },
];

/** A single normalized entity (mirrors an HA state object). */
export interface EntityState {
  entity_id: string;
  label: string;
  state: number | string | null;
  unit: string | null;
  device_class: DeviceClass;
  state_class: StateClass | null;
  available: boolean;
  attributes: Record<string, unknown>;
}

/** A per-unit device group (entities + the unit's identity/attributes). */
export interface UnitEntityGroup {
  /** Telemetry key: hex unit id, or special "v_bus" / "main" / "alt". */
  unitKey: string;
  /** HA device id: `<systemBase>_<unitKey>`. */
  deviceId: string;
  name: string;
  present: boolean;
  /** Bound to a channel the fitted module doesn't have (a UI3 in place of a UI7): not «нет связи»,
   *  nothing to measure — kept out of the flow and the «не отвечают» count (set by useTelemetryEntities). */
  noChannel?: boolean;
  src?: TelemetryUnitSrc;
  // --- contract v4 additive ---
  /** Stable point slug (attribute; the map key stays unit_id — re-key by point cancelled). */
  point?: string;
  /** Field class = schema.fields_by_source key (via resolveSrcClass) — gate fields on this. */
  srcClass: string;
  /** Capability tags for this unit, e.g. ["DIRECTION","DEVICE_ENERGY"]. */
  caps?: string[];
  /** Display resolution hint. */
  res?: number;
  /** Live quality bitmask (contract §1). */
  qLive?: number;
  /** Seconds since the last live sample (hub `age_s`); undefined on older firmware. */
  ageS?: number | null;
  /** Power resolution, W (hub `p_res_w`); undefined → the app's default no-flow threshold. */
  pResW?: number;
  /** The hub holds an old value (q_live STALE bit 4, or age_s > 5): shown as «замерло N с». */
  stale?: boolean;
  /** Role reported in the frame itself (v2); the units-config join wins when loaded. */
  role?: string;
  /** True when the unit measures bidirectional flow (caps.DIRECTION or a signed `dir`). */
  hasDirection: boolean;
  /** Per-phase readings of a U3I3 unit (`ph[]`, this frame only), sorted by phase; absent → none. */
  phases?: TelemetryPhase[];
  /** Phase rotation measured on this unit (3-phase STD, root 9bc08b40). */
  phaseSeq?: PhaseSeq;
  /** Non-state fields surfaced as HA attributes. */
  attributes: Record<string, unknown>;
  entities: EntityState[];
}

export interface SystemEntityGroup {
  deviceId: string;
  ts: number;
  freq: number;
  /** "nominal" = `freq` is the configured value, not a measurement (canon 2b3dc1fd). */
  freqSrc?: 'measured' | 'nominal';
  net_pwr: number;
  net_dir: number;
  /** Device-computed totals (power/energy/cost across units), if present. */
  totals?: TelemetrySystemTotals;
  // --- contract v4 additive (time/sequence) ---
  /** Time quality: 0 unsynced · 1 synced · 2 holdover. */
  tq?: number;
  time_src?: string;
  sync_age_s?: number;
  /** Monotonic frame sequence (gap ⇒ dropped frames). */
  seq?: number;
  entities: EntityState[];
}

export interface NormalizedTelemetry {
  system: SystemEntityGroup;
  units: UnitEntityGroup[];
}

/** Default system device-id namespace. TODO: derive from system-config device_id. */
export const DEFAULT_SYSTEM_BASE = 'rbgrid_main_001';

/** Read a numeric metric value from a unit group by metric suffix (e.g. "power"); null if absent. */
export function unitMetricValue(group: UnitEntityGroup, suffix: string): number | null {
  const id = `sensor.rbgrid_${group.unitKey}_${suffix}`;
  const e = group.entities.find((x) => x.entity_id === id);
  return typeof e?.state === 'number' ? e.state : null;
}

/**
 * Map the wire `dir` to the HA enum state. Firmware `rbpower_current_direction_t`:
 * 0 UNKNOWN · 1 CONSUMING (+P) · 2 SUPPLYING (−P) · 3 BIDIRECTIONAL · 4 ZERO.
 * Negative values are accepted as export too (older demo fixtures used ±1).
 */
export function directionLabel(dir?: number): 'import' | 'export' | 'idle' {
  if (dir === 1) return 'import';
  if (dir === 2 || (dir !== undefined && dir < 0)) return 'export';
  return 'idle';
}

const finite = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

/** Below this current a power factor is noise (UX audit INP-22/HEU-30: «cos φ −0,46» at 0 A). */
export const PF_MIN_CURRENT_A = 0.05;
/** A cos φ worth showing: within [−1, 1] and with current flowing (when the current is known). */
export const pfMeaningful = (pf: number, i: number | undefined): boolean =>
  Math.abs(pf) <= 1.0001 && (i === undefined || Math.abs(i) >= PF_MIN_CURRENT_A);

/** Valid `ph[]` entries (phase 1..3, numeric fields only; unknown fields stay absent). */
export function normalizePhases(raw: unknown): TelemetryPhase[] | undefined {
  if (!Array.isArray(raw)) return undefined;
  const out: TelemetryPhase[] = [];
  for (const e of raw) {
    if (!e || typeof e !== 'object') continue;
    const r = e as Record<string, unknown>;
    if (!finite(r.ph) || r.ph < 1 || r.ph > 3 || out.some((x) => x.ph === r.ph)) continue;
    const ph: TelemetryPhase = { ph: r.ph };
    for (const k of ['v', 'i', 'p', 'q', 'pf', 'angle'] as const) if (finite(r[k])) ph[k] = r[k] as number;
    if (ph.pf !== undefined && !pfMeaningful(ph.pf, ph.i)) delete ph.pf;
    out.push(ph);
  }
  return out.length ? out.sort((a, b) => a.ph - b.ph) : undefined;
}

function unitAttributes(u: TelemetryUnit): Record<string, unknown> {
  // CT state per channel (root d03f19ed): reversed sensor (BASIC), load %, near-limit, clipping.
  return {
    src: u.src, bus: u.bus, module: u.module, ch: u.ch, ch_label: u.ch_label, energy_mode: u.energy_mode,
    reversed: u.reversed, load_pct: u.load_pct, ct_warn: u.ct_warn, clip: u.clip,
  };
}

/**
 * Frame v2 (root canon 274a569c) keys every unit by HEX4; the app's site model still names the
 * non-physical bus and the alternative supply by their v1 keys ("v_bus" pseudo unit, "alt"), so
 * map those two back (by the frame's refs, or type VOLTAGE_BUS). The main input keeps its hex —
 * as on a v1 rbAmp install; "0006" is recognised as the main key either way.
 */
function v2Key(frame: TelemetryFrame, key: string, u: TelemetryUnit): string {
  if (frame.v !== 2) return key;
  const k = key.toUpperCase();
  const same = (ref?: string) => ref != null && ref.toUpperCase() === k;
  if (same(frame.vbus_unit) || u.type === 'VOLTAGE_BUS') return 'v_bus';
  if (same(frame.alt_unit)) return 'alt';
  return k;
}

/**
 * Normalize one live telemetry frame into the canonical entity model.
 * Per ENTITY_MODEL.md: per-unit metric entities + a system device with net/freq entities.
 */
export function normalizeTelemetryFrame(
  frame: TelemetryFrame,
  systemBase: string = DEFAULT_SYSTEM_BASE
): NormalizedTelemetry {
  const units: UnitEntityGroup[] = Object.entries(frame.units || {}).map(([wireKey, u]) => {
    const unitKey = v2Key(frame, wireKey, u);
    const present = u.present !== false;
    // `age_s: null` on a present unit = not read since boot (root 16.10): the 0 W it carries is
    // «no data», not a reading. Older firmware has no `age_s` at all — unaffected.
    const unread = present && 'age_s' in u && u.age_s === null;
    const attributes = unitAttributes(u);
    const deviceId = `${systemBase}_${unitKey}`;

    const entities: EntityState[] = UNIT_METRICS.map((m) => {
      let value = m.read(u);
      // No measurement → no number (UX audit T3): an ABSENT unit's live values are «—», not the
      // 0 the hub keeps in the frame; energy counters stay (they are persisted totals).
      if ((!present || unread) && m.state_class === 'measurement') value = undefined;
      if (m.key === 'power_factor' && value !== undefined && !pfMeaningful(value, u.i)) value = undefined;
      return {
        entity_id: `sensor.rbgrid_${unitKey}_${m.suffix}`,
        label: m.label,
        state: value === undefined ? null : value,
        unit: m.unit,
        device_class: m.device_class,
        state_class: m.state_class,
        available: present && value !== undefined,
        attributes,
      };
    });

    // direction (enum) entity
    entities.push({
      entity_id: `sensor.rbgrid_${unitKey}_direction`,
      label: 'Direction',
      state: u.dir === undefined ? null : directionLabel(u.dir),
      unit: null,
      device_class: 'enum',
      state_class: null,
      available: present && u.dir !== undefined,
      attributes,
    });

    const caps = u.caps;
    const hasDirection = (caps?.includes('DIRECTION') ?? false) || u.dir !== undefined;
    return {
      unitKey,
      deviceId,
      name: u.point ?? `RBGrid Unit ${unitKey}`,
      present,
      src: u.src,
      point: u.point,
      srcClass: resolveSrcClass(u),
      caps,
      res: u.res,
      qLive: u.q_live,
      ageS: u.age_s,
      pResW: typeof u.p_res_w === 'number' && u.p_res_w > 0 ? u.p_res_w : undefined,
      // q_live bit STALE = 4 (root): no sample for > 5 s; age_s says how long.
      stale: ((u.q_live ?? 0) & 4) === 4 || (typeof u.age_s === 'number' && u.age_s > 5),
      role: u.role,
      hasDirection,
      phases: unread ? undefined : normalizePhases(u.ph),
      phaseSeq: !unread && (u.phase_seq === 'forward' || u.phase_seq === 'reverse' || u.phase_seq === 'abnormal') ? u.phase_seq : undefined,
      attributes,
      entities,
    };
  });

  const system: SystemEntityGroup = {
    deviceId: systemBase,
    ts: frame.ts,
    freq: frame.freq,
    freqSrc: frame.freq_src,
    net_pwr: frame.net_pwr,
    net_dir: frame.net_dir,
    totals: frame.system,
    tq: frame.tq,
    time_src: frame.time_src,
    sync_age_s: frame.sync_age_s,
    seq: frame.seq,
    entities: [
      { entity_id: 'sensor.rbgrid_net_power',     label: 'Net power',     state: frame.net_pwr ?? null,        unit: 'W',  device_class: 'power',     state_class: 'measurement', available: true, attributes: {} },
      { entity_id: 'sensor.rbgrid_net_direction', label: 'Net direction', state: directionLabel(frame.net_dir), unit: null, device_class: 'enum',      state_class: null,          available: true, attributes: {} },
      { entity_id: 'sensor.rbgrid_frequency',     label: 'Frequency',     state: frame.freq ?? null,           unit: 'Hz', device_class: 'frequency', state_class: 'measurement', available: true, attributes: {} },
    ],
  };

  return { system, units };
}
