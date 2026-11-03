/**
 * useUnitsConfig — joins units-config onto the live telemetry entities.
 *
 * Telemetry carries the live electrical values (P/I/U/energy) but NOT the configured
 * line nominal, phase, short label or role — those live in units-config (gaps G2/G3/G4/G6).
 * This hook loads the 'units' config shadow and exposes a per-unit lookup so Overview
 * layouts (line table, phase cards, EnergyFlowTree) and the FlowFrame role-join read one source.
 *
 * Join keys (canon fc0f962b, verified on hub .139): the stable `point` slug when set; else the
 * HEX4 UPPERCASE key of `unit_id` (%04X — telemetry "9020", "6A00"); else, while the device
 * reports `legacy_supply_keys`, the supply/bus units by their legacy telemetry keys:
 * "main" = MAIN_SUPPLY current unit, "v_bus" = the VOLTAGE_BUS unit, "alt" = ALTERNATIVE_SUPPLY.
 * Numeric limits of 0 mean "not set" (root) → exposed as undefined; the UI never substitutes.
 */
import { useEffect, useMemo, useRef } from 'react';
import { useConfigShadow, useDeviceSession } from '../core';
import type { JsonObject, JsonValue } from '../core';
import { UnitRole } from '../types/device';
import { UNIT_ROLE_NAMES } from '../constants/enums';
import { UnitEntityGroup } from '../entities/model';
import { t } from '../i18n';
import {
  hexKey, normKey, LEGACY_MAIN_KEY, LEGACY_ALT_KEY, VBUS_KEY,
} from '../entities/siteModel';
import { conductorsOf } from '../entities/gridSystem';
import { PHASE_GROUP_CH } from '../entities/phaseGroup';

export interface LineConfig {
  /** REST id (HEX4) of the configured unit — use it for /api/v1/* calls, never the telemetry key. */
  hex?: string;
  shortName?: string;
  name?: string;
  /** Firmware unit type, e.g. CURRENT_LOAD / VOLTAGE_BUS. */
  type?: string;
  /** 0=unset, 1..3=L1..L3, 4=3P, 5/6/7=L1L2/L2L3/L3L1 (two-pole) — entities/gridSystem. */
  phase?: number;
  /** Bound as a module's phase group (channel_idx 254, U3I3): its readings are Σ of the phases. */
  phaseGroup?: boolean;
  /** Rated voltage (V), when set. */
  ratedVoltageV?: number;
  /** Line nominal (W) — the load-bar denominator (G3). undefined when not set (0 on the wire). */
  ratedPowerW?: number;
  maxCurrentA?: number;
  role?: UnitRole;
  /** Consumer group (G5 `group_id`, 0 on the wire = none → undefined). */
  groupId?: number;
  /** This unit is its group's trunk (feeder) meter (G5 `role_trunk` = 1). */
  trunk?: boolean;
  /** EV station's own «charging» threshold / idle time (root 56be1d45; 0 on the wire = system.ev). */
  chargeThresholdW?: number;
  idleTimeoutMin?: number;
  /** v1.1.2 (root 6e32d29): false = live only, no hour/day/month records; undefined = the hub doesn't say. */
  history?: boolean;
}

export interface UnitsConfigJoin {
  /** Config for a telemetry group (joined by point, then HEX4 key, then legacy supply key). */
  forGroup: (g: UnitEntityGroup) => LineConfig | undefined;
  /** telemetry key (HEX4 UPPERCASE or legacy "main"/"alt"/"v_bus") → configured role. */
  roleByUnit: Record<string, UnitRole>;
  /** telemetry key → conductors a single reading stands for (W9: 3P → 3, two-pole → 2; phase groups and 1 omitted). */
  conductorsByUnit: Record<string, number>;
  /** Main-input ceiling (MAIN_SUPPLY current unit): input limit + breaker rating (G4). */
  input: { ratedPowerW?: number; maxCurrentA?: number; hex?: string; phase?: number; ratedVoltageV?: number };
  /** Phase codes of every configured unit (mains-system inference, entities/gridSystem). */
  phases: number[];
  /** v1.1.2: points that keep history (`history:true`); null when the hub doesn't report the flag. */
  historyCount: number | null;
  /** Whether any units-config has been reported yet. */
  loaded: boolean;
}

const str = (v: JsonValue | undefined): string | undefined =>
  typeof v === 'string' && v !== '' ? v : undefined;
/** Positive finite number, else undefined (0 = "not set" on the wire). */
const posOrUndef = (v: JsonValue | undefined): number | undefined =>
  typeof v === 'number' && Number.isFinite(v) && v > 0 ? v : undefined;
const asRole = (v: JsonValue | undefined): UnitRole | undefined =>
  typeof v === 'string' && (UNIT_ROLE_NAMES as readonly string[]).includes(v) ? (v as UnitRole) : undefined;

