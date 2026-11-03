/**
 * FlowFrame — the single "flow frame" the energy-flow schemas render from.
 * Built in the UI from one live telemetry frame (UI-design spec §5; no history aggregation).
 *
 * Mapping confirmed by design in spec §14 (W3), with the three corrections:
 *  - grid  = P of the main unit (0006). Sign ONLY if main has DIRECTION; otherwise |P|
 *            and neither export, the ↑ arrow, nor the solar-share ring is drawn (U3).
 *  - solar = Σ P of generation points; null (not 0) when there are none.
 *  - house = grid + solar WHEN main has DIRECTION (Σ lines under-counts by the unmetered
 *            part); else Σ non-main consumers if any; else |grid|.
 *  - loads = non-main, non-generation points, PLUS a synthetic «Прочее» row
 *            (house − Σ loads) when it is > 5 % of house AND > 50 W.
 */
import { NormalizedTelemetry, UnitEntityGroup, unitMetricValue } from '../../entities/model';
import { UnitRole } from '../../types/device';
import {
  isMainKey, isPseudoKey, isAltKey, isGenerationRole, isExporting, signedPowerW, normKey,
  MAIN_HEX_KEY,
} from '../../entities/siteModel';
import { t } from '../../i18n';

/** Icon id understood by the flow renderers (see energy-flow-reference `P` set). */
export type FlowIcon = 'tower' | 'sun' | 'home' | 'plug' | 'drop' | 'flame' | 'bulb' | 'car' | 'layers';

export interface FlowLoad {
  id: string;
  name: string;
  short?: string;
  icon: FlowIcon;
  /** kW; null = no data (drawn grey, dashed). */
  v: number | null;
  alarm?: boolean;
  gen?: boolean;
  /** Synthetic «Прочее»/«Остаток» row — unmetered load, not clickable. */
  synthetic?: boolean;
  /** This card stands for a whole consumer GROUP (G5 `group_id`) — opens the group's tab. */
  group?: { id: string; count: number };
}

export interface FlowGroup {
  id: string;
  name: string;
  /** Member telemetry keys (the trunk excluded). */
  ids: string[];
  /** Telemetry key of the group's trunk (feeder) meter (G5 `role_trunk`), if any. */
  trunk?: string;
}

export interface FlowFrame {
  /** kW. >0 import, <0 export. Signed only when {@link gridHasDirection}. */
  grid: number;
  /** kW of generation, or null when there is none. */
  solar: number | null;
  /** kW of house load. */
  house: number;
  loads: FlowLoad[];
  groups?: FlowGroup[];
  /**
   * Set when the frame shows ONE group (its tab): the hub is the group, the single source card
   * above it is the house, the loads are the group's members.
   */
  scope?: { id: string; name: string; house: number };
  /** No live connection — schemas freeze and grey out. */
  lost?: boolean;
  /** Whether the main input measures direction (gates export arrow + solar-share ring). */
  gridHasDirection: boolean;
  /** The main input measures now (false = absent: grid «—», no direction — UX audit T3). */
  gridPresent?: boolean;
  /** The house load is known (some point measures, and it isn't the impossible case below). */
  houseKnown?: boolean;
  /**
   * The input reads EXPORT but nothing on site can produce (no generation / storage) — a
   * reversed current sensor or a wrong role. Diagnostic only; the house load is then unknown
   * (UX audit T5: «Отдаём 1,07 кВт» next to «Нагрузка дома 1,07 кВт» on .110).
   */
  exportWithoutSource?: boolean;
}

/**
 * HEX4 key of the main input. NOTE: while the device reports `legacy_supply_keys` the live
 * telemetry keys it as "main" — find the main unit with {@link isMainKey}, never by equality.
 * History/summary endpoints address it by this HEX4 id.
 */
export const MAIN_UNIT_KEY = MAIN_HEX_KEY;
const W_PER_KW = 1000;
/** Synthetic «Прочее» thresholds (spec §14 W3): > 5 % of house AND > 50 W. */
const OTHER_MIN_FRACTION = 0.05;
const OTHER_MIN_W = 50;

const kw = (w: number | null): number => (w == null ? 0 : w / W_PER_KW);

/**
 * Generation by its configured role (RENEWABLE_SOURCE). Only when the unit has NO configured
 * role at all (units-config not loaded / unknown unit) fall back to "exporting right now".
 */
