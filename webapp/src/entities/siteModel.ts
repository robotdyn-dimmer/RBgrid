/**
 * Site model primitives — ONE place for how the UI classifies units, so the flow schemas,
 * line table, phase cards and EV views agree on the same frame (review 02 B-4/B-12/B-13).
 *
 * Canon fc0f962b (firmware note configurable-surface 2026-10-13):
 *  - roles are the firmware enum (schema `enums.role`); generation = RENEWABLE_SOURCE,
 *    EV points = EV_CHARGING, battery = STORAGE_SYSTEM. There is no "GENERATION".
 *  - unit keys are HEX4 = %04X UPPERCASE; with `legacy_supply_keys` the telemetry still
 *    emits the main input as "main", the alternative supply as "alt", the bus as "v_bus".
 *  - power: `p_act` magnitude + a separate direction (`dir`) → sign comes from direction.
 */
import { UnitEntityGroup, unitMetricValue } from './model';
import type { UnitRole } from '../types/device';
import { conductorsOf } from './gridSystem';
import { t } from '../i18n';

/** Legacy telemetry keys for supply units (until the v2 frame, T12). */
export const LEGACY_MAIN_KEY = 'main';
export const LEGACY_ALT_KEY = 'alt';
export const VBUS_KEY = 'v_bus';
/** HEX4 key of the main input (RBGRID_UNIT_MAIN_SUPPLY, local id 6). */
export const MAIN_HEX_KEY = '0006';

/** Non-physical pseudo-units that are never a load/line. */
const PSEUDO_KEYS = new Set([VBUS_KEY]);

export const isPseudoKey = (key: string): boolean => PSEUDO_KEYS.has(key);
export const isMainKey = (key: string): boolean => key === LEGACY_MAIN_KEY || key.toUpperCase() === MAIN_HEX_KEY;
export const isAltKey = (key: string): boolean => key === LEGACY_ALT_KEY;

export const isGenerationRole = (r?: UnitRole): boolean => r === 'RENEWABLE_SOURCE';
export const isSupplyRole = (r?: UnitRole): boolean => r === 'MAIN_SUPPLY' || r === 'ALTERNATIVE_SUPPLY';
export const isStationRole = (r?: UnitRole): boolean => r === 'EV_CHARGING';
export const isStorageRole = (r?: UnitRole): boolean => r === 'STORAGE_SYSTEM';

/** Normalize a unit id (number, "0x9020", "9020", "6a00") to the HEX4 UPPERCASE telemetry key. */
export function hexKey(unitId: unknown): string | undefined {
  let n: number | undefined;
  if (typeof unitId === 'number' && Number.isFinite(unitId)) n = unitId;
  else if (typeof unitId === 'string') {
    const s = unitId.trim();
    // Strings are keys, i.e. hex — "9020" is 0x9020, not decimal 9020.
    if (/^(0x)?[0-9a-f]{1,8}$/i.test(s)) n = parseInt(s.replace(/^0x/i, ''), 16);
  }
  if (n === undefined || !Number.isFinite(n) || n < 0) return undefined;
  return (n >>> 0).toString(16).toUpperCase().padStart(4, '0');
}

/** HEX4 of the voltage bus (RBGRID_UNIT_VOLTAGE_BUS, local id 7). */
export const VBUS_HEX_KEY = '0007';

/**
 * The unit id the REST API expects (history/summary/economics/units take HEX4 only — the hub
 * answers "Invalid unit_id format (expected hex)" for "main"). Legacy telemetry keys map to
 * their fixed local ids; "alt" has no fixed id → pass its configured hex via `altHex`.
 */
export function apiUnitId(key: string, altHex?: string): string | null {
  if (key === LEGACY_MAIN_KEY) return MAIN_HEX_KEY;
  if (key === VBUS_KEY) return VBUS_HEX_KEY;
  if (key === LEGACY_ALT_KEY) return altHex ?? null;
  return key.toUpperCase();
}

/** Telemetry key normalized for lookups: legacy keys as-is, hex keys UPPERCASE. */
export const normKey = (key: string): string =>
  key === LEGACY_MAIN_KEY || key === LEGACY_ALT_KEY || key === VBUS_KEY ? key : key.toUpperCase();

function directionState(g: UnitEntityGroup): string | null {
  const e = g.entities.find((x) => x.entity_id === `sensor.rbgrid_${g.unitKey}_direction`);
  return typeof e?.state === 'string' ? e.state : null;
}