function toLineConfig(u: JsonObject): LineConfig {
  const phase = typeof u.phase === 'number' && u.phase >= 1 && u.phase <= 7 ? u.phase : undefined;
  return {
    hex: hexKey(u.unit_id),
    shortName: str(u.short_name),
    name: str(u.name),
    type: str(u.type),
    phase,
    phaseGroup: u.channel_idx === PHASE_GROUP_CH || undefined,
    history: typeof u.history === 'boolean' ? u.history : undefined,
    ratedPowerW: posOrUndef(u.rated_power_w),
    maxCurrentA: posOrUndef(u.max_current_a),
    ratedVoltageV: posOrUndef(u.rated_voltage_v),
    role: asRole(u.role),
    groupId: typeof u.group_id === 'number' && u.group_id > 0 ? u.group_id : undefined,
    trunk: u.role_trunk === 1 || u.role_trunk === true,
    chargeThresholdW: posOrUndef(u.charge_threshold_w),
    idleTimeoutMin: posOrUndef(u.idle_timeout_min),
  };
}

/** Which legacy telemetry key (if any) a config unit is emitted under. */
function legacyKeyOf(cfg: LineConfig): string | undefined {
  if (cfg.type === 'VOLTAGE_BUS') return VBUS_KEY;
  if (cfg.role === 'MAIN_SUPPLY') return LEGACY_MAIN_KEY;
  if (cfg.role === 'ALTERNATIVE_SUPPLY') return LEGACY_ALT_KEY;
  return undefined;
}

export function useUnitsConfig(): UnitsConfigJoin {
  const session = useDeviceSession();
  const units = useConfigShadow('units');

  // Pull units-config once (the Overview needs the nominal/phase/role join).
  const didLoad = useRef(false);
  useEffect(() => {
    if (didLoad.current) return;
    didLoad.current = true;
    if (Object.keys(session.current.get('units').reported).length === 0) {
      // Shared GET (DeviceSession.loadDomain); one retry after 5 s — a busy hub dropped it and the
      // Overview read «breaker rating not set» / «Point 9010» for a configured site (H3P-03).
      session.loadDomain('units').catch(() => {
        setTimeout(() => { session.loadDomain('units').catch(() => { /* offline → no join, live values still render */ }); }, 5000);
      });
    }
  }, [session]);

  return useMemo<UnitsConfigJoin>(
    () => buildUnitsJoin(Array.isArray(units.reported.units) ? (units.reported.units as JsonObject[]) : []),
    [units.reported],
  );
}

/** Pure join builder over the units-config `units[]` (exported for contract tests). */
export function buildUnitsJoin(list: JsonObject[]): UnitsConfigJoin {
  const byPoint = new Map<string, LineConfig>();
  const byKey = new Map<string, LineConfig>();
  const roleByUnit: Record<string, UnitRole> = {};
  const conductorsByUnit: Record<string, number> = {};
  let input: UnitsConfigJoin['input'] = {};
  const phases: number[] = [];
  let historyCount: number | null = null;

  for (const raw of list) {
    if (!raw || typeof raw !== 'object') continue;
    const cfg = toLineConfig(raw);
    if (cfg.history !== undefined) historyCount = (historyCount ?? 0) + (cfg.history ? 1 : 0);
    if (cfg.phase) phases.push(cfg.phase);
    const point = str(raw.point);
    const hex = hexKey(raw.unit_id);
    const legacy = legacyKeyOf(cfg);
    if (point) byPoint.set(point, cfg);
    for (const k of [hex, legacy]) {
      if (!k || byKey.has(k)) continue; // first MAIN_SUPPLY current unit wins "main"
      byKey.set(k, cfg);
      if (cfg.role) roleByUnit[k] = cfg.role;
      const n = cfg.phaseGroup ? 1 : conductorsOf(cfg.phase);
      if (n > 1) conductorsByUnit[k] = n;
    }
    if (legacy === LEGACY_MAIN_KEY && !input.ratedPowerW && !input.maxCurrentA) {
      // REST id of the input: the MAIN_SUPPLY rbAmp unit (0006 on the hubs today).
      input = { ratedPowerW: cfg.ratedPowerW, maxCurrentA: cfg.maxCurrentA, hex, phase: cfg.phase, ratedVoltageV: cfg.ratedVoltageV };
    }
  }

  const forGroup = (g: UnitEntityGroup): LineConfig | undefined =>
    (g.point ? byPoint.get(g.point) : undefined) ?? byKey.get(normKey(g.unitKey));

  return { forGroup, roleByUnit, conductorsByUnit, input, phases, historyCount, loaded: list.length > 0 };
}

/** Map a numeric phase (G6) to its display label. */
export function phaseLabel(phase?: number): string {
  switch (phase) {
    case 1: return 'L1';
    case 2: return 'L2';
    case 3: return 'L3';
    case 4: return t('misc.phase3');
    case 5: return 'L1–L2';
    case 6: return 'L2–L3';
    case 7: return 'L3–L1';
    default: return '—';
  }
}