function isGeneration(g: UnitEntityGroup, role?: UnitRole): boolean {
  if (role) return isGenerationRole(role);
  return isExporting(g);
}

const has = (n: string, ...words: string[]): boolean => words.some((w) => n.includes(w));

/** Pick an icon from the configured role, then the name (best-effort; renderer tolerates any). */
function iconFor(g: UnitEntityGroup, gen: boolean, role: UnitRole | undefined, name: string): FlowIcon {
  if (gen) return 'sun';
  if (role === 'EV_CHARGING') return 'car';
  if (role === 'HEATING_COOLING') return 'flame';
  const n = `${g.point ?? ''} ${name}`.toLowerCase();
  if ((g.point ?? '').startsWith('ev.') || has(n, 'заряд', 'charger', 'charging')) return 'car';
  if (has(n, 'бойлер', 'водонагрев', 'boiler', 'water heater')) return 'drop';
  if (has(n, 'отоплен', 'котёл', 'котел', 'heating', 'heater')) return 'flame';
  if (has(n, 'освещ', 'свет', 'light', 'lamp')) return 'bulb';
  return 'plug';
}

function isAlarm(g: UnitEntityGroup): boolean {
  // qLive carries the quality bitmask; treat any set bit above "good" plus a device alarm flag.
  const a = g.attributes?.['alarm'];
  return a === true;
}

export interface BuildFlowOptions {
  /** unitKey → configured role (from units-config); enables role-accurate generation detection. */
  roleByUnit?: Record<string, UnitRole>;
  /** unitKey → conductors a single-CT reading stands for (W9: 3P → 3, two-pole → 2); absent = 1. */
  conductorsByUnit?: Record<string, number>;
  /** Consumer groups from units-config (G5 `group_id` / `role_trunk`); omitted → no groups. */
  groups?: FlowGroup[];
  /** Force the "no live data" state (schemas freeze/grey). */
  lost?: boolean;
  /** Display name for a unit (configured short_name/name); defaults to the point slug. */
  nameOf?: (g: UnitEntityGroup) => string;
}

/**
 * Build a {@link FlowFrame} from normalized telemetry. Pure; safe to call every frame
 * (the renderer, not this, handles in-place/no-flicker updates).
 */