/** True when the unit currently reports export/generation direction. */
export const isExporting = (g: UnitEntityGroup): boolean => directionState(g) === 'export';

/**
 * Signed active power in W: magnitude from p_act, sign from the direction state
 * (export → negative). Falls back to p_act's own sign when no direction is reported.
 * null when the unit has no power reading or is not present.
 */
export function signedPowerW(g: UnitEntityGroup): number | null {
  if (!g.present) return null;
  const p = unitMetricValue(g, 'power');
  if (p == null) return null;
  const dir = directionState(g);
  if (dir === 'export') return -Math.abs(p);
  if (dir === 'import') return Math.abs(p);
  return p;
}

/**
 * Full power of a point (W9). A single-CT multi-phase point (units-config `phase` 3P or two-pole)
 * reports ONE conductor — firmware neither multiplies nor divides (root delta №4) — so its full
 * power is that × the conductors, an ESTIMATE «≈ при балансе». A phase-grouped unit (U3I3,
 * `ph[]`) already reports Σ → measured as is. Per-phase views keep {@link signedPowerW}.
 */
export function fullPower(
  g: UnitEntityGroup,
  cfg?: { phase?: number; phaseGroup?: boolean },
): { w: number | null; estimated: boolean } {
  const w = signedPowerW(g);
  const n = cfg?.phaseGroup || g.phases?.length ? 1 : conductorsOf(cfg?.phase);
  return { w: w == null ? null : w * n, estimated: n > 1 };
}
export const fullPowerW = (g: UnitEntityGroup, cfg?: { phase?: number; phaseGroup?: boolean }): number | null =>
  fullPower(g, cfg).w;

/** Display name: configured short name → configured name → point slug → key. */
/**
 * Full name for roomy places (page headers, tables, alerts): configured name first. The hub
 * caps short_name at 10 BYTES (≈ 5 Cyrillic letters), so it is for schemes/tiles only.
 */
/** The hub's factory names for its own supply units, shown in the UI language (HOME-17). */
const FACTORY_NAMES: Record<string, string> = {
  'Main Supply Current': 'names.mainInput',
  'Main Voltage Bus': 'names.voltageBus',
};
/** The hub's factory channel name «rbAmp I2 ch0 (5A)» (HOME-17: technical words on the
 *  homeowner's screens) → «Без имени · датчик 5 А (I2, I0)» until the point is named. The channel as
 *  printed on the module: current channels «I0», «I1»… from 0 (operator via root 16.10 — it read
 *  «I3, channel 1» for ch0); the hub's new default «rbAmp I3 I0 (30A)» and the old «… ch0 …» alike. */
const RBAMP_FACTORY = /^rbAmp\s+([A-Z]\d+)\s+(?:ch|I)(\d+)\s*\((\d+(?:[.,]\d+)?)\s*A\)$/i;
const localizeFactory = (name: string | undefined): string | undefined => {
  if (!name) return undefined;
  if (FACTORY_NAMES[name]) return t(FACTORY_NAMES[name]);
  const m = RBAMP_FACTORY.exec(name.trim());
  if (m) return t('names.unnamedChannel', { module: m[1], jack: `I${Number(m[2])}`, amps: m[3] });
  return name;
};

/**
 * A point without a configured name: by what it is («Шина напряжения»), else «Точка 9020» —
 * never the slug (`floor1.kitchen`) or «RBGrid Unit 0006» (UX audit T6: one point, five names).
 */
function fallbackName(g: UnitEntityGroup): string {
  if (g.unitKey === VBUS_KEY) return t('names.voltageBus');
  if (isMainKey(g.unitKey)) return t('names.mainInput');
  return t('names.pointN', { id: g.unitKey });
}

/** Full name — page headers, tables, chips, alerts. */
export function fullDisplayName(
  g: UnitEntityGroup,
  cfg?: { shortName?: string; name?: string },
): string {
  return localizeFactory(cfg?.name) || localizeFactory(cfg?.shortName) || fallbackName(g);
}

/** Short name — schemas and tiles (the short name when set). */
export function displayName(
  g: UnitEntityGroup,
  cfg?: { shortName?: string; name?: string },
): string {
  return localizeFactory(cfg?.shortName) || localizeFactory(cfg?.name) || fallbackName(g);
}