export function buildFlowFrame(data: NormalizedTelemetry, opts: BuildFlowOptions = {}): FlowFrame {
  const { roleByUnit = {}, groups, lost } = opts;
  const nameOf = opts.nameOf ?? ((g: UnitEntityGroup) => g.point || g.name);
  const roleOf = (u: UnitEntityGroup): UnitRole | undefined =>
    roleByUnit[normKey(u.unitKey)] ?? (u.role as UnitRole | undefined); // v2 frames carry the role
  // W9: full power of a single-CT multi-phase point ≈ one conductor × N (balance estimate);
  // a unit reporting ph[] is already Σ.
  const conductors = (u: UnitEntityGroup): number =>
    (u.phases?.length ? 1 : opts.conductorsByUnit?.[normKey(u.unitKey)] ?? 1);
  const fullW = (u: UnitEntityGroup): number | null => {
    const p = signedPowerW(u);
    return p == null ? null : p * conductors(u);
  };
  const units = data.units;

  const main =
    units.find((u) => isMainKey(u.unitKey)) ||
    units.find((u) => roleOf(u) === 'MAIN_SUPPLY' && !isPseudoKey(u.unitKey));

  // grid (kW). Legacy ADC input: sign only when it measures direction. An rbAmp input (current
  // installs, no native ADC): the sign of p_act IS the direction (root f451b6a5; no DIRECTION cap).
  const legacyMain = !!main && isMainKey(main.unitKey);
  const gridHasDirection = main ? (legacyMain ? main.hasDirection : true) : data.system.net_dir !== 0 || false;
  let gridW: number;
  if (main && !legacyMain) {
    gridW = fullW(main) ?? 0;
  } else if (main) {
    const p = (unitMetricValue(main, 'power') ?? 0) * conductors(main);
    gridW = gridHasDirection ? (isExporting(main) ? -Math.abs(p) : Math.abs(p)) : Math.abs(p);
  } else {
    // Fall back to the system net figure.
    gridW = gridHasDirection ? data.system.net_pwr : Math.abs(data.system.net_pwr);
  }
  const grid = kw(gridW);

  // Exclude the main input, supplies and non-physical pseudo-units (bus voltage) from the lines.
  const nonMain = units.filter((u) => !u.noChannel).filter(
    (u) => u !== main && !isPseudoKey(u.unitKey) && !isAltKey(u.unitKey) && roleOf(u) !== 'MAIN_SUPPLY'
      && roleOf(u) !== 'ALTERNATIVE_SUPPLY',
  );

  // solar = Σ generation; null when none.
  const genUnits = nonMain.filter((u) => isGeneration(u, roleOf(u)));
  const solar =
    genUnits.length === 0
      ? null
      : genUnits.reduce((s, u) => s + Math.abs(kw(unitMetricValue(u, 'power'))) * conductors(u), 0);

  // consumers = non-main, non-generation (incl. storage: signed, negative while discharging).
  const consumers = nonMain.filter((u) => !isGeneration(u, roleOf(u)));

  const loads: FlowLoad[] = consumers.map((u) => {
    const p = fullW(u);
    const name = nameOf(u);
    return {
      id: u.unitKey,
      name,
      short: name,
      icon: iconFor(u, false, roleOf(u), name),
      v: p == null ? null : kw(p),
      alarm: isAlarm(u),
    };
  });

  // house (kW): grid + solar with direction; else Σ consumers; else |grid|.
  // A group with a trunk meter: the trunk already measures its members → count the trunk only.
  const underTrunk = new Set((groups ?? []).filter((g) => g.trunk).flatMap((g) => g.ids));
  const sumConsumers = loads.reduce((s, l) => s + (underTrunk.has(l.id) ? 0 : l.v ?? 0), 0);
  let house: number;
  const gridPresent = !main || main.present;
  const canSupply = genUnits.length > 0 || consumers.some((u) => roleOf(u) === 'STORAGE_SYSTEM');
  const exportWithoutSource = gridHasDirection && gridPresent && grid < -0.005 && !canSupply;
  // An absent input reports 0, not a measurement → the house is Σ consumers then.
  if (gridHasDirection && gridPresent && !exportWithoutSource) house = grid + (solar ?? 0);
  else if (consumers.length > 0) house = sumConsumers;
  else house = Math.abs(grid);
  const anyMeasuring = (main?.present ?? false) || consumers.some((u) => u.present) || genUnits.some((u) => u.present)
    || (!main && units.length === 0);
  const houseKnown = anyMeasuring && !(exportWithoutSource && !consumers.some((u) => u.present));

  // synthetic «Прочее» — unmetered remainder.
  const other = house - sumConsumers;
  // Only the input metered (no lines): the whole house is «Прочее», kept at 0 W too — else the
  // schema vanished whenever the input read 0 and came back with the load (.139 18.10).
  const inputOnly = consumers.length === 0 && genUnits.length === 0 && !!main && gridPresent && houseKnown;
  if (inputOnly) {
    loads.push({ id: '__other__', name: t('flow.other'), icon: 'plug', v: Math.max(0, other), synthetic: true });
  } else if (house > 0 && other > house * OTHER_MIN_FRACTION && other * W_PER_KW > OTHER_MIN_W) {
    loads.push({ id: '__other__', name: t('flow.other'), icon: 'plug', v: other, synthetic: true });
  }

  return { grid, solar, house, loads, groups, lost, gridHasDirection, gridPresent, houseKnown, exportWithoutSource };
}

/** Share of house load covered by generation (0..1). Only meaningful with direction. */
export function solarShare(frame: FlowFrame): number {
  if (frame.solar == null || frame.house <= 0) return 0;
  return Math.min(1, frame.solar / frame.house);
}

/** Consumer colour on the flow diagrams: shades of the consumption hue; the unmetered rest grey
 *  (solar / export colours stay reserved for generation and export — UX audit GEN-09). */
export function consumerColor(l: FlowLoad, i: number): string {
  if (l.synthetic) return 'var(--status-offline)';
  const steps = ['var(--primary)', 'color-mix(in srgb, var(--primary) 72%, var(--surface))', 'color-mix(in srgb, var(--primary) 50%, var(--surface))'];
  return steps[i % steps.length];
}

/** Nothing flows now (input, generation and every line under 5 W) — the distribution schemas
 *  would draw empty axes / a lone label, so they say it in words instead (.139 18.10). */
export function flowIdle(f: FlowFrame): boolean {
  const still = (v: number | null | undefined) => v == null || Math.abs(v) < 0.005;
  return !f.lost && f.gridPresent !== false && still(f.grid) && still(f.solar) && still(f.house) && f.loads.every((l) => still(l.v));
}
